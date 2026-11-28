#include "services/tilemap/tilemap_lua.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "raylib.h"
#include "services/mounts/mounts.h"
#include "services/stores/texture_store.h"
#include "services/tilemap/tilemap.h"

#define TILEMAP_MT    "gramarye.tilemap.map"
#define MAP_NAME_MAX  48
#define LIST_MAX      256

typedef struct MapUD {
    Tilemap *map;
    char     name[MAP_NAME_MAX + 1];
    char     package[MOUNTS_ID_MAX];
} MapUD;

static struct { int ref; Tilemap *map; TilemapView view; } g_view = { LUA_NOREF, NULL, { 0 } };
static TilemapSprite g_sprites[TILEMAP_MAX_SPRITES];
static TilemapArc    g_arcs[TILEMAP_MAX_ARCS];
static Vector3       g_arc_pts[TILEMAP_MAX_ARC_PTS];
static TilemapMark   g_marks[TILEMAP_MAX_MARKS];
static TilemapRing   g_rings[TILEMAP_MAX_RINGS];
static struct {
    int ref; Tilemap *map; int32_t slot; float scale;
    int32_t hx, hy, sx, sy, sw, sh;
} g_palette = { LUA_NOREF, NULL, 0, 1.0f, -1, -1, 0, 0, 0, 0 };

static MapUD *check_map(lua_State *L, int idx) {
    MapUD *ud = (MapUD *)luaL_checkudata(L, idx, TILEMAP_MT);
    if (!ud->map) luaL_error(L, "tilemap was freed");
    return ud;
}

static MapUD *push_map(lua_State *L, Tilemap *m) {
    MapUD *ud = (MapUD *)lua_newuserdatauv(L, sizeof(MapUD), 0);
    memset(ud, 0, sizeof(*ud));
    ud->map = m;
    luaL_setmetatable(L, TILEMAP_MT);
    return ud;
}

static int32_t check_layer(lua_State *L, MapUD *ud, int idx) {
    lua_Integer l = luaL_checkinteger(L, idx);
    luaL_argcheck(L, l >= 1 && l <= Tilemap_layer_count(ud->map), idx, "no such layer");
    return (int32_t)(l - 1);
}

static bool is_terrain(MapUD *ud, int32_t layer) {
    return Tilemap_layer_kind(ud->map, layer) == TILEMAP_LAYER_TERRAIN;
}

static TileRef check_value(lua_State *L, MapUD *ud, int32_t layer, int idx) {
    int32_t a = (int32_t)luaL_optinteger(L, idx, 0);
    if (is_terrain(ud, layer)) {
        luaL_argcheck(L, a >= 0 && a <= Tilemap_terrain_count(ud->map), idx, "no such terrain");
        int32_t dir = -1;
        if (!lua_isnoneornil(L, idx + 1)) {
            lua_Integer d = luaL_checkinteger(L, idx + 1) % 360;
            dir = (int32_t)(d < 0 ? d + 360 : d);
        }
        return terrain_cell(a, dir);
    }
    int32_t tile = (int32_t)luaL_optinteger(L, idx + 1, 0);
    luaL_argcheck(L, a >= 0 && a <= Tilemap_tileset_count(ud->map), idx, "no such tileset");
    luaL_argcheck(L, tile >= 0 && tile < TILEMAP_TILE_BASE, idx + 1, "tile out of range");
    return tile_ref(a, tile);
}

static int fail(lua_State *L, const char *msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

static bool valid_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > MAP_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

static const Mount *find_mount(const char *id) {
    for (int32_t i = 0; i < Mounts_count(); i++) {
        const Mount *mt = Mounts_get(i);
        if (strcmp(mt->id, id) == 0) return mt;
    }
    return NULL;
}

static float num_field(lua_State *L, int t, const char *k, float def) {
    lua_getfield(L, t, k);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static int32_t int_field(lua_State *L, int t, const char *k, int32_t def) {
    lua_getfield(L, t, k);
    int32_t v = lua_isnumber(L, -1) ? (int32_t)lua_tointeger(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static bool bool_field(lua_State *L, int t, const char *k, bool def) {
    lua_getfield(L, t, k);
    bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static int sub_table(lua_State *L, int t, const char *k) {
    lua_getfield(L, t, k);
    if (lua_istable(L, -1)) return lua_gettop(L);
    lua_pop(L, 1);
    return 0;
}

static void rebind(lua_State *L, int *ref, int idx) {
    if (*ref != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, *ref);
        bool same = lua_rawequal(L, -1, idx);
        lua_pop(L, 1);
        if (same) return;
        luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    }
    lua_pushvalue(L, idx);
    *ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

static void unbind(lua_State *L, int *ref) {
    if (*ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, *ref);
    *ref = LUA_NOREF;
}

static int l_new(lua_State *L) {
    int32_t w = (int32_t)luaL_checkinteger(L, 1);
    int32_t h = (int32_t)luaL_checkinteger(L, 2);
    int32_t tile = 16, layers = 1, levels = 8;
    char names[TILEMAP_MAX_LAYERS][TILEMAP_LAYER_NAME_MAX] = { "ground" };
    const char *ptrs[TILEMAP_MAX_LAYERS] = { names[0] };
    bool terrain[TILEMAP_MAX_LAYERS] = { false };
    int32_t elevation[TILEMAP_MAX_LAYERS] = { 0 };
    if (lua_istable(L, 3)) {
        tile = int_field(L, 3, "tile", 16);
        levels = int_field(L, 3, "levels", 8);
        luaL_argcheck(L, levels >= 1 && levels <= TILEMAP_MAX_LEVELS, 3, "levels: 1-32");
        int lt = sub_table(L, 3, "layers");
        if (lt) {
            layers = (int32_t)lua_rawlen(L, lt);
            luaL_argcheck(L, layers >= 1 && layers <= TILEMAP_MAX_LAYERS, 3, "1-40 layers");
            for (int32_t i = 0; i < layers; i++) {
                lua_rawgeti(L, lt, i + 1);
                const char *name = "layer";
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "kind");
                    terrain[i] = lua_isstring(L, -1) && strcmp(lua_tostring(L, -1), "terrain") == 0;
                    lua_pop(L, 1);
                    elevation[i] = int_field(L, lua_gettop(L), "elevation", 0);
                    lua_getfield(L, -1, "name");
                    if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
                    snprintf(names[i], sizeof(names[i]), "%s", name);
                    lua_pop(L, 1);
                } else {
                    if (lua_isstring(L, -1)) name = lua_tostring(L, -1);
                    snprintf(names[i], sizeof(names[i]), "%s", name);
                }
                ptrs[i] = names[i];
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    }
    Tilemap *m = Tilemap_new(w, h, tile, layers, ptrs);
    if (!m) return luaL_error(L, "tilemap.new: bad size %dx%d (1-%d cells a side, tile 1-1024)",
                              (int)w, (int)h, TILEMAP_MAX_SIDE);
    Tilemap_set_levels(m, levels);
    for (int32_t i = 0; i < layers; i++) {
        if (terrain[i]) Tilemap_set_layer_kind(m, i, TILEMAP_LAYER_TERRAIN);
        Tilemap_set_layer_elevation(m, i, elevation[i]);
    }
    Tilemap_set_dirty(m, false);
    push_map(L, m);
    return 1;
}

static int l_load(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!valid_name(name)) return fail(L, "bad map name");
    char rel[MAP_NAME_MAX + 16], path[MOUNTS_PATH_MAX];
    snprintf(rel, sizeof(rel), "maps/%s.lua", name);
    int32_t mount = Mounts_find(rel, path, sizeof(path));
    if (mount < 0) return fail(L, TextFormat("no map named '%s'", name));

    char err[256];
    Tilemap *m = Tilemap_load(L, path, err, sizeof(err));
    if (!m) return fail(L, err);
    MapUD *ud = push_map(L, m);
    snprintf(ud->name, sizeof(ud->name), "%s", name);
    snprintf(ud->package, sizeof(ud->package), "%s", Mounts_get(mount)->id);
    lua_pushstring(L, ud->package);
    TraceLog(LOG_INFO, "TILEMAP: loaded %s (%dx%d)", path, (int)Tilemap_width(m), (int)Tilemap_height(m));
    return 2;
}

typedef struct ListEntry {
    char name[MAP_NAME_MAX + 1];
    char package[MOUNTS_ID_MAX];
} ListEntry;

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const ListEntry *)a)->name, ((const ListEntry *)b)->name);
}

