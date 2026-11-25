#include "dev/file_watch.h"
#include "script_host.h"
#include "services/mounts/mounts.h"
#include "raylib.h"

#include <stdio.h>
#include <string.h>

#define FILE_WATCH_INTERVAL 0.5f

static float g_elapsed;

static int32_t redo_lookup(const ScriptHostLoadedFile *f, char *out, size_t cap) {
    if (f->below == INT32_MAX) return Mounts_find(f->rel, out, cap);
    return Mounts_find_below(f->below, f->rel, out, cap);
}

void FileWatch_update(ScriptHost *host, float dt) {
    g_elapsed += dt;
    if (g_elapsed < FILE_WATCH_INTERVAL) return;
    g_elapsed = 0.0f;

    int32_t n = ScriptHost_loaded_count(host);
    for (int32_t i = 0; i < n; i++) {
        const ScriptHostLoadedFile *f = ScriptHost_loaded_get(host, i);
        char now[MOUNTS_PATH_MAX];
        int32_t mount = redo_lookup(f, now, sizeof(now));

        const char *why = NULL;
        if (mount < 0)                          why = "was removed";
        else if (mount != f->mount)             why = "is now provided by another mount";
        else if (GetFileModTime(now) != f->mtime) why = "changed";
        if (!why) continue;

        const Mount *m = Mounts_get(mount);
        TraceLog(LOG_INFO, "WATCH: %s %s -> reloading (now from [%d] %s)",
                 f->rel, why, (int)mount, m ? m->id : "none");
        ScriptHost_reload_current(host);
        return;
    }
}
