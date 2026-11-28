#include "services/tilemap/tilemap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "raymath.h"
#include "rlgl.h"
#include "services/mounts/mounts.h"
#include "services/mounts/packages.h"
#include "services/stores/texture_store.h"

#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
#define TILEMAP_GLSL_VERSION 100
#else
#define TILEMAP_GLSL_VERSION 330
#endif

#define UNDO_MAX_ENTRIES 64
#define UNDO_MAX_BYTES   ((size_t)64 * 1024 * 1024)
#define UNDO_TILE        32

typedef struct UndoTile {
    int8_t   layer;
    int32_t  tx, ty;
    TileRef *cells;
} UndoTile;

typedef struct UndoEntry {
    int32_t   count, cap;
    UndoTile *tiles;
} UndoEntry;

typedef struct UndoStack {
    UndoEntry e[UNDO_MAX_ENTRIES];
    int8_t    count;
} UndoStack;

struct Tilemap {
    int16_t  w, h, tile;
    int8_t   layer_count;
    int8_t   tileset_count;
    int8_t   terrain_count;
    int8_t   levels;
    bool     dirty;
    char     layer_names[TILEMAP_MAX_LAYERS][TILEMAP_LAYER_NAME_MAX];
    bool     layer_visible[TILEMAP_MAX_LAYERS];
    uint8_t  layer_kind[TILEMAP_MAX_LAYERS];
    uint8_t  layer_elevation[TILEMAP_MAX_LAYERS];
    int16_t  level_height;
    int16_t  slope_run;
    float    tilt;
    TileRef *cells[TILEMAP_MAX_LAYERS];
    TilemapTileset tilesets[TILEMAP_MAX_TILESETS];
    TilemapTerrain terrains[TILEMAP_MAX_TERRAINS];
    Color    swatch[TILEMAP_MAX_TERRAINS];
    UndoStack undo, redo;

    Texture2D ids[TILEMAP_MAX_LAYERS];
    bool      ids_stale[TILEMAP_MAX_LAYERS];
    int32_t   ids_rect[TILEMAP_MAX_LAYERS][4];
    uint8_t  *slopes[TILEMAP_MAX_LAYERS];
    uint8_t  *wslopes[TILEMAP_MAX_LAYERS];
    bool      slopes_stale[TILEMAP_MAX_LAYERS];
    int32_t   slope_rect[TILEMAP_MAX_LAYERS][4];
    Texture2D fills;
    bool      fills_stale;
    int16_t   fill_w, fill_h;
    struct ChunkGeo *chunks;
    int16_t   chunk_cells;
    int16_t   chunks_x, chunks_y;
    int8_t    lvl_first[TILEMAP_MAX_LEVELS];
    int8_t    lvl_next[TILEMAP_MAX_LAYERS];
    bool      lvl_indexed;
    uint32_t  edit_gen;
    bool      recording;
    uint8_t  *rec_seen[TILEMAP_MAX_LAYERS];
    uint8_t  *cut;
    int32_t   cut_key[5];
    bool      cut_any;
    int32_t   cut_max, cut_here;
    bool      cut_gpu_stale;
    Texture2D cut_tex;
    Mesh      cut_faces;
    int32_t   stat_built_frame;
    float     stat_build_ms;
    int32_t   stat_pending;
    bool      stat_far;
    uint32_t  frame_no;
    uint8_t  *ov_level;
    Texture2D ov_tex;
    int32_t   ov_dirty[4];
};

static size_t cell_count(const Tilemap *m) { return (size_t)m->w * (size_t)m->h; }
static void undo_touch(Tilemap *m, int32_t layer, int32_t x, int32_t y);
static size_t layer_bytes(const Tilemap *m) { return cell_count(m) * sizeof(TileRef); }

static bool in_bounds(const Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    return m && layer >= 0 && layer < m->layer_count &&
           x >= 0 && y >= 0 && x < m->w && y < m->h;
}

static void meshes_stale(Tilemap *m, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

static void rect_grow(int32_t *d, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (d[2] < d[0]) { d[0] = x0; d[1] = y0; d[2] = x1; d[3] = y1; return; }
    if (x0 < d[0]) d[0] = x0;
    if (y0 < d[1]) d[1] = y0;
    if (x1 > d[2]) d[2] = x1;
    if (y1 > d[3]) d[3] = y1;
}

static void overview_dirty(Tilemap *m, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    rect_grow(m->ov_dirty, x0, y0, x1, y1);
}

#define SYNC_REACH 9

static void cells_dirty(Tilemap *m, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    for (int32_t l = 0; l < m->layer_count; l++) {
        if (m->layer_kind[l] != TILEMAP_LAYER_TERRAIN) continue;
        rect_grow(m->ids_rect[l], x0 - SYNC_REACH, y0 - SYNC_REACH, x1 + SYNC_REACH, y1 + SYNC_REACH);
        rect_grow(m->slope_rect[l], x0 - SYNC_REACH, y0 - SYNC_REACH, x1 + SYNC_REACH, y1 + SYNC_REACH);
    }
}

static void cells_changed(Tilemap *m, int32_t layer) {
    m->edit_gen++;
    overview_dirty(m, 0, 0, m->w - 1, m->h - 1);
    m->ids_stale[layer] = m->slopes_stale[layer] = true;
    for (int32_t l = 0; l < m->layer_count; l++) {
        if (m->layer_elevation[l] > 0) m->ids_stale[l] = m->slopes_stale[l] = true;
    }
    meshes_stale(m, 0, 0, m->w - 1, m->h - 1);
}

#define MESH_REACH 8
static void cell_changed(Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    (void)layer;
    m->edit_gen++;
    cells_dirty(m, x, y, x, y);
    meshes_stale(m, x - MESH_REACH, y - MESH_REACH, x + MESH_REACH, y + MESH_REACH);
    overview_dirty(m, x, y, x, y);
}

static void region_changed(Tilemap *m, int32_t layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    (void)layer;
    m->edit_gen++;
    cells_dirty(m, x0, y0, x1, y1);
    meshes_stale(m, x0 - MESH_REACH, y0 - MESH_REACH, x1 + MESH_REACH, y1 + MESH_REACH);
    overview_dirty(m, x0, y0, x1, y1);
}

static void all_cells_changed(Tilemap *m) {
    m->edit_gen++;
    overview_dirty(m, 0, 0, m->w - 1, m->h - 1);
    for (int32_t l = 0; l < m->layer_count; l++) m->ids_stale[l] = m->slopes_stale[l] = true;
    meshes_stale(m, 0, 0, m->w - 1, m->h - 1);
}

static bool is_terrain_layer(const Tilemap *m, int32_t layer) {
    return m->layer_kind[layer] == TILEMAP_LAYER_TERRAIN;
}

#define DEFAULT_TILT 40.0f

Tilemap *Tilemap_new(int32_t w, int32_t h, int32_t tile, int32_t layers,
                     const char *const *layer_names) {
    if (w < 1 || h < 1 || w > TILEMAP_MAX_SIDE || h > TILEMAP_MAX_SIDE) return NULL;
    if (tile < 1 || tile > 1024 || layers < 1 || layers > TILEMAP_MAX_LAYERS) return NULL;

    Tilemap *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->w = (int16_t)w;
    m->h = (int16_t)h;
    m->tile = (int16_t)tile;
    m->layer_count = (int8_t)layers;
    int32_t lh = tile * 3 / 4;
    m->level_height = (int16_t)(lh < 1 ? 1 : lh > TILEMAP_MAX_LEVEL_HEIGHT ? TILEMAP_MAX_LEVEL_HEIGHT : lh);
    m->slope_run = (int16_t)(tile * 3 / 2);
    m->tilt = DEFAULT_TILT;
    m->levels = 8;
    m->ov_dirty[2] = m->ov_dirty[3] = -1;
    for (int32_t l = 0; l < TILEMAP_MAX_LAYERS; l++) {
        m->ids_rect[l][2] = m->ids_rect[l][3] = m->slope_rect[l][2] = m->slope_rect[l][3] = -1;
    }
    for (int32_t i = 0; i < layers; i++) {
        m->cells[i] = calloc(cell_count(m), sizeof(TileRef));
        if (!m->cells[i]) { Tilemap_free(m); return NULL; }
        const char *name = layer_names && layer_names[i] ? layer_names[i] : NULL;
        if (name) snprintf(m->layer_names[i], TILEMAP_LAYER_NAME_MAX, "%s", name);
        else      snprintf(m->layer_names[i], TILEMAP_LAYER_NAME_MAX, "layer%d", (int)(i + 1));
        m->layer_visible[i] = true;
    }
    return m;
}

static void undo_entry_free(UndoEntry *e) {
    for (int32_t i = 0; i < e->count; i++) free(e->tiles[i].cells);
    free(e->tiles);
    *e = (UndoEntry){ 0 };
}

static void undo_clear(UndoStack *s) {
    for (int32_t i = 0; i < s->count; i++) undo_entry_free(&s->e[i]);
    s->count = 0;
}

static void free_meshes(Tilemap *m, bool gpu);

void Tilemap_free(Tilemap *m) {
    if (!m) return;
    bool gpu = IsWindowReady();
    if (gpu) {
        for (int32_t i = 0; i < TILEMAP_MAX_LAYERS; i++) {
            if (m->ids[i].id != 0) UnloadTexture(m->ids[i]);
        }
        if (m->fills.id != 0) UnloadTexture(m->fills);
        if (m->cut_tex.id != 0) UnloadTexture(m->cut_tex);
        if (m->ov_tex.id != 0) UnloadTexture(m->ov_tex);
        if (m->cut_faces.vaoId != 0 || m->cut_faces.vboId) UnloadMesh(m->cut_faces);
    }
    free(m->cut);
    free(m->ov_level);
    free_meshes(m, gpu);
    for (int32_t i = 0; i < TILEMAP_MAX_LAYERS; i++) {
        free(m->cells[i]);
        free(m->slopes[i]);
        free(m->wslopes[i]);
    }
    for (int32_t i = 0; i < m->tileset_count; i++) {
        if (m->tilesets[i].texture)      TextureStore_release(m->tilesets[i].texture);
        if (m->tilesets[i].skin_texture) TextureStore_release(m->tilesets[i].skin_texture);
    }
    undo_clear(&m->undo);
    undo_clear(&m->redo);
    for (int32_t i = 0; i < TILEMAP_MAX_LAYERS; i++) free(m->rec_seen[i]);
    free(m);
}

int32_t Tilemap_width(const Tilemap *m)       { return m ? m->w : 0; }
int32_t Tilemap_height(const Tilemap *m)      { return m ? m->h : 0; }
int32_t Tilemap_tile(const Tilemap *m)        { return m ? m->tile : 0; }
int32_t Tilemap_layer_count(const Tilemap *m) { return m ? m->layer_count : 0; }
bool    Tilemap_dirty(const Tilemap *m)       { return m && m->dirty; }
void    Tilemap_set_dirty(Tilemap *m, bool d) { if (m) m->dirty = d; }

const char *Tilemap_layer_name(const Tilemap *m, int32_t layer) {
    return (m && layer >= 0 && layer < m->layer_count) ? m->layer_names[layer] : NULL;
}

bool Tilemap_layer_visible(const Tilemap *m, int32_t layer) {
    return m && layer >= 0 && layer < m->layer_count && m->layer_visible[layer];
}

void Tilemap_set_layer_visible(Tilemap *m, int32_t layer, bool visible) {
    if (m && layer >= 0 && layer < m->layer_count) m->layer_visible[layer] = visible;
}

TilemapLayerKind Tilemap_layer_kind(const Tilemap *m, int32_t layer) {
    return (m && layer >= 0 && layer < m->layer_count) ? (TilemapLayerKind)m->layer_kind[layer]
                                                       : TILEMAP_LAYER_TILES;
}

void Tilemap_set_layer_kind(Tilemap *m, int32_t layer, TilemapLayerKind kind) {
    if (!m || layer < 0 || layer >= m->layer_count || m->layer_kind[layer] == (uint8_t)kind) return;
    m->layer_kind[layer] = (uint8_t)kind;
    m->lvl_indexed = false;
    m->edit_gen++;
    memset(m->cells[layer], 0, layer_bytes(m));
    m->recording = false;
    undo_clear(&m->undo);
    undo_clear(&m->redo);
    cells_changed(m, layer);
}

int32_t Tilemap_levels(const Tilemap *m) { return m ? m->levels : 0; }

void Tilemap_set_levels(Tilemap *m, int32_t levels) {
    if (!m) return;
    levels = levels < 1 ? 1 : levels > TILEMAP_MAX_LEVELS ? TILEMAP_MAX_LEVELS : levels;
    if (m->levels == levels) return;
    m->levels = (int8_t)levels;
    m->lvl_indexed = false;
    m->edit_gen++;
    for (int32_t l = 0; l < m->layer_count; l++) {
        if (m->layer_elevation[l] >= levels) m->layer_elevation[l] = (uint8_t)(levels - 1);
    }
    m->dirty = true;
    all_cells_changed(m);
}

int32_t Tilemap_layer_elevation(const Tilemap *m, int32_t layer) {
    return (m && layer >= 0 && layer < m->layer_count) ? m->layer_elevation[layer] : 0;
}

void Tilemap_set_layer_elevation(Tilemap *m, int32_t layer, int32_t elevation) {
    if (!m || layer < 0 || layer >= m->layer_count) return;
    elevation = elevation < 0 ? 0 : elevation >= m->levels ? m->levels - 1 : elevation;
    if (m->layer_elevation[layer] == (uint8_t)elevation) return;
    m->layer_elevation[layer] = (uint8_t)elevation;
    m->lvl_indexed = false;
    m->edit_gen++;
    m->dirty = true;
    all_cells_changed(m);
}

int32_t Tilemap_level_height(const Tilemap *m) { return m ? m->level_height : 0; }
float   Tilemap_tilt(const Tilemap *m)         { return m ? m->tilt : 0.0f; }
int32_t Tilemap_slope_run(const Tilemap *m)    { return m ? m->slope_run : 0; }

void Tilemap_set_elevation_style(Tilemap *m, int32_t level_height, float tilt, int32_t slope_run) {
    if (!m) return;
    level_height = level_height < 1 ? 1 : level_height > TILEMAP_MAX_LEVEL_HEIGHT ? TILEMAP_MAX_LEVEL_HEIGHT : level_height;
    tilt = tilt < 0.0f ? 0.0f : tilt > 90.0f ? 90.0f : tilt;
    int32_t max_run = m->tile * 6;
    slope_run = slope_run < 1 ? 1 : slope_run > max_run ? max_run : slope_run;
    if (m->level_height == level_height && m->tilt == tilt && m->slope_run == slope_run) return;
    bool shape = m->level_height != level_height || m->slope_run != slope_run;
    m->level_height = (int16_t)level_height;
    m->tilt = tilt;
    m->slope_run = (int16_t)slope_run;
    m->dirty = true;
    if (shape) meshes_stale(m, 0, 0, m->w - 1, m->h - 1);
}

int32_t Tilemap_layer_height(const Tilemap *m, int32_t layer) {
    if (!m || layer < 0 || layer >= m->layer_count) return 0;
    return m->layer_elevation[layer] * m->level_height;
}

static int32_t surface_layer(const Tilemap *m, int32_t x, int32_t y, int32_t below) {
    int32_t best = -1, best_level = -1;
    if (!m || x < 0 || y < 0 || x >= m->w || y >= m->h) return -1;
    for (int32_t l = 0; l < m->layer_count; l++) {
        int32_t e = m->layer_elevation[l];
        if (!is_terrain_layer(m, l) || e <= best_level || e >= below) continue;
        int32_t s = terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]);
        if (s > 0 && s <= m->terrain_count) { best = l; best_level = e; }
    }
    return best;
}

static int32_t floor_layer(const Tilemap *m, int32_t x, int32_t y, int32_t z);

int32_t Tilemap_level_at(const Tilemap *m, int32_t x, int32_t y, int32_t *slot) {
    int32_t l = m ? floor_layer(m, x, y, m->levels - 1) : -1;
    if (slot) *slot = l >= 0 ? terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]) : 0;
    return l >= 0 ? m->layer_elevation[l] : -1;
}

static void level_index(Tilemap *m) {
    for (int32_t k = 0; k < TILEMAP_MAX_LEVELS; k++) m->lvl_first[k] = -1;
    for (int32_t l = m->layer_count - 1; l >= 0; l--) {
        m->lvl_next[l] = -1;
        if (!is_terrain_layer(m, l)) continue;
        int32_t k = m->layer_elevation[l];
        m->lvl_next[l] = m->lvl_first[k];
        m->lvl_first[k] = (int8_t)l;
    }
    m->lvl_indexed = true;
}

static int32_t block_layer(const Tilemap *m, int32_t x, int32_t y, int32_t z) {
    if (!m || z < 0 || z >= m->levels || x < 0 || y < 0 || x >= m->w || y >= m->h) return -1;
    if (!m->lvl_indexed) level_index((Tilemap *)m);
    for (int32_t l = m->lvl_first[z]; l >= 0; l = m->lvl_next[l]) {
        int32_t s = terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]);
        if (s > 0 && s <= m->terrain_count) return l;
    }
    return -1;
}

enum { BLOCK_NONE = 0, BLOCK_SOLID, BLOCK_WATER };

static bool slot_water(const Tilemap *m, int32_t slot) {
    return slot >= 1 && slot <= m->terrain_count && m->terrains[slot - 1].water;
}

static int32_t block_kind(const Tilemap *m, int32_t x, int32_t y, int32_t z, int32_t *layer) {
    int32_t l = block_layer(m, x, y, z);
    if (layer) *layer = l;
    if (l < 0) return BLOCK_NONE;
    return slot_water(m, terrain_cell_slot(m->cells[l][(size_t)y * m->w + x])) ? BLOCK_WATER : BLOCK_SOLID;
}

static bool solid_at(const Tilemap *m, int32_t x, int32_t y, int32_t z) {
    return block_kind(m, x, y, z, NULL) == BLOCK_SOLID;
}

static int32_t floor_here(const Tilemap *m, int32_t x, int32_t y, int32_t k) {
    int32_t l;
    int32_t kind = block_kind(m, x, y, k, &l);
    if (kind == BLOCK_NONE || (k > 0 && kind != BLOCK_SOLID)) return -1;
    if (k + 1 < m->levels && solid_at(m, x, y, k + 1)) return -1;
    return l;
}

static int32_t floor_layer(const Tilemap *m, int32_t x, int32_t y, int32_t z) {
    if (!m || x < 0 || y < 0 || x >= m->w || y >= m->h) return -1;
    if (z >= m->levels) z = m->levels - 1;
    for (int32_t k = z; k >= 0; k--) {
        int32_t l = floor_here(m, x, y, k);
        if (l >= 0) return l;
    }
    return -1;
}

static int32_t water_over(const Tilemap *m, int32_t x, int32_t y, int32_t k, int32_t fl, bool *deep) {
    TileRef f = fl >= 0 ? Tilemap_flags(m, fl, x, y) : 0;
    if (deep) *deep = (f & TILEMAP_CELL_WATER) != 0;
    int32_t d = 0;
    while (k + 1 + d < m->levels && block_kind(m, x, y, k + 1 + d, NULL) == BLOCK_WATER) d++;
    return d;
}

int32_t Tilemap_floor_at(const Tilemap *m, int32_t x, int32_t y, int32_t z, int32_t *slot) {
    int32_t l = floor_layer(m, x, y, z);
    if (slot) *slot = l >= 0 ? terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]) : 0;
    return l >= 0 ? m->layer_elevation[l] : -1;
}

bool Tilemap_covered(const Tilemap *m, int32_t x, int32_t y, int32_t z) {
    if (!m) return false;
    for (int32_t k = z + 1; k < m->levels; k++) {
        if (solid_at(m, x, y, k)) return true;
    }
    return false;
}

int32_t Tilemap_water_at(const Tilemap *m, int32_t x, int32_t y, int32_t z, bool *deep) {
    if (deep) *deep = false;
    if (!m) return 0;
    if (z < 0) z = Tilemap_level_at(m, x, y, NULL);
    int32_t l = z >= 0 ? floor_here(m, x, y, z) : -1;
    if (l < 0) return 0;
    return water_over(m, x, y, z, l, deep);
}

#define SLOPE_CODE(d) ((d) + 0.5f) * 32.0f
#define SLOPE_NONE 255

static float slope_decode(uint8_t c) { return c / 32.0f - 0.5f; }

enum { SC_EMPTY = 0, SC_SOLID, SC_WATER };

static void slope_distances(const uint8_t *kind, const uint8_t *outside, uint32_t through, float *dist,
                            int32_t ww, int32_t wh) {
    size_t wn = (size_t)ww * wh;
    for (size_t i = 0; i < wn; i++) dist[i] = 1e9f;
    for (int32_t y = 0; y < wh; y++) {
        for (int32_t x = 0; x < ww; x++) {
            size_t i = (size_t)y * ww + x;
            if (!((through >> kind[i]) & 1u)) continue;
            for (int32_t dy = -1; dy <= 1; dy++) {
                for (int32_t dx = -1; dx <= 1; dx++) {
                    int32_t nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= ww || ny >= wh) continue;
                    if (!outside[(size_t)ny * ww + nx]) continue;
                    float d = (dx != 0 && dy != 0) ? 0.7071f : 0.5f;
                    if (d < dist[i]) dist[i] = d;
                }
            }
        }
    }
    for (int32_t pass = 0; pass < 2; pass++) {
        int32_t step = pass == 0 ? 1 : -1;
        for (size_t k = 0; k < wn; k++) {
            size_t idx = pass == 0 ? k : wn - 1 - k;
            int32_t x = (int32_t)(idx % ww), y = (int32_t)(idx / ww);
            if (!((through >> kind[idx]) & 1u)) continue;
            const int32_t ox[4] = { -step, 0, -step, step }, oy[4] = { 0, -step, -step, -step };
            const float   ow[4] = { 1.0f, 1.0f, 1.4142f, 1.4142f };
            for (int32_t j = 0; j < 4; j++) {
                int32_t nx = x + ox[j], ny = y + oy[j];
                if (nx < 0 || ny < 0 || nx >= ww || ny >= wh) continue;
                size_t ni = (size_t)ny * ww + nx;
                if (!((through >> kind[ni]) & 1u)) continue;
                if (dist[ni] + ow[j] < dist[idx]) dist[idx] = dist[ni] + ow[j];
            }
        }
    }
}

static void slope_window(Tilemap *m, int32_t layer, int32_t ox0, int32_t oy0, int32_t ox1, int32_t oy1) {
    uint8_t *out = m->slopes[layer];
    int32_t level = m->layer_elevation[layer];
    for (int32_t y = oy0; y <= oy1; y++) memset(&out[(size_t)y * m->w + ox0], SLOPE_NONE, (size_t)(ox1 - ox0 + 1));
    if (m->wslopes[layer]) {
        for (int32_t y = oy0; y <= oy1; y++) memset(&m->wslopes[layer][(size_t)y * m->w + ox0], SLOPE_NONE, (size_t)(ox1 - ox0 + 1));
    }
    if (level == 0 || !is_terrain_layer(m, layer)) return;
    int32_t wx0 = ox0 - SYNC_REACH < 0 ? 0 : ox0 - SYNC_REACH, wy0 = oy0 - SYNC_REACH < 0 ? 0 : oy0 - SYNC_REACH;
    int32_t wx1 = ox1 + SYNC_REACH >= m->w ? m->w - 1 : ox1 + SYNC_REACH;
    int32_t wy1 = oy1 + SYNC_REACH >= m->h ? m->h - 1 : oy1 + SYNC_REACH;
    int32_t ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;
    size_t wn = (size_t)ww * wh;
    float *dist = malloc(wn * sizeof(float));
    uint8_t *kind = malloc(wn), *dry = malloc(wn), *low = malloc(wn);
    if (!dist || !kind || !dry || !low) { free(dist); free(kind); free(dry); free(low); return; }
    const TileRef *cells = m->cells[layer];
    bool water = false;
    for (int32_t y = wy0; y <= wy1; y++) {
        for (int32_t x = wx0; x <= wx1; x++) {
            size_t i = (size_t)(y - wy0) * ww + (x - wx0);
            int32_t slot = terrain_cell_slot(cells[(size_t)y * m->w + x]);
            kind[i] = slot == 0 ? SC_EMPTY : slot_water(m, slot) ? SC_WATER : SC_SOLID;
            dry[i] = low[i] = 0;
            if (kind[i] == SC_EMPTY) {
                int32_t below = surface_layer(m, x, y, level);
                dry[i] = low[i] = (below >= 0 ? m->layer_elevation[below] : -1) == level - 1;
            } else if (kind[i] == SC_WATER) {
                water = true;
                low[i] = block_kind(m, x, y, level - 1, NULL) == BLOCK_SOLID;
            }
        }
    }
    if (water && !m->wslopes[layer]) {
        size_t n = cell_count(m);
        m->wslopes[layer] = malloc(n);
        if (m->wslopes[layer]) memcpy(m->wslopes[layer], out, n);
    }
    for (int32_t f = 0; f < 2; f++) {
        uint8_t *dst = f == 0 ? out : m->wslopes[layer];
        if (!dst) continue;
        const uint8_t *outside = f == 0 ? low : dry;
        uint32_t through = f == 0 ? 1u << SC_SOLID : (1u << SC_SOLID) | (1u << SC_WATER);
        slope_distances(kind, outside, through, dist, ww, wh);
        for (int32_t y = oy0; y <= oy1; y++) {
            for (int32_t x = ox0; x <= ox1; x++) {
                size_t i = (size_t)(y - wy0) * ww + (x - wx0), g = (size_t)y * m->w + x;
                if (outside[i]) { dst[g] = 0; continue; }
                if (!((through >> kind[i]) & 1u) || dist[i] >= 1e8f) continue;
                float c = SLOPE_CODE(dist[i]);
                dst[g] = (uint8_t)(c > 255.0f ? 255.0f : c + 0.5f);
            }
        }
    }
    free(dist);
    free(kind);
    free(dry);
    free(low);
}

static const uint8_t *slope_field(const Tilemap *cm, int32_t layer) {
    Tilemap *m = (Tilemap *)cm;
    size_t n = cell_count(m);
    int32_t *r = m->slope_rect[layer];
    if (!m->slopes[layer]) {
        m->slopes[layer] = malloc(n);
        if (!m->slopes[layer]) return NULL;
        m->slopes_stale[layer] = true;
    }
    if (m->slopes_stale[layer]) {
        slope_window(m, layer, 0, 0, m->w - 1, m->h - 1);
    } else if (r[2] >= r[0]) {
        slope_window(m, layer, r[0] < 0 ? 0 : r[0], r[1] < 0 ? 0 : r[1],
                     r[2] >= m->w ? m->w - 1 : r[2], r[3] >= m->h ? m->h - 1 : r[3]);
    }
    m->slopes_stale[layer] = false;
    r[0] = r[1] = 0;
    r[2] = r[3] = -1;
    return m->slopes[layer];
}

static const uint8_t *water_slope_field(const Tilemap *m, int32_t layer) {
    const uint8_t *ground = slope_field(m, layer);
    return m->wslopes[layer] ? m->wslopes[layer] : ground;
}

static float smoothstepf(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

typedef struct StairGeom {
    int8_t facing;
    int8_t index;
    int8_t depth;
    int8_t drop;
} StairGeom;

static const int8_t STAIR_DX[4] = { 0, 1, 0, -1 };
static const int8_t STAIR_DY[4] = { -1, 0, 1, 0 };

static bool stairs_cell(const Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= m->w || y >= m->h) return false;
    int32_t slot = terrain_cell_slot(m->cells[layer][(size_t)y * m->w + x]);
    return slot >= 1 && slot <= m->terrain_count && m->terrains[slot - 1].stairs;
}

