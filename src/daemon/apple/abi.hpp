// Apple Music native-lib ABI bindings (x86_64, APK 3.6.0-beta).
//
// We call into Apple's libstoreservicescore / libmediaplatform /
// libandroidappmusic by way of the Itanium-mangled C++ symbols they
// export. Each symbol below is declared as a function pointer type;
// the Loader class (apple/loader.hpp) dlopens the libs at runtime
// and resolves the symbols via dlsym.
//
// We deliberately treat all Apple types as opaque (`void*`) and pass
// strings as pointers to a layout-compatible `std_string` union. This
// is the pattern used by the upstream `wrapper` project; it avoids
// compile-time inclusion of Apple headers we don't have, at the cost
// of giving up type safety.
//
// Stability: the symbol names below are pinned to APK build 1109. If
// the APK is changed, re-derive with `nm -D --defined libstoreservicescore.so`
// and update LIBS_VERSION.json.
//
// Phase 1.0 brought runtime init online (DNS, DeviceGUID, RequestContext,
// FootHillConfig). Phase 1.1 adds AuthenticateFlow + URLRequest token harvest.
// Phase 1.3 adds SVPlaybackLeaseManager + FairPlay sample decrypt symbols.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wrapper::apple::abi {

// libc++ on the Android NDK uses an inline namespace `__ndk1`, which
// shows up in mangled names. The C++ standard library types we touch
// (`std::shared_ptr`, `std::basic_string`, `std::vector`) have a
// well-known POD-ish layout we can fake without including <memory>
// etc. Mismatching this layout WILL miscompile silently; tread
// carefully.

// std::__ndk1::shared_ptr<T> on x86_64: { T* obj; control_block* ctl; }.
// Total size 16 bytes. Apple methods returning shared_ptr take a
// hidden first arg pointing at one of these.
struct shared_ptr {
    void* obj = nullptr;
    void* ctrl_blk = nullptr;
};

// std::__ndk1::basic_string<char> on x86_64 uses the libc++ "long" /
// "short" union layout. Size: 24 bytes. Short mode uses the low bit
// of the first byte as the tag (0 = short). Long mode stores
// {capacity, size, data_ptr}.
union std_string {
    struct {
        std::uint8_t mark;     // bit 0 set => long mode
        char str[23];          // SSO buffer (22 chars + NUL)
    } _short;
    struct {
        std::size_t cap;       // capacity (with bit 0 = 1 => long)
        std::size_t size;      // length
        const char* data;      // pointer to heap-allocated storage
    } _long;
};

static_assert(sizeof(std_string) == 24, "std_string ABI mismatch");
static_assert(sizeof(shared_ptr) == 16, "shared_ptr ABI mismatch");

// Pass a C string to an Apple method by reference. We deliberately do
// NOT take ownership of `s`: the Apple call reads the pointer
// directly. The caller must keep the underlying buffer live for the
// duration of the call.
inline std_string make_string_view(const char* s) {
    std_string out{};
    out._long.cap = 1;                    // any odd value => long mode
    out._long.size = std::strlen(s);
    out._long.data = s;
    return out;
}

// Empty std::vector<T> on x86_64: three null pointers (begin/end/capacity).
struct std_vector {
    void* begin = nullptr;
    void* end = nullptr;
    void* end_capacity = nullptr;
};

// ---------------------------------------------------------------------------
// Function pointer types for runtime-init symbols (Phase 1.0)
// ---------------------------------------------------------------------------
//
// `using fn_X = void (*)(...);` rather than direct extern "C" decls.
// This keeps the daemon ELF free of unresolved Apple symbols at link
// time so it can be started without the Apple libs available - useful
// for stub-mode operation (WRAPPER_APPLE_INIT=0).

// bionic: void _resolv_set_nameservers_for_net(unsigned, const char**, int, const char*)
using fn_resolv_set_nameservers_for_net =
    void (*)(unsigned netid, const char** servers, int count, const char* domains);

// FootHillConfig::config(std::string const&)
using fn_FootHillConfig_config = void (*)(std_string* device_id);

