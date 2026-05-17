// wrapper-v2 daemon entry point.
//
// Phase 1.1 brings real Apple-ID / password login. The daemon starts
// in LoggedOut state, expects credentials via HTTP, and drives
// Apple's AuthenticateFlow under the hood:
//
//   GET    /health
//   GET    /me
//   POST   /login         body: { "apple_id": "...", "password": "..." }
//   POST   /login/2fa     body: { "code": "123456" }
//   DELETE /login
//
// Persistence: mount WRAPPER_BASE_DIR so Apple keeps mpl_db across
// restarts. After a prior POST /login (or first-time -L style login),
// startup may restore tokens from that session without password
// (WRAPPER_RESTORE_SESSION=1, default).
//
// Configuration is environment-only. Optional argv: --help. All knobs
// use the WRAPPER_ prefix:
//
//   WRAPPER_HOST          Bind address (default 0.0.0.0)
//   WRAPPER_PORT          Bind port    (default 80)
//   WRAPPER_BASE_DIR      Apple-lib working dir (default
//                         /data/data/com.apple.android.music/files)
//   WRAPPER_DEVICE_INFO   9-tuple device identifier
//   WRAPPER_APPLE_INIT    "0" to skip Apple lib init at startup
//                         (useful for /health-only smoke tests).
//   WRAPPER_RESTORE_SESSION  "0" skips on-disk session restore after init.
//   WRAPPER_APPLE_ID      Optional label for GET /me apple_id after restore
//                         (not sent to Apple).
//   WRAPPER_USERNAME      With WRAPPER_PASSWORD, run password sign-in at
//                         startup if not already authenticated (same as
//                         POST /login username field — Apple ID email).
//   WRAPPER_PASSWORD      App-specific password for WRAPPER_USERNAME auto-login.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <signal.h>
#include <ucontext.h>
#include <unistd.h>

#include <httplib.h>

#include "apple/auth.hpp"
#include "apple/loader.hpp"
#include "apple/runtime.hpp"
#include "server.hpp"

namespace {

constexpr const char* kDefaultHost    = "0.0.0.0";
constexpr int         kDefaultPort    = 80;
constexpr const char* kVersion        = "0.1.0-phase1.1";

std::atomic<httplib::Server*> g_server{nullptr};

// Minimal async-signal-safe crash line (no /proc parsing — avoids huge logs).
void on_crash(int sig, siginfo_t* info, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    void* fault_addr = info ? info->si_addr : nullptr;
    void* rip = nullptr;
#ifdef __x86_64__
    if (uc) rip = reinterpret_cast<void*>(uc->uc_mcontext.gregs[REG_RIP]);
#endif
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "wrapper-v2: fatal signal %d fault_addr=%p rip=%p\n",
                     sig, fault_addr, rip);
    (void)write(STDERR_FILENO, buf, n > 0 ? n : 0);
    _exit(128 + sig);
}

void on_signal(int sig) {
    auto* s = g_server.load();
    if (s != nullptr) {
        std::fprintf(stderr, "wrapper-v2: caught signal %d, stopping server\n", sig);
        s->stop();
    }
}

std::string env_or(const char* name, std::string fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    return std::string(v);
}

bool env_bool(const char* name, bool fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    if (std::strcmp(v, "0") == 0 || std::strcmp(v, "false") == 0
        || std::strcmp(v, "no") == 0) {
        return false;
    }
    return true;
}

bool consume_argv(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view x = argv[i];
        if (x == "--help" || x == "-h") {
            std::printf(
                "wrapper-v2 daemon (%s)\n"
                "Usage: %s\n"
                "\n"
                "Bind address and port are set with WRAPPER_HOST / WRAPPER_PORT\n"
                "(defaults %s / %d).\n"
                "\n"
                "Environment:\n"
                "  WRAPPER_HOST             bind address\n"
                "  WRAPPER_PORT             bind port\n"
                "  WRAPPER_BASE_DIR         Apple-lib working dir\n"
                "  WRAPPER_DEVICE_INFO      9-tuple device identifier\n"
                "  WRAPPER_APPLE_INIT       set to 0 to skip Apple lib init\n"
                "  WRAPPER_RESTORE_SESSION  set to 0 to skip session restore\n"
                "  WRAPPER_APPLE_ID         optional /me label after restore\n"
                "  WRAPPER_USERNAME         Apple ID for env auto-login (+WRAPPER_PASSWORD)\n"
                "  WRAPPER_PASSWORD         app password for env auto-login\n",
                kVersion, argv[0], kDefaultHost, kDefaultPort);
            return false;
        }
        std::fprintf(stderr,
                     "wrapper-v2: unexpected argument '%s' "
                     "(use environment variables; try '%s --help')\n",
                     argv[i], argv[0]);
        std::exit(2);
    }
    return true;
}

