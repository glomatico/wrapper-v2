// Pinned public artifact digests; no Apple binary content.
#pragma once
#include <utility>
namespace worker {
inline constexpr std::pair<const char*,const char*> apple_libraries[]={
    {"libBlocksRuntime.so","9814b756ae173a546eb12bf1019746f14b17a6546806445ff67f75b0aba8856a"},
    {"libCoreADI.so","b5b971cea0bbe828bdcfaf219fd91fc1e37cf8daee84001ef917d0b1c2b985ed"},
    {"libCoreFoundation.so","c5727cecfa18833a6828caf2072aa6a4edf39b73752597e4de35854ea9a71ce0"},
    {"libCoreFP.so","81ba0dbd857a9e76402434ee13756051a77de91ec18d637ea4350687e0da39b3"},
    {"libCoreLSKD.so","bf813206d382bd8b27384bd546a5b9aa730df32c8543d709756b081b619a8b3a"},
    {"libandroidappmusic.so","2fcdf31a0b3f0f8841ef63cf46efffcf9672a2cea0eef5b066c537d43e293083"},
    {"libc++_shared.so","82ffc20ba230bbdd031b27853d6cfaaa27079aabe6dd478cafd9b6fa866a35c2"},
    {"libcurl.so","922d6c70ab8a86a761046c70902d3d6c56b136fb2a3b21dbc5a4e3fe5f6130ae"},
    {"libdaapkit.so","e70575f2f3279141d03c91dff7400151e321fb30051f170e25bdbeae862a7bbd"},
    {"libdispatch.so","7d360c1e81c6982b70b32bdc4e343fd586d727722fd6562df211aaf1955b0e2c"},
    {"libicudata_sv_apple.so","afe9021fd9fee67f1f2909f3bb603f0362e93be5ef761d4dee9812249b2aa0f0"},
    {"libicui18n_sv_apple.so","7ae2e3c0e78e54e94a2ce2d7f3efbb4375e2505769f4ebeb43f05ac7d344ff76"},
    {"libicutest_sv_apple.so","3bc7b14ca5a9eefcbfdbaa943a0ff81a9406832ecd6ac5d18fb818569a733750"},
    {"libicuuc_sv_apple.so","cac055936358d0ea35021240a7a36a2110994f3c8883a47bb28fd2c634e6f069"},
    {"libmedialibrarycore.so","3c42bf5f52d90931d65e463f44720a28c60f575416a6420ad71682e1be91e6b7"},
    {"libmediaplatform.so","cb4ab591e0ea6c74cbcbe23b616566425172efc77f83933b6aa7a3ffcc88de6d"},
    {"libstoreservicescore.so","0ba169fec7b5a0f70113ab01ba6c60b893c36f9aa080797b13d3830dd3784010"},
    {"libxml2.so","41422a7384114f4a98fa7a09e019c9f2aec07c429d6d2ae4b63022b111325109"},
};
}
