#include <dlfcn.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <time.h>

/* Inspect the ELF supplied by this kernel, independently of Bionic's DSO list. */
static int has_gnu_only_vdso(void) {
    const uintptr_t base = getauxval(AT_SYSINFO_EHDR);
    if (!base) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)base;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(base + eh->e_phoff);
    for (size_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn *dyn = (const Elf64_Dyn *)(base + ph[i].p_offset);
        int gnu = 0, sysv = 0;
        for (size_t j = 0; j < ph[i].p_filesz / sizeof(*dyn) && dyn[j].d_tag; ++j) {
            gnu |= dyn[j].d_tag == DT_GNU_HASH;
            sysv |= dyn[j].d_tag == DT_HASH;
        }
        return gnu && !sysv;
    }
    return 0;
}

int main(void) {
    if (!has_gnu_only_vdso()) {
        fputs("SKIP: requires ARM64 Linux with a GNU-hash-only vDSO\n", stderr);
        return 77;
    }

    void *fixture = dlopen("libwrapper_fixture.so", RTLD_NOW | RTLD_GLOBAL);
    if (!fixture) goto fail;
    int (*direct)(void) = dlsym(fixture, "wrapper_linker_fixture_value");
    if (!direct || direct() != 42) goto fail;

    puts("GNU-only vDSO, dlopen, constructor and handle lookup passed; testing global lookup");
    fflush(stdout);
    int (*global)(void) = dlsym(RTLD_DEFAULT, "wrapper_linker_fixture_value");
    if (global != direct || global() != 42) goto fail;
    dlerror();
    if (dlsym(RTLD_DEFAULT, "wrapper_fixture_missing") != NULL || dlerror() == NULL) goto fail;

    void *math = dlopen("libm.so", RTLD_NOW | RTLD_LOCAL);
    if (!math) goto fail;
    double (*cosine)(double) = dlsym(math, "cos");
    if (!cosine || cosine(0.0) != 1.0) goto fail;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || !has_gnu_only_vdso()) goto fail;
    if (dlclose(math) || dlclose(fixture)) goto fail;
    puts("PASS: global and missing symbols, pinned libm, libc clock and dlclose");
    return 0;

fail: {
    const char *message = dlerror();
    fprintf(stderr, "FAIL: %s\n", message ? message : "unexpected result");
    return 1;
}
}