static int l_list(lua_State *L) {
    static ListEntry entries[LIST_MAX];
    int32_t n = 0;
    for (int32_t i = Mounts_count() - 1; i >= 0; i--) {
        const Mount *mt = Mounts_get(i);
        char dir[MOUNTS_PATH_MAX + 8];
        snprintf(dir, sizeof(dir), "%smaps", mt->root);
        if (!mt->root[0] || !DirectoryExists(dir)) continue;
        FilePathList files = LoadDirectoryFilesEx(dir, ".lua", false);
        for (uint32_t k = 0; k < files.count && n < LIST_MAX; k++) {
            const char *base = GetFileNameWithoutExt(files.paths[k]);
            if (!valid_name(base)) continue;
            bool seen = false;
            for (int32_t j = 0; j < n && !seen; j++) seen = strcmp(entries[j].name, base) == 0;
            if (seen) continue;
            snprintf(entries[n].name, sizeof(entries[n].name), "%s", base);
            snprintf(entries[n].package, sizeof(entries[n].package), "%s", mt->id);
            n++;
        }
        UnloadDirectoryFiles(files);
    }
    qsort(entries, (size_t)n, sizeof(ListEntry), cmp_entry);
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 2);
        lua_pushstring(L, entries[i].name);    lua_setfield(L, -2, "name");
        lua_pushstring(L, entries[i].package); lua_setfield(L, -2, "package");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static Texture2D sprite_texture(const char *rel) {
    char path[MOUNTS_PATH_MAX];
    if (Mounts_resolve(rel, path, sizeof(path)) < 0) return (Texture2D){ 0 };
    int32_t id = TextureStore_register(rel, path);
    if (id && !TextureStore_is_resident(id)) TextureStore_acquire(id);
    return TextureStore_get(id);
}