// storeservicescore::DeviceGUID::instance() -> shared_ptr<DeviceGUID>
using fn_DeviceGUID_instance = void (*)(shared_ptr* out);

// storeservicescore::DeviceGUID::configure(std::string const&,
//                                          std::string const&,
//                                          unsigned int const&,
//                                          bool const&)
using fn_DeviceGUID_configure =
    void (*)(void* hidden_return,
             void* this_,
             std_string* arg1,
             std_string* arg2,
             const unsigned int* arg3,
             const std::uint8_t* arg4);

// std::shared_ptr<RequestContext>::make_shared<std::string&>(std::string&)
using fn_make_shared_RequestContext =
    void (*)(shared_ptr* out, std_string* arg);

// storeservicescore::RequestContextConfig::RequestContextConfig()
using fn_RequestContextConfig_ctor = void (*)(void* this_);

// All RequestContextConfig string setters have the same signature.
using fn_RCC_set_string = void (*)(void* this_, std_string* s);

// RequestContextManager::configure(shared_ptr<RequestContext> const&)
using fn_RequestContextManager_configure = void (*)(shared_ptr* req_ctx);

// storeservicescore::RequestContext::init(shared_ptr<RequestContextConfig> const&)
using fn_RequestContext_init =
    void (*)(void* hidden_return, void* this_, shared_ptr* config);

// ---------------------------------------------------------------------------
// Phase 1.1 - AndroidPresentationInterface + callbacks
// ---------------------------------------------------------------------------
//
// AuthenticateFlow uses a "presentation interface" object to ask the
// host program for credentials and to surface UI dialogs. We satisfy
// it with the Android-flavored implementation Apple ships
// (`AndroidPresentationInterface`), and we register two handlers whose
// signatures match Apple's C++ `std::shared_ptr` callback types. The
// dialog handler logs each ProtocolDialog and must call
// handleProtocolDialogResponse or login stalls.

// std::shared_ptr<AndroidPresentationInterface>::make_shared<>()
using fn_make_shared_AndroidPresentationInterface = void (*)(shared_ptr* out);

// Apple's setCredentialsHandler / setDialogHandler store a function pointer
// whose C++ type passes std::shared_ptr by value. On x86_64 Itanium ABI,
// std::shared_ptr has a non-trivial copy ctor/dtor so it is passed *indirectly*
// (the caller puts a pointer to a temporary in %rdi/%rsi). Declaring the
// callback with explicit shared_ptr* parameters matches that exactly — which
// is also what the upstream C wrapper does (struct shared_ptr *).
using fn_credential_handler = void (*)(shared_ptr* cred_request,
                                       shared_ptr* cred_response_handler);

using fn_dialog_handler = void (*)(long dialog_id,
                                   shared_ptr* dialog,
                                   shared_ptr* response_handler);

// AndroidPresentationInterface::setCredentialsHandler(fn_credential_handler)
using fn_API_setCredentialsHandler =
    void (*)(void* this_, fn_credential_handler cb);

// AndroidPresentationInterface::setDialogHandler(fn_dialog_handler)
using fn_API_setDialogHandler =
    void (*)(void* this_, fn_dialog_handler cb);

// AndroidPresentationInterface::handleCredentialsResponse(
//     shared_ptr<CredentialsResponse> const&)
using fn_API_handleCredentialsResponse =
    void (*)(void* this_, shared_ptr* cred_response);

// AndroidPresentationInterface::handleProtocolDialogResponse(
//     long const&, shared_ptr<ProtocolDialogResponse> const&)
using fn_API_handleProtocolDialogResponse =
    void (*)(void* this_, const long* dialog_id, const shared_ptr* dialog_response);

// ---------------------------------------------------------------------------
// Phase 1.1 - ProtocolDialog / ProtocolDialogResponse (login UI)
// ---------------------------------------------------------------------------

