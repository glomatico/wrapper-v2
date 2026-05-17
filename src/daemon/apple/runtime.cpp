// Port of upstream main.c::init() + init_ctx() to C++.
//
// Behavioral diffs vs. upstream:
//   - We install a presentation interface with credential + dialog
//     handlers. The dialog handler auto-selects "Use Existing Apple ID"
//     when the title is "Sign In" (same as upstream), and always calls
//     handleProtocolDialogResponse so AuthenticateFlow does not stall.
//   - We install SVPlaybackLeaseManager + requestLease before exposing
//     POST /decrypt, matching upstream main() after init_ctx.
//   - Errors during init log to stderr; credential + ProtocolDialog
//     callbacks log a single line each to stderr (no secrets).
//
// All Apple calls are unchecked: there are no error returns to
// validate. If an Apple call crashes mid-init, we want the process to
// die rather than mask the failure with a half-initialized
// RequestContext.

#include "apple/runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <sstream>
#include <string>

#include "apple/auth.hpp"

namespace wrapper::apple {

namespace {

constexpr int kDeviceInfoPartCount = 9;

void end_lease_cb(const int& code) {
    std::fprintf(stderr, "runtime: playback lease ended (code=%d)\n", code);
}

void pb_err_cb(void* /*unused*/) {
    std::fprintf(stderr, "runtime: playback StoreErrorCondition callback\n");
}

std::function<void(const int&)> g_end_lease_fn(end_lease_cb);
std::function<void(void*)>      g_pb_err_fn(pb_err_cb);

// Apple's CredentialsResponse setters can retain references to string storage
// past the callback return. Upstream passes global char buffers; keep equivalent
// process-lifetime storage here rather than local std::string::c_str() pointers.
std::string g_credential_username;
std::string g_credential_password;

// Split "a/b/c/d/.../i" into 9 string parts. Returns empty vector
// if the count doesn't match (the runtime considers that a hard
// error: Apple's libs assume all 9 fields are present).
std::vector<std::string> split_device_info(const std::string& s) {
    std::vector<std::string> out;
    out.reserve(kDeviceInfoPartCount);

    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, '/')) {
        out.push_back(std::move(part));
    }
    if (static_cast<int>(out.size()) != kDeviceInfoPartCount) {
        out.clear();
    }
    return out;
}

std::string read_apple_string(const abi::std_string* s) {
    if (s == nullptr) return {};
    const bool is_long = (s->_long.cap & 1u) != 0;
    if (is_long) {
        if (s->_long.data == nullptr || s->_long.size == 0) return {};
        return std::string(s->_long.data, s->_long.size);
    }
    const std::size_t len = s->_short.mark >> 1;
    return std::string(s->_short.str, len);
}

// Single-line stderr (no raw newlines / control chars).
std::string sanitize_log_chunk(std::string x, std::size_t max_len) {
    for (char& c : x) {
        if (static_cast<unsigned char>(c) < 0x20) c = ' ';
    }
    if (x.size() > max_len) {
        x.resize(max_len);
        x += "...";
    }
    return x;
}

// ----------------------------------------------------------------------
// Callbacks registered on AndroidPresentationInterface.
//
// Apple's setDialogHandler / setCredentialsHandler store a function pointer
// typed as taking std::shared_ptr<T> by value. On x86_64 Itanium ABI,
// std::shared_ptr has a non-trivial copy ctor/dtor, so it is passed
// *indirectly*: the caller puts a pointer to a temporary in %rdi/%rsi.
// Declaring the callbacks with explicit shared_ptr* parameters matches that
// calling convention exactly — same as the upstream C wrapper (main.c).
// Do NOT use by-value abi::shared_ptr here: abi::shared_ptr is trivially
// copyable so the compiler would expect its fields in registers, creating
// a fatal ABI mismatch.
// ----------------------------------------------------------------------

