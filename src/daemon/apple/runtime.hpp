// Apple Music native runtime initialization.
//
// `Runtime::initialize()` runs the equivalent of upstream main.c's
// `init()` and `init_ctx()` in one go, then wires SVPlaybackLeaseManager
// + `SVFootHillSessionCtrl::instance()` for FairPlay decrypt. After it
// returns true, the process holds a configured `RequestContext` shared_ptr
// that downstream Apple-API calls (storefront lookup, m3u8, decrypt) need.
//
// The runtime is process-wide and single-instance: Apple's libs use process-global
// state and are not safe to re-initialize.

#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "apple/abi.hpp"
#include "apple/loader.hpp"

namespace wrapper::apple {

// Configuration sourced from environment variables. See
// .env.example for defaults.
struct RuntimeConfig {
    // Filesystem path Apple's libs will use for FairPlay key cache,
    // mpl_db, etc. Mapped to upstream's --base-dir / -B flag.
    std::string base_dir = "/data/data/com.apple.android.music/files";

    // Slash-separated 9-tuple identifying the fake "Apple Music
    // Android client" we present to Apple's servers. Order:
    //   ClientIdentifier / VersionIdentifier / PlatformIdentifier /
    //   ProductVersion   / DeviceModel       / BuildVersion       /
    //   LocaleIdentifier / LanguageIdentifier / AndroidID
    std::string device_info =
        "Music/4.9/Android/10/Samsung S9/7663313/en-US/en-US/dc28071e981c439e";
};

// Singleton: Apple's libs share process-global state, so only one
// Runtime instance per process is ever valid.
class Runtime {
public:
    static Runtime& instance();

    // Idempotent: subsequent calls after the first successful one
    // are no-ops returning true. On failure the runtime stays
    // uninitialized and subsequent calls will retry. Errors are
    // logged to stderr with a `runtime: ` prefix. The Loader must
    // outlive the runtime - we cache a pointer for credential
    // trampoline access.
    bool initialize(const Loader& loader, const RuntimeConfig& cfg);

    bool initialized() const { return initialized_.load(std::memory_order_acquire); }

    // Snapshot of the configuration that was used for initialize().
    // Empty string if not initialized yet.
    std::string base_dir() const;
    std::string device_info() const;

    // Read-only access used by the auth worker thread.
    // Both methods return zero-initialized values if not initialized.
    abi::shared_ptr request_ctx_copy() const;
    abi::shared_ptr device_guid_copy() const;
    abi::shared_ptr presentation_interface_copy() const;

    // Loader pointer captured at initialize() time. Used by the C-style
    // credential handler trampoline which can't carry a user-data
    // pointer through Apple's callback ABI.
    const Loader* loader() const { return loader_; }

    // FairPlay sample decrypt (POST /decrypt). Requires successful
    // SVPlaybackLeaseManager + SVFootHillSessionCtrl init at startup.
    bool playback_ready() const { return playback_ready_; }
    void* foothill_session() const { return foothill_; }
    std::mutex& playback_mutex() { return playback_mutex_; }
    void refresh_playback_lease();

private:
    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool init_dns_and_foothill(const Symbols& s,
                               const std::vector<std::string>& parts);
    bool init_request_context(const Symbols& s,
                              const RuntimeConfig& cfg,
                              const std::vector<std::string>& parts);
    bool init_presentation_interface(const Symbols& s);
    bool init_playback_session(const Symbols& s);

    mutable std::mutex mu_;
    std::atomic<bool> initialized_{false};
    const Loader* loader_ = nullptr;
    abi::shared_ptr request_ctx_{};
    abi::shared_ptr device_guid_{};
    abi::shared_ptr presentation_interface_{};
    std::string base_dir_;
    std::string device_info_;

    std::mutex         playback_mutex_;
    // Placement buffer for SVPlaybackLeaseManager (ctor + instance methods).
    // Must cover the real object size; 16 was far too small and UB on arm64.
    alignas(16) unsigned char lease_mgr_[4096]{};
    void*    foothill_         = nullptr;
    bool     playback_ready_  = false;
};

}  // namespace wrapper::apple
