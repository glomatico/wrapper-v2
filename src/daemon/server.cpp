#include "server.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "apple/decrypt.hpp"

namespace wrapper {

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// How long POST /login is willing to block waiting for AuthenticateFlow
// to settle to a terminal state (Authenticated/Failed) or to Awaiting2FA.
// Apple's flow normally completes well within a couple of seconds when
// no 2FA is needed; the credentialHandler dispatch is what controls
// when this returns.
constexpr auto kLoginPhase1Timeout = 30s;

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

int base64_dec_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool base64_decode(std::string_view in, std::vector<std::uint8_t>* out) {
    out->clear();
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) continue;
        if (c == '=') break;
        int d = base64_dec_digit(c);
        if (d < 0) return false;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out->push_back(static_cast<std::uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string ret;
    ret.reserve(((data.size() + 2) / 3) * 4);
    unsigned int val = 0;
    int          valb = -6;
    for (std::uint8_t c : data) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            ret.push_back(tbl[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        ret.push_back(tbl[((val << 8) >> (valb + 8)) & 0x3F]);
    }
    while (ret.size() % 4 != 0) {
        ret.push_back('=');
    }
    return ret;
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
    // as a soft failure (auth/decrypt won't work) rather than a hard one.
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
            {"phase",   1.3},
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
    //   504 if the flow has not produced any state inside kLoginPhase1Timeout
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

        auto state = account_.wait_for_settled_state(kLoginPhase1Timeout);
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

    // ---- POST /decrypt ----
    // FairPlay sample decrypt. Body: adam_id, uri, samples: [base64, ...]
    // or a single "sample": base64. Requires authenticated + playback_ready.
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

        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception& e) {
            respond_json(res, 400, json{{"error", "invalid_json"}, {"detail", e.what()}});
            return;
        }
        if (!body.is_object()) {
            respond_json(res, 400, json{{"error", "invalid_body"}, {"detail", "expected object"}});
            return;
        }

        std::string adam_id;
        if (body.contains("adam_id") && body["adam_id"].is_string()) {
            adam_id = body["adam_id"].get<std::string>();
        } else if (body.contains("adamId") && body["adamId"].is_string()) {
            adam_id = body["adamId"].get<std::string>();
        } else {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected string 'adam_id' (or 'adamId')"},
            });
            return;
        }

        if (!body.contains("uri") || !body["uri"].is_string()) {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected string 'uri' (SKD URI)"},
            });
            return;
        }
        std::string uri = body["uri"].get<std::string>();

        std::vector<std::vector<std::uint8_t>> ciphertexts;
        if (body.contains("samples")) {
            if (!body["samples"].is_array()) {
                respond_json(res, 400, json{{"error", "invalid_field"}, {"detail", "'samples' must be array"}});
                return;
            }
            for (const auto& el : body["samples"]) {
                if (!el.is_string()) {
                    respond_json(res, 400, json{{"error", "invalid_field"}, {"detail", "each sample must be base64 string"}});
                    return;
                }
                std::vector<std::uint8_t> chunk;
                if (!base64_decode(el.get<std::string>(), &chunk)) {
                    respond_json(res, 400, json{{"error", "invalid_base64"}, {"detail", "samples must be standard base64"}});
                    return;
                }
                ciphertexts.push_back(std::move(chunk));
            }
        } else if (body.contains("sample") && body["sample"].is_string()) {
            std::vector<std::uint8_t> chunk;
            if (!base64_decode(body["sample"].get<std::string>(), &chunk)) {
                respond_json(res, 400, json{{"error", "invalid_base64"}, {"detail", "sample must be standard base64"}});
                return;
            }
            ciphertexts.push_back(std::move(chunk));
        } else {
            respond_json(res, 400, json{
                {"error", "missing_field"},
                {"detail", "expected 'samples' array or string 'sample' (base64)"},
            });
            return;
        }

        if (adam_id.empty() || uri.empty()) {
            respond_json(res, 400, json{{"error", "empty_field"}, {"detail", "adam_id and uri must be non-empty"}});
            return;
        }

        apple::DecryptResult dr;
        {
            std::lock_guard<std::mutex> lock(rt_.playback_mutex());
            dr = apple::decrypt_samples(loader_, rt_, std::move(adam_id), std::move(uri),
                                        std::move(ciphertexts));
        }

        if (!dr.ok) {
            respond_json(res, 502, json{
                {"error", "decrypt_failed"},
                {"detail", dr.error},
            });
            return;
        }

        json samples = json::array();
        for (const auto& p : dr.plaintexts) {
            samples.push_back(base64_encode(p));
        }
        respond_json(res, 200, json{{"samples", std::move(samples)}});
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
