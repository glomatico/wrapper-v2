# API 22 uses unsorted find(1) output for the syscall archive. Keep the
# linked ARM64 syscall members in the order recorded by the SDK linker's
# STT_FILE symbols. The remaining members do not enter linker64.
sdk_syscalls := __rt_sigaction dup3 __getpid sigaltstack close mmap __accept4 \
    sched_setscheduler __openat __socket tgkill __connect __gettimeofday prctl \
    __clock_gettime pread64 _exit read fcntl __rt_sigprocmask mprotect munmap \
    getuid write __set_tid_address fstat64 writev ftruncate getrlimit lseek \
    __exit __pselect6 sendto __ppoll __ioctl
sdk_members := $(addprefix arch-arm64/syscalls/,$(addsuffix .S,$(sdk_syscalls)))
all-S-files-under = $(if $(filter arch-arm64/syscalls,$(1)),$(sdk_members)) \
    $(filter-out $(sdk_members),$(patsubst $(LOCAL_PATH)/%,%,$(sort $(wildcard $(LOCAL_PATH)/$(1)/*.S))))

include bionic/Android.mk
include external/compiler-rt/Android.mk
include external/jemalloc/Android.mk
include build/libs/host/Android.mk
include build/tools/acp/Android.mk
