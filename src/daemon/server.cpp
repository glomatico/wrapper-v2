#include "server.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "apple/decrypt.hpp"
#include "apple/playback.hpp"

namespace wrapper {

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// How long POST /login is willing to block waiting for AuthenticateFlow
// to settle to a terminal state (Authenticated/Failed) or to Awaiting2FA.
// Apple's flow normally completes well within a couple of seconds when
// no 2FA is needed; the credentialHandler dispatch is what controls
// when this returns.
constexpr auto kLoginTimeout = 30s;

// How long POST /login/2fa is willing to block after submitting the
// code. The flow has to make a network round-trip back to Apple.
constexpr auto kLogin2faTimeout    = 60s;

void respond_json(httplib::Response& res, int status, json body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void access_log(const char* method, const httplib::Request& req) {
    const std::string& peer = req.remote_addr;
    const char* p = peer.empty() ? "-" : peer.c_str();
    std::fprintf(stderr, "http: %s %s client=%s\n", method, req.path.c_str(), p);
}

std::string iso8601_utc(std::chrono::system_clock::time_point tp) {
    if (tp.time_since_epoch().count() == 0) return {};
    auto t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
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
        {"loader_ok",          loader.ok()},
        {"initialized",        rt.initialized()},
        {"playback_ready",     rt.playback_ready()},
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

std::uint32_t read_u32_be(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24)
         | (static_cast<std::uint32_t>(p[1]) << 16)
         | (static_cast<std::uint32_t>(p[2]) << 8)
         |  static_cast<std::uint32_t>(p[3]);
}

void append_u32_be(std::string* out, std::uint32_t v) {
    out->push_back(static_cast<char>((v >> 24) & 0xff));
    out->push_back(static_cast<char>((v >> 16) & 0xff));
    out->push_back(static_cast<char>((v >> 8) & 0xff));
    out->push_back(static_cast<char>(v & 0xff));
}

struct SampleDecryptFrame {
    std::string adam_id;
    std::string uri;
    std::vector<std::vector<std::uint8_t>> samples;
};

bool parse_sample_decrypt_frame(const std::string& body,
                                SampleDecryptFrame* out,
                                std::string* error) {
    constexpr std::uint32_t kMaxSamples = 100000;
    constexpr std::uint32_t kMaxFieldLen = 1024 * 1024;
    constexpr std::uint64_t kMaxBodyLen = 256ull * 1024ull * 1024ull;

    const auto* data = reinterpret_cast<const std::uint8_t*>(body.data());
    const std::size_t size = body.size();
    if (size > kMaxBodyLen) {
        *error = "request body too large";
        return false;
    }
    if (size < 12) {
        *error = "frame too short";
        return false;
    }

    const std::uint32_t adam_len = read_u32_be(data);
    const std::uint32_t uri_len = read_u32_be(data + 4);
    const std::uint32_t sample_count = read_u32_be(data + 8);
    if (adam_len == 0 || uri_len == 0) {
        *error = "adam_id and uri must be non-empty";
        return false;
    }
    if (adam_len > kMaxFieldLen || uri_len > kMaxFieldLen) {
        *error = "adam_id or uri is too large";
        return false;
    }
    if (sample_count == 0 || sample_count > kMaxSamples) {
        *error = "sample_count must be between 1 and 100000";
        return false;
    }

    const std::uint64_t table_bytes = static_cast<std::uint64_t>(sample_count) * 4u;
    const std::uint64_t fixed = 12ull + table_bytes + adam_len + uri_len;
    if (fixed > size) {
        *error = "frame length table exceeds request body";
        return false;
    }

    std::vector<std::uint32_t> lengths;
    lengths.reserve(sample_count);
    std::uint64_t sample_bytes = 0;
    const std::uint8_t* lenp = data + 12;
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        const std::uint32_t n = read_u32_be(lenp + (static_cast<std::size_t>(i) * 4u));
        if (n == 0) {
            *error = "sample length must be non-zero";
            return false;
        }
        sample_bytes += n;
        if (sample_bytes > kMaxBodyLen) {
            *error = "sample payload too large";
            return false;
        }
        lengths.push_back(n);
    }
    if (fixed + sample_bytes != size) {
        *error = "frame size does not match declared lengths";
        return false;
    }

    const char* p = body.data() + 12 + static_cast<std::size_t>(table_bytes);
    out->adam_id.assign(p, adam_len);
    p += adam_len;
    out->uri.assign(p, uri_len);
    p += uri_len;

    out->samples.clear();
    out->samples.reserve(sample_count);
    for (std::uint32_t n : lengths) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(p);
        out->samples.emplace_back(b, b + n);
        p += n;
    }
    return true;
}