static bool stairs_geometry(const Tilemap *m, int32_t layer, int32_t x, int32_t y, StairGeom *g) {
    int32_t level = m->layer_elevation[layer];
    if (level == 0 || !stairs_cell(m, layer, x, y)) return false;
    static const int8_t order[4] = { 2, 1, 3, 0 };
    for (int32_t k = 0; k < 4; k++) {
        int32_t d = order[k], dx = STAIR_DX[d], dy = STAIR_DY[d];
        int32_t ahead = 0, nx = x, ny = y;
        while (stairs_cell(m, layer, nx, ny)) { nx += dx; ny += dy; ahead++; }
        if (nx < 0 || ny < 0 || nx >= m->w || ny >= m->h) continue;
        if (terrain_cell_slot(m->cells[layer][(size_t)ny * m->w + nx]) != 0) continue;
        int32_t bl = surface_layer(m, nx, ny, level);
        int32_t below = bl >= 0 ? m->layer_elevation[bl] : 0;
        if (below >= level) continue;
        int32_t behind = 0, bx = x - dx, by = y - dy;
        while (stairs_cell(m, layer, bx, by)) { bx -= dx; by -= dy; behind++; }
        int32_t depth = ahead + behind;
        g->facing = (int8_t)d;
        g->index = (int8_t)(ahead - 1 > 7 ? 7 : ahead - 1);
        g->depth = (int8_t)(depth > 8 ? 8 : depth);
        g->drop = (int8_t)(level - below > 7 ? 7 : level - below);
        return true;
    }
    return false;
}

static float stairs_t(const Tilemap *m, const StairGeom *g, int32_t x, int32_t y, float wx, float wy) {
    float ts = (float)m->tile;
    float fx = wx / ts - x, fy = wy / ts - y;
    float u = g->facing == 0 ? fy : g->facing == 1 ? 1.0f - fx : g->facing == 2 ? 1.0f - fy : fx;
    return (g->index + u) / g->depth;
}

static float field_dist(const Tilemap *m, const uint8_t *sl, float wx, float wy) {
    float ts = (float)m->tile;
    float gx = wx / ts - 0.5f, gy = wy / ts - 0.5f;
    int32_t cx = (int32_t)floorf(gx), cy = (int32_t)floorf(gy);
    float fx = gx - cx, fy = gy - cy;
    float d[4];
    for (int32_t k = 0; k < 4; k++) {
        int32_t x = cx + (k & 1), y = cy + (k >> 1);
        x = x < 0 ? 0 : x >= m->w ? m->w - 1 : x;
        y = y < 0 ? 0 : y >= m->h ? m->h - 1 : y;
        d[k] = slope_decode(sl[(size_t)y * m->w + x]);
    }
    float dc = (d[0] * (1 - fx) + d[1] * fx) * (1 - fy) + (d[2] * (1 - fx) + d[3] * fx) * fy;
    return dc * ts;
}

static float incline_ramp(const Tilemap *m, int32_t layer, float wx, float wy) {
    const uint8_t *sl = slope_field(m, layer);
    if (!sl || m->layer_elevation[layer] == 0) return 1.0f;
    return smoothstepf(0.0f, (float)m->slope_run, field_dist(m, sl, wx, wy));
}

static float layer_surface(const Tilemap *m, int32_t layer, float wx, float wy) {
    float top = (float)(m->layer_elevation[layer] * m->level_height);
    float ts0 = (float)m->tile;
    int32_t sx = (int32_t)floorf(wx / ts0), sy = (int32_t)floorf(wy / ts0);
    StairGeom sg;
    if (stairs_geometry(m, layer, sx, sy, &sg)) {
        float t = stairs_t(m, &sg, sx, sy, wx, wy);
        return top - sg.drop * m->level_height * (1.0f - t);
    }
    return top - m->level_height * (1.0f - incline_ramp(m, layer, wx, wy));
}

#define STEP_PX 4.0f

static float field_draw_height(const Tilemap *m, int32_t layer, const uint8_t *sl, float wx, float wy) {
    float top = (float)(m->layer_elevation[layer] * m->level_height);
    if (!sl || m->layer_elevation[layer] == 0) return top;
    float r = smoothstepf(0.0f, (float)m->slope_run, field_dist(m, sl, wx, wy));
    return top - (m->level_height - 1.0f) * (1.0f - r);
}

static float stairs_draw_height(const Tilemap *m, int32_t layer, const StairGeom *g,
                                int32_t x, int32_t y, float wx, float wy) {
    float top = (float)(m->layer_elevation[layer] * m->level_height);
    float t = stairs_t(m, g, x, y, wx, wy);
    float total = g->drop * (float)m->level_height;
    float steps = fmaxf(1.0f, floorf(total / STEP_PX + 0.5f));
    float k = fminf(floorf(t * steps), steps - 1.0f);
    return top - total + total * (k + 1.0f) / steps;
}

float Tilemap_floor_height(const Tilemap *m, float wx, float wy, int32_t z, int32_t *level) {
    if (level) *level = -1;
    if (!m) return 0.0f;
    float ts = (float)m->tile;
    int32_t l = floor_layer(m, (int32_t)floorf(wx / ts), (int32_t)floorf(wy / ts), z);
    if (l < 0) return 0.0f;
    if (level) *level = m->layer_elevation[l];
    return layer_surface(m, l, wx, wy);
}

float Tilemap_height_at(const Tilemap *m, float wx, float wy, int32_t *level) {
    if (level) *level = -1;
    if (!m) return 0.0f;
    float ts = (float)m->tile;
    int32_t l = floor_layer(m, (int32_t)floorf(wx / ts), (int32_t)floorf(wy / ts), m->levels - 1);
    if (l < 0) return 0.0f;
    if (level) *level = m->layer_elevation[l];
    return layer_surface(m, l, wx, wy);
}

TileRef Tilemap_flags(const Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    if (!in_bounds(m, layer, x, y) || !is_terrain_layer(m, layer)) return 0;
    TileRef v = m->cells[layer][(size_t)y * m->w + x];
    int32_t slot = terrain_cell_slot(v);
    if (slot == 0 || slot > m->terrain_count) return 0;
    const TilemapTerrain *t = &m->terrains[slot - 1];
    return (v & TILEMAP_CELL_FLAGS) | (t->solid ? TILEMAP_CELL_SOLID : 0) | (t->water ? TILEMAP_CELL_WATER : 0);
}

int32_t Tilemap_paint_flags(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                            int32_t w, int32_t h, TileRef flags, bool on) {
    if (!m || layer < 0 || layer >= m->layer_count || !is_terrain_layer(m, layer)) return 0;
    flags &= TILEMAP_CELL_FLAGS;
    int32_t changed = 0;
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            if (!in_bounds(m, layer, x + i, y + j)) continue;
            TileRef *c = &m->cells[layer][(size_t)(y + j) * m->w + (x + i)];
            if (terrain_cell_slot(*c) == 0) continue;
            TileRef v = on ? (*c | flags) : (*c & ~flags);
            if (v != *c) { undo_touch(m, layer, x + i, y + j); *c = v; changed++; }
        }
    }
    if (changed) m->dirty = true;
    return changed;
}

static int32_t standable(const Tilemap *m, int32_t x, int32_t y, int32_t k, int32_t clearance, TilemapStep *why) {
    int32_t l = floor_here(m, x, y, k);
    if (l < 0) return -1;
    for (int32_t h = 2; h <= clearance; h++) {
        if (solid_at(m, x, y, k + h)) {
            if (why) *why = TILEMAP_STEP_LOW;
            return -1;
        }
    }
    return l;
}

static bool stairs_slot(const Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    int32_t slot = terrain_cell_slot(m->cells[layer][(size_t)y * m->w + x]);
    return slot >= 1 && slot <= m->terrain_count && m->terrains[slot - 1].stairs;
}

typedef struct FloorState { int32_t layer, depth, eff; uint8_t water; } FloorState;

static TilemapStep floor_state(const Tilemap *m, int32_t x, int32_t y, int32_t k, const TilemapWalker *w,
                               FloorState *s) {
    TilemapStep why = TILEMAP_STEP_VOID;
    int32_t l = standable(m, x, y, k, w->clearance, &why);
    if (l < 0) return why;
    if ((Tilemap_flags(m, l, x, y) & TILEMAP_CELL_SOLID) && !(w->pass & TILEMAP_CELL_SOLID)) return TILEMAP_STEP_SOLID;
    bool deep;
    int32_t d = water_over(m, x, y, k, l, &deep);
    *s = (FloorState){ l, d, k, 0 };
    if (d == 0 && !deep) return TILEMAP_STEP_OK;
    if (!deep && d <= w->wade) { s->water = 1; return TILEMAP_STEP_OK; }
    if (!(w->pass & TILEMAP_CELL_WATER)) return TILEMAP_STEP_WATER;
    s->water = 2;
    s->eff = k + d;
    return TILEMAP_STEP_OK;
}

static TilemapStep worse(TilemapStep a, TilemapStep b) {
    static const int8_t rank[] = { [TILEMAP_STEP_OK] = -1, [TILEMAP_STEP_VOID] = 0, [TILEMAP_STEP_CLIFF] = 1,
                                   [TILEMAP_STEP_LOW] = 2, [TILEMAP_STEP_WATER] = 3, [TILEMAP_STEP_SOLID] = 4 };
    return rank[b] > rank[a] ? b : a;
}

TilemapStep Tilemap_can_step(const Tilemap *m, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1,
                             const TilemapWalker *walker, int32_t *z1, int32_t *cost) {
    if (!m || x1 < 0 || y1 < 0 || x1 >= m->w || y1 >= m->h) return TILEMAP_STEP_VOID;
    TilemapWalker w = walker ? *walker : TILEMAP_WALKER_DEFAULT;
    if (w.clearance < 1) w.clearance = 1;
    if (w.wade_cost < 1) w.wade_cost = 1;
    if (w.swim_cost < 1) w.swim_cost = 1;
    if (z0 < 0) z0 = Tilemap_level_at(m, x0, y0, NULL);
    bool from_void = z0 < 0;
    if (from_void) z0 = Tilemap_level_at(m, x1, y1, NULL);
    FloorState a = { -1, 0, z0, 0 };
    if (!from_void && floor_state(m, x0, y0, z0, &w, &a) != TILEMAP_STEP_OK) {
        a = (FloorState){ block_layer(m, x0, y0, z0), 0, z0, 0 };
    }
    TilemapStep why = TILEMAP_STEP_VOID;
    for (int32_t i = 0; i < 2 * m->levels; i++) {
        int32_t d = (i + 1) / 2 * (i % 2 ? 1 : -1);
        int32_t k = z0 + d;
        if (k < 0 || k >= m->levels) continue;
        FloorState b;
        TilemapStep r = floor_state(m, x1, y1, k, &w, &b);
        if (r != TILEMAP_STEP_OK) { why = worse(why, r); continue; }
        int32_t climb = b.eff - a.eff;
        if (!from_void && (climb >= 2 || climb <= -2)) {
            bool stairs = climb > 0 ? stairs_slot(m, b.layer, x1, y1) : (a.layer >= 0 && stairs_slot(m, a.layer, x0, y0));
            if (!stairs) { why = worse(why, TILEMAP_STEP_CLIFF); continue; }
        }
        if (z1) *z1 = k;
        if (cost) *cost = b.water == 1 ? w.wade_cost : b.water == 2 ? w.swim_cost : 1;
        return TILEMAP_STEP_OK;
    }
    return why;
}

#define REACH_STATE_BITS 18

typedef struct ReachWindow {
    int32_t x0, y0, side, levels;
    int32_t count;
    int16_t *cost;
    int16_t *steps;
    int32_t *from;
    int32_t *order;
    int32_t *heap;
    int32_t heap_n;
    uint8_t *done;
} ReachWindow;

static bool reach_alloc(ReachWindow *rw, int32_t max_cost, int32_t levels) {
    rw->side = 2 * max_cost + 1;
    rw->levels = levels;
    size_t n = (size_t)rw->side * rw->side * levels;
    rw->cost = malloc(n * sizeof(int16_t));
    rw->steps = malloc(n * sizeof(int16_t));
    rw->from = malloc(n * sizeof(int32_t));
    rw->order = malloc(n * sizeof(int32_t));
    rw->heap = malloc((4 * n + 1) * sizeof(int32_t));
    rw->done = calloc(n, 1);
    if (!rw->cost || !rw->steps || !rw->from || !rw->order || !rw->heap || !rw->done) return false;
    memset(rw->cost, 0xFF, n * sizeof(int16_t));
    return true;
}

static void reach_free(ReachWindow *rw) {
    free(rw->cost); free(rw->steps); free(rw->from); free(rw->order); free(rw->heap); free(rw->done);
}

static void heap_push(ReachWindow *rw, int32_t key) {
    int32_t i = rw->heap_n++;
    while (i > 0 && rw->heap[(i - 1) / 2] > key) { rw->heap[i] = rw->heap[(i - 1) / 2]; i = (i - 1) / 2; }
    rw->heap[i] = key;
}

static int32_t heap_pop(ReachWindow *rw) {
    int32_t top = rw->heap[0], last = rw->heap[--rw->heap_n], i = 0;
    for (;;) {
        int32_t c = 2 * i + 1;
        if (c >= rw->heap_n) break;
        if (c + 1 < rw->heap_n && rw->heap[c + 1] < rw->heap[c]) c++;
        if (rw->heap[c] >= last) break;
        rw->heap[i] = rw->heap[c];
        i = c;
    }
    if (rw->heap_n > 0) rw->heap[i] = last;
    return top;
}

static void reach_window(const Tilemap *m, int32_t x, int32_t y, int32_t z, int32_t max_cost,
                         const TilemapWalker *w, ReachWindow *rw) {
    rw->x0 = x - max_cost;
    rw->y0 = y - max_cost;
    rw->count = 0;
    rw->heap_n = 0;
    if (!m || x < 0 || y < 0 || x >= m->w || y >= m->h) return;
    if (z < 0) z = Tilemap_level_at(m, x, y, NULL);
    if (z < 0 || z >= rw->levels) return;
    int32_t L = rw->levels;
    int32_t start = (max_cost * rw->side + max_cost) * L + z;
    rw->cost[start] = 0;
    rw->steps[start] = 0;
    rw->from[start] = -1;
    heap_push(rw, start);
    const int32_t mask = (1 << REACH_STATE_BITS) - 1;
    static const int32_t dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    while (rw->heap_n > 0) {
        int32_t key = heap_pop(rw), i = key & mask;
        if (rw->done[i] || (key >> REACH_STATE_BITS) != rw->cost[i]) continue;
        rw->done[i] = 1;
        rw->order[rw->count++] = i;
        int32_t cell = i / L, cz = i % L;
        int32_t cx = rw->x0 + cell % rw->side, cy = rw->y0 + cell / rw->side;
        for (int32_t k = 0; k < 4; k++) {
            int32_t nx = cx + dx[k], ny = cy + dy[k];
            int32_t wx = nx - rw->x0, wy = ny - rw->y0;
            if (wx < 0 || wy < 0 || wx >= rw->side || wy >= rw->side) continue;
            int32_t nz, step = 1;
            if (Tilemap_can_step(m, cx, cy, cz, nx, ny, w, &nz, &step) != TILEMAP_STEP_OK) continue;
            int32_t c = rw->cost[i] + step;
            if (c > max_cost) continue;
            int32_t j = (wy * rw->side + wx) * L + nz;
            if (rw->done[j] || (rw->cost[j] >= 0 && rw->cost[j] <= c)) continue;
            rw->cost[j] = (int16_t)c;
            rw->steps[j] = (int16_t)(rw->steps[i] + 1);
            rw->from[j] = i;
            heap_push(rw, c << REACH_STATE_BITS | j);
        }
    }
}

static int32_t clamp_steps(int32_t s) { return s < 0 ? 0 : s > TILEMAP_MAX_STEPS ? TILEMAP_MAX_STEPS : s; }

int32_t Tilemap_reach(const Tilemap *m, int32_t x, int32_t y, int32_t z, int32_t max_cost, const TilemapWalker *w,
                      TilemapReach *out, int32_t cap) {
    if (!m) return 0;
    max_cost = clamp_steps(max_cost);
    TilemapWalker dw = TILEMAP_WALKER_DEFAULT;
    ReachWindow rw = { 0 };
    if (!reach_alloc(&rw, max_cost, m->levels)) { reach_free(&rw); return 0; }
    reach_window(m, x, y, z, max_cost, w ? w : &dw, &rw);
    int32_t n = rw.count < cap ? rw.count : cap, L = rw.levels;
    for (int32_t k = 0; k < n; k++) {
        int32_t i = rw.order[k], f = rw.from[i];
        int32_t c = i / L, fc = f / L;
        out[k] = (TilemapReach){ rw.x0 + c % rw.side, rw.y0 + c / rw.side, i % L, rw.steps[i], rw.cost[i],
                                 f < 0 ? -1 : rw.x0 + fc % rw.side, f < 0 ? -1 : rw.y0 + fc / rw.side,
                                 f < 0 ? -1 : f % L };
    }
    reach_free(&rw);
    return n;
}

int32_t Tilemap_path(const Tilemap *m, int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1,
                     int32_t max_cost, const TilemapWalker *w, int32_t *xyz, int32_t cap) {
    if (!m) return 0;
    max_cost = clamp_steps(max_cost);
    TilemapWalker dw = TILEMAP_WALKER_DEFAULT;
    ReachWindow rw = { 0 };
    if (!reach_alloc(&rw, max_cost, m->levels)) { reach_free(&rw); return 0; }
    reach_window(m, x0, y0, z0, max_cost, w ? w : &dw, &rw);
    int32_t len = 0, L = rw.levels;
    int32_t wx = x1 - rw.x0, wy = y1 - rw.y0;
    if (rw.count > 0 && wx >= 0 && wy >= 0 && wx < rw.side && wy < rw.side) {
        int32_t best = -1;
        for (int32_t k = 0; k < L; k++) {
            int32_t i = (wy * rw.side + wx) * L + k;
            if (rw.cost[i] < 0 || (z1 >= 0 && k != z1)) continue;
            if (best < 0 || rw.cost[i] < rw.cost[best]) best = i;
        }
        if (best >= 0 && rw.steps[best] + 1 <= cap) {
            len = rw.steps[best] + 1;
            int32_t i = best;
            for (int32_t k = len - 1; k >= 0; k--) {
                int32_t c = i / L;
                xyz[k * 3 + 0] = rw.x0 + c % rw.side;
                xyz[k * 3 + 1] = rw.y0 + c / rw.side;
                xyz[k * 3 + 2] = i % L;
                i = rw.from[i];
            }
        }
    }
    reach_free(&rw);
    return len;
}

static bool solid_point(const Tilemap *m, float x, float y, float z, int32_t max_level, const uint8_t *cut) {
    float ts = (float)m->tile, lh = (float)m->level_height;
    int32_t cx = (int32_t)floorf(x / ts), cy = (int32_t)floorf(y / ts);
    if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h) return false;
    if (z < 0.0f) return block_layer(m, cx, cy, 0) >= 0;
    int32_t top_level = max_level >= 0 && max_level < m->levels - 1 ? max_level : m->levels - 1;
    if (cut && cut[(size_t)cy * m->w + cx] < top_level) top_level = cut[(size_t)cy * m->w + cx];
    int32_t k = (int32_t)ceilf(z / lh);
    if (k < 1) k = 1;
    int32_t k1 = k;
    while (k1 >= 1 && !solid_at(m, cx, cy, k1)) k1--;
    if (k1 < 1 || k1 > top_level) return false;
    int32_t k2 = k1;
    while (k2 + 1 <= top_level && solid_at(m, cx, cy, k2 + 1)) k2++;
    float top;
    if (k2 == top_level && k2 < m->levels - 1 && solid_at(m, cx, cy, k2 + 1)) {
        top = k2 * lh;
    } else {
        top = layer_surface(m, block_layer(m, cx, cy, k2), x, y);
    }
    return z < top;
}

static bool floor_under(const Tilemap *m, float x, float y, float z, float *h, TileRef *flags) {
    float ts = (float)m->tile, lh = (float)m->level_height;
    int32_t cx = (int32_t)floorf(x / ts), cy = (int32_t)floorf(y / ts);
    if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h) return false;
    int32_t k = (int32_t)ceilf((z + 1.0f) / lh);
    k = k < 0 ? 0 : k >= m->levels ? m->levels - 1 : k;
    for (; k >= 0; k--) {
        int32_t l = floor_layer(m, cx, cy, k);
        if (l < 0) return false;
        float fh = layer_surface(m, l, x, y);
        int32_t e = m->layer_elevation[l];
        if (fh <= z + 1.0f) {
            *h = fh;
            if (flags) *flags = Tilemap_flags(m, l, cx, cy);
            return true;
        }
        k = e;
    }
    return false;
}

static bool arc_blocked(const Tilemap *m, Vector3 p, float obstacle, Vector3 a, Vector3 b) {
    if (solid_point(m, p.x, p.y, p.z + 1.0f, -1, NULL)) return true;
    float h;
    TileRef f;
    if (obstacle > 0.0f && floor_under(m, p.x, p.y, p.z, &h, &f) && (f & TILEMAP_CELL_SOLID) && p.z < h + obstacle) {
        float ts = (float)m->tile;
        int32_t cx = (int32_t)floorf(p.x / ts), cy = (int32_t)floorf(p.y / ts);
        bool at_a = cx == (int32_t)floorf(a.x / ts) && cy == (int32_t)floorf(a.y / ts);
        bool at_b = cx == (int32_t)floorf(b.x / ts) && cy == (int32_t)floorf(b.y / ts);
        return !at_a && !at_b;
    }
    return false;
}

static float arc_k(Vector3 a, Vector3 b, float peak) {
    float d = b.z - a.z;
    float hgt = fmaxf(a.z, b.z) + fmaxf(peak, 0.0f) - a.z;
    if (hgt <= 0.0f) return 0.0f;
    return 2.0f * hgt - d + 2.0f * sqrtf(fmaxf(hgt * (hgt - d), 0.0f));
}

static Vector3 arc_point(Vector3 a, Vector3 b, float k, float t) {
    return (Vector3){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t + k * t * (1.0f - t) };
}

int32_t Tilemap_arc(const Tilemap *m, Vector3 a, Vector3 b, float peak, float obstacle,
                    Vector3 *out, int32_t n, Vector3 *hit) {
    if (!m || n < 2) return -1;
    float k = arc_k(a, b, peak);
    int32_t first = -1;
    for (int32_t i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);
        out[i] = arc_point(a, b, k, t);
        if (first < 0 && i > 0 && i < n - 1 && arc_blocked(m, out[i], obstacle, a, b)) first = i;
    }
    if (first >= 0 && hit) {
        float lo = (float)(first - 1) / (float)(n - 1), hi = (float)first / (float)(n - 1);
        for (int32_t it = 0; it < 14; it++) {
            float mid = 0.5f * (lo + hi);
            if (arc_blocked(m, arc_point(a, b, k, mid), obstacle, a, b)) hi = mid;
            else lo = mid;
        }
        *hit = arc_point(a, b, k, hi);
    }
    return first;
}

int32_t Tilemap_trace(const Tilemap *m, Vector3 p, Vector3 v, float gravity, float dt,
                      Vector3 *out, int32_t n, bool *landed) {
    if (landed) *landed = false;
    if (!m || n < 1 || dt <= 0.0f) return 0;
    int32_t count = 0;
    while (count < n) {
        out[count++] = p;
        Vector3 q = { p.x + v.x * dt, p.y + v.y * dt, p.z + v.z * dt };
        v.z -= gravity * dt;
        if (solid_point(m, q.x, q.y, q.z, -1, NULL)) {
            float lo = 0.0f, hi = 1.0f;
            for (int32_t it = 0; it < 14; it++) {
                float mid = 0.5f * (lo + hi);
                if (solid_point(m, p.x + (q.x - p.x) * mid, p.y + (q.y - p.y) * mid, p.z + (q.z - p.z) * mid, -1, NULL)) hi = mid;
                else lo = mid;
            }
            if (count < n) {
                out[count++] = (Vector3){ p.x + (q.x - p.x) * hi, p.y + (q.y - p.y) * hi, p.z + (q.z - p.z) * hi };
            }
            if (landed) *landed = true;
            break;
        }
        if (q.z < -4.0f * TILEMAP_MAX_LEVEL_HEIGHT * TILEMAP_MAX_LEVELS) break;
        p = q;
    }
    return count;
}

static bool raycast_cut(const Tilemap *m, Vector3 o, Vector3 dir, float max_dist, int32_t max_level,
                        const uint8_t *cut, Vector3 *hit) {
    if (!m) return false;
    float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f) return false;
    dir = (Vector3){ dir.x / len, dir.y / len, dir.z / len };
    int32_t top_level = max_level >= 0 && max_level < m->levels - 1 ? max_level : m->levels - 1;
    float top = (float)(top_level * m->level_height);
    float t = 0.0f;
    if (o.z > top) {
        if (dir.z >= 0.0f) return false;
        t = (o.z - top) / -dir.z;
    }
    const float step = 0.5f;
    float prev = t;
    for (; t <= max_dist; t += step) {
        Vector3 p = { o.x + dir.x * t, o.y + dir.y * t, o.z + dir.z * t };
        if (solid_point(m, p.x, p.y, p.z, max_level, cut)) {
            float lo = prev, hi = t;
            for (int32_t it = 0; it < 14; it++) {
                float mid = 0.5f * (lo + hi);
                if (solid_point(m, o.x + dir.x * mid, o.y + dir.y * mid, o.z + dir.z * mid, max_level, cut)) hi = mid;
                else lo = mid;
            }
            if (hit) *hit = (Vector3){ o.x + dir.x * hi, o.y + dir.y * hi, o.z + dir.z * hi };
            return true;
        }
        if (p.z < -1.0f && dir.z <= 0.0f) return false;
        prev = t;
    }
    return false;
}

bool Tilemap_raycast(const Tilemap *m, Vector3 o, Vector3 dir, float max_dist, int32_t max_level, Vector3 *hit) {
    return raycast_cut(m, o, dir, max_dist, max_level, NULL, hit);
}

static void load_tileset_textures(TilemapTileset *t) {
    char path[MOUNTS_PATH_MAX];
    if (Mounts_resolve(t->image, path, sizeof(path)) >= 0) {
        t->texture = TextureStore_load(t->image, path);
        Texture2D tex = TextureStore_get(t->texture);
        if (tex.id != 0) {
            t->cols = (int16_t)(tex.width / t->tile_w);
            t->rows = (int16_t)(tex.height / t->tile_h);
            if (t->cols > 0 && (int32_t)t->cols * t->rows >= TILEMAP_TILE_BASE) {
                t->rows = (int16_t)((TILEMAP_TILE_BASE - 1) / t->cols);
                TraceLog(LOG_WARNING, "TILEMAP: '%s' has too many tiles; using the first %d rows",
                         t->image, (int)t->rows);
            }
        }
    }
    if (t->skin[0] && Mounts_resolve(t->skin, path, sizeof(path)) >= 0) {
        t->skin_texture = TextureStore_load(t->skin, path);
    }
}

int32_t Tilemap_add_tileset(Tilemap *m, const char *id, const char *image,
                            const char *skin, int32_t tile_w, int32_t tile_h) {
    if (!m || !id || !id[0] || !image || !image[0]) return 0;
    for (int32_t i = 0; i < m->tileset_count; i++) {
        if (strcmp(m->tilesets[i].id, id) == 0) return i + 1;
    }
    if (m->tileset_count >= TILEMAP_MAX_TILESETS) return 0;
    if (strlen(id) >= TILEMAP_ID_MAX || strlen(image) >= TILEMAP_REL_MAX) return 0;
    if (skin && strlen(skin) >= TILEMAP_REL_MAX) return 0;

    TilemapTileset *t = &m->tilesets[m->tileset_count];
    memset(t, 0, sizeof(*t));
    snprintf(t->id, sizeof(t->id), "%s", id);
    snprintf(t->image, sizeof(t->image), "%s", image);
    snprintf(t->skin, sizeof(t->skin), "%s", skin ? skin : "");
    t->tile_w = (int16_t)(tile_w >= 1 && tile_w <= 1024 ? tile_w : m->tile);
    t->tile_h = (int16_t)(tile_h >= 1 && tile_h <= 1024 ? tile_h : m->tile);
    load_tileset_textures(t);
    if (t->cols == 0) TraceLog(LOG_WARNING, "TILEMAP: tileset '%s': image '%s' not loaded", id, image);
    m->tileset_count++;
    return m->tileset_count;
}

