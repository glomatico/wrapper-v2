#pragma once
#include "common.hpp"
namespace worker {
using Decrypt=long(*)(void*,uint32_t,void*,void*,size_t);
struct Capture {
    std::array<unsigned char,0x8000> ctx{};
    std::array<unsigned char,0x800> state{};
    unsigned char bl=0,r9b=0;
    ~Capture(){wipe(ctx.data(),ctx.size());wipe(state.data(),state.size());bl=r9b=0;}
    Capture()=default;Capture(const Capture&)=delete;Capture& operator=(const Capture&)=delete;
    Json json()const{return {{"profile",profile},{"ctx",base64(ctx.data(),ctx.size())},{"state",base64(state.data(),state.size())},{"bl",bl},{"r9b",r9b}};}
};
void install_capture();
void capture(void*,Decrypt,Capture&);
}