static bool read_sprite(lua_State *L, int e, TilemapSprite *sp) {
    memset(sp, 0, sizeof(*sp));
    lua_getfield(L, e, "image");
    const char *image = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    sp->texture = image ? sprite_texture(image) : (Texture2D){ 0 };
    lua_pop(L, 1);
    if (sp->texture.id == 0) return false;
    sp->src = (Rectangle){ 0, 0, (float)sp->texture.width, (float)sp->texture.height };
    int t;
    if ((t = sub_table(L, e, "src"))) {
        sp->src = (Rectangle){ num_field(L, t, "x", 0), num_field(L, t, "y", 0),
                               num_field(L, t, "w", sp->src.width), num_field(L, t, "h", sp->src.height) };
        lua_pop(L, 1);
    }
    sp->x = num_field(L, e, "x", 0.0f);
    sp->y = num_field(L, e, "y", 0.0f);
    sp->w = num_field(L, e, "w", sp->src.width);
    sp->h = num_field(L, e, "h", sp->src.height);
    sp->level = int_field(L, e, "level", 0);
    sp->z = num_field(L, e, "z", -1.0f);
    sp->tint = WHITE;
    if ((t = sub_table(L, e, "tint"))) {
        uint8_t *c[4] = { &sp->tint.r, &sp->tint.g, &sp->tint.b, &sp->tint.a };
        for (int32_t i = 0; i < 4; i++) {
            lua_rawgeti(L, t, i + 1);
            if (lua_isnumber(L, -1)) *c[i] = (uint8_t)lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return true;
}

static Color color_field(lua_State *L, int t, const char *k, Color def) {
    int c = sub_table(L, t, k);
    if (!c) return def;
    uint8_t *ch[4] = { &def.r, &def.g, &def.b, &def.a };
    for (int32_t i = 0; i < 4; i++) {
        lua_rawgeti(L, c, i + 1);
        if (lua_isnumber(L, -1)) *ch[i] = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return def;
}

static void read_overlays(lua_State *L, int opts, TilemapView *v) {
    int t;
    if ((t = sub_table(L, opts, "arcs"))) {
        int32_t used = 0, n = (int32_t)lua_rawlen(L, t);
        for (int32_t i = 1; i <= n && v->arc_count < TILEMAP_MAX_ARCS; i++) {
            lua_rawgeti(L, t, i);
            int e = lua_gettop(L);
            int pts = lua_istable(L, e) ? sub_table(L, e, "points") : 0;
            if (pts) {
                int32_t count = (int32_t)lua_rawlen(L, pts) / 3;
                if (count > TILEMAP_MAX_ARC_PTS - used) count = TILEMAP_MAX_ARC_PTS - used;
                for (int32_t k = 0; k < count; k++) {
                    float c[3];
                    for (int32_t j = 0; j < 3; j++) {
                        lua_rawgeti(L, pts, k * 3 + j + 1);
                        c[j] = (float)lua_tonumber(L, -1);
                        lua_pop(L, 1);
                    }
                    g_arc_pts[used + k] = (Vector3){ c[0], c[1], c[2] };
                }
                lua_pop(L, 1);
                if (count >= 2) {
                    g_arcs[v->arc_count++] = (TilemapArc){
                        .points = &g_arc_pts[used], .count = count,
                        .color = color_field(L, e, "color", (Color){ 255, 230, 120, 255 }),
                        .width = num_field(L, e, "width", 3.0f) };
                    used += count;
                }
            }
            lua_pop(L, 1);
        }
        v->arcs = g_arcs;
        lua_pop(L, 1);
    }
    if ((t = sub_table(L, opts, "marks"))) {
        int32_t n = (int32_t)lua_rawlen(L, t);
        for (int32_t i = 1; i <= n && v->mark_count < TILEMAP_MAX_MARKS; i++) {
            lua_rawgeti(L, t, i);
            int e = lua_gettop(L);
            if (lua_istable(L, e)) {
                g_marks[v->mark_count++] = (TilemapMark){ int_field(L, e, "x", 0), int_field(L, e, "y", 0),
                                                          int_field(L, e, "level", -1),
                                                          color_field(L, e, "color", (Color){ 90, 200, 255, 70 }) };
            }
            lua_pop(L, 1);
        }
        v->marks = g_marks;
        lua_pop(L, 1);
    }
    if ((t = sub_table(L, opts, "rings"))) {
        int32_t n = (int32_t)lua_rawlen(L, t);
        for (int32_t i = 1; i <= n && v->ring_count < TILEMAP_MAX_RINGS; i++) {
            lua_rawgeti(L, t, i);
            int e = lua_gettop(L);
            if (lua_istable(L, e)) {
                g_rings[v->ring_count++] = (TilemapRing){
                    { num_field(L, e, "x", 0), num_field(L, e, "y", 0), num_field(L, e, "z", 0) },
                    num_field(L, e, "r", 6.0f), color_field(L, e, "color", (Color){ 255, 255, 255, 220 }) };
            }
            lua_pop(L, 1);
        }
        v->rings = g_rings;
        lua_pop(L, 1);
    }
}

static int l_pick(lua_State *L) {
    float wx, wy;
    if (!Tilemap_pick((float)luaL_checknumber(L, 1), (float)luaL_checknumber(L, 2),
                      (float)luaL_optnumber(L, 3, 0.0), &wx, &wy)) return 0;
    lua_pushnumber(L, wx);
    lua_pushnumber(L, wy);
    return 2;
}

static int l_view(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        unbind(L, &g_view.ref);
        g_view.map = NULL;
        return 0;
    }
    MapUD *ud = check_map(L, 1);
    TilemapView v = { .zoom = 1.0f, .bg = { 18, 18, 26, 255 }, .bounds = true, .active_layer = -1, .tilt = -1.0f,
                      .fov = 30.0f, .marker_z = -1.0f,
                      .cut_level = -1 };
    if (lua_istable(L, 2)) {
        v.x = num_field(L, 2, "x", 0.0f);
        v.y = num_field(L, 2, "y", 0.0f);
        v.zoom = num_field(L, 2, "zoom", 1.0f);
        v.grid = bool_field(L, 2, "grid", false);
        v.bounds = bool_field(L, 2, "bounds", true);
        v.dim = bool_field(L, 2, "dim", false);
        v.active_layer = int_field(L, 2, "layer", 0) - 1;
        v.tilt = num_field(L, 2, "tilt", -1.0f);
        v.rotation = num_field(L, 2, "rotation", 0.0f);
        v.fov = num_field(L, 2, "fov", 30.0f);
        v.cut_level = int_field(L, 2, "cut_level", -1);
        v.section = bool_field(L, 2, "section", false) || int_field(L, 2, "dim_below", -1) >= 0;
        lua_getfield(L, 2, "supersample");
        v.supersample = lua_isboolean(L, -1) ? (lua_toboolean(L, -1) ? 2 : 1)
                      : lua_isnumber(L, -1) ? (int32_t)lua_tointeger(L, -1) : 1;
        lua_pop(L, 1);
        v.arrows = bool_field(L, 2, "arrows", false);
        v.flags = bool_field(L, 2, "flags", false);
        int t;
        if ((t = sub_table(L, 2, "room"))) {
            v.room = true;
            v.room_x = int_field(L, t, "x", 0);
            v.room_y = int_field(L, t, "y", 0);
            v.room_z = int_field(L, t, "level", 0);
            lua_pop(L, 1);
        }
        if ((t = sub_table(L, 2, "see_through"))) {
            v.see_through = true;
            v.st_x = num_field(L, t, "x", 0.0f);
            v.st_y = num_field(L, t, "y", 0.0f);
            v.st_z = num_field(L, t, "z", 0.0f);
            v.st_radius = num_field(L, t, "r", 90.0f);
            lua_pop(L, 1);
        }
        if ((t = sub_table(L, 2, "bg"))) {
            uint8_t *c[4] = { &v.bg.r, &v.bg.g, &v.bg.b, &v.bg.a };
            for (int32_t i = 0; i < 4; i++) {
                lua_rawgeti(L, t, i + 1);
                if (lua_isnumber(L, -1)) *c[i] = (uint8_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        if ((t = sub_table(L, 2, "cursor"))) {
            v.cursor = true;
            v.cx = int_field(L, t, "x", 0); v.cy = int_field(L, t, "y", 0);
            v.cw = int_field(L, t, "w", 1); v.ch = int_field(L, t, "h", 1);
            v.erase_cursor = bool_field(L, t, "erase", false);
            lua_pop(L, 1);
        }
        if ((t = sub_table(L, 2, "ghost"))) {
            v.ghost = true;
            v.gx = int_field(L, t, "x", 0); v.gy = int_field(L, t, "y", 0);
            v.g_tileset = int_field(L, t, "tileset", 0);
            v.g_tx = int_field(L, t, "tx", 0); v.g_ty = int_field(L, t, "ty", 0);
            v.g_w = int_field(L, t, "w", 1); v.g_h = int_field(L, t, "h", 1);
            v.g_terrain = int_field(L, t, "terrain", 0);
            lua_pop(L, 1);
        }
        if ((t = sub_table(L, 2, "marker"))) {
            v.marker = true;
            v.mx = num_field(L, t, "x", 0.0f);
            v.my = num_field(L, t, "y", 0.0f);
            v.marker_level = int_field(L, t, "level", 0);
            v.marker_z = num_field(L, t, "z", -1.0f);
            lua_pop(L, 1);
        }
        read_overlays(L, 2, &v);
        if ((t = sub_table(L, 2, "sprites"))) {
            int32_t n = (int32_t)lua_rawlen(L, t);
            for (int32_t i = 1; i <= n && v.sprite_count < TILEMAP_MAX_SPRITES; i++) {
                lua_rawgeti(L, t, i);
                int e = lua_gettop(L);
                TilemapSprite sp;
                if (lua_istable(L, e) && read_sprite(L, e, &sp)) g_sprites[v.sprite_count++] = sp;
                lua_pop(L, 1);
            }
            v.sprites = g_sprites;
            lua_pop(L, 1);
        }
    }
    rebind(L, &g_view.ref, 1);
    g_view.map = ud->map;
    g_view.view = v;
    return 0;
}

static int l_palette(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        unbind(L, &g_palette.ref);
        g_palette.map = NULL;
        return 0;
    }
    MapUD *ud = check_map(L, 1);
    g_palette.slot = (int32_t)luaL_checkinteger(L, 2);
    g_palette.scale = 1.0f;
    g_palette.hx = g_palette.hy = -1;
    g_palette.sw = g_palette.sh = 0;
    if (lua_istable(L, 3)) {
        g_palette.scale = num_field(L, 3, "scale", 1.0f);
        int t;
        if ((t = sub_table(L, 3, "hover"))) {
            g_palette.hx = int_field(L, t, "x", -1);
            g_palette.hy = int_field(L, t, "y", -1);
            lua_pop(L, 1);
        }
        if ((t = sub_table(L, 3, "select"))) {
            g_palette.sx = int_field(L, t, "x", 0); g_palette.sy = int_field(L, t, "y", 0);
            g_palette.sw = int_field(L, t, "w", 1); g_palette.sh = int_field(L, t, "h", 1);
            lua_pop(L, 1);
        }
    }
    rebind(L, &g_palette.ref, 1);
    g_palette.map = ud->map;
    return 0;
}

static int m_size(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushinteger(L, Tilemap_width(ud->map));
    lua_pushinteger(L, Tilemap_height(ud->map));
    return 2;
}

static int m_tile(lua_State *L) {
    lua_pushinteger(L, Tilemap_tile(check_map(L, 1)->map));
    return 1;
}

static int m_layers(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t n = Tilemap_layer_count(ud->map);
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        lua_pushstring(L, Tilemap_layer_name(ud->map, i));
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int m_layer_visible(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushboolean(L, Tilemap_layer_visible(ud->map, check_layer(L, ud, 2)));
    return 1;
}

static int m_set_layer_visible(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Tilemap_set_layer_visible(ud->map, check_layer(L, ud, 2), lua_toboolean(L, 3));
    return 0;
}

static int m_layer_kind(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushstring(L, is_terrain(ud, check_layer(L, ud, 2)) ? "terrain" : "tiles");
    return 1;
}

static int m_layer_elevation(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushinteger(L, Tilemap_layer_elevation(ud->map, check_layer(L, ud, 2)));
    return 1;
}

static int m_set_layer_elevation(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Tilemap_set_layer_elevation(ud->map, check_layer(L, ud, 2), (int32_t)luaL_checkinteger(L, 3));
    return 0;
}

static int m_elevation(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushinteger(L, Tilemap_level_height(ud->map));
    lua_pushnumber(L, Tilemap_tilt(ud->map));
    lua_pushinteger(L, Tilemap_slope_run(ud->map));
    return 3;
}

static int m_set_elevation(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Tilemap_set_elevation_style(ud->map, (int32_t)luaL_checkinteger(L, 2), (float)luaL_checknumber(L, 3),
                                (int32_t)luaL_optinteger(L, 4, Tilemap_slope_run(ud->map)));
    return 0;
}

static TileRef check_flag(lua_State *L, int idx) {
    const char *f = luaL_checkstring(L, idx);
    if (strcmp(f, "solid") == 0) return TILEMAP_CELL_SOLID;
    if (strcmp(f, "water") == 0) return TILEMAP_CELL_WATER;
    luaL_argerror(L, idx, "flag is \"solid\" or \"water\"");
    return 0;
}

static int m_flags(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    TileRef f = Tilemap_flags(ud->map, check_layer(L, ud, 2), (int32_t)luaL_checkinteger(L, 3),
                              (int32_t)luaL_checkinteger(L, 4));
    lua_pushboolean(L, (f & TILEMAP_CELL_SOLID) != 0);
    lua_pushboolean(L, (f & TILEMAP_CELL_WATER) != 0);
    return 2;
}

static int m_paint_flags(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    luaL_argcheck(L, is_terrain(ud, layer), 2, "flags live on terrain layers");
    lua_pushinteger(L, Tilemap_paint_flags(ud->map, layer,
        (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
        (int32_t)luaL_checkinteger(L, 5), (int32_t)luaL_checkinteger(L, 6),
        check_flag(L, 7), lua_toboolean(L, 8)));
    return 1;
}

static int m_surface(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2), y = (int32_t)luaL_checkinteger(L, 3);
    int32_t slot = 0;
    int32_t level = lua_isnoneornil(L, 4) ? Tilemap_level_at(ud->map, x, y, &slot)
                                          : Tilemap_floor_at(ud->map, x, y, (int32_t)luaL_checkinteger(L, 4), &slot);
    int32_t layer = -1;
    for (int32_t l = 0; l < Tilemap_layer_count(ud->map) && level >= 0; l++) {
        if (Tilemap_layer_kind(ud->map, l) == TILEMAP_LAYER_TERRAIN && Tilemap_layer_elevation(ud->map, l) == level &&
            terrain_cell_slot(Tilemap_get(ud->map, l, x, y)) != 0) layer = l;
    }
    TileRef f = layer >= 0 ? Tilemap_flags(ud->map, layer, x, y) : 0;
    bool deep = false;
    int32_t depth = level >= 0 ? Tilemap_water_at(ud->map, x, y, level, &deep) : 0;
    lua_pushinteger(L, level);
    lua_pushinteger(L, slot);
    lua_pushboolean(L, (f & TILEMAP_CELL_SOLID) != 0);
    lua_pushboolean(L, (f & TILEMAP_CELL_WATER) != 0 || depth > 0);
    if (layer >= 0) lua_pushinteger(L, layer + 1);
    else            lua_pushnil(L);
    lua_pushinteger(L, depth);
    return 6;
}

static int m_height_at(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t level = -1;
    float wx = (float)luaL_checknumber(L, 2), wy = (float)luaL_checknumber(L, 3);
    float h = lua_isnoneornil(L, 4) ? Tilemap_height_at(ud->map, wx, wy, &level)
                                    : Tilemap_floor_height(ud->map, wx, wy, (int32_t)luaL_checkinteger(L, 4), &level);
    lua_pushnumber(L, h);
    lua_pushinteger(L, level);
    return 2;
}

static int m_covered(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushboolean(L, Tilemap_covered(ud->map, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3),
                                       (int32_t)luaL_checkinteger(L, 4)));
    return 1;
}

static int m_levels(lua_State *L) {
    lua_pushinteger(L, Tilemap_levels(check_map(L, 1)->map));
    return 1;
}

static int m_set_levels(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Tilemap_set_levels(ud->map, (int32_t)luaL_checkinteger(L, 2));
    return 0;
}

static void push_points(lua_State *L, const Vector3 *p, int32_t n) {
    lua_createtable(L, n * 3, 0);
    for (int32_t i = 0; i < n; i++) {
        lua_pushnumber(L, p[i].x); lua_rawseti(L, -2, i * 3 + 1);
        lua_pushnumber(L, p[i].y); lua_rawseti(L, -2, i * 3 + 2);
        lua_pushnumber(L, p[i].z); lua_rawseti(L, -2, i * 3 + 3);
    }
}

static int m_arc(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Vector3 a = { (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4) };
    Vector3 b = { (float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7) };
    float peak = 12.0f, obstacle = 0.0f;
    int32_t n = 32;
    if (lua_istable(L, 8)) {
        peak = num_field(L, 8, "peak", peak);
        obstacle = num_field(L, 8, "obstacle", obstacle);
        n = int_field(L, 8, "samples", n);
    }
    n = n < 2 ? 2 : n > 512 ? 512 : n;
    static Vector3 pts[512];
    Vector3 hit;
    int32_t first = Tilemap_arc(ud->map, a, b, peak, obstacle, pts, n, &hit);
    push_points(L, pts, n);
    if (first < 0) return 1;
    lua_pushnumber(L, hit.x);
    lua_pushnumber(L, hit.y);
    lua_pushnumber(L, hit.z);
    return 4;
}

static int m_trace(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Vector3 p = { (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4) };
    Vector3 v = { (float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7) };
    float g = 600.0f, dt = 1.0f / 60.0f;
    int32_t n = 240;
    if (lua_istable(L, 8)) {
        g = num_field(L, 8, "gravity", g);
        dt = num_field(L, 8, "dt", dt);
        n = int_field(L, 8, "max", n);
    }
    n = n < 1 ? 1 : n > 1024 ? 1024 : n;
    static Vector3 pts[1024];
    bool landed = false;
    int32_t count = Tilemap_trace(ud->map, p, v, g, dt, pts, n, &landed);
    push_points(L, pts, count);
    lua_pushboolean(L, landed);
    return 2;
}

static int m_raycast(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Vector3 o = { (float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3), (float)luaL_checknumber(L, 4) };
    Vector3 d = { (float)luaL_checknumber(L, 5), (float)luaL_checknumber(L, 6), (float)luaL_checknumber(L, 7) };
    Vector3 hit;
    float max = (float)luaL_optnumber(L, 8, 8000.0);
    bool found;
    if (lua_istable(L, 9)) {
        found = Tilemap_raycast_room(ud->map, o, d, max, int_field(L, 9, "x", 0), int_field(L, 9, "y", 0),
                                     int_field(L, 9, "level", 0), &hit);
    } else {
        found = Tilemap_raycast(ud->map, o, d, max, (int32_t)luaL_optinteger(L, 9, -1), &hit);
    }
    if (!found) return 0;
    lua_pushnumber(L, hit.x);
    lua_pushnumber(L, hit.y);
    lua_pushnumber(L, hit.z);
    return 3;
}

typedef struct Walker { TilemapWalker w; int32_t level, to_level; } Walker;

static Walker check_walker(lua_State *L, int idx) {
    Walker w = { TILEMAP_WALKER_DEFAULT, -1, -1 };
    if (lua_istable(L, idx)) {
        if (bool_field(L, idx, "solid", false)) w.w.pass |= TILEMAP_CELL_SOLID;
        if (bool_field(L, idx, "water", false)) w.w.pass |= TILEMAP_CELL_WATER;
        w.level = int_field(L, idx, "level", -1);
        w.w.clearance = int_field(L, idx, "height", 1);
        w.w.wade = int_field(L, idx, "wade", 1);
        w.w.wade_cost = int_field(L, idx, "wade_cost", 2);
        w.w.swim_cost = int_field(L, idx, "swim_cost", 2);
        w.to_level = int_field(L, idx, "to_level", -1);
    }
    return w;
}

static int m_reach(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Walker w = check_walker(L, 5);
    int32_t cap = 8192;
    TilemapReach *cells = malloc((size_t)cap * sizeof(TilemapReach));
    if (!cells) return luaL_error(L, "out of memory");
    int32_t n = Tilemap_reach(ud->map, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3), w.level,
                              (int32_t)luaL_checkinteger(L, 4), &w.w, cells, cap);
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 8);
        lua_pushinteger(L, cells[i].cost);   lua_setfield(L, -2, "cost");
        lua_pushinteger(L, cells[i].x);      lua_setfield(L, -2, "x");
        lua_pushinteger(L, cells[i].y);      lua_setfield(L, -2, "y");
        lua_pushinteger(L, cells[i].z);      lua_setfield(L, -2, "z");
        lua_pushinteger(L, cells[i].steps);  lua_setfield(L, -2, "steps");
        lua_pushinteger(L, cells[i].from_x); lua_setfield(L, -2, "from_x");
        lua_pushinteger(L, cells[i].from_y); lua_setfield(L, -2, "from_y");
        lua_pushinteger(L, cells[i].from_z); lua_setfield(L, -2, "from_z");
        lua_rawseti(L, -2, i + 1);
    }
    free(cells);
    return 1;
}

static int m_path(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Walker w = check_walker(L, 7);
    static int32_t xyz[3 * (TILEMAP_MAX_STEPS + 1)];
    int32_t n = Tilemap_path(ud->map, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3), w.level,
                             (int32_t)luaL_checkinteger(L, 4), (int32_t)luaL_checkinteger(L, 5), w.to_level,
                             (int32_t)luaL_optinteger(L, 6, TILEMAP_MAX_STEPS), &w.w,
                             xyz, TILEMAP_MAX_STEPS + 1);
    if (n == 0) return 0;
    lua_createtable(L, n, 0);
    for (int32_t i = 0; i < n; i++) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, xyz[i * 3]);     lua_setfield(L, -2, "x");
        lua_pushinteger(L, xyz[i * 3 + 1]); lua_setfield(L, -2, "y");
        lua_pushinteger(L, xyz[i * 3 + 2]); lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int m_can_step(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    Walker w = check_walker(L, 6);
    int32_t z1 = -1, cost = 1;
    TilemapStep r = Tilemap_can_step(ud->map, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3), w.level,
                                     (int32_t)luaL_checkinteger(L, 4), (int32_t)luaL_checkinteger(L, 5),
                                     &w.w, &z1, &cost);
    static const char *why[] = { NULL, "solid", "water", "cliff", "void", "low" };
    lua_pushboolean(L, r == TILEMAP_STEP_OK);
    if (r == TILEMAP_STEP_OK) {
        lua_pushnil(L);
        lua_pushinteger(L, z1);
        lua_pushinteger(L, cost);
    } else {
        lua_pushstring(L, why[r]);
        lua_pushnil(L);
        lua_pushnil(L);
    }
    return 4;
}

