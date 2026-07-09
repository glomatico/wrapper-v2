// Stdio IPC worker used by the Rust supervisor.

#pragma once

#include "apple/auth.hpp"
#include "apple/loader.hpp"
#include "apple/runtime.hpp"
#include "info.hpp"

namespace wrapper {

int run_ipc_worker(apple::Runtime& rt,
                   apple::Loader& loader,
                   apple::Account& account,
                   const ServerInfo& info);

}  // namespace wrapper
