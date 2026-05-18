#include "apple/loader.hpp"

#include "apple/aarch64_sret_thunks.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

namespace wrapper::apple {

namespace {

// Mirrors WRAPPER_RUNTIME_TRACE in runtime.cpp: when set to any non-empty
// non-"0" value, the loader prints a single stderr line before each dlopen /
// dlsym group so a SIGSEGV inside DT_INIT_ARRAY (or a missing symbol) can be
// localized to the exact library group. Stays off by default since the
// per-call tracing is verbose.
bool loader_trace_enabled() {
    static const bool v = []() {
        const char* x = std::getenv("WRAPPER_RUNTIME_TRACE");
        if (x == nullptr || *x == '\0') return false;
        if (std::strcmp(x, "0") == 0) return false;
        return true;
    }();
    return v;
}

void trace(const char* stage) {
    if (!loader_trace_enabled()) return;
    std::fprintf(stderr, "loader: trace %s\n", stage);
    std::fflush(stderr);
}

// Helper: dlopen with friendlier error reporting.
void* open_lib(const std::string& path, std::string* err_out) {
    if (loader_trace_enabled()) {
        std::fprintf(stderr, "loader: dlopen %s ...\n", path.c_str());
        std::fflush(stderr);
    }
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char* msg = dlerror();
        if (err_out != nullptr) {
            *err_out = "dlopen(" + path + "): " + (msg ? msg : "unknown error");
        }
        std::fprintf(stderr, "loader: %s\n", err_out ? err_out->c_str() : "");
    } else if (loader_trace_enabled()) {
        std::fprintf(stderr, "loader: dlopen %s -> %p\n", path.c_str(), h);
        std::fflush(stderr);
    }
    return h;
}

void* open_lib_optional(const std::string& path) {
    if (loader_trace_enabled()) {
        std::fprintf(stderr, "loader: dlopen(optional) %s ...\n", path.c_str());
        std::fflush(stderr);
    }
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char* msg = dlerror();
        std::fprintf(stderr, "loader: optional dlopen %s: %s\n", path.c_str(),
                     msg ? msg : "?");
    } else if (loader_trace_enabled()) {
        std::fprintf(stderr, "loader: dlopen(optional) %s -> %p\n", path.c_str(), h);
        std::fflush(stderr);
    }
    return h;
}

// Helper: dlsym with friendlier error reporting. Handle may be a real
// dlopen result OR RTLD_DEFAULT (which on bionic/x86_64 is the literal
// value 0 - so a null-check on the handle here would be wrong).
template <typename T>
bool resolve(void* h, const char* name, T* out, std::string* err_out) {
    dlerror();
    void* sym = dlsym(h, name);
    const char* msg = dlerror();
    if (msg != nullptr || sym == nullptr) {
        if (err_out) {
            *err_out = std::string("dlsym(") + name + "): " + (msg ? msg : "not found");
        }
        std::fprintf(stderr, "loader: %s\n", err_out ? err_out->c_str() : "");
        return false;
    }
    *out = reinterpret_cast<T>(sym);
    return true;
}

bool resolve_vtable(const char* name, void*** out, std::string* err_out) {
    dlerror();
    void* sym = dlsym(RTLD_DEFAULT, name);
    const char* msg = dlerror();
    if (sym == nullptr || msg != nullptr) {
        if (err_out) {
            *err_out = std::string("dlsym(") + name + "): " + (msg ? msg : "not found");
        }
        std::fprintf(stderr, "loader: %s\n", err_out ? err_out->c_str() : "");
        return false;
    }
    *out = reinterpret_cast<void**>(sym);
    return true;
}

void clear_fairplay_symbols(Symbols* s) {
    s->SVPlaybackLeaseManager_ctor                      = nullptr;
    s->SVPlaybackLeaseManager_refreshLeaseAutomatically = nullptr;
    s->SVPlaybackLeaseManager_requestLease              = nullptr;
    s->SVFootHillSessionCtrl_instance                   = nullptr;
    s->SVFootHillSessionCtrl_decryptContext             = nullptr;
    s->SVFootHillPContext_kdContext                     = nullptr;
    s->fp_sample_decrypt                                = nullptr;
    s->SVFootHillSessionCtrl_resetAllContexts           = nullptr;
    s->shared_ptr_SVFootHillPContext_dtor               = nullptr;
}

}  // namespace

