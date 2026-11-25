#ifndef SERVICES_TILEMAP_H
#define SERVICES_TILEMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "raylib.h"

struct lua_State;

#define TILEMAP_MAX_SIDE      1024
#define TILEMAP_MAX_LAYERS    40
#define TILEMAP_MAX_TILESETS  16
#define TILEMAP_TILE_BASE     10000
#define TILEMAP_ID_MAX        32
#define TILEMAP_REL_MAX       160
#define TILEMAP_LAYER_NAME_MAX 24
#define TILEMAP_MAX_TERRAINS  16
#define TILEMAP_MAX_LEVELS    32
#define TILEMAP_MAX_ELEVATION (TILEMAP_MAX_LEVELS - 1)
#define TILEMAP_MAX_LEVEL_HEIGHT 32

typedef uint32_t TileRef;

#define TILEMAP_DIR_SHIFT 8
#define TILEMAP_CELL_SOLID (1u << 17)
#define TILEMAP_CELL_WATER (1u << 18)
#define TILEMAP_CELL_FLAGS (TILEMAP_CELL_SOLID | TILEMAP_CELL_WATER)
static inline TileRef terrain_cell(int32_t slot, int32_t dir_deg) {
    if (slot <= 0) return 0;
    TileRef d = dir_deg < 0 ? 0 : (TileRef)((dir_deg % 360) + 1);
    return (TileRef)slot | (d << TILEMAP_DIR_SHIFT);
}
static inline int32_t terrain_cell_slot(TileRef v) { return (int32_t)(v & 0xFFu); }
static inline int32_t terrain_cell_dir(TileRef v) {
    int32_t d = (int32_t)((v >> TILEMAP_DIR_SHIFT) & 0x1FFu);
    return d == 0 ? -1 : d - 1;
}

static inline TileRef tile_ref(int32_t tileset, int32_t tile) {
    return tileset > 0 ? (TileRef)(tileset * TILEMAP_TILE_BASE + tile) : 0;
}
static inline int32_t tile_ref_tileset(TileRef r) { return (int32_t)(r / TILEMAP_TILE_BASE); }
static inline int32_t tile_ref_tile(TileRef r)    { return (int32_t)(r % TILEMAP_TILE_BASE); }

typedef struct TilemapTileset {
    char    id[TILEMAP_ID_MAX];
    char    image[TILEMAP_REL_MAX];
    char    skin[TILEMAP_REL_MAX];
    int16_t tile_w, tile_h;
    int16_t cols, rows;
    int32_t texture;
    int32_t skin_texture;
} TilemapTileset;

typedef enum TilemapLayerKind {
    TILEMAP_LAYER_TILES   = 0,
    TILEMAP_LAYER_TERRAIN = 1,
} TilemapLayerKind;

typedef struct TilemapTerrain {
    char    id[TILEMAP_ID_MAX];
    char    image[TILEMAP_REL_MAX];
    Color   color;
    float   noise;
    float   blades;
    float   foam;
    float   foam_speed;
    Color   foam_color;
    float   waves;
    float   flow;
    float   sway;
    float   sway_speed;
    Color   cliff;
    bool    stairs;
    bool    water;
    bool    solid;
} TilemapTerrain;

typedef struct Tilemap Tilemap;

Tilemap *Tilemap_new(int32_t w, int32_t h, int32_t tile, int32_t layers,
                     const char *const *layer_names);
void     Tilemap_free(Tilemap *m);

int32_t     Tilemap_width(const Tilemap *m);
int32_t     Tilemap_height(const Tilemap *m);
int32_t     Tilemap_tile(const Tilemap *m);
int32_t     Tilemap_layer_count(const Tilemap *m);
const char *Tilemap_layer_name(const Tilemap *m, int32_t layer);
bool        Tilemap_layer_visible(const Tilemap *m, int32_t layer);
void        Tilemap_set_layer_visible(Tilemap *m, int32_t layer, bool visible);
bool        Tilemap_dirty(const Tilemap *m);
void        Tilemap_set_dirty(Tilemap *m, bool dirty);

int32_t Tilemap_add_layer(Tilemap *m, const char *name, TilemapLayerKind kind, int32_t elevation);
bool    Tilemap_remove_layer(Tilemap *m, int32_t layer);
bool    Tilemap_layer_empty(const Tilemap *m, int32_t layer);
TilemapLayerKind Tilemap_layer_kind(const Tilemap *m, int32_t layer);
void             Tilemap_set_layer_kind(Tilemap *m, int32_t layer, TilemapLayerKind kind);

