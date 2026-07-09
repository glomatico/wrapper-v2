#include "ipc.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "apple/decrypt.hpp"
#include "apple/playback.hpp"

namespace wrapper {

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr std::uint32_t kMagic = 0x57563249u;  // WV2I
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kKindRequest = 1;
constexpr std::uint16_t kKindResponse = 2;
constexpr std::uint16_t kFlagBinary = 1;
constexpr std::size_t kMaxPayload = 256u * 1024u * 1024u;

enum Opcode : std::uint16_t {
    OpHealth = 1,
    OpMe = 2,
    OpLogin = 3,
    OpLogin2fa = 4,
    OpLogout = 5,
    OpPlayback = 6,
    OpDecryptBatch = 7,
    OpShutdown = 8,
};

constexpr auto kLoginTimeout = 30s;
constexpr auto kLogin2faTimeout = 60s;

struct Frame {
    std::uint16_t kind = 0;
    std::uint32_t request_id = 0;
    std::uint16_t opcode = 0;
    std::uint16_t flags = 0;
    std::vector<std::uint8_t> payload;
};

std::uint16_t read_u16_be(const std::uint8_t* p) {
    return (static_cast<std::uint16_t>(p[0]) << 8)
         |  static_cast<std::uint16_t>(p[1]);
}

std::uint32_t read_u32_be(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24)
         | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)
         |  static_cast<std::uint32_t>(p[3]);
}

void append_u16_be(std::vector<std::uint8_t>* out, std::uint16_t v) {
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>(v & 0xff));
}

void append_u32_be(std::vector<std::uint8_t>* out, std::uint32_t v) {
    out->push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out->push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out->push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out->push_back(static_cast<std::uint8_t>(v & 0xff));
}

