#include "services/local_chunk/local_chunk.h"
#include <stdint.h>
#include "services/world_noise.h"
#include "services/world/world_gen.h"

#include "raylib.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

static PlanetV3 v3sub(PlanetV3 a, PlanetV3 b)   { return (PlanetV3){ a.x-b.x, a.y-b.y, a.z-b.z }; }
static PlanetV3 v3add(PlanetV3 a, PlanetV3 b)   { return (PlanetV3){ a.x+b.x, a.y+b.y, a.z+b.z }; }
static PlanetV3 v3scale(PlanetV3 a, float s)    { return (PlanetV3){ a.x*s, a.y*s, a.z*s }; }
static float    v3dot(PlanetV3 a, PlanetV3 b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
static PlanetV3 v3cross(PlanetV3 a, PlanetV3 b) {
    return (PlanetV3){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static PlanetV3 v3norm(PlanetV3 a) {
    float len = sqrtf(v3dot(a, a));
    return (len > 1e-8f) ? v3scale(a, 1.0f / len) : a;
}

static void tangent_frame(PlanetV3 center, PlanetV3 *tan, PlanetV3 *bit) {
    PlanetV3 up = (fabsf(center.y) < 0.9f) ? (PlanetV3){0,1,0} : (PlanetV3){1,0,0};
    *tan = v3norm(v3cross(up, center));
    *bit = v3cross(center, *tan);
}

static void project_local(PlanetV3 center, PlanetV3 tan, PlanetV3 bit,
                           PlanetV3 p, float *lx, float *ly) {
    PlanetV3 d = v3sub(p, v3scale(center, v3dot(p, center)));
    *lx = v3dot(d, tan);
    *ly = v3dot(d, bit);
}

static PlanetV3 unproject_local(PlanetV3 center, PlanetV3 tan, PlanetV3 bit, float lx, float ly) {
    return v3norm(v3add(v3add(center, v3scale(tan, lx)), v3scale(bit, ly)));
}

static PlanetV3 edge_midpoint(const Planet *p, int32_t cellA, int32_t cellB) {
    int32_t shared[2], n = 0;
    int32_t degA = p->degree[cellA], degB = p->degree[cellB];
    for (int32_t k = 0; k < degA && n < 2; k++) {
        int32_t corner = p->cell_corners[cellA][k];
        for (int32_t j = 0; j < degB; j++) {
            if (p->cell_corners[cellB][j] == corner) { shared[n++] = corner; break; }
        }
    }
    if (n < 2) return v3norm(v3scale(v3add(p->pos[cellA], p->pos[cellB]), 0.5f));
    return v3norm(v3scale(v3add(p->corner_pos[shared[0]], p->corner_pos[shared[1]]), 0.5f));
}

uint64_t local_chunk_cache_key(uint32_t world_seed, int32_t level, int32_t cell) {
    return ((uint64_t)world_seed << 32)
         | ((uint64_t)(level & LOCAL_CHUNK_KEY_LEVEL_MASK) << LOCAL_CHUNK_KEY_CELL_BITS)
         | (uint64_t)(cell & LOCAL_CHUNK_KEY_CELL_MASK);
}

void local_chunk_north_frame(PlanetV3 center, PlanetV3 *east, PlanetV3 *north) {
    PlanetV3 globalNorth = { 0.0f, 1.0f, 0.0f };
    PlanetV3 t = v3sub(globalNorth, v3scale(center, v3dot(globalNorth, center)));
    float len = sqrtf(v3dot(t, t));
    if (len < 1e-6f) { tangent_frame(center, east, north); return; }
    *north = v3scale(t, 1.0f / len);
    *east  = v3norm(v3cross(*north, center));
}

static float cell_spacing(const Planet *planet) {
    return 1.10715f / (float)(planet->frequency > 0 ? planet->frequency : 1);
}

static float warp_amp(const Planet *planet)  { return 0.2f * cell_spacing(planet); }
static float warp_reach(const Planet *planet) { return warp_amp(planet) * 1.7321f; }

PlanetV3 local_chunk_border_warp(const Planet *planet, uint32_t world_seed, PlanetV3 p) {
    float freq = 2.5f / cell_spacing(planet);
    float amp  = warp_amp(planet);
    PlanetV3 d = {
        world_noise_fbm(p, freq, 2, world_seed + 9100u),
        world_noise_fbm(p, freq, 2, world_seed + 9101u),
        world_noise_fbm(p, freq, 2, world_seed + 9102u),
    };
    return v3norm(v3add(p, v3scale(d, amp)));
}

static int32_t nearest_cell(const Planet *planet, int32_t hint, PlanetV3 q) {
    int32_t c = hint;
    for (int32_t iter = 0; iter < 16; iter++) {
        int32_t best = c;
        float bestDot = v3dot(q, planet->pos[c]);
        for (int32_t k = 0; k < planet->degree[c]; k++) {
            int32_t nb = planet->neighbors[c][k];
            if (nb < 0) continue;
            float d = v3dot(q, planet->pos[nb]);
            if (d > bestDot || (d == bestDot && nb < best)) { bestDot = d; best = nb; }
        }
        if (best == c) break;
        c = best;
    }
    return c;
}

int32_t local_chunk_owner_at(const Planet *planet, uint32_t world_seed, int32_t hint_cell, PlanetV3 p) {
    return nearest_cell(planet, hint_cell, local_chunk_border_warp(planet, world_seed, p));
}

static LocalChunkBlend blend_warped(const Planet *planet, int32_t hint, PlanetV3 q) {
    int32_t c = nearest_cell(planet, hint, q);
    LocalChunkBlend best = { { c, c, c }, { 1.0f, 0.0f, 0.0f } };
    float bestMin = -1e30f;

    PlanetV3 A = planet->pos[c];
    int32_t deg = planet->degree[c];
    for (int32_t k = 0; k < deg; k++) {
        int32_t nb = planet->neighbors[c][k], nc = planet->neighbors[c][(k + 1) % deg];
        if (nb < 0 || nc < 0) continue;
        PlanetV3 B = planet->pos[nb], C = planet->pos[nc];
        float det = v3dot(A, v3cross(B, C));
        if (fabsf(det) < 1e-12f) continue;
        float wa = v3dot(q, v3cross(B, C)) / det;
        float wb = v3dot(q, v3cross(C, A)) / det;
        float wc = v3dot(q, v3cross(A, B)) / det;
        float lo = fminf(wa, fminf(wb, wc));
        if (lo > bestMin) {
            bestMin = lo;
            best = (LocalChunkBlend){ { c, nb, nc }, { wa, wb, wc } };
            if (lo >= 0.0f) break;
        }
    }

    float sum = 0.0f;
    for (int32_t i = 0; i < 3; i++) { if (best.weight[i] < 0.0f) best.weight[i] = 0.0f; sum += best.weight[i]; }
    if (sum < 1e-8f) return (LocalChunkBlend){ { c, c, c }, { 1.0f, 0.0f, 0.0f } };
    for (int32_t i = 0; i < 3; i++) best.weight[i] /= sum;
    return best;
}

LocalChunkBlend local_chunk_blend_at(const Planet *planet, uint32_t world_seed, int32_t hint_cell, PlanetV3 p) {
    return blend_warped(planet, hint_cell, local_chunk_border_warp(planet, world_seed, p));
}

float local_chunk_extent(const Planet *planet, int32_t cell) {
    PlanetV3 center = planet->pos[cell];
    PlanetV3 east, north;
    local_chunk_north_frame(center, &east, &north);
    float maxDist = 0.0f;
    for (int32_t k = 0; k < planet->degree[cell]; k++) {
        float lx, ly;
        project_local(center, east, north, planet->corner_pos[planet->cell_corners[cell][k]], &lx, &ly);
        float d = sqrtf(lx * lx + ly * ly);
        if (d > maxDist) maxDist = d;
    }
    if (maxDist < 1e-6f) maxDist = 1.0f;
    return maxDist + warp_reach(planet);
}

static int32_t res_for_span(const Planet *planet, float span) {
    int32_t res = (int32_t)ceilf(span * planet_radius_m(planet) / LOCAL_CHUNK_METERS_PER_PIXEL);
    return res < 2 ? 2 : res;
}

int32_t local_chunk_res_for_cell(const Planet *planet, int32_t cell) {
    return res_for_span(planet, 2.0f * local_chunk_extent(planet, cell));
}

static PlanetV3 grid_world(const LocalChunkFields *f, int32_t gx, int32_t gy) {
    float lx = ((gx + 0.5f) / f->res * 2.0f - 1.0f) * f->extent;
    float ly = (1.0f - (gy + 0.5f) / f->res * 2.0f) * f->extent;
    return unproject_local(f->planet->pos[f->cell], f->east, f->north, lx, ly);
}

static bool local_to_grid(const LocalChunkFields *f, float lx, float ly, int32_t *gx, int32_t *gy) {
    int32_t i = (int32_t)floorf((lx / f->extent * 0.5f + 0.5f) * f->res);
    int32_t j = (int32_t)floorf((0.5f - ly / f->extent * 0.5f) * f->res);
    *gx = i; *gy = j;
    return i >= 0 && i < f->res && j >= 0 && j < f->res;
}

static bool world_to_grid(const LocalChunkFields *f, PlanetV3 p, int32_t *gx, int32_t *gy) {
    PlanetV3 center = f->planet->pos[f->cell];
    if (v3dot(p, center) <= 0.0f) return false;
    float lx, ly;
    project_local(center, f->east, f->north, p, &lx, &ly);
    return local_to_grid(f, lx, ly, gx, gy);
}

LocalChunkFields *local_chunk_create(const Planet *planet,
                                      const WorldFields *fields,
                                      uint32_t world_seed,
                                      int32_t generator_index,
                                      int32_t cell, int32_t res) {
    if (!planet || !fields || cell < 0 || cell >= fields->count || res < 2) return NULL;

    LocalChunkFields *f = (LocalChunkFields *)calloc(1, sizeof(LocalChunkFields));
    if (!f) return NULL;

    f->cell            = cell;
    f->res             = res;
    f->world_seed      = world_seed;
    f->key             = local_chunk_cache_key(world_seed, planet->level, cell);
    f->seed            = world_noise_hash3(cell, planet->level, 0, world_seed);
    f->planet          = planet;
    f->world           = *fields;
    f->generator_index = generator_index;

    f->self_elevation   = fields->elevation[cell];
    f->self_temperature = fields->temperature[cell];
    f->self_humidity    = fields->humidity[cell];
    f->self_terrain     = fields->terrain[cell];

    int32_t deg = planet->degree[cell];
    f->degree = (uint8_t)deg;
    for (int32_t k = 0; k < LOCAL_CHUNK_MAX_NEIGHBORS; k++) f->neighbors[k].cell = -1;
    for (int32_t k = 0; k < deg; k++) {
        int32_t nb = planet->neighbors[cell][k];
        LocalChunkNeighbor *n = &f->neighbors[k];
        n->cell            = nb;
        n->elevation       = fields->elevation[nb];
        n->temperature     = fields->temperature[nb];
        n->humidity        = fields->humidity[nb];
        n->terrain         = fields->terrain[nb];
        n->flows_into_me   = fields->downhill[nb] == cell;
        n->i_flow_into_it  = fields->downhill[cell] == nb;
    }

    size_t cells = (size_t)res * (size_t)res;
    f->elevation   = (float *)calloc(cells, sizeof(float));
    f->temperature = (float *)calloc(cells, sizeof(float));
    f->humidity    = (float *)calloc(cells, sizeof(float));
    f->lake        = (float *)calloc(cells, sizeof(float));
    f->terrain     = (uint8_t *)calloc(cells, sizeof(uint8_t));
    f->river_mask  = (uint8_t *)calloc(cells, sizeof(uint8_t));
    f->owner       = (uint8_t *)calloc(cells, sizeof(uint8_t));
    if (!f->elevation || !f->temperature || !f->humidity || !f->lake || !f->terrain || !f->river_mask || !f->owner) {
        local_chunk_destroy(f);
        return NULL;
    }

    return f;
}

void local_chunk_destroy(LocalChunkFields *f) {
    if (!f) return;
    free(f->elevation);
    free(f->temperature);
    free(f->humidity);
    free(f->lake);
    free(f->terrain);
    free(f->river_mask);
    free(f->owner);
    free(f);
}

static void stage_frame(LocalChunkFields *f) {
    local_chunk_north_frame(f->planet->pos[f->cell], &f->east, &f->north);
    f->extent = local_chunk_extent(f->planet, f->cell);
}

#define RESIDUAL_CACHE_CAP 32

typedef struct ResidualCache {
    int32_t n;
    int32_t cell[RESIDUAL_CACHE_CAP];
    float   value[RESIDUAL_CACHE_CAP];
} ResidualCache;

static float coarse_residual(ResidualCache *rc, const WorldGenerator *gen, const LocalChunkFields *f, int32_t cell) {
    for (int32_t i = 0; i < rc->n; i++) if (rc->cell[i] == cell) return rc->value[i];
    WorldGenSample s = gen->sample(gen, f->world.params, f->planet->pos[cell], cell);
    float r = f->world.elevation[cell] - s.elevation;
    if (rc->n < RESIDUAL_CACHE_CAP) { rc->cell[rc->n] = cell; rc->value[rc->n] = r; rc->n++; }
    return r;
}

static const float   kDetailCyclesPerCell = 3.0f;
static const float   kDetailAmpPerSpacing = 2.5f;
static const int32_t kDetailOctaves       = 4;
static const float   kClimateDetailAmp    = 0.03f;
static const float   kLakeEdgeAmp         = 0.25f;

typedef struct LocalSample { float elevation, temperature, humidity, lake; } LocalSample;

static LocalSample sample_local(const LocalChunkFields *f, const WorldGenerator *gen, ResidualCache *rc,
                                int32_t cell, PlanetV3 p, bool climate) {
    const float spacing = cell_spacing(f->planet);
    const float freq    = kDetailCyclesPerCell / spacing;
    LocalChunkBlend b = local_chunk_blend_at(f->planet, f->world_seed, cell, p);

    float residual = 0.0f, temp = 0.0f, hum = 0.0f, lake = 0.0f;
    for (int32_t i = 0; i < 3; i++) {
        if (b.weight[i] <= 0.0f) continue;
        residual += b.weight[i] * coarse_residual(rc, gen, f, b.cell[i]);
        temp     += b.weight[i] * f->world.temperature[b.cell[i]];
        hum      += b.weight[i] * f->world.humidity[b.cell[i]];
        if (f->world.terrain[b.cell[i]] == TILE_TERRAIN_LAKE) lake += b.weight[i];
    }

    LocalSample s = { 0 };
    s.elevation = gen->sample(gen, f->world.params, p, cell).elevation + residual
                + world_noise_fbm(p, freq, kDetailOctaves, f->world_seed + 9001u) * kDetailAmpPerSpacing * spacing;
    s.lake      = lake + kLakeEdgeAmp * world_noise_fbm(p, freq, 3, f->world_seed + 9004u);
    if (climate) {
        s.temperature = world_noise_clamp01(temp + kClimateDetailAmp * world_noise_fbm(p, freq, 2, f->world_seed + 9002u));
        s.humidity    = world_noise_clamp01(hum  + kClimateDetailAmp * world_noise_fbm(p, freq, 2, f->world_seed + 9003u));
    }
    return s;
}

static bool sample_is_water(const LocalChunkFields *f, LocalSample s) {
    return s.elevation < f->world.params->sea_level || s.lake > 0.5f;
}

static void stage_fields(LocalChunkFields *f) {
    const WorldGenerator *gen = world_gen_get(f->generator_index);
    ResidualCache rc = { 0 };
    int32_t res = f->res;
    for (int32_t gy = 0; gy < res; gy++) {
        for (int32_t gx = 0; gx < res; gx++) {
            LocalSample s = sample_local(f, gen, &rc, f->cell, grid_world(f, gx, gy), true);
            int32_t i = gy * res + gx;
            f->elevation[i]   = s.elevation;
            f->temperature[i] = s.temperature;
            f->humidity[i]    = s.humidity;
            f->lake[i]        = s.lake;
        }
    }
}

static void stage_ownership(LocalChunkFields *f) {
    int32_t res = f->res;
    for (int32_t gy = 0; gy < res; gy++) {
        for (int32_t gx = 0; gx < res; gx++) {
            int32_t owner = local_chunk_owner_at(f->planet, f->world_seed, f->cell, grid_world(f, gx, gy));
            uint8_t slot = LOCAL_CHUNK_OWNER_UNKNOWN;
            if (owner == f->cell) slot = LOCAL_CHUNK_OWNER_SELF;
            for (int32_t k = 0; k < f->degree && slot == LOCAL_CHUNK_OWNER_UNKNOWN; k++) {
                if (f->neighbors[k].cell == owner) slot = (uint8_t)(1 + k);
            }
            f->owner[gy * res + gx] = slot;
        }
    }
}

#define ROUTE_RES         32
#define ROUTE_NODES       (ROUTE_RES * ROUTE_RES)
#define ROUTE_HEAP_CAP    (ROUTE_NODES * 8)
#define ROUTE_MAX_STREAMS (LOCAL_CHUNK_MAX_NEIGHBORS + 3)
#define ROUTE_MAX_POINTS  1024

static const float kRouteLow          = 2.0f;
static const float kRouteNoise        = 3.0f;
static const float kRouteForeign      = 6.0f;
static const float kRouteUphill       = 25.0f;
static const float kRouteChannel      = 0.25f;
static const float kRouteCoast        = 5.0f;
static const int32_t kRouteMinSourceNodes = 4;
static const float kRouteNoiseCycles  = 6.0f;
static const float kMeanderAmp        = 0.7f;
static const float kMeanderWavelength = 2.5f;

typedef struct RouteStream {
    int32_t n;
    uint8_t level;
    float   x[ROUTE_MAX_POINTS], y[ROUTE_MAX_POINTS];
} RouteStream;

typedef struct RouteGrid {
    int32_t  cell;
    uint32_t seed;
    PlanetV3 center, east, north;
    float    extent, node_size;
    float    elev_min, elev_range;

    float    elev[ROUTE_NODES];
    float    step[ROUTE_NODES];
    uint8_t  owned[ROUTE_NODES];
    uint8_t  water[ROUTE_NODES];
    uint8_t  channel[ROUTE_NODES];

    float    dist[ROUTE_NODES];
    int32_t  prev[ROUTE_NODES];
    int32_t  path[ROUTE_NODES];
    float    heap_key[ROUTE_HEAP_CAP];
    int32_t  heap_node[ROUTE_HEAP_CAP];
    int32_t  heap_n;

    float       tmp_x[ROUTE_MAX_POINTS], tmp_y[ROUTE_MAX_POINTS];
    int32_t     stream_count;
    RouteStream streams[ROUTE_MAX_STREAMS];
} RouteGrid;

static void route_node_xy(const RouteGrid *rg, int32_t node, float *lx, float *ly) {
    int32_t ix = node % ROUTE_RES, iy = node / ROUTE_RES;
    *lx = ((ix + 0.5f) / ROUTE_RES * 2.0f - 1.0f) * rg->extent;
    *ly = (1.0f - (iy + 0.5f) / ROUTE_RES * 2.0f) * rg->extent;
}

static int32_t route_node_at(const RouteGrid *rg, float lx, float ly) {
    int32_t ix = (int32_t)floorf((lx / rg->extent * 0.5f + 0.5f) * ROUTE_RES);
    int32_t iy = (int32_t)floorf((0.5f - ly / rg->extent * 0.5f) * ROUTE_RES);
    ix = ix < 0 ? 0 : (ix >= ROUTE_RES ? ROUTE_RES - 1 : ix);
    iy = iy < 0 ? 0 : (iy >= ROUTE_RES ? ROUTE_RES - 1 : iy);
    return iy * ROUTE_RES + ix;
}

static float route_lowness(const RouteGrid *rg, int32_t node) {
    return world_noise_clamp01((rg->elev[node] - rg->elev_min) / rg->elev_range);
}

static void route_build(RouteGrid *rg, const LocalChunkFields *f, const WorldGenerator *gen, int32_t cell) {
    const Planet *planet = f->planet;
    rg->cell         = cell;
    rg->seed         = world_noise_hash3(cell, planet->level, 0, f->world_seed + 9300u);
    rg->center       = planet->pos[cell];
    local_chunk_north_frame(rg->center, &rg->east, &rg->north);
    rg->extent       = local_chunk_extent(planet, cell);
    rg->node_size    = 2.0f * rg->extent / ROUTE_RES;
    rg->stream_count = 0;
    memset(rg->channel, 0, sizeof(rg->channel));

    const float noiseFreq = kRouteNoiseCycles / cell_spacing(planet);
    ResidualCache rc = { 0 };
    float lo = 1e30f, hi = -1e30f;
    for (int32_t i = 0; i < ROUTE_NODES; i++) {
        float lx, ly;
        route_node_xy(rg, i, &lx, &ly);
        PlanetV3 p = unproject_local(rg->center, rg->east, rg->north, lx, ly);
        LocalSample s = sample_local(f, gen, &rc, cell, p, false);
        rg->elev[i]  = s.elevation;
        rg->water[i] = sample_is_water(f, s);
        rg->owned[i] = local_chunk_owner_at(planet, f->world_seed, cell, p) == cell;
        rg->step[i]  = world_noise_clamp01(0.5f + 0.5f * world_noise_fbm(p, noiseFreq, 2, f->world_seed + 9200u));
        if (!rg->water[i]) { lo = fminf(lo, s.elevation); hi = fmaxf(hi, s.elevation); }
    }
    if (lo > hi) { lo = 0.0f; hi = 1.0f; }
    rg->elev_min   = lo;
    rg->elev_range = fmaxf(hi - lo, 1e-6f);
    for (int32_t i = 0; i < ROUTE_NODES; i++) {
        bool coast = false;
        int32_t ix = i % ROUTE_RES, iy = i / ROUTE_RES;
        for (int32_t dy = -1; dy <= 1 && !coast && !rg->water[i]; dy++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                int32_t nx = ix + dx, ny = iy + dy;
                if (nx < 0 || nx >= ROUTE_RES || ny < 0 || ny >= ROUTE_RES) continue;
                if (rg->water[ny * ROUTE_RES + nx]) { coast = true; break; }
            }
        }
        rg->step[i] = 1.0f + kRouteLow * route_lowness(rg, i) + kRouteNoise * rg->step[i]
                    + (rg->owned[i] ? 0.0f : kRouteForeign) + (coast ? kRouteCoast : 0.0f);
    }
}

static void heap_push(RouteGrid *rg, float key, int32_t node) {
    if (rg->heap_n >= ROUTE_HEAP_CAP) return;
    int32_t i = rg->heap_n++;
    while (i > 0) {
        int32_t p = (i - 1) / 2;
        if (rg->heap_key[p] <= key) break;
        rg->heap_key[i] = rg->heap_key[p]; rg->heap_node[i] = rg->heap_node[p];
        i = p;
    }
    rg->heap_key[i] = key; rg->heap_node[i] = node;
}

static int32_t heap_pop(RouteGrid *rg, float *key) {
    int32_t top = rg->heap_node[0];
    *key = rg->heap_key[0];
    rg->heap_n--;
    float   k = rg->heap_key[rg->heap_n];
    int32_t n = rg->heap_node[rg->heap_n];
    int32_t i = 0;
    for (;;) {
        int32_t c = 2 * i + 1;
        if (c >= rg->heap_n) break;
        if (c + 1 < rg->heap_n && rg->heap_key[c + 1] < rg->heap_key[c]) c++;
        if (rg->heap_key[c] >= k) break;
        rg->heap_key[i] = rg->heap_key[c]; rg->heap_node[i] = rg->heap_node[c];
        i = c;
    }
    rg->heap_key[i] = k; rg->heap_node[i] = n;
    return top;
}

static int32_t route_search(RouteGrid *rg, int32_t src, int32_t dst) {
    for (int32_t i = 0; i < ROUTE_NODES; i++) { rg->dist[i] = 1e30f; rg->prev[i] = -1; }
    rg->heap_n = 0;
    rg->dist[src] = 0.0f;
    heap_push(rg, 0.0f, src);

    int32_t end = -1;
    while (rg->heap_n > 0) {
        float d;
        int32_t u = heap_pop(rg, &d);
        if (d > rg->dist[u]) continue;
        if (u != src && (u == dst || rg->channel[u] || rg->water[u])) { end = u; break; }

        int32_t ux = u % ROUTE_RES, uy = u / ROUTE_RES;
        for (int32_t dy = -1; dy <= 1; dy++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                if (dx == 0 && dy == 0) continue;
                int32_t nx = ux + dx, ny = uy + dy;
                if (nx < 0 || nx >= ROUTE_RES || ny < 0 || ny >= ROUTE_RES) continue;
                int32_t v = ny * ROUTE_RES + nx;
                float len = (dx != 0 && dy != 0) ? 1.41421f : 1.0f;
                float c = len * rg->step[v] * (rg->channel[v] ? kRouteChannel : 1.0f)
                        + kRouteUphill * fmaxf(0.0f, rg->elev[v] - rg->elev[u]) / rg->elev_range;
                if (d + c < rg->dist[v]) {
                    rg->dist[v] = d + c;
                    rg->prev[v] = u;
                    heap_push(rg, d + c, v);
                }
            }
        }
    }
    if (end < 0) return 0;

    int32_t n = 0;
    for (int32_t v = end; v >= 0 && n < ROUTE_NODES; v = rg->prev[v]) rg->path[n++] = v;
    for (int32_t i = 0; i < n / 2; i++) {
        int32_t t = rg->path[i]; rg->path[i] = rg->path[n - 1 - i]; rg->path[n - 1 - i] = t;
    }
    return n;
}

static int32_t route_pick_source(const RouteGrid *rg, int32_t salt, float maxRadius) {
    int32_t best = -1;
    float bestScore = -1e30f;
    for (int32_t i = 0; i < ROUTE_NODES; i++) {
        if (!rg->owned[i] || rg->water[i] || rg->channel[i]) continue;
        float lx, ly;
        route_node_xy(rg, i, &lx, &ly);
        if (lx * lx + ly * ly > maxRadius * maxRadius) continue;
        float score = 0.6f * route_lowness(rg, i) + world_noise_hashf(i, salt, 0, rg->seed);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}

static int32_t chaikin(float *ox, float *oy, const float *x, const float *y, int32_t n) {
    if (n < 3 || 2 * n > ROUTE_MAX_POINTS) {
        memcpy(ox, x, (size_t)n * sizeof(float));
        memcpy(oy, y, (size_t)n * sizeof(float));
        return n;
    }
    int32_t m = 0;
    ox[m] = x[0]; oy[m++] = y[0];
    for (int32_t i = 0; i < n - 1; i++) {
        ox[m] = 0.75f * x[i] + 0.25f * x[i + 1]; oy[m++] = 0.75f * y[i] + 0.25f * y[i + 1];
        ox[m] = 0.25f * x[i] + 0.75f * x[i + 1]; oy[m++] = 0.25f * y[i] + 0.75f * y[i + 1];
    }
    ox[m] = x[n - 1]; oy[m++] = y[n - 1];
    return m;
}

static int32_t resample(float *ox, float *oy, const float *x, const float *y, int32_t n, float h) {
    int32_t m = 0;
    ox[m] = x[0]; oy[m++] = y[0];
    float carry = 0.0f;
    for (int32_t i = 0; i < n - 1 && m < ROUTE_MAX_POINTS - 1; i++) {
        float dx = x[i + 1] - x[i], dy = y[i + 1] - y[i];
        float len = sqrtf(dx * dx + dy * dy);
        float t = h - carry;
        while (t < len && m < ROUTE_MAX_POINTS - 1) {
            ox[m] = x[i] + dx * t / len; oy[m++] = y[i] + dy * t / len;
            t += h;
        }
        carry = len - (t - h);
    }
    ox[m] = x[n - 1]; oy[m++] = y[n - 1];
    return m;
}

static void meander(const RouteGrid *rg, float *ox, float *oy, const float *x, const float *y,
                    int32_t n, uint32_t seed) {
    float total = 0.0f;
    for (int32_t i = 0; i < n - 1; i++) total += hypotf(x[i + 1] - x[i], y[i + 1] - y[i]);
    float taperLen = 2.0f * rg->node_size;

    ox[0] = x[0]; oy[0] = y[0];
    float s = 0.0f;
    for (int32_t i = 1; i < n - 1; i++) {
        s += hypotf(x[i] - x[i - 1], y[i] - y[i - 1]);
        float tx = x[i + 1] - x[i - 1], ty = y[i + 1] - y[i - 1];
        float len = sqrtf(tx * tx + ty * ty);
        if (len < 1e-9f) { ox[i] = x[i]; oy[i] = y[i]; continue; }
        float taper = fminf(1.0f, fminf(s, total - s) / taperLen);
        float flat  = 1.0f - route_lowness(rg, route_node_at(rg, x[i], y[i]));
        float w = world_noise_fbm((PlanetV3){ s / (kMeanderWavelength * rg->node_size), 0.0f, 0.0f }, 1.0f, 2, seed);
        float off = kMeanderAmp * rg->node_size * (0.4f + 0.6f * flat) * taper * w;
        ox[i] = x[i] - ty / len * off;
        oy[i] = y[i] + tx / len * off;
    }
    ox[n - 1] = x[n - 1]; oy[n - 1] = y[n - 1];
}

static void route_add_stream(RouteGrid *rg, int32_t n, const float *start, const float *end,
                             int32_t dst, uint8_t level) {
    if (n < 2 || rg->stream_count >= ROUTE_MAX_STREAMS) return;
    int32_t si = rg->stream_count;
    RouteStream *s = &rg->streams[si];
    const float jitter = 0.7f * rg->node_size;

    for (int32_t i = 0; i < n; i++) {
        int32_t node = rg->path[i];
        route_node_xy(rg, node, &s->x[i], &s->y[i]);
        s->x[i] += (world_noise_hashf(node, 1, 0, rg->seed) - 0.5f) * jitter;
        s->y[i] += (world_noise_hashf(node, 2, 0, rg->seed) - 0.5f) * jitter;
    }
    if (start) { s->x[0] = start[0]; s->y[0] = start[1]; }

    int32_t last = rg->path[n - 1];
    if (last == dst && end) {
        s->x[n - 1] = end[0]; s->y[n - 1] = end[1];
    } else if (rg->channel[last]) {
        const RouteStream *into = &rg->streams[rg->channel[last] - 1];
        float bestD = 1e30f;
        for (int32_t i = 0; i < into->n; i++) {
            float d = hypotf(into->x[i] - s->x[n - 1], into->y[i] - s->y[n - 1]);
            if (d < bestD) { bestD = d; s->x[n - 1] = into->x[i]; s->y[n - 1] = into->y[i]; }
        }
    }
    for (int32_t i = 0; i < n; i++) if (!rg->channel[rg->path[i]]) rg->channel[rg->path[i]] = (uint8_t)(si + 1);

    int32_t m = chaikin(rg->tmp_x, rg->tmp_y, s->x, s->y, n);
    m = chaikin(s->x, s->y, rg->tmp_x, rg->tmp_y, m);
    m = resample(rg->tmp_x, rg->tmp_y, s->x, s->y, m, 0.25f * rg->node_size);
    meander(rg, s->x, s->y, rg->tmp_x, rg->tmp_y, m, world_noise_hash3(si, 0, 0, rg->seed));
    s->n     = m;
    s->level = level;
    rg->stream_count++;
}

static void rasterize_streams(LocalChunkFields *f, const RouteGrid *rg) {
    PlanetV3 fCenter = f->planet->pos[f->cell];
    float halfPixel = f->extent / (float)f->res;
    int32_t res = f->res;
    for (int32_t si = 0; si < rg->stream_count; si++) {
        const RouteStream *s = &rg->streams[si];
        int32_t width = s->level >= 4 ? 2 : 1;
        for (int32_t i = 0; i < s->n - 1; i++) {
            float dx = s->x[i + 1] - s->x[i], dy = s->y[i + 1] - s->y[i];
            int32_t steps = (int32_t)ceilf(hypotf(dx, dy) / halfPixel) + 1;
            for (int32_t j = 0; j <= steps; j++) {
                float t = (float)j / (float)steps;
                PlanetV3 wp = unproject_local(rg->center, rg->east, rg->north, s->x[i] + dx * t, s->y[i] + dy * t);
                if (v3dot(wp, fCenter) <= 0.0f) continue;
                float lx, ly;
                project_local(fCenter, f->east, f->north, wp, &lx, &ly);
                int32_t gx, gy;
                local_to_grid(f, lx, ly, &gx, &gy);
                for (int32_t oy = 0; oy < width; oy++) {
                    for (int32_t ox = 0; ox < width; ox++) {
                        int32_t cx = gx + ox, cy = gy + oy;
                        if (cx < 0 || cx >= res || cy < 0 || cy >= res) continue;
                        uint8_t *m = &f->river_mask[cy * res + cx];
                        if (*m < s->level) *m = s->level;
                    }
                }
            }
        }
    }
}

static void route_cell(RouteGrid *rg, LocalChunkFields *f, const WorldGenerator *gen, int32_t cell) {
    const Planet *planet = f->planet;
    const WorldFields *w = &f->world;
    int32_t deg = planet->degree[cell];

    int32_t outflowK = -1;
    for (int32_t k = 0; k < deg; k++) {
        int32_t nb = planet->neighbors[cell][k];
        if (nb >= 0 && w->downhill[cell] == nb) { outflowK = k; break; }
    }
    int32_t outCell = outflowK >= 0 ? planet->neighbors[cell][outflowK] : -1;
    bool haveOutflow = outflowK >= 0 && w->river[cell] > 0;

    int32_t inflow[LOCAL_CHUNK_MAX_NEIGHBORS], nIn = 0;
    for (int32_t k = 0; k < deg; k++) {
        int32_t nb = planet->neighbors[cell][k];
        if (nb < 0 || w->downhill[nb] != cell) continue;
        if (w->river[nb] == 0) continue;
        int32_t at = nIn++;
        while (at > 0 && w->flow[planet->neighbors[cell][inflow[at - 1]]] < w->flow[nb]) {
            inflow[at] = inflow[at - 1];
            at--;
        }
        inflow[at] = k;
    }
    if (!haveOutflow && nIn == 0) return;

    route_build(rg, f, gen, cell);
    float hexRadius = rg->extent - warp_reach(planet);

    float end[2];
    int32_t dst = -1;
    if (haveOutflow) {
        project_local(rg->center, rg->east, rg->north, edge_midpoint(planet, cell, outCell), &end[0], &end[1]);
        dst = route_node_at(rg, end[0], end[1]);
    } else {
        for (int32_t i = 0; i < ROUTE_NODES; i++) {
            if (!rg->owned[i] || rg->water[i]) continue;
            if (dst < 0 || rg->elev[i] < rg->elev[dst]) dst = i;
        }
        if (dst < 0) return;
        route_node_xy(rg, dst, &end[0], &end[1]);
    }

    for (int32_t i = 0; i < nIn; i++) {
        int32_t nb = planet->neighbors[cell][inflow[i]];
        float start[2];
        project_local(rg->center, rg->east, rg->north, edge_midpoint(planet, cell, nb), &start[0], &start[1]);
        int32_t src = route_node_at(rg, start[0], start[1]);
        if (rg->water[src]) continue;
        uint8_t order = rg->stream_count == 0 ? (w->river[cell] > w->river[nb] ? w->river[cell] : w->river[nb])
                                              : w->river[nb];
        route_add_stream(rg, route_search(rg, src, dst), start, end, dst, (uint8_t)(1 + order));
    }
    if (nIn == 0) {
        int32_t src = route_pick_source(rg, 0, 0.6f * hexRadius);
        if (src >= 0) route_add_stream(rg, route_search(rg, src, dst), NULL, end, dst, (uint8_t)(1 + w->river[cell]));
    }

    if (w->river[cell] > 0) {
        int32_t extra = (int32_t)(world_noise_hash3(cell, 0, 1, rg->seed) % 3u);
        for (int32_t k = 0; k < extra; k++) {
            int32_t src = route_pick_source(rg, k + 1, hexRadius);
            if (src < 0) break;
            int32_t n = route_search(rg, src, dst);
            if (n >= kRouteMinSourceNodes) route_add_stream(rg, n, NULL, end, dst, 1);
        }
    }

    rasterize_streams(f, rg);
}

static void stage_rivers(LocalChunkFields *f) {
    RouteGrid *rg = (RouteGrid *)malloc(sizeof(RouteGrid));
    if (!rg) return;
    const WorldGenerator *gen = world_gen_get(f->generator_index);
    route_cell(rg, f, gen, f->cell);
    for (int32_t k = 0; k < f->degree; k++) {
        if (f->neighbors[k].cell >= 0) route_cell(rg, f, gen, f->neighbors[k].cell);
    }
    free(rg);
}

static void stage_biome(LocalChunkFields *f) {
    size_t cells = (size_t)f->res * (size_t)f->res;
    float sea = f->world.params->sea_level;
    for (size_t i = 0; i < cells; i++) {
        bool water = f->elevation[i] < sea;
        if (!water && f->lake[i] > 0.5f) {
            f->terrain[i] = TILE_TERRAIN_LAKE;
            water = true;
        } else {
            f->terrain[i] = (uint8_t)tile_classify(f->world.params, f->elevation[i],
                                                   f->temperature[i], f->humidity[i]);
        }
        if (water) f->river_mask[i] = 0;
    }
}

static const LocalChunkStage STAGES[] = {
    { "Frame",      stage_frame },
    { "Fields",     stage_fields },
    { "Ownership",  stage_ownership },
    { "Rivers",     stage_rivers },
    { "Biome",      stage_biome },
};
static const int32_t STAGE_COUNT = (int32_t)(sizeof(STAGES) / sizeof(STAGES[0]));

int32_t local_chunk_stage_count(void) { return STAGE_COUNT; }

const LocalChunkStage *local_chunk_stage_get(int32_t index) {
    if (index < 0 || index >= STAGE_COUNT) return NULL;
    return &STAGES[index];
}

void local_chunk_run(LocalChunkFields *f) {
    if (!f) return;
    for (int32_t i = 0; i < STAGE_COUNT; i++) STAGES[i].run(f);
}

static Color lerp_color(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return (Color){
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t),
        255,
    };
}

static Color shade_color(Color c, float s) {
    float r = c.r * s, g = c.g * s, b = c.b * s;
    return (Color){ (uint8_t)fminf(r, 255.0f), (uint8_t)fminf(g, 255.0f), (uint8_t)fminf(b, 255.0f), 255 };
}

static Color river_tint(Color base, uint8_t level) {
    static const float strength[] = { 0.0f, 0.4f, 0.7f, 0.85f, 1.0f };
    float t = level < 4 ? strength[level] : 1.0f;
    return lerp_color(base, (Color){ 60, 150, 230, 255 }, t);
}

static Color base_tile_color(const LocalChunkFields *f, int32_t i) {
    float e = f->elevation[i], sea = f->world.params->sea_level;
    uint8_t t = f->terrain[i] < TILE_TERRAIN_COUNT ? f->terrain[i] : TILE_TERRAIN_OCEAN;
    if (t == TILE_TERRAIN_LAKE) return TILE_TERRAIN_COLORS[t];
    float s = (e < sea) ? 0.7f + 0.3f * world_noise_clamp01(1.0f + (e - sea) / 0.3f)
                        : 0.8f + 0.4f * world_noise_clamp01((e - sea) / 0.7f);
    return shade_color(TILE_TERRAIN_COLORS[t], s);
}

static Color tile_color(const LocalChunkFields *f, int32_t i) {
    if (f->river_mask[i]) return river_tint(base_tile_color(f, i), f->river_mask[i]);
    return base_tile_color(f, i);
}

void local_chunk_draw_in_rect(const LocalChunkFields *f, float x, float y, float fw, float fh) {
    DrawRectangle((int32_t)x, (int32_t)y, (int32_t)fw, (int32_t)fh, (Color){ 10, 10, 16, 255 });
    if (!f || fw < 1.0f || fh < 1.0f) return;

    int32_t res = f->res;
    float cw = fw / (float)res, ch = fh / (float)res;
    BeginScissorMode((int32_t)x, (int32_t)y, (int32_t)ceilf(fw), (int32_t)ceilf(fh));
    for (int32_t gy = 0; gy < res; gy++) {
        for (int32_t gx = 0; gx < res; gx++) {
            int32_t i = gy * res + gx;
            Color c = tile_color(f, i);
            if (f->owner[i] != LOCAL_CHUNK_OWNER_SELF) c = lerp_color(c, (Color){ 10, 10, 16, 255 }, 0.45f);
            DrawRectangle((int32_t)(x + gx * cw), (int32_t)(y + gy * ch),
                          (int32_t)ceilf(cw), (int32_t)ceilf(ch), c);
        }
    }
    EndScissorMode();

    DrawText(TextFormat("#%d  %s", f->cell, tile_terrain_name(f->self_terrain)),
              (int32_t)x + 4, (int32_t)(y + fh) - 18, 12, (Color){ 230, 230, 235, 255 });
}

static const char *VIEW_NAMES[LOCAL_CHUNK_VIEW_COUNT] = { "Terrain", "Topography" };

const char *local_chunk_view_name(int32_t view) {
    return (view >= 0 && view < LOCAL_CHUNK_VIEW_COUNT) ? VIEW_NAMES[view] : "?";
}

static Color hypsometric(float t) {
    static const Color stops[] = {
        { 86, 140, 78, 255 }, { 196, 186, 120, 255 }, { 150, 108, 76, 255 }, { 236, 236, 236, 255 },
    };
    t = world_noise_clamp01(t) * 3.0f;
    int32_t i = (int32_t)t;
    if (i > 2) i = 2;
    return lerp_color(stops[i], stops[i + 1], t - (float)i);
}

static float view_elev_at(const LocalChunkGroup *g, const float *elev, int32_t gx, int32_t gy, float fallback) {
    int32_t vres = g->view_res;
    gx = gx < 0 ? 0 : (gx >= vres ? vres - 1 : gx);
    gy = gy < 0 ? 0 : (gy >= vres ? vres - 1 : gy);
    int32_t vi = gy * vres + gx;
    return g->view_tile[vi] == LOCAL_CHUNK_OWNER_UNKNOWN ? fallback : elev[vi];
}

static bool view_is_water(const LocalChunkGroup *g, const float *elev, int32_t vi, float sea) {
    const LocalChunkFields *t = g->tiles[g->view_tile[vi]];
    return elev[vi] < sea || t->terrain[g->view_index[vi]] == TILE_TERRAIN_LAKE;
}

static float view_slope(const LocalChunkGroup *g, const float *elev, int32_t gx, int32_t gy, float e) {
    return view_elev_at(g, elev, gx + 1, gy, e) - view_elev_at(g, elev, gx - 1, gy, e)
         + view_elev_at(g, elev, gx, gy + 1, e) - view_elev_at(g, elev, gx, gy - 1, e);
}

static void terrain_colors(const LocalChunkGroup *g, Color *px) {
    int32_t n = g->view_res * g->view_res;
    for (int32_t vi = 0; vi < n; vi++) {
        if (g->view_tile[vi] != LOCAL_CHUNK_OWNER_UNKNOWN) px[vi] = tile_color(g->tiles[g->view_tile[vi]], g->view_index[vi]);
    }
}

static const float   kTopoMetersPerUnit = 6000.0f;
static const float   kTopoContourM      = 20.0f;
static const int32_t kTopoIndexEvery    = 5;
static const float   kTopoHillshade     = 2.0f;

static void topography_colors(const LocalChunkGroup *g, const float *elev, Color *px) {
    int32_t vres = g->view_res;
    float sea = g->tiles[0]->world.params->sea_level;
    float lo = 1e30f, hi = -1e30f;
    for (int32_t vi = 0; vi < vres * vres; vi++) {
        if (g->view_tile[vi] == LOCAL_CHUNK_OWNER_UNKNOWN || elev[vi] < sea) continue;
        lo = fminf(lo, elev[vi]); hi = fmaxf(hi, elev[vi]);
    }
    if (lo > hi) { lo = sea; hi = sea + 1.0f; }
    float relief = fmaxf(hi - lo, 1e-4f);
    float interval = kTopoContourM / kTopoMetersPerUnit;
    float shadeK = kTopoHillshade * kTopoMetersPerUnit / (2.0f * LOCAL_CHUNK_METERS_PER_PIXEL);

    for (int32_t gy = 0; gy < vres; gy++) {
        for (int32_t gx = 0; gx < vres; gx++) {
            int32_t vi = gy * vres + gx;
            if (g->view_tile[vi] == LOCAL_CHUNK_OWNER_UNKNOWN) continue;
            const LocalChunkFields *t = g->tiles[g->view_tile[vi]];
            float e = elev[vi];
            bool water = view_is_water(g, elev, vi, sea);

            Color c;
            bool line = false, major = false;
            if (water) {
                c = lerp_color((Color){ 110, 170, 215, 255 }, (Color){ 30, 60, 120, 255 }, (sea - e) / 0.3f);
            } else {
                c = hypsometric((e - lo) / relief);
                c = shade_color(c, fminf(1.35f, fmaxf(0.55f, 1.0f + shadeK * view_slope(g, elev, gx, gy, e))));
            }

            int32_t band = (int32_t)floorf((e - sea) / interval);
            const int32_t nbs[2][2] = { { gx + 1, gy }, { gx, gy + 1 } };
            for (int32_t k = 0; k < 2; k++) {
                int32_t nx = nbs[k][0], ny = nbs[k][1];
                if (nx >= vres || ny >= vres) continue;
                int32_t ni = ny * vres + nx;
                if (g->view_tile[ni] == LOCAL_CHUNK_OWNER_UNKNOWN) continue;
                bool nWater = view_is_water(g, elev, ni, sea);
                if (nWater != water) { line = true; major = true; continue; }
                if (water) continue;
                int32_t nBand = (int32_t)floorf((elev[ni] - sea) / interval);
                if (nBand != band) {
                    line = true;
                    if ((band > nBand ? band : nBand) % kTopoIndexEvery == 0) major = true;
                }
            }
            if (line) c = lerp_color(c, (Color){ 40, 30, 20, 255 }, major ? 0.55f : 0.3f);
            uint8_t river = t->river_mask[g->view_index[vi]];
            if (river) c = river_tint(c, river);
            px[vi] = c;
        }
    }
}

static Texture2D bake_group_view(const LocalChunkGroup *g, const float *elev, int32_t view,
                                 bool tintCenter, bool seams) {
    int32_t vres = g->view_res;
    Image img = GenImageColor(vres, vres, BLANK);
    Color *px = (Color *)img.data;
    if (view == LOCAL_CHUNK_VIEW_TOPOGRAPHY) topography_colors(g, elev, px);
    else                                     terrain_colors(g, px);

    for (int32_t gy = 0; gy < vres; gy++) {
        for (int32_t gx = 0; gx < vres; gx++) {
            int32_t vi = gy * vres + gx;
            uint8_t ti = g->view_tile[vi];
            if (ti == LOCAL_CHUNK_OWNER_UNKNOWN) continue;
            if (tintCenter && ti == 0) px[vi] = lerp_color(px[vi], (Color){ 255, 245, 200, 255 }, 0.12f);
            if (seams) {
                bool edge = (gx > 0 && g->view_tile[vi - 1] != ti) || (gy > 0 && g->view_tile[vi - vres] != ti);
                if (edge) px[vi] = lerp_color(px[vi], (Color){ 0, 0, 0, 255 }, 0.35f);
            }
        }
    }
    Texture2D tex = LoadTextureFromImage(img);
    UnloadImage(img);
    return tex;
}

static void build_group_view(LocalChunkGroup *g) {
    const LocalChunkFields *center = g->tiles[0];
    const Planet *planet = center->planet;
    PlanetV3 centerPos = planet->pos[center->cell];

    float maxReach = 0.0f;
    for (int32_t i = 0; i < g->count; i++) {
        int32_t cell = g->tiles[i]->cell;
        for (int32_t k = 0; k < planet->degree[cell]; k++) {
            float lx, ly;
            project_local(centerPos, center->east, center->north,
                          planet->corner_pos[planet->cell_corners[cell][k]], &lx, &ly);
            float d = sqrtf(lx * lx + ly * ly);
            if (d > maxReach) maxReach = d;
        }
    }
    if (maxReach < 1e-6f) maxReach = 1.0f;

    g->view_span  = maxReach * 2.0f * 1.05f;
    int32_t vres  = res_for_span(planet, g->view_span);
    g->view_res   = vres;
    g->view_tile  = (uint8_t *)malloc((size_t)vres * vres);
    g->view_index = (int32_t *)malloc((size_t)vres * vres * sizeof(int32_t));
    if (!g->view_tile || !g->view_index) {
        free(g->view_tile); free(g->view_index);
        g->view_tile = NULL; g->view_index = NULL; g->view_res = 0;
        return;
    }

    float half = g->view_span * 0.5f;
    for (int32_t gy = 0; gy < vres; gy++) {
        for (int32_t gx = 0; gx < vres; gx++) {
            int32_t vi = gy * vres + gx;
            g->view_tile[vi] = LOCAL_CHUNK_OWNER_UNKNOWN;
            g->view_index[vi] = 0;

            float sx = ((gx + 0.5f) / vres * 2.0f - 1.0f) * half;
            float sy = (1.0f - (gy + 0.5f) / vres * 2.0f) * half;
            PlanetV3 worldPt = unproject_local(centerPos, center->east, center->north, sx, sy);
            int32_t owner = local_chunk_owner_at(planet, center->world_seed, center->cell, worldPt);

            int32_t hit = -1;
            for (int32_t i = 0; i < g->count; i++) if (g->tiles[i]->cell == owner) { hit = i; break; }
            if (hit < 0) continue;
            if (local_chunk_owner_at(planet, center->world_seed, owner, worldPt) != owner) g->owner_mismatches++;

            const LocalChunkFields *t = g->tiles[hit];
            int32_t ti, tj;
            if (!world_to_grid(t, worldPt, &ti, &tj)) { g->owner_mismatches++; continue; }
            g->view_tile[vi]  = (uint8_t)hit;
            g->view_index[vi] = tj * t->res + ti;
        }
    }

    float sea = center->world.params->sea_level;
    float *elev = (float *)malloc((size_t)vres * vres * sizeof(float));
    if (!elev) return;
    for (int32_t vi = 0; vi < vres * vres; vi++) {
        uint8_t ti = g->view_tile[vi];
        elev[vi] = ti == LOCAL_CHUNK_OWNER_UNKNOWN ? sea : g->tiles[ti]->elevation[g->view_index[vi]];
    }
    for (int32_t v = 0; v < LOCAL_CHUNK_VIEW_COUNT; v++) {
        g->view_connected[v] = bake_group_view(g, elev, v, false, true);
        g->view_tinted[v]    = bake_group_view(g, elev, v, true, false);
    }
    free(elev);
}

LocalChunkGroup local_chunk_create_group(const Planet *planet,
                                          const WorldFields *fields,
                                          uint32_t world_seed,
                                          int32_t generator_index,
                                          int32_t cell) {
    LocalChunkGroup g;
    memset(&g, 0, sizeof(g));
    if (!planet || cell < 0 || cell >= planet->cell_count) return g;

    LocalChunkFields *center = local_chunk_create(planet, fields, world_seed, generator_index, cell,
                                                  local_chunk_res_for_cell(planet, cell));
    if (!center) return g;
    local_chunk_run(center);
    g.tiles[g.count++] = center;

    for (int32_t k = 0; k < center->degree; k++) {
        int32_t nb = center->neighbors[k].cell;
        if (nb < 0) continue;
        LocalChunkFields *t = local_chunk_create(planet, fields, world_seed, generator_index, nb,
                                                 local_chunk_res_for_cell(planet, nb));
        if (!t) continue;
        local_chunk_run(t);
        g.tiles[g.count++] = t;
    }

    build_group_view(&g);
    return g;
}

void local_chunk_destroy_group(LocalChunkGroup *g) {
    if (!g) return;
    for (int32_t i = 0; i < g->count; i++) local_chunk_destroy(g->tiles[i]);
    free(g->view_tile);
    free(g->view_index);
    for (int32_t v = 0; v < LOCAL_CHUNK_VIEW_COUNT; v++) {
        if (g->view_connected[v].id) UnloadTexture(g->view_connected[v]);
        if (g->view_tinted[v].id)    UnloadTexture(g->view_tinted[v]);
    }
    memset(g, 0, sizeof(*g));
}

static void draw_group_view(const LocalChunkGroup *g, float x, float y, float fw, float fh, Texture2D tex) {
    DrawRectangle((int32_t)x, (int32_t)y, (int32_t)fw, (int32_t)fh, (Color){ 10, 10, 16, 255 });
    if (!g || g->count == 0 || !g->tiles[0] || g->view_res == 0 || !tex.id || fw < 1.0f || fh < 1.0f) return;

    float side = fminf(fw, fh);
    float ox = x + (fw - side) * 0.5f, oy = y + (fh - side) * 0.5f;
    DrawTexturePro(tex, (Rectangle){ 0, 0, (float)tex.width, (float)tex.height },
                   (Rectangle){ ox, oy, side, side }, (Vector2){ 0, 0 }, 0.0f, WHITE);

    const LocalChunkFields *center = g->tiles[0];
    PlanetV3 centerPos = center->planet->pos[center->cell];
    float half = g->view_span * 0.5f;
    for (int32_t i = 0; i < g->count; i++) {
        float lx, ly;
        project_local(centerPos, center->east, center->north, center->planet->pos[g->tiles[i]->cell], &lx, &ly);
        float px = ox + side * 0.5f + lx / half * (side * 0.5f);
        float py = oy + side * 0.5f - ly / half * (side * 0.5f);
        DrawText(TextFormat("#%d", g->tiles[i]->cell), (int32_t)px - 14, (int32_t)py - 6, 12,
                  (Color){ 255, 255, 255, i == 0 ? 255 : 200 });
    }
}

static Texture2D group_view_tex(const Texture2D *views, int32_t view) {
    return (view >= 0 && view < LOCAL_CHUNK_VIEW_COUNT) ? views[view] : (Texture2D){ 0 };
}

void local_chunk_draw_group_in_rect(const LocalChunkGroup *g, int32_t view, float x, float y, float fw, float fh) {
    draw_group_view(g, x, y, fw, fh, g ? group_view_tex(g->view_tinted, view) : (Texture2D){ 0 });
}

void local_chunk_draw_connected_in_rect(const LocalChunkGroup *g, int32_t view, float x, float y, float fw, float fh) {
    draw_group_view(g, x, y, fw, fh, g ? group_view_tex(g->view_connected, view) : (Texture2D){ 0 });
}
