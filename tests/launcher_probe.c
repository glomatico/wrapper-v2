// A worker that can lose its IPC reader or parent at any point.
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char proc_pid[32];
    ssize_t n = readlink("/proc/self", proc_pid, sizeof(proc_pid) - 1);
    if (n < 0) return 1;
    proc_pid[n] = '\0';

    // Leave PID 1 one live orphan and one zombie to collect while we run.
    pid_t child = fork();
    if (child < 0) return 1;
    if (child == 0) {
        pid_t orphan = fork();
        if (orphan < 0) _exit(1);
        if (orphan == 0) for (;;) pause();
        pid_t zombie = fork();
        if (zombie < 0) _exit(1);
        if (zombie == 0) _exit(0);
        _exit(0);
    }
    int status;
    if (waitpid(child, &status, 0) != child || status != 0) return 1;
    printf("%d %d %s\n", getpid(), getppid(), proc_pid);
    fflush(stdout);
    switch (getchar()) {
        case 'e': return 37;
        case 's': raise(SIGTERM); return 1;
        default: for (;;) pause();
    }
}