bool read_full(int fd, void* buf, std::size_t len) {
    auto* p = static_cast<std::uint8_t*>(buf);
    std::size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_full(int fd, const void* buf, std::size_t len) {
    const auto* p = static_cast<const std::uint8_t*>(buf);
    std::size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_frame(Frame* f) {
    std::uint8_t h[20];
    if (!read_full(STDIN_FILENO, h, sizeof(h))) return false;
    if (read_u32_be(h) != kMagic || read_u16_be(h + 4) != kVersion) {
        std::fprintf(stderr, "ipc: bad frame header\n");
        return false;
    }
    f->kind = read_u16_be(h + 6);
    f->request_id = read_u32_be(h + 8);
    f->opcode = read_u16_be(h + 12);
    f->flags = read_u16_be(h + 14);
    const std::uint32_t payload_len = read_u32_be(h + 16);
    if (payload_len > kMaxPayload) {
        std::fprintf(stderr, "ipc: payload too large: %u\n", payload_len);
        return false;
    }
    f->payload.assign(payload_len, 0);
    return payload_len == 0 || read_full(STDIN_FILENO, f->payload.data(), payload_len);
}

bool write_frame(const Frame& f) {
    std::vector<std::uint8_t> h;
    h.reserve(20);
    append_u32_be(&h, kMagic);
    append_u16_be(&h, kVersion);
    append_u16_be(&h, f.kind);
    append_u32_be(&h, f.request_id);
    append_u16_be(&h, f.opcode);
    append_u16_be(&h, f.flags);
    append_u32_be(&h, static_cast<std::uint32_t>(f.payload.size()));
    return write_full(STDOUT_FILENO, h.data(), h.size())
        && (f.payload.empty() || write_full(STDOUT_FILENO, f.payload.data(), f.payload.size()));
}

std::string iso8601_utc(std::chrono::system_clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) return {};
    auto t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::tm tm_utc{};
    gmtime_r(&t, &tm_utc);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

json snapshot_to_json(const apple::AccountSnapshot& snap) {
    json out;
    out["state"] = apple::to_string(snap.state);
    if (!snap.apple_id.empty()) {
        out["apple_id"] = snap.apple_id;
        out["username"] = snap.apple_id;
    }
    if (snap.state == apple::LoginState::Authenticated) {
        if (!snap.storefront.empty())       out["storefront"]       = snap.storefront;
        if (!snap.dsid.empty())             out["dsid"]             = snap.dsid;
        if (!snap.music_user_token.empty()) out["music_user_token"] = snap.music_user_token;
        if (!snap.dev_token.empty())        out["dev_token"]        = snap.dev_token;
        out["logged_in_at"] = iso8601_utc(snap.logged_in_at);
    }
    if (snap.state == apple::LoginState::Failed) {
        if (!snap.last_error.empty()) out["error"] = snap.last_error;
        if (snap.last_error_code != 0) out["error_code"] = snap.last_error_code;
    }
    return out;
}

json runtime_to_json(const apple::Loader& loader,
                     const apple::Runtime& rt,
                     const ServerInfo& info) {
    json runtime = {
        {"apple_init_enabled", info.apple_init_enabled},
        {"loader_ok", loader.ok()},
        {"initialized", rt.initialized()},
        {"playback_ready", rt.playback_ready()},
    };
    if (!loader.ok() && !loader.last_error().empty()) {
        runtime["loader_error"] = loader.last_error();
    }
    if (rt.initialized()) {
        runtime["base_dir"] = rt.base_dir();
        runtime["device_info"] = rt.device_info();
    }
    return runtime;
}

bool restore_session_enabled() {
    const char* v = std::getenv("WRAPPER_RESTORE_SESSION");
    if (v == nullptr || *v == '\0') return true;
    return std::strcmp(v, "0") != 0
        && std::strcmp(v, "false") != 0
        && std::strcmp(v, "no") != 0;
}

bool authenticated_or_restored(apple::Account& account,
                               const apple::Loader& loader,
                               const apple::Runtime& rt) {
    if (account.state() == apple::LoginState::Authenticated) return true;
    if (!restore_session_enabled()) return false;
    return account.try_restore_cached_session(loader, rt)
        || account.state() == apple::LoginState::Authenticated;
}

int http_status_for(apple::LoginState s) {
    switch (s) {
        case apple::LoginState::Authenticated: return 200;
        case apple::LoginState::Awaiting2FA:   return 202;
        case apple::LoginState::Failed:        return 401;
        case apple::LoginState::InProgress:    return 504;
        case apple::LoginState::LoggedOut:     return 400;
    }
    return 500;
}

Frame json_response(const Frame& req,
                    int status,
                    json body,
                    bool retryable = false,
                    bool restart_worker = false) {
    json envelope = {
        {"ok", status >= 200 && status < 300},
        {"http_status", status},
        {"content_type", "application/json"},
        {"body", std::move(body)},
        {"retryable", retryable},
        {"restart_worker", restart_worker},
    };
    const std::string s = envelope.dump();
    Frame out;
    out.kind = kKindResponse;
    out.request_id = req.request_id;
    out.opcode = req.opcode;
    out.payload.assign(s.begin(), s.end());
    return out;
}

Frame binary_response(const Frame& req, std::vector<std::uint8_t> body) {
    Frame out;
    out.kind = kKindResponse;
    out.request_id = req.request_id;
    out.opcode = req.opcode;
    out.flags = kFlagBinary;
    out.payload = std::move(body);
    return out;
}

json parse_payload_json(const Frame& req) {
    if (req.payload.empty()) return json::object();
    return json::parse(req.payload.begin(), req.payload.end());
}

Frame handle_health(const Frame& req,
                    const apple::Loader& loader,
                    const apple::Runtime& rt,
                    const ServerInfo& info) {
    return json_response(req, 200, json{
        {"status", "ok"},
        {"version", info.version},
        {"runtime", runtime_to_json(loader, rt, info)},
    });
}

Frame handle_me(const Frame& req,
                apple::Account& account,
                const apple::Loader& loader,
                const apple::Runtime& rt,
                const ServerInfo& info) {
    if (rt.initialized() && restore_session_enabled()) {
        (void)authenticated_or_restored(account, loader, rt);
    }
    return json_response(req, 200, json{
        {"version", info.version},
        {"runtime", runtime_to_json(loader, rt, info)},
        {"auth", snapshot_to_json(account.public_snapshot())},
    });
}

Frame handle_login(const Frame& req,
                   apple::Account& account,
                   const apple::Loader& loader,
                   const apple::Runtime& rt) {
    if (!rt.initialized()) {
        return json_response(req, 503, json{
            {"error", "runtime_not_initialized"},
            {"detail", "Apple lib init has not completed; check /health"},
        });
    }
    json body = parse_payload_json(req);
    if (!body.is_object() || !body.contains("password") || !body["password"].is_string()) {
        return json_response(req, 400, json{
            {"error", "missing_field"},
            {"detail", "expected JSON object with string 'password' and 'username' or 'apple_id'"},
        });
    }
    std::string login_name;
    const bool has_apple = body.contains("apple_id") && body["apple_id"].is_string();
    const bool has_user = body.contains("username") && body["username"].is_string();
    if (has_apple && has_user) {
        std::string a = body["apple_id"].get<std::string>();
        std::string u = body["username"].get<std::string>();
        if (a != u) {
            return json_response(req, 400, json{
                {"error", "conflicting_identifiers"},
                {"detail", "'username' and 'apple_id' must match if both are sent"},
            });
        }
        login_name = std::move(a);
    } else if (has_apple) {
        login_name = body["apple_id"].get<std::string>();
    } else if (has_user) {
        login_name = body["username"].get<std::string>();
    } else {
        return json_response(req, 400, json{
            {"error", "missing_field"},
            {"detail", "expected 'username' or 'apple_id' string"},
        });
    }
    std::string password = body["password"].get<std::string>();
    if (login_name.empty() || password.empty()) {
        return json_response(req, 400, json{
            {"error", "empty_field"},
            {"detail", "username/apple_id and password must be non-empty"},
        });
    }
    if (!account.start_login(loader, rt, std::move(login_name), std::move(password))) {
        return json_response(req, 409, json{
            {"error", "already_in_progress"},
            {"detail", "a login flow is already running; DELETE /login to abort"},
        });
    }
    auto state = account.wait_for_settled_state(kLoginTimeout);
    return json_response(req, http_status_for(state), snapshot_to_json(account.public_snapshot()));
}

Frame handle_login_2fa(const Frame& req, apple::Account& account) {
    json body = parse_payload_json(req);
    if (!body.is_object() || !body.contains("code") || !body["code"].is_string()) {
        return json_response(req, 400, json{
            {"error", "missing_field"},
            {"detail", "expected JSON object with string 'code'"},
        });
    }
    std::string code = body["code"].get<std::string>();
    if (code.empty()) {
        return json_response(req, 400, json{
            {"error", "empty_code"},
            {"detail", "code must be non-empty"},
        });
    }
    if (!account.submit_2fa(std::move(code))) {
        return json_response(req, 409, json{
            {"error", "not_awaiting_2fa"},
            {"detail", "no login is currently waiting for a 2FA code; start one with POST /login"},
        });
    }
    auto state = account.wait_for_settled_state(kLogin2faTimeout);
    return json_response(req, http_status_for(state), snapshot_to_json(account.public_snapshot()));
}

Frame handle_logout(const Frame& req, apple::Account& account) {
    auto prev = account.state();
    account.logout();
    return json_response(req, 200, json{
        {"state", apple::to_string(account.state())},
        {"was", apple::to_string(prev)},
    });
}

Frame handle_playback(const Frame& req,
                      apple::Account& account,
                      const apple::Loader& loader,
                      apple::Runtime& rt) {
    if (!rt.initialized()) {
        return json_response(req, 503, json{
            {"error", "runtime_not_initialized"},
            {"detail", "Apple lib init has not completed; check /health"},
        });
    }
    if (!authenticated_or_restored(account, loader, rt)) {
        return json_response(req, 401, json{
            {"error", "not_authenticated"},
            {"detail", "POST /login or restore a session first"},
        });
    }
    json body = parse_payload_json(req);
    std::string adam_id = body.value("adam_id", "");
    if (adam_id.empty()) {
        return json_response(req, 400, json{
            {"error", "missing_field"},
            {"detail", "expected query string '?adam_id=<numeric store id>'"},
        });
    }
    apple::PlaybackResult pr;
    {
        std::lock_guard<std::mutex> lock(rt.playback_mutex());
        pr = apple::fetch_playback_json(loader, rt, std::move(adam_id));
    }
    if (!pr.ok) {
        return json_response(req, 502, json{
            {"error", "playback_dispatch_failed"},
            {"detail", pr.error},
        });
    }
    return json_response(req, 200, std::move(pr.body));
}

bool parse_decrypt_payload(const Frame& req,
                           std::string* adam_id,
                           std::string* uri,
                           std::vector<std::vector<std::uint8_t>>* samples) {
    if (req.payload.size() < 8) return false;
    const auto* p = req.payload.data();
    const std::uint16_t adam_len = read_u16_be(p);
    const std::uint16_t uri_len = read_u16_be(p + 2);
    const std::uint32_t sample_count = read_u32_be(p + 4);
    if (adam_len == 0 || uri_len == 0 || sample_count == 0) return false;
    const std::uint64_t table_bytes = static_cast<std::uint64_t>(sample_count) * 4ull;
    const std::uint64_t fixed = 8ull + table_bytes + adam_len + uri_len;
    if (fixed > req.payload.size()) return false;

    std::vector<std::uint32_t> lengths;
    lengths.reserve(sample_count);
    std::uint64_t sample_bytes = 0;
    const auto* lenp = p + 8;
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        const std::uint32_t n = read_u32_be(lenp + (static_cast<std::size_t>(i) * 4u));
        if (n == 0) return false;
        sample_bytes += n;
        if (sample_bytes > 256ull * 1024ull * 1024ull) return false;
        lengths.push_back(n);
    }
    if (fixed + sample_bytes != req.payload.size()) return false;

    const char* s = reinterpret_cast<const char*>(p + 8 + table_bytes);
    adam_id->assign(s, adam_len);
    s += adam_len;
    uri->assign(s, uri_len);
    s += uri_len;
    samples->clear();
    samples->reserve(sample_count);
    for (std::uint32_t n : lengths) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(s);
        samples->emplace_back(b, b + n);
        s += n;
    }
    return true;
}

