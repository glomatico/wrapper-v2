#include "capture.hpp"
#if !defined(__aarch64__) || !defined(__ANDROID__)
#error This implementation requires Android AArch64.
#endif
namespace worker {
// The ARM64 native runtime is separate from its as-yet unverified capture
// contract. Never install x86 hooks or mislabel ARM native state as x86 state.
void install_capture() {}
Json capture(void*,Decrypt) {
    throw Error("unsupported_capture_profile");
}
}
