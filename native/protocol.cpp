#include "protocol.hpp"
#include <cerrno>
#include <cstdint>
namespace worker {
void wipe_json(Json& j) {
    if(j.is_string()) wipe(j.get_ref<std::string&>());
    else if(j.is_structured()) for(auto& item:j) wipe_json(item);
}
std::string base64(const unsigned char* p,size_t n) {
    constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s; s.reserve((n+2)/3*4);
    for(size_t i=0;i<n;i+=3) {
        unsigned v=unsigned(p[i])<<16;
        if(i+1<n) v|=unsigned(p[i+1])<<8;
        if(i+2<n) v|=p[i+2];
        s+=alphabet[v>>18]; s+=alphabet[(v>>12)&63];
        s+=i+1<n?alphabet[(v>>6)&63]:'='; s+=i+2<n?alphabet[v&63]:'=';
    }
    return s;
}
std::string string_param(const Json& j,const char* key,size_t max,bool empty) {
    if(!j.contains(key)||!j[key].is_string()) throw Error("invalid_params");
    const auto& s=j[key].get_ref<const std::string&>();
    if((!empty&&s.empty())||s.size()>max||s.find('\0')!=std::string::npos) throw Error("invalid_params");
    return s;
}
void validate_adam(const std::string& s) {
    if(s.empty()||s.size()>20||s.find_first_not_of("0123456789")!=std::string::npos) throw Error("invalid_params");
}
void validate_uri(const std::string& s) {
    if(s.size()<=6||s.size()>4096||s.compare(0,6,"skd://")) throw Error("invalid_params");
    for(unsigned char c:s) if(c<=32||c==127) throw Error("invalid_params");
}
static bool read_exact(int fd,void* data,size_t size,bool eof_allowed) {
    auto p=static_cast<unsigned char*>(data); size_t n=0;
    while(n<size) {
        auto got=::read(fd,p+n,size-n);
        if(got<0&&errno==EINTR) continue;
        if(got==0&&n==0&&eof_allowed) return false;
        if(got<=0) throw Error("transport_closed",false,true);
        n+=size_t(got);
    }
    return true;
}
bool Protocol::read(Json& j) {
    unsigned char h[4];
    if(!read_exact(input_,h,4,true)) return false;
    uint32_t n=(uint32_t(h[0])<<24)|(uint32_t(h[1])<<16)|(uint32_t(h[2])<<8)|h[3];
    if(!n||n>1024*1024) throw Error("invalid_frame",false,true);
    Secret body; body.value.resize(n);
    read_exact(input_,&body.value[0],n,false);
    // Limit nesting during parsing to protect the worker stack.
    bool too_deep=false;
    j=Json::parse(body.value,[&](int depth,Json::parse_event_t,Json&){if(depth>64) {too_deep=true; throw Error("invalid_json");} return true;},false);
    if(too_deep||j.is_discarded()) throw Error("invalid_json");
    return true;
}
void Protocol::write(const Json& j) {
    Secret body; body.value=j.dump();
    size_t n=body.value.size();
    if(n>4*1024*1024) throw Error("response_too_large",false,true);
    unsigned char h[4]={static_cast<unsigned char>(n>>24),static_cast<unsigned char>(n>>16),static_cast<unsigned char>(n>>8),static_cast<unsigned char>(n)};
    for(auto part:{std::pair<const void*,size_t>{h,4},{body.value.data(),n}}) {
        auto p=static_cast<const char*>(part.first); size_t left=part.second;
        while(left) {auto sent=::write(output_,p,left); if(sent<0&&errno==EINTR) continue; if(sent<=0) throw Error("transport_closed",false,true); p+=sent;left-=size_t(sent);}
    }
}
void Protocol::validate(const Json& j) {
    if(!j.is_object()) throw Error("invalid_request");
    if(!j.contains("v")||!j["v"].is_number_integer()||j["v"]!=1) throw Error("unsupported_protocol");
    string_param(j,"id",64); string_param(j,"op",64);
    if(!j.contains("params")||!j["params"].is_object()) throw Error("invalid_params");
    if(j.contains("timeout_ms")&&(!j["timeout_ms"].is_number_integer()||j["timeout_ms"]<100||j["timeout_ms"]>180000)) throw Error("invalid_params");
}
}