std::vector<std::uint8_t> build_decrypt_response_payload(
    const std::vector<std::vector<std::uint8_t>>& samples) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + samples.size() * 4);
    append_u32_be(&out, static_cast<std::uint32_t>(samples.size()));
    for (const auto& sample : samples) {
        append_u32_be(&out, static_cast<std::uint32_t>(sample.size()));
    }
    for (const auto& sample : samples) {
        out.insert(out.end(), sample.begin(), sample.end());
    }
    return out;
}

Frame handle_decrypt_batch(const Frame& req,
                            apple::Account& account,
                            const apple::Loader& loader,
                            apple::Runtime& rt) {
    if (!rt.initialized()) {
        return json_response(req, 503, json{
            {"error", "runtime_not_initialized"},
            {"detail", "Apple lib init has not completed; check /health"},
        });
    }
    if (!rt.playback_ready()) {
        return json_response(req, 503, json{
            {"error", "decrypt_unavailable"},
            {"detail", "FPS playback stack did not initialize; check /health.runtime"},
        });
    }
    if (!authenticated_or_restored(account, loader, rt)) {
        return json_response(req, 401, json{
            {"error", "not_authenticated"},
            {"detail", "POST /login or restore a session first"},
        });
    }
    std::string adam_id;
    std::string uri;
    std::vector<std::vector<std::uint8_t>> samples;
    if (!parse_decrypt_payload(req, &adam_id, &uri, &samples)) {
        return json_response(req, 400, json{
            {"error", "invalid_frame"},
            {"detail", "invalid decrypt batch payload"},
        });
    }
    const std::size_t expected_count = samples.size();
    apple::DecryptResult dr;
    {
        std::lock_guard<std::mutex> lock(rt.playback_mutex());
        dr = apple::decrypt_samples(loader, rt, std::move(adam_id), std::move(uri), std::move(samples));
    }
    if (!dr.ok || dr.plaintexts.size() != expected_count) {
        return json_response(req, 502, json{
            {"error", "decrypt_failed"},
            {"detail", dr.error.empty() ? "FPS decrypt failed" : dr.error},
        }, true, true);
    }
    return binary_response(req, build_decrypt_response_payload(dr.plaintexts));
}

