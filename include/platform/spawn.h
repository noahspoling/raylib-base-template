#ifndef PLATFORM_SPAWN_H
#define PLATFORM_SPAWN_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ChildProcess {
    intptr_t handle;
    bool     running;
    int32_t  exit_code;
} ChildProcess;

bool ChildProcess_supported(void);

bool ChildProcess_spawn(ChildProcess *child, const char *exe, const char *const *argv);

bool ChildProcess_poll(ChildProcess *child);

#endif
