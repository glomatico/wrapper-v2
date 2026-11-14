#if !defined(__x86_64__) || !defined(__ANDROID__)
#error Only the pinned x86_64 Android capture ABI is implemented.
#endif
#include "capture.hpp"
#include "capture_offsets.hpp"
#include <dobby.h>
#include <link.h>
#include <fcntl.h>
#include <cstring>
namespace worker {
namespace {
constexpr uintptr_t entry=0x107070;
constexpr unsigned char prefix[]={0x44,0x88,0x8d,0x78,0xfd,0xff,0xff,0x4c,0x89,0xad,0x40,0xfd,0xff,0xff};
static_assert(sizeof(capture_offsets)/sizeof(*capture_offsets)==754,"Capture layout changed");
struct Image{uintptr_t base=0;bool executable=false;};
int locate(dl_phdr_info* info,size_t,void* out) {
    if(!info->dlpi_name)return 0;
    const char* name=std::strrchr(info->dlpi_name,'/');name=name?name+1:info->dlpi_name;
    if(std::strcmp(name,"libCoreLSKD.so"))return 0;
    auto& image=*static_cast<Image*>(out);image.base=info->dlpi_addr;
    for(unsigned i=0;i<info->dlpi_phnum;++i){const auto& p=info->dlpi_phdr[i];if(p.p_type==PT_LOAD&&(p.p_flags&PF_X)&&entry>=p.p_vaddr&&entry+sizeof prefix<=p.p_vaddr+p.p_filesz)image.executable=true;}
    return 1;
}
struct Window{Capture& output;int fd;bool done=false;};
thread_local Window* active=nullptr;
void callback(RegisterContext* registers,const HookEntryInfo*) {
    auto w=active;if(!w||w->done)return;
    const auto& r=registers->general.regs;
    if(r.rdi!=0x06c4f3f6||r.r13!=8578||r.rbp<0x800)return;
    uintptr_t table=0;std::array<unsigned char,0x800> stack{};
    bool ok=pread(w->fd,&table,sizeof table,r.rbp-0x90)==sizeof table&&
        table&&pread(w->fd,w->output.ctx.data(),w->output.ctx.size(),table)==ssize_t(w->output.ctx.size())&&
        pread(w->fd,stack.data(),stack.size(),r.rbp-stack.size())==ssize_t(stack.size());
    if(ok){for(auto offset:capture_offsets)w->output.state[offset]=stack[offset];w->output.bl=r.rbx&255;w->output.r9b=r.r9&255;w->done=true;}
    wipe(stack.data(),stack.size());
}
bool installed=false;
}
void install_capture() {
    if(installed)return;
    // Identity is also checked before loading the library. Verify again at hook time.
    if(sha256_file("/system/lib64/libCoreLSKD.so")!="bf813206d382bd8b27384bd546a5b9aa730df32c8543d709756b081b619a8b3a")throw Error("unsupported_library",false,true);
    Image image;dl_iterate_phdr(locate,&image);
    if(!image.base||!image.executable||std::memcmp(reinterpret_cast<void*>(image.base+entry),prefix,sizeof prefix))throw Error("unsupported_capture_hook",false,true);
    log_set_level(-1);
    if(DobbyInstrument(reinterpret_cast<void*>(image.base+entry),callback))throw Error("capture_hook_failed",false,true);
    installed=true;
}
void capture(void* engine,Decrypt decrypt,Capture& output) {
    if(!installed||!engine||!decrypt||active)throw Error("capture_unavailable",false,true);
    Fd memory(open("/proc/self/mem",O_RDONLY|O_CLOEXEC));
    if(memory.value<0)throw Error("capture_memory_unavailable",false,true);
    Window window{output,memory.value};
    struct Guard{~Guard(){active=nullptr;}}guard;
    active=&window;
    std::array<unsigned char,64> dummy{};
    auto status=decrypt(engine,5,dummy.data(),dummy.data(),dummy.size());
    wipe(dummy.data(),dummy.size());
    if(status<0||!window.done)throw Error("context_capture_failed",false,true);
}
}
