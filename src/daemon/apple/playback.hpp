// Playback dispatch retrieval (GET /playback).
//
// Mirrors upstream wrapper main.c:get_m3u8_method_download but returns
// the *whole* MZ playback dispatch instead of just one m3u8 URL.
// Implementation drives Apple's storeservicescore::PurchaseRequest with
// urlBagKey="subDownload"; PurchaseRequest::run() builds a parsed
// PurchaseResponse whose items each expose a __CFDictionary* of the
// raw plist payload. We walk those dicts and convert CF types to JSON
// (string/number/bool/array/object/base64-data/iso8601-date) so the
// client receives a native nlohmann::json document.

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace wrapper::apple {

class Loader;
class Runtime;

struct PlaybackResult {
    bool             ok = false;
    nlohmann::json   body;  // JSON document (application/json response body)
    std::string      error; // populated when ok == false
};

// Blocking. Caller must hold runtime.playback_mutex() (Apple's
// PurchaseRequest dispatch uses process-global state we share with
// /decrypt). Requires runtime.initialized() and an authenticated account.
PlaybackResult fetch_playback_json(const Loader& loader,
                                   Runtime&      runtime,
                                   std::string   adam_id);

}  // namespace wrapper::apple
