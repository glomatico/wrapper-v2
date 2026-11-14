#include "protocol.hpp"
#include <sys/socket.h>
#include <iostream>
#include <thread>
using namespace worker;
using namespace std::string_literals;
static void check(bool v){if(!v)throw std::runtime_error("test failed");}
template<class F>void rejects(F f,const char* code){try{f();throw std::runtime_error("accepted invalid input");}catch(const Error&e){check(std::string(e.code)==code);}}
int main(){
    check(sha256(reinterpret_cast<const unsigned char*>("abc"),3)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check(sha256(nullptr,0)=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::string million(1000000,'a');
    check(sha256(reinterpret_cast<const unsigned char*>(million.data()),million.size())=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    check(base64(reinterpret_cast<const unsigned char*>("f"),1)=="Zg==");
    check(base64(reinterpret_cast<const unsigned char*>("foo"),3)=="Zm9v");
    Json request={{"v",1},{"id","1"},{"op","initialize"},{"params",Json::object()}};
    Protocol::validate(request);
    auto bad=request;bad["v"]=2;rejects([&]{Protocol::validate(bad);},"unsupported_protocol");
    bad=request;bad["timeout_ms"]=true;rejects([&]{Protocol::validate(bad);},"invalid_params");
    rejects([]{validate_adam("1&price=0");},"invalid_params");
    rejects([]{validate_uri("https://other");},"invalid_params");
    rejects([]{validate_uri("skd://x\n");},"invalid_params");
    int fds[2];check(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);Fd a(fds[0]),b(fds[1]);
    Protocol sender(a.value,a.value),receiver(b.value,b.value);
    sender.write(request);Json received;check(receiver.read(received));check(received==request);
    // Fragmentation must not split an envelope.
    std::string wire="\0\0\0\2"s+"{}";
    std::thread fragmented([&]{for(char c:wire)check(write(a.value,&c,1)==1);});
    check(receiver.read(received));check(received.is_object());fragmented.join();
    unsigned char large[4]={0,32,0,0};check(write(a.value,large,4)==4);
    rejects([&]{receiver.read(received);},"invalid_frame");
    Json secret={{"a","private"},{"nested",Json::array({"password"})}};wipe_json(secret);
    check(secret["a"]==""&&secret["nested"][0]=="");
    std::cout<<"common tests passed\n";
}