void maybe_auto_login_from_env(wrapper::apple::Account& account,
                               const wrapper::apple::Loader& loader,
                               const wrapper::apple::Runtime& runtime,
                               bool apple_init_enabled) {
    if (!apple_init_enabled || !runtime.initialized()) {
        return;
    }
    std::string user = env_or("WRAPPER_USERNAME", "");
    const char* pw   = std::getenv("WRAPPER_PASSWORD");
    if (user.empty() || pw == nullptr || *pw == '\0') {
        if ((pw != nullptr && *pw != '\0') && user.empty()) {
            std::fprintf(stderr,
                         "wrapper-v2: WRAPPER_PASSWORD is set but WRAPPER_USERNAME "
                         "is missing; skipping env auto-login\n");
        }
        return;
    }
    if (account.state() == wrapper::apple::LoginState::Authenticated) {
        return;
    }
    std::string password(pw);
    if (!account.start_login(loader, runtime, std::move(user), std::move(password))) {
        std::fprintf(stderr,
                     "wrapper-v2: env auto-login could not start (state=%s)\n",
                     wrapper::apple::to_string(account.state()));
        return;
    }
    auto st = account.wait_for_settled_state(std::chrono::milliseconds(30000));
    if (st == wrapper::apple::LoginState::Authenticated) {
        std::fprintf(stderr, "wrapper-v2: env auto-login succeeded\n");
    } else if (st == wrapper::apple::LoginState::Awaiting2FA) {
        std::fprintf(stderr,
                     "wrapper-v2: env auto-login needs 2FA; POST /login/2fa with "
                     "{\"code\":\"...\"}\n");
    } else if (st == wrapper::apple::LoginState::Failed) {
        auto snap = account.public_snapshot();
        std::fprintf(stderr, "wrapper-v2: env auto-login failed: %s (code %d)\n",
                     snap.last_error.c_str(), snap.last_error_code);
    } else {
        std::fprintf(stderr,
                     "wrapper-v2: env auto-login still in progress or timed out "
                     "(state=%s)\n",
                     wrapper::apple::to_string(st));
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (!consume_argv(argc, argv)) {
        return 0;
    }

    std::string listen_host = env_or("WRAPPER_HOST", kDefaultHost);
    int listen_port =
        std::atoi(env_or("WRAPPER_PORT", std::to_string(kDefaultPort)).c_str());
    if (listen_port <= 0) {
        listen_port = kDefaultPort;
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Install crash handler for a single stderr line with fault_addr + rip.
    {
        struct sigaction sa{};
        sa.sa_sigaction = on_crash;
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sigaction(SIGSEGV, &sa, nullptr);
        sigaction(SIGABRT, &sa, nullptr);
    }

    wrapper::ServerInfo info;
    info.version = kVersion;
    info.apple_init_enabled = env_bool("WRAPPER_APPLE_INIT", true);

    auto& account = wrapper::apple::Account::instance();
    wrapper::apple::Loader   loader;
    auto& runtime = wrapper::apple::Runtime::instance();

    std::string libs_dir = env_or("WRAPPER_LIBS_DIR", "/system/lib64");

    if (info.apple_init_enabled) {
        if (loader.open(libs_dir)) {
            wrapper::apple::RuntimeConfig rcfg;
            rcfg.base_dir    = env_or("WRAPPER_BASE_DIR",    rcfg.base_dir);
            rcfg.device_info = env_or("WRAPPER_DEVICE_INFO", rcfg.device_info);
            if (runtime.initialize(loader, rcfg)) {
                if (env_bool("WRAPPER_RESTORE_SESSION", true)) {
                    if (account.try_restore_cached_session(loader, runtime)) {
                        std::fprintf(stderr,
                                     "wrapper-v2: session restored from Apple cache "
                                     "(GET /me without POST /login)\n");
                    }
                }
            } else {
                std::fprintf(stderr,
                             "wrapper-v2: Apple runtime init failed after dlopen "
                             "succeeded; the loaded libs may be from an unexpected "
                             "APK version. Continuing in stub mode.\n");
            }
        } else {
            std::fprintf(stderr,
                         "wrapper-v2: Apple lib load failed (libs_dir=%s, error=%s). "
                         "Continuing in stub mode; HTTP API works but Apple-backed "
                         "endpoints will return 503.\n",
                         libs_dir.c_str(), loader.last_error().c_str());
        }
    } else {
        std::fprintf(stderr,
                     "wrapper-v2: WRAPPER_APPLE_INIT=0, skipping Apple lib init "
                     "(stub mode: /health only; POST /login returns 503)\n");
    }

    maybe_auto_login_from_env(account, loader, runtime, info.apple_init_enabled);

    httplib::Server svr;
    g_server.store(&svr);

    wrapper::Server server(svr, runtime, loader, account, info);
    server.mount();

    std::fprintf(stderr, "wrapper-v2: %s listening on %s:%d\n", kVersion,
                 listen_host.c_str(), listen_port);

    if (!svr.listen(listen_host, listen_port)) {
        std::fprintf(stderr, "wrapper-v2: bind failed on %s:%d\n",
                     listen_host.c_str(), listen_port);
        return 1;
    }
    return 0;
}