bool Loader::open(const std::string& libs_dir) {
    if (ok_) return true;

    auto load = [&](const std::string& path, void** dest) {
        *dest = open_lib(path, &last_error_);
        return *dest != nullptr;
    };

    // libc++_shared.so is already a DT_NEEDED of the daemon; do not dlopen.
    //
    // Pre-load FairPlay helpers first so SVFootHill symbols resolve (some
    // builds only export the chain once CoreFP/CoreLSKD are in the process).
    trace("dlopen libCoreFP.so (optional)");
    h_libcorefp_   = open_lib_optional(libs_dir + "/libCoreFP.so");
    trace("dlopen libCoreLSKD.so (optional)");
    h_libcorelskd_ = open_lib_optional(libs_dir + "/libCoreLSKD.so");

    // Match upstream CMake link order: androidappmusic, storeservicescore,
    // mediaplatform (after cxx). Dependencies still resolve transitively.
    trace("dlopen libandroidappmusic.so");
    if (!load(libs_dir + "/libandroidappmusic.so",   &h_libandroidappmusic_)) {
        return false;
    }
    trace("dlopen libstoreservicescore.so");
    if (!load(libs_dir + "/libstoreservicescore.so", &h_libstoreservicescore_)) {
        return false;
    }
    trace("dlopen libmediaplatform.so");
    if (!load(libs_dir + "/libmediaplatform.so",     &h_libmediaplatform_)) {
        return false;
    }
    trace("all core libs loaded; starting symbol resolution");

    using namespace abi;

    if (!resolve(RTLD_DEFAULT,
                 mangled::resolv_set_nameservers_for_net,
                 &symbols_.resolv_set_nameservers_for_net, &last_error_)) {
        return false;
    }

    if (!resolve_vtable(mangled::vtable_RequestContextConfig,
                        &symbols_.vtable_RequestContextConfig, &last_error_)) {
        return false;
    }
    if (!resolve_vtable(mangled::vtable_CredentialsResponse,
                        &symbols_.vtable_CredentialsResponse, &last_error_)) {
        return false;
    }
    if (!resolve_vtable(mangled::vtable_ProtocolDialogResponse,
                        &symbols_.vtable_ProtocolDialogResponse, &last_error_)) {
        return false;
    }
    if (!resolve_vtable(mangled::vtable_HTTPMessage,
                        &symbols_.vtable_HTTPMessage, &last_error_)) {
        return false;
    }

#define RESOLVE(field, name) \
    if (!resolve(RTLD_DEFAULT, mangled::name, &symbols_.field, &last_error_)) return false

    RESOLVE(FootHillConfig_config,          FootHillConfig_config);
    RESOLVE(DeviceGUID_instance,            DeviceGUID_instance);
    RESOLVE(DeviceGUID_configure,           DeviceGUID_configure);
    RESOLVE(make_shared_RequestContext,     make_shared_RequestContext);
    RESOLVE(RequestContextConfig_ctor,      RequestContextConfig_ctor);

    RESOLVE(RCC_setBaseDirectoryPath,  RCC_setBaseDirectoryPath);
    RESOLVE(RCC_setClientIdentifier,   RCC_setClientIdentifier);
    RESOLVE(RCC_setVersionIdentifier,  RCC_setVersionIdentifier);
    RESOLVE(RCC_setPlatformIdentifier, RCC_setPlatformIdentifier);
    RESOLVE(RCC_setProductVersion,     RCC_setProductVersion);
    RESOLVE(RCC_setDeviceModel,        RCC_setDeviceModel);
    RESOLVE(RCC_setBuildVersion,       RCC_setBuildVersion);
    RESOLVE(RCC_setLocaleIdentifier,   RCC_setLocaleIdentifier);
    RESOLVE(RCC_setLanguageIdentifier, RCC_setLanguageIdentifier);

    RESOLVE(RequestContextManager_configure,         RequestContextManager_configure);
    RESOLVE(RequestContext_init,                     RequestContext_init);
    RESOLVE(RequestContext_setFairPlayDirectoryPath, RequestContext_setFairPlayDirectoryPath);

    RESOLVE(make_shared_AndroidPresentationInterface, make_shared_AndroidPresentationInterface);
    RESOLVE(API_setCredentialsHandler,                API_setCredentialsHandler);
    RESOLVE(API_setDialogHandler,                     API_setDialogHandler);
    RESOLVE(API_handleCredentialsResponse,            API_handleCredentialsResponse);
    RESOLVE(API_handleProtocolDialogResponse,         API_handleProtocolDialogResponse);
    RESOLVE(RequestContext_setPresentationInterface,  RequestContext_setPresentationInterface);

    RESOLVE(ProtocolDialog_title,    ProtocolDialog_title);
    RESOLVE(ProtocolDialog_message,  ProtocolDialog_message);
    RESOLVE(ProtocolDialog_buttons,  ProtocolDialog_buttons);
    RESOLVE(ProtocolButton_title,    ProtocolButton_title);
    RESOLVE(ProtocolDialogResponse_ctor, ProtocolDialogResponse_ctor);
    RESOLVE(ProtocolDialogResponse_setSelectedButton, ProtocolDialogResponse_setSelectedButton);

    RESOLVE(CR_requiresHSA2VerificationCode, CR_requiresHSA2VerificationCode);
    RESOLVE(CR_title,                        CR_title);
    RESOLVE(CR_message,                      CR_message);

    RESOLVE(CredentialsResponse_ctor,            CredentialsResponse_ctor);
    RESOLVE(CredentialsResponse_setUserName,     CredentialsResponse_setUserName);
    RESOLVE(CredentialsResponse_setPassword,     CredentialsResponse_setPassword);
    RESOLVE(CredentialsResponse_setResponseType, CredentialsResponse_setResponseType);

    RESOLVE(make_shared_AuthenticateFlow, make_shared_AuthenticateFlow);
    RESOLVE(AuthenticateFlow_run,         AuthenticateFlow_run);
    RESOLVE(AuthenticateFlow_response,    AuthenticateFlow_response);

    RESOLVE(AR_responseType,    AR_responseType);
    RESOLVE(AR_customerMessage, AR_customerMessage);
    RESOLVE(AR_error,           AR_error);

    RESOLVE(SEC_errorCode, SEC_errorCode);
    RESOLVE(SEC_what,      SEC_what);

    RESOLVE(DeviceGUID_guid, DeviceGUID_guid);
    RESOLVE(Data_bytes,      Data_bytes);

    RESOLVE(HTTPMessage_ctor,        HTTPMessage_ctor);
    // C1 is optional: arm64 builds may need the complete-object ctor
    // when HTTPMessage has virtual bases.  If it’s absent we fall back to C2.
    {
        dlerror();
        void* sym = dlsym(RTLD_DEFAULT, abi::mangled::HTTPMessage_ctor_c1);
        if (sym != nullptr && dlerror() == nullptr) {
            symbols_.HTTPMessage_ctor_c1 = reinterpret_cast<abi::fn_HTTPMessage_ctor_c1>(sym);
        }
    }
    RESOLVE(HTTPMessage_setHeader,   HTTPMessage_setHeader);
    RESOLVE(HTTPMessage_setBodyData, HTTPMessage_setBodyData);

    RESOLVE(URLRequest_ctor,              URLRequest_ctor);
    RESOLVE(URLRequest_setRequestParameter, URLRequest_setRequestParameter);
    RESOLVE(URLRequest_run,               URLRequest_run);
    RESOLVE(URLRequest_error,             URLRequest_error);
    RESOLVE(URLRequest_response,          URLRequest_response);
    RESOLVE(URLResponse_underlyingResponse, URLResponse_underlyingResponse);

    // ---- PurchaseRequest + protocolDictionary ----
    RESOLVE(PurchaseRequest_ctor,                    PurchaseRequest_ctor);
    RESOLVE(PurchaseRequest_setProcessDialogActions, PurchaseRequest_setProcessDialogActions);
    RESOLVE(PurchaseRequest_setURLBagKey,            PurchaseRequest_setURLBagKey);
    RESOLVE(PurchaseRequest_setBuyParameters,        PurchaseRequest_setBuyParameters);
    RESOLVE(PurchaseRequest_run,                     PurchaseRequest_run);
    RESOLVE(PurchaseRequest_response,                PurchaseRequest_response);
    RESOLVE(PurchaseResponse_error,                  PurchaseResponse_error);
    RESOLVE(PurchaseResponse_items,                  PurchaseResponse_items);
    RESOLVE(PurchaseItem_dictionary,                 PurchaseItem_dictionary);
    RESOLVE(URLRequest_setURLResponsePreprocessor,   URLRequest_setURLResponsePreprocessor);
    RESOLVE(URLResponse_protocolDictionary,          URLResponse_protocolDictionary);

    RESOLVE(RequestContext_storeFrontIdentifier, RequestContext_storeFrontIdentifier);

#undef RESOLVE

    // CoreFoundation symbols are plain C, name = symbol. They are loaded
    // transitively (libCoreFoundation.so is a DT_NEEDED of libstoreservicescore.so);
    // RTLD_DEFAULT picks them up.
    if (!resolve(RTLD_DEFAULT, "CFRetain",
                 &symbols_.CFRetain, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFRelease",
                 &symbols_.CFRelease, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFDataGetBytePtr",
                 &symbols_.CFDataGetBytePtr, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFDataGetLength",
                 &symbols_.CFDataGetLength, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFPropertyListCreateData",
                 &symbols_.CFPropertyListCreateData, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFStringCreateWithCString",
                 &symbols_.CFStringCreateWithCString, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFArrayCreate",
                 &symbols_.CFArrayCreate, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "CFDictionaryCreate",
                 &symbols_.CFDictionaryCreate, &last_error_)) return false;
    // CF callback structs are exported as data symbols; dlsym returns the
    // address of the global. Store as `const void*` and pass-through.
    if (!resolve(RTLD_DEFAULT, "kCFTypeArrayCallBacks",
                 &symbols_.kCFTypeArrayCallBacks, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "kCFTypeDictionaryKeyCallBacks",
                 &symbols_.kCFTypeDictionaryKeyCallBacks, &last_error_)) return false;
    if (!resolve(RTLD_DEFAULT, "kCFTypeDictionaryValueCallBacks",
                 &symbols_.kCFTypeDictionaryValueCallBacks, &last_error_)) return false;

    // CF type-introspection symbols (powering the CF->JSON walker).
#define RESOLVE_CF(name) \
    if (!resolve(RTLD_DEFAULT, #name, &symbols_.name, &last_error_)) return false

    RESOLVE_CF(CFGetTypeID);
    RESOLVE_CF(CFStringGetTypeID);
    RESOLVE_CF(CFNumberGetTypeID);
    RESOLVE_CF(CFBooleanGetTypeID);
    RESOLVE_CF(CFArrayGetTypeID);
    RESOLVE_CF(CFDictionaryGetTypeID);
    RESOLVE_CF(CFDataGetTypeID);
    RESOLVE_CF(CFDateGetTypeID);
    RESOLVE_CF(CFNullGetTypeID);

    RESOLVE_CF(CFStringGetCStringPtr);
    RESOLVE_CF(CFStringGetLength);
    RESOLVE_CF(CFStringGetCString);
    RESOLVE_CF(CFStringGetMaximumSizeForEncoding);

    RESOLVE_CF(CFNumberGetType);
    RESOLVE_CF(CFNumberGetValue);
    RESOLVE_CF(CFNumberIsFloatType);

    RESOLVE_CF(CFBooleanGetValue);
    RESOLVE_CF(CFArrayGetCount);
    RESOLVE_CF(CFArrayGetValueAtIndex);
    RESOLVE_CF(CFDictionaryGetCount);
    RESOLVE_CF(CFDictionaryGetKeysAndValues);
    RESOLVE_CF(CFDateGetAbsoluteTime);

#undef RESOLVE_CF

    clear_fairplay_symbols(&symbols_);
    std::string fp_err;
    // Match zhaarey/apple-music-downloader agent.js: FootHill + lease symbols are
    // resolved from libandroidappmusic.so exports. Try that handle first, then
    // RTLD_DEFAULT (bionic/global lookup can miss symbols visible via the DSO).
    auto resolve_fp = [&](const char* mangled_name, auto* out_slot) -> bool {
        void* const handles[] = {
            h_libandroidappmusic_,
            h_libmediaplatform_,
            h_libstoreservicescore_,
            nullptr,
        };
        for (int i = 0; handles[i] != nullptr; ++i) {
            if (resolve(handles[i], mangled_name, out_slot, &fp_err)) return true;
        }
        return resolve(RTLD_DEFAULT, mangled_name, out_slot, &fp_err);
    };
#define RESOLVE_FP(field, name) fp_ok &= resolve_fp(mangled::name, &symbols_.field)

    bool fp_ok = true;
    fp_ok &= RESOLVE_FP(SVPlaybackLeaseManager_ctor, SVPlaybackLeaseManager_ctor);
    fp_ok &= RESOLVE_FP(SVPlaybackLeaseManager_refreshLeaseAutomatically,
                        SVPlaybackLeaseManager_refreshLeaseAutomatically);
    fp_ok &= RESOLVE_FP(SVPlaybackLeaseManager_requestLease, SVPlaybackLeaseManager_requestLease);
    fp_ok &= RESOLVE_FP(SVFootHillSessionCtrl_instance, SVFootHillSessionCtrl_instance);
    foot_hill_persistent_key_fn_   = nullptr;
    foot_hill_persistent_key_abi8_ = false;
    {
        abi::fn_SVFootHillSessionCtrl_getPersistentKey  pk8 = nullptr;
        abi::fn_SVFootHillSessionCtrl_getPersistentKey7 pk7 = nullptr;
        bool                                              pk  = false;
        if (resolve_fp(mangled::SVFootHillSessionCtrl_getPersistentKey_8str, &pk8)) {
            foot_hill_persistent_key_fn_   = reinterpret_cast<void*>(pk8);
            foot_hill_persistent_key_abi8_ = true;
            pk                             = true;
        } else if (resolve_fp(mangled::SVFootHillSessionCtrl_getPersistentKey_7str, &pk7)) {
            foot_hill_persistent_key_fn_   = reinterpret_cast<void*>(pk7);
            foot_hill_persistent_key_abi8_ = false;
            pk                             = true;
        }
        fp_ok &= pk;
    }
    fp_ok &= RESOLVE_FP(SVFootHillSessionCtrl_decryptContext,
                        SVFootHillSessionCtrl_decryptContext);
    fp_ok &= RESOLVE_FP(SVFootHillPContext_kdContext, SVFootHillPContext_kdContext);
    fp_ok &= RESOLVE_FP(fp_sample_decrypt, fp_sample_decrypt);
    fp_ok &= RESOLVE_FP(SVFootHillSessionCtrl_resetAllContexts,
                        SVFootHillSessionCtrl_resetAllContexts);
    fp_ok &= RESOLVE_FP(shared_ptr_SVFootHillPContext_dtor,
                        shared_ptr_SVFootHillPContext_dtor);

#undef RESOLVE_FP

    if (foot_hill_persistent_key_fn_ != nullptr) {
        trace(foot_hill_persistent_key_abi8_
                  ? "FairPlay getPersistentKey ABI=8-string"
                  : "FairPlay getPersistentKey ABI=7-string");
    }

    if (!fp_ok) {
        clear_fairplay_symbols(&symbols_);
        foot_hill_persistent_key_fn_   = nullptr;
        foot_hill_persistent_key_abi8_ = false;
        fairplay_decrypt_available_ = false;
        std::fprintf(stderr,
                     "loader: FairPlay decrypt symbols unavailable (%s); "
                     "POST /decrypt disabled\n",
                     fp_err.c_str());
    } else {
        fairplay_decrypt_available_ = true;
    }

    ok_ = true;
    last_error_.clear();
    return true;
}

void Loader::close() {
    ok_                       = false;
    fairplay_decrypt_available_ = false;
    symbols_                  = Symbols{};
    foot_hill_persistent_key_fn_   = nullptr;
    foot_hill_persistent_key_abi8_ = false;
    h_libstoreservicescore_   = nullptr;
    h_libmediaplatform_       = nullptr;
    h_libandroidappmusic_     = nullptr;
    h_libcorefp_              = nullptr;
    h_libcorelskd_            = nullptr;
}

void Loader::foot_hill_get_persistent_key(abi::shared_ptr* ret,
                                          void*            foothill_instance,
                                          abi::std_string* adam_id,
                                          abi::std_string* key_uri,
                                          abi::std_string* key_format,
                                          abi::std_string* key_format_ver,
                                          abi::std_string* server_uri,
                                          abi::std_string* protocol_type,
                                          abi::std_string* fps_cert) const {
    if (foot_hill_persistent_key_fn_ == nullptr) return;
    if (foot_hill_persistent_key_abi8_) {
        auto* fn = reinterpret_cast<abi::fn_SVFootHillSessionCtrl_getPersistentKey>(
            foot_hill_persistent_key_fn_);
        aarch64_sret::svfoot_get_persistent_key(
            ret, foothill_instance, adam_id, adam_id, key_uri, key_format,
            key_format_ver, server_uri, protocol_type, fps_cert, fn);
    } else {
        auto* fn = reinterpret_cast<abi::fn_SVFootHillSessionCtrl_getPersistentKey7>(
            foot_hill_persistent_key_fn_);
        aarch64_sret::svfoot_get_persistent_key_7str(
            ret, foothill_instance, adam_id, key_uri, key_format,
            key_format_ver, server_uri, protocol_type, fps_cert, fn);
    }
}

}  // namespace wrapper::apple
