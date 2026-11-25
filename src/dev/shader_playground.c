#include "dev/shader_playground.h"
#include "dev/shader_editor.h"
#include <stdint.h>

#include "raylib.h"
#include "rlgl.h"
#include "lua.h"
#include "lauxlib.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PLAYGROUND_DIR      GRAMARYE_DEV_DIR "shaders/"
#define PLAYGROUND_MAX      64
#define PLAYGROUND_NAME     64
#define PLAYGROUND_POLL     0.25
#define PLAYGROUND_DEBOUNCE 0.30

typedef struct Entry {
    char name[PLAYGROUND_NAME];
} Entry;

static Entry     g_entries[PLAYGROUND_MAX];
static int32_t   g_count;
static int32_t   g_current = -1;
static Shader    g_shader;
static bool      g_shader_ok;
static Texture2D g_texture;
static bool      g_texture_owned;
static Texture2D g_white;
static uint32_t  g_disk_hash;
static uint32_t  g_vs_hash;
static long      g_png_time;
static uint32_t  g_compiled_rev;
static double    g_last_poll;
static bool      g_disk_conflict;
static char      g_status[192];
static bool      g_status_ok = true;
static char      g_log[8192];
static int32_t   g_log_len;

static const char *const STARTER_FALLBACK =
    "#version 330\n\n"
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n\n"
    "uniform sampler2D texture0;\n"
    "uniform float uTime;\n"
    "uniform vec2  uResolution;\n"
    "uniform vec2  uMouse;\n"
    "uniform vec2  uTextureSize;\n\n"
    "out vec4 finalColor;\n\n"
    "void main() {\n"
    "    finalColor = vec4(fragTexCoord, 0.0, 1.0);\n"
    "}\n";

static const char *entry_file(int32_t i, const char *ext) {
    return TextFormat("%s%s.%s", PLAYGROUND_DIR, g_entries[i].name, ext);
}

static uint32_t str_hash(const char *text) {
    uint32_t h = 2166136261u;
    for (const char *c = text; *c; c++) {
        if (*c == '\r') continue;
        h = (h ^ (uint8_t)*c) * 16777619u;
    }
    return h | 1u;
}

static uint32_t file_hash(const char *path) {
    if (!FileExists(path)) return 0;
    char *text = LoadFileText(path);
    if (!text) return 0;
    uint32_t h = str_hash(text);
    UnloadFileText(text);
    return h;
}

static bool is_dirty(void) {
    return g_current >= 0 && str_hash(shader_editor_text()) != g_disk_hash;
}

static void set_status(bool ok, const char *msg) {
    snprintf(g_status, sizeof(g_status), "%s", msg);
    g_status_ok = ok;
}

static void rescan(void) {
    char keep[PLAYGROUND_NAME] = "";
    if (g_current >= 0) snprintf(keep, sizeof(keep), "%s", g_entries[g_current].name);
    g_count = 0;
    FilePathList files = LoadDirectoryFilesEx(PLAYGROUND_DIR, ".fs", false);
    for (uint32_t i = 0; i < files.count && g_count < PLAYGROUND_MAX; i++) {
        snprintf(g_entries[g_count].name, PLAYGROUND_NAME, "%s", GetFileNameWithoutExt(files.paths[i]));
        g_count++;
    }
    UnloadDirectoryFiles(files);
    for (int32_t i = 1; i < g_count; i++) {
        Entry e = g_entries[i];
        int32_t j = i;
        while (j > 0 && strcmp(g_entries[j - 1].name, e.name) > 0) { g_entries[j] = g_entries[j - 1]; j--; }
        g_entries[j] = e;
    }
    g_current = -1;
    for (int32_t i = 0; i < g_count; i++) if (strcmp(g_entries[i].name, keep) == 0) g_current = i;
}