extern "C" void wrapper_dialog_handler(long dialog_id,
                                       abi::shared_ptr* dialog,
                                       abi::shared_ptr* /* response_handler */) {
    Runtime& rt = Runtime::instance();
    const Loader* loader = rt.loader();
    if (loader == nullptr || !loader->ok()) return;
    if (dialog == nullptr || dialog->obj == nullptr) return;

    const Symbols& sy = loader->sym();
    std::string title   = read_apple_string(sy.ProtocolDialog_title(dialog->obj));
    abi::shared_ptr apInf = rt.presentation_interface_copy();
    if (apInf.obj == nullptr) return;

    // Allocate with ::operator new so libc++'s shared_ptr control-block
    // teardown matches the allocator (malloc + operator delete is UB).
    auto* const emplace = static_cast<std::uint8_t*>(::operator new(72));
    std::memset(emplace + 8, 0, 16);
    *reinterpret_cast<void**>(emplace) = sy.vtable_ProtocolDialogResponse + 2;
    abi::shared_ptr diag_resp;
    diag_resp.obj      = emplace + 24;
    diag_resp.ctrl_blk = emplace;
    sy.ProtocolDialogResponse_ctor(diag_resp.obj);

    std::vector<std::string> button_labels;
    bool auto_picked = false;
    abi::std_vector* buttons = sy.ProtocolDialog_buttons(dialog->obj);
    if (buttons != nullptr && buttons->begin != nullptr && buttons->end != nullptr) {
        auto* b_begin = static_cast<abi::shared_ptr*>(buttons->begin);
        auto* b_end   = static_cast<abi::shared_ptr*>(buttons->end);
        for (auto* b = b_begin; b != b_end; ++b) {
            if (b->obj == nullptr) continue;
            std::string bt = read_apple_string(sy.ProtocolButton_title(b->obj));
            const bool is_existing = (bt == "Use Existing Apple ID");
            button_labels.push_back(sanitize_log_chunk(std::move(bt), 80));
            if (!auto_picked && title == "Sign In" && is_existing) {
                sy.ProtocolDialogResponse_setSelectedButton(diag_resp.obj, b);
                auto_picked = true;
            }
        }
    }

    {
        std::string joined;
        for (std::size_t i = 0; i < button_labels.size(); ++i) {
            if (i != 0) joined += ", ";
            joined += button_labels[i];
        }
        joined = sanitize_log_chunk(std::move(joined), 400);
        std::string title_log = sanitize_log_chunk(std::move(title), 200);
        std::fprintf(stderr,
                     "runtime: ProtocolDialog id=%ld title=\"%s\" buttons=[%s] "
                     "picked_existing_apple_id=%s\n",
                     static_cast<long>(dialog_id), title_log.c_str(), joined.c_str(),
                     auto_picked ? "yes" : "no");
    }

    long id_arg = dialog_id;
    sy.API_handleProtocolDialogResponse(apInf.obj, &id_arg, &diag_resp);
    // Ownership of emplace follows Apple's shared_ptr refcount; do not free here.
}

extern "C" void wrapper_credential_handler(abi::shared_ptr* cred_request,
                                           abi::shared_ptr* /* cred_response_handler */) {
    Runtime& rt = Runtime::instance();
    const Loader* loader = rt.loader();
    if (loader == nullptr || !loader->ok()) return;
    const Symbols& s = loader->sym();
    abi::shared_ptr apInf = rt.presentation_interface_copy();
    if (apInf.obj == nullptr) return;

    const bool need_2fa =
        (cred_request != nullptr && cred_request->obj != nullptr)
        ? (s.CR_requiresHSA2VerificationCode(cred_request->obj) != 0)
        : false;

    std::fprintf(stderr,
                 "runtime: CredentialsRequest HSA2=%s\n",
                 need_2fa ? "yes" : "no");

    std::string username, password;
    Account::instance().fetch_credentials_blocking(&username, &password, need_2fa);
    g_credential_username = std::move(username);
    g_credential_password = std::move(password);

    auto* const emplace = static_cast<std::uint8_t*>(::operator new(80));
    std::memset(emplace + 8, 0, 16);
    *reinterpret_cast<void**>(emplace) = s.vtable_CredentialsResponse + 2;
    abi::shared_ptr cred_resp;
    cred_resp.obj      = emplace + 24;
    cred_resp.ctrl_blk = emplace;

    s.CredentialsResponse_ctor(cred_resp.obj);
    auto user_view = abi::make_string_view(g_credential_username.c_str());
    s.CredentialsResponse_setUserName(cred_resp.obj, &user_view);
    auto pass_view = abi::make_string_view(g_credential_password.c_str());
    s.CredentialsResponse_setPassword(cred_resp.obj, &pass_view);
    s.CredentialsResponse_setResponseType(cred_resp.obj, /*kSubmit=*/2);

    s.API_handleCredentialsResponse(apInf.obj, &cred_resp);
    // Ownership of emplace follows Apple's shared_ptr refcount; do not free here.
}

}  // namespace

Runtime& Runtime::instance() {
    static Runtime r;
    return r;
}