static int m_mesh_stats(lua_State *L) {
    TilemapMeshStats st;
    Tilemap_mesh_stats(check_map(L, 1)->map, &st);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, st.chunks);             lua_setfield(L, -2, "chunks");
    lua_pushinteger(L, st.built);              lua_setfield(L, -2, "built");
    lua_pushinteger(L, (lua_Integer)st.vertices); lua_setfield(L, -2, "vertices");
    lua_pushinteger(L, st.built_last_frame);   lua_setfield(L, -2, "built_last_frame");
    lua_pushnumber(L, st.build_ms_last_frame); lua_setfield(L, -2, "build_ms");
    lua_pushinteger(L, st.pending);            lua_setfield(L, -2, "pending");
    lua_pushboolean(L, st.far);                lua_setfield(L, -2, "far");
    return 1;
}

static int m_water_at(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t x = (int32_t)luaL_checkinteger(L, 2), y = (int32_t)luaL_checkinteger(L, 3);
    int32_t z = lua_isnoneornil(L, 4) ? Tilemap_level_at(ud->map, x, y, NULL) : (int32_t)luaL_checkinteger(L, 4);
    bool deep = false;
    int32_t d = z >= 0 ? Tilemap_water_at(ud->map, x, y, z, &deep) : 0;
    lua_pushinteger(L, d);
    lua_pushnumber(L, z >= 0 ? (float)((z + d) * Tilemap_level_height(ud->map)) : 0.0f);
    lua_pushboolean(L, deep);
    return 3;
}

