#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>
#include "app_mode.h"

typedef struct lua_State lua_State;
typedef int (*AppLuaOpen)(lua_State *L);

typedef struct NativeModule {
    const char *name;
    AppLuaOpen  open;
} NativeModule;

typedef struct AppConfig {
    AppMode             mode;
    bool                single;
    const char         *start_scene;
    int32_t             argc;
    char              **argv;
    const NativeModule *native;
    int32_t             native_count;
} AppConfig;

int gramarye_app_run(const AppConfig *cfg);

#endif