int32_t Tilemap_levels(const Tilemap *m);
void    Tilemap_set_levels(Tilemap *m, int32_t levels);
int32_t Tilemap_layer_elevation(const Tilemap *m, int32_t layer);
void    Tilemap_set_layer_elevation(Tilemap *m, int32_t layer, int32_t elevation);
int32_t Tilemap_level_height(const Tilemap *m);
float   Tilemap_tilt(const Tilemap *m);
int32_t Tilemap_slope_run(const Tilemap *m);
void    Tilemap_set_elevation_style(Tilemap *m, int32_t level_height, float tilt, int32_t slope_run);
int32_t Tilemap_layer_height(const Tilemap *m, int32_t layer);
int32_t Tilemap_level_at(const Tilemap *m, int32_t x, int32_t y, int32_t *slot);
int32_t Tilemap_floor_at(const Tilemap *m, int32_t x, int32_t y, int32_t z, int32_t *slot);
bool    Tilemap_covered(const Tilemap *m, int32_t x, int32_t y, int32_t z);
int32_t Tilemap_water_at(const Tilemap *m, int32_t x, int32_t y, int32_t z, bool *deep);
float   Tilemap_height_at(const Tilemap *m, float wx, float wy, int32_t *level);
float   Tilemap_floor_height(const Tilemap *m, float wx, float wy, int32_t z, int32_t *level);

TileRef Tilemap_flags(const Tilemap *m, int32_t layer, int32_t x, int32_t y);
int32_t Tilemap_paint_flags(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                            int32_t w, int32_t h, TileRef flags, bool on);

typedef enum TilemapStep {
    TILEMAP_STEP_OK = 0,
    TILEMAP_STEP_SOLID,
    TILEMAP_STEP_WATER,
    TILEMAP_STEP_CLIFF,
    TILEMAP_STEP_VOID,
    TILEMAP_STEP_LOW,
} TilemapStep;

typedef struct TilemapWalker {
    TileRef pass;
    int32_t clearance;
    int32_t wade;
    int32_t wade_cost;
    int32_t swim_cost;
} TilemapWalker;
#define TILEMAP_WALKER_DEFAULT ((TilemapWalker){ 0, 1, 1, 2, 2 })

TilemapStep Tilemap_can_step(const Tilemap *m, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1,
                             const TilemapWalker *w, int32_t *z1, int32_t *cost);

#define TILEMAP_MAX_STEPS 32

typedef struct TilemapReach {
    int32_t x, y, z;
    int32_t steps;
    int32_t cost;
    int32_t from_x, from_y, from_z;
} TilemapReach;

int32_t Tilemap_reach(const Tilemap *m, int32_t x, int32_t y, int32_t z, int32_t max_cost, const TilemapWalker *w,
                      TilemapReach *out, int32_t cap);
int32_t Tilemap_path(const Tilemap *m, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                     int32_t max_cost, const TilemapWalker *w, int32_t *xyz, int32_t cap);

int32_t Tilemap_arc(const Tilemap *m, Vector3 a, Vector3 b, float peak, float obstacle,
                    Vector3 *out, int32_t n, Vector3 *hit);
int32_t Tilemap_trace(const Tilemap *m, Vector3 p, Vector3 v, float gravity, float dt,
                      Vector3 *out, int32_t n, bool *landed);
bool    Tilemap_raycast(const Tilemap *m, Vector3 origin, Vector3 dir, float max_dist, int32_t max_level,
                        Vector3 *hit);
bool    Tilemap_raycast_room(const Tilemap *m, Vector3 origin, Vector3 dir, float max_dist,
                             int32_t x, int32_t y, int32_t z, Vector3 *hit);

int32_t Tilemap_add_tileset(Tilemap *m, const char *id, const char *image,
                            const char *skin, int32_t tile_w, int32_t tile_h);
int32_t               Tilemap_tileset_count(const Tilemap *m);
const TilemapTileset *Tilemap_tileset(const Tilemap *m, int32_t slot);