static int m_layer_height(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushinteger(L, Tilemap_layer_height(ud->map, check_layer(L, ud, 2)));
    return 1;
}

static int m_level_at(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t slot = 0;
    int32_t x = (int32_t)luaL_checkinteger(L, 2), y = (int32_t)luaL_checkinteger(L, 3);
    lua_pushinteger(L, lua_isnoneornil(L, 4) ? Tilemap_level_at(ud->map, x, y, &slot)
                                             : Tilemap_floor_at(ud->map, x, y, (int32_t)luaL_checkinteger(L, 4), &slot));
    lua_pushinteger(L, slot);
    return 2;
}

static int m_add_terrain(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    TilemapTerrain def;
    Tilemap_terrain_from_lua(L, 2, &def);
    if (!def.id[0]) return luaL_error(L, "add_terrain: id is a required string");
    int32_t slot = Tilemap_add_terrain(ud->map, &def);
    if (slot == 0) return fail(L, TextFormat("can't add terrain '%s' (full, or a name/path too long)", def.id));
    lua_pushinteger(L, slot);
    return 1;
}

static void push_color(lua_State *L, Color c) {
    lua_createtable(L, 4, 0);
    lua_pushinteger(L, c.r); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, c.g); lua_rawseti(L, -2, 2);
    lua_pushinteger(L, c.b); lua_rawseti(L, -2, 3);
    lua_pushinteger(L, c.a); lua_rawseti(L, -2, 4);
}

