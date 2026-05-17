#include "apple/loader.hpp"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>

namespace wrapper::apple {

namespace {

// Helper: dlopen with friendlier error reporting.
void* open_lib(const std::string& path, std::string* err_out) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char* msg = dlerror();
        if (err_out != nullptr) {
            *err_out = "dlopen(" + path + "): " + (msg ? msg : "unknown error");
        }
        std::fprintf(stderr, "loader: %s\n", err_out ? err_out->c_str() : "");
    }
    return h;
}

void* open_lib_optional(const std::string& path) {
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (h == nullptr) {
        const char* msg = dlerror();
        std::fprintf(stderr, "loader: optional dlopen %s: %s\n", path.c_str(),
                     msg ? msg : "?");
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
    s->SVFootHillSessionCtrl_getPersistentKey           = nullptr;
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
    h_libcorefp_   = open_lib_optional(libs_dir + "/libCoreFP.so");
    h_libcorelskd_ = open_lib_optional(libs_dir + "/libCoreLSKD.so");

    // Match upstream CMake link order: androidappmusic, storeservicescore,
    // mediaplatform (after cxx). Dependencies still resolve transitively.
    if (!load(libs_dir + "/libandroidappmusic.so",   &h_libandroidappmusic_)) {
        return false;
    }
    if (!load(libs_dir + "/libstoreservicescore.so", &h_libstoreservicescore_)) {
        return false;
    }
    if (!load(libs_dir + "/libmediaplatform.so",     &h_libmediaplatform_)) {
        return false;
    }

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
    RESOLVE(HTTPMessage_setHeader,   HTTPMessage_setHeader);
    RESOLVE(HTTPMessage_setBodyData, HTTPMessage_setBodyData);

    RESOLVE(URLRequest_ctor,              URLRequest_ctor);
    RESOLVE(URLRequest_setRequestParameter, URLRequest_setRequestParameter);
    RESOLVE(URLRequest_run,               URLRequest_run);
    RESOLVE(URLRequest_error,             URLRequest_error);
    RESOLVE(URLRequest_response,          URLRequest_response);
    RESOLVE(URLResponse_underlyingResponse, URLResponse_underlyingResponse);

    RESOLVE(RequestContext_storeFrontIdentifier, RequestContext_storeFrontIdentifier);

#undef RESOLVE

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
    fp_ok &= RESOLVE_FP(SVFootHillSessionCtrl_getPersistentKey,
                        SVFootHillSessionCtrl_getPersistentKey);
    fp_ok &= RESOLVE_FP(SVFootHillSessionCtrl_decryptContext,
                        SVFootHillSessionCtrl_decryptContext);
    fp_ok &= RESOLVE_FP(SVFootHillPContext_kdContext, SVFootHillPContext_kdContext);
    fp_ok &= RESOLVE_FP(fp_sample_decrypt, fp_sample_decrypt);
    fp_ok &= RESOLVE_FP(SVFootHillSessionCtrl_resetAllContexts,
                        SVFootHillSessionCtrl_resetAllContexts);
    fp_ok &= RESOLVE_FP(shared_ptr_SVFootHillPContext_dtor,
                        shared_ptr_SVFootHillPContext_dtor);

#undef RESOLVE_FP

    if (!fp_ok) {
        clear_fairplay_symbols(&symbols_);
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
    h_libstoreservicescore_   = nullptr;
    h_libmediaplatform_       = nullptr;
    h_libandroidappmusic_     = nullptr;
    h_libcorefp_              = nullptr;
    h_libcorelskd_            = nullptr;
}

}  // namespace wrapper::apple
