#pragma once
#include "common.hpp"
namespace worker {
using Decrypt=long(*)(void*,uint32_t,void*,void*,size_t);
void install_capture();
// Each implementation owns its native state layout and returns a tagged
// context. Shared runtime code never interprets architecture-specific fields.
Json capture(void*,Decrypt);
}
