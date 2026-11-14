#include "runtime.hpp"
#include "library_pins.hpp"
#include <sys/file.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
namespace worker {
Shared emplace_native(size_t bytes,size_t offset,const char* table,const char* constructor) {
    auto v=symbol<void**>(table)+2;
    auto ctor=symbol<void(*)(void*)>(constructor);
    auto raw=static_cast<unsigned char*>(::operator new(bytes));
    std::memset(raw,0,bytes);std::memcpy(raw,&v,sizeof v);
    ctor(raw+offset);
    struct Layout{void* obj;void* control;} layout{raw+offset,raw};
    Shared result;
    std::memcpy(static_cast<void*>(&result),&layout,sizeof layout);
    return result;
}
void Runtime::initialize(const Json& params) {
    if(initialized)throw Error("already_initialized");
    if(params.contains("request_budget")) {
        const auto& n=params["request_budget"];
        if(!n.is_number_integer()||n<1||n>128)throw Error("invalid_params");
        request_budget=n.get<unsigned>();
    }
    directory=string_param(params,"session_dir",1024);
    if(directory.front()!='/'||directory.find("/../")!=std::string::npos||directory=="/")throw Error("invalid_params");
    auto device=string_param(params,"device_info",512);
    std::vector<std::string> parts;size_t start=0;
    for(size_t i=0;i<=device.size();++i)if(i==device.size()||device[i]=='/'){parts.push_back(device.substr(start,i-start));start=i+1;}
    if(parts.size()!=9)throw Error("invalid_params");
    for(const auto& part:parts)if(part.empty())throw Error("invalid_params");
    if(!params.contains("dns")||!params["dns"].is_array()||params["dns"].empty()||params["dns"].size()>4)throw Error("invalid_params");
    std::vector<std::string> dns;
    for(const auto& value:params["dns"]) {
        if(!value.is_string())throw Error("invalid_params");
        std::string address=value;unsigned char parsed[16];
        if(inet_pton(AF_INET,address.c_str(),parsed)!=1&&inet_pton(AF_INET6,address.c_str(),parsed)!=1)throw Error("invalid_params");
        dns.push_back(std::move(address));
    }
    struct stat st{};
    if(stat(directory.c_str(),&st)||!S_ISDIR(st.st_mode))throw Error("session_directory_unavailable",false,true);
    lock_=open((directory+"/.worker.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(lock_<0||flock(lock_,LOCK_EX|LOCK_NB))throw Error("session_in_use",true,true);
    const auto identity=sha256(reinterpret_cast<const unsigned char*>(device.data()),device.size());
    const auto identity_path=directory+"/.worker-device";
    Fd device_file(open(identity_path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    if(device_file.value>=0) {
        if(write(device_file.value,identity.data(),identity.size())!=ssize_t(identity.size())||fsync(device_file.value))throw Error("session_directory_unavailable",false,true);
    } else if(errno==EEXIST) {
        Fd saved(open(identity_path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));
        char value[65]{};
        if(saved.value<0||read(saved.value,value,sizeof value)!=64||identity!=std::string(value,64))throw Error("session_device_mismatch",false,true);
    } else throw Error("session_directory_unavailable",false,true);
    if(mkdir((directory+"/mpl_db").c_str(),0700)&&errno!=EEXIST)throw Error("session_directory_unavailable",false,true);
    cached_session=stat((directory+"/mpl_db/kvs.sqlitedb").c_str(),&st)==0&&S_ISREG(st.st_mode)&&st.st_size>0;
    // Hash every ABI-bearing dependency, not just the instrumented library.
    for(const auto& pair:apple_libraries)if(sha256_file(std::string("/system/lib64/")+pair.first)!=pair.second)throw Error("unsupported_library",false,true);
    for(const char* name:{"libCoreFP.so","libCoreLSKD.so","libandroidappmusic.so","libstoreservicescore.so","libmediaplatform.so"})
        if(!dlopen((std::string("/system/lib64/")+name).c_str(),RTLD_NOW|RTLD_GLOBAL))throw Error("native_load_failed",false,true);
    std::vector<const char*> servers;for(const auto& ip:dns)servers.push_back(ip.c_str());
    call<void>(names::resolv,0u,servers.data(),int(servers.size()),".");
    call<void>(names::FootHillConfig_config,&parts[8]);
    call<void>(names::DeviceGUID_instance,&guid);
    if(!guid)throw Error("native_init_failed",false,true);
    std::string empty;unsigned kind=29;bool yes=true;
    call<void>(names::DeviceGUID_configure,guid_config_.data(),guid.get(),&parts[8],&empty,&kind,&yes);
    std::string db=directory+"/mpl_db";
    call<void>(names::make_shared_RequestContext,&request,&db);
    if(!request)throw Error("native_init_failed",false,true);
    auto destructor=symbol<void(*)(void*)>(names::RequestContextConfig_dtor);
    void* config=::operator new(2048);std::memset(config,0,2048);
    call<void>(names::RequestContextConfig_ctor,config);
    config_=Shared(config,[destructor](void* p){destructor(p);::operator delete(p);});
    call<void>(names::RCC_setBaseDirectoryPath,config,&db);
    const char* setters[]={names::RCC_setClientIdentifier,names::RCC_setVersionIdentifier,names::RCC_setPlatformIdentifier,names::RCC_setProductVersion,names::RCC_setDeviceModel,names::RCC_setBuildVersion,names::RCC_setLocaleIdentifier,names::RCC_setLanguageIdentifier};
    for(unsigned i=0;i<8;++i)call<void>(setters[i],config,&parts[i]);
    call<void>(names::RequestContextManager_configure,&request);
    call<void>(names::RequestContext_init,request_config_.data(),request.get(),&config_);
    call<void>(names::RequestContext_setFairPlayDirectoryPath,request.get(),&directory);
    call<void>(names::make_shared_AndroidPresentationInterface,&presentation);
    if(!presentation)throw Error("native_init_failed",false,true);
    call<void>(names::RequestContext_setPresentationInterface,request.get(),&presentation);
    foothill=call<void*>(names::SVFootHillSessionCtrl_instance);
    if(!foothill)throw Error("native_init_failed",false,true);
    initialized=true;
}
void Runtime::lease() {
    bool yes=true;
    if(!lease_ready_) {
        lease_end_=[this](const int&){lease_expired=true;};
        lease_error_=[this](const Shared&){lease_expired=true;};
        call<void>(names::SVPlaybackLeaseManager_ctor,lease_.data(),&lease_end_,&lease_error_);
        call<void>(names::SVPlaybackLeaseManager_refreshLeaseAutomatically,lease_.data(),&yes);
        lease_ready_=true;
    }
    lease_expired=false;
    call<void>(names::SVPlaybackLeaseManager_requestLease,lease_.data(),&yes);
}
}