using fn_ProtocolDialog_title    = std_string* (*)(void* this_);
using fn_ProtocolDialog_message  = std_string* (*)(void* this_);
using fn_ProtocolDialog_buttons  = std_vector* (*)(void* this_);
using fn_ProtocolButton_title    = std_string* (*)(void* this_);
using fn_ProtocolDialogResponse_ctor = void (*)(void* this_);
using fn_ProtocolDialogResponse_setSelectedButton =
    void (*)(void* this_, const shared_ptr* button);

// storeservicescore::RequestContext::setPresentationInterface(
//     shared_ptr<PresentationInterface> const&)
using fn_RequestContext_setPresentationInterface =
    void (*)(void* this_, shared_ptr* presentation_interface);

// ---------------------------------------------------------------------------
// Phase 1.1 - CredentialsRequest / CredentialsResponse
// ---------------------------------------------------------------------------
//
// CredentialsRequest is what gets passed *to* the credential handler
// (read-only metadata about why credentials are being asked for).
// CredentialsResponse is what we construct and return.

// CredentialsRequest::requiresHSA2VerificationCode() const
using fn_CR_requiresHSA2VerificationCode = std::uint8_t (*)(void* this_);

// CredentialsRequest::title() const / message() const - return a
// pointer to a std::string we may read but must not free.
using fn_CR_title   = std_string* (*)(void* this_);
using fn_CR_message = std_string* (*)(void* this_);

// CredentialsResponse::CredentialsResponse() - default constructor.
using fn_CredentialsResponse_ctor = void (*)(void* this_);

// CredentialsResponse::setUserName / setPassword (std::string const&)
using fn_CredentialsResponse_set_string = void (*)(void* this_, std_string* s);

// CredentialsResponse::setResponseType(ResponseType) - upstream passes
// `2` after setting username+password to signal "submit credentials".
using fn_CredentialsResponse_setResponseType = void (*)(void* this_, int response_type);

// ---------------------------------------------------------------------------
// Phase 1.1 - AuthenticateFlow / AuthenticateResponse
// ---------------------------------------------------------------------------

// std::shared_ptr<AuthenticateFlow>::make_shared<RequestContext&>(RequestContext&)
using fn_make_shared_AuthenticateFlow =
    void (*)(shared_ptr* out, shared_ptr* req_ctx);

// AuthenticateFlow::run() - synchronous; fires credentialHandler internally.
using fn_AuthenticateFlow_run = void (*)(void* this_);

// AuthenticateFlow::response() const - returns a shared_ptr<AuthenticateResponse>
// owned by the flow.
using fn_AuthenticateFlow_response = shared_ptr* (*)(void* this_);

// AuthenticateResponse::responseType() const - 6 means success;
// other values indicate failure or cancellation.
using fn_AR_responseType = int (*)(void* this_);

// AuthenticateResponse::customerMessage() const - returns pointer to
// a std::string (might be empty).
using fn_AR_customerMessage = std_string* (*)(void* this_);

// AuthenticateResponse::error() const - returns a shared_ptr<StoreErrorCondition>;
// null if no error.
using fn_AR_error = shared_ptr* (*)(void* this_);

// ---------------------------------------------------------------------------
// Phase 1.1 - StoreErrorCondition
// ---------------------------------------------------------------------------

using fn_SEC_errorCode = int (*)(void* this_);
using fn_SEC_what      = const char* (*)(void* this_);

// ---------------------------------------------------------------------------
// Phase 1.1 - DeviceGUID accessor + mediaplatform::Data
// ---------------------------------------------------------------------------

// DeviceGUID::guid() - returns a (Data, ...) tuple via a hidden 16-byte
// out param. The first 8 bytes are a pointer to mediaplatform::Data
// whose bytes() yields the GUID string.
using fn_DeviceGUID_guid = void (*)(void* hidden_return /* [2]void* */,
                                    void* this_);

// mediaplatform::Data::bytes() const -> char* (NUL-terminated for the
// strings Apple's HTTP layer returns).
using fn_Data_bytes = const char* (*)(void* this_);

