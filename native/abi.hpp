#pragma once
// Use real C++ return types. Clang emits SysV sret on x86_64 and AAPCS64 x8
// on arm64-v8a, including non-trivial shared_ptr and vector return values.
// Opaque Configuration results have an independently verified 88-byte layout.
#include <array>
#include <cstdint>
namespace worker {
struct ConfigurationResult {
    alignas(8) std::array<std::uint8_t,88> bytes{};
};
static_assert(sizeof(ConfigurationResult)==88);
}