bool Runtime::init_playback_session(const Symbols& s) {
    std::memset(lease_mgr_, 0, sizeof(lease_mgr_));
    s.SVPlaybackLeaseManager_ctor(lease_mgr_, &g_end_lease_fn, &g_pb_err_fn);
    std::uint8_t autom = 1;
    s.SVPlaybackLeaseManager_refreshLeaseAutomatically(lease_mgr_, &autom);
    s.SVPlaybackLeaseManager_requestLease(lease_mgr_, &autom);
    foothill_ = s.SVFootHillSessionCtrl_instance();
    if (foothill_ == nullptr) {
        std::fprintf(stderr,
                     "runtime: SVFootHillSessionCtrl::instance returned null\n");
        return false;
    }
    return true;
}

void Runtime::refresh_playback_lease() {
    if (loader_ == nullptr || !loader_->ok() || !playback_ready_) {
        return;
    }
    const Symbols& s     = loader_->sym();
    std::uint8_t   autom = 1;
    std::lock_guard<std::mutex> lock(playback_mutex_);
    s.SVPlaybackLeaseManager_requestLease(lease_mgr_, &autom);
}

bool Runtime::initialize(const Loader& loader, const RuntimeConfig& cfg) {
    std::lock_guard<std::mutex> g(mu_);
    if (initialized_.load(std::memory_order_relaxed)) return true;

    if (!loader.ok()) {
        std::fprintf(stderr,
                     "runtime: cannot initialize, loader is not ok (last_error=%s)\n",
                     loader.last_error().c_str());
        return false;
    }

    auto parts = split_device_info(cfg.device_info);
    if (parts.empty()) {
        std::fprintf(stderr,
                     "runtime: WRAPPER_DEVICE_INFO must have %d slash-separated "
                     "parts, got %zu (raw=%.200s)\n",
                     kDeviceInfoPartCount,
                     std::count(cfg.device_info.begin(), cfg.device_info.end(), '/') + 1,
                     cfg.device_info.c_str());
        return false;
    }

    const Symbols& s = loader.sym();
    if (!init_dns_and_foothill(s, parts)) return false;
    if (!init_request_context(s, cfg, parts)) return false;
    if (!init_presentation_interface(s)) return false;

    base_dir_    = cfg.base_dir;
    device_info_ = cfg.device_info;
    loader_      = &loader;

    if (!loader.fairplay_decrypt_available()) {
        playback_ready_ = false;
        std::fprintf(stderr,
                     "runtime: FairPlay decrypt chain not loaded; POST /decrypt unavailable\n");
    } else {
        playback_ready_ = init_playback_session(s);
        if (!playback_ready_) {
            std::fprintf(stderr,
                         "runtime: warning: FairPlay playback init failed; "
                         "POST /decrypt unavailable\n");
        }
    }

    initialized_.store(true, std::memory_order_release);

    return true;
}

bool Runtime::init_dns_and_foothill(const Symbols& s,
                                    const std::vector<std::string>& parts) {
    // Bionic's resolver inside the chroot has no /etc/resolv.conf to
    // read. Set Alibaba's public DNS so the bundled libcurl can
    // reach api.itunes.apple.com etc. Upstream uses the same hosts.
    static const char* resolvers[2] = {"223.5.5.5", "223.6.6.6"};
    s.resolv_set_nameservers_for_net(0, resolvers, 2, ".");

    // FootHillConfig::config(android_id). Android ID is part 8 of
    // the device-info tuple.
    auto android_id = abi::make_string_view(parts[8].c_str());
    s.FootHillConfig_config(&android_id);

    // DeviceGUID::instance() -> shared_ptr<DeviceGUID>
    s.DeviceGUID_instance(&device_guid_);
    if (device_guid_.obj == nullptr) {
        std::fprintf(stderr, "runtime: DeviceGUID::instance returned null\n");
        return false;
    }

    // DeviceGUID::configure(android_id, "", 29, true). The first arg
    // is the hidden return slot for the (88-byte) Configuration<>
    // value Apple discards.
    static std::uint8_t cfg_ret[88];
    static const unsigned int cfg_kind = 29;
    static const std::uint8_t cfg_flag = 1;
    auto empty = abi::make_string_view("");
    s.DeviceGUID_configure(&cfg_ret, device_guid_.obj,
                           &android_id, &empty, &cfg_kind, &cfg_flag);

    return true;
}

