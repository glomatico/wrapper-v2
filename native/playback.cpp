#include "runtime.hpp"
#include <ctime>
namespace worker {
namespace {
Json plist(const void* p,unsigned depth,size_t& budget) {
    if(!p)return nullptr;
    if(depth>32||budget--==0)throw Error("invalid_playback_response");
    auto type=call<unsigned long>("CFGetTypeID",p);
    auto is=[&](const char* name){return type==call<unsigned long>(name);};
    if(is("CFStringGetTypeID")) {
        auto n=call<long>("CFStringGetLength",p);
        if(n<0||n>1024*1024)throw Error("invalid_playback_response");
        auto max=call<long>("CFStringGetMaximumSizeForEncoding",n,0x08000100ul)+1;
        if(max<=0||max>4*1024*1024)throw Error("invalid_playback_response");
        std::vector<char> buffer(size_t(max),0);
        if(!call<unsigned char>("CFStringGetCString",p,buffer.data(),max,0x08000100ul))throw Error("invalid_playback_response");
        return std::string(buffer.data());
    }
    if(is("CFDictionaryGetTypeID")) {
        auto n=call<long>("CFDictionaryGetCount",p);if(n<0||size_t(n)>budget)throw Error("invalid_playback_response");
        std::vector<const void*> keys(n),values(n);
        call<void>("CFDictionaryGetKeysAndValues",p,keys.data(),values.data());
        Json out=Json::object();
        for(long i=0;i<n;++i){auto key=plist(keys[i],depth+1,budget);if(!key.is_string())throw Error("invalid_playback_response");out[key.get<std::string>()]=plist(values[i],depth+1,budget);}
        return out;
    }
    if(is("CFArrayGetTypeID")) {
        auto n=call<long>("CFArrayGetCount",p);if(n<0||size_t(n)>budget)throw Error("invalid_playback_response");
        Json out=Json::array();for(long i=0;i<n;++i)out.push_back(plist(call<const void*>("CFArrayGetValueAtIndex",p,i),depth+1,budget));return out;
    }
    if(is("CFBooleanGetTypeID"))return call<unsigned char>("CFBooleanGetValue",p)!=0;
    if(is("CFNumberGetTypeID")) {
        if(call<unsigned char>("CFNumberIsFloatType",p)){double v=0;if(!call<unsigned char>("CFNumberGetValue",p,6l,&v))throw Error("invalid_playback_response");return v;}
        int64_t v=0;if(!call<unsigned char>("CFNumberGetValue",p,4l,&v))throw Error("invalid_playback_response");return v;
    }
    if(is("CFDataGetTypeID")) {
        auto n=call<long>("CFDataGetLength",p);if(n<0||n>1024*1024)throw Error("invalid_playback_response");
        return base64(call<const unsigned char*>("CFDataGetBytePtr",p),size_t(n));
    }
    if(is("CFDateGetTypeID")) {
        auto t=static_cast<time_t>(call<double>("CFDateGetAbsoluteTime",p)+978307200.0);
        std::tm tm{};char buffer[32]{};if(!gmtime_r(&t,&tm)||!std::strftime(buffer,sizeof buffer,"%Y-%m-%dT%H:%M:%SZ",&tm))throw Error("invalid_playback_response");return buffer;
    }
    if(is("CFNullGetTypeID"))return nullptr;
    throw Error("invalid_playback_response");
}
}
Json Runtime::playback(const std::string& adam) {
    validate_adam(adam);
    NativeObject purchase(8192,names::PurchaseRequest_dtor);
    call<void>(names::PurchaseRequest_ctor,purchase.data,&request);
    call<void>(names::PurchaseRequest_setProcessDialogActions,purchase.data,true);
    std::string bag="subDownload",buy="salableAdamId="+adam+"&price=0&pricingParameters=SUBS&productType=S";
    call<void>(names::PurchaseRequest_setURLBagKey,purchase.data,&bag);
    call<void>(names::PurchaseRequest_setBuyParameters,purchase.data,&buy);
    call<void>(names::PurchaseRequest_run,purchase.data);
    auto response=call<Shared*>(names::PurchaseRequest_response,purchase.data);
    if(!response||!*response)throw Error("playback_failed",true);
    auto error=call<Shared*>(names::PurchaseResponse_error,response->get());
    if(error&&*error)throw Error("playback_failed",true);
    std::vector<Shared> items;
    call<void>(names::PurchaseResponse_items,&items,response->get());
    if(items.empty()||items.size()>128)throw Error("playback_unavailable");
    Json songs=Json::array();size_t budget=65536;
    for(const auto& item:items) {
        if(!item)throw Error("invalid_playback_response");
        // Pinned PurchaseItem::dictionary returns this; its first member is CFDictionaryRef.
        const void* dictionary=nullptr;std::memcpy(&dictionary,item.get(),sizeof dictionary);
        songs.push_back(plist(dictionary,0,budget));
    }
    return {{"songList",std::move(songs)}};
}
}
