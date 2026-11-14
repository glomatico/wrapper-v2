#pragma once
#include "common.hpp"
#include "protocol.hpp"
#include "symbols.hpp"
#include <memory>
#include <functional>
#include <dlfcn.h>
#include <cstring>
#include <atomic>
namespace worker {
using Shared=std::shared_ptr<void>;
static_assert(sizeof(Shared)==16 && sizeof(std::string)==24,"Pinned Android libc++ ABI required");
template<class T>T symbol(const char* name) {
    auto p=dlsym(RTLD_DEFAULT,name);if(!p)throw Error("missing_native_symbol",false,true);return reinterpret_cast<T>(p);
}
template<class R,class... Args>R call(const char* name,Args... args){return symbol<R(*)(Args...)>(name)(args...);}
struct NativeObject {
    void* data; void(*destroy)(void*);
    NativeObject(size_t size,const char* destructor):data(nullptr),destroy(symbol<void(*)(void*)>(destructor)){data=::operator new(size);std::memset(data,0,size);}
    ~NativeObject(){destroy(data);::operator delete(data);}
    NativeObject(const NativeObject&)=delete;
};
// Only native __shared_ptr_emplace instances without exported object destructors
// need adoption. The layout is pinned and tested with the APK's libc++ runtime.
Shared emplace_native(size_t bytes,size_t offset,const char* vtable,const char* constructor);
class Runtime {
    int lock_=-1;
    Shared config_;
    alignas(16) std::array<unsigned char,88> guid_config_{},request_config_{};
    alignas(16) std::array<unsigned char,4096> lease_{};
    std::function<void(const int&)> lease_end_;
    std::function<void(const Shared&)> lease_error_;
    bool lease_ready_=false;
public:
    Shared request,guid,presentation;
    void* foothill=nullptr;
    std::string directory;
    bool initialized=false;
    bool cached_session=false;
    unsigned request_budget=128;
    std::atomic<bool> lease_expired{false};
    void initialize(const Json&);
    void lease();
    Json playback(const std::string&);
    Json context(const std::string&,const std::string&);
};
class Session {
    Runtime& runtime_;
    Protocol& protocol_;
    std::string state_="logged_out",storefront_,request_id_;
    Secret username_,password_,dev_token_,music_token_;
    unsigned challenge_=0,credentials_count_=0;
    bool login_active_=false,callback_failed_=false;
    static Session* active_;
    static void credentials(Shared*,Shared*) noexcept;
    static void dialog(long,Shared*,Shared*) noexcept;
    void credentials_impl(Shared*);
    void dialog_impl(long,Shared*);
    bool validate_cached();
public:
    Session(Runtime& r,Protocol& p):runtime_(r),protocol_(p){}
    void attach();
    Json restore();
    Json login(const Json&,const std::string& id);
    Json status() const;
    Json export_credentials();
    void require();
    void expire();
};
}
