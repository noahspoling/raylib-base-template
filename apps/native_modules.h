#ifndef APPS_NATIVE_MODULES_H
#define APPS_NATIVE_MODULES_H

#include <stddef.h>
#include "app.h"

static const NativeModule GAME_NATIVE_MODULES[] = {
    { NULL, NULL },
};
#define GAME_NATIVE_MODULE_COUNT \
    ((int32_t)(sizeof(GAME_NATIVE_MODULES) / sizeof(GAME_NATIVE_MODULES[0])) - 1)

#endif
