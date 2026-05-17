// Apple Music native-library loader.
//
// `Loader::open()` dlopens Apple's JNI stack (mediaplatform, storeservicescore,
// androidappmusic) and optional FairPlay helpers (CoreFP / CoreLSKD), then
// resolves mangled symbols into `Symbols`.
//
// The daemon ELF itself has *no* DT_NEEDED reference to Apple's libs -
// this is what lets us start the daemon in stub mode even when the libs
// have not been staged. If core libs are missing or a non-FairPlay
// symbol fails to resolve, `open()` returns false. FairPlay-only symbols
// may fail separately; see `fairplay_decrypt_available()`.

#pragma once

#include <string>

#include "apple/abi.hpp"

namespace wrapper::apple {

// Function-pointer table populated by Loader::open(). Core auth/token
// fields are non-null after ok()==true. FairPlay decrypt pointers (lease,
// FootHill, fp_sample_decrypt) are set only when fairplay_decrypt_available()
// is true; otherwise they may be null even if ok().
struct Symbols {
    // Vtables for std::__shared_ptr_emplace<T, ...>. Stored as `void**`
    // so callers can do the upstream-style pointer arithmetic (skip
    // past type_info + dtor slots).
    void** vtable_RequestContextConfig = nullptr;
    void** vtable_CredentialsResponse  = nullptr;
    void** vtable_ProtocolDialogResponse = nullptr;
    void** vtable_HTTPMessage          = nullptr;

    abi::fn_resolv_set_nameservers_for_net     resolv_set_nameservers_for_net = nullptr;
    abi::fn_FootHillConfig_config              FootHillConfig_config          = nullptr;
    abi::fn_DeviceGUID_instance                DeviceGUID_instance            = nullptr;
    abi::fn_DeviceGUID_configure               DeviceGUID_configure           = nullptr;
    abi::fn_make_shared_RequestContext         make_shared_RequestContext     = nullptr;
    abi::fn_RequestContextConfig_ctor          RequestContextConfig_ctor      = nullptr;

    abi::fn_RCC_set_string RCC_setBaseDirectoryPath  = nullptr;
    abi::fn_RCC_set_string RCC_setClientIdentifier   = nullptr;
    abi::fn_RCC_set_string RCC_setVersionIdentifier  = nullptr;
    abi::fn_RCC_set_string RCC_setPlatformIdentifier = nullptr;
    abi::fn_RCC_set_string RCC_setProductVersion     = nullptr;
    abi::fn_RCC_set_string RCC_setDeviceModel        = nullptr;
    abi::fn_RCC_set_string RCC_setBuildVersion       = nullptr;
    abi::fn_RCC_set_string RCC_setLocaleIdentifier   = nullptr;
    abi::fn_RCC_set_string RCC_setLanguageIdentifier = nullptr;

    abi::fn_RequestContextManager_configure RequestContextManager_configure = nullptr;
    abi::fn_RequestContext_init             RequestContext_init             = nullptr;
    abi::fn_RCC_set_string                  RequestContext_setFairPlayDirectoryPath = nullptr;

    // ---- Phase 1.1: AndroidPresentationInterface + auth flow ----
    abi::fn_make_shared_AndroidPresentationInterface make_shared_AndroidPresentationInterface = nullptr;
    abi::fn_API_setCredentialsHandler                API_setCredentialsHandler                = nullptr;
    abi::fn_API_setDialogHandler                     API_setDialogHandler                     = nullptr;
    abi::fn_API_handleCredentialsResponse            API_handleCredentialsResponse            = nullptr;
    abi::fn_API_handleProtocolDialogResponse         API_handleProtocolDialogResponse         = nullptr;
    abi::fn_RequestContext_setPresentationInterface  RequestContext_setPresentationInterface  = nullptr;

    abi::fn_ProtocolDialog_title    ProtocolDialog_title    = nullptr;
    abi::fn_ProtocolDialog_message  ProtocolDialog_message  = nullptr;
    abi::fn_ProtocolDialog_buttons  ProtocolDialog_buttons  = nullptr;
    abi::fn_ProtocolButton_title    ProtocolButton_title    = nullptr;
    abi::fn_ProtocolDialogResponse_ctor ProtocolDialogResponse_ctor = nullptr;
    abi::fn_ProtocolDialogResponse_setSelectedButton ProtocolDialogResponse_setSelectedButton = nullptr;

    abi::fn_CR_requiresHSA2VerificationCode CR_requiresHSA2VerificationCode = nullptr;
    abi::fn_CR_title                        CR_title                        = nullptr;
    abi::fn_CR_message                      CR_message                      = nullptr;