int32_t               Tilemap_add_terrain(Tilemap *m, const TilemapTerrain *def);
int32_t               Tilemap_terrain_count(const Tilemap *m);
const TilemapTerrain *Tilemap_terrain(const Tilemap *m, int32_t slot);
Color                 Tilemap_terrain_swatch(const Tilemap *m, int32_t slot);
void                  Tilemap_terrain_from_lua(struct lua_State *L, int32_t t, TilemapTerrain *out);

TileRef Tilemap_get(const Tilemap *m, int32_t layer, int32_t x, int32_t y);
void    Tilemap_set(Tilemap *m, int32_t layer, int32_t x, int32_t y, TileRef ref);
int32_t Tilemap_stamp(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                      int32_t tileset, int32_t tx, int32_t ty, int32_t w, int32_t h);
int32_t Tilemap_fill_rect(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                          int32_t w, int32_t h, TileRef ref);
int32_t Tilemap_fill_empty(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                           int32_t w, int32_t h, TileRef ref);
int32_t Tilemap_flood(Tilemap *m, int32_t layer, int32_t x, int32_t y, TileRef ref);

void Tilemap_snapshot(Tilemap *m, int32_t layer);
void Tilemap_snapshot_layers(Tilemap *m, const int32_t *layers, int32_t n);
void Tilemap_drop_snapshot(Tilemap *m);
int32_t Tilemap_undo(Tilemap *m);
int32_t Tilemap_redo(Tilemap *m);

bool     Tilemap_save(const Tilemap *m, const char *path, char *err, size_t errcap);
Tilemap *Tilemap_load(struct lua_State *L, const char *path, char *err, size_t errcap);

#define TILEMAP_MAX_SPRITES 256
#define TILEMAP_MAX_ARCS    16
#define TILEMAP_MAX_ARC_PTS 2048
#define TILEMAP_MAX_MARKS   2048
#define TILEMAP_MAX_RINGS   32

typedef struct TilemapArc {
    const Vector3 *points;
    int32_t count;
    Color   color;
    float   width;
} TilemapArc;

typedef struct TilemapMark {
    int32_t x, y;
    int32_t level;
    Color   color;
} TilemapMark;

typedef struct TilemapRing {
    Vector3 at;
    float   radius;
    Color   color;
} TilemapRing;

typedef struct TilemapSprite {
    Texture2D texture;
    Rectangle src;
    float     x, y;
    float     w, h;
    int32_t   level;
    float     z;
    Color     tint;
} TilemapSprite;

typedef struct TilemapView {
    float   x, y;
    float   zoom;
    Color   bg;
    bool    grid;
    bool    bounds;
    int32_t active_layer;
    bool    dim;
    bool    cursor;
    int32_t cx, cy, cw, ch;
    bool    ghost;
    int32_t gx, gy, g_tileset, g_tx, g_ty, g_w, g_h;
    int32_t g_terrain;
    bool    erase_cursor;
    bool    marker;
    float   mx, my;
    int32_t marker_level;
    float   marker_z;
    bool    flags;
    const TilemapSprite *sprites;
    int32_t sprite_count;
    const TilemapArc  *arcs;  int32_t arc_count;
    const TilemapMark *marks; int32_t mark_count;
    const TilemapRing *rings; int32_t ring_count;
    float   tilt;
    float   rotation;
    float   fov;
    bool    arrows;
    int32_t supersample;
    int32_t cut_level;
    bool    room;
    int32_t room_x, room_y, room_z;
    bool    section;
    bool    see_through;
    float   st_x, st_y, st_z, st_radius;
} TilemapView;

typedef struct TilemapMeshStats {
    int32_t chunks, built;
    int64_t vertices;
    int32_t built_last_frame;
    float   build_ms_last_frame;
    int32_t pending;
    bool    far;
} TilemapMeshStats;
void Tilemap_mesh_stats(const Tilemap *m, TilemapMeshStats *out);

void Tilemap_draw(const Tilemap *m, const TilemapView *v, float x, float y, float w, float h);
bool Tilemap_pick(float sx, float sy, float height, float *wx, float *wy);

void Tilemap_draw_tileset(const Tilemap *m, int32_t slot, float x, float y, float scale,
                          int32_t hx, int32_t hy, int32_t sx, int32_t sy, int32_t sw, int32_t sh);

void Tilemap_shutdown(void);

void Tilemap_reload_shaders(void);
void Tilemap_watch_shaders(float dt);

#endif
