#include "runtime.hpp"
#include <sys/stat.h>
#include <chrono>
namespace worker {
Session* Session::active_=nullptr;
void Session::attach() {
    active_=this;
    call<void>(names::API_setCredentialsHandler,runtime_.presentation.get(),&Session::credentials);
    call<void>(names::API_setDialogHandler,runtime_.presentation.get(),&Session::dialog);
}
void Session::credentials(Shared* request,Shared*) noexcept {
    try { if(active_) active_->credentials_impl(request); }
    catch(const Error& e) {
        try { if(active_) active_->protocol_.write({{"v",1},{"id",active_->request_id_},{"ok",false},{"error",{{"code",e.code},{"retryable",false},{"restart_required",true}}}}); } catch(...) {}
        _exit(70);
    }
    catch(...) { _exit(70); } // Never unwind into proprietary callback code.
}
void Session::dialog(long id,Shared* dialog,Shared*) noexcept {
    try { if(active_)active_->dialog_impl(id,dialog); }
    catch(...) { _exit(70); }
}
void Session::credentials_impl(Shared* request) {
    auto response=emplace_native(80,24,names::vtable_CredentialsResponse,names::CredentialsResponse_ctor);
    bool submit=login_active_&&++credentials_count_<=3;
    if(submit) {
        submitted_password_.value=password_.value;
        bool two=request&&*request&&call<bool>(names::CR_requiresHSA2VerificationCode,request->get());
        if(two) {
            auto challenge=std::to_string(++challenge_);
            auto code=protocol_.challenge(request_id_,challenge);
            if(code.empty())submit=false;
            else submitted_password_.value+=code;
            wipe(code);
        }
    }
    if(submit) {
        call<void>(names::CredentialsResponse_setUserName,response.get(),&username_.value);
        // Do not pass the address of a callback-local temporary. Apple can
        // retain a reference to this std::string after this function returns.
        call<void>(names::CredentialsResponse_setPassword,response.get(),&submitted_password_.value);
    } else {callback_failed_=true;state_="expired";}
    call<void>(names::CredentialsResponse_setResponseType,response.get(),submit?2:0);
    call<void>(names::API_handleCredentialsResponse,runtime_.presentation.get(),&response);
}
void Session::dialog_impl(long id,Shared* dialog) {
    auto response=emplace_native(72,24,names::vtable_ProtocolDialogResponse,names::ProtocolDialogResponse_ctor);
    bool selected=false;
    if(login_active_&&dialog&&*dialog) {
        auto title=call<const std::string*>(names::ProtocolDialog_title,dialog->get());
        auto buttons=call<const std::vector<Shared>*>(names::ProtocolDialog_buttons,dialog->get());
        if(title&&*title=="Sign In"&&buttons&&buttons->size()<=32)for(const auto& b:*buttons) {
            if(!b)continue;
            auto label=call<const std::string*>(names::ProtocolButton_title,b.get());
            if(label&&*label=="Use Existing Apple ID") {
                call<void>(names::ProtocolDialogResponse_setSelectedButton,response.get(),&b);selected=true;break;
            }
        }
    }
    // Match the working legacy ARM64 Wrapper: respond to every dialog, but
    // only select the known "Use Existing Apple ID" button. An unrelated
    // dialog is not itself proof that authentication failed; the Apple flow's
    // final response (or a cancelled credentials prompt) decides that.
    call<void>(names::API_handleProtocolDialogResponse,runtime_.presentation.get(),&id,&response);
}
static Json signed_request(Runtime& r,const std::string& url,const Json* payload) {
    auto destroy=symbol<void(*)(void*)>(names::HTTPMessage_dtor);
    void* memory=::operator new(4096);std::memset(memory,0,4096);
    call<void>(names::HTTPMessage_ctor_c1,memory,url,std::string(payload?"POST":"GET"));
    Shared message(memory,[destroy](void* p){destroy(p);::operator delete(p);});
    auto header=[&](std::string key,std::string value){call<void>(names::HTTPMessage_setHeader,memory,&key,&value);};
    Secret body;
    if(payload) {
        header("Content-Type","application/json; charset=UTF-8");header("Expect","");
        header("X-Apple-Requesting-Bundle-Id","com.apple.android.music");
        header("X-Apple-Requesting-Bundle-Version","Music/4.9 Android/10 model/Samsung S9 build/7663313 (dt:66)");
        body.value=payload->dump();
        call<void>(names::HTTPMessage_setBodyData,memory,body.value.data(),body.value.size());
    }
    NativeObject request(8192,names::URLRequest_dtor);
    call<void>(names::URLRequest_ctor,request.data,&message,&r.request);
    if(!payload) {
        std::string client="clientId",music="musicAndroid",version="version",one="1";
        call<void>(names::URLRequest_setRequestParameter,request.data,&client,&music);
        call<void>(names::URLRequest_setRequestParameter,request.data,&version,&one);
    }
    call<void>(names::URLRequest_run,request.data);
    auto error=call<Shared*>(names::URLRequest_error,request.data);
    if(error&&*error)throw Error("session_validation_failed",true);
    auto response=call<Shared*>(names::URLRequest_response,request.data);
    if(!response||!*response)throw Error("session_validation_failed",true);
    auto inner=call<Shared*>(names::URLResponse_underlyingResponse,response->get());
    if(!inner||!*inner)throw Error("session_validation_failed",true);
    void* data=nullptr;std::memcpy(&data,static_cast<unsigned char*>(inner->get())+48,sizeof data);
    if(!data)throw Error("session_validation_failed",true);
    auto bytes=call<const char*>(names::Data_bytes,data);
    auto length=call<size_t>(names::Data_length,data);
    if(!bytes||!length||length>1024*1024)throw Error("session_validation_failed",true);
    // Native Data may contain a trailing NUL; accept it, not arbitrary suffixes.
    while(length&&bytes[length-1]=='\0')--length;
    auto result=Json::parse(bytes,bytes+length,nullptr,false);
    if(result.is_discarded()||!result.is_object())throw Error("session_validation_failed",true);
    return result;
}
bool Session::validate_cached() {
    wipe(dev_token_.value);
    wipe(music_token_.value);
    Shared bag;std::string storefront;
    storefront=call<std::string>(names::RequestContext_storeFrontIdentifier,runtime_.request.get(),&bag);
    if(storefront.empty())return false;
    auto token=signed_request(runtime_,"https://sf-api-token-service.itunes.apple.com/apiToken",nullptr);
    JsonGuard tg{token};
    if(!token.contains("token")||!token["token"].is_string()||token["token"].get_ref<const std::string&>().empty())return false;
    Shared guid;
    guid=call<Shared>(names::DeviceGUID_guid,runtime_.guid.get());
    if(!guid)return false;
    auto bytes=call<const char*>(names::Data_bytes,guid.get());
    if(!bytes)return false;
    Json payload={{"guid",std::string(bytes)},{"assertion",token["token"]},{"tcc-acceptance-date",std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())}};
    JsonGuard pg{payload};
    auto result=signed_request(runtime_,"https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/createMusicToken",&payload);
    JsonGuard rg{result};
    if(!result.contains("music_token")||!result["music_token"].is_string()||result["music_token"].get_ref<const std::string&>().empty())return false;
    storefront_=std::move(storefront);
    dev_token_.value=token["token"].get<std::string>();
    music_token_.value=result["music_token"].get<std::string>();
    return true;
}
Json Session::restore() {
    struct stat st{};
    if(!runtime_.cached_session||stat((runtime_.directory+"/mpl_db/kvs.sqlitedb").c_str(),&st)||!S_ISREG(st.st_mode)||st.st_size<=0)throw Error("no_cached_session");
    callback_failed_=false;state_="logged_out";storefront_.clear();
    wipe(dev_token_.value);wipe(music_token_.value);
    if(!validate_cached()||callback_failed_){expire();throw Error("session_expired");}
    state_="authenticated";runtime_.lease();return status();
}
Json Session::login(const Json& params,const std::string& id) {
    if(state_=="authenticated")throw Error("already_authenticated");
    username_.value=string_param(params,"username",320);
    password_.value=string_param(params,"password",4096);
    request_id_=id;credentials_count_=0;callback_failed_=false;login_active_=true;
    struct Guard{Session& s;~Guard(){s.login_active_=false;wipe(s.username_.value);wipe(s.password_.value);wipe(s.submitted_password_.value);}} guard{*this};
    // These two legacy token hint files are regenerated by native authentication.
    for(const char* hint:{"STOREFRONT_ID","MUSIC_TOKEN"})
        if(unlink((runtime_.directory+"/"+hint).c_str())&&errno!=ENOENT)throw Error("session_directory_unavailable");
    Shared flow=call<Shared>(names::make_shared_AuthenticateFlow,&runtime_.request);
    if(!flow)throw Error("authentication_flow_unavailable");
    call<void>(names::AuthenticateFlow_run,flow.get());
    auto response=call<Shared*>(names::AuthenticateFlow_response,flow.get());
    if(!response||!*response){state_="logged_out";throw Error("authentication_response_missing");}
    if(callback_failed_){state_="logged_out";throw Error("authentication_callback_failed");}
    if(call<int>(names::AR_responseType,response->get())!=6){
        state_="logged_out";
        // A response without any credential callback points to a different
        // integration failure than Apple rejecting a submitted login.
        throw Error(credentials_count_ ? "authentication_response_not_success" : "authentication_no_credentials_prompt");
    }
    runtime_.cached_session=true;
    if(!validate_cached()){state_="logged_out";throw Error("session_validation_failed",true);}
    state_="authenticated";runtime_.lease();return status();
}
Json Session::status() const {return {{"state",state_},{"storefront",storefront_}};}
Json Session::export_credentials() {
    require();
    if(dev_token_.value.empty()||music_token_.value.empty())
        throw Error("session_expired");
    return {{"dev_token",dev_token_.value},{"music_user_token",music_token_.value}};
}
void Session::require(){
    if(state_=="expired")throw Error("session_expired");
    if(state_!="authenticated")throw Error("authentication_required");
}
void Session::expire(){
    state_="expired";storefront_.clear();
    wipe(dev_token_.value);wipe(music_token_.value);
}
}