    abi::fn_CredentialsResponse_ctor           CredentialsResponse_ctor           = nullptr;
    abi::fn_CredentialsResponse_set_string     CredentialsResponse_setUserName    = nullptr;
    abi::fn_CredentialsResponse_set_string     CredentialsResponse_setPassword    = nullptr;
    abi::fn_CredentialsResponse_setResponseType CredentialsResponse_setResponseType = nullptr;

    abi::fn_make_shared_AuthenticateFlow make_shared_AuthenticateFlow = nullptr;
    abi::fn_AuthenticateFlow_run         AuthenticateFlow_run         = nullptr;
    abi::fn_AuthenticateFlow_response    AuthenticateFlow_response    = nullptr;

    abi::fn_AR_responseType    AR_responseType    = nullptr;
    abi::fn_AR_customerMessage AR_customerMessage = nullptr;
    abi::fn_AR_error           AR_error           = nullptr;

    abi::fn_SEC_errorCode SEC_errorCode = nullptr;
    abi::fn_SEC_what      SEC_what      = nullptr;

    // ---- Phase 1.1: token harvest ----
    abi::fn_DeviceGUID_guid DeviceGUID_guid = nullptr;
    abi::fn_Data_bytes      Data_bytes      = nullptr;

    abi::fn_HTTPMessage_ctor        HTTPMessage_ctor        = nullptr;
    abi::fn_HTTPMessage_setHeader   HTTPMessage_setHeader   = nullptr;
    abi::fn_HTTPMessage_setBodyData HTTPMessage_setBodyData = nullptr;

    abi::fn_URLRequest_ctor              URLRequest_ctor              = nullptr;
    abi::fn_URLRequest_setRequestParameter URLRequest_setRequestParameter = nullptr;
    abi::fn_URLRequest_run               URLRequest_run               = nullptr;
    abi::fn_URLRequest_error             URLRequest_error             = nullptr;
    abi::fn_URLRequest_response          URLRequest_response          = nullptr;
    abi::fn_URLResponse_underlyingResponse URLResponse_underlyingResponse = nullptr;

    abi::fn_RequestContext_storeFrontIdentifier RequestContext_storeFrontIdentifier = nullptr;

    // ---- Phase 1.3: FairPlay decrypt ----
    abi::fn_SVPlaybackLeaseManager_ctor                      SVPlaybackLeaseManager_ctor                      = nullptr;
    abi::fn_SVPlaybackLeaseManager_refreshLeaseAutomatically SVPlaybackLeaseManager_refreshLeaseAutomatically = nullptr;
    abi::fn_SVPlaybackLeaseManager_requestLease              SVPlaybackLeaseManager_requestLease              = nullptr;
    abi::fn_SVFootHillSessionCtrl_instance                   SVFootHillSessionCtrl_instance                   = nullptr;
    abi::fn_SVFootHillSessionCtrl_getPersistentKey           SVFootHillSessionCtrl_getPersistentKey           = nullptr;
    abi::fn_SVFootHillSessionCtrl_decryptContext             SVFootHillSessionCtrl_decryptContext             = nullptr;
    abi::fn_SVFootHillPContext_kdContext                     SVFootHillPContext_kdContext                     = nullptr;
    abi::fn_fp_sample_decrypt                                fp_sample_decrypt                                = nullptr;
    abi::fn_SVFootHillSessionCtrl_resetAllContexts           SVFootHillSessionCtrl_resetAllContexts           = nullptr;
    abi::fn_shared_ptr_SVFootHillPContext_dtor               shared_ptr_SVFootHillPContext_dtor               = nullptr;
};

class Loader {
public:
    Loader() = default;

    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;

    // Try to dlopen Apple's libs from `libs_dir` (typically /system/lib64
    // inside the chroot). On success, resolves all symbols into the
    // internal Symbols table and returns true. On failure, leaves the
    // loader in a closed state and returns false; subsequent ok() calls
    // return false.
    bool open(const std::string& libs_dir);

    // Release all dlopen handles. Symbols become invalid.
    void close();

    bool ok() const { return ok_; }

    // True when FairPlay decrypt dlsyms all succeeded (lease + FootHill + sample op).
    bool fairplay_decrypt_available() const { return fairplay_decrypt_available_; }

    const Symbols& sym() const { return symbols_; }

    // Last failure description (path of the lib, symbol name, dlerror()).
    // Empty if no failure has been recorded.
    const std::string& last_error() const { return last_error_; }

private:
    void* h_libstoreservicescore_ = nullptr;
    void* h_libmediaplatform_     = nullptr;
    void* h_libandroidappmusic_   = nullptr;
    void* h_libcorefp_            = nullptr;
    void* h_libcorelskd_          = nullptr;

    Symbols     symbols_;
    bool        ok_{false};
    bool        fairplay_decrypt_available_{false};
    std::string last_error_;
};

}  // namespace wrapper::apple