// ---------------------------------------------------------------------------
// Phase 1.1 - URLRequest / URLResponse / HTTPMessage
// ---------------------------------------------------------------------------
//
// To call out to Apple's iTunes endpoints (apiToken, createMusicToken)
// we construct an HTTPMessage, wrap it in a URLRequest which is bound
// to our RequestContext (so it gets signed with DSID + X-Token), call
// run(), then unwrap URLResponse -> underlyingResponse -> raw bytes.

// HTTPMessage::HTTPMessage(string url, string method)
using fn_HTTPMessage_ctor =
    void (*)(void* this_, std_string* url, std_string* method);

// HTTPMessage::setHeader(string name, string value)
using fn_HTTPMessage_setHeader =
    void (*)(void* this_, std_string* name, std_string* value);

// HTTPMessage::setBodyData(char* body, size_t len)
using fn_HTTPMessage_setBodyData =
    void (*)(void* this_, const char* body, std::size_t len);

// URLRequest::URLRequest(shared_ptr<HTTPMessage> const&,
//                       shared_ptr<RequestContext> const&)
using fn_URLRequest_ctor =
    void (*)(void* this_, shared_ptr* http_message, shared_ptr* req_ctx);

// URLRequest::setRequestParameter(std::string name, std::string value)
// - the two-string overload (a vector<string> overload also exists).
using fn_URLRequest_setRequestParameter =
    void (*)(void* this_, std_string* name, std_string* value);

using fn_URLRequest_run      = void (*)(void* this_);
using fn_URLRequest_error    = shared_ptr* (*)(void* this_);
using fn_URLRequest_response = shared_ptr* (*)(void* this_);

// URLResponse::underlyingResponse() const - returns the inner
// HTTPMessage shared_ptr whose body bytes we read out at offset +48.
using fn_URLResponse_underlyingResponse = shared_ptr* (*)(void* this_);

// RequestContext::storeFrontIdentifier(shared_ptr<URLBag> const&) const
// - writes a std::string to the hidden first arg; the URLBag arg is
// passed as a null shared_ptr (upstream pattern).
using fn_RequestContext_storeFrontIdentifier =
    void (*)(std_string* out, void* this_, shared_ptr* url_bag);

// ---------------------------------------------------------------------------
// Phase 1.3 - SVPlaybackLeaseManager + SVFootHillSessionCtrl (FairPlay decrypt)
// ---------------------------------------------------------------------------

// SVPlaybackLeaseManager::SVPlaybackLeaseManager(
//   std::function<void(int const&)> const&,
//   std::function<void(std::shared_ptr<storeservicescore::StoreErrorCondition>
//   const&)> const&)  [upstream main.cpp uses std::function<void(void*)> for 2nd]
using fn_SVPlaybackLeaseManager_ctor = void (*)(
    void* this_,
    void* end_lease_std_function,
    void* pb_err_std_function);

using fn_SVPlaybackLeaseManager_refreshLeaseAutomatically = void (*)(void* this_,
                                                                      std::uint8_t* flag);
using fn_SVPlaybackLeaseManager_requestLease               = void (*)(void* this_,
                                                      std::uint8_t* flag);

using fn_SVFootHillSessionCtrl_instance = void* (*)();

using fn_SVFootHillSessionCtrl_getPersistentKey =
    void (*)(shared_ptr* ret,
             void*       fh,
             std_string* adam_id,
             std_string* key_uri,
             std_string* key_format,
             std_string* key_format_ver,
             std_string* server_uri,
             std_string* protocol_type,
             std_string* fps_cert);

// Third argument is the persistent-key handle (upstream passes persistK.obj).
using fn_SVFootHillSessionCtrl_decryptContext = void (*)(shared_ptr* ret,
                                                         void*       fh,
                                                         void* persistent_key_obj);

// Returns pointer to kdContext; upstream dereferences once.
using fn_SVFootHillPContext_kdContext = void** (*)(void* pctx);

using fn_fp_sample_decrypt = long (*)(void*       kd_context,
                                      std::uint32_t op,
                                      void*       in_out,
                                      void*       out_alias,
                                      std::size_t size);

