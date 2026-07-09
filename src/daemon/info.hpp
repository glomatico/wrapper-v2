#pragma once

#include <string>

namespace wrapper {

struct ServerInfo {
    std::string version = "0.0.2";
    bool apple_init_enabled = true;
};

}  // namespace wrapper
