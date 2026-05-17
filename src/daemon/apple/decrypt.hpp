// FairPlay FPS sample decryption (POST /decrypt). Mirrors upstream
// wrapper main.c:getKdContext + NfcRKVnxuKZy04KWbdFu71Ou.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wrapper::apple {

class Loader;
class Runtime;

struct DecryptResult {
    bool                     ok = false;
    std::string              error;
    std::vector<std::vector<std::uint8_t>> plaintexts;
};

// Decrypt one or more ciphertext samples for the same (adam_id, uri) key.
// Caller must hold runtime.playback_mutex() for the Apple calls (server does).
DecryptResult decrypt_samples(const Loader& loader,
                              Runtime&      runtime,
                              std::string   adam_id,
                              std::string   key_uri,
                              std::vector<std::vector<std::uint8_t>> ciphertexts);

}  // namespace wrapper::apple