bool build_sample_decrypt_frame(const std::vector<std::vector<std::uint8_t>>& samples,
                                std::string* out) {
    if (samples.size() > 0xffffffffu) return false;
    std::uint64_t size = 4ull + (static_cast<std::uint64_t>(samples.size()) * 4ull);
    for (const auto& sample : samples) {
        if (sample.size() > 0xffffffffu) return false;
        size += sample.size();
    }
    if (size > static_cast<std::uint64_t>(out->max_size())) return false;

    out->clear();
    out->reserve(static_cast<std::size_t>(size));
    append_u32_be(out, static_cast<std::uint32_t>(samples.size()));
    for (const auto& sample : samples) {
        append_u32_be(out, static_cast<std::uint32_t>(sample.size()));
    }
    for (const auto& sample : samples) {
        out->append(reinterpret_cast<const char*>(sample.data()), sample.size());
    }
    return true;
}

}  // namespace

Server::Server(httplib::Server& svr,
               apple::Runtime& rt,
               apple::Loader& loader,
               apple::Account& account,
               ServerInfo info)
    : svr_(svr), rt_(rt), loader_(loader), account_(account), info_(std::move(info)) {}

void Server::mount() {
    // ---- GET /health ----
    // Liveness + runtime debug info. Always returns 200 if the
    // process is up; consumers should treat runtime.initialized==false
    // as a soft failure (auth/decrypt endpoints won't work) rather than a hard one.
    svr_.Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("GET", req);
        json runtime = {
            {"apple_init_enabled", info_.apple_init_enabled},
            {"loader_ok",          loader_.ok()},
            {"initialized",        rt_.initialized()},
            {"playback_ready",     rt_.playback_ready()},
        };
        if (!loader_.ok() && !loader_.last_error().empty()) {
            runtime["loader_error"] = loader_.last_error();
        }
        if (rt_.initialized()) {
            runtime["base_dir"] = rt_.base_dir();
            runtime["device_info"] = rt_.device_info();
        }
        respond_json(res, 200, json{
            {"status",  "ok"},
            {"version", info_.version},
            {"runtime", std::move(runtime)},
        });
    });

    // ---- GET /me ----
    // Combined daemon snapshot: version, runtime probe (same facts as
    // /health.runtime), and auth (Apple ID state + harvested tokens after
    // a successful POST /login). iTunes account-token / X-Token are NOT
    // exposed — only dev_token, music_user_token, storefront, dsid.
    svr_.Get("/me", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("GET", req);
        json body = {
            {"version", info_.version},
            {"runtime", runtime_to_json(loader_, rt_, info_)},
            {"auth", snapshot_to_json(account_.public_snapshot())},
        };
        respond_json(res, 200, std::move(body));
    });

    // ---- POST /login ----
    // Body: { "username": "...", "password": "..." }
    //    or { "apple_id": "...", "password": "..." } (synonyms)
    // Returns:
    //   200 if AuthenticateFlow completed (state=authenticated, tokens present)
    //   202 if Apple asked for HSA2 (state=awaiting_2fa) - follow up with
    //       POST /login/2fa
    //   401 if Apple rejected credentials (state=failed)
    //   409 if a login is already in progress
    //   503 if the runtime is not initialized
    //   504 if the flow has not produced any state inside kLoginTimeout
    svr_.Post("/login", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("POST", req);
        if (!rt_.initialized()) {
            respond_json(res, 503, json{
                {"error", "runtime_not_initialized"},
                {"detail", "Apple lib init has not completed; check /health"},
            });
            return;
        }

        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& e) {
            respond_json(res, 400, json{{"error", "invalid_json"}, {"detail", e.what()}});
            return;
        }
        if (!body.is_object() || !body.contains("password")
            || !body["password"].is_string()) {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected JSON object with string 'password' and "
                            "'username' or 'apple_id'"},
            });
            return;
        }
        std::string login_name;
        const bool has_apple =
            body.contains("apple_id") && body["apple_id"].is_string();
        const bool has_user =
            body.contains("username") && body["username"].is_string();
        if (has_apple && has_user) {
            std::string a = body["apple_id"].get<std::string>();
            std::string u = body["username"].get<std::string>();
            if (a != u) {
                respond_json(res, 400, json{
                    {"error", "conflicting_identifiers"},
                    {"detail", "'username' and 'apple_id' must match if both are sent"},
                });
                return;
            }
            login_name = std::move(a);
        } else if (has_apple) {
            login_name = body["apple_id"].get<std::string>();
        } else if (has_user) {
            login_name = body["username"].get<std::string>();
        } else {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected 'username' or 'apple_id' string"},
            });
            return;
        }
        std::string password = body["password"].get<std::string>();
        if (login_name.empty() || password.empty()) {
            respond_json(res, 400, json{
                {"error", "empty_field"},
                {"detail", "username/apple_id and password must be non-empty"},
            });
            return;
        }

        if (!account_.start_login(loader_, rt_, std::move(login_name), std::move(password))) {
            if (account_.state() == apple::LoginState::Authenticated) {
                respond_json(res, 409, json{
                    {"error", "already_authenticated"},
                    {"detail", "call DELETE /login before signing in again"},
                });
                return;
            }
            respond_json(res, 409, json{
                {"error", "already_in_progress"},
                {"detail", "a login flow is already running; DELETE /login to abort"},
            });
            return;
        }

        auto state = account_.wait_for_settled_state(kLoginTimeout);
        respond_json(res, http_status_for(state), snapshot_to_json(account_.public_snapshot()));
    });

    // ---- POST /login/2fa ----
    // Body: { "code": "123456" }
    // Returns 200 / 401 / 409 / 504 with the same shape as /login.
    svr_.Post("/login/2fa", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("POST", req);
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& e) {
            respond_json(res, 400, json{{"error", "invalid_json"}, {"detail", e.what()}});
            return;
        }
        if (!body.is_object() || !body.contains("code") || !body["code"].is_string()) {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected JSON object with string 'code'"},
            });
            return;
        }
        std::string code = body["code"].get<std::string>();
        if (code.empty()) {
            respond_json(res, 400, json{
                {"error", "empty_code"},
                {"detail", "code must be non-empty"},
            });
            return;
        }

        if (!account_.submit_2fa(std::move(code))) {
            respond_json(res, 409, json{
                {"error", "not_awaiting_2fa"},
                {"detail", "no login is currently waiting for a 2FA code; "
                           "start one with POST /login"},
            });
            return;
        }

        auto state = account_.wait_for_settled_state(kLogin2faTimeout);
        respond_json(res, http_status_for(state), snapshot_to_json(account_.public_snapshot()));
    });

    // ---- GET /playback ----
    // Returns Apple's full MZ-protocol playback dispatch as native JSON
    // (the CFDictionary plist tree walked into nlohmann::json: dict ->
    // object, array -> array, string/number/bool -> matching JSON types,
    // CFData -> base64 string, CFDate -> ISO 8601). Driven by
    // storeservicescore::PurchaseRequest with urlBagKey="subDownload"
    // (matching upstream wrapper's get_m3u8_method_download). Unlike
    // upstream which extracts just the last asset's URL, we hand back
    // every flavor / key URI / metadata field Apple included.
    svr_.Get("/playback", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("GET", req);
        if (!rt_.initialized()) {
            respond_json(res, 503, json{
                {"error", "runtime_not_initialized"},
                {"detail", "Apple lib init has not completed; check /health"},
            });
            return;
        }
        if (account_.state() != apple::LoginState::Authenticated) {
            respond_json(res, 401, json{
                {"error", "not_authenticated"},
                {"detail", "POST /login or restore a session first"},
            });
            return;
        }

        std::string adam_id;
        if (req.has_param("adam_id")) {
            adam_id = req.get_param_value("adam_id");
        } else if (req.has_param("adamId")) {
            adam_id = req.get_param_value("adamId");
        } else {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected query string '?adam_id=<numeric store id>'"},
            });
            return;
        }
        if (adam_id.empty()) {
            respond_json(res, 400, json{
                {"error", "empty_field"},
                {"detail", "adam_id must be non-empty"},
            });
            return;
        }

        apple::PlaybackResult pr;
        {
            // PurchaseRequest drives Apple's URLBag dispatcher; same global
            // state /decrypt touches, so share the playback mutex to avoid
            // overlap with a concurrent decrypt request.
            std::lock_guard<std::mutex> lock(rt_.playback_mutex());
            pr = apple::fetch_playback_json(loader_, rt_, std::move(adam_id));
        }

        if (!pr.ok) {
            respond_json(res, 502, json{
                {"error", "playback_dispatch_failed"},
                {"detail", pr.error},
            });
            return;
        }

        respond_json(res, 200, std::move(pr.body));
    });

    // ---- POST /decrypt ----
    // FairPlay sample decrypt. Binary request:
    //   u32be adam_id_len, u32be uri_len, u32be sample_count,
    //   u32be sample_len[sample_count], adam_id bytes, uri bytes,
    //   concatenated ciphertext samples.
    // Binary response:
    //   u32be sample_count, u32be sample_len[sample_count],
    //   concatenated plaintext samples.
    // Requires authenticated + playback_ready.
    svr_.Post("/decrypt", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("POST", req);
        if (!rt_.initialized()) {
            respond_json(res, 503, json{
                {"error", "runtime_not_initialized"},
                {"detail", "Apple lib init has not completed; check /health"},
            });
            return;
        }
        if (!rt_.playback_ready()) {
            respond_json(res, 503, json{
                {"error", "decrypt_unavailable"},
                {"detail", "FairPlay playback stack did not initialize; check /health.runtime"},
            });
            return;
        }
        if (account_.state() != apple::LoginState::Authenticated) {
            respond_json(res, 401, json{
                {"error", "not_authenticated"},
                {"detail", "POST /login or restore a session first"},
            });
            return;
        }

        SampleDecryptFrame frame;
        std::string parse_error;
        if (!parse_sample_decrypt_frame(req.body, &frame, &parse_error)) {
            respond_json(res, 400, json{{"error", "invalid_frame"}, {"detail", parse_error}});
            return;
        }

        apple::DecryptResult dr;
        {
            std::lock_guard<std::mutex> lock(rt_.playback_mutex());
            dr = apple::decrypt_samples(loader_, rt_, std::move(frame.adam_id), std::move(frame.uri),
                                        std::move(frame.samples));
        }

        if (!dr.ok) {
            respond_json(res, 502, json{
                {"error", "decrypt_failed"},
                {"detail", dr.error},
            });
            return;
        }

        std::string response_body;
        if (!build_sample_decrypt_frame(dr.plaintexts, &response_body)) {
            respond_json(res, 500, json{
                {"error", "response_too_large"},
                {"detail", "decrypted sample frame is too large"},
            });
            return;
        }
        res.status = 200;
        res.set_content(response_body, "application/octet-stream");
    });

    // ---- DELETE /login ----
    // Clears in-memory tokens and (if a flow is running) signals the
    // worker thread to abort. Apple's kvs.sqlitedb cache is NOT
    // touched; the next POST /login will reuse it if still valid.
    svr_.Delete("/login", [this](const httplib::Request& req, httplib::Response& res) {
        access_log("DELETE", req);
        auto prev = account_.state();
        account_.logout();
        respond_json(res, 200, json{
            {"state", apple::to_string(account_.state())},
            {"was",   apple::to_string(prev)},
        });
    });

    // ---- exception fallback ----
    svr_.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                  std::exception_ptr ep) {
        std::fprintf(stderr, "http: exception %s %s\n",
                     req.method.c_str(), req.path.c_str());
        std::string what = "unknown";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        respond_json(res, 500, json{{"error", "internal"}, {"detail", what}});
    });
}

}  // namespace wrapper
