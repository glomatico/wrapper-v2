#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <string>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int fail(const char* operation) {
    std::perror(operation);
    return 70;
}

bool drop_capabilities() {
    __user_cap_header_struct header{_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct empty[2]{};
    return syscall(SYS_capset, &header, empty) == 0;
}

int exit_code(int status) {
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int exec_worker(const std::string& root) {
    if (mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr))
        return fail("worker-launch: make mounts private");
    for (const char* name : {"/proc", "/dev"}) {
        if (mkdir((root + name).c_str(), 0755) && errno != EEXIST)
            return fail("worker-launch: create mount target");
    }
    if (mount("proc", (root + "/proc").c_str(), "proc",
              MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr) ||
        mount("/dev", (root + "/dev").c_str(), nullptr, MS_BIND | MS_REC, nullptr) ||
        chdir(root.c_str()) || chroot(".") || chdir("/"))
        return fail("worker-launch: set up rootfs");

    setenv("ANDROID_DNS_MODE", "local", 1);
    setenv("ANDROID_DATA", "/data", 1);
    setenv("ANDROID_ROOT", "/system", 1);
    setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 1);
    setenv("CURL_CA_BUNDLE", "/etc/ssl/certs/ca-certificates.crt", 1);
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    execl("/system/bin/apple-music-worker", "apple-music-worker", static_cast<char*>(nullptr));
    return fail("worker-launch: exec worker");
}

// PID 1 reaps adopted children. Its exit makes the kernel kill and reap the
// whole namespace, even if the Android linker never reaches the worker's main.
int namespace_init(const char* root, int launcher_pipe) {
    sigset_t children, previous;
    sigemptyset(&children);
    sigaddset(&children, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &children, &previous))
        return fail("worker-launch: block SIGCHLD");
    int signals = signalfd(-1, &children, SFD_CLOEXEC);
    if (signals < 0) return fail("worker-launch: signalfd");

    const pid_t worker = fork();
    if (worker < 0) return fail("worker-launch: fork worker");
    if (worker == 0) {
        close(signals);
        close(launcher_pipe);
        if (sigprocmask(SIG_SETMASK, &previous, nullptr))
            return fail("worker-launch: restore signal mask");
        return exec_worker(root);
    }
    if (!drop_capabilities()) return fail("worker-launch: drop init capabilities");
    close(STDIN_FILENO);

    // Watch hangup only. Rust owns the only reader of the response pipe,
    // and the outer launcher owns the only writer of launcher_pipe. Both
    // closures remain observable even if they happened before this poll.
    pollfd fds[] = {{STDOUT_FILENO, 0, 0}, {launcher_pipe, 0, 0}, {signals, POLLIN, 0}};
    for (;;) {
        if (poll(fds, 3, -1) < 0) {
            if (errno == EINTR) continue;
            return fail("worker-launch: poll");
        }
        if (fds[0].revents || fds[1].revents) return 0;
        signalfd_siginfo info;
        if (read(signals, &info, sizeof(info)) < 0) {
            if (errno == EINTR) continue;
            return fail("worker-launch: read SIGCHLD");
        }
        // Several exits may have coalesced into one SIGCHLD.
        for (;;) {
            int status;
            pid_t pid = waitpid(-1, &status, WNOHANG);
            if (pid == worker) return exit_code(status);
            if (pid == 0) break;
            if (pid < 0 && errno != EINTR) return fail("worker-launch: reap child");
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: worker-launch ROOTFS\n", stderr);
        return 64;
    }
    if (unshare(CLONE_NEWNS | CLONE_NEWPID)) return fail("worker-launch: unshare");
    int lifetime[2];
    if (pipe2(lifetime, O_CLOEXEC)) return fail("worker-launch: lifetime pipe");
    pid_t init = fork();
    if (init < 0) return fail("worker-launch: fork namespace init");
    if (init == 0) {
        close(lifetime[1]);
        _exit(namespace_init(argv[1], lifetime[0]));
    }
    close(lifetime[0]);
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    if (!drop_capabilities()) return fail("worker-launch: drop launcher capabilities");
    int status;
    while (waitpid(init, &status, 0) < 0) {
        if (errno != EINTR) return fail("worker-launch: wait for namespace init");
    }
    return exit_code(status);
}