using fn_SVFootHillSessionCtrl_resetAllContexts = void (*)(void* fh);

using fn_shared_ptr_SVFootHillPContext_dtor = void (*)(shared_ptr* this_);

// Mangled symbol names. These are the *strings* we feed to dlsym.
// Kept centrally so that the Loader implementation does not embed
// them inline (easier to audit and regenerate from `nm` output).
namespace mangled {

inline constexpr const char* resolv_set_nameservers_for_net =
    "_resolv_set_nameservers_for_net";

inline constexpr const char* FootHillConfig_config =
    "_ZN14FootHillConfig6configERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE";

inline constexpr const char* DeviceGUID_instance =
    "_ZN17storeservicescore10DeviceGUID8instanceEv";

inline constexpr const char* DeviceGUID_configure =
    "_ZN17storeservicescore10DeviceGUID9configureERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_RKjRKb";

inline constexpr const char* make_shared_RequestContext =
    "_ZNSt6__ndk110shared_ptrIN17storeservicescore14RequestContextEE11make_sharedIJRNS_12basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEEEES3_DpOT_";

inline constexpr const char* vtable_RequestContextConfig =
    "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore20RequestContextConfigENS_9allocatorIS2_EEEE";

inline constexpr const char* RequestContextConfig_ctor =
    "_ZN17storeservicescore20RequestContextConfigC2Ev";

inline constexpr const char* RCC_setBaseDirectoryPath =
    "_ZN17storeservicescore20RequestContextConfig20setBaseDirectoryPathERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setClientIdentifier =
    "_ZN17storeservicescore20RequestContextConfig19setClientIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setVersionIdentifier =
    "_ZN17storeservicescore20RequestContextConfig20setVersionIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setPlatformIdentifier =
    "_ZN17storeservicescore20RequestContextConfig21setPlatformIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setProductVersion =
    "_ZN17storeservicescore20RequestContextConfig17setProductVersionERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setDeviceModel =
    "_ZN17storeservicescore20RequestContextConfig14setDeviceModelERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setBuildVersion =
    "_ZN17storeservicescore20RequestContextConfig15setBuildVersionERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setLocaleIdentifier =
    "_ZN17storeservicescore20RequestContextConfig19setLocaleIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* RCC_setLanguageIdentifier =
    "_ZN17storeservicescore20RequestContextConfig21setLanguageIdentifierERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";

inline constexpr const char* RequestContextManager_configure =
    "_ZN21RequestContextManager9configureERKNSt6__ndk110shared_ptrIN17storeservicescore14RequestContextEEE";
inline constexpr const char* RequestContext_init =
    "_ZN17storeservicescore14RequestContext4initERKNSt6__ndk110shared_ptrINS_20RequestContextConfigEEE";
inline constexpr const char* RequestContext_setFairPlayDirectoryPath =
    "_ZN17storeservicescore14RequestContext24setFairPlayDirectoryPathERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";

// ---- Phase 1.1: AndroidPresentationInterface + auth flow ----

inline constexpr const char* make_shared_AndroidPresentationInterface =
    "_ZNSt6__ndk110shared_ptrIN20androidstoreservices28AndroidPresentationInterfaceEE11make_sharedIJEEES3_DpOT_";

inline constexpr const char* API_setCredentialsHandler =
    "_ZN20androidstoreservices28AndroidPresentationInterface21setCredentialsHandlerEPFvNSt6__ndk110shared_ptrIN17storeservicescore18CredentialsRequestEEENS2_INS_33AndroidCredentialsResponseHandlerEEEE";

inline constexpr const char* API_setDialogHandler =
    "_ZN20androidstoreservices28AndroidPresentationInterface16setDialogHandlerEPFvlNSt6__ndk110shared_ptrIN17storeservicescore14ProtocolDialogEEENS2_INS_36AndroidProtocolDialogResponseHandlerEEEE";

inline constexpr const char* API_handleCredentialsResponse =
    "_ZN20androidstoreservices28AndroidPresentationInterface25handleCredentialsResponseERKNSt6__ndk110shared_ptrIN17storeservicescore19CredentialsResponseEEE";
inline constexpr const char* API_handleProtocolDialogResponse =
    "_ZN20androidstoreservices28AndroidPresentationInterface28handleProtocolDialogResponseERKlRKNSt6__ndk110shared_ptrIN17storeservicescore22ProtocolDialogResponseEEE";

inline constexpr const char* vtable_ProtocolDialogResponse =
    "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore22ProtocolDialogResponseENS_9allocatorIS2_EEEE";
inline constexpr const char* ProtocolDialogResponse_ctor =
    "_ZN17storeservicescore22ProtocolDialogResponseC1Ev";
inline constexpr const char* ProtocolDialogResponse_setSelectedButton =
    "_ZN17storeservicescore22ProtocolDialogResponse17setSelectedButtonERKNSt6__ndk110shared_ptrINS_14ProtocolButtonEEE";
inline constexpr const char* ProtocolDialog_title =
    "_ZNK17storeservicescore14ProtocolDialog5titleEv";
inline constexpr const char* ProtocolDialog_message =
    "_ZNK17storeservicescore14ProtocolDialog7messageEv";
inline constexpr const char* ProtocolDialog_buttons =
    "_ZNK17storeservicescore14ProtocolDialog7buttonsEv";
inline constexpr const char* ProtocolButton_title =
    "_ZNK17storeservicescore14ProtocolButton5titleEv";

inline constexpr const char* RequestContext_setPresentationInterface =
    "_ZN17storeservicescore14RequestContext24setPresentationInterfaceERKNSt6__ndk110shared_ptrINS_21PresentationInterfaceEEE";

inline constexpr const char* CR_requiresHSA2VerificationCode =
    "_ZNK17storeservicescore18CredentialsRequest28requiresHSA2VerificationCodeEv";
inline constexpr const char* CR_title =
    "_ZNK17storeservicescore18CredentialsRequest5titleEv";
inline constexpr const char* CR_message =
    "_ZNK17storeservicescore18CredentialsRequest7messageEv";

inline constexpr const char* vtable_CredentialsResponse =
    "_ZTVNSt6__ndk120__shared_ptr_emplaceIN17storeservicescore19CredentialsResponseENS_9allocatorIS2_EEEE";
inline constexpr const char* CredentialsResponse_ctor =
    "_ZN17storeservicescore19CredentialsResponseC1Ev";
inline constexpr const char* CredentialsResponse_setUserName =
    "_ZN17storeservicescore19CredentialsResponse11setUserNameERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* CredentialsResponse_setPassword =
    "_ZN17storeservicescore19CredentialsResponse11setPasswordERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
inline constexpr const char* CredentialsResponse_setResponseType =
    "_ZN17storeservicescore19CredentialsResponse15setResponseTypeENS0_12ResponseTypeE";

inline constexpr const char* make_shared_AuthenticateFlow =
    "_ZNSt6__ndk110shared_ptrIN17storeservicescore16AuthenticateFlowEE11make_sharedIJRNS0_INS1_14RequestContextEEEEEES3_DpOT_";
inline constexpr const char* AuthenticateFlow_run =
    "_ZN17storeservicescore16AuthenticateFlow3runEv";
inline constexpr const char* AuthenticateFlow_response =
    "_ZNK17storeservicescore16AuthenticateFlow8responseEv";

inline constexpr const char* AR_responseType =
    "_ZNK17storeservicescore20AuthenticateResponse12responseTypeEv";
inline constexpr const char* AR_customerMessage =
    "_ZNK17storeservicescore20AuthenticateResponse15customerMessageEv";
inline constexpr const char* AR_error =
    "_ZNK17storeservicescore20AuthenticateResponse5errorEv";

inline constexpr const char* SEC_errorCode =
    "_ZNK17storeservicescore19StoreErrorCondition9errorCodeEv";
inline constexpr const char* SEC_what =
    "_ZNK17storeservicescore19StoreErrorCondition4whatEv";

// ---- Phase 1.1: token harvest ----

inline constexpr const char* DeviceGUID_guid =
    "_ZN17storeservicescore10DeviceGUID4guidEv";
inline constexpr const char* Data_bytes =
    "_ZNK13mediaplatform4Data5bytesEv";

inline constexpr const char* vtable_HTTPMessage =
    "_ZTVNSt6__ndk120__shared_ptr_emplaceIN13mediaplatform11HTTPMessageENS_9allocatorIS2_EEEE";
inline constexpr const char* HTTPMessage_ctor =
    "_ZN13mediaplatform11HTTPMessageC2ENSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES7_";
inline constexpr const char* HTTPMessage_setHeader =
    "_ZN13mediaplatform11HTTPMessage9setHeaderERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_";
inline constexpr const char* HTTPMessage_setBodyData =
    "_ZN13mediaplatform11HTTPMessage11setBodyDataEPcm";

inline constexpr const char* URLRequest_ctor =
    "_ZN17storeservicescore10URLRequestC2ERKNSt6__ndk110shared_ptrIN13mediaplatform11HTTPMessageEEERKNS2_INS_14RequestContextEEE";
inline constexpr const char* URLRequest_setRequestParameter =
    "_ZN17storeservicescore10URLRequest19setRequestParameterERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_";
inline constexpr const char* URLRequest_run =
    "_ZN17storeservicescore10URLRequest3runEv";
inline constexpr const char* URLRequest_error =
    "_ZNK17storeservicescore10URLRequest5errorEv";
inline constexpr const char* URLRequest_response =
    "_ZNK17storeservicescore10URLRequest8responseEv";
inline constexpr const char* URLResponse_underlyingResponse =
    "_ZNK17storeservicescore11URLResponse18underlyingResponseEv";

inline constexpr const char* RequestContext_storeFrontIdentifier =
    "_ZNK17storeservicescore14RequestContext20storeFrontIdentifierERKNSt6__ndk110shared_ptrINS_6URLBagEEE";

// ---- Phase 1.3: FairPlay decrypt / lease ----

inline constexpr const char* SVPlaybackLeaseManager_ctor =
    "_ZN22SVPlaybackLeaseManagerC2ERKNSt6__ndk18functionIFvRKiEEERKNS1_IFvRKNS0_10shared_ptrIN17storeservicescore19StoreErrorConditionEEEEEE";

inline constexpr const char* SVPlaybackLeaseManager_refreshLeaseAutomatically =
    "_ZN22SVPlaybackLeaseManager25refreshLeaseAutomaticallyERKb";

inline constexpr const char* SVPlaybackLeaseManager_requestLease =
    "_ZN22SVPlaybackLeaseManager12requestLeaseERKb";

inline constexpr const char* SVFootHillSessionCtrl_instance =
    "_ZN21SVFootHillSessionCtrl8instanceEv";

// Must match `readelf --dyn-syms -W libandroidappmusic.so` exactly (dlsym is literal).
inline constexpr const char* SVFootHillSessionCtrl_getPersistentKey =
    "_ZN21SVFootHillSessionCtrl16getPersistentKeyERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES8_S8_S8_S8_S8_S8_";

inline constexpr const char* SVFootHillSessionCtrl_decryptContext =
    "_ZN21SVFootHillSessionCtrl14decryptContextERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEERKN11SVDecryptor15SVDecryptorTypeERKb";

inline constexpr const char* SVFootHillPContext_kdContext =
    "_ZNK18SVFootHillPContext9kdContextEv";

inline constexpr const char* fp_sample_decrypt = "NfcRKVnxuKZy04KWbdFu71Ou";

inline constexpr const char* SVFootHillSessionCtrl_resetAllContexts =
    "_ZN21SVFootHillSessionCtrl16resetAllContextsEv";

inline constexpr const char* shared_ptr_SVFootHillPContext_dtor =
    "_ZNSt6__ndk110shared_ptrI18SVFootHillPContextED2Ev";

}  // namespace mangled

}  // namespace wrapper::apple::abi