static int m_terrains(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t n = Tilemap_terrain_count(ud->map);
    lua_createtable(L, n, 0);
    for (int32_t i = 1; i <= n; i++) {
        const TilemapTerrain *t = Tilemap_terrain(ud->map, i);
        lua_createtable(L, 0, 9);
        lua_pushstring(L, t->id);        lua_setfield(L, -2, "id");
        if (t->image[0]) { lua_pushstring(L, t->image); lua_setfield(L, -2, "image"); }
        if (t->color.a)  { push_color(L, t->color);     lua_setfield(L, -2, "color"); }
        push_color(L, Tilemap_terrain_swatch(ud->map, i)); lua_setfield(L, -2, "swatch");
        lua_pushnumber(L, t->noise);      lua_setfield(L, -2, "noise");
        lua_pushnumber(L, t->blades);     lua_setfield(L, -2, "blades");
        lua_pushnumber(L, t->foam);       lua_setfield(L, -2, "foam");
        lua_pushnumber(L, t->foam_speed); lua_setfield(L, -2, "foam_speed");
        push_color(L, t->foam_color);     lua_setfield(L, -2, "foam_color");
        if (t->cliff.a) { push_color(L, t->cliff); lua_setfield(L, -2, "cliff"); }
        lua_pushboolean(L, t->stairs);    lua_setfield(L, -2, "stairs");
        lua_pushnumber(L, t->waves);      lua_setfield(L, -2, "waves");
        lua_pushnumber(L, t->sway);       lua_setfield(L, -2, "sway");
        lua_pushnumber(L, t->sway_speed); lua_setfield(L, -2, "sway_speed");
        lua_pushnumber(L, t->flow);       lua_setfield(L, -2, "flow");
        lua_pushboolean(L, t->water);     lua_setfield(L, -2, "water");
        lua_pushboolean(L, t->solid);     lua_setfield(L, -2, "solid");
        lua_rawseti(L, -2, i);
    }
    return 1;
}