bool Runtime::init_request_context(const Symbols& s,
                                   const RuntimeConfig& cfg,
                                   const std::vector<std::string>& parts) {
    // make_shared<RequestContext>(mpl_db_path).
    std::string mpl_db = cfg.base_dir + "/mpl_db";
    auto mpl_db_view = abi::make_string_view(mpl_db.c_str());
    s.make_shared_RequestContext(&request_ctx_, &mpl_db_view);

    if (request_ctx_.obj == nullptr) {
        std::fprintf(stderr, "runtime: make_shared<RequestContext> returned null\n");
        return false;
    }

    // RequestContextConfig: emplaced in a buffer with a hand-rolled vtable
    // pointer for the shared_ptr control block. Pattern from upstream
    // init_ctx(), with extra slack so Release builds do not stomp adjacent
    // storage if Apple's object is larger than the historical constant.
    alignas(16) static std::uint8_t rcc_buf[2048];
    *reinterpret_cast<void**>(rcc_buf) =
        s.vtable_RequestContextConfig + 2;  // skip past vtable header (type_info + dtor slots)

    abi::shared_ptr rcc;
    rcc.obj = rcc_buf + 32;
    rcc.ctrl_blk = rcc_buf;

    s.RequestContextConfig_ctor(rcc.obj);

    // Setters: order matches upstream init_ctx().
    s.RCC_setBaseDirectoryPath(rcc.obj, &mpl_db_view);

    auto s0 = abi::make_string_view(parts[0].c_str());
    s.RCC_setClientIdentifier(rcc.obj, &s0);

    auto s1 = abi::make_string_view(parts[1].c_str());
    s.RCC_setVersionIdentifier(rcc.obj, &s1);

    auto s2 = abi::make_string_view(parts[2].c_str());
    s.RCC_setPlatformIdentifier(rcc.obj, &s2);

    auto s3 = abi::make_string_view(parts[3].c_str());
    s.RCC_setProductVersion(rcc.obj, &s3);

    auto s4 = abi::make_string_view(parts[4].c_str());
    s.RCC_setDeviceModel(rcc.obj, &s4);

    auto s5 = abi::make_string_view(parts[5].c_str());
    s.RCC_setBuildVersion(rcc.obj, &s5);

    auto s6 = abi::make_string_view(parts[6].c_str());
    s.RCC_setLocaleIdentifier(rcc.obj, &s6);

    auto s7 = abi::make_string_view(parts[7].c_str());
    s.RCC_setLanguageIdentifier(rcc.obj, &s7);

    // Wire the RequestContext into the singleton manager.
    s.RequestContextManager_configure(&request_ctx_);

    // RequestContext::init(rcc). Upstream uses an 88-byte hidden
    // return slot - match it.
    static std::uint8_t rci_ret[88];
    s.RequestContext_init(&rci_ret, request_ctx_.obj, &rcc);

    auto fp_dir = abi::make_string_view(cfg.base_dir.c_str());
    s.RequestContext_setFairPlayDirectoryPath(request_ctx_.obj, &fp_dir);

    return true;
}

bool Runtime::init_presentation_interface(const Symbols& s) {
    // make_shared<AndroidPresentationInterface>()
    s.make_shared_AndroidPresentationInterface(&presentation_interface_);
    if (presentation_interface_.obj == nullptr) {
        std::fprintf(stderr,
                     "runtime: make_shared<AndroidPresentationInterface> returned null\n");
        return false;
    }

    s.API_setDialogHandler(presentation_interface_.obj, &wrapper_dialog_handler);
    s.API_setCredentialsHandler(presentation_interface_.obj, &wrapper_credential_handler);

    // Wire the apInf into the RequestContext so AuthenticateFlow can
    // surface credential prompts back to us.
    s.RequestContext_setPresentationInterface(request_ctx_.obj, &presentation_interface_);
    return true;
}

std::string Runtime::base_dir() const {
    if (!initialized_.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> g(mu_);
    return base_dir_;
}

std::string Runtime::device_info() const {
    if (!initialized_.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> g(mu_);
    return device_info_;
}

abi::shared_ptr Runtime::request_ctx_copy() const {
    if (!initialized_.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> g(mu_);
    return request_ctx_;
}

abi::shared_ptr Runtime::device_guid_copy() const {
    if (!initialized_.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> g(mu_);
    return device_guid_;
}

abi::shared_ptr Runtime::presentation_interface_copy() const {
    if (!initialized_.load(std::memory_order_acquire)) return {};
    std::lock_guard<std::mutex> g(mu_);
    return presentation_interface_;
}

}  // namespace wrapper::apple
