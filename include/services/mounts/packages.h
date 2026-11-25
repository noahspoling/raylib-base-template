#ifndef SERVICES_PACKAGES_H
#define SERVICES_PACKAGES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_mode.h"
#include "services/mounts/mounts.h"

struct lua_State;

#define PACKAGES_MAX      64
#define PACKAGE_LIST_MAX  8
#define PACKAGE_NAME_MAX  64

typedef struct Package {
    char    id[MOUNTS_ID_MAX];
    char    name[PACKAGE_NAME_MAX];
    char    version[24];
    char    folder[PACKAGE_NAME_MAX];
    char    root[MOUNTS_PATH_MAX];
    char    depends[PACKAGE_LIST_MAX][MOUNTS_ID_MAX];
    char    native[PACKAGE_LIST_MAX][MOUNTS_ID_MAX];
    char    replaces[PACKAGE_LIST_MAX][MOUNTS_ID_MAX];
    int8_t  depends_count, native_count, replaces_count;
    char    home[APP_MODE_COUNT][PACKAGE_NAME_MAX];
    uint8_t apps;
    bool    builtin;
    bool    valid;
    char    error[160];
} Package;

typedef bool (*PackagesNativeCheck)(const char *module, void *user);

bool Packages_load_data(struct lua_State *L, const char *path, char *err, size_t errcap);

void Packages_scan(struct lua_State *L);

int32_t        Packages_count(void);
const Package *Packages_get(int32_t index);
const Package *Packages_find(const char *id);

bool Packages_check(const Package *p, AppMode mode, PackagesNativeCheck check,
                    void *user, char *reason, size_t cap);

bool Packages_build_stack(AppMode mode, const char *const *mods, int32_t mod_count,
                          PackagesNativeCheck check, void *user, char *err, size_t errcap);

int32_t        Packages_active_count(void);
const Package *Packages_active(int32_t index);

const char *Packages_home(AppMode mode);

#endif
