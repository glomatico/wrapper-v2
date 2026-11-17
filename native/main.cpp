#include "runtime.hpp"
#include "capture.hpp"
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/prctl.h>
namespace worker {
void deadline(int){_exit(124);}
void arm(unsigned ms){itimerval timer{};timer.it_value.tv_sec=ms/1000;timer.it_value.tv_usec=(ms%1000)*1000;setitimer(ITIMER_REAL,&timer,nullptr);}
}
int main() {
    using namespace worker;
    umask(0077);
    rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);
    if(prctl(PR_SET_PDEATHSIG,SIGKILL))return 70;
    signal(SIGPIPE,SIG_IGN);signal(SIGALRM,deadline);
    Fd output(dup(STDOUT_FILENO));
    if(output.value<0)return 70;
    fcntl(output.value,F_SETFD,FD_CLOEXEC);
    Fd null(open("/dev/null",O_WRONLY|O_CLOEXEC));
    if(null.value<0)return 70;
    dup2(null.value,STDOUT_FILENO);dup2(null.value,STDERR_FILENO);
    Protocol protocol(STDIN_FILENO,output.value);
    // Runtime globals and Apple background threads have process lifetime.
    // _exit prevents static teardown/dlclose races on every termination path.
    auto runtime=new Runtime;auto session=new Session(*runtime,protocol);
    unsigned operations=0;
    for(;;) {
        Json request;JsonGuard guard{request};std::string id,op;bool stop=false;unsigned exit_code=0;
        try {
            // Bound partial frames as well as native calls.
            arm(180000);
            if(!protocol.read(request))_exit(0);
            arm(0);
            if(request.is_object()&&request.contains("id")&&request["id"].is_string()&&request["id"].get_ref<const std::string&>().size()<=64)id=request["id"];
            Protocol::validate(request);
            arm(request.value("timeout_ms",120000u));
            op=request["op"];
            const auto& params=request["params"];
            Json result;JsonGuard result_guard{result};
            if(op=="shutdown"||op=="session.close"){result={{"state","closed"}};stop=true;}
            else if(op=="initialize") {
                runtime->initialize(params);session->attach();install_capture();
                result={{"protocol",1},{"profile",profile},{"architecture",architecture},
                        {"context_supported",context_supported},{"session",session->status()}};
            } else {
                if(!runtime->initialized)throw Error("not_initialized");
                if(op=="session.status")result=session->status();
                else if(op=="session.credentials")result=session->export_credentials();
                else if(op=="session.restore")result=session->restore();
                else if(op=="session.login")result=session->login(params,id);
                else if(op=="playback.get"||op=="context.get") {
                    session->require();
                    if(operations>=runtime->request_budget)throw Error("worker_recycle",true,true);
                    auto adam=string_param(params,"adam_id",20);validate_adam(adam);
                    if(op=="playback.get"){++operations;result=runtime->playback(adam);}
                    else{auto uri=string_param(params,"key_uri",4096);validate_uri(uri);++operations;result=runtime->context(adam,uri);}
                    session->require();
                } else throw Error("unknown_operation");
            }
            result["operations"]=operations;
            Json response={{"v",1},{"id",id},{"ok",true},{"result",std::move(result)}};JsonGuard response_guard{response};
            protocol.write(response);arm(0);
        } catch(const Error& e) {
            if(std::string(e.code)=="transport_closed"||std::string(e.code)=="invalid_frame")_exit(65);
            try{
                Json error={{"code",e.code},{"retryable",e.retryable},{"restart_required",e.fatal}};
                if(op=="session.login"&&std::string(e.code)=="authentication_failed")
                    error["auth"]=session->authentication_diagnostic();
                protocol.write({{"v",1},{"id",id},{"ok",false},{"error",std::move(error)}});
            }catch(...){_exit(65);}
            arm(0);stop=e.fatal;exit_code=70;
        } catch(...) {
            try{protocol.write({{"v",1},{"id",id},{"ok",false},{"error",{{"code","internal_error"},{"retryable",false},{"restart_required",true}}}});}catch(...){}
            _exit(70);
        }
        if(stop)_exit(exit_code);
    }
}