static bool save_current(void) {
    if (g_current < 0) return false;
    const char *text = shader_editor_text();
    if (!SaveFileText(entry_file(g_current, "fs"), text)) {
        set_status(false, TextFormat("could not write %s.fs", g_entries[g_current].name));
        return false;
    }
    g_disk_hash = str_hash(text);
    g_disk_conflict = false;
    return true;
}

static void load_text_from_disk(bool keep_view) {
    char *text = LoadFileText(entry_file(g_current, "fs"));
    shader_editor_set_text(text ? text : "", keep_view);
    g_disk_hash = str_hash(text ? text : "");
    if (text) UnloadFileText(text);
    g_disk_conflict = false;
}

static void capture_log(int level, const char *text, va_list args) {
    char line[4096];
    va_list copy;
    va_copy(copy, args);
    vsnprintf(line, sizeof(line), text, copy);
    va_end(copy);
    const char *tag = level >= LOG_ERROR ? "ERROR" : level == LOG_WARNING ? "WARNING"
                    : level == LOG_INFO ? "INFO" : "DEBUG";
    printf("%s: %s\n", tag, line);

    const char *msg = NULL, *m;
    if ((m = strstr(line, "Compile error: "))) msg = m + 15;
    else if ((m = strstr(line, "Link error: "))) msg = m + 12;
    if (!msg) return;
    int32_t n = snprintf(g_log + g_log_len, sizeof(g_log) - (size_t)g_log_len, "%s\n", msg);
    if (n > 0) g_log_len += n;
    if (g_log_len >= (int32_t)sizeof(g_log)) g_log_len = (int32_t)sizeof(g_log) - 1;
}

static void compile(void) {
    if (g_current < 0) return;
    g_compiled_rev = shader_editor_revision();
    const char *vs_path = entry_file(g_current, "vs");
    char *vs = FileExists(vs_path) ? LoadFileText(vs_path) : NULL;
    g_vs_hash = vs ? str_hash(vs) : 0;

    g_log_len = 0;
    g_log[0] = 0;
    SetTraceLogCallback(capture_log);
    Shader next = LoadShaderFromMemory(vs, shader_editor_text());
    SetTraceLogCallback(NULL);
    if (vs) UnloadFileText(vs);

    const char *name = g_entries[g_current].name;
    if (next.id == 0 || next.id == rlGetShaderIdDefault()) {
        UnloadShader(next);
        shader_editor_set_errors(g_log_len ? g_log : "compile failed (no log from the driver; see terminal)");
        set_status(false, TextFormat("%s: error%s", name, g_shader_ok ? ", preview shows last good version" : ""));
        return;
    }
    if (g_shader_ok) UnloadShader(g_shader);
    g_shader = next;
    g_shader_ok = true;
    shader_editor_set_errors(NULL);
    set_status(true, TextFormat("%s compiled at %.1fs", name, GetTime()));
}

static void release_texture(void) {
    if (g_texture_owned) UnloadTexture(g_texture);
    g_texture = g_white;
    g_texture_owned = false;
}

static void load_texture(void) {
    release_texture();
    const char *png = entry_file(g_current, "png");
    g_png_time = FileExists(png) ? GetFileModTime(png) : 0;
    if (!g_png_time) return;
    Texture2D t = LoadTexture(png);
    if (t.id != 0) { g_texture = t; g_texture_owned = true; }
}

void shader_playground_unload(void) {
    if (is_dirty()) save_current();
    if (g_shader_ok) UnloadShader(g_shader);
    g_shader_ok = false;
    release_texture();
    if (g_white.id) UnloadTexture(g_white);
    g_white = (Texture2D){ 0 };
    g_texture = g_white;
    g_current = -1;
    shader_editor_set_errors(NULL);
    shader_editor_shutdown();
}

static void select_entry(int32_t i) {
    if (i < 0 || i >= g_count) return;
    if (is_dirty()) save_current();
    if (g_white.id == 0) {
        Image px = GenImageColor(1, 1, WHITE);
        g_white = LoadTextureFromImage(px);
        UnloadImage(px);
        g_texture = g_white;
    }
    if (g_shader_ok) UnloadShader(g_shader);
    g_shader_ok = false;
    g_current = i;
    load_text_from_disk(false);
    load_texture();
    compile();
    g_last_poll = GetTime();
}

