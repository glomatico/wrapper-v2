// Apple Music native-library loader.
//
// `Loader::open()` dlopens Apple's JNI stack (mediaplatform, storeservicescore,
// androidappmusic) and optional FPS helpers (Apple CoreFP / CoreLSKD), then
// resolves mangled symbols into `Symbols`.
//
// The daemon ELF itself has *no* DT_NEEDED reference to Apple's libs -
// this is what lets us start the daemon in stub mode even when the libs
// have not been staged. If core libs are missing or a non-FPS
// symbol fails to resolve, `open()` returns false. FPS-only symbols
// may fail separately; see `fps_decrypt_available()`.

#pragma once

#include <string>

#include "apple/abi.hpp"

namespace wrapper::apple {

// Function-pointer table populated by Loader::open(). Core auth/token
// fields are non-null after ok()==true. FPS decrypt pointers (lease,
// FootHill, fp_sample_decrypt) are set only when fps_decrypt_available()
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

    // ---- AndroidPresentationInterface + auth flow ----
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

    // ---- token harvest ----
    abi::fn_DeviceGUID_guid DeviceGUID_guid = nullptr;
    abi::fn_Data_bytes      Data_bytes      = nullptr;

    abi::fn_HTTPMessage_ctor        HTTPMessage_ctor        = nullptr;
    abi::fn_HTTPMessage_ctor_c1     HTTPMessage_ctor_c1     = nullptr;  // arm64 fallback
    abi::fn_HTTPMessage_setHeader   HTTPMessage_setHeader   = nullptr;
    abi::fn_HTTPMessage_setBodyData HTTPMessage_setBodyData = nullptr;

    abi::fn_URLRequest_ctor              URLRequest_ctor              = nullptr;
    abi::fn_URLRequest_setRequestParameter URLRequest_setRequestParameter = nullptr;
    abi::fn_URLRequest_run               URLRequest_run               = nullptr;
    abi::fn_URLRequest_error             URLRequest_error             = nullptr;
    abi::fn_URLRequest_response          URLRequest_response          = nullptr;
    abi::fn_URLResponse_underlyingResponse URLResponse_underlyingResponse = nullptr;

    // ---- PurchaseRequest / protocolDictionary / CoreFoundation ----
    abi::fn_PurchaseRequest_ctor                     PurchaseRequest_ctor                     = nullptr;
    abi::fn_PurchaseRequest_setProcessDialogActions  PurchaseRequest_setProcessDialogActions  = nullptr;
    abi::fn_PurchaseRequest_set_string               PurchaseRequest_setURLBagKey             = nullptr;
    abi::fn_PurchaseRequest_set_string               PurchaseRequest_setBuyParameters         = nullptr;
    abi::fn_PurchaseRequest_run                      PurchaseRequest_run                      = nullptr;
    abi::fn_PurchaseRequest_response                 PurchaseRequest_response                 = nullptr;
    abi::fn_PurchaseResponse_error                   PurchaseResponse_error                   = nullptr;
    abi::fn_PurchaseResponse_items                   PurchaseResponse_items                   = nullptr;
    abi::fn_PurchaseItem_dictionary                  PurchaseItem_dictionary                  = nullptr;
    abi::fn_URLRequest_setURLResponsePreprocessor    URLRequest_setURLResponsePreprocessor    = nullptr;
    abi::fn_URLResponse_protocolDictionary           URLResponse_protocolDictionary           = nullptr;

    // CoreFoundation: we serialize a CFDictionary* (either the URLResponse's
    // protocolDictionary, or a synthetic dict we build from PurchaseItem
    // dictionaries when the preprocessor hook doesn't fire) back to XML
    // plist bytes for the HTTP response. CFRelease drops the retained refs.
    // These are plain C symbols, no mangling.
    void* (*CFRetain)(const void* cf)                                         = nullptr;
    void  (*CFRelease)(const void* cf)                                        = nullptr;
    const unsigned char* (*CFDataGetBytePtr)(const void* data)                = nullptr;
    long  (*CFDataGetLength)(const void* data)                                = nullptr;
    // CFPropertyListCreateData(allocator, plist, format, options, error)
    // format=100 is kCFPropertyListXMLFormat_v1_0.
    void* (*CFPropertyListCreateData)(void* alloc, void* plist,
                                      long format, unsigned long options,
                                      void* error)                            = nullptr;
    // Container builders for the synthetic root dict when /playback has
    // to fall back from the preprocessor hook to PurchaseResponse::items().
    void* (*CFStringCreateWithCString)(void* alloc, const char* cstr,
                                       unsigned long encoding)                = nullptr;
    void* (*CFArrayCreate)(void* alloc, const void* const* values,
                           long numValues, const void* callbacks)             = nullptr;
    void* (*CFDictionaryCreate)(void* alloc, const void* const* keys,
                                const void* const* values, long numValues,
                                const void* key_cb, const void* val_cb)       = nullptr;
    const void* kCFTypeArrayCallBacks               = nullptr;
    const void* kCFTypeDictionaryKeyCallBacks       = nullptr;
    const void* kCFTypeDictionaryValueCallBacks     = nullptr;

