#include "platform/spawn.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

bool ChildProcess_supported(void) { return true; }

bool ChildProcess_spawn(ChildProcess *child, const char *exe, const char *const *argv) {
    memset(child, 0, sizeof(*child));
    char cmd[4096];
    size_t len = 0;
    for (int32_t i = 0; argv[i]; i++) {
        int32_t n = snprintf(cmd + len, sizeof(cmd) - len, "%s\"%s\"", i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(cmd) - len) return false;
        len += (size_t)n;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    child->handle = (intptr_t)pi.hProcess;
    child->running = true;
    return true;
}

bool ChildProcess_poll(ChildProcess *child) {
    if (!child->running) return false;
    HANDLE h = (HANDLE)child->handle;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(h, &code);
        CloseHandle(h);
        child->handle = 0;
        child->running = false;
        child->exit_code = (int32_t)code;
    }
    return child->running;
}

#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)

bool ChildProcess_supported(void) { return false; }

bool ChildProcess_spawn(ChildProcess *child, const char *exe, const char *const *argv) {
    (void)exe; (void)argv;
    memset(child, 0, sizeof(*child));
    return false;
}

bool ChildProcess_poll(ChildProcess *child) { return child->running = false; }

#else

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char **environ;

bool ChildProcess_supported(void) { return true; }

bool ChildProcess_spawn(ChildProcess *child, const char *exe, const char *const *argv) {
    memset(child, 0, sizeof(*child));
    pid_t pid = 0;
    if (posix_spawn(&pid, exe, NULL, NULL, (char *const *)argv, environ) != 0) return false;
    child->handle = (intptr_t)pid;
    child->running = true;
    return true;
}

bool ChildProcess_poll(ChildProcess *child) {
    if (!child->running) return false;
    int status = 0;
    pid_t r = waitpid((pid_t)child->handle, &status, WNOHANG);
    if (r == (pid_t)child->handle) {
        child->running = false;
        child->handle = 0;
        child->exit_code = WIFEXITED(status) ? (int32_t)WEXITSTATUS(status)
                         : WIFSIGNALED(status) ? -(int32_t)WTERMSIG(status) : -1;
    } else if (r < 0) {
        child->running = false;
        child->handle = 0;
        child->exit_code = -1;
    }
    return child->running;
}

#endif