static bool valid_name(const char *name) {
    if (!name[0] || strlen(name) >= PLAYGROUND_NAME - 4) return false;
    for (const char *c = name; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') return false;
    return true;
}

static const char *new_entry(const char *name) {
    if (!valid_name(name)) return "use letters, digits, _ or -";
    const char *path = TextFormat("%s%s.fs", PLAYGROUND_DIR, name);
    if (!FileExists(path)) {
        char *starter = LoadFileText(PLAYGROUND_DIR "starter.fs");
        bool ok = SaveFileText(TextFormat("%s%s.fs", PLAYGROUND_DIR, name), starter ? starter : STARTER_FALLBACK);
        if (starter) UnloadFileText(starter);
        if (!ok) return "could not write the file";
    }
    if (is_dirty()) save_current();
    rescan();
    for (int32_t i = 0; i < g_count; i++) if (strcmp(g_entries[i].name, name) == 0) select_entry(i);
    return NULL;
}

static void poll_disk(void) {
    double now = GetTime();
    if (now - g_last_poll < PLAYGROUND_POLL) return;
    g_last_poll = now;

    uint32_t fs = file_hash(entry_file(g_current, "fs"));
    if (!fs) {
        char name[PLAYGROUND_NAME];
        snprintf(name, sizeof(name), "%s", g_entries[g_current].name);
        rescan();
        if (g_current < 0) set_status(false, TextFormat("%s.fs was deleted or renamed outside the app", name));
        return;
    }
    if (fs != g_disk_hash) {
        if (!is_dirty()) {
            load_text_from_disk(true);
            compile();
        } else if (!g_disk_conflict) {
            g_disk_conflict = true;
            set_status(false, "file changed on disk; Ctrl+S overwrites it, Revert loads it");
        }
    }
    if (file_hash(entry_file(g_current, "vs")) != g_vs_hash) compile();
    const char *png = entry_file(g_current, "png");
    long png_time = FileExists(png) ? GetFileModTime(png) : 0;
    if (png_time != g_png_time) load_texture();
}

static void tick(void) {
    if (g_current < 0) return;
    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl && IsKeyPressed(KEY_S) && save_current())
        set_status(true, TextFormat("saved %s.fs", g_entries[g_current].name));
    if (ctrl && IsKeyPressed(KEY_ENTER)) compile();
    poll_disk();
    if (g_current < 0) return;
    if (shader_editor_revision() != g_compiled_rev && GetTime() - shader_editor_last_edit() > PLAYGROUND_DEBOUNCE)
        compile();
    shader_editor_set_info(TextFormat("%s.fs%s", g_entries[g_current].name,
                                      is_dirty() ? "  *unsaved (Ctrl+S)" : ""));
}

static void set_uniform(const char *name, const void *v, int type) {
    int loc = GetShaderLocation(g_shader, name);
    if (loc >= 0) SetShaderValue(g_shader, loc, v, type);
}

