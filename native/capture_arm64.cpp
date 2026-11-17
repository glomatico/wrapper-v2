#include "capture.hpp"
#include "capture_arm64_offsets.hpp"
#include <dobby.h>
#include <link.h>
#include <fcntl.h>
#include <cstring>
#include <limits>
#if !defined(__aarch64__) || !defined(__ANDROID__)
#error This implementation requires Android AArch64.
#endif
namespace worker {
namespace {
constexpr uintptr_t entry=0x105928;
constexpr unsigned char prefix[]={0xe8,0xe3,0x4f,0xf9,0xe3,0x73,0x53,0xf9};
constexpr size_t stack_start=0x1e00,state_size=0x900,table_size=0x2520;
constexpr uint64_t table_virtual=0x20000000,input_virtual=0x40000000,output_virtual=0x50000000;
constexpr std::pair<size_t,uint64_t> pointers[]={
    {0x26e0,0},{0x2680,0x110},{0x2678,0x530},{0x2670,0xa70},
    {0x2668,0xe90},{0x2660,0x13b0},{0x2650,0x17c0},{0x2648,0x1be0},
    {0x2640,0x2010},{0x2368,0x12a0},{0x2360,0x2420},{0x2358,0xa0d}};
static_assert(sizeof(arm64_live_offsets)/sizeof(*arm64_live_offsets)==1128,"ARM64 capture layout changed");
struct Capture {
    std::array<unsigned char,table_size> ctx{};
    std::array<unsigned char,state_size> state{};
    ~Capture(){wipe(ctx.data(),ctx.size());wipe(state.data(),state.size());}
    Json json()const {
        return {{"profile",context_profile},
            {"ctx",base64(ctx.data(),ctx.size())},{"state",base64(state.data(),state.size())}};
    }
};
struct Image {uintptr_t base=0;bool executable=false;};
int locate(dl_phdr_info* info,size_t,void* opaque) {
    if(!info->dlpi_name)return 0;
    const char* name=std::strrchr(info->dlpi_name,'/');name=name?name+1:info->dlpi_name;
    if(std::strcmp(name,"libCoreLSKD.so"))return 0;
    auto& image=*static_cast<Image*>(opaque);image.base=info->dlpi_addr;
    for(unsigned i=0;i<info->dlpi_phnum;++i) {
        const auto& p=info->dlpi_phdr[i];
        if(p.p_type==PT_LOAD&&(p.p_flags&PF_X)&&entry>=p.p_vaddr&&entry+sizeof prefix<=p.p_vaddr+p.p_filesz)
            image.executable=true;
    }
    return 1;
}
struct Window {Capture& result;int fd;uintptr_t sample;bool done=false;bool failed=false;};
thread_local Window* active=nullptr;
bool installed=false;
void callback(RegisterContext* registers,const HookEntryInfo*) noexcept {
    auto* w=active;
    if(!w||w->done||w->failed)return;
    if(registers->general.regs.x21!=0x6dc490)return;
    std::array<unsigned char,state_size> stack{};
    struct Erase {decltype(stack)& data;~Erase(){wipe(data.data(),data.size());}} erase{stack};
    if(registers->sp>std::numeric_limits<uintptr_t>::max()-stack_start-state_size ||
       pread(w->fd,stack.data(),stack.size(),registers->sp+stack_start)!=ssize_t(stack.size())) {
        w->failed=true;return;
    }
    auto word=[&](size_t slot){uint64_t value=0;std::memcpy(&value,stack.data()+slot-stack_start,8);return value;};
    uint32_t index=0,length=0;
    std::memcpy(&index,stack.data()+0x2688-stack_start,4);
    std::memcpy(&length,stack.data()+0x21d0-stack_start,4);
    const uint64_t table=word(0x26e0);
    bool valid=index==8360&&length==16&&word(0x1e28)==w->sample&&word(0x1e30)==w->sample&&
        table&&table<=std::numeric_limits<uintptr_t>::max()-table_size;
    for(auto [slot,offset]:pointers)valid=valid&&word(slot)==table+offset;
    if(!valid||pread(w->fd,w->result.ctx.data(),table_size,table)!=ssize_t(table_size)) {
        w->failed=true;return;
    }
    for(auto offset:arm64_live_offsets)w->result.state[offset-stack_start]=stack[offset-stack_start];
    for(auto [slot,offset]:pointers) {
        uint64_t value=table_virtual+offset;
        std::memcpy(w->result.state.data()+slot-stack_start,&value,8);
    }
    std::memcpy(w->result.state.data()+0x1e28-stack_start,&input_virtual,8);
    std::memcpy(w->result.state.data()+0x1e30-stack_start,&output_virtual,8);
    w->done=true;
}
}
void install_capture() {
    if(installed)return;
    if(sha256_file("/system/lib64/libCoreLSKD.so")!=
       "36e49510c1c3b0c133b263cfc6996f79cda112320b51d5f9dbaa1175cab6d8d9")
        throw Error("unsupported_library",false,true);
    Image image;dl_iterate_phdr(locate,&image);
    auto* target=reinterpret_cast<unsigned char*>(image.base+entry);
    if(!image.base||!image.executable||std::memcmp(target,prefix,sizeof prefix))
        throw Error("unsupported_capture_hook",false,true);
    // Dobby reserves x17. Relocating just the first instruction is essential:
    // native code reloads x17 at entry+8. A long trampoline corrupts live x17.
    // No incoming NZCV/vector/LR value is live at this verified block boundary.
    log_set_level(-1);dobby_enable_near_branch_trampoline();
    if(DobbyInstrument(target,callback))throw Error("capture_hook_failed",false,true);
    uint32_t branch=0;std::memcpy(&branch,target,4);
    if((branch&0xfc000000)!=0x14000000||std::memcmp(target+4,prefix+4,4))
        throw Error("unsupported_capture_trampoline",false,true);
    installed=true;
}
Json capture(void* engine,Decrypt decrypt) {
    if(!installed||!engine||!decrypt||active)throw Error("capture_unavailable",false,true);
    Fd memory(open("/proc/self/mem",O_RDONLY|O_CLOEXEC));
    if(memory.value<0)throw Error("capture_memory_unavailable",false,true);
    Capture result;
    std::array<unsigned char,16> dummy{};
    Window window{result,memory.value,reinterpret_cast<uintptr_t>(dummy.data())};
    struct Guard {decltype(dummy)& bytes;~Guard(){active=nullptr;wipe(bytes.data(),bytes.size());}} guard{dummy};
    active=&window;
    const auto status=decrypt(engine,5,dummy.data(),dummy.data(),dummy.size());
    if(status<0||window.failed||!window.done)throw Error("context_capture_failed",false,true);
    return result.json();
}
}