int32_t Tilemap_tileset_count(const Tilemap *m) { return m ? m->tileset_count : 0; }

const TilemapTileset *Tilemap_tileset(const Tilemap *m, int32_t slot) {
    return (m && slot >= 1 && slot <= m->tileset_count) ? &m->tilesets[slot - 1] : NULL;
}

static Color image_average(const char *rel) {
    char path[MOUNTS_PATH_MAX];
    if (Mounts_resolve(rel, path, sizeof(path)) < 0) return (Color){ 0 };
    Image img = LoadImage(path);
    if (!img.data) return (Color){ 0 };
    Color *px = LoadImageColors(img);
    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int32_t i = 0; px && i < img.width * img.height; i++) {
        if (px[i].a < 128) continue;
        r += px[i].r; g += px[i].g; b += px[i].b; n++;
    }
    UnloadImageColors(px);
    UnloadImage(img);
    if (n == 0) return (Color){ 0 };
    return (Color){ (uint8_t)(r / n), (uint8_t)(g / n), (uint8_t)(b / n), 255 };
}

#define MISSING_COLOR ((Color){ 255, 0, 255, 255 })

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

int32_t Tilemap_add_terrain(Tilemap *m, const TilemapTerrain *def) {
    if (!m || !def || !def->id[0]) return 0;
    if (strlen(def->id) >= TILEMAP_ID_MAX || strlen(def->image) >= TILEMAP_REL_MAX) return 0;
    int32_t slot = 0;
    for (int32_t i = 0; i < m->terrain_count && !slot; i++) {
        if (strcmp(m->terrains[i].id, def->id) == 0) slot = i + 1;
    }
    bool is_new = slot == 0;
    if (is_new && m->terrain_count >= TILEMAP_MAX_TERRAINS) return 0;

    TilemapTerrain t;
    memset(&t, 0, sizeof(t));
    snprintf(t.id, sizeof(t.id), "%s", def->id);
    snprintf(t.image, sizeof(t.image), "%s", def->image);
    t.color      = def->color;
    t.noise      = clampf(def->noise, 0.0f, 0.45f);
    t.blades     = clampf(def->blades, 0.0f, (float)m->tile);
    t.foam       = clampf(def->foam, 0.0f, (float)m->tile);
    t.foam_speed = def->foam_speed;
    t.foam_color = def->foam_color;
    t.waves      = clampf(def->waves, 0.0f, 1.0f);
    t.sway       = clampf(def->sway, 0.0f, 8.0f);
    t.sway_speed = clampf(def->sway_speed, 0.0f, 8.0f);
    t.flow       = clampf(def->flow, 0.0f, 200.0f);
    t.cliff      = def->cliff;
    t.stairs     = def->stairs;
    t.water      = def->water;
    t.solid      = def->solid;

    if (is_new) slot = ++m->terrain_count;
    TilemapTerrain *dst = &m->terrains[slot - 1];
    if (!is_new && memcmp(dst, &t, sizeof(t)) == 0) return slot;
    *dst = t;
    m->swatch[slot - 1] = t.color.a != 0 ? t.color : image_average(t.image);
    if (m->swatch[slot - 1].a == 0) m->swatch[slot - 1] = MISSING_COLOR;
    m->fills_stale = true;
    all_cells_changed(m);
    return slot;
}

int32_t Tilemap_terrain_count(const Tilemap *m) { return m ? m->terrain_count : 0; }

const TilemapTerrain *Tilemap_terrain(const Tilemap *m, int32_t slot) {
    return (m && slot >= 1 && slot <= m->terrain_count) ? &m->terrains[slot - 1] : NULL;
}

Color Tilemap_terrain_swatch(const Tilemap *m, int32_t slot) {
    return (m && slot >= 1 && slot <= m->terrain_count) ? m->swatch[slot - 1] : (Color){ 0 };
}

TileRef Tilemap_get(const Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    return in_bounds(m, layer, x, y) ? m->cells[layer][(size_t)y * m->w + x] : 0;
}

static TileRef keep_flags(const Tilemap *m, int32_t layer, TileRef old, TileRef ref) {
    if (!is_terrain_layer(m, layer) || terrain_cell_slot(ref) == 0) return ref & ~TILEMAP_CELL_FLAGS;
    return (ref & ~TILEMAP_CELL_FLAGS) | (old & TILEMAP_CELL_FLAGS);
}

static bool put(Tilemap *m, int32_t layer, int32_t x, int32_t y, TileRef ref) {
    TileRef *c = &m->cells[layer][(size_t)y * m->w + x];
    if (is_terrain_layer(m, layer)) ref = keep_flags(m, layer, *c, ref);
    if (*c == ref) return false;
    undo_touch(m, layer, x, y);
    *c = ref;
    cell_changed(m, layer, x, y);
    return true;
}

void Tilemap_set(Tilemap *m, int32_t layer, int32_t x, int32_t y, TileRef ref) {
    if (in_bounds(m, layer, x, y) && put(m, layer, x, y, ref)) m->dirty = true;
}

int32_t Tilemap_stamp(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                      int32_t tileset, int32_t tx, int32_t ty, int32_t w, int32_t h) {
    const TilemapTileset *t = Tilemap_tileset(m, tileset);
    if (!t || t->cols <= 0 || !m || layer < 0 || layer >= m->layer_count) return 0;
    int32_t changed = 0;
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            int32_t col = tx + i, row = ty + j;
            if (col < 0 || row < 0 || col >= t->cols || row >= t->rows) continue;
            if (!in_bounds(m, layer, x + i, y + j)) continue;
            changed += put(m, layer, x + i, y + j, tile_ref(tileset, row * t->cols + col));
        }
    }
    if (changed) m->dirty = true;
    return changed;
}

int32_t Tilemap_fill_empty(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                           int32_t w, int32_t h, TileRef ref) {
    if (!m || layer < 0 || layer >= m->layer_count) return 0;
    int32_t changed = 0;
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            if (!in_bounds(m, layer, x + i, y + j)) continue;
            TileRef c = m->cells[layer][(size_t)(y + j) * m->w + (x + i)];
            bool empty = is_terrain_layer(m, layer) ? terrain_cell_slot(c) == 0 : c == 0;
            if (empty) changed += put(m, layer, x + i, y + j, ref);
        }
    }
    if (changed) m->dirty = true;
    return changed;
}

int32_t Tilemap_fill_rect(Tilemap *m, int32_t layer, int32_t x, int32_t y,
                          int32_t w, int32_t h, TileRef ref) {
    if (!m || layer < 0 || layer >= m->layer_count) return 0;
    int32_t changed = 0;
    for (int32_t j = 0; j < h; j++) {
        for (int32_t i = 0; i < w; i++) {
            if (in_bounds(m, layer, x + i, y + j)) changed += put(m, layer, x + i, y + j, ref);
        }
    }
    if (changed) m->dirty = true;
    return changed;
}

int32_t Tilemap_flood(Tilemap *m, int32_t layer, int32_t x, int32_t y, TileRef ref) {
    if (!in_bounds(m, layer, x, y)) return 0;
    TileRef *cells = m->cells[layer];
    TileRef mask = is_terrain_layer(m, layer) ? ~TILEMAP_CELL_FLAGS : ~(TileRef)0;
    TileRef target = cells[(size_t)y * m->w + x] & mask;
    if (target == (ref & mask)) return 0;

    int32_t *stack = malloc(cell_count(m) * sizeof(int32_t));
    if (!stack) return 0;
    int32_t top = 0, changed = 0;
    int32_t bx0 = x, by0 = y, bx1 = x, by1 = y;
    stack[top++] = y * m->w + x;
    undo_touch(m, layer, x, y);
    cells[y * m->w + x] = keep_flags(m, layer, cells[y * m->w + x], ref);
    while (top > 0) {
        int32_t i = stack[--top];
        int32_t cx = i % m->w, cy = i / m->w;
        changed++;
        if (cx < bx0) bx0 = cx;
        if (cx > bx1) bx1 = cx;
        if (cy < by0) by0 = cy;
        if (cy > by1) by1 = cy;
        const int32_t nx[4] = { cx - 1, cx + 1, cx, cx };
        const int32_t ny[4] = { cy, cy, cy - 1, cy + 1 };
        for (int32_t k = 0; k < 4; k++) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= m->w || ny[k] >= m->h) continue;
            int32_t n = ny[k] * m->w + nx[k];
            if ((cells[n] & mask) != target) continue;
            undo_touch(m, layer, nx[k], ny[k]);
            cells[n] = keep_flags(m, layer, cells[n], ref);
            stack[top++] = n;
        }
    }
    free(stack);
    m->dirty = true;
    region_changed(m, layer, bx0, by0, bx1, by1);
    return changed;
}

#define UNDO_TILE_BYTES ((size_t)UNDO_TILE * UNDO_TILE * sizeof(TileRef))

static size_t undo_bytes(const UndoStack *s) {
    size_t n = 0;
    for (int32_t i = 0; i < s->count; i++) n += (size_t)s->e[i].count * UNDO_TILE_BYTES;
    return n;
}

static void undo_trim(UndoStack *s, bool keep_newest) {
    while (s->count > (keep_newest ? 1 : 0) &&
           (s->count >= UNDO_MAX_ENTRIES + (keep_newest ? 1 : 0) || undo_bytes(s) > UNDO_MAX_BYTES)) {
        undo_entry_free(&s->e[0]);
        memmove(&s->e[0], &s->e[1], (size_t)(s->count - 1) * sizeof(UndoEntry));
        s->e[--s->count] = (UndoEntry){ 0 };
    }
}

static void undo_push(UndoStack *s, UndoEntry *e) {
    if (s->count >= UNDO_MAX_ENTRIES) undo_trim(s, false);
    if (s->count >= UNDO_MAX_ENTRIES) { undo_entry_free(e); return; }
    s->e[s->count++] = *e;
    *e = (UndoEntry){ 0 };
    undo_trim(s, true);
}

static int32_t undo_tiles_x(const Tilemap *m) { return (m->w + UNDO_TILE - 1) / UNDO_TILE; }
static int32_t undo_tiles_y(const Tilemap *m) { return (m->h + UNDO_TILE - 1) / UNDO_TILE; }

static void undo_copy(Tilemap *m, int32_t layer, int32_t tx, int32_t ty, TileRef *buf, bool in) {
    int32_t x0 = tx * UNDO_TILE, y0 = ty * UNDO_TILE;
    int32_t w = m->w - x0 < UNDO_TILE ? m->w - x0 : UNDO_TILE, h = m->h - y0 < UNDO_TILE ? m->h - y0 : UNDO_TILE;
    for (int32_t j = 0; j < h; j++) {
        TileRef *row = &m->cells[layer][(size_t)(y0 + j) * m->w + x0];
        if (in) memcpy(row, &buf[j * UNDO_TILE], (size_t)w * sizeof(TileRef));
        else    memcpy(&buf[j * UNDO_TILE], row, (size_t)w * sizeof(TileRef));
    }
}

static void undo_touch(Tilemap *m, int32_t layer, int32_t x, int32_t y) {
    if (!m->recording || m->undo.count == 0) return;
    int32_t tx = x / UNDO_TILE, ty = y / UNDO_TILE, ti = ty * undo_tiles_x(m) + tx;
    if (!m->rec_seen[layer]) {
        m->rec_seen[layer] = calloc((size_t)undo_tiles_x(m) * undo_tiles_y(m), 1);
        if (!m->rec_seen[layer]) return;
    }
    if (m->rec_seen[layer][ti]) return;
    UndoEntry *e = &m->undo.e[m->undo.count - 1];
    if (e->count == e->cap) {
        int32_t cap = e->cap ? e->cap * 2 : 16;
        UndoTile *t = realloc(e->tiles, (size_t)cap * sizeof(UndoTile));
        if (!t) return;
        e->tiles = t;
        e->cap = cap;
    }
    TileRef *buf = malloc(UNDO_TILE_BYTES);
    if (!buf) return;
    undo_copy(m, layer, tx, ty, buf, false);
    e->tiles[e->count++] = (UndoTile){ (int8_t)layer, tx, ty, buf };
    m->rec_seen[layer][ti] = 1;
}

static void undo_stop(Tilemap *m) {
    if (!m->recording) return;
    m->recording = false;
    for (int32_t l = 0; l < TILEMAP_MAX_LAYERS; l++) {
        if (m->rec_seen[l]) memset(m->rec_seen[l], 0, (size_t)undo_tiles_x(m) * undo_tiles_y(m));
    }
    undo_trim(&m->undo, false);
}

void Tilemap_snapshot_layers(Tilemap *m, const int32_t *layers, int32_t n) {
    (void)layers; (void)n;
    Tilemap_snapshot(m, 0);
}

void Tilemap_snapshot(Tilemap *m, int32_t layer) {
    (void)layer;
    if (!m) return;
    undo_stop(m);
    if (m->undo.count > 0 && m->undo.e[m->undo.count - 1].count == 0) undo_entry_free(&m->undo.e[--m->undo.count]);
    UndoEntry e = { 0 };
    undo_push(&m->undo, &e);
    undo_clear(&m->redo);
    m->recording = true;
}

void Tilemap_drop_snapshot(Tilemap *m) {
    if (!m || m->undo.count == 0) return;
    undo_stop(m);
    undo_entry_free(&m->undo.e[--m->undo.count]);
}

static void region_changed(Tilemap *m, int32_t layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1);

static int32_t undo_swap(Tilemap *m, UndoStack *from, UndoStack *to) {
    undo_stop(m);
    while (from->count > 0 && from->e[from->count - 1].count == 0) undo_entry_free(&from->e[--from->count]);
    if (from->count == 0) return -1;
    UndoEntry e = from->e[--from->count];
    from->e[from->count] = (UndoEntry){ 0 };
    TileRef *tmp = malloc(UNDO_TILE_BYTES);
    if (!tmp) { from->count++; return -1; }
    for (int32_t i = e.count - 1; i >= 0; i--) {
        UndoTile *t = &e.tiles[i];
        undo_copy(m, t->layer, t->tx, t->ty, tmp, false);
        undo_copy(m, t->layer, t->tx, t->ty, t->cells, true);
        memcpy(t->cells, tmp, UNDO_TILE_BYTES);
        region_changed(m, t->layer, t->tx * UNDO_TILE, t->ty * UNDO_TILE,
                       t->tx * UNDO_TILE + UNDO_TILE - 1, t->ty * UNDO_TILE + UNDO_TILE - 1);
    }
    free(tmp);
    int32_t first = e.count > 0 ? e.tiles[0].layer : -1;
    undo_push(to, &e);
    m->dirty = true;
    return first;
}

int32_t Tilemap_undo(Tilemap *m) { return m ? undo_swap(m, &m->undo, &m->redo) : -1; }
int32_t Tilemap_redo(Tilemap *m) { return m ? undo_swap(m, &m->redo, &m->undo) : -1; }

static void write_lua_string(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n')        fputs("\\n", f);
        else if (c < 32)           fprintf(f, "\\%03u", (unsigned)c);
        else                       fputc(c, f);
    }
    fputc('"', f);
}

bool Tilemap_save(const Tilemap *m, const char *path, char *err, size_t errcap) {
    if (!m || !path) { snprintf(err, errcap, "nothing to save"); return false; }

    char tmp[MOUNTS_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { snprintf(err, errcap, "cannot write %s", tmp); return false; }

    fputs("-- Tilemap saved by the gramarye editor. Cells: 0 = empty, otherwise\n"
          "-- tileset * 10000 + tile (tile = column + row * columns of that tileset);\n"
          "-- on a terrain layer, the terrain's slot in `terrains` + 256 * (direction + 1)\n"
          "-- + 131072 if it's an obstruction + 262144 if it's water.\n"
          "-- A layer's `rows` hold one string per row of cells, as runs: \"v*n\" is n\n"
          "-- cells of value v, \"v\" one, separated by spaces.\n"
          "return {\n    version = 3,\n", f);
    fprintf(f, "    width = %d, height = %d, tile = %d, levels = %d,\n", (int)m->w, (int)m->h, (int)m->tile,
            (int)m->levels);
    fprintf(f, "    level_height = %d, tilt = %g, slope_run = %d,\n", (int)m->level_height, (double)m->tilt,
            (int)m->slope_run);

    fputs("    tilesets = {\n", f);
    for (int32_t i = 0; i < m->tileset_count; i++) {
        const TilemapTileset *t = &m->tilesets[i];
        fputs("        { id = ", f);    write_lua_string(f, t->id);
        fputs(", image = ", f);         write_lua_string(f, t->image);
        if (t->skin[0]) { fputs(", skin = ", f); write_lua_string(f, t->skin); }
        fprintf(f, ", tile_w = %d, tile_h = %d },\n", (int)t->tile_w, (int)t->tile_h);
    }
    fputs("    },\n", f);
    if (m->terrain_count > 0) {
        fputs("    terrains = {\n", f);
        for (int32_t i = 0; i < m->terrain_count; i++) {
            const TilemapTerrain *t = &m->terrains[i];
            fputs("        { id = ", f);   write_lua_string(f, t->id);
            if (t->image[0]) { fputs(", image = ", f); write_lua_string(f, t->image); }
            if (t->color.a) {
                fprintf(f, ", color = { %u, %u, %u, %u }", t->color.r, t->color.g, t->color.b, t->color.a);
            }
            fprintf(f, ", noise = %g, blades = %g, foam = %g, foam_speed = %g,"
                       " foam_color = { %u, %u, %u, %u }",
                    (double)t->noise, (double)t->blades, (double)t->foam, (double)t->foam_speed,
                    t->foam_color.r, t->foam_color.g, t->foam_color.b, t->foam_color.a);
            if (t->cliff.a) {
                fprintf(f, ", cliff = { %u, %u, %u, %u }", t->cliff.r, t->cliff.g, t->cliff.b, t->cliff.a);
            }
            if (t->waves != 1.0f) fprintf(f, ", waves = %g", (double)t->waves);
            if (t->sway > 0.0f) fprintf(f, ", sway = %g, sway_speed = %g", (double)t->sway, (double)t->sway_speed);
            if (t->flow > 0.0f)   fprintf(f, ", flow = %g", (double)t->flow);
            if (t->stairs) fputs(", stairs = true", f);
            if (t->water)  fputs(", water = true", f);
            if (t->solid)  fputs(", solid = true", f);
            fputs(" },\n", f);
        }
        fputs("    },\n", f);
    }
    fputs("    layers = {\n", f);
    for (int32_t l = 0; l < m->layer_count; l++) {
        fputs("        { name = ", f);
        write_lua_string(f, m->layer_names[l]);
        if (m->layer_kind[l] == TILEMAP_LAYER_TERRAIN) fputs(", kind = \"terrain\"", f);
        if (m->layer_elevation[l]) fprintf(f, ", elevation = %d", (int)m->layer_elevation[l]);
        fputs(", rows = {\n", f);
        for (int32_t y = 0; y < m->h; y++) {
            fputs("            \"", f);
            const TileRef *row = &m->cells[l][(size_t)y * m->w];
            for (int32_t x = 0; x < m->w;) {
                int32_t e = x + 1;
                while (e < m->w && row[e] == row[x]) e++;
                if (x > 0) fputc(' ', f);
                if (e - x > 1) fprintf(f, "%u*%d", (unsigned)row[x], (int)(e - x));
                else           fprintf(f, "%u", (unsigned)row[x]);
                x = e;
            }
            fputs("\",\n", f);
        }
        fputs("        } },\n", f);
    }
    fputs("    },\n}\n", f);

    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); snprintf(err, errcap, "write failed: %s", tmp); return false; }
#if defined(_WIN32)
    remove(path);
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        snprintf(err, errcap, "cannot replace %s", path);
        return false;
    }
    return true;
}

static bool cell_value_ok(const Tilemap *m, bool terrain, lua_Integer v) {
    if (v < 0) return false;
    if (!terrain) return v / TILEMAP_TILE_BASE <= m->tileset_count;
    return (v & ~(lua_Integer)TILEMAP_CELL_FLAGS) >> (TILEMAP_DIR_SHIFT + 9) == 0 &&
           terrain_cell_slot((TileRef)v) <= m->terrain_count && ((v >> TILEMAP_DIR_SHIFT) & 0x1FF) <= 360;
}

static lua_Integer field_int(lua_State *L, int32_t t, const char *key, lua_Integer def) {
    lua_getfield(L, t, key);
    int isnum = 0;
    lua_Integer v = lua_tointegerx(L, -1, &isnum);
    lua_pop(L, 1);
    return isnum ? v : def;
}

static void field_str(lua_State *L, int32_t t, const char *key, char *out, size_t cap) {
    lua_getfield(L, t, key);
    const char *s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    snprintf(out, cap, "%s", s);
    lua_pop(L, 1);
}

static bool field_bool(lua_State *L, int32_t t, const char *key) {
    lua_getfield(L, t, key);
    bool v = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return v;
}