void shader_playground_draw_editor_in_rect(float x, float y, float w, float h) {
    if (g_current < 0) {
        DrawRectangle((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (Color){ 18, 18, 26, 255 });
        return;
    }
    shader_editor_draw_in_rect(x, y, w, h);
}

void shader_playground_draw_in_rect(float x, float y, float w, float h) {
    DrawRectangle((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (Color){ 12, 12, 18, 255 });
    if (w < 2.0f || h < 2.0f) return;
    tick();
    if (g_current < 0 || !g_shader_ok) return;

    Vector2 m = GetMousePosition();
    float t = (float)GetTime();
    float res[2] = { w, h }, mouse[2] = { (m.x - x) / w, (m.y - y) / h };
    float tex[2] = { (float)g_texture.width, (float)g_texture.height };
    BeginShaderMode(g_shader);
    set_uniform("uTime", &t, SHADER_UNIFORM_FLOAT);
    set_uniform("uResolution", res, SHADER_UNIFORM_VEC2);
    set_uniform("uMouse", mouse, SHADER_UNIFORM_VEC2);
    set_uniform("uTextureSize", tex, SHADER_UNIFORM_VEC2);
    DrawTexturePro(g_texture, (Rectangle){ 0, 0, tex[0], tex[1] }, (Rectangle){ x, y, w, h },
                   (Vector2){ 0, 0 }, 0.0f, WHITE);
    EndShaderMode();
}

static int l_count(lua_State *L)   { lua_pushinteger(L, g_count); return 1; }
static int l_current(lua_State *L) { lua_pushinteger(L, g_current + 1); return 1; }
static int l_rescan(lua_State *L)  { (void)L; rescan(); return 0; }
static int l_select(lua_State *L)  { select_entry((int32_t)luaL_checkinteger(L, 1) - 1); return 0; }
static int l_unload(lua_State *L)  { (void)L; shader_playground_unload(); return 0; }
static int l_dir(lua_State *L)     { lua_pushstring(L, PLAYGROUND_DIR); return 1; }
static int l_dirty(lua_State *L)   { lua_pushboolean(L, is_dirty()); return 1; }
static int l_focused(lua_State *L) { lua_pushboolean(L, shader_editor_focused()); return 1; }
static int l_focus(lua_State *L)   { shader_editor_set_focus(lua_toboolean(L, 1)); return 0; }

static int l_save(lua_State *L) {
    bool ok = save_current();
    if (ok) set_status(true, TextFormat("saved %s.fs", g_entries[g_current].name));
    lua_pushboolean(L, ok);
    return 1;
}

static int l_revert(lua_State *L) {
    (void)L;
    if (g_current < 0) return 0;
    load_text_from_disk(true);
    load_texture();
    compile();
    return 0;
}

static int l_new(lua_State *L) {
    const char *err = new_entry(luaL_checkstring(L, 1));
    lua_pushboolean(L, err == NULL);
    lua_pushstring(L, err ? err : "");
    return 2;
}

static int l_capture_escape(lua_State *L) {
    SetExitKey(lua_toboolean(L, 1) ? KEY_NULL : KEY_ESCAPE);
    return 0;
}

static int l_name(lua_State *L) {
    int32_t i = (int32_t)luaL_checkinteger(L, 1) - 1;
    lua_pushstring(L, (i >= 0 && i < g_count) ? g_entries[i].name : "");
    return 1;
}

static int l_status(lua_State *L) {
    lua_pushstring(L, g_status);
    lua_pushboolean(L, g_status_ok);
    return 2;
}

void shader_playground_lua_register(ScriptHost *host) {
    ScriptHost_register_function(host, "shader_playground", "count",          l_count);
    ScriptHost_register_function(host, "shader_playground", "name",           l_name);
    ScriptHost_register_function(host, "shader_playground", "current",        l_current);
    ScriptHost_register_function(host, "shader_playground", "select",         l_select);
    ScriptHost_register_function(host, "shader_playground", "rescan",         l_rescan);
    ScriptHost_register_function(host, "shader_playground", "revert",         l_revert);
    ScriptHost_register_function(host, "shader_playground", "save",           l_save);
    ScriptHost_register_function(host, "shader_playground", "new",            l_new);
    ScriptHost_register_function(host, "shader_playground", "dirty",          l_dirty);
    ScriptHost_register_function(host, "shader_playground", "editor_focused", l_focused);
    ScriptHost_register_function(host, "shader_playground", "focus_editor",   l_focus);
    ScriptHost_register_function(host, "shader_playground", "capture_escape", l_capture_escape);
    ScriptHost_register_function(host, "shader_playground", "unload",         l_unload);
    ScriptHost_register_function(host, "shader_playground", "status",         l_status);
    ScriptHost_register_function(host, "shader_playground", "dir",            l_dir);
}
