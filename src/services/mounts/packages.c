#include "services/mounts/packages.h"
#include "raylib.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <string.h>

static Package  g_packages[PACKAGES_MAX];
static int32_t  g_count;

static const Package *g_active[PACKAGES_MAX];
static int32_t        g_active_count;

static const char *const MODE_NAMES[APP_MODE_COUNT] = { "launcher", "game", "editor" };

const char *AppMode_name(AppMode mode) {
    return ((int)mode >= 0 && (int)mode < APP_MODE_COUNT) ? MODE_NAMES[mode] : "?";
}

bool AppMode_parse(const char *name, AppMode *out) {
    if (!name) return false;
    for (int32_t i = 0; i < APP_MODE_COUNT; i++) {
        if (strcmp(name, MODE_NAMES[i]) == 0) {
            if (out) *out = (AppMode)i;
            return true;
        }
    }
    return false;
}

bool Packages_load_data(lua_State *L, const char *path, char *err, size_t errcap) {
    char *text = LoadFileText(path);
    if (!text) {
        snprintf(err, errcap, "cannot read %s", path);
        return false;
    }
    char chunkname[MOUNTS_PATH_MAX + 2];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);
    int32_t rc = luaL_loadbufferx(L, text, strlen(text), chunkname, "t");
    UnloadFileText(text);
    if (rc != LUA_OK) {
        snprintf(err, errcap, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    lua_newtable(L);
    lua_setupvalue(L, -2, 1);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        snprintf(err, errcap, "%s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        snprintf(err, errcap, "%s must return a table", path);
        lua_pop(L, 1);
        return false;
    }
    return true;
}

static void read_string(lua_State *L, const char *key, char *out, size_t cap) {
    lua_getfield(L, -1, key);
    if (lua_type(L, -1) == LUA_TSTRING) snprintf(out, cap, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
}

static int8_t read_list(lua_State *L, const char *key, char out[][MOUNTS_ID_MAX]) {
    int8_t n = 0;
    lua_getfield(L, -1, key);
    if (lua_type(L, -1) == LUA_TSTRING) {
        snprintf(out[n++], MOUNTS_ID_MAX, "%s", lua_tostring(L, -1));
    } else if (lua_istable(L, -1)) {
        lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
        for (lua_Integer i = 1; i <= len && n < PACKAGE_LIST_MAX; i++) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TSTRING) snprintf(out[n++], MOUNTS_ID_MAX, "%s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return n;
}

static void read_manifest(lua_State *L, Package *p) {
    char path[MOUNTS_PATH_MAX];
    snprintf(path, sizeof(path), "%smod.lua", p->root);
    if (!Packages_load_data(L, path, p->error, sizeof(p->error))) return;

    read_string(L, "id", p->id, sizeof(p->id));
    read_string(L, "name", p->name, sizeof(p->name));
    read_string(L, "version", p->version, sizeof(p->version));
    p->depends_count  = read_list(L, "depends", p->depends);
    p->native_count   = read_list(L, "native", p->native);
    p->replaces_count = read_list(L, "replaces", p->replaces);

    char apps[PACKAGE_LIST_MAX][MOUNTS_ID_MAX];
    int8_t app_count = read_list(L, "apps", apps);
    if (app_count == 0) {
        p->apps = (uint8_t)((1u << APP_GAME) | (1u << APP_EDITOR));
    }
    for (int8_t i = 0; i < app_count; i++) {
        AppMode m;
        if (AppMode_parse(apps[i], &m)) p->apps |= (uint8_t)(1u << m);
        else TraceLog(LOG_WARNING, "PACKAGES: %s: unknown app '%s'", path, apps[i]);
    }

    lua_getfield(L, -1, "home");
    if (lua_istable(L, -1)) {
        for (int32_t m = 0; m < APP_MODE_COUNT; m++) {
            read_string(L, MODE_NAMES[m], p->home[m], sizeof(p->home[m]));
        }
    }
    lua_pop(L, 2);

    if (!p->id[0]) {
        snprintf(p->error, sizeof(p->error), "%s has no id", path);
        return;
    }
    if (!p->name[0]) snprintf(p->name, sizeof(p->name), "%s", p->folder);
    p->valid = true;
}

static Package *add_package(lua_State *L, const char *root_dir, const char *folder, bool builtin) {
    if (g_count >= PACKAGES_MAX) {
        TraceLog(LOG_WARNING, "PACKAGES: too many packages (%d), skipping '%s'", PACKAGES_MAX, folder);
        return NULL;
    }
    Package *p = &g_packages[g_count++];
    memset(p, 0, sizeof(*p));
    snprintf(p->folder, sizeof(p->folder), "%s", folder);
    snprintf(p->root, sizeof(p->root), "%s%s/", root_dir, folder);
    p->builtin = builtin;
    read_manifest(L, p);
    if (!p->valid) {
        TraceLog(LOG_WARNING, "PACKAGES: '%s' ignored: %s", folder, p->error);
        return p;
    }
    for (int32_t i = 0; i < g_count - 1; i++) {
        if (g_packages[i].valid && strcmp(g_packages[i].id, p->id) == 0) {
            snprintf(p->error, sizeof(p->error), "duplicate id %s (also in %s)", p->id, g_packages[i].root);
            p->valid = false;
            TraceLog(LOG_WARNING, "PACKAGES: '%s' ignored: %s", folder, p->error);
            break;
        }
    }
    return p;
}

static void scan_builtins(lua_State *L) {
    char path[MOUNTS_PATH_MAX], err[256];
    snprintf(path, sizeof(path), "%sindex.lua", Mounts_packages_dir());
    if (!Packages_load_data(L, path, err, sizeof(err))) {
        TraceLog(LOG_ERROR, "PACKAGES: %s", err);
        return;
    }
    lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
    for (lua_Integer i = 1; i <= len; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_type(L, -1) == LUA_TSTRING) {
            char folder[PACKAGE_NAME_MAX];
            snprintf(folder, sizeof(folder), "%s", lua_tostring(L, -1));
            lua_pop(L, 1);
            add_package(L, Mounts_packages_dir(), folder, true);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

static void scan_mods(lua_State *L) {
#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    const char *dir = Mounts_mods_dir();
    if (!dir[0] || !DirectoryExists(dir)) return;
    FilePathList list = LoadDirectoryFiles(dir);
    for (unsigned int i = 0; i < list.count; i++) {
        const char *entry = list.paths[i];
        if (!DirectoryExists(entry)) continue;
        char manifest[MOUNTS_PATH_MAX];
        snprintf(manifest, sizeof(manifest), "%s/mod.lua", entry);
        if (!FileExists(manifest)) continue;
        add_package(L, dir, GetFileName(entry), false);
    }
    UnloadDirectoryFiles(list);
#else
    (void)L;
#endif
}

void Packages_scan(lua_State *L) {
    g_count = 0;
    g_active_count = 0;
    scan_builtins(L);
    scan_mods(L);
    TraceLog(LOG_INFO, "PACKAGES: found %d (%s)", g_count, Mounts_mods_dir()[0] ? Mounts_mods_dir() : "no mods folder");
}

int32_t Packages_count(void) { return g_count; }

const Package *Packages_get(int32_t index) {
    return (index >= 0 && index < g_count) ? &g_packages[index] : NULL;
}

const Package *Packages_find(const char *id) {
    if (!id) return NULL;
    for (int32_t i = 0; i < g_count; i++) {
        if (g_packages[i].valid && strcmp(g_packages[i].id, id) == 0) return &g_packages[i];
    }
    return NULL;
}

bool Packages_check(const Package *p, AppMode mode, PackagesNativeCheck check,
                    void *user, char *reason, size_t cap) {
    if (!p->valid) { snprintf(reason, cap, "%s", p->error); return false; }
    if (!(p->apps & (1u << mode))) {
        snprintf(reason, cap, "not used by the %s", AppMode_name(mode));
        return false;
    }
    for (int8_t i = 0; i < p->depends_count; i++) {
        if (!Packages_find(p->depends[i])) {
            snprintf(reason, cap, "needs %s (not installed)", p->depends[i]);
            return false;
        }
    }
    for (int8_t i = 0; i < p->native_count; i++) {
        if (check && !check(p->native[i], user)) {
            snprintf(reason, cap, "needs C module %s (not in this build)", p->native[i]);
            return false;
        }
    }
    reason[0] = '\0';
    return true;
}

typedef struct {
    const Package *list[PACKAGES_MAX];
    bool           keep[PACKAGES_MAX];
    int32_t        count;
} Wanted;

static int32_t wanted_index(const Wanted *w, const char *id) {
    for (int32_t i = 0; i < w->count; i++) {
        if (w->keep[i] && strcmp(w->list[i]->id, id) == 0) return i;
    }
    return -1;
}

static void problem(char *err, size_t errcap, const char *fmt, const char *a, const char *b) {
    char msg[256];
    snprintf(msg, sizeof(msg), fmt, a, b);
    TraceLog(LOG_WARNING, "PACKAGES: %s", msg);
    if (err && errcap && !err[0]) snprintf(err, errcap, "%s", msg);
}

static void wanted_add(Wanted *w, const Package *p) {
    if (w->count >= PACKAGES_MAX || wanted_index(w, p->id) >= 0) return;
    w->list[w->count] = p;
    w->keep[w->count] = true;
    w->count++;
}

static bool emit(const Wanted *w, int32_t i, uint8_t *state, char *err, size_t errcap) {
    if (state[i] == 2) return true;
    if (state[i] == 1) {
        problem(err, errcap, "dependency cycle through %s%s", w->list[i]->id, "");
        return false;
    }
    state[i] = 1;
    const Package *p = w->list[i];
    for (int8_t d = 0; d < p->depends_count; d++) {
        int32_t j = wanted_index(w, p->depends[d]);
        if (j >= 0 && !emit(w, j, state, err, errcap)) return false;
    }
    state[i] = 2;
    if (g_active_count < PACKAGES_MAX) g_active[g_active_count++] = p;
    return true;
}

bool Packages_build_stack(AppMode mode, const char *const *mods, int32_t mod_count,
                          PackagesNativeCheck check, void *user, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    Wanted w = { 0 };
    uint8_t mode_bit = (uint8_t)(1u << mode);

    for (int32_t i = 0; i < g_count; i++) {
        const Package *p = &g_packages[i];
        if (!p->builtin || !p->valid || !(p->apps & mode_bit)) continue;
        wanted_add(&w, p);
    }
    for (int32_t i = 0; i < mod_count; i++) {
        const Package *p = Packages_find(mods[i]);
        if (!p) { problem(err, errcap, "mod %s is not installed%s", mods[i], ""); continue; }
        if (!(p->apps & mode_bit)) continue;
        wanted_add(&w, p);
    }

    for (int32_t i = 0; i < w.count; i++) {
        const Package *p = w.list[i];
        for (int8_t r = 0; r < p->replaces_count; r++) {
            int32_t j = wanted_index(&w, p->replaces[r]);
            if (j >= 0 && j != i) {
                w.keep[j] = false;
                TraceLog(LOG_INFO, "PACKAGES: %s replaces %s", p->id, p->replaces[r]);
            }
        }
    }

    for (bool changed = true; changed;) {
        changed = false;
        for (int32_t i = 0; i < w.count; i++) {
            if (!w.keep[i]) continue;
            const Package *p = w.list[i];
            for (int8_t n = 0; n < p->native_count && w.keep[i]; n++) {
                if (check && !check(p->native[n], user)) {
                    problem(err, errcap, "%s skipped: C module %s is not in this build", p->id, p->native[n]);
                    w.keep[i] = false;
                    changed = true;
                }
            }
            for (int8_t d = 0; d < p->depends_count && w.keep[i]; d++) {
                if (wanted_index(&w, p->depends[d]) < 0) {
                    problem(err, errcap, "%s skipped: needs %s", p->id, p->depends[d]);
                    w.keep[i] = false;
                    changed = true;
                }
            }
        }
    }

    g_active_count = 0;
    uint8_t state[PACKAGES_MAX] = { 0 };
    for (int32_t i = 0; i < w.count; i++) {
        if (w.keep[i] && !emit(&w, i, state, err, errcap)) break;
    }

    Mounts_clear();
    for (int32_t i = 0; i < g_active_count; i++) {
        Mounts_push(g_active[i]->id, g_active[i]->root);
    }
    TraceLog(LOG_INFO, "PACKAGES: stack for %s", AppMode_name(mode));
    Mounts_log();

    if (g_active_count == 0) {
        if (err && errcap && !err[0]) snprintf(err, errcap, "no packages could be mounted for %s", AppMode_name(mode));
        return false;
    }
    return true;
}

int32_t Packages_active_count(void) { return g_active_count; }

const Package *Packages_active(int32_t index) {
    return (index >= 0 && index < g_active_count) ? g_active[index] : NULL;
}

const char *Packages_home(AppMode mode) {
    for (int32_t i = g_active_count - 1; i >= 0; i--) {
        if (g_active[i]->home[mode][0]) return g_active[i]->home[mode];
    }
    return NULL;
}