static float field_float(lua_State *L, int32_t t, const char *key, float def) {
    lua_getfield(L, t, key);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static Color field_color(lua_State *L, int32_t t, const char *key, Color def) {
    lua_getfield(L, t, key);
    if (lua_istable(L, -1)) {
        uint8_t c[4] = { 0, 0, 0, 255 };
        for (int32_t i = 0; i < 4; i++) {
            lua_rawgeti(L, -1, i + 1);
            if (lua_isnumber(L, -1)) {
                lua_Integer v = lua_tointeger(L, -1);
                c[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
            lua_pop(L, 1);
        }
        def = (Color){ c[0], c[1], c[2], c[3] };
    }
    lua_pop(L, 1);
    return def;
}

void Tilemap_terrain_from_lua(lua_State *L, int32_t t, TilemapTerrain *out) {
    memset(out, 0, sizeof(*out));
    field_str(L, t, "id", out->id, sizeof(out->id));
    field_str(L, t, "image", out->image, sizeof(out->image));
    out->color      = field_color(L, t, "color", (Color){ 0 });
    out->noise      = field_float(L, t, "noise", 0.2f);
    out->blades     = field_float(L, t, "blades", 0.0f);
    out->foam       = field_float(L, t, "foam", 0.0f);
    out->foam_speed = field_float(L, t, "foam_speed", 1.5f);
    out->foam_color = field_color(L, t, "foam_color", (Color){ 235, 245, 255, 220 });
    out->waves      = field_float(L, t, "waves", 1.0f);
    out->sway       = field_float(L, t, "sway", 0.0f);
    out->sway_speed = field_float(L, t, "sway_speed", 1.0f);
    out->flow       = field_float(L, t, "flow", 0.0f);
    out->cliff      = field_color(L, t, "cliff", (Color){ 0 });
    out->stairs     = field_bool(L, t, "stairs");
    out->water      = field_bool(L, t, "water");
    out->solid      = field_bool(L, t, "solid");
}

int32_t Tilemap_add_layer(Tilemap *m, const char *name, TilemapLayerKind kind, int32_t elevation) {
    if (!m || m->layer_count >= TILEMAP_MAX_LAYERS) return -1;
    if (elevation < 0 || elevation >= m->levels) elevation = 0;
    int32_t l = m->layer_count;
    m->cells[l] = calloc(cell_count(m), sizeof(TileRef));
    if (!m->cells[l]) return -1;
    snprintf(m->layer_names[l], TILEMAP_LAYER_NAME_MAX, "%s", name);
    m->layer_visible[l] = true;
    m->layer_kind[l] = (uint8_t)kind;
    m->layer_elevation[l] = (uint8_t)elevation;
    m->ids_stale[l] = m->slopes_stale[l] = true;
    m->layer_count++;
    m->lvl_indexed = false;
    m->edit_gen++;
    m->dirty = true;
    return l;
}

bool Tilemap_layer_empty(const Tilemap *m, int32_t layer) {
    if (!m || layer < 0 || layer >= m->layer_count) return true;
    size_t n = cell_count(m);
    for (size_t i = 0; i < n; i++) {
        if (m->cells[layer][i] != 0) return false;
    }
    return true;
}

bool Tilemap_remove_layer(Tilemap *m, int32_t layer) {
    if (!m || layer < 0 || layer >= m->layer_count || m->layer_count <= 1) return false;
    if (m->ids[layer].id != 0 && IsWindowReady()) UnloadTexture(m->ids[layer]);
    free(m->cells[layer]);
    free(m->slopes[layer]);
    free(m->wslopes[layer]);
    int32_t last = m->layer_count - 1;
    for (int32_t l = layer; l < last; l++) {
        memcpy(m->layer_names[l], m->layer_names[l + 1], TILEMAP_LAYER_NAME_MAX);
        m->layer_visible[l] = m->layer_visible[l + 1];
        m->layer_kind[l] = m->layer_kind[l + 1];
        m->layer_elevation[l] = m->layer_elevation[l + 1];
        m->cells[l] = m->cells[l + 1];
        m->ids[l] = m->ids[l + 1];
        m->ids_stale[l] = m->ids_stale[l + 1];
        memcpy(m->ids_rect[l], m->ids_rect[l + 1], sizeof(m->ids_rect[l]));
        memcpy(m->slope_rect[l], m->slope_rect[l + 1], sizeof(m->slope_rect[l]));
        m->slopes[l] = m->slopes[l + 1];
        m->wslopes[l] = m->wslopes[l + 1];
        m->slopes_stale[l] = m->slopes_stale[l + 1];
    }
    m->cells[last] = NULL;
    m->slopes[last] = NULL;
    m->wslopes[last] = NULL;
    m->ids[last] = (Texture2D){ 0 };
    m->layer_count--;
    m->lvl_indexed = false;
    m->edit_gen++;
    m->recording = false;
    undo_clear(&m->undo);
    undo_clear(&m->redo);
    for (int32_t l = 0; l < TILEMAP_MAX_LAYERS; l++) { free(m->rec_seen[l]); m->rec_seen[l] = NULL; }
    all_cells_changed(m);
    m->dirty = true;
    return true;
}

static void blocks_from_columns(Tilemap *m) {
    int32_t top = 0;
    for (int32_t l = 0; l < m->layer_count; l++) {
        if (is_terrain_layer(m, l) && m->layer_elevation[l] > top) top = m->layer_elevation[l];
    }
    if (top < 1) return;
    size_t n = cell_count(m);
    TileRef *old[TILEMAP_MAX_LAYERS] = { 0 };
    int32_t count = m->layer_count;
    for (int32_t l = 0; l < count; l++) {
        if (!is_terrain_layer(m, l) || m->layer_elevation[l] == 0) continue;
        old[l] = malloc(n * sizeof(TileRef));
        if (!old[l]) goto out;
        memcpy(old[l], m->cells[l], n * sizeof(TileRef));
    }
    for (int32_t k = 1; k <= top; k++) {
        int32_t dst = -1;
        for (int32_t l = 0; l < m->layer_count && dst < 0; l++) {
            if (is_terrain_layer(m, l) && m->layer_elevation[l] == k) dst = l;
        }
        if (dst < 0) dst = Tilemap_add_layer(m, TextFormat("level %d", (int)k), TILEMAP_LAYER_TERRAIN, k);
        if (dst < 0) break;
        for (size_t i = 0; i < n; i++) {
            TileRef best = 0;
            int32_t best_e = TILEMAP_MAX_LEVELS;
            for (int32_t l = 0; l < count; l++) {
                if (!old[l] || m->layer_elevation[l] < k || m->layer_elevation[l] >= best_e) continue;
                if (terrain_cell_slot(old[l][i]) == 0) continue;
                best = old[l][i];
                best_e = m->layer_elevation[l];
            }
            m->cells[dst][i] = best;
        }
        for (int32_t l = 0; l < m->layer_count; l++) {
            if (l != dst && is_terrain_layer(m, l) && m->layer_elevation[l] == k) memset(m->cells[l], 0, n * sizeof(TileRef));
        }
    }
out:
    for (int32_t l = 0; l < count; l++) free(old[l]);
}

Tilemap *Tilemap_load(lua_State *L, const char *path, char *err, size_t errcap) {
    int32_t base = lua_gettop(L);
    if (!Packages_load_data(L, path, err, errcap)) return NULL;
    int32_t t = lua_gettop(L);
    Tilemap *m = NULL;

    lua_Integer version = field_int(L, t, "version", 1);
    if (version < 1 || version > 3) {
        snprintf(err, errcap, "%s: unsupported version", path);
        goto done;
    }
    lua_Integer w = field_int(L, t, "width", 0), h = field_int(L, t, "height", 0);
    lua_Integer tile = field_int(L, t, "tile", 16);

    char names[TILEMAP_MAX_LAYERS][TILEMAP_LAYER_NAME_MAX];
    const char *name_ptrs[TILEMAP_MAX_LAYERS];
    bool terrain_layer[TILEMAP_MAX_LAYERS] = { false };
    lua_Integer elevation[TILEMAP_MAX_LAYERS] = { 0 };
    lua_getfield(L, t, "layers");
    int32_t layers_idx = lua_gettop(L);
    lua_Integer layer_count = lua_istable(L, layers_idx) ? (lua_Integer)lua_rawlen(L, layers_idx) : 0;
    if (layer_count < 1 || layer_count > TILEMAP_MAX_LAYERS) {
        snprintf(err, errcap, "%s: needs 1-%d layers", path, TILEMAP_MAX_LAYERS);
        goto done;
    }
    for (int32_t l = 0; l < layer_count; l++) {
        lua_rawgeti(L, layers_idx, l + 1);
        names[l][0] = '\0';
        if (lua_istable(L, -1)) {
            char kind[16];
            field_str(L, lua_gettop(L), "name", names[l], sizeof(names[l]));
            field_str(L, lua_gettop(L), "kind", kind, sizeof(kind));
            terrain_layer[l] = strcmp(kind, "terrain") == 0;
            elevation[l] = field_int(L, lua_gettop(L), "elevation", 0);
        }
        name_ptrs[l] = names[l][0] ? names[l] : NULL;
        lua_pop(L, 1);
    }

    m = Tilemap_new((int32_t)w, (int32_t)h, (int32_t)tile, (int32_t)layer_count, name_ptrs);
    if (!m) {
        snprintf(err, errcap, "%s: bad size %lldx%lld (tile %lld)", path,
                 (long long)w, (long long)h, (long long)tile);
        goto done;
    }
    lua_Integer top_elevation = 0;
    for (int32_t l = 0; l < layer_count; l++) if (elevation[l] > top_elevation) top_elevation = elevation[l];
    Tilemap_set_levels(m, (int32_t)field_int(L, t, "levels", top_elevation + 1 > 4 ? top_elevation + 1 : 4));
    for (int32_t l = 0; l < layer_count; l++) {
        if (terrain_layer[l]) Tilemap_set_layer_kind(m, l, TILEMAP_LAYER_TERRAIN);
        Tilemap_set_layer_elevation(m, l, (int32_t)elevation[l]);
    }
    Tilemap_set_elevation_style(m, (int32_t)field_int(L, t, "level_height", m->level_height),
                                field_float(L, t, "tilt", DEFAULT_TILT),
                                (int32_t)field_int(L, t, "slope_run", m->slope_run));

    lua_getfield(L, t, "tilesets");
    int32_t ts_idx = lua_gettop(L);
    int32_t ts_count = lua_istable(L, ts_idx) ? (int32_t)lua_rawlen(L, ts_idx) : 0;
    for (int32_t i = 0; i < ts_count; i++) {
        lua_rawgeti(L, ts_idx, i + 1);
        int32_t e = lua_gettop(L);
        char id[TILEMAP_ID_MAX] = "", image[TILEMAP_REL_MAX] = "", skin[TILEMAP_REL_MAX] = "";
        int32_t tw = (int32_t)tile, th = (int32_t)tile;
        if (lua_istable(L, e)) {
            field_str(L, e, "id", id, sizeof(id));
            field_str(L, e, "image", image, sizeof(image));
            field_str(L, e, "skin", skin, sizeof(skin));
            tw = (int32_t)field_int(L, e, "tile_w", tile);
            th = (int32_t)field_int(L, e, "tile_h", tile);
        }
        lua_pop(L, 1);
        if (Tilemap_add_tileset(m, id, image, skin, tw, th) != i + 1) {
            snprintf(err, errcap, "%s: tileset %d ('%s') is invalid or a duplicate", path, (int)(i + 1), id);
            Tilemap_free(m);
            m = NULL;
            goto done;
        }
    }

    lua_getfield(L, t, "terrains");
    int32_t tr_idx = lua_gettop(L);
    int32_t tr_count = lua_istable(L, tr_idx) ? (int32_t)lua_rawlen(L, tr_idx) : 0;
    for (int32_t i = 0; i < tr_count; i++) {
        lua_rawgeti(L, tr_idx, i + 1);
        TilemapTerrain def;
        memset(&def, 0, sizeof(def));
        if (lua_istable(L, -1)) Tilemap_terrain_from_lua(L, lua_gettop(L), &def);
        lua_pop(L, 1);
        if (Tilemap_add_terrain(m, &def) != i + 1) {
            snprintf(err, errcap, "%s: terrain %d ('%s') is invalid or a duplicate", path, (int)(i + 1), def.id);
            Tilemap_free(m);
            m = NULL;
            goto done;
        }
    }

    int32_t bad = 0;
    lua_Integer n = (lua_Integer)cell_count(m);
    for (int32_t l = 0; l < m->layer_count; l++) {
        lua_rawgeti(L, layers_idx, l + 1);
        int32_t lt = lua_gettop(L);
        bool terrain = m->layer_kind[l] == TILEMAP_LAYER_TERRAIN;
        if (lua_istable(L, lt)) lua_getfield(L, lt, "rows");
        else                    lua_pushnil(L);
        if (lua_istable(L, -1)) {
            for (int32_t y = 0; y < m->h; y++) {
                lua_rawgeti(L, -1, y + 1);
                const char *p = lua_tostring(L, -1);
                int32_t x = 0;
                while (p && *p && x < m->w) {
                    char *end;
                    unsigned long v = strtoul(p, &end, 10);
                    if (end == p) break;
                    unsigned long count = 1;
                    if (*end == '*') { p = end + 1; count = strtoul(p, &end, 10); if (end == p) break; }
                    p = end;
                    while (*p == ' ' || *p == ',') p++;
                    if (!cell_value_ok(m, terrain, (lua_Integer)v)) { bad += (int32_t)count; v = 0; }
                    for (unsigned long k = 0; k < count && x < m->w; k++) m->cells[l][(size_t)y * m->w + x++] = (TileRef)v;
                }
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
            if (lua_istable(L, lt)) lua_getfield(L, lt, "cells");
            else                    lua_pushnil(L);
            if (lua_istable(L, -1)) {
                lua_Integer len = (lua_Integer)lua_rawlen(L, -1);
                for (lua_Integer k = 1; k <= len && k <= n; k++) {
                    lua_rawgeti(L, -1, k);
                    lua_Integer v = lua_tointeger(L, -1);
                    lua_pop(L, 1);
                    if (!cell_value_ok(m, terrain, v)) { bad++; v = 0; }
                    m->cells[l][k - 1] = (TileRef)v;
                }
            }
        }
        lua_pop(L, 2);
    }
    if (bad) TraceLog(LOG_WARNING, "TILEMAP: %s: %d cells pointed at missing tilesets/terrains (cleared)", path, (int)bad);
    if (version < 2) blocks_from_columns(m);
    all_cells_changed(m);
    m->dirty = false;

done:
    lua_settop(L, base);
    return m;
}

typedef struct ShaderFiles {
    Shader shader;
    bool   tried;
    char   vs[MOUNTS_PATH_MAX], fs[MOUNTS_PATH_MAX];
    long   vs_time, fs_time;
} ShaderFiles;

static uint32_t g_shader_gen = 1;

static bool shader_files_load(ShaderFiles *s, const char *vs_name, const char *fs_name) {
    if (s->tried) return s->shader.id != 0;
    s->tried = true;
    s->vs[0] = s->fs[0] = '\0';
    if (Mounts_resolve(TextFormat("shaders/glsl%d/%s", TILEMAP_GLSL_VERSION, fs_name), s->fs, sizeof(s->fs)) < 0) return false;
    if (vs_name &&
        Mounts_resolve(TextFormat("shaders/glsl%d/%s", TILEMAP_GLSL_VERSION, vs_name), s->vs, sizeof(s->vs)) < 0) {
        return false;
    }
    s->fs_time = GetFileModTime(s->fs);
    s->vs_time = s->vs[0] ? GetFileModTime(s->vs) : 0;
    s->shader = LoadShader(s->vs[0] ? s->vs : NULL, s->fs);
    return s->shader.id != 0;
}

static void shader_files_unload(ShaderFiles *s) {
    if (s->shader.id != 0) UnloadShader(s->shader);
    s->shader = (Shader){ 0 };
    s->tried = false;
}

static bool shader_files_changed(const ShaderFiles *s) {
    if (!s->tried || !s->fs[0]) return false;
    if (GetFileModTime(s->fs) != s->fs_time) return true;
    return s->vs[0] && GetFileModTime(s->vs) != s->vs_time;
}

static struct {
    ShaderFiles f;
    int32_t     tex1, texel;
} g_skin;

static bool skin_shader_ready(void) {
    if (!g_skin.f.tried) {
        g_skin.tex1 = g_skin.texel = -1;
        if (shader_files_load(&g_skin.f, NULL, "skin.fs")) {
            g_skin.tex1 = GetShaderLocation(g_skin.f.shader, "texture1");
            g_skin.texel = GetShaderLocation(g_skin.f.shader, "skinTexelSize");
        }
        if (g_skin.tex1 < 0) TraceLog(LOG_WARNING, "TILEMAP: skin shader unavailable; skinned tilesets draw unskinned");
    }
    return g_skin.f.shader.id != 0 && g_skin.tex1 >= 0;
}

static bool begin_skin(const TilemapTileset *t) {
    if (!t->skin_texture || !skin_shader_ready()) return false;
    Texture2D skin = TextureStore_get(t->skin_texture);
    if (skin.id == 0) return false;
    BeginShaderMode(g_skin.f.shader);
    SetShaderValueTexture(g_skin.f.shader, g_skin.tex1, skin);
    Vector2 texel = { 1.0f / (float)skin.width, 1.0f / (float)skin.height };
    SetShaderValue(g_skin.f.shader, g_skin.texel, &texel, SHADER_UNIFORM_VEC2);
    return true;
}

static struct {
    ShaderFiles f;
    int32_t fills, map_size, tile_size, fill_size, count, time, params, foam, extra, swatch, mode, view_down, raised;
    int32_t cover, cover_rect, see, sway, water_pass, cut_info, cut_map, view_cos;
} g_ground;

static bool ground_shader_ready(void) {
    if (!g_ground.f.tried) {
        g_ground.fills = -1;
        if (shader_files_load(&g_ground.f, NULL, "ground.fs")) {
            Shader sh = g_ground.f.shader;
            g_ground.fills     = GetShaderLocation(sh, "terrainFills");
            g_ground.map_size  = GetShaderLocation(sh, "mapSize");
            g_ground.tile_size = GetShaderLocation(sh, "tileSize");
            g_ground.fill_size = GetShaderLocation(sh, "fillSize");
            g_ground.count     = GetShaderLocation(sh, "terrainCount");
            g_ground.time      = GetShaderLocation(sh, "time");
            g_ground.params    = GetShaderLocation(sh, "terrainParams");
            g_ground.foam      = GetShaderLocation(sh, "terrainFoam");
            g_ground.extra     = GetShaderLocation(sh, "terrainExtra");
            g_ground.swatch    = GetShaderLocation(sh, "terrainSwatch");
            g_ground.mode      = GetShaderLocation(sh, "outputMode");
            g_ground.view_down = GetShaderLocation(sh, "viewDown");
            g_ground.raised    = GetShaderLocation(sh, "raisedInfo");
            g_ground.cover     = GetShaderLocation(sh, "coverMask");
            g_ground.cover_rect = GetShaderLocation(sh, "coverRect");
            g_ground.see       = GetShaderLocation(sh, "seeThrough");
            g_ground.sway      = GetShaderLocation(sh, "terrainSway");
            g_ground.water_pass = GetShaderLocation(sh, "waterPass");
            g_ground.cut_info  = GetShaderLocation(sh, "cutInfo");
            g_ground.cut_map   = GetShaderLocation(sh, "cutMap");
            g_ground.view_cos  = GetShaderLocation(sh, "viewCos");
            if (sh.locs && g_ground.fills >= 0) {
                sh.locs[SHADER_LOC_MAP_SPECULAR] = g_ground.fills;
                sh.locs[SHADER_LOC_MAP_NORMAL] = g_ground.cover;
                sh.locs[SHADER_LOC_MAP_ROUGHNESS] = g_ground.cut_map;
            }
        }
        if (g_ground.fills < 0) TraceLog(LOG_WARNING, "TILEMAP: ground shader unavailable; terrain layers won't draw");
    }
    return g_ground.f.shader.id != 0 && g_ground.fills >= 0;
}

static struct {
    ShaderFiles f;
    int32_t cliff, swatch, time, see, cut_info, cut_map, map_size, tile_size, level_height, water_pass;
} g_wall;

static bool wall_shader_ready(void) {
    if (!g_wall.f.tried) {
        g_wall.cliff = -1;
        if (shader_files_load(&g_wall.f, "wall.vs", "wall.fs")) {
            g_wall.cliff  = GetShaderLocation(g_wall.f.shader, "cliffColor");
            g_wall.swatch = GetShaderLocation(g_wall.f.shader, "swatchColor");
            g_wall.time   = GetShaderLocation(g_wall.f.shader, "time");
            g_wall.see    = GetShaderLocation(g_wall.f.shader, "seeThrough");
            g_wall.cut_info = GetShaderLocation(g_wall.f.shader, "cutInfo");
            g_wall.cut_map = GetShaderLocation(g_wall.f.shader, "cutMap");
            g_wall.map_size = GetShaderLocation(g_wall.f.shader, "mapSize");
            g_wall.tile_size = GetShaderLocation(g_wall.f.shader, "tileSize");
            g_wall.level_height = GetShaderLocation(g_wall.f.shader, "levelHeight");
            g_wall.water_pass = GetShaderLocation(g_wall.f.shader, "waterPass");
            if (g_wall.f.shader.locs && g_wall.cliff >= 0) g_wall.f.shader.locs[SHADER_LOC_MAP_ROUGHNESS] = g_wall.cut_map;
        }
        if (g_wall.cliff < 0) TraceLog(LOG_WARNING, "TILEMAP: wall shader unavailable; raised layers draw without cliffs");
    }
    return g_wall.f.shader.id != 0 && g_wall.cliff >= 0;
}

static struct { ShaderFiles f; } g_sprite;

static bool sprite_shader_ready(void) {
    if (!g_sprite.f.tried && !shader_files_load(&g_sprite.f, NULL, "sprite.fs")) {
        TraceLog(LOG_WARNING, "TILEMAP: sprite shader unavailable; sprite edges may hide things");
    }
    return g_sprite.f.shader.id != 0;
}

void Tilemap_reload_shaders(void) {
    shader_files_unload(&g_skin.f);
    shader_files_unload(&g_ground.f);
    shader_files_unload(&g_wall.f);
    shader_files_unload(&g_sprite.f);
    g_shader_gen++;
}

void Tilemap_watch_shaders(float dt) {
    static float elapsed = 0.0f;
    elapsed += dt;
    if (elapsed < 0.5f) return;
    elapsed = 0.0f;
    ShaderFiles *all[4] = { &g_skin.f, &g_ground.f, &g_wall.f, &g_sprite.f };
    for (int32_t i = 0; i < 4; i++) {
        if (!shader_files_changed(all[i])) continue;
        TraceLog(LOG_INFO, "TILEMAP: %s changed -> reloading", all[i]->fs);
        shader_files_unload(all[i]);
        g_shader_gen++;
    }
}

static float shader_time(void) { return (float)fmod(GetTime(), 3600.0); }

static Texture2D       g_marker;


static Rectangle tile_src(const TilemapTileset *t, int32_t tile) {
    return (Rectangle){ (float)((tile % t->cols) * t->tile_w), (float)((tile / t->cols) * t->tile_h),
                        (float)t->tile_w, (float)t->tile_h };
}

static int32_t *g_bucket = NULL;
static size_t   g_bucket_cap = 0;

static bool bucket_reserve(size_t n) {
    if (n <= g_bucket_cap) return true;
    int32_t *p = realloc(g_bucket, n * sizeof(int32_t));
    if (!p) return false;
    g_bucket = p;
    g_bucket_cap = n;
    return true;
}

static void draw_layer(const Tilemap *m, int32_t layer, int32_t x0, int32_t y0,
                       int32_t x1, int32_t y1, float dy, Color tint) {
    const TileRef *cells = m->cells[layer];
    int32_t vis_w = x1 - x0, vis_h = y1 - y0;
    if (vis_w <= 0 || vis_h <= 0 || !bucket_reserve((size_t)vis_w * (size_t)vis_h)) return;

    int32_t count[TILEMAP_MAX_TILESETS + 1] = { 0 };
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            int32_t s = tile_ref_tileset(cells[(size_t)y * m->w + x]);
            if (s >= 1 && s <= m->tileset_count) count[s]++;
        }
    }
    int32_t start[TILEMAP_MAX_TILESETS + 2] = { 0 };
    for (int32_t s = 1; s <= m->tileset_count; s++) start[s + 1] = start[s] + count[s];
    int32_t fill[TILEMAP_MAX_TILESETS + 1];
    for (int32_t s = 1; s <= m->tileset_count; s++) fill[s] = start[s];
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            int32_t i = y * m->w + x;
            int32_t s = tile_ref_tileset(cells[i]);
            if (s >= 1 && s <= m->tileset_count) g_bucket[fill[s]++] = i;
        }
    }

    float ts = (float)m->tile;
    for (int32_t s = 1; s <= m->tileset_count; s++) {
        if (count[s] == 0) continue;
        const TilemapTileset *t = &m->tilesets[s - 1];
        Texture2D tex = TextureStore_get(t->texture);
        if (tex.id == 0 || t->cols <= 0) continue;
        int32_t ntiles = t->cols * t->rows;
        bool skinned = t->skin[0] && begin_skin(t);
        for (int32_t k = start[s]; k < start[s] + count[s]; k++) {
            int32_t i = g_bucket[k];
            int32_t tile = tile_ref_tile(cells[i]);
            if (tile >= ntiles) continue;
            Rectangle dst = { (float)(i % m->w) * ts, (float)(i / m->w) * ts + dy,
                              (float)t->tile_w, (float)t->tile_h };
            DrawTexturePro(tex, tile_src(t, tile), dst, (Vector2){ 0, 0 }, 0.0f, tint);
        }
        if (skinned) EndShaderMode();
    }
}

static uint8_t *g_ids_scratch = NULL;
static size_t   g_ids_cap = 0;

#define CELL_HAS_DIR 1u
static const uint8_t FALL_BIT[4] = { 2, 4, 8, 16 };
static const int8_t  FALL_DX[4]  = { 0, 1, 0, -1 };
static const int8_t  FALL_DY[4]  = { -1, 0, 1, 0 };

static int32_t ground_below(const Tilemap *m, int32_t level, int32_t x, int32_t y) {
    int32_t best = -1, slot = 0;
    for (int32_t l = 0; l < m->layer_count; l++) {
        int32_t e = m->layer_elevation[l];
        if (e >= level || e <= best || m->layer_kind[l] != TILEMAP_LAYER_TERRAIN) continue;
        int32_t s = terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]);
        if (s > 0 && s <= m->terrain_count) { best = e; slot = s; }
    }
    return slot;
}

static bool sync_ids(Tilemap *m, int32_t layer) {
    bool full = m->ids[layer].id == 0 || m->ids_stale[layer];
    int32_t *r = m->ids_rect[layer];
    if (!full && r[2] < r[0]) return true;
    int32_t x0 = 0, y0 = 0, x1 = m->w - 1, y1 = m->h - 1;
    if (!full) {
        x0 = r[0] < 0 ? 0 : r[0]; y0 = r[1] < 0 ? 0 : r[1];
        x1 = r[2] >= m->w ? m->w - 1 : r[2]; y1 = r[3] >= m->h ? m->h - 1 : r[3];
    }
    r[0] = r[1] = 0;
    r[2] = r[3] = -1;
    int32_t rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    size_t n = (size_t)rw * rh;
    if (n * 4 > g_ids_cap) {
        uint8_t *p = realloc(g_ids_scratch, n * 4);
        if (!p) return false;
        g_ids_scratch = p;
        g_ids_cap = n * 4;
    }
    const TileRef *cells = m->cells[layer];
    int32_t level = m->layer_elevation[layer];
    const uint8_t *slopes = slope_field(m, layer);
    int32_t cx0 = x0 > 0 ? x0 - 1 : 0, cy0 = y0 > 0 ? y0 - 1 : 0;
    int32_t cx1 = x1 + 1 < m->w ? x1 + 1 : x1, cy1 = y1 + 1 < m->h ? y1 + 1 : y1;
    int32_t cw = cx1 - cx0 + 1;
    uint8_t *cov = calloc((size_t)cw * (cy1 - cy0 + 1), 1);
    if (cov && level + 1 < m->levels && is_terrain_layer(m, layer)) {
        for (int32_t y = cy0; y <= cy1; y++) {
            for (int32_t x = cx0; x <= cx1; x++) cov[(size_t)(y - cy0) * cw + (x - cx0)] = solid_at(m, x, y, level + 1);
        }
    }
    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            size_t i = (size_t)y * m->w + x;
            int32_t slot = terrain_cell_slot(cells[i]);
            if (slot > m->terrain_count) slot = 0;
            int32_t dir = slot ? terrain_cell_dir(cells[i]) : -1;
            uint8_t flags = dir >= 0 ? CELL_HAS_DIR : 0;
            if (level > 0 && slot && m->terrains[slot - 1].water) {
                for (int32_t d = 0; d < 4; d++) {
                    int32_t nx = x + FALL_DX[d], ny = y + FALL_DY[d];
                    if (nx < 0 || ny < 0 || nx >= m->w || ny >= m->h) continue;
                    if (terrain_cell_slot(cells[(size_t)ny * m->w + nx]) != 0) continue;
                    int32_t below = ground_below(m, level, nx, ny);
                    if (below && m->terrains[below - 1].water) flags |= FALL_BIT[d];
                }
            }
            if (cov && slot && cov[(size_t)(y - cy0) * cw + (x - cx0)] && !m->terrains[slot - 1].water &&
                !m->terrains[slot - 1].stairs) {
                static const int8_t ndx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, ndy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
                int32_t best = 0;
                for (int32_t d = 0; d < 8 && !(d == 4 && best); d++) {
                    int32_t nx = x + ndx[d], ny = y + ndy[d];
                    if (nx < cx0 || ny < cy0 || nx > cx1 || ny > cy1) continue;
                    if (cov[(size_t)(ny - cy0) * cw + (nx - cx0)]) continue;
                    int32_t fl = floor_here(m, nx, ny, level);
                    if (fl < 0) continue;
                    int32_t s2 = terrain_cell_slot(m->cells[fl][(size_t)ny * m->w + nx]);
                    if (s2 < 1 || s2 > m->terrain_count || m->terrains[s2 - 1].water || m->terrains[s2 - 1].stairs) continue;
                    if (s2 > best) best = s2;
                }
                if (best) slot = best;
            }
            uint8_t *px = &g_ids_scratch[((size_t)(y - y0) * rw + (x - x0)) * 4];
            px[0] = (uint8_t)slot;
            px[1] = dir >= 0 ? (uint8_t)((dir * 256) / 360) : 0;
            StairGeom sg;
            if (stairs_geometry(m, layer, x, y, &sg)) {
                px[1] = (uint8_t)(sg.facing | (sg.index << 2) | ((sg.depth - 1) << 5));
                flags = (uint8_t)((flags & 31u) | (sg.drop << 5));
            }
            px[2] = flags;
            px[3] = slopes ? slopes[i] : SLOPE_NONE;
        }
    }
    free(cov);
    if (m->ids[layer].id == 0) {
        Image img = { .data = g_ids_scratch, .width = m->w, .height = m->h, .mipmaps = 1,
                      .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        m->ids[layer] = LoadTextureFromImage(img);
        SetTextureFilter(m->ids[layer], TEXTURE_FILTER_POINT);
        SetTextureWrap(m->ids[layer], TEXTURE_WRAP_CLAMP);
    } else if (full) {
        UpdateTexture(m->ids[layer], g_ids_scratch);
    } else {
        UpdateTextureRec(m->ids[layer], (Rectangle){ (float)x0, (float)y0, (float)rw, (float)rh }, g_ids_scratch);
    }
    m->ids_stale[layer] = false;
    return m->ids[layer].id != 0;
}

