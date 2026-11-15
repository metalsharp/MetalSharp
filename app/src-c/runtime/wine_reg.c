#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include "metalsharp_backend/wine_reg.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool ms_wine_reg(const char* wine, const char* prefix, char* const reg_args[]) {
    char* argv[16];
    size_t n = 0;
    pid_t pid;
    int status = 0;
    time_t deadline;
    argv[n++] = (char*)wine;
    argv[n++] = "reg";
    for (size_t i = 0; reg_args[i] && n < sizeof(argv) / sizeof(argv[0]) - 1; i++)
        argv[n++] = reg_args[i];
    argv[n] = NULL;
    pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        setenv("WINEPREFIX", prefix, 1);
        setenv("WINEDEBUG", "-all", 1);
        unsetenv("WINEDLLOVERRIDES");
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);
        execv(wine, argv);
        _exit(127);
    }
    deadline = time(NULL) + 60;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0 && errno != EINTR)
            return false;
        if (time(NULL) >= deadline) {
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            return false;
        }
        usleep(50000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
