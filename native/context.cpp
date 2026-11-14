#include "runtime.hpp"
#include "capture.hpp"
#include "fps_certificate.hpp"
namespace worker {
Json Runtime::context(const std::string& adam,const std::string& uri) {
    validate_adam(adam);validate_uri(uri);
    struct Reset{void* foothill;~Reset(){call<void>(names::SVFootHillSessionCtrl_resetAllContexts,foothill);}} reset{foothill};
    std::string format="com.apple.streamingkeydelivery",version="1";
    std::string server="https://play.itunes.apple.com/WebObjects/MZPlay.woa/music/fps",protocol="simplified",certificate=kFpsCert;
    Shared key;
    call<void>(names::SVFootHillSessionCtrl_getPersistentKey_7str,&key,foothill,&adam,&uri,&format,&version,&server,&protocol,&certificate);
    if(!key)throw Error("context_acquisition_failed",true);
    Shared context;int type=0;bool flag=false;
    // The pinned function ignores type/flag, but supply the full declared ABI.
    call<void>(names::SVFootHillSessionCtrl_decryptContext,&context,foothill,key.get(),&type,&flag);
    if(!context)throw Error("context_acquisition_failed",true);
    auto handle=call<void**>(names::SVFootHillPContext_kdContext,context.get());
    if(!handle||!*handle)throw Error("context_acquisition_failed",true);
    void* engine=nullptr;std::memcpy(&engine,*handle,sizeof engine);
    Capture captured;capture(engine,symbol<Decrypt>(names::fp_sample_decrypt),captured);
    return {{"context",captured.json()}};
}
}