static int m_add_tileset(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_getfield(L, 2, "id");
    lua_getfield(L, 2, "image");
    lua_getfield(L, 2, "skin");
    if (!lua_isstring(L, -3) || !lua_isstring(L, -2)) {
        return luaL_error(L, "add_tileset: id and image are required strings");
    }
    const char *id = lua_tostring(L, -3);
    const char *image = lua_tostring(L, -2);
    const char *skin = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    int32_t tile = int_field(L, 2, "tile", Tilemap_tile(ud->map));
    int32_t tw = int_field(L, 2, "tile_w", tile), th = int_field(L, 2, "tile_h", tile);
    int32_t slot = Tilemap_add_tileset(ud->map, id, image, skin, tw, th);
    lua_pop(L, 3);
    if (slot == 0) return fail(L, TextFormat("can't add tileset '%s' (full, or a name/path too long)", id));
    lua_pushinteger(L, slot);
    return 1;
}

static int m_tilesets(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t n = Tilemap_tileset_count(ud->map);
    lua_createtable(L, n, 0);
    for (int32_t i = 1; i <= n; i++) {
        const TilemapTileset *t = Tilemap_tileset(ud->map, i);
        lua_createtable(L, 0, 8);
        lua_pushstring(L, t->id);      lua_setfield(L, -2, "id");
        lua_pushstring(L, t->image);   lua_setfield(L, -2, "image");
        if (t->skin[0]) { lua_pushstring(L, t->skin); lua_setfield(L, -2, "skin"); }
        lua_pushinteger(L, t->tile_w); lua_setfield(L, -2, "tile_w");
        lua_pushinteger(L, t->tile_h); lua_setfield(L, -2, "tile_h");
        lua_pushinteger(L, t->cols);   lua_setfield(L, -2, "cols");
        lua_pushinteger(L, t->rows);   lua_setfield(L, -2, "rows");
        lua_pushboolean(L, t->cols > 0); lua_setfield(L, -2, "loaded");
        lua_rawseti(L, -2, i);
    }
    return 1;
}

static int m_get(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    TileRef r = Tilemap_get(ud->map, layer, (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4));
    if (is_terrain(ud, layer)) {
        lua_pushinteger(L, terrain_cell_slot(r));
        int32_t dir = terrain_cell_dir(r);
        if (dir < 0) lua_pushnil(L);
        else         lua_pushinteger(L, dir);
        return 2;
    }
    lua_pushinteger(L, tile_ref_tileset(r));
    lua_pushinteger(L, tile_ref_tile(r));
    return 2;
}

static int m_set(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    TileRef v = check_value(L, ud, layer, 5);
    Tilemap_set(ud->map, layer, (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4), v);
    return 0;
}

