#include "protocol.hpp"
#include <sys/socket.h>
#include <sys/time.h>
#include <iostream>
#include <thread>
#include <chrono>
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
    const unsigned char malformed[]={0,0,0,1,'{'};
    check(write(a.value,malformed,sizeof malformed)==sizeof malformed);
    rejects([&]{receiver.read(received);},"invalid_json");
    // Fragmentation must not split an envelope.
    std::string wire="\0\0\0\2"s+"{}";
    std::thread fragmented([&]{for(char c:wire)check(write(a.value,&c,1)==1);});
    check(receiver.read(received));check(received.is_object());fragmented.join();
    // Authentication continuations are correlated to both request and challenge.
    sender.write({{"v",1},{"id","login"},{"op","auth.respond"},{"params",{{"challenge_id","c1"},{"code","123456"}}}});
    check(receiver.challenge("login","c1")=="123456");
    check(sender.read(received)&&received["event"]=="auth.challenge");
    sender.write({{"v",1},{"id","wrong"},{"op","auth.respond"},{"params",{{"challenge_id","c1"},{"code","123456"}}}});
    rejects([&]{receiver.challenge("login","c1");},"invalid_auth_continuation");
    check(sender.read(received));
    sender.write({{"v",1},{"id","login"},{"op","auth.respond"},{"params",{{"challenge_id","c1"},{"code",""}}}});
    check(receiver.challenge("login","c1").empty());check(sender.read(received));
    sender.write({{"v",1},{"id","login"},{"op","auth.respond"},{"params",{{"challenge_id","c1"},{"code","12a456"}}}});
    rejects([&]{receiver.challenge("login","c1");},"invalid_auth_code");check(sender.read(received));
    // The native request timer must pause while a human answers 2FA, then
    // resume after the continuation (instead of killing the waiting worker).
    itimerval deadline{};deadline.it_value.tv_sec=1;
    check(setitimer(ITIMER_REAL,&deadline,nullptr)==0);
    std::thread delayed([&]{
        Json challenge;
        check(sender.read(challenge)&&challenge["event"]=="auth.challenge");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        sender.write({{"v",1},{"id","login"},{"op","auth.respond"},
                      {"params",{{"challenge_id","delayed"},{"code","123456"}}}});
    });
    check(receiver.challenge("login","delayed")=="123456");
    itimerval resumed{};
    check(getitimer(ITIMER_REAL,&resumed)==0);
    check(resumed.it_value.tv_sec>0||resumed.it_value.tv_usec>0);
    itimerval disabled{};check(setitimer(ITIMER_REAL,&disabled,nullptr)==0);
    delayed.join();
    // Reject excessive nesting before exhausting the native stack.
    std::string nested(70,'[');nested+="0";nested+=std::string(70,']');
    unsigned char depth_header[4]={0,0,0,static_cast<unsigned char>(nested.size())};
    check(write(a.value,depth_header,4)==4);
    check(write(a.value,nested.data(),nested.size())==ssize_t(nested.size()));
    rejects([&]{receiver.read(received);},"invalid_json");
    unsigned char large[4]={0,32,0,0};check(write(a.value,large,4)==4);
    rejects([&]{receiver.read(received);},"invalid_frame");
    const unsigned char truncated[]={0,0,0,10,'{','}'};
    check(write(a.value,truncated,sizeof truncated)==sizeof truncated);
    check(shutdown(a.value,SHUT_WR)==0);
    rejects([&]{receiver.read(received);},"transport_closed");
    Json secret={{"a","private"},{"nested",Json::array({"password"})}};wipe_json(secret);
    check(secret["a"]==""&&secret["nested"][0]=="");
    std::cout<<"common tests passed\n";
}
