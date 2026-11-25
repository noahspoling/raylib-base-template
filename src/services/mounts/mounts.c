#include "services/mounts/mounts.h"
#include "raylib.h"

#include <stdio.h>
#include <string.h>

static Mount   g_mounts[MOUNTS_MAX];
static int32_t g_count;

static char g_packages_dir[MOUNTS_PATH_MAX];
static char g_mods_dir[MOUNTS_PATH_MAX];
static char g_user_dir[MOUNTS_PATH_MAX];

void Mounts_init(void) {
    g_count = 0;
#if defined(GRAMARYE_SOURCE_DIR)
    snprintf(g_packages_dir, sizeof(g_packages_dir), "%spackages/", GRAMARYE_SOURCE_DIR);
    snprintf(g_mods_dir,     sizeof(g_mods_dir),     "%smods/",     GRAMARYE_SOURCE_DIR);
    snprintf(g_user_dir,     sizeof(g_user_dir),     "%suser/",     GRAMARYE_SOURCE_DIR);
#elif defined(__ANDROID__)
    g_packages_dir[0] = '\0';
    g_mods_dir[0]     = '\0';
    snprintf(g_user_dir, sizeof(g_user_dir), "user/");
#elif defined(__EMSCRIPTEN__)
    snprintf(g_packages_dir, sizeof(g_packages_dir), "packages/");
    g_mods_dir[0] = '\0';
    snprintf(g_user_dir, sizeof(g_user_dir), "user/");
#else
    const char *app = GetApplicationDirectory();
    snprintf(g_packages_dir, sizeof(g_packages_dir), "%spackages/", app);
    snprintf(g_mods_dir,     sizeof(g_mods_dir),     "%smods/",     app);
    snprintf(g_user_dir,     sizeof(g_user_dir),     "%suser/",     app);
#endif

#if !defined(__ANDROID__)
    if (!DirectoryExists(g_user_dir)) MakeDirectory(g_user_dir);
#endif
    TraceLog(LOG_INFO, "MOUNTS: packages=%s mods=%s user=%s",
             g_packages_dir[0] ? g_packages_dir : "(apk root)",
             g_mods_dir[0] ? g_mods_dir : "(none)", g_user_dir);
}

const char *Mounts_packages_dir(void) { return g_packages_dir; }
const char *Mounts_mods_dir(void)     { return g_mods_dir; }
const char *Mounts_user_dir(void)     { return g_user_dir; }

void Mounts_clear(void) { g_count = 0; }

bool Mounts_push(const char *id, const char *root) {
    if (!id || !root) return false;
    if (g_count >= MOUNTS_MAX) {
        TraceLog(LOG_WARNING, "MOUNTS: stack full (%d), dropping '%s'", MOUNTS_MAX, id);
        return false;
    }
    Mount *m = &g_mounts[g_count++];
    snprintf(m->id, sizeof(m->id), "%s", id);
    size_t n = strlen(root);
    bool needs_slash = n > 0 && root[n - 1] != '/' && root[n - 1] != '\\';
    snprintf(m->root, sizeof(m->root), "%s%s", root, needs_slash ? "/" : "");
    return true;
}

int32_t Mounts_count(void) { return g_count; }

const Mount *Mounts_get(int32_t index) {
    return (index >= 0 && index < g_count) ? &g_mounts[index] : NULL;
}

bool Mounts_file_exists(const char *path) {
#if defined(__ANDROID__)
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
#else
    return FileExists(path);
#endif
}

int32_t Mounts_find_below(int32_t below, const char *rel, char *out, size_t cap) {
    if (!rel || !out || cap == 0) return -1;
    if (below > g_count) below = g_count;
    for (int32_t i = below - 1; i >= 0; i--) {
        int32_t n = snprintf(out, cap, "%s%s", g_mounts[i].root, rel);
        if (n < 0 || (size_t)n >= cap) continue;
        if (Mounts_file_exists(out)) return i;
    }
    out[0] = '\0';
    return -1;
}

int32_t Mounts_find(const char *rel, char *out, size_t cap) {
    return Mounts_find_below(g_count, rel, out, cap);
}

int32_t Mounts_resolve(const char *rel, char *out, size_t cap) {
    int32_t idx = Mounts_find(rel, out, cap);
    if (idx >= 0) return idx;
    TraceLog(LOG_WARNING, "MOUNTS: '%s' not found in any mount. Tried:", rel ? rel : "(null)");
    for (int32_t i = g_count - 1; i >= 0; i--) {
        TraceLog(LOG_WARNING, "MOUNTS:   [%d] %s%s", i, g_mounts[i].root, rel ? rel : "");
    }
    if (g_count == 0) TraceLog(LOG_WARNING, "MOUNTS:   (stack is empty)");
    return -1;
}

int32_t Mounts_index_of_path(const char *full_path) {
    if (!full_path) return -1;
    int32_t best = -1;
    size_t best_len = 0;
    for (int32_t i = 0; i < g_count; i++) {
        size_t n = strlen(g_mounts[i].root);
        if (strncmp(full_path, g_mounts[i].root, n) == 0 && (best < 0 || n >= best_len)) {
            best = i;
            best_len = n;
        }
    }
    return best;
}

void Mounts_log(void) {
    TraceLog(LOG_INFO, "MOUNTS: %d mounted (highest priority first)", g_count);
    for (int32_t i = g_count - 1; i >= 0; i--) {
        TraceLog(LOG_INFO, "MOUNT [%d] %s -> %s", i, g_mounts[i].id,
                 g_mounts[i].root[0] ? g_mounts[i].root : "(apk root)");
    }
}