static int m_fill(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    TileRef v = check_value(L, ud, layer, 7);
    lua_pushinteger(L, Tilemap_fill_rect(ud->map, layer,
        (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
        (int32_t)luaL_checkinteger(L, 5), (int32_t)luaL_checkinteger(L, 6), v));
    return 1;
}

static int m_stamp(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    luaL_argcheck(L, !is_terrain(ud, layer), 2, "stamp needs a tile layer (use fill on terrain layers)");
    lua_pushinteger(L, Tilemap_stamp(ud->map, layer,
        (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
        (int32_t)luaL_checkinteger(L, 5),
        (int32_t)luaL_checkinteger(L, 6), (int32_t)luaL_checkinteger(L, 7),
        (int32_t)luaL_optinteger(L, 8, 1), (int32_t)luaL_optinteger(L, 9, 1)));
    return 1;
}

static int m_erase(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    lua_pushinteger(L, Tilemap_fill_rect(ud->map, layer,
        (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
        (int32_t)luaL_optinteger(L, 5, 1), (int32_t)luaL_optinteger(L, 6, 1), 0));
    return 1;
}

static int m_flood(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    TileRef v = check_value(L, ud, layer, 5);
    lua_pushinteger(L, Tilemap_flood(ud->map, layer, (int32_t)luaL_checkinteger(L, 3),
                                     (int32_t)luaL_checkinteger(L, 4), v));
    return 1;
}

static int m_snapshot(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layers[TILEMAP_MAX_LAYERS], n = 0;
    for (int i = 2; i <= lua_gettop(L) && n < TILEMAP_MAX_LAYERS; i++) layers[n++] = check_layer(L, ud, i);
    luaL_argcheck(L, n > 0, 2, "a layer");
    Tilemap_snapshot_layers(ud->map, layers, n);
    return 0;
}

static int m_add_layer(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    const char *name = luaL_checkstring(L, 2);
    const char *kind = luaL_optstring(L, 3, "tiles");
    int32_t l = Tilemap_add_layer(ud->map, name, strcmp(kind, "terrain") == 0 ? TILEMAP_LAYER_TERRAIN : TILEMAP_LAYER_TILES,
                                  (int32_t)luaL_optinteger(L, 4, 0));
    if (l < 0) return 0;
    lua_pushinteger(L, l + 1);
    return 1;
}

static int m_remove_layer(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushboolean(L, Tilemap_remove_layer(ud->map, check_layer(L, ud, 2)));
    return 1;
}

static int m_layer_empty(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    lua_pushboolean(L, Tilemap_layer_empty(ud->map, check_layer(L, ud, 2)));
    return 1;
}

static int m_fill_empty(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    int32_t layer = check_layer(L, ud, 2);
    TileRef v = check_value(L, ud, layer, 7);
    lua_pushinteger(L, Tilemap_fill_empty(ud->map, layer,
        (int32_t)luaL_checkinteger(L, 3), (int32_t)luaL_checkinteger(L, 4),
        (int32_t)luaL_checkinteger(L, 5), (int32_t)luaL_checkinteger(L, 6), v));
    return 1;
}

static int m_drop_snapshot(lua_State *L) {
    Tilemap_drop_snapshot(check_map(L, 1)->map);
    return 0;
}

static int push_layer_or_nil(lua_State *L, int32_t layer) {
    if (layer < 0) lua_pushnil(L);
    else           lua_pushinteger(L, layer + 1);
    return 1;
}

static int m_undo(lua_State *L) { return push_layer_or_nil(L, Tilemap_undo(check_map(L, 1)->map)); }
static int m_redo(lua_State *L) { return push_layer_or_nil(L, Tilemap_redo(check_map(L, 1)->map)); }

static int m_dirty(lua_State *L) {
    lua_pushboolean(L, Tilemap_dirty(check_map(L, 1)->map));
    return 1;
}

static int m_name(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    if (!ud->name[0]) return 0;
    lua_pushstring(L, ud->name);
    lua_pushstring(L, ud->package);
    return 2;
}

static int m_save(lua_State *L) {
    MapUD *ud = check_map(L, 1);
    const char *pkg = luaL_checkstring(L, 2);
    const char *name = luaL_checkstring(L, 3);
    if (!valid_name(name)) return fail(L, "map names are 1-48 letters, digits, _ or -");
    const Mount *mt = find_mount(pkg);
    if (!mt) return fail(L, TextFormat("package '%s' is not mounted", pkg));
    if (!mt->root[0]) return fail(L, TextFormat("package '%s' is read-only on this platform", pkg));

    char dir[MOUNTS_PATH_MAX + 8], path[MOUNTS_PATH_MAX + 64];
    snprintf(dir, sizeof(dir), "%smaps", mt->root);
    if (!DirectoryExists(dir) && MakeDirectory(dir) != 0) {
        return fail(L, TextFormat("cannot create %s", dir));
    }
    snprintf(path, sizeof(path), "%s/%s.lua", dir, name);
    char err[256];
    if (!Tilemap_save(ud->map, path, err, sizeof(err))) return fail(L, err);

    Tilemap_set_dirty(ud->map, false);
    snprintf(ud->name, sizeof(ud->name), "%s", name);
    snprintf(ud->package, sizeof(ud->package), "%s", mt->id);
    TraceLog(LOG_INFO, "TILEMAP: saved %s", path);
    lua_pushboolean(L, 1);
    lua_pushstring(L, path);
    return 2;
}

static void free_map(lua_State *L, MapUD *ud) {
    if (!ud->map) return;
    if (g_view.map == ud->map)    { unbind(L, &g_view.ref);    g_view.map = NULL; }
    if (g_palette.map == ud->map) { unbind(L, &g_palette.ref); g_palette.map = NULL; }
    Tilemap_free(ud->map);
    ud->map = NULL;
}

static int m_free(lua_State *L) {
    free_map(L, (MapUD *)luaL_checkudata(L, 1, TILEMAP_MT));
    return 0;
}

static int m_gc(lua_State *L) {
    MapUD *ud = (MapUD *)luaL_checkudata(L, 1, TILEMAP_MT);
    if (g_view.map == ud->map)    { g_view.map = NULL;    g_view.ref = LUA_NOREF; }
    if (g_palette.map == ud->map) { g_palette.map = NULL; g_palette.ref = LUA_NOREF; }
    Tilemap_free(ud->map);
    ud->map = NULL;
    return 0;
}

void tilemap_lua_register(ScriptHost *host) {
    lua_State *L = ScriptHost_state(host);

    static const luaL_Reg methods[] = {
        { "size", m_size },           { "tile", m_tile },
        { "layers", m_layers },       { "layer_visible", m_layer_visible },
        { "set_layer_visible", m_set_layer_visible },
        { "layer_kind", m_layer_kind },
        { "layer_elevation", m_layer_elevation }, { "set_layer_elevation", m_set_layer_elevation },
        { "elevation", m_elevation }, { "set_elevation", m_set_elevation },
        { "layer_height", m_layer_height }, { "level_at", m_level_at },
        { "height_at", m_height_at }, { "surface", m_surface },
        { "flags", m_flags }, { "paint_flags", m_paint_flags },
        { "can_step", m_can_step }, { "covered", m_covered },
        { "levels", m_levels }, { "set_levels", m_set_levels },
        { "arc", m_arc }, { "trace", m_trace }, { "raycast", m_raycast },
        { "reach", m_reach }, { "path", m_path },
        { "add_tileset", m_add_tileset }, { "tilesets", m_tilesets },
        { "add_terrain", m_add_terrain }, { "terrains", m_terrains },
        { "get", m_get },             { "set", m_set },
        { "fill", m_fill },
        { "stamp", m_stamp },         { "erase", m_erase },
        { "flood", m_flood },         { "snapshot", m_snapshot },
        { "fill_empty", m_fill_empty }, { "add_layer", m_add_layer },
        { "water_at", m_water_at },   { "mesh_stats", m_mesh_stats },
        { "remove_layer", m_remove_layer }, { "layer_empty", m_layer_empty },
        { "drop_snapshot", m_drop_snapshot },
        { "undo", m_undo },           { "redo", m_redo },
        { "dirty", m_dirty },         { "name", m_name },
        { "save", m_save },           { "free", m_free },
        { NULL, NULL },
    };
    if (luaL_newmetatable(L, TILEMAP_MT)) {
        luaL_newlib(L, methods);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, m_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);

    ScriptHost_register_function(host, "tilemap", "new",     l_new);
    ScriptHost_register_function(host, "tilemap", "load",    l_load);
    ScriptHost_register_function(host, "tilemap", "list",    l_list);
    ScriptHost_register_function(host, "tilemap", "view",    l_view);
    ScriptHost_register_function(host, "tilemap", "palette", l_palette);
    ScriptHost_register_function(host, "tilemap", "pick",    l_pick);

    lua_getglobal(L, "gramarye");
    lua_getfield(L, -1, "tilemap");
    lua_pushinteger(L, TILEMAP_VIEW_KIND);    lua_setfield(L, -2, "VIEW_KIND");
    lua_pushinteger(L, TILEMAP_PALETTE_KIND); lua_setfield(L, -2, "PALETTE_KIND");
    lua_pop(L, 2);
}

bool tilemap_lua_draw(int32_t kind, float x, float y, float w, float h) {
    if (kind == TILEMAP_VIEW_KIND) {
        if (g_view.map) Tilemap_draw(g_view.map, &g_view.view, x, y, w, h);
        else DrawRectangle((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (Color){ 18, 18, 26, 255 });
        return true;
    }
    if (kind == TILEMAP_PALETTE_KIND) {
        if (g_palette.map) {
            Tilemap_draw_tileset(g_palette.map, g_palette.slot, x, y, g_palette.scale,
                                 g_palette.hx, g_palette.hy,
                                 g_palette.sx, g_palette.sy, g_palette.sw, g_palette.sh);
        }
        return true;
    }
    return false;
}

void tilemap_lua_reload_shaders(void) { Tilemap_reload_shaders(); }
void tilemap_lua_watch_shaders(float dt) { Tilemap_watch_shaders(dt); }

void tilemap_lua_shutdown(void) {
    g_view.ref = g_palette.ref = LUA_NOREF;
    g_view.map = g_palette.map = NULL;
    Tilemap_shutdown();
}