    // CF type introspection (used by the CF->JSON walker that powers
    // GET /playback's body). On x86_64 CFTypeID is unsigned long.
    unsigned long (*CFGetTypeID)(const void* cf)                              = nullptr;
    unsigned long (*CFStringGetTypeID)()                                       = nullptr;
    unsigned long (*CFNumberGetTypeID)()                                       = nullptr;
    unsigned long (*CFBooleanGetTypeID)()                                      = nullptr;
    unsigned long (*CFArrayGetTypeID)()                                        = nullptr;
    unsigned long (*CFDictionaryGetTypeID)()                                   = nullptr;
    unsigned long (*CFDataGetTypeID)()                                         = nullptr;
    unsigned long (*CFDateGetTypeID)()                                         = nullptr;
    unsigned long (*CFNullGetTypeID)()                                         = nullptr;

    const char*   (*CFStringGetCStringPtr)(const void* str,
                                           unsigned long encoding)             = nullptr;
    long          (*CFStringGetLength)(const void* str)                       = nullptr;
    unsigned char (*CFStringGetCString)(const void* str, char* buf,
                                        long buf_size,
                                        unsigned long encoding)                = nullptr;
    long          (*CFStringGetMaximumSizeForEncoding)(long length,
                                                       unsigned long enc)      = nullptr;

    long          (*CFNumberGetType)(const void* num)                         = nullptr;
    unsigned char (*CFNumberGetValue)(const void* num, long type,
                                       void* out)                              = nullptr;
    unsigned char (*CFNumberIsFloatType)(const void* num)                     = nullptr;

    unsigned char (*CFBooleanGetValue)(const void* b)                         = nullptr;

    long          (*CFArrayGetCount)(const void* arr)                         = nullptr;
    const void*   (*CFArrayGetValueAtIndex)(const void* arr, long idx)        = nullptr;

    long          (*CFDictionaryGetCount)(const void* d)                      = nullptr;
    void          (*CFDictionaryGetKeysAndValues)(const void* d,
                                                  const void** keys,
                                                  const void** values)         = nullptr;

    double        (*CFDateGetAbsoluteTime)(const void* date)                  = nullptr;

    abi::fn_RequestContext_storeFrontIdentifier RequestContext_storeFrontIdentifier = nullptr;

    // ---- FPS decrypt ----
    abi::fn_SVPlaybackLeaseManager_ctor                      SVPlaybackLeaseManager_ctor                      = nullptr;
    abi::fn_SVPlaybackLeaseManager_refreshLeaseAutomatically SVPlaybackLeaseManager_refreshLeaseAutomatically = nullptr;
    abi::fn_SVPlaybackLeaseManager_requestLease              SVPlaybackLeaseManager_requestLease              = nullptr;
    abi::fn_SVFootHillSessionCtrl_instance                   SVFootHillSessionCtrl_instance                   = nullptr;
    abi::fn_SVFootHillSessionCtrl_decryptContext               SVFootHillSessionCtrl_decryptContext             = nullptr;
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

    // True when FPS decrypt dlsyms all succeeded (lease + FootHill + sample op).
    bool fps_decrypt_available() const { return fps_decrypt_available_; }

    const Symbols& sym() const { return symbols_; }

    // Last failure description (path of the lib, symbol name, dlerror()).
    // Empty if no failure has been recorded.
    const std::string& last_error() const { return last_error_; }

    // SVFootHillSessionCtrl::getPersistentKey — two possible ABIs; resolved in
    // open(), dispatched with AArch64 sret thunks in loader.cpp.
    void foot_hill_get_persistent_key(abi::shared_ptr* ret,
                                      void*             foothill_instance,
                                      abi::std_string*  adam_id,
                                      abi::std_string*  key_uri,
                                      abi::std_string*  key_format,
                                      abi::std_string*  key_format_ver,
                                      abi::std_string*  server_uri,
                                      abi::std_string*  protocol_type,
                                      abi::std_string*  fps_cert) const;

private:
    void* h_libstoreservicescore_ = nullptr;
    void* h_libmediaplatform_     = nullptr;
    void* h_libandroidappmusic_   = nullptr;
    void* h_libcorefp_            = nullptr;
    void* h_libcorelskd_          = nullptr;

    Symbols     symbols_;
    bool        ok_{false};
    bool        fps_decrypt_available_{false};
    std::string last_error_;

    void* foot_hill_persistent_key_fn_   = nullptr;
    bool  foot_hill_persistent_key_abi8_ = false;
};

}  // namespace wrapper::apple
