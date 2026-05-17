#include "apple/decrypt.hpp"

#include <mutex>
#include <utility>

#include "apple/fairplay_cert.inc"
#include "apple/loader.hpp"
#include "apple/runtime.hpp"

namespace wrapper::apple {

namespace {

constexpr char kPrefetchAdam[] = "0";

// zhaarey/wrapper main.c getKdContext:
// - Never destroys SVFootHillPContext (stack POD shared_ptr, no refcount decrement).
// - preshareCtx: first successful adam=="0" path caches kd; later adam=="0" skips
//   getPersistentKey entirely (no URI check in upstream).
//
// Calling shared_ptr_SVFootHillPContext_dtor breaks kd / Apple's FootHill state.

std::mutex g_preshare_mu;
void*      g_preshare_kd = nullptr;

}  // namespace

DecryptResult decrypt_samples(const Loader& loader,
                              Runtime&      runtime,
                              std::string   adam_id,
                              std::string   key_uri,
                              std::vector<std::vector<std::uint8_t>> ciphertexts) {
    DecryptResult out;
    if (!loader.ok() || !loader.fairplay_decrypt_available()) {
        out.error = "FairPlay decrypt chain not loaded";
        return out;
    }
    if (!runtime.playback_ready()) {
        out.error = "playback stack not ready";
        return out;
    }
    if (adam_id.empty() || key_uri.empty()) {
        out.error = "adam_id and uri are required";
        return out;
    }
    if (ciphertexts.empty()) {
        out.error = "at least one sample required";
        return out;
    }

    const Symbols& s  = loader.sym();
    void*          fh = runtime.foothill_session();

    void* kd = nullptr;

    if (adam_id == kPrefetchAdam) {
        std::lock_guard<std::mutex> lock(g_preshare_mu);
        if (g_preshare_kd != nullptr) {
            kd = g_preshare_kd;
        }
    }

    if (kd == nullptr) {
        auto        default_id = abi::make_string_view(adam_id.c_str());
        auto        uri        = abi::make_string_view(key_uri.c_str());
        auto        key_format = abi::make_string_view("com.apple.streamingkeydelivery");
        auto        key_ver    = abi::make_string_view("1");
        auto        server_uri =
            abi::make_string_view("https://play.itunes.apple.com/WebObjects/MZPlay.woa/music/fps");
        auto        protocol   = abi::make_string_view("simplified");
        auto        fps_cert   = abi::make_string_view(kFairPlayCert);

        abi::shared_ptr persist{};
        // Same as zhaarey/apple-music-downloader agent.js (7 string args after this).
        s.SVFootHillSessionCtrl_getPersistentKey(
            &persist, fh, &default_id, &uri, &key_format, &key_ver, &server_uri, &protocol,
            &fps_cert);

        if (persist.obj == nullptr) {
            out.error = "getPersistentKey failed (key or lease?)";
            return out;
        }

        abi::shared_ptr sv_ctx{};
        s.SVFootHillSessionCtrl_decryptContext(&sv_ctx, fh, persist.obj);

        if (sv_ctx.obj == nullptr) {
            out.error = "decryptContext failed";
            return out;
        }

        // Upstream main.c does TWO dereferences:
        //   void* p = *kdContext_method(ctx);   // *(void**) -> void*
        //   ... NfcRKVn(*(void**)p, ...)         // re-cast and deref again
        // i.e. fp_sample_decrypt receives **kdContext_method(ctx). Doing only
        // one deref passes the kd-handle struct pointer instead of the actual
        // engine state pointer; fp_sample_decrypt doesn't error but the
        // produced plaintext is garbage (audio plays back unplayable).
        void** kd_pp = s.SVFootHillPContext_kdContext(sv_ctx.obj);
        if (kd_pp == nullptr || *kd_pp == nullptr) {
            out.error = "kdContext is null";
            return out;
        }
        kd = *reinterpret_cast<void**>(*kd_pp);
        if (kd == nullptr) {
            out.error = "kdContext inner pointer is null";
            return out;
        }

        if (adam_id == kPrefetchAdam) {
            std::lock_guard<std::mutex> lock(g_preshare_mu);
            g_preshare_kd = kd;
        }

        // Intentionally no shared_ptr dtors — see block comment above.
        (void)persist;
        (void)sv_ctx;
    }

    out.plaintexts.reserve(ciphertexts.size());
    for (auto& chunk : ciphertexts) {
        if (chunk.empty()) {
            out.error = "empty sample";
            out.plaintexts.clear();
            return out;
        }
        s.fp_sample_decrypt(kd, 5u, chunk.data(), chunk.data(), chunk.size());
        out.plaintexts.push_back(std::move(chunk));
    }

    out.ok = true;
    return out;
}

}  // namespace wrapper::apple