Frame dispatch(const Frame& req,
               apple::Runtime& rt,
               apple::Loader& loader,
               apple::Account& account,
               const ServerInfo& info) {
    try {
        switch (req.opcode) {
            case OpHealth: return handle_health(req, loader, rt, info);
            case OpMe: return handle_me(req, account, loader, rt, info);
            case OpLogin: return handle_login(req, account, loader, rt);
            case OpLogin2fa: return handle_login_2fa(req, account);
            case OpLogout: return handle_logout(req, account);
            case OpPlayback: return handle_playback(req, account, loader, rt);
            case OpDecryptBatch: return handle_decrypt_batch(req, account, loader, rt);
            case OpShutdown: std::exit(0);
            default:
                return json_response(req, 400, json{
                    {"error", "unknown_opcode"},
                    {"detail", req.opcode},
                });
        }
    } catch (const std::exception& e) {
        return json_response(req, 500, json{{"error", "internal"}, {"detail", e.what()}});
    } catch (...) {
        return json_response(req, 500, json{{"error", "internal"}, {"detail", "unknown exception"}});
    }
}

}  // namespace

int run_ipc_worker(apple::Runtime& rt,
                   apple::Loader& loader,
                   apple::Account& account,
                   const ServerInfo& info) {
    std::fprintf(stderr, "wrapper-v2: ipc worker ready\n");
    for (;;) {
        Frame req;
        if (!read_frame(&req)) {
            std::fprintf(stderr, "ipc: input closed\n");
            return 0;
        }
        if (req.kind != kKindRequest) {
            std::fprintf(stderr, "ipc: ignoring non-request frame kind=%u\n", req.kind);
            continue;
        }
        Frame resp = dispatch(req, rt, loader, account, info);
        if (!write_frame(resp)) {
            std::fprintf(stderr, "ipc: output closed\n");
            return 0;
        }
    }
}

}  // namespace wrapper
