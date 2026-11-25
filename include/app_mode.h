#ifndef APP_MODE_H
#define APP_MODE_H

#include <stdbool.h>

typedef enum AppMode {
    APP_LAUNCHER = 0,
    APP_GAME,
    APP_EDITOR,
    APP_MODE_COUNT
} AppMode;

const char *AppMode_name(AppMode mode);
bool        AppMode_parse(const char *name, AppMode *out);

#endif