static uint32_t hash3(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static bool sync_fills(Tilemap *m) {
    if (m->fills.id != 0 && !m->fills_stale) return true;
    if (m->fills.id != 0) UnloadTexture(m->fills);
    m->fills = (Texture2D){ 0 };
    m->fills_stale = false;

    int32_t n = m->terrain_count;
    Image imgs[TILEMAP_MAX_TERRAINS] = { 0 };
    int32_t fw = 0, fh = 0;
    for (int32_t i = 0; i < n; i++) {
        const TilemapTerrain *t = &m->terrains[i];
        if (!t->image[0]) continue;
        char path[MOUNTS_PATH_MAX];
        if (Mounts_resolve(t->image, path, sizeof(path)) >= 0) imgs[i] = LoadImage(path);
        if (!imgs[i].data) {
            TraceLog(LOG_WARNING, "TILEMAP: terrain '%s': image '%s' not loaded", t->id, t->image);
            continue;
        }
        ImageFormat(&imgs[i], PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        if (fw == 0) { fw = imgs[i].width; fh = imgs[i].height; }
    }
    if (fw == 0) { fw = 32; fh = 32; }

    Image strip = GenImageColor(fw * n, fh, BLANK);
    for (int32_t i = 0; i < n; i++) {
        if (imgs[i].data) {
            if (imgs[i].width != fw || imgs[i].height != fh) {
                TraceLog(LOG_WARNING, "TILEMAP: terrain '%s' is %dx%d; resizing to %dx%d (make fills one size)",
                         m->terrains[i].id, imgs[i].width, imgs[i].height, (int)fw, (int)fh);
                ImageResizeNN(&imgs[i], fw, fh);
            }
            ImageDrawImage(&strip, imgs[i], i * fw, 0, WHITE);
            UnloadImage(imgs[i]);
            continue;
        }
        Color base = m->terrains[i].color.a ? m->terrains[i].color : MISSING_COLOR;
        for (int32_t y = 0; y < fh; y++) {
            for (int32_t x = 0; x < fw; x++) {
                uint32_t h = hash3((uint32_t)x, (uint32_t)y, (uint32_t)i) % 100u;
                float k = h < 12 ? 0.88f : h < 18 ? 1.08f : 1.0f;
                Color c = { (uint8_t)clampf(base.r * k, 0, 255), (uint8_t)clampf(base.g * k, 0, 255),
                            (uint8_t)clampf(base.b * k, 0, 255), base.a };
                ImageDrawPixel(&strip, i * fw + x, y, c);
            }
        }
    }
    m->fills = LoadTextureFromImage(strip);
    UnloadImage(strip);
    SetTextureFilter(m->fills, TEXTURE_FILTER_POINT);
    SetTextureWrap(m->fills, TEXTURE_WRAP_CLAMP);
    m->fill_w = (int16_t)fw;
    m->fill_h = (int16_t)fh;
    return m->fills.id != 0;
}

#define GROUND_COLOR  0.0f
#define GROUND_SLOTS  1.0f
#define GROUND_SHADOW 2.0f

static struct { float info[4]; float see[4]; Texture2D tex; } g_cut;
static float g_view_cos = 1.0f;
static Texture2D g_nocut;

static Texture2D cut_texture(void) {
    if (g_cut.tex.id != 0) return g_cut.tex;
    if (g_nocut.id == 0) {
        static uint8_t none = 255;
        Image img = { .data = &none, .width = 1, .height = 1, .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
        g_nocut = LoadTextureFromImage(img);
    }
    return g_nocut;
}

static void ground_uniforms(Tilemap *m, int32_t layer, float mode, Vector2 down) {
    float params[TILEMAP_MAX_TERRAINS * 4], foam[TILEMAP_MAX_TERRAINS * 4];
    float extra[TILEMAP_MAX_TERRAINS * 4], swatch[TILEMAP_MAX_TERRAINS * 4], sway[TILEMAP_MAX_TERRAINS * 4];
    for (int32_t i = 0; i < m->terrain_count; i++) {
        const TilemapTerrain *t = &m->terrains[i];
        params[i * 4 + 0] = t->noise;
        params[i * 4 + 1] = t->blades;
        params[i * 4 + 2] = t->foam;
        params[i * 4 + 3] = t->foam_speed;
        foam[i * 4 + 0] = t->foam_color.r / 255.0f;
        foam[i * 4 + 1] = t->foam_color.g / 255.0f;
        foam[i * 4 + 2] = t->foam_color.b / 255.0f;
        foam[i * 4 + 3] = t->foam_color.a / 255.0f;
        extra[i * 4 + 0] = t->stairs ? 1.0f : 0.0f;
        extra[i * 4 + 1] = t->waves;
        extra[i * 4 + 2] = t->flow;
        extra[i * 4 + 3] = t->water ? 1.0f : 0.0f;
        sway[i * 4 + 0] = t->sway;
        sway[i * 4 + 1] = t->sway_speed;
        sway[i * 4 + 2] = 0.0f;
        sway[i * 4 + 3] = 0.0f;
        swatch[i * 4 + 0] = m->swatch[i].r / 255.0f;
        swatch[i * 4 + 1] = m->swatch[i].g / 255.0f;
        swatch[i * 4 + 2] = m->swatch[i].b / 255.0f;
        swatch[i * 4 + 3] = 1.0f;
    }
    float raised[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (m->layer_elevation[layer] > 0) {
        raised[0] = (float)Tilemap_layer_height(m, layer);
        raised[1] = (float)m->level_height;
        raised[2] = (float)m->slope_run;
        raised[3] = 1.0f;
    }
    Vector2 map_size  = { (float)m->w, (float)m->h };
    Vector2 fill_size = { (float)m->fill_w, (float)m->fill_h };
    float   tile_size = (float)m->tile;
    float   now       = shader_time();
    int32_t count     = m->terrain_count;
    Shader  sh        = g_ground.f.shader;
    SetShaderValue(sh, g_ground.map_size, &map_size, SHADER_UNIFORM_VEC2);
    SetShaderValue(sh, g_ground.tile_size, &tile_size, SHADER_UNIFORM_FLOAT);
    SetShaderValue(sh, g_ground.fill_size, &fill_size, SHADER_UNIFORM_VEC2);
    SetShaderValue(sh, g_ground.count, &count, SHADER_UNIFORM_INT);
    SetShaderValue(sh, g_ground.time, &now, SHADER_UNIFORM_FLOAT);
    SetShaderValue(sh, g_ground.mode, &mode, SHADER_UNIFORM_FLOAT);
    SetShaderValue(sh, g_ground.view_down, &down, SHADER_UNIFORM_VEC2);
    SetShaderValue(sh, g_ground.raised, raised, SHADER_UNIFORM_VEC4);
    SetShaderValueV(sh, g_ground.params, params, SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(sh, g_ground.foam, foam, SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(sh, g_ground.extra, extra, SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(sh, g_ground.swatch, swatch, SHADER_UNIFORM_VEC4, count);
    SetShaderValueV(sh, g_ground.sway, sway, SHADER_UNIFORM_VEC4, count);
    float none4[4] = { 0, 0, 0, 0 }, none1 = 0.0f;
    SetShaderValue(sh, g_ground.cover_rect, none4, SHADER_UNIFORM_VEC4);
    SetShaderValue(sh, g_ground.water_pass, &none1, SHADER_UNIFORM_FLOAT);
    bool color = mode == GROUND_COLOR;
    SetShaderValue(sh, g_ground.cut_info, color ? g_cut.info : none4, SHADER_UNIFORM_VEC4);
    SetShaderValue(sh, g_ground.see, color ? g_cut.see : none4, SHADER_UNIFORM_VEC4);
    SetShaderValueTexture(sh, g_ground.cut_map, cut_texture());
    SetShaderValue(sh, g_ground.view_cos, &g_view_cos, SHADER_UNIFORM_FLOAT);
}

static void draw_terrain_layer(const Tilemap *cm, int32_t layer, Color tint, float mode, Vector2 down) {
    Tilemap *m = (Tilemap *)cm;
    if (m->terrain_count == 0 || !ground_shader_ready()) return;
    if (!sync_ids(m, layer) || !sync_fills(m)) return;
    float ts = (float)m->tile;
    BeginShaderMode(g_ground.f.shader);
    ground_uniforms(m, layer, mode, down);
    SetShaderValueTexture(g_ground.f.shader, g_ground.fills, m->fills);
    DrawTexturePro(m->ids[layer], (Rectangle){ 0, 0, (float)m->w, (float)m->h },
                   (Rectangle){ 0, 0, m->w * ts, m->h * ts }, (Vector2){ 0, 0 }, 0.0f, tint);
    EndShaderMode();
}

#define CHUNK_PX     512
#define MASK_PAD     4
#define SHADOW_PX    3
#define HEIGHT_EPS   0.01f
#define MAX_RUNS     16

typedef struct LevelGeo {
    Mesh      top;
    Mesh      wtop;
    Texture2D cover;
    bool      water;
} LevelGeo;

typedef struct ChunkGeo {
    bool     built, stale;
    uint32_t gen;
    uint32_t seen;
    double   built_at;
    Mesh     far;
    bool     far_built, far_stale;
    Mesh     walls;
    Mesh     water_walls;
    float    px0, py0, pw, ph;
    LevelGeo level[TILEMAP_MAX_LEVELS];
} ChunkGeo;

static void ensure_chunks(Tilemap *m) {
    if (m->chunk_cells > 0) return;
    int32_t c = CHUNK_PX / m->tile;
    m->chunk_cells = (int16_t)(c < 4 ? 4 : c > 64 ? 64 : c);
    m->chunks_x = (int16_t)((m->w + m->chunk_cells - 1) / m->chunk_cells);
    m->chunks_y = (int16_t)((m->h + m->chunk_cells - 1) / m->chunk_cells);
}

static void meshes_stale(Tilemap *m, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (!m->chunks) return;
    int32_t c = m->chunk_cells;
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= m->w ? m->w - 1 : x1; y1 = y1 >= m->h ? m->h - 1 : y1;
    if (x1 < x0 || y1 < y0) return;
    for (int32_t cy = y0 / c; cy <= y1 / c; cy++) {
        for (int32_t cx = x0 / c; cx <= x1 / c; cx++) {
            m->chunks[cy * m->chunks_x + cx].stale = true;
            m->chunks[cy * m->chunks_x + cx].far_stale = true;
        }
    }
}

static void unload_mesh(Mesh *mesh, bool gpu) {
    if (gpu && mesh->vboId) UnloadMesh(*mesh);
    else if (mesh->vboId) RL_FREE(mesh->vboId);
    *mesh = (Mesh){ 0 };
}

static void unload_chunk(ChunkGeo *c, bool gpu) {
    unload_mesh(&c->walls, gpu);
    unload_mesh(&c->water_walls, gpu);
    for (int32_t k = 0; k < TILEMAP_MAX_LEVELS; k++) {
        unload_mesh(&c->level[k].top, gpu);
        unload_mesh(&c->level[k].wtop, gpu);
        if (gpu && c->level[k].cover.id != 0) UnloadTexture(c->level[k].cover);
        c->level[k].cover = (Texture2D){ 0 };
    }
}

static void free_meshes(Tilemap *m, bool gpu) {
    if (!m->chunks) return;
    for (int32_t i = 0; i < m->chunks_x * m->chunks_y; i++) {
        unload_chunk(&m->chunks[i], gpu);
        unload_mesh(&m->chunks[i].far, gpu);
    }
    free(m->chunks);
    m->chunks = NULL;
}

typedef struct MeshBuf {
    float   *pos, *uv, *nrm;
    uint8_t *col;
    int32_t  n, cap;
    bool     walls;
    bool     failed;
} MeshBuf;

static bool meshbuf_grow(MeshBuf *b, int32_t add) {
    if (b->failed) return false;
    if (b->n + add <= b->cap) return true;
    int32_t cap = b->cap ? b->cap * 2 : 1024;
    while (cap < b->n + add) cap *= 2;
    float *pos = RL_REALLOC(b->pos, (size_t)cap * 3 * sizeof(float));
    if (pos) b->pos = pos;
    float *uv = RL_REALLOC(b->uv, (size_t)cap * 2 * sizeof(float));
    if (uv) b->uv = uv;
    float *nrm = b->walls ? RL_REALLOC(b->nrm, (size_t)cap * 3 * sizeof(float)) : b->nrm;
    if (nrm) b->nrm = nrm;
    uint8_t *col = b->walls ? RL_REALLOC(b->col, (size_t)cap * 4) : b->col;
    if (col) b->col = col;
    if (!pos || !uv || (b->walls && (!nrm || !col))) { b->failed = true; return false; }
    b->cap = cap;
    return true;
}

static void meshbuf_vertex(MeshBuf *b, float x, float h, float y, float u, float v,
                           float nx, float ny, const uint8_t *col) {
    int32_t i = b->n++;
    b->pos[i * 3 + 0] = x; b->pos[i * 3 + 1] = h; b->pos[i * 3 + 2] = y;
    b->uv[i * 2 + 0] = u;  b->uv[i * 2 + 1] = v;
    if (b->walls) {
        b->nrm[i * 3 + 0] = nx; b->nrm[i * 3 + 1] = 0.0f; b->nrm[i * 3 + 2] = ny;
        memcpy(&b->col[i * 4], col, 4);
    }
}

static Mesh meshbuf_upload(MeshBuf *b) {
    Mesh mesh = { 0 };
    if (!b->failed && b->n > 0) {
        mesh.vertexCount = b->n;
        mesh.triangleCount = b->n / 3;
        mesh.vertices = b->pos;
        mesh.texcoords = b->uv;
        mesh.normals = b->walls ? b->nrm : NULL;
        mesh.colors = b->walls ? b->col : NULL;
        UploadMesh(&mesh, false);
        mesh.vertices = NULL; mesh.texcoords = NULL; mesh.normals = NULL; mesh.colors = NULL;
    }
    RL_FREE(b->pos); RL_FREE(b->uv); RL_FREE(b->nrm); RL_FREE(b->col);
    *b = (MeshBuf){ 0 };
    return mesh;
}

static void top_quad(MeshBuf *b, const Tilemap *m, float x0, float y0, float x1, float y1,
                     float h00, float h10, float h01, float h11) {
    if (!meshbuf_grow(b, 6)) return;
    float mw = (float)(m->w * m->tile), mh = (float)(m->h * m->tile);
    float px[6] = { x0, x1, x1, x0, x1, x0 }, py[6] = { y0, y0, y1, y0, y1, y1 };
    float ph[6] = { h00, h10, h11, h00, h11, h01 };
    for (int32_t k = 0; k < 6; k++) meshbuf_vertex(b, px[k], ph[k], py[k], px[k] / mw, py[k] / mh, 0, 0, NULL);
}

#define WALL_FALLS 1u
#define WALL_RISER 2u
#define WALL_SLOPE 4u
#define WALL_SECTION 8u

typedef struct WallSpan { float top_a, bot_a, top_b, bot_b; } WallSpan;

static void wall_quad(MeshBuf *b, float ax, float ay, float bx, float by, WallSpan g, WallSpan sh,
                      float nx, float ny, uint8_t slot, uint8_t style) {
    if (g.top_a - g.bot_a < HEIGHT_EPS && g.top_b - g.bot_b < HEIGHT_EPS) return;
    if (!meshbuf_grow(b, 6)) return;
    uint8_t col[4] = { slot, style, 0, 255 };
    meshbuf_vertex(b, ax, g.bot_a, ay, sh.top_a, sh.bot_a, nx, ny, col);
    meshbuf_vertex(b, bx, g.bot_b, by, sh.top_b, sh.bot_b, nx, ny, col);
    meshbuf_vertex(b, bx, g.top_b, by, sh.top_b, sh.bot_b, nx, ny, col);
    meshbuf_vertex(b, ax, g.bot_a, ay, sh.top_a, sh.bot_a, nx, ny, col);
    meshbuf_vertex(b, bx, g.top_b, by, sh.top_b, sh.bot_b, nx, ny, col);
    meshbuf_vertex(b, ax, g.top_a, ay, sh.top_a, sh.bot_a, nx, ny, col);
}

static RenderTexture2D g_mask;

static bool mask_target(int32_t w, int32_t h) {
    if (g_mask.id != 0 && g_mask.texture.width >= w && g_mask.texture.height >= h) return true;
    if (g_mask.id != 0) {
        w = w > g_mask.texture.width ? w : g_mask.texture.width;
        h = h > g_mask.texture.height ? h : g_mask.texture.height;
        UnloadRenderTexture(g_mask);
    }
    g_mask = LoadRenderTexture(w, h);
    if (g_mask.id == 0) return false;
    SetTextureFilter(g_mask.texture, TEXTURE_FILTER_POINT);
    return true;
}

static void draw_terrain_rect(const Tilemap *cm, int32_t layer, Rectangle r, Color tint, float mode, Vector2 down) {
    Tilemap *m = (Tilemap *)cm;
    if (m->terrain_count == 0 || !ground_shader_ready()) return;
    if (!sync_ids(m, layer) || !sync_fills(m)) return;
    float ts = (float)m->tile;
    BeginShaderMode(g_ground.f.shader);
    ground_uniforms(m, layer, mode, down);
    SetShaderValueTexture(g_ground.f.shader, g_ground.fills, m->fills);
    DrawTexturePro(m->ids[layer], (Rectangle){ r.x / ts, r.y / ts, r.width / ts, r.height / ts }, r,
                   (Vector2){ 0, 0 }, 0.0f, tint);
    EndShaderMode();
}

enum { CELL_NONE = 0, CELL_FLAT, CELL_SMOOTH, CELL_STEP };

#define SMOOTH_PX 4
static int32_t smooth_px(int32_t tile) { return tile % SMOOTH_PX == 0 && tile / SMOOTH_PX <= 64 ? SMOOTH_PX : 1; }

typedef struct LevelWork {
    bool     present;
    bool     full;
    int32_t  layer;
    int32_t  tx, ty;
    uint8_t *kind;
    float   *corner;
    float   *stepped;
    uint8_t *wkind;
    float   *wcorner;
    uint8_t *wkeep;
} LevelWork;

typedef struct ChunkWork {
    const Tilemap *m;
    int32_t  levels;
    float    lh;
    int32_t  px0, py0, pw, ph;
    int32_t  kx0, ky0, kw, kh;
    const uint8_t *atlas;
    int32_t  atlas_w;
    uint32_t *bits;
    uint32_t *water;
    uint32_t *filled;
    uint32_t *steps;
    LevelWork lv[TILEMAP_MAX_LEVELS];
} ChunkWork;

static uint8_t work_mask(const ChunkWork *w, int32_t k, int32_t x, int32_t y) {
    const Tilemap *m = w->m;
    if (!w->lv[k].present || x < 0 || y < 0 || x >= m->w * m->tile || y >= m->h * m->tile) return 0;
    int32_t mx = x - w->px0, my = y - w->py0;
    if (mx < 0 || my < 0 || mx >= w->pw || my >= w->ph) return 0;
    if (w->lv[k].full) return (uint8_t)terrain_cell_slot(m->cells[w->lv[k].layer][(size_t)(y / m->tile) * m->w + x / m->tile]);
    return w->atlas[((size_t)(w->lv[k].ty + my) * w->atlas_w + w->lv[k].tx + mx) * 4];
}

static uint32_t work_bits(const ChunkWork *w, const uint32_t *bits, int32_t x, int32_t y) {
    int32_t mx = x - w->px0, my = y - w->py0;
    if (mx < 0 || my < 0 || mx >= w->pw || my >= w->ph) return 0;
    return bits[(size_t)my * w->pw + mx];
}

static uint8_t work_kind(const ChunkWork *w, int32_t k, int32_t x, int32_t y) {
    if (!w->lv[k].kind) return CELL_NONE;
    int32_t ts = w->m->tile;
    int32_t cx = (x >= 0 ? x / ts : -1) - w->kx0, cy = (y >= 0 ? y / ts : -1) - w->ky0;
    if (cx < 0 || cy < 0 || cx >= w->kw || cy >= w->kh) return CELL_NONE;
    return w->lv[k].kind[cy * w->kw + cx];
}

static float work_corner(const ChunkWork *w, int32_t k, int32_t x, int32_t y) {
    return w->lv[k].corner[(size_t)(y - w->py0) * (w->pw + 1) + (x - w->px0)];
}

static void edge_heights(const ChunkWork *w, int32_t k, int32_t x, int32_t y, int32_t ax, int32_t ay,
                         int32_t bx, int32_t by, float *ha, float *hb) {
    uint8_t kind = work_kind(w, k, x, y);
    if (kind == CELL_STEP) {
        *ha = *hb = w->lv[k].stepped[(size_t)(y - w->py0) * w->pw + (x - w->px0)];
    } else if (kind == CELL_SMOOTH) {
        *ha = work_corner(w, k, ax, ay);
        *hb = work_corner(w, k, bx, by);
    } else {
        *ha = *hb = k * w->lh;
    }
}

typedef struct Run { int8_t k1, k2; float bot, top_a, top_b; } Run;

static int32_t pixel_runs(const ChunkWork *w, const uint32_t *arr, int32_t x, int32_t y, int32_t ax, int32_t ay,
                          int32_t bx, int32_t by, Run *out) {
    uint32_t bits = work_bits(w, arr, x, y);
    int32_t n = 0;
    for (int32_t k = 1; k < w->levels && bits >> k && n < MAX_RUNS; k++) {
        if (!(bits & (1u << k))) continue;
        int32_t k2 = k;
        while (k2 + 1 < w->levels && (bits & (1u << (k2 + 1)))) k2++;
        Run r = { (int8_t)k, (int8_t)k2, (k - 1) * w->lh, 0, 0 };
        edge_heights(w, k2, x, y, ax, ay, bx, by, &r.top_a, &r.top_b);
        out[n++] = r;
        k = k2;
    }
    return n;
}

typedef struct WallRun {
    bool     open, used;
    float    ax, ay, bx, by, nx, ny;
    WallSpan g, sh;
    uint8_t  slot, style;
} WallRun;

typedef struct Tracks {
    WallRun  r[2][TILEMAP_MAX_LEVELS];
    uint32_t live[2];
} Tracks;

static void run_flush(MeshBuf *b, WallRun *r) {
    if (r->open) wall_quad(b, r->ax, r->ay, r->bx, r->by, r->g, r->sh, r->nx, r->ny, r->slot, r->style);
    r->open = false;
}

static bool span_flat(WallSpan s) {
    return fabsf(s.top_a - s.top_b) < HEIGHT_EPS && fabsf(s.bot_a - s.bot_b) < HEIGHT_EPS;
}

static bool span_same(WallSpan s, WallSpan t) {
    return fabsf(s.top_a - t.top_a) < HEIGHT_EPS && fabsf(s.bot_a - t.bot_a) < HEIGHT_EPS;
}

static void run_add(MeshBuf *b, WallRun *r, float ax, float ay, float bx, float by, WallSpan g, WallSpan sh,
                    float nx, float ny, uint8_t slot, uint8_t style) {
    r->used = true;
    bool flat = span_flat(g) && span_flat(sh);
    if (r->open && flat && r->slot == slot && r->style == style && r->nx == nx && r->ny == ny &&
        span_same(r->g, g) && span_same(r->sh, sh) && r->bx == ax && r->by == ay) {
        r->bx = bx; r->by = by;
        return;
    }
    run_flush(b, r);
    if (flat) {
        *r = (WallRun){ true, true, ax, ay, bx, by, nx, ny, g, sh, slot, style };
    } else {
        wall_quad(b, ax, ay, bx, by, g, sh, nx, ny, slot, style);
    }
}

static void emit_piece(const ChunkWork *w, MeshBuf *b, Tracks *tr, int32_t side, int32_t x, int32_t y, const Run *r,
                       float ax, float ay, float bx, float by, float lo_a, float lo_b, float hi_a, float hi_b,
                       float nx, float ny, bool water) {
    const Tilemap *m = w->m;
    float lo = fminf(lo_a, lo_b), hi = fmaxf(hi_a, hi_b);
    if (hi - lo < HEIGHT_EPS) return;
    uint8_t top_kind = work_kind(w, r->k2, x, y);
    for (int32_t k = r->k1; k <= r->k2; k++) {
        float b0 = (k - 1) * w->lh, b1 = k == r->k2 ? 1e9f : k * w->lh;
        if (b1 <= lo || b0 >= hi) continue;
        float pa = fmaxf(lo_a, b0), pb = fmaxf(lo_b, b0);
        float qa = fminf(hi_a, b1), qb = fminf(hi_b, b1);
        if (qa - pa < HEIGHT_EPS && qb - pb < HEIGHT_EPS) continue;
        if (qa < pa) qa = pa;
        if (qb < pb) qb = pb;
        uint8_t mb = work_mask(w, k, x, y), slot = mb & 63u;
        if (slot == 0 || slot > m->terrain_count) {
            slot = work_mask(w, r->k2, x, y) & 63u;
            if (slot == 0 || slot > m->terrain_count) continue;
        }
        uint8_t style = (water || (mb & 64u)) ? WALL_FALLS : 0;
        if (m->terrains[slot - 1].stairs && top_kind == CELL_STEP) style |= WALL_RISER;
        if (k == r->k2 && top_kind == CELL_SMOOTH && hi - lo < m->level_height * 0.5f) style = WALL_SLOPE;
        run_add(b, &tr->r[side][k], ax, ay, bx, by, (WallSpan){ qa, pa, qb, pb }, (WallSpan){ hi_a, lo_a, hi_b, lo_b },
                nx, ny, slot, style);
        tr->live[side] |= 1u << k;
    }
}

static void edge_walls(const ChunkWork *w, MeshBuf *b, Tracks *tr,
                       const uint32_t *own, const uint32_t *other, bool water,
                       int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                       int32_t ax, int32_t ay, int32_t bx, int32_t by, float nx, float ny) {
    uint32_t bp = work_bits(w, own, x0, y0), bq = work_bits(w, own, x1, y1);
    if (bp == 0 && bq == 0) return;
    int32_t ts = w->m->tile;
    if (bp == bq) {
        uint32_t ob = other != own ? work_bits(w, other, x0, y0) : bp;
        if (ob == (other != own ? work_bits(w, other, x1, y1) : bq)) {
            uint32_t tops = (bp & ~(bp >> 1)) | (ob & ~(ob >> 1));
            int32_t pa = (y0 / ts - w->ky0) * w->kw + (x0 / ts - w->kx0), pb = (y1 / ts - w->ky0) * w->kw + (x1 / ts - w->kx0);
            if (x0 >= 0 && y0 >= 0 && x1 >= 0 && y1 >= 0 && !(tops & (w->steps[pa] | w->steps[pb]))) return;
        }
    }
    Run rp[MAX_RUNS], rq[MAX_RUNS], op[MAX_RUNS], oq[MAX_RUNS];
    int32_t np = pixel_runs(w, own, x0, y0, ax, ay, bx, by, rp);
    int32_t nq = pixel_runs(w, own, x1, y1, ax, ay, bx, by, rq);
    int32_t nop = np, noq = nq;
    if (other != own) {
        nop = pixel_runs(w, other, x0, y0, ax, ay, bx, by, op);
        noq = pixel_runs(w, other, x1, y1, ax, ay, bx, by, oq);
    }
    const Run *orp = other != own ? op : rp, *orq = other != own ? oq : rq;
    for (int32_t side = 0; side < 2; side++) {
        const Run *a = side == 0 ? rp : rq, *o = side == 0 ? orq : orp;
        int32_t na = side == 0 ? np : nq, no = side == 0 ? noq : nop;
        int32_t px = side == 0 ? x0 : x1, py = side == 0 ? y0 : y1;
        float fx = side == 0 ? nx : -nx, fy = side == 0 ? ny : -ny;
        for (int32_t i = 0; i < na; i++) {
            const Run *r = &a[i];
            float lo_a = r->bot, lo_b = r->bot;
            float r_mid = 0.5f * (r->top_a + r->top_b);
            for (int32_t j = 0; j < no; j++) {
                const Run *s = &o[j];
                float s_mid = 0.5f * (s->top_a + s->top_b), lo_mid = 0.5f * (lo_a + lo_b);
                if (s->bot >= r_mid || s_mid <= lo_mid) continue;
                if (s->bot > lo_mid + HEIGHT_EPS) {
                    emit_piece(w, b, tr, side, px, py, r, (float)ax, (float)ay, (float)bx, (float)by,
                               lo_a, lo_b, s->bot, s->bot, fx, fy, water);
                }
                lo_a = fmaxf(lo_a, s->top_a);
                lo_b = fmaxf(lo_b, s->top_b);
            }
            if (r_mid > 0.5f * (lo_a + lo_b) + HEIGHT_EPS || r->top_a > lo_a + HEIGHT_EPS || r->top_b > lo_b + HEIGHT_EPS) {
                emit_piece(w, b, tr, side, px, py, r, (float)ax, (float)ay, (float)bx, (float)by,
                           lo_a, lo_b, fmaxf(r->top_a, lo_a), fmaxf(r->top_b, lo_b), fx, fy, water);
            }
        }
    }
}

static void tracks_flush_unused(MeshBuf *b, Tracks *t, bool all) {
    for (int32_t s = 0; s < 2; s++) {
        for (uint32_t m = t->live[s]; m != 0; m &= m - 1) {
            int32_t k = 0;
            while (!((m >> k) & 1u)) k++;
            WallRun *r = &t->r[s][k];
            if (all || !r->used) run_flush(b, r);
            r->used = false;
            if (!r->open) t->live[s] &= ~(1u << k);
        }
    }
}

static int32_t level_layer(const Tilemap *m, int32_t k) {
    for (int32_t l = 0; l < m->layer_count; l++) {
        if (is_terrain_layer(m, l) && m->layer_elevation[l] == k) return l;
    }
    return -1;
}

static void work_free(ChunkWork *w) {
    for (int32_t k = 0; k < TILEMAP_MAX_LEVELS; k++) {
        free(w->lv[k].kind); free(w->lv[k].corner); free(w->lv[k].stepped);
        free(w->lv[k].wkind); free(w->lv[k].wcorner); free(w->lv[k].wkeep);
    }
    free(w->bits); free(w->water); free(w->filled); free(w->steps);
}

static void work_heights(ChunkWork *w, int32_t k, const uint8_t *slopes, uint8_t *kinds, float *corner, float *stepped) {
    const Tilemap *m = w->m;
    LevelWork *lv = &w->lv[k];
    int32_t ts = m->tile, layer = lv->layer;
    const TileRef *cells = m->cells[layer];
    float top = k * w->lh;
    for (int32_t y = 0; y < w->kh; y++) {
        for (int32_t x = 0; x < w->kw; x++) {
            int32_t cx = w->kx0 + x, cy = w->ky0 + y;
            if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h) continue;
            bool near = false;
            for (int32_t dy = -1; dy <= 1 && !near; dy++) {
                for (int32_t dx = -1; dx <= 1 && !near; dx++) {
                    int32_t nx = cx + dx, ny = cy + dy;
                    if (nx >= 0 && ny >= 0 && nx < m->w && ny < m->h &&
                        terrain_cell_slot(cells[(size_t)ny * m->w + nx]) != 0) near = true;
                }
            }
            if (!near) continue;
            uint8_t *kind = &kinds[y * w->kw + x];
            StairGeom sg;
            if (stepped && stairs_geometry(m, layer, cx, cy, &sg)) {
                *kind = CELL_STEP;
                for (int32_t py = cy * ts; py < (cy + 1) * ts; py++) {
                    for (int32_t px = cx * ts; px < (cx + 1) * ts; px++) {
                        int32_t lx = px - w->px0, ly = py - w->py0;
                        if (lx < 0 || ly < 0 || lx >= w->pw || ly >= w->ph) continue;
                        stepped[(size_t)ly * w->pw + lx] = stairs_draw_height(m, layer, &sg, cx, cy, px + 0.5f, py + 0.5f);
                    }
                }
                continue;
            }
            *kind = CELL_FLAT;
            bool sure = slopes != NULL;
            for (int32_t dy = -1; dy <= 1 && sure; dy++) {
                for (int32_t dx = -1; dx <= 1 && sure; dx++) {
                    int32_t nx = cx + dx < 0 ? 0 : cx + dx >= m->w ? m->w - 1 : cx + dx;
                    int32_t ny = cy + dy < 0 ? 0 : cy + dy >= m->h ? m->h - 1 : cy + dy;
                    if (slope_decode(slopes[(size_t)ny * m->w + nx]) * ts < m->slope_run) sure = false;
                }
            }
            if (sure) continue;
            int32_t sp = smooth_px(ts), n = ts / sp;
            float grid[(64 / 1 + 1) * (64 / 1 + 1)];
            for (int32_t gy = 0; gy <= n; gy++) {
                for (int32_t gx = 0; gx <= n; gx++) {
                    float hgt = field_draw_height(m, layer, slopes, (float)(cx * ts + gx * sp), (float)(cy * ts + gy * sp));
                    grid[gy * (n + 1) + gx] = hgt;
                    if (fabsf(hgt - top) > HEIGHT_EPS) *kind = CELL_SMOOTH;
                }
            }
            for (int32_t py = cy * ts; py <= (cy + 1) * ts; py++) {
                for (int32_t px = cx * ts; px <= (cx + 1) * ts; px++) {
                    if (px < w->px0 || py < w->py0 || px > w->px0 + w->pw || py > w->py0 + w->ph) continue;
                    int32_t lx = px - cx * ts, ly = py - cy * ts;
                    int32_t gx = lx / sp < n ? lx / sp : n - 1, gy = ly / sp < n ? ly / sp : n - 1;
                    float fx = (float)(lx - gx * sp) / sp, fy = (float)(ly - gy * sp) / sp;
                    const float *g = &grid[gy * (n + 1) + gx];
                    float hgt = (g[0] * (1 - fx) + g[1] * fx) * (1 - fy) + (g[n + 1] * (1 - fx) + g[n + 2] * fx) * fy;
                    corner[(size_t)(py - w->py0) * (w->pw + 1) + (px - w->px0)] = hgt;
                }
            }
        }
    }
}

static bool work_level(ChunkWork *w, int32_t k) {
    LevelWork *lv = &w->lv[k];
    lv->kind = calloc((size_t)w->kw * w->kh, 1);
    lv->corner = malloc((size_t)(w->pw + 1) * (w->ph + 1) * sizeof(float));
    lv->stepped = calloc((size_t)w->pw * w->ph, sizeof(float));
    if (!lv->kind || !lv->corner || !lv->stepped) return false;
    work_heights(w, k, slope_field(w->m, lv->layer), lv->kind, lv->corner, lv->stepped);
    return true;
}

static bool work_water(ChunkWork *w, int32_t k) {
    const Tilemap *m = w->m;
    LevelWork *lv = &w->lv[k];
    lv->wkind = calloc((size_t)w->kw * w->kh, 1);
    lv->wcorner = malloc((size_t)(w->pw + 1) * (w->ph + 1) * sizeof(float));
    lv->wkeep = calloc((size_t)w->kw * w->kh, 1);
    if (!lv->wkind || !lv->wcorner || !lv->wkeep) return false;
    work_heights(w, k, water_slope_field(m, lv->layer), lv->wkind, lv->wcorner, NULL);
    const TileRef *cells = m->cells[lv->layer];
    int32_t reach = (m->slope_run + m->tile - 1) / m->tile + 1;
    int32_t gw = w->kw + 2 * reach, gh = w->kh + 2 * reach;
    uint8_t *g = calloc((size_t)gw * gh, 1), *row = calloc((size_t)gw * gh, 1);
    if (!g || !row) { free(g); free(row); return false; }
    for (int32_t y = 0; y < gh; y++) {
        int32_t cy = w->ky0 - reach + y;
        if (cy < 0 || cy >= m->h) continue;
        for (int32_t x = 0; x < gw; x++) {
            int32_t cx = w->kx0 - reach + x;
            if (cx >= 0 && cx < m->w) g[(size_t)y * gw + x] = slot_water(m, terrain_cell_slot(cells[(size_t)cy * m->w + cx]));
        }
    }
    for (int32_t y = 0; y < gh; y++) {
        for (int32_t x = reach; x < gw - reach; x++) {
            uint8_t v = 0;
            for (int32_t d = -reach; d <= reach && !v; d++) v = g[(size_t)y * gw + x + d];
            row[(size_t)y * gw + x] = v;
        }
    }
    for (int32_t y = 0; y < w->kh; y++) {
        for (int32_t x = 0; x < w->kw; x++) {
            uint8_t v = 0;
            for (int32_t d = 0; d <= 2 * reach && !v; d++) v = row[(size_t)(y + d) * gw + x + reach];
            lv->wkeep[y * w->kw + x] = v;
        }
    }
    free(g);
    free(row);
    return true;
}

static Mesh level_top(const ChunkWork *w, int32_t k, int32_t cx0, int32_t cy0, int32_t cx1, int32_t cy1,
                      const uint8_t *kinds, const float *corner, const float *st, const uint8_t *keep) {
    const Tilemap *m = w->m;
    int32_t ts = m->tile;
    float top = k * w->lh;
    MeshBuf b = { 0 };
#define KIND_AT(cx, cy) ((keep == NULL || keep[((cy) - w->ky0) * w->kw + ((cx) - w->kx0)]) ? \
                         kinds[((cy) - w->ky0) * w->kw + ((cx) - w->kx0)] : CELL_NONE)
#define CORNER_AT(px, py) corner[(size_t)((py) - w->py0) * (w->pw + 1) + ((px) - w->px0)]
    for (int32_t cy = cy0; cy < cy1; cy++) {
        for (int32_t cx = cx0; cx < cx1; cx++) {
            uint8_t kind = KIND_AT(cx, cy);
            if (kind == CELL_FLAT) {
                int32_t end = cx;
                while (end + 1 < cx1 && KIND_AT(end + 1, cy) == CELL_FLAT) end++;
                top_quad(&b, m, (float)(cx * ts), (float)(cy * ts), (float)((end + 1) * ts), (float)((cy + 1) * ts),
                         top, top, top, top);
                cx = end;
            } else if (kind == CELL_SMOOTH) {
                int32_t sp = smooth_px(ts);
                for (int32_t py = cy * ts; py < (cy + 1) * ts; py += sp) {
                    for (int32_t px = cx * ts; px < (cx + 1) * ts; px += sp) {
                        top_quad(&b, m, (float)px, (float)py, (float)(px + sp), (float)(py + sp),
                                 CORNER_AT(px, py), CORNER_AT(px + sp, py),
                                 CORNER_AT(px, py + sp), CORNER_AT(px + sp, py + sp));
                    }
                }
            } else if (kind == CELL_STEP && st) {
                for (int32_t py = cy * ts; py < (cy + 1) * ts; py++) {
                    for (int32_t px = cx * ts; px < (cx + 1) * ts;) {
                        float h = st[(size_t)(py - w->py0) * w->pw + (px - w->px0)];
                        int32_t end = px + 1;
                        while (end < (cx + 1) * ts && fabsf(st[(size_t)(py - w->py0) * w->pw + (end - w->px0)] - h) < HEIGHT_EPS) end++;
                        top_quad(&b, m, (float)px, (float)py, (float)end, (float)(py + 1), h, h, h, h);
                        px = end;
                    }
                }
            }
        }
    }
#undef KIND_AT
#undef CORNER_AT
    return meshbuf_upload(&b);
}

static void build_chunk(Tilemap *m, int32_t ci, int32_t cj, ChunkGeo *cg) {
    unload_chunk(cg, true);
    cg->built = true;
    cg->stale = false;
    cg->gen = g_shader_gen;

    int32_t ts = m->tile, cc = m->chunk_cells;
    int32_t cx0 = ci * cc, cy0 = cj * cc;
    int32_t cx1 = cx0 + cc > m->w ? m->w : cx0 + cc, cy1 = cy0 + cc > m->h ? m->h : cy0 + cc;
    ChunkWork w = { .m = m, .levels = m->levels, .lh = (float)m->level_height };
    w.kx0 = cx0 - 1; w.ky0 = cy0 - 1; w.kw = cx1 - cx0 + 2; w.kh = cy1 - cy0 + 2;
    w.px0 = cx0 * ts - MASK_PAD; w.py0 = cy0 * ts - MASK_PAD;
    w.pw = (cx1 - cx0) * ts + 2 * MASK_PAD; w.ph = (cy1 - cy0) * ts + 2 * MASK_PAD;
    cg->px0 = (float)w.px0; cg->py0 = (float)w.py0; cg->pw = (float)w.pw; cg->ph = (float)w.ph;

    int32_t present = 0, rendered = 0;
    bool inside_map = w.kx0 >= 0 && w.ky0 >= 0 && w.kx0 + w.kw <= m->w && w.ky0 + w.kh <= m->h;
    for (int32_t k = 1; k < m->levels; k++) {
        int32_t l = level_layer(m, k);
        if (l < 0) continue;
        bool any = false, all = inside_map;
        for (int32_t y = w.ky0 < 0 ? 0 : w.ky0; y < w.ky0 + w.kh && y < m->h; y++) {
            for (int32_t x = w.kx0 < 0 ? 0 : w.kx0; x < w.kx0 + w.kw && x < m->w; x++) {
                int32_t slot = terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]);
                if (slot != 0) any = true;
                if (slot < 1 || slot > m->terrain_count || m->terrains[slot - 1].water || m->terrains[slot - 1].stairs) all = false;
            }
            if (any && !all) break;
        }
        if (!any || !sync_ids(m, l)) continue;
        w.lv[k].present = true;
        w.lv[k].full = all;
        w.lv[k].layer = l;
        present++;
        if (!all) rendered++;
    }
    if (present == 0 || !sync_fills(m)) return;

    int32_t cols = 4096 / w.pw;
    int32_t tiles = rendered > 0 ? rendered : 1;
    cols = cols < 1 ? 1 : cols > tiles ? tiles : cols;
    int32_t rows = (tiles + cols - 1) / cols;
    int32_t aw = cols * w.pw, ah = rows * w.ph;
    if (!mask_target(aw, ah)) return;
    int32_t rt_h = g_mask.texture.height;
    BeginTextureMode(g_mask);
    ClearBackground(BLANK);
    rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
    int32_t idx = 0;
    for (int32_t k = 1; k < m->levels; k++) {
        if (!w.lv[k].present || w.lv[k].full) continue;
        w.lv[k].tx = (idx % cols) * w.pw;
        w.lv[k].ty = (idx / cols) * w.ph;
        idx++;
        Camera2D cam = { .offset = { (float)w.lv[k].tx, (float)(rt_h - ah + w.lv[k].ty) },
                         .target = { (float)w.px0, (float)w.py0 }, .rotation = 0.0f, .zoom = 1.0f };
        BeginMode2D(cam);
        draw_terrain_rect(m, w.lv[k].layer, (Rectangle){ (float)w.px0, (float)w.py0, (float)w.pw, (float)w.ph },
                          WHITE, GROUND_SLOTS, (Vector2){ 0, 0 });
        EndMode2D();
    }
    EndBlendMode();
    rlDrawRenderBatchActive();
    unsigned char *atlas = rlReadScreenPixels(aw, ah);
    EndTextureMode();
    if (!atlas) return;
    w.atlas = atlas;
    w.atlas_w = aw;

    size_t npx = (size_t)w.pw * w.ph;
    w.bits = calloc(npx, sizeof(uint32_t));
    w.water = calloc(npx, sizeof(uint32_t));
    w.filled = calloc(npx, sizeof(uint32_t));
    bool ok = w.bits && w.water && w.filled;
    bool has_water[TILEMAP_MAX_LEVELS] = { false };
    bool water_slot[64] = { false };
    for (int32_t i = 1; i < 64; i++) water_slot[i] = slot_water(m, i);
    for (int32_t k = 1; k < m->levels && ok; k++) {
        if (!w.lv[k].present) continue;
        ok = work_level(&w, k);
        int32_t xs = w.px0 < 0 ? -w.px0 : 0, ys = w.py0 < 0 ? -w.py0 : 0;
        int32_t xe = w.px0 + w.pw > m->w * ts ? m->w * ts - w.px0 : w.pw;
        int32_t ye = w.py0 + w.ph > m->h * ts ? m->h * ts - w.py0 : w.ph;
        uint32_t bit = 1u << k;
        if (w.lv[k].full) {
            for (int32_t y = ys; y < ye; y++) {
                for (int32_t x = xs; x < xe; x++) {
                    w.bits[(size_t)y * w.pw + x] |= bit;
                    w.filled[(size_t)y * w.pw + x] |= bit;
                }
            }
            continue;
        }
        for (int32_t y = ys; y < ye && ok; y++) {
            const uint8_t *row = &atlas[((size_t)(w.lv[k].ty + y) * w.atlas_w + w.lv[k].tx) * 4];
            size_t i0 = (size_t)y * w.pw;
            for (int32_t x = xs; x < xe; x++) {
                uint32_t slot = row[x * 4] & 63u;
                if (!slot) continue;
                if (water_slot[slot]) { w.water[i0 + x] |= bit; has_water[k] = true; }
                else                  w.bits[i0 + x] |= bit;
                w.filled[i0 + x] |= bit;
            }
        }
    }
    for (int32_t k = 1; k < m->levels && ok; k++) {
        if (w.lv[k].present && has_water[k]) ok = work_water(&w, k);
    }
    w.steps = calloc((size_t)w.kw * w.kh, sizeof(uint32_t));
    ok = ok && w.steps;
    for (int32_t k = 1; k < m->levels && ok; k++) {
        if (!w.lv[k].present) continue;
        for (int32_t i = 0; i < w.kw * w.kh; i++) if (w.lv[k].kind[i] == CELL_STEP) w.steps[i] |= 1u << k;
    }
    if (!ok) { RL_FREE(atlas); work_free(&w); return; }

    int32_t ground = level_layer(m, 0);
    for (int32_t k = 1; k < m->levels; k++) {
        if (!w.lv[k].present) continue;
        LevelWork *lv = &w.lv[k];
        cg->level[k].top = level_top(&w, k, cx0, cy0, cx1, cy1, lv->kind, lv->corner, lv->stepped, NULL);
        cg->level[k].water = has_water[k];
        if (has_water[k]) cg->level[k].wtop = level_top(&w, k, cx0, cy0, cx1, cy1, lv->wkind, lv->wcorner, NULL, lv->wkeep);
        const uint8_t *gfield = has_water[k] ? slope_field(m, lv->layer) : NULL;
        const uint8_t *wfield = has_water[k] ? water_slope_field(m, lv->layer) : NULL;
        if ((k + 1 < m->levels && w.lv[k + 1].present) || has_water[k]) {
            uint8_t *px = RL_MALLOC(npx);
            Image img = { .data = px, .width = w.pw, .height = w.ph, .mipmaps = 1,
                          .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
            for (size_t i = 0; i < npx; i++) {
                uint8_t v = (uint8_t)((((w.bits[i] >> (k + 1)) & 1u) << 7) | (((w.water[i] >> (k + 1)) & 1u) << 6));
                if ((w.water[i] >> k) & 1u) {
                    int32_t j = k;
                    while (j >= 1 && ((w.water[i] >> j) & 1u)) j--;
                    int32_t depth = k - j;
                    if (j == 0) {
                        int32_t gx = (w.px0 + (int32_t)(i % w.pw)) / ts, gy = (w.py0 + (int32_t)(i / w.pw)) / ts;
                        if (ground < 0 || gx < 0 || gy < 0 || gx >= m->w || gy >= m->h ||
                            slot_water(m, terrain_cell_slot(m->cells[ground][(size_t)gy * m->w + gx]))) depth = 31;
                    }
                    v |= (uint8_t)depth;
                } else if (gfield && wfield != gfield && v == 0 && ((w.bits[i] >> k) & 1u)) {
                    int32_t x = w.px0 + (int32_t)(i % w.pw), y = w.py0 + (int32_t)(i / w.pw);
                    int32_t kc = (y / ts - w.ky0) * w.kw + (x / ts - w.kx0);
                    if (lv->wkeep[kc] && lv->kind[kc] == CELL_SMOOTH) {
                        float wx = x + 0.5f, wy = y + 0.5f;
                        float dg = field_dist(m, gfield, wx, wy), dw = field_dist(m, wfield, wx, wy);
                        if (dg < (float)m->slope_run && dg < dw - 0.5f) {
                            float run = (float)m->slope_run;
                            float under = (w.lh - 1.0f) * (smoothstepf(0.0f, run, dw) - smoothstepf(0.0f, run, dg));
                            if (under > 0.5f) {
                                int32_t q = (int32_t)(31.0f * under / w.lh + 0.5f);
                                v = (uint8_t)(32 + (q < 1 ? 1 : q > 31 ? 31 : q));
                            }
                        }
                    }
                }
                px[i] = v;
            }
            cg->level[k].cover = LoadTextureFromImage(img);
            SetTextureFilter(cg->level[k].cover, TEXTURE_FILTER_POINT);
            SetTextureWrap(cg->level[k].cover, TEXTURE_WRAP_CLAMP);
            UnloadImage(img);
        }
    }

    bool any_water = false;
    for (int32_t k = 1; k < m->levels; k++) any_water = any_water || has_water[k];
    for (int32_t pass = 0; pass < (any_water ? 2 : 1); pass++) {
        const uint32_t *own = pass == 0 ? w.bits : w.water, *other = pass == 0 ? w.bits : w.filled;
        MeshBuf faces = { .walls = true };
        static Tracks tracks;
        int32_t X0 = cx0 * ts, Y0 = cy0 * ts, X1 = cx1 * ts, Y1 = cy1 * ts;
#define SAME_EDGE(ip, iq, cp, cq) \
        (own[ip] == own[iq] && other[ip] == other[iq] && \
         !((((own[ip] & ~(own[ip] >> 1)) | (other[ip] & ~(other[ip] >> 1)))) & (w.steps[cp] | w.steps[cq])))
        for (int32_t x = X0 - (ci == 0 ? 1 : 0); x < X1; x++) {
            memset(&tracks, 0, sizeof(tracks));
            int32_t cxp = (x >= 0 ? x / ts : -1) - w.kx0, cxq = (x + 1) / ts - w.kx0;
            for (int32_t y = Y0; y < Y1; y++) {
                size_t ip = (size_t)(y - w.py0) * w.pw + (x - w.px0);
                int32_t row = (y / ts - w.ky0) * w.kw;
                if (!(x >= 0 && SAME_EDGE(ip, ip + 1, row + cxp, row + cxq)))
                    edge_walls(&w, &faces, &tracks, own, other, pass == 1, x, y, x + 1, y, x + 1, y, x + 1, y + 1, 1.0f, 0.0f);
                if (tracks.live[0] | tracks.live[1]) tracks_flush_unused(&faces, &tracks, false);
            }
            tracks_flush_unused(&faces, &tracks, true);
        }
        for (int32_t y = Y0 - (cj == 0 ? 1 : 0); y < Y1; y++) {
            memset(&tracks, 0, sizeof(tracks));
            int32_t rp = ((y >= 0 ? y / ts : -1) - w.ky0) * w.kw, rq = ((y + 1) / ts - w.ky0) * w.kw;
            for (int32_t x = X0; x < X1; x++) {
                size_t ip = (size_t)(y - w.py0) * w.pw + (x - w.px0);
                int32_t cx = x / ts - w.kx0;
                if (!(y >= 0 && SAME_EDGE(ip, ip + w.pw, rp + cx, rq + cx)))
                    edge_walls(&w, &faces, &tracks, own, other, pass == 1, x, y, x, y + 1, x, y + 1, x + 1, y + 1, 0.0f, 1.0f);
                if (tracks.live[0] | tracks.live[1]) tracks_flush_unused(&faces, &tracks, false);
            }
            tracks_flush_unused(&faces, &tracks, true);
        }
#undef SAME_EDGE
        if (pass == 0) cg->walls = meshbuf_upload(&faces);
        else           cg->water_walls = meshbuf_upload(&faces);
    }
    RL_FREE(atlas);
    work_free(&w);
}

#define BUILD_BUDGET_MS 8.0
#define MAX_RESIDENT_CHUNKS 160
#define REBUILD_COOLDOWN_S 0.1

static void update_meshes(Tilemap *m, Rectangle view) {
    double t0 = GetTime();
    m->stat_built_frame = 0;
    m->stat_pending = 0;
    m->frame_no++;
    ensure_chunks(m);
    if (!m->chunks) {
        m->chunks = calloc((size_t)m->chunks_x * m->chunks_y, sizeof(ChunkGeo));
        if (!m->chunks) return;
    }
    int32_t cc = m->chunk_cells, ts = m->tile;
    int32_t ci0 = (int32_t)floorf(view.x / (cc * ts)), cj0 = (int32_t)floorf(view.y / (cc * ts));
    int32_t ci1 = (int32_t)floorf((view.x + view.width) / (cc * ts)), cj1 = (int32_t)floorf((view.y + view.height) / (cc * ts));
    ci0 = ci0 < 0 ? 0 : ci0; cj0 = cj0 < 0 ? 0 : cj0;
    ci1 = ci1 >= m->chunks_x ? m->chunks_x - 1 : ci1; cj1 = cj1 >= m->chunks_y ? m->chunks_y - 1 : cj1;
    float mx = (view.x + view.width * 0.5f) / (cc * ts) - 0.5f, my = (view.y + view.height * 0.5f) / (cc * ts) - 0.5f;
    for (int32_t cj = cj0; cj <= cj1; cj++) {
        for (int32_t ci = ci0; ci <= ci1; ci++) m->chunks[cj * m->chunks_x + ci].seen = m->frame_no;
    }
    for (;;) {
        int32_t best = -1;
        float best_d = 0.0f;
        for (int32_t cj = cj0; cj <= cj1; cj++) {
            for (int32_t ci = ci0; ci <= ci1; ci++) {
                const ChunkGeo *c = &m->chunks[cj * m->chunks_x + ci];
                if (c->built && !c->stale && c->gen == g_shader_gen) continue;
                if (c->built && c->gen == g_shader_gen && t0 - c->built_at < REBUILD_COOLDOWN_S) continue;
                float d = (ci - mx) * (ci - mx) + (cj - my) * (cj - my);
                if (best < 0 || d < best_d) { best = cj * m->chunks_x + ci; best_d = d; }
            }
        }
        if (best < 0) break;
        if (m->stat_built_frame > 0 && (GetTime() - t0) * 1000.0 >= BUILD_BUDGET_MS) {
            for (int32_t cj = cj0; cj <= cj1; cj++) {
                for (int32_t ci = ci0; ci <= ci1; ci++) {
                    const ChunkGeo *c = &m->chunks[cj * m->chunks_x + ci];
                    if (!c->built || c->stale || c->gen != g_shader_gen) m->stat_pending++;
                }
            }
            break;
        }
        build_chunk(m, best % m->chunks_x, best / m->chunks_x, &m->chunks[best]);
        m->chunks[best].built_at = GetTime();
        m->stat_built_frame++;
    }
    int32_t total = m->chunks_x * m->chunks_y, built = 0;
    for (int32_t i = 0; i < total; i++) built += m->chunks[i].built;
    while (built > MAX_RESIDENT_CHUNKS) {
        int32_t oldest = -1;
        for (int32_t i = 0; i < total; i++) {
            const ChunkGeo *c = &m->chunks[i];
            if (c->built && c->seen != m->frame_no && (oldest < 0 || c->seen < m->chunks[oldest].seen)) oldest = i;
        }
        if (oldest < 0) break;
        unload_chunk(&m->chunks[oldest], true);
        m->chunks[oldest].built = false;
        built--;
    }
    m->stat_build_ms = (float)((GetTime() - t0) * 1000.0);
}

static Material *mesh_material(Shader sh, Texture2D tex0, Texture2D tex1, Texture2D tex2, Texture2D tex3, Color tint);

#define FAR_ZOOM 0.5f
#define FAR_STEP 2

static uint8_t surface_of(const Tilemap *m, int32_t x, int32_t y, int32_t *slot) {
    *slot = 0;
    int32_t l = floor_layer(m, x, y, m->levels - 1);
    if (l < 0) return 255;
    int32_t k = m->layer_elevation[l];
    *slot = terrain_cell_slot(m->cells[l][(size_t)y * m->w + x]);
    int32_t d = water_over(m, x, y, k, l, NULL);
    if (d > 0) {
        int32_t wl = block_layer(m, x, y, k + d);
        if (wl >= 0) *slot = terrain_cell_slot(m->cells[wl][(size_t)y * m->w + x]);
    }
    return (uint8_t)(k + d);
}

static bool sync_overview(Tilemap *m) {
    size_t n = cell_count(m);
    bool whole = m->ov_tex.id == 0 || !m->ov_level;
    if (!m->ov_level) {
        m->ov_level = malloc(n);
        if (!m->ov_level) return false;
    }
    int32_t x0 = 0, y0 = 0, x1 = m->w - 1, y1 = m->h - 1;
    if (!whole) {
        if (m->ov_dirty[2] < m->ov_dirty[0]) return true;
        x0 = m->ov_dirty[0] - 1; y0 = m->ov_dirty[1] - 1; x1 = m->ov_dirty[2] + 1; y1 = m->ov_dirty[3] + 1;
        x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
        x1 = x1 >= m->w ? m->w - 1 : x1; y1 = y1 >= m->h ? m->h - 1 : y1;
    }
    m->ov_dirty[0] = m->ov_dirty[1] = 0;
    m->ov_dirty[2] = m->ov_dirty[3] = -1;
    int32_t rw = x1 - x0 + 1, rh = y1 - y0 + 1;
    uint8_t *slots = malloc((size_t)rw * rh);
    Color *px = malloc((size_t)rw * rh * sizeof(Color));
    if (!slots || !px) { free(slots); free(px); return false; }
    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            int32_t slot;
            m->ov_level[(size_t)y * m->w + x] = surface_of(m, x, y, &slot);
            slots[(size_t)(y - y0) * rw + (x - x0)] = (uint8_t)slot;
        }
    }
    float lv = (float)(m->levels > 1 ? m->levels - 1 : 1);
    for (int32_t y = y0; y <= y1; y++) {
        for (int32_t x = x0; x <= x1; x++) {
            uint8_t k = m->ov_level[(size_t)y * m->w + x];
            int32_t slot = slots[(size_t)(y - y0) * rw + (x - x0)];
            Color *c = &px[(size_t)(y - y0) * rw + (x - x0)];
            if (k == 255 || slot < 1 || slot > m->terrain_count) { *c = BLANK; continue; }
            int32_t xa = x > 0 ? x - 1 : x, ya = y > 0 ? y - 1 : y;
            int32_t xb = x + 1 < m->w ? x + 1 : x, yb = y + 1 < m->h ? y + 1 : y;
            uint8_t ka = m->ov_level[(size_t)ya * m->w + xa], kb = m->ov_level[(size_t)yb * m->w + xb];
            ka = ka == 255 ? k : ka; kb = kb == 255 ? k : kb;
            float shade = 0.88f + 0.12f * (k / lv) + 0.09f * clampf((float)(ka - kb), -2.5f, 2.5f);
            Color sw = m->swatch[slot - 1];
            *c = (Color){ (uint8_t)clampf(sw.r * shade, 0, 255), (uint8_t)clampf(sw.g * shade, 0, 255),
                          (uint8_t)clampf(sw.b * shade, 0, 255), 255 };
        }
    }
    if (whole) {
        if (m->ov_tex.id != 0) UnloadTexture(m->ov_tex);
        Image img = { .data = px, .width = rw, .height = rh, .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        m->ov_tex = LoadTextureFromImage(img);
        SetTextureFilter(m->ov_tex, TEXTURE_FILTER_POINT);
        SetTextureWrap(m->ov_tex, TEXTURE_WRAP_CLAMP);
    } else {
        UpdateTextureRec(m->ov_tex, (Rectangle){ (float)x0, (float)y0, (float)rw, (float)rh }, px);
    }
    free(slots);
    free(px);
    return m->ov_tex.id != 0;
}

static void build_far(Tilemap *m, int32_t ci, int32_t cj, ChunkGeo *cg) {
    unload_mesh(&cg->far, true);
    cg->far_built = true;
    cg->far_stale = false;
    int32_t cc = m->chunk_cells, ts = m->tile;
    int32_t cx0 = ci * cc, cy0 = cj * cc;
    int32_t cx1 = cx0 + cc > m->w ? m->w : cx0 + cc, cy1 = cy0 + cc > m->h ? m->h : cy0 + cc;
    float lh = (float)m->level_height;
    MeshBuf b = { 0 };
    float hgt[64 / FAR_STEP + 1][64 / FAR_STEP + 1];
    int32_t nx = (cx1 - cx0 + FAR_STEP - 1) / FAR_STEP, ny = (cy1 - cy0 + FAR_STEP - 1) / FAR_STEP;
    for (int32_t j = 0; j <= ny; j++) {
        for (int32_t i = 0; i <= nx; i++) {
            int32_t gx = cx0 + i * FAR_STEP, gy = cy0 + j * FAR_STEP;
            gx = gx > m->w ? m->w : gx; gy = gy > m->h ? m->h : gy;
            float sum = 0.0f;
            int32_t count = 0;
            for (int32_t dy = -1; dy <= 0; dy++) {
                for (int32_t dx = -1; dx <= 0; dx++) {
                    int32_t x = gx + dx, y = gy + dy;
                    if (x < 0 || y < 0 || x >= m->w || y >= m->h) continue;
                    uint8_t k = m->ov_level[(size_t)y * m->w + x];
                    if (k == 255) continue;
                    sum += k * lh;
                    count++;
                }
            }
            hgt[j][i] = count ? sum / count : 0.0f;
        }
    }
    for (int32_t j = 0; j < ny; j++) {
        for (int32_t i = 0; i < nx; i++) {
            float x0 = (float)((cx0 + i * FAR_STEP) * ts), y0 = (float)((cy0 + j * FAR_STEP) * ts);
            float x1 = fminf((float)((cx0 + (i + 1) * FAR_STEP) * ts), (float)(cx1 * ts));
            float y1 = fminf((float)((cy0 + (j + 1) * FAR_STEP) * ts), (float)(cy1 * ts));
            top_quad(&b, m, x0, y0, x1, y1, hgt[j][i], hgt[j][i + 1], hgt[j + 1][i], hgt[j + 1][i + 1]);
        }
    }
    cg->far = meshbuf_upload(&b);
}

static void update_far(Tilemap *m, Rectangle view) {
    double t0 = GetTime();
    m->stat_built_frame = 0;
    m->stat_pending = 0;
    ensure_chunks(m);
    if (!m->chunks) {
        m->chunks = calloc((size_t)m->chunks_x * m->chunks_y, sizeof(ChunkGeo));
        if (!m->chunks) return;
    }
    if (!sync_overview(m)) return;
    int32_t cc = m->chunk_cells, ts = m->tile;
    int32_t ci0 = (int32_t)floorf(view.x / (cc * ts)), cj0 = (int32_t)floorf(view.y / (cc * ts));
    int32_t ci1 = (int32_t)floorf((view.x + view.width) / (cc * ts)), cj1 = (int32_t)floorf((view.y + view.height) / (cc * ts));
    ci0 = ci0 < 0 ? 0 : ci0; cj0 = cj0 < 0 ? 0 : cj0;
    ci1 = ci1 >= m->chunks_x ? m->chunks_x - 1 : ci1; cj1 = cj1 >= m->chunks_y ? m->chunks_y - 1 : cj1;
    for (int32_t cj = cj0; cj <= cj1; cj++) {
        for (int32_t ci = ci0; ci <= ci1; ci++) {
            ChunkGeo *c = &m->chunks[cj * m->chunks_x + ci];
            if (c->far_built && !c->far_stale) continue;
            if (m->stat_built_frame > 0 && (GetTime() - t0) * 1000.0 >= BUILD_BUDGET_MS) { m->stat_pending++; continue; }
            build_far(m, ci, cj, c);
            m->stat_built_frame++;
        }
    }
    m->stat_build_ms = (float)((GetTime() - t0) * 1000.0);
}

static void draw_far(const Tilemap *cm, Rectangle view) {
    Tilemap *m = (Tilemap *)cm;
    if (!m->chunks || m->ov_tex.id == 0) return;
    int32_t cc = m->chunk_cells, ts = m->tile;
    int32_t ci0 = (int32_t)floorf(view.x / (cc * ts)), cj0 = (int32_t)floorf(view.y / (cc * ts));
    int32_t ci1 = (int32_t)floorf((view.x + view.width) / (cc * ts)), cj1 = (int32_t)floorf((view.y + view.height) / (cc * ts));
    ci0 = ci0 < 0 ? 0 : ci0; cj0 = cj0 < 0 ? 0 : cj0;
    ci1 = ci1 >= m->chunks_x ? m->chunks_x - 1 : ci1; cj1 = cj1 >= m->chunks_y ? m->chunks_y - 1 : cj1;
    rlDrawRenderBatchActive();
    Shader def = { rlGetShaderIdDefault(), rlGetShaderLocsDefault() };
    Material *mat = mesh_material(def, m->ov_tex, (Texture2D){ 0 }, (Texture2D){ 0 }, (Texture2D){ 0 }, WHITE);
    for (int32_t cj = cj0; cj <= cj1 && mat; cj++) {
        for (int32_t ci = ci0; ci <= ci1; ci++) {
            const ChunkGeo *c = &m->chunks[cj * m->chunks_x + ci];
            if (c->far_built && c->far.vertexCount > 0) DrawMesh(c->far, *mat, MatrixIdentity());
        }
    }
}

void Tilemap_mesh_stats(const Tilemap *m, TilemapMeshStats *out) {
    memset(out, 0, sizeof(*out));
    if (!m) return;
    out->far = m->stat_far;
    out->built_last_frame = m->stat_built_frame;
    out->build_ms_last_frame = m->stat_build_ms;
    out->pending = m->stat_pending;
    if (!m->chunks) return;
    for (int32_t i = 0; i < m->chunks_x * m->chunks_y; i++) {
        const ChunkGeo *c = &m->chunks[i];
        out->chunks++;
        out->vertices += c->far.vertexCount;
        if (!c->built) continue;
        out->built++;
        out->vertices += c->walls.vertexCount + c->water_walls.vertexCount;
        for (int32_t k = 0; k < TILEMAP_MAX_LEVELS; k++) out->vertices += c->level[k].top.vertexCount + c->level[k].wtop.vertexCount;
    }
}

#define ROOM_RADIUS 32

static bool cut_room(Tilemap *m, int32_t x, int32_t y, int32_t z) {
    int32_t L = m->levels;
    if (x < 0 || y < 0 || x >= m->w || y >= m->h || z < 0 || z + 1 >= L) return false;
    if (solid_at(m, x, y, z + 1) || !Tilemap_covered(m, x, y, z)) return false;
    int32_t R = ROOM_RADIUS, side = 2 * R + 1, ox = x - R, oy = y - R;
    size_t n = (size_t)side * side;
    int8_t *top = malloc(n);
    int16_t *floor_of = malloc(n * sizeof(int16_t));
    uint8_t *seen = calloc(n * L, 1);
    int32_t *queue = malloc(n * L * sizeof(int32_t));
    bool ok = top && floor_of && seen && queue;
    if (ok) {
        for (size_t i = 0; i < n; i++) {
            int32_t cx = ox + (int32_t)(i % side), cy = oy + (int32_t)(i / side);
            int32_t t = -1;
            if (cx >= 0 && cy >= 0 && cx < m->w && cy < m->h) {
                for (int32_t k = L - 1; k >= 1 && t < 0; k--) if (solid_at(m, cx, cy, k)) t = k;
            }
            top[i] = (int8_t)t;
            floor_of[i] = -1;
        }
        int32_t head = 0, count = 0;
        int32_t start = (R * side + R) * L + z + 1;
        seen[start] = 1;
        queue[count++] = start;
        while (head < count) {
            int32_t i = queue[head++], col = i / L, k = i % L;
            int32_t cx = ox + col % side, cy = oy + col / side;
            if (k == 1 || solid_at(m, cx, cy, k - 1)) {
                int32_t f = k - 1, cur = floor_of[col];
                int32_t df = f > z ? f - z : z - f, dc = cur > z ? cur - z : z - cur;
                if (cur < 0 || df < dc || (df == dc && f < cur)) floor_of[col] = (int16_t)f;
            }
            const int32_t nb[6][2] = { { -1, 0 }, { 1, 0 }, { -side, 0 }, { side, 0 }, { 0, -1 }, { 0, 1 } };
            for (int32_t d = 0; d < 6; d++) {
                int32_t ncol = col + nb[d][0], nk = k + nb[d][1];
                if (d == 0 && col % side == 0) continue;
                if (d == 1 && col % side == side - 1) continue;
                if (ncol < 0 || ncol >= (int32_t)n || nk < 1 || nk >= L) continue;
                int32_t j = ncol * L + nk;
                if (seen[j] || top[ncol] <= nk) continue;
                int32_t nx = ox + ncol % side, ny = oy + ncol / side;
                if (solid_at(m, nx, ny, nk)) continue;
                seen[j] = 1;
                queue[count++] = j;
            }
        }
        size_t cells = cell_count(m);
        memset(m->cut, 255, cells);
        int32_t *q = (int32_t *)queue, qn = 0, qh = 0;
        if ((size_t)n * L < cells) {
            free(queue);
            queue = q = malloc(cells * sizeof(int32_t));
        }
        if (q) {
            for (size_t i = 0; i < n; i++) {
                if (floor_of[i] < 0) continue;
                int32_t cx = ox + (int32_t)(i % side), cy = oy + (int32_t)(i / side);
                int32_t c = floor_of[i] + 1;
                m->cut[(size_t)cy * m->w + cx] = (uint8_t)(c > 254 ? 254 : c);
                q[qn++] = cy * m->w + cx;
            }
            while (qh < qn) {
                int32_t i = q[qh++], cx = i % m->w, cy = i / m->w;
                const int32_t dx[4] = { 1, -1, 0, 0 }, dy[4] = { 0, 0, 1, -1 };
                for (int32_t d = 0; d < 4; d++) {
                    int32_t nx = cx + dx[d], ny = cy + dy[d];
                    if (nx < 0 || ny < 0 || nx >= m->w || ny >= m->h) continue;
                    int32_t j = ny * m->w + nx;
                    if (m->cut[j] != 255) continue;
                    m->cut[j] = m->cut[i];
                    q[qn++] = j;
                }
            }
        }
        ok = q != NULL;
    }
    free(top); free(floor_of); free(seen); free(queue);
    return ok;
}

static bool cut_request(Tilemap *m, int32_t kind, int32_t x, int32_t y, int32_t z) {
    if (kind == 1) x = y = 0;
    int32_t key[5] = { kind, x, y, z, (int32_t)m->edit_gen };
    if (m->cut && memcmp(key, m->cut_key, sizeof(key)) == 0) return m->cut_any;
    memcpy(m->cut_key, key, sizeof(key));
    m->cut_gpu_stale = true;
    m->cut_any = false;
    size_t cells = cell_count(m);
    if (!m->cut) m->cut = malloc(cells);
    if (!m->cut) return false;
    if (kind == 1) {
        int32_t c = z < 0 ? 0 : z > 254 ? 254 : z;
        memset(m->cut, c, cells);
        m->cut_any = true;
    } else if (kind == 2) {
        m->cut_any = cut_room(m, x, y, z);
    }
    if (!m->cut_any) { memset(m->cut, 255, cells); return false; }
    m->cut_max = 0;
    for (size_t i = 0; i < cells; i++) if (m->cut[i] != 255 && m->cut[i] > m->cut_max) m->cut_max = m->cut[i];
    m->cut_here = kind == 1 ? m->cut[0] : m->cut[(size_t)y * m->w + x];
    return true;
}

static void cut_sync_gpu(Tilemap *m) {
    if (!m->cut_gpu_stale || !m->cut) return;
    m->cut_gpu_stale = false;
    Image img = { .data = m->cut, .width = m->w, .height = m->h, .mipmaps = 1,
                  .format = PIXELFORMAT_UNCOMPRESSED_GRAYSCALE };
    if (m->cut_tex.id == 0) {
        m->cut_tex = LoadTextureFromImage(img);
        SetTextureFilter(m->cut_tex, TEXTURE_FILTER_POINT);
        SetTextureWrap(m->cut_tex, TEXTURE_WRAP_CLAMP);
    } else {
        UpdateTexture(m->cut_tex, m->cut);
    }
    if (m->cut_faces.vertexCount > 0) UnloadMesh(m->cut_faces);
    m->cut_faces = (Mesh){ 0 };
    if (!m->cut_any) return;
    MeshBuf b = { .walls = true };
    float ts = (float)m->tile, lh = (float)m->level_height;
    for (int32_t y = 0; y < m->h; y++) {
        for (int32_t x = 0; x < m->w; x++) {
            for (int32_t d = 0; d < 2; d++) {
                int32_t nx = x + (d == 0), ny = y + (d == 1);
                if (nx >= m->w || ny >= m->h) continue;
                int32_t ca = m->cut[(size_t)y * m->w + x], cb = m->cut[(size_t)ny * m->w + nx];
                if (ca == cb || ca == 255 || cb == 255) continue;
                bool a_low = ca < cb;
                int32_t lx = a_low ? x : nx, ly = a_low ? y : ny, hx = a_low ? nx : x, hy = a_low ? ny : y;
                int32_t lo = a_low ? ca : cb, hi = a_low ? cb : ca;
                float ax = d == 0 ? (float)nx * ts : (float)x * ts, ay = d == 0 ? (float)y * ts : (float)ny * ts;
                float bx = d == 0 ? ax : ax + ts, by = d == 0 ? ay + ts : ay;
                float fx = d == 0 ? (a_low ? -1.0f : 1.0f) : 0.0f, fy = d == 1 ? (a_low ? -1.0f : 1.0f) : 0.0f;
                int32_t run0 = -1;
                for (int32_t k = lo + 1; k <= hi + 1; k++) {
                    bool both = k <= hi && k < m->levels && solid_at(m, lx, ly, k) && solid_at(m, hx, hy, k);
                    if (both && run0 < 0) run0 = k;
                    if (!both && run0 >= 0) {
                        int32_t tl = block_layer(m, hx, hy, k - 1);
                        int32_t slot = tl >= 0 ? terrain_cell_slot(m->cells[tl][(size_t)hy * m->w + hx]) : 1;
                        WallSpan g = { (k - 1) * lh, (run0 - 1) * lh, (k - 1) * lh, (run0 - 1) * lh };
                        wall_quad(&b, ax, ay, bx, by, g, g, fx, fy, (uint8_t)slot, WALL_SECTION);
                        run0 = -1;
                    }
                }
            }
        }
    }
    m->cut_faces = meshbuf_upload(&b);
}

bool Tilemap_raycast_room(const Tilemap *cm, Vector3 o, Vector3 dir, float max_dist, int32_t x, int32_t y, int32_t z,
                          Vector3 *hit) {
    Tilemap *m = (Tilemap *)cm;
    if (!m) return false;
    bool cut = cut_request(m, 2, x, y, z);
    return raycast_cut(m, o, dir, max_dist, -1, cut ? m->cut : NULL, hit);
}

static Color cliff_color(const Tilemap *m, int32_t slot) {
    const TilemapTerrain *t = &m->terrains[slot - 1];
    if (t->cliff.a) return t->cliff;
    Color s = m->swatch[slot - 1];
    return (Color){ (uint8_t)(s.r * 0.3f + 96 * 0.7f), (uint8_t)(s.g * 0.3f + 84 * 0.7f),
                    (uint8_t)(s.b * 0.3f + 72 * 0.7f), 255 };
}

static Material g_material;
static bool     g_material_ok;

static Material *mesh_material(Shader sh, Texture2D tex0, Texture2D tex1, Texture2D tex2, Texture2D tex3, Color tint) {
    if (!g_material_ok) {
        g_material = LoadMaterialDefault();
        g_material_ok = g_material.maps != NULL;
        if (!g_material_ok) return NULL;
    }
    g_material.shader = sh;
    g_material.maps[MATERIAL_MAP_DIFFUSE].texture = tex0;
    g_material.maps[MATERIAL_MAP_DIFFUSE].color = tint;
    g_material.maps[MATERIAL_MAP_SPECULAR].texture = tex1;
    g_material.maps[MATERIAL_MAP_NORMAL].texture = tex2;
    g_material.maps[MATERIAL_MAP_ROUGHNESS].texture = tex3;
    return &g_material;
}

typedef struct RaisedView {
    int32_t top;
    bool    water;
    uint8_t alpha[TILEMAP_MAX_LEVELS];
} RaisedView;

#define DIM_RGB 110

static void draw_raised(const Tilemap *cm, Rectangle view, const RaisedView *rv, Vector2 down) {
    Tilemap *m = (Tilemap *)cm;
    if (!m->chunks || !ground_shader_ready()) return;
    int32_t cc = m->chunk_cells, ts = m->tile;
    int32_t ci0 = (int32_t)floorf(view.x / (cc * ts)), cj0 = (int32_t)floorf(view.y / (cc * ts));
    int32_t ci1 = (int32_t)floorf((view.x + view.width) / (cc * ts)), cj1 = (int32_t)floorf((view.y + view.height) / (cc * ts));
    ci0 = ci0 < 0 ? 0 : ci0; cj0 = cj0 < 0 ? 0 : cj0;
    ci1 = ci1 >= m->chunks_x ? m->chunks_x - 1 : ci1; cj1 = cj1 >= m->chunks_y ? m->chunks_y - 1 : cj1;
    rlDrawRenderBatchActive();
    int32_t top_level = rv->top < m->levels - 1 ? rv->top : m->levels - 1;
    Texture2D cut = cut_texture();
    float pass = rv->water ? 1.0f : 0.0f;

    for (int32_t k = top_level; k >= 1; k--) {
        int32_t layer = level_layer(m, k);
        if (layer < 0 || !m->layer_visible[layer]) continue;
        Color tint = { 255, 255, 255, rv->alpha[k] };
        Shader sh = g_ground.f.shader;
        ground_uniforms(m, layer, GROUND_COLOR, down);
        SetShaderValue(sh, g_ground.water_pass, &pass, SHADER_UNIFORM_FLOAT);
        for (int32_t cj = cj0; cj <= cj1; cj++) {
            for (int32_t ci = ci0; ci <= ci1; ci++) {
                const ChunkGeo *c = &m->chunks[cj * m->chunks_x + ci];
                const Mesh *mesh = rv->water ? &c->level[k].wtop : &c->level[k].top;
                if (!c->built || mesh->vertexCount == 0 || (rv->water && !c->level[k].water)) continue;
                float rect[4] = { c->px0, c->py0, c->level[k].cover.id ? c->pw : 0.0f, c->ph };
                SetShaderValue(sh, g_ground.cover_rect, rect, SHADER_UNIFORM_VEC4);
                Material *mat = mesh_material(sh, m->ids[layer], m->fills, c->level[k].cover, cut, tint);
                if (mat) DrawMesh(*mesh, *mat, MatrixIdentity());
            }
        }
    }

    if (!wall_shader_ready()) return;
    float cliff[TILEMAP_MAX_TERRAINS * 4], swatch[TILEMAP_MAX_TERRAINS * 4];
    for (int32_t i = 0; i < m->terrain_count; i++) {
        Color c = cliff_color(m, i + 1), s = m->swatch[i];
        cliff[i * 4 + 0] = c.r / 255.0f; cliff[i * 4 + 1] = c.g / 255.0f; cliff[i * 4 + 2] = c.b / 255.0f;
        cliff[i * 4 + 3] = m->terrains[i].stairs ? 1.0f : 0.0f;
        swatch[i * 4 + 0] = s.r / 255.0f; swatch[i * 4 + 1] = s.g / 255.0f; swatch[i * 4 + 2] = s.b / 255.0f;
        swatch[i * 4 + 3] = 1.0f;
    }
    Shader ws = g_wall.f.shader;
    float now = shader_time();
    Vector2 map_size = { (float)m->w, (float)m->h };
    float tile_size = (float)ts, level_height = (float)m->level_height;
    SetShaderValueV(ws, g_wall.cliff, cliff, SHADER_UNIFORM_VEC4, m->terrain_count);
    SetShaderValueV(ws, g_wall.swatch, swatch, SHADER_UNIFORM_VEC4, m->terrain_count);
    SetShaderValue(ws, g_wall.time, &now, SHADER_UNIFORM_FLOAT);
    SetShaderValue(ws, g_wall.cut_info, g_cut.info, SHADER_UNIFORM_VEC4);
    SetShaderValue(ws, g_wall.see, g_cut.see, SHADER_UNIFORM_VEC4);
    SetShaderValue(ws, g_wall.map_size, &map_size, SHADER_UNIFORM_VEC2);
    SetShaderValue(ws, g_wall.tile_size, &tile_size, SHADER_UNIFORM_FLOAT);
    SetShaderValue(ws, g_wall.level_height, &level_height, SHADER_UNIFORM_FLOAT);
    SetShaderValue(ws, g_wall.water_pass, &pass, SHADER_UNIFORM_FLOAT);
    Material *wmat = mesh_material(ws, (Texture2D){ 0 }, (Texture2D){ 0 }, (Texture2D){ 0 }, cut,
                                   (Color){ 255, 255, 255, rv->alpha[0] });
    for (int32_t cj = cj0; cj <= cj1 && wmat; cj++) {
        for (int32_t ci = ci0; ci <= ci1; ci++) {
            const ChunkGeo *c = &m->chunks[cj * m->chunks_x + ci];
            const Mesh *mesh = rv->water ? &c->water_walls : &c->walls;
            if (c->built && mesh->vertexCount > 0) DrawMesh(*mesh, *wmat, MatrixIdentity());
        }
    }
    if (wmat && !rv->water && g_cut.info[0] > 0.5f && m->cut_faces.vertexCount > 0) {
        DrawMesh(m->cut_faces, *wmat, MatrixIdentity());
    }
}

static void push_board(float h) {
    Matrix b = { 1, 0, 0, 0,
                 0, 0, 0, h,
                 0, 1, 0, 0,
                 0, 0, 0, 1 };
    rlPushMatrix();
    rlMultMatrixf(MatrixToFloat(b));
}

typedef struct VisibleCells { int32_t x0, y0, x1, y1; } VisibleCells;

static VisibleCells cells_for(const Tilemap *m, Rectangle r, int32_t overhang) {
    float ts = (float)m->tile;
    int32_t x0 = (int32_t)floorf(r.x / ts) - overhang;
    int32_t y0 = (int32_t)floorf(r.y / ts) - overhang;
    int32_t x1 = (int32_t)ceilf((r.x + r.width) / ts);
    int32_t y1 = (int32_t)ceilf((r.y + r.height) / ts);
    return (VisibleCells){ x0 < 0 ? 0 : x0 > m->w ? m->w : x0, y0 < 0 ? 0 : y0 > m->h ? m->h : y0,
                           x1 < 0 ? 0 : x1 > m->w ? m->w : x1, y1 < 0 ? 0 : y1 > m->h ? m->h : y1 };
}

static Texture2D marker_texture(void) {
    if (g_marker.id == 0) {
        Image img = GenImageColor(12, 12, BLANK);
        ImageDrawCircle(&img, 6, 6, 5, (Color){ 20, 16, 16, 255 });
        ImageDrawCircle(&img, 6, 6, 4, (Color){ 250, 110, 80, 255 });
        g_marker = LoadTextureFromImage(img);
        UnloadImage(img);
        SetTextureFilter(g_marker, TEXTURE_FILTER_POINT);
    }
    return g_marker;
}

static void draw_sprite_card(const Tilemap *m, const TilemapSprite *sp, Vector3 right, float stretch) {
    if (sp->texture.id == 0 || sp->h <= 0.0f || sp->src.height <= 0.0f) return;
    float w = sp->w > 0.0f ? sp->w : sp->src.width;
    float y0 = sp->z >= 0.0f ? sp->z : (float)((sp->level < 0 ? 0 : sp->level) * m->level_height);
    float y1 = y0 + sp->h * stretch;
    Vector3 l = { sp->x - right.x * w * 0.5f, 0, sp->y - right.z * w * 0.5f };
    Vector3 r = { sp->x + right.x * w * 0.5f, 0, sp->y + right.z * w * 0.5f };
    float tw = (float)sp->texture.width, th = (float)sp->texture.height;
    float u0 = sp->src.x / tw, u1 = (sp->src.x + sp->src.width) / tw;
    float v0 = sp->src.y / th, v1 = (sp->src.y + sp->src.height) / th;
    rlSetTexture(sp->texture.id);
    rlBegin(RL_QUADS);
    rlColor4ub(sp->tint.r, sp->tint.g, sp->tint.b, sp->tint.a);
    rlTexCoord2f(u0, v1); rlVertex3f(l.x, y0, l.z);
    rlTexCoord2f(u1, v1); rlVertex3f(r.x, y0, r.z);
    rlTexCoord2f(u1, v0); rlVertex3f(r.x, y1, r.z);
    rlTexCoord2f(u0, v0); rlVertex3f(l.x, y1, l.z);
    rlEnd();
    rlSetTexture(0);
}

static void draw_arrows(const Tilemap *m, int32_t layer, VisibleCells c, float px) {
    float ts = (float)m->tile;
    Color col = { 255, 255, 255, 200 };
    for (int32_t y = c.y0; y < c.y1; y++) {
        for (int32_t x = c.x0; x < c.x1; x++) {
            TileRef v = m->cells[layer][(size_t)y * m->w + x];
            int32_t dir = terrain_cell_dir(v);
            if (dir < 0 || terrain_cell_slot(v) == 0) continue;
            float a = dir * (PI / 180.0f);
            Vector2 d = { sinf(a), -cosf(a) };
            Vector2 n = { -d.y, d.x };
            Vector2 mid = { (x + 0.5f) * ts, (y + 0.5f) * ts };
            float r = ts * 0.32f;
            Vector2 tip = { mid.x + d.x * r, mid.y + d.y * r };
            Vector2 tail = { mid.x - d.x * r, mid.y - d.y * r };
            DrawLineEx(tail, tip, 1.5f * px, col);
            DrawLineEx(tip, (Vector2){ tip.x - (d.x - n.x) * r * 0.5f, tip.y - (d.y - n.y) * r * 0.5f }, 1.5f * px, col);
            DrawLineEx(tip, (Vector2){ tip.x - (d.x + n.x) * r * 0.5f, tip.y - (d.y + n.y) * r * 0.5f }, 1.5f * px, col);
        }
    }
}

static void draw_flags(const Tilemap *m, int32_t layer, VisibleCells c, float px) {
    float ts = (float)m->tile;
    for (int32_t y = c.y0; y < c.y1; y++) {
        for (int32_t x = c.x0; x < c.x1; x++) {
            TileRef f = Tilemap_flags(m, layer, x, y);
            if (!f) continue;
            TileRef own = m->cells[layer][(size_t)y * m->w + x] & TILEMAP_CELL_FLAGS;
            Rectangle r = { x * ts, y * ts, ts, ts };
            if (f & TILEMAP_CELL_WATER) {
                DrawRectangleRec(r, (Color){ 70, 150, 255, (own & TILEMAP_CELL_WATER) ? 110 : 40 });
            }
            if (f & TILEMAP_CELL_SOLID) {
                uint8_t a = (own & TILEMAP_CELL_SOLID) ? 120 : 50;
                DrawRectangleRec(r, (Color){ 235, 70, 60, a });
                Color line = { 255, 120, 110, (uint8_t)(a + 80) };
                DrawLineEx((Vector2){ r.x + 3, r.y + 3 }, (Vector2){ r.x + ts - 3, r.y + ts - 3 }, 1.5f * px, line);
                DrawLineEx((Vector2){ r.x + ts - 3, r.y + 3 }, (Vector2){ r.x + 3, r.y + ts - 3 }, 1.5f * px, line);
            }
        }
    }
}

static void draw_ghost(const Tilemap *m, const TilemapView *v) {
    if (v->g_terrain > 0) {
        Color c = Fade(Tilemap_terrain_swatch(m, v->g_terrain), 0.5f);
        float ts = (float)m->tile;
        DrawRectangleRec((Rectangle){ v->gx * ts, v->gy * ts, v->g_w * ts, v->g_h * ts }, c);
        return;
    }
    const TilemapTileset *t = Tilemap_tileset(m, v->g_tileset);
    if (!t || t->cols <= 0) return;
    Texture2D tex = TextureStore_get(t->texture);
    if (tex.id == 0) return;
    float ts = (float)m->tile;
    bool skinned = t->skin[0] && begin_skin(t);
    for (int32_t j = 0; j < v->g_h; j++) {
        for (int32_t i = 0; i < v->g_w; i++) {
            int32_t col = v->g_tx + i, row = v->g_ty + j;
            int32_t cx = v->gx + i, cy = v->gy + j;
            if (col < 0 || row < 0 || col >= t->cols || row >= t->rows) continue;
            if (cx < 0 || cy < 0 || cx >= m->w || cy >= m->h) continue;
            Rectangle dst = { cx * ts, cy * ts, (float)t->tile_w, (float)t->tile_h };
            DrawTexturePro(tex, tile_src(t, row * t->cols + col), dst, (Vector2){ 0, 0 }, 0.0f,
                           Fade(WHITE, 0.6f));
        }
    }
    if (skinned) EndShaderMode();
}

static Vector3 to3d(Vector3 b) { return (Vector3){ b.x, b.z, b.y }; }

static void draw_ribbon(const Vector3 *pts, int32_t n, Vector3 toward, Vector3 fallback, float half, Color c) {
    for (int32_t i = 0; i + 1 < n; i++) {
        Vector3 a = to3d(pts[i]), b = to3d(pts[i + 1]);
        Vector3 seg = { b.x - a.x, b.y - a.y, b.z - a.z };
        Vector3 side = { seg.y * toward.z - seg.z * toward.y, seg.z * toward.x - seg.x * toward.z,
                         seg.x * toward.y - seg.y * toward.x };
        float l = sqrtf(side.x * side.x + side.y * side.y + side.z * side.z);
        side = l > 1e-5f ? (Vector3){ side.x / l * half, side.y / l * half, side.z / l * half }
                         : (Vector3){ fallback.x * half, fallback.y * half, fallback.z * half };
        rlBegin(RL_QUADS);
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlVertex3f(a.x - side.x, a.y - side.y, a.z - side.z);
        rlVertex3f(a.x + side.x, a.y + side.y, a.z + side.z);
        rlVertex3f(b.x + side.x, b.y + side.y, b.z + side.z);
        rlVertex3f(b.x - side.x, b.y - side.y, b.z - side.z);
        rlEnd();
    }
}

typedef struct ViewCam ViewCam;
static void draw_overlays(const Tilemap *m, const TilemapView *v, const ViewCam *vc, float zoom);

#define ORTHO_DISTANCE 20000.0f

struct ViewCam {
    Camera3D cam;
    Vector3  toward;
    Vector3  up, right;
    float    tilt;
    float    distance;
};

static float view_tilt(const Tilemap *m, const TilemapView *v) {
    float t = v->tilt >= 0.0f ? v->tilt : m->tilt;
    return t < 0.0f ? 0.0f : t > 85.0f ? 85.0f : t;
}

static ViewCam view_camera(const Tilemap *m, const TilemapView *v, float w, float h) {
    ViewCam c = { 0 };
    float zoom = v->zoom > 0.01f ? v->zoom : 1.0f;
    float th = v->rotation * (PI / 180.0f);
    c.tilt = view_tilt(m, v);
    float ph = c.tilt * (PI / 180.0f);
    float st = sinf(th), ct = cosf(th), sp = sinf(ph), cp = cosf(ph);
    c.toward = (Vector3){ sp * st, cp, sp * ct };
    c.up     = (Vector3){ -st * cp, sp, -ct * cp };
    c.right  = (Vector3){ ct, 0.0f, -st };
    Vector3 target = { v->x, 0.0f, v->y };
    if (v->fov > 0.0f) {
        c.distance = h / (2.0f * zoom * tanf(v->fov * 0.5f * (PI / 180.0f)));
        c.cam.fovy = v->fov;
        c.cam.projection = CAMERA_PERSPECTIVE;
    } else {
        c.distance = ORTHO_DISTANCE;
        c.cam.fovy = h / zoom;
        c.cam.projection = CAMERA_ORTHOGRAPHIC;
    }
    c.cam.target = target;
    c.cam.position = (Vector3){ target.x + c.toward.x * c.distance, c.toward.y * c.distance,
                                target.z + c.toward.z * c.distance };
    c.cam.up = c.up;
    (void)w;
    return c;
}

static Ray view_ray(const ViewCam *c, float sx, float sy, float w, float h) {
    float ax = 2.0f * sx / w - 1.0f, ay = 1.0f - 2.0f * sy / h, aspect = w / h;
    Vector3 f = { -c->toward.x, -c->toward.y, -c->toward.z };
    if (c->cam.projection == CAMERA_PERSPECTIVE) {
        float tf = tanf(c->cam.fovy * 0.5f * (PI / 180.0f));
        Vector3 d = { f.x + (c->right.x * ax * aspect + c->up.x * ay) * tf,
                      f.y + (c->right.y * ax * aspect + c->up.y * ay) * tf,
                      f.z + (c->right.z * ax * aspect + c->up.z * ay) * tf };
        return (Ray){ c->cam.position, d };
    }
    float half = c->cam.fovy * 0.5f;
    Vector3 o = { c->cam.position.x + (c->right.x * ax * aspect + c->up.x * ay) * half,
                  c->cam.position.y + (c->right.y * ax * aspect + c->up.y * ay) * half,
                  c->cam.position.z + (c->right.z * ax * aspect + c->up.z * ay) * half };
    return (Ray){ o, f };
}

static Vector2 ray_board(Ray r, float height) {
    float t = r.direction.y < -1e-6f ? (height - r.position.y) / r.direction.y : 1e6f;
    if (t > 1e6f) t = 1e6f;
    return (Vector2){ r.position.x + r.direction.x * t, r.position.z + r.direction.z * t };
}

static struct { bool ok; ViewCam c; float x, y, w, h, scale; } g_last;

bool Tilemap_pick(float sx, float sy, float height, float *wx, float *wy) {
    if (!g_last.ok) return false;
    float s = g_last.scale;
    Vector2 p = ray_board(view_ray(&g_last.c, (sx - g_last.x) * s, (sy - g_last.y) * s, g_last.w, g_last.h), height);
    *wx = p.x;
    *wy = p.y;
    return true;
}

static void draw_overlays(const Tilemap *m, const TilemapView *v, const ViewCam *vc, float zoom) {
    float ts = (float)m->tile;
    float px = 1.0f / zoom;
    rlDrawRenderBatchActive();
    rlDisableDepthMask();
    rlEnableDepthTest();
    for (int32_t i = 0; i < v->mark_count && v->marks; i++) {
        const TilemapMark *mk = &v->marks[i];
        float h;
        int32_t lv;
        h = mk->level >= 0 ? Tilemap_floor_height(m, (mk->x + 0.5f) * ts, (mk->y + 0.5f) * ts, mk->level, &lv)
                           : Tilemap_height_at(m, (mk->x + 0.5f) * ts, (mk->y + 0.5f) * ts, &lv);
        if (lv < 0) continue;
        push_board(h + 0.4f);
        Rectangle r = { mk->x * ts + 1.0f, mk->y * ts + 1.0f, ts - 2.0f, ts - 2.0f };
        DrawRectangleRec(r, mk->color);
        DrawRectangleLinesEx(r, 1.5f * px, (Color){ mk->color.r, mk->color.g, mk->color.b, 230 });
        rlPopMatrix();
    }
    static Vector3 shadow[TILEMAP_MAX_ARC_PTS];
    for (int32_t i = 0; i < v->arc_count && v->arcs; i++) {
        const TilemapArc *a = &v->arcs[i];
        int32_t n = a->count < TILEMAP_MAX_ARC_PTS ? a->count : TILEMAP_MAX_ARC_PTS;
        for (int32_t k = 0; k < n; k++) {
            float h = 0.0f;
            if (!floor_under(m, a->points[k].x, a->points[k].y, a->points[k].z, &h, NULL)) h = 0.0f;
            shadow[k] = (Vector3){ a->points[k].x, a->points[k].y, h + 0.4f };
        }
        draw_ribbon(shadow, n, (Vector3){ 0, 1, 0 }, vc->right, a->width * 0.5f * px, (Color){ 0, 0, 0, 70 });
    }
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    for (int32_t pass = 0; pass < 2; pass++) {
        for (int32_t i = 0; i < v->arc_count && v->arcs; i++) {
            const TilemapArc *a = &v->arcs[i];
            Color c = a->color;
            if (pass == 0) c.a = (uint8_t)(c.a * 0.3f);
            draw_ribbon(a->points, a->count, vc->toward, vc->up, a->width * 0.5f * px, c);
        }
        rlDrawRenderBatchActive();
        rlEnableDepthTest();
    }
    rlDisableDepthTest();
    for (int32_t i = 0; i < v->ring_count && v->rings; i++) {
        const TilemapRing *r = &v->rings[i];
        push_board(r->at.z + 0.5f);
        DrawRing((Vector2){ r->at.x, r->at.y }, r->radius - 1.2f * px, r->radius + 1.2f * px, 0.0f, 360.0f, 32, r->color);
        rlPopMatrix();
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableDepthTest();
}

static RenderTexture2D g_view_rt;

static bool view_target(int32_t w, int32_t h) {
    if (g_view_rt.id != 0 && g_view_rt.texture.width == w && g_view_rt.texture.height == h) return true;
    if (g_view_rt.id != 0) UnloadRenderTexture(g_view_rt);
    g_view_rt = LoadRenderTexture(w, h);
    return g_view_rt.id != 0;
}

void Tilemap_draw(const Tilemap *m, const TilemapView *v, float x, float y, float w, float h) {
    DrawRectangle((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, v->bg);
    int32_t iw = (int32_t)roundf(w), ih = (int32_t)roundf(h);
    int32_t ss = v->supersample >= 2 ? 2 : 1;
    if (!m || iw < 2 || ih < 2 || !view_target(iw * ss, ih * ss)) return;
    SetTextureFilter(g_view_rt.texture, ss > 1 ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT);

    float zoom = v->zoom > 0.01f ? v->zoom : 1.0f;
    float ts = (float)m->tile;
    float map_w = m->w * ts, map_h = m->h * ts;
    TilemapView sv = *v;
    sv.zoom = zoom * ss;
    float rw = (float)(iw * ss), rh = (float)(ih * ss);
    ViewCam vc = view_camera(m, &sv, rw, rh);
    g_last.ok = true;
    g_last.c = vc;
    g_last.x = x; g_last.y = y; g_last.w = rw; g_last.h = rh; g_last.scale = (float)ss;

    float th = v->rotation * (PI / 180.0f);
    Vector2 down = { sinf(th), cosf(th) };

    bool raised[TILEMAP_MAX_LAYERS] = { false }, raised_terrain = false;
    float top_h = 0.0f;
    for (int32_t l = 0; l < m->layer_count; l++) {
        if (!m->layer_visible[l] || m->layer_elevation[l] == 0) continue;
        raised[l] = true;
        float lh = (float)Tilemap_layer_height(m, l);
        if (lh > top_h) top_h = lh;
        if (m->layer_kind[l] == TILEMAP_LAYER_TERRAIN) raised_terrain = true;
    }

    Rectangle view;
    {
        float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
        float sxs[4] = { 0, rw, 0, rw }, sys[4] = { 0, 0, rh, rh };
        float hs[2] = { 0.0f, top_h };
        for (int32_t i = 0; i < 4; i++) {
            Ray r = view_ray(&vc, sxs[i], sys[i], rw, rh);
            for (int32_t k = 0; k < 2; k++) {
                Vector2 p = ray_board(r, hs[k]);
                if (p.x < minx) minx = p.x;
                if (p.x > maxx) maxx = p.x;
                if (p.y < miny) miny = p.y;
                if (p.y > maxy) maxy = p.y;
            }
        }
        float pad = ts;
        minx = fmaxf(minx, -pad); miny = fmaxf(miny, -pad);
        maxx = fminf(maxx, map_w + pad); maxy = fminf(maxy, map_h + pad);
        view = (Rectangle){ minx, miny, fmaxf(maxx - minx, 1.0f), fmaxf(maxy - miny, 1.0f) };
    }
    int32_t overhang = 0;
    for (int32_t s = 0; s < m->tileset_count; s++) {
        int32_t big = m->tilesets[s].tile_w > m->tilesets[s].tile_h ? m->tilesets[s].tile_w : m->tilesets[s].tile_h;
        int32_t cells_over = (int32_t)ceilf(big / ts) - 1;
        if (cells_over > overhang) overhang = cells_over;
    }
    VisibleCells vis = cells_for(m, view, overhang);

    bool meshed = raised_terrain && m->terrain_count > 0 && ground_shader_ready();
    bool far_view = meshed && zoom < FAR_ZOOM;
    ((Tilemap *)m)->stat_far = far_view;
    if (far_view) update_far((Tilemap *)m, view);
    else if (meshed) update_meshes((Tilemap *)m, view);
    int32_t cut_kind = 0, rx = 0, ry = 0, rz = 0;
    if (v->room || v->see_through) {
        cut_kind = 2;
        rx = v->room ? v->room_x : (int32_t)floorf(v->st_x / m->tile);
        ry = v->room ? v->room_y : (int32_t)floorf(v->st_y / m->tile);
        rz = v->room ? v->room_z : Tilemap_floor_at(m, rx, ry, (int32_t)floorf(v->st_z / m->level_height + 0.5f), NULL);
    } else if (v->cut_level >= 0) {
        cut_kind = 1;
        rz = v->cut_level;
    }
    bool cutting = cut_kind != 0 && !far_view && cut_request((Tilemap *)m, cut_kind, rx, ry, rz);
    if (cutting) cut_sync_gpu((Tilemap *)m);
    bool section = cutting && v->section;
    bool everywhere = cutting && !v->see_through;
    int32_t cut = everywhere ? m->cut_here : -1;
    memset(&g_cut, 0, sizeof(g_cut));
    g_view_cos = cosf((v->tilt >= 0.0f ? v->tilt : m->tilt) * DEG2RAD);
    if (cutting) {
        g_cut.info[0] = v->see_through ? 2.0f : 1.0f;
        g_cut.info[1] = section ? 1.0f : 0.0f;
        g_cut.tex = m->cut_tex;
    }

    uint8_t alpha[TILEMAP_MAX_LAYERS];
    for (int32_t l = 0; l < m->layer_count; l++) {
        bool faded = v->dim && v->active_layer >= 0 && l != v->active_layer;
        alpha[l] = faded ? 90 : 255;
    }

    BeginTextureMode(g_view_rt);
    ClearBackground(v->bg);
    float diag = sqrtf(map_w * map_w + map_h * map_h);
    float near = vc.cam.projection == CAMERA_PERSPECTIVE ? fmaxf(1.0f, vc.distance * 0.05f) : 1.0f;
    float far = vc.distance + diag + 2000.0f;
    rlSetClipPlanes(near, far);
    BeginMode3D(vc.cam);
    rlDisableBackfaceCulling();

    rlDisableDepthTest();
    push_board(0.0f);
    Color shade = { (uint8_t)(v->bg.r * 0.6f), (uint8_t)(v->bg.g * 0.6f), (uint8_t)(v->bg.b * 0.6f), 255 };
    DrawRectangleRec((Rectangle){ 0, 0, map_w, map_h }, shade);
    for (int32_t l = 0; l < m->layer_count && !far_view; l++) {
        if (!m->layer_visible[l] || m->layer_elevation[l] > 0) continue;
        Color tint = alpha[l] < 255 ? Fade(WHITE, 0.35f) : WHITE;
        if (section && cut >= 2 && m->layer_kind[l] != TILEMAP_LAYER_TERRAIN) tint = (Color){ DIM_RGB, DIM_RGB, DIM_RGB, tint.a };
        if (m->layer_kind[l] == TILEMAP_LAYER_TERRAIN) draw_terrain_layer(m, l, tint, GROUND_COLOR, down);
        else draw_layer(m, l, vis.x0, vis.y0, vis.x1, vis.y1, 0.0f, tint);
    }
    if (meshed && !far_view && cut != 0) {
        rlPushMatrix();
        rlTranslatef(down.x * SHADOW_PX, down.y * SHADOW_PX, 0.0f);
        for (int32_t l = 0; l < m->layer_count; l++) {
            if (!raised[l] || m->layer_kind[l] != TILEMAP_LAYER_TERRAIN || m->layer_elevation[l] != 1) continue;
            draw_terrain_layer(m, l, (Color){ 255, 255, 255, alpha[l] }, GROUND_SHADOW, down);
        }
        rlPopMatrix();
    }
    rlPopMatrix();
    rlDrawRenderBatchActive();

    rlEnableDepthTest();
    RaisedView rv = { .top = everywhere ? m->cut_max : m->levels - 1 };
    if (far_view) {
        draw_far(m, view);
    } else if (meshed) {
        for (int32_t k = 0; k < TILEMAP_MAX_LEVELS; k++) rv.alpha[k] = 255;
        for (int32_t l = 0; l < m->layer_count; l++) {
            if (raised[l] && m->layer_kind[l] == TILEMAP_LAYER_TERRAIN) rv.alpha[m->layer_elevation[l]] = alpha[l];
        }
        if (cutting && v->see_through) {
            Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
            float px = v->st_x, py = v->st_z, pz = v->st_y;
            float cx = mvp.m0 * px + mvp.m4 * py + mvp.m8 * pz + mvp.m12;
            float cy = mvp.m1 * px + mvp.m5 * py + mvp.m9 * pz + mvp.m13;
            float cz = mvp.m2 * px + mvp.m6 * py + mvp.m10 * pz + mvp.m14;
            float cw = mvp.m3 * px + mvp.m7 * py + mvp.m11 * pz + mvp.m15;
            if (cw > 1e-6f) {
                g_cut.see[0] = (cx / cw * 0.5f + 0.5f) * rw;
                g_cut.see[1] = (cy / cw * 0.5f + 0.5f) * rh;
                g_cut.see[2] = v->st_radius * (float)ss;
                g_cut.see[3] = cz / cw * 0.5f + 0.5f;
            }
        }
        draw_raised(m, view, &rv, down);
    }


    for (int32_t l = 0; l < m->layer_count && !far_view; l++) {
        if (!raised[l] || m->layer_kind[l] == TILEMAP_LAYER_TERRAIN) continue;
        if (cut >= 0 && m->layer_elevation[l] > cut) continue;
        bool dim = section && cut >= 0 && m->layer_elevation[l] < cut - 1;
        push_board((float)Tilemap_layer_height(m, l) + 0.05f);
        draw_layer(m, l, vis.x0, vis.y0, vis.x1, vis.y1, 0.0f,
                   (Color){ dim ? DIM_RGB : 255, dim ? DIM_RGB : 255, dim ? DIM_RGB : 255, alpha[l] });
        rlPopMatrix();
    }
    rlDrawRenderBatchActive();

    float stretch = 1.0f / fmaxf(sinf(vc.tilt * (PI / 180.0f)), 0.2f);
    bool alpha_test = sprite_shader_ready();
    if (alpha_test) BeginShaderMode(g_sprite.f.shader);
    for (int32_t i = 0; i < v->sprite_count && v->sprites; i++) draw_sprite_card(m, &v->sprites[i], vc.right, stretch);
    if (v->marker) {
        Texture2D mt = marker_texture();
        TilemapSprite sp = { .texture = mt, .src = { 0, 0, (float)mt.width, (float)mt.height },
                             .x = v->mx, .y = v->my, .w = (float)mt.width, .h = (float)mt.height,
                             .level = v->marker_level, .z = v->marker_z, .tint = WHITE };
        draw_sprite_card(m, &sp, vc.right, stretch);
    }
    if (alpha_test) EndShaderMode();
    rlDrawRenderBatchActive();

    if (meshed && !far_view) {
        rlDisableDepthMask();
        rv.water = true;
        draw_raised(m, view, &rv, down);
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
    }

    if (v->mark_count > 0 || v->arc_count > 0 || v->ring_count > 0) draw_overlays(m, v, &vc, zoom);

    rlDisableDepthTest();
    float lift = v->active_layer >= 0 ? (float)Tilemap_layer_height(m, v->active_layer) : 0.0f;
    float px = 1.0f / zoom;
    push_board(lift);
    if (v->ghost) draw_ghost(m, v);
    VisibleCells g = cells_for(m, view, 0);
    if (v->arrows && v->active_layer >= 0 && v->active_layer < m->layer_count &&
        m->layer_kind[v->active_layer] == TILEMAP_LAYER_TERRAIN && ts * zoom >= 10.0f) {
        draw_arrows(m, v->active_layer, g, px);
    }
    if (v->flags && v->active_layer >= 0 && v->active_layer < m->layer_count &&
        m->layer_kind[v->active_layer] == TILEMAP_LAYER_TERRAIN) {
        draw_flags(m, v->active_layer, g, px);
    }
    if (v->grid && ts * zoom >= 6.0f) {
        Color gc = { 255, 255, 255, 28 };
        for (int32_t gx = g.x0; gx <= g.x1; gx++) {
            DrawLineEx((Vector2){ gx * ts, g.y0 * ts }, (Vector2){ gx * ts, g.y1 * ts }, px, gc);
        }
        for (int32_t gy = g.y0; gy <= g.y1; gy++) {
            DrawLineEx((Vector2){ g.x0 * ts, gy * ts }, (Vector2){ g.x1 * ts, gy * ts }, px, gc);
        }
    }
    if (v->cursor && v->cw > 0 && v->ch > 0) {
        Color cc = v->erase_cursor ? (Color){ 235, 95, 95, 255 } : (Color){ 255, 220, 90, 255 };
        DrawRectangleLinesEx((Rectangle){ v->cx * ts, v->cy * ts, v->cw * ts, v->ch * ts }, 2 * px, cc);
    }
    rlPopMatrix();
    if (v->bounds) {
        push_board(0.0f);
        DrawRectangleLinesEx((Rectangle){ -px, -px, map_w + 2 * px, map_h + 2 * px }, 2 * px,
                             (Color){ 255, 255, 255, 90 });
        rlPopMatrix();
    }

    EndMode3D();
    memset(&g_cut, 0, sizeof(g_cut));
    rlEnableBackfaceCulling();
    rlSetClipPlanes(RL_CULL_DISTANCE_NEAR, RL_CULL_DISTANCE_FAR);
    EndTextureMode();

    DrawTexturePro(g_view_rt.texture, (Rectangle){ 0, 0, rw, -rh },
                   (Rectangle){ x, y, (float)iw, (float)ih }, (Vector2){ 0, 0 }, 0.0f, WHITE);
}

void Tilemap_shutdown(void) {
    Tilemap_reload_shaders();
    if (g_mask.id != 0) UnloadRenderTexture(g_mask);
    g_mask = (RenderTexture2D){ 0 };
    if (g_material_ok) RL_FREE(g_material.maps);
    g_material = (Material){ 0 };
    g_material_ok = false;
    if (g_marker.id != 0) UnloadTexture(g_marker);
    g_marker = (Texture2D){ 0 };
    if (g_nocut.id != 0) UnloadTexture(g_nocut);
    g_nocut = (Texture2D){ 0 };
    if (g_view_rt.id != 0) UnloadRenderTexture(g_view_rt);
    g_view_rt = (RenderTexture2D){ 0 };
    g_last.ok = false;
}

void Tilemap_draw_tileset(const Tilemap *m, int32_t slot, float x, float y, float scale,
                          int32_t hx, int32_t hy, int32_t sx, int32_t sy, int32_t sw, int32_t sh) {
    const TilemapTileset *t = Tilemap_tileset(m, slot);
    if (!t) return;
    Texture2D tex = TextureStore_get(t->texture);
    if (tex.id == 0 || t->cols <= 0) {
        DrawText(TextFormat("missing image: %s", t->image), (int32_t)x + 8, (int32_t)y + 8, 14, (Color){ 230, 120, 110, 255 });
        return;
    }
    float tw = t->tile_w * scale, th = t->tile_h * scale;

    for (int32_t r = 0; r < t->rows; r++) {
        for (int32_t c = 0; c < t->cols; c++) {
            Color bg = ((r + c) & 1) ? (Color){ 44, 44, 56, 255 } : (Color){ 52, 52, 66, 255 };
            DrawRectangleRec((Rectangle){ x + c * tw, y + r * th, tw, th }, bg);
        }
    }
    bool skinned = t->skin[0] && begin_skin(t);
    DrawTextureEx(tex, (Vector2){ x, y }, 0.0f, scale, WHITE);
    if (skinned) EndShaderMode();

    Color gc = { 0, 0, 0, 70 };
    for (int32_t c = 1; c < t->cols; c++) DrawLineV((Vector2){ x + c * tw, y }, (Vector2){ x + c * tw, y + t->rows * th }, gc);
    for (int32_t r = 1; r < t->rows; r++) DrawLineV((Vector2){ x, y + r * th }, (Vector2){ x + t->cols * tw, y + r * th }, gc);

    if (hx >= 0 && hy >= 0 && hx < t->cols && hy < t->rows) {
        DrawRectangleLinesEx((Rectangle){ x + hx * tw, y + hy * th, tw, th }, 1.0f, (Color){ 255, 255, 255, 170 });
    }
    if (sw > 0 && sh > 0) {
        DrawRectangleLinesEx((Rectangle){ x + sx * tw, y + sy * th, sw * tw, sh * th }, 2.0f, (Color){ 255, 220, 90, 255 });
    }
}
