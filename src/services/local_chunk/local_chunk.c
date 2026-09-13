#include "services/local_chunk/local_chunk.h"
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

static PlanetV3 edge_midpoint(const Planet *p, int cellA, int cellB) {
    int shared[2], n = 0;
    int degA = p->degree[cellA], degB = p->degree[cellB];
    for (int k = 0; k < degA && n < 2; k++) {
        int corner = p->cell_corners[cellA][k];
        for (int j = 0; j < degB; j++) {
            if (p->cell_corners[cellB][j] == corner) { shared[n++] = corner; break; }
        }
    }
    if (n < 2) return v3norm(v3scale(v3add(p->pos[cellA], p->pos[cellB]), 0.5f));
    return v3norm(v3scale(v3add(p->corner_pos[shared[0]], p->corner_pos[shared[1]]), 0.5f));
}

int local_chunk_stage_count(void);
const LocalChunkStage *local_chunk_stage_get(int index);

unsigned int local_chunk_cache_key(unsigned int world_seed, int cell) {
    return world_noise_hash3(cell, 0, 0, world_seed);
}

void local_chunk_north_frame(PlanetV3 center, PlanetV3 *east, PlanetV3 *north) {
    PlanetV3 globalNorth = { 0.0f, 1.0f, 0.0f };
    PlanetV3 t = v3sub(globalNorth, v3scale(center, v3dot(globalNorth, center)));
    float len = sqrtf(v3dot(t, t));
    if (len < 1e-6f) { tangent_frame(center, east, north); return; }
    *north = v3scale(t, 1.0f / len);
    *east  = v3norm(v3cross(*north, center));
}

static void pick_north_south_slot(const Planet *planet, int cell, int *outN, int *outS) {
    PlanetV3 center = planet->pos[cell];
    PlanetV3 east, north;
    local_chunk_north_frame(center, &east, &north);

    int deg = planet->degree[cell];
    int bestN = -1; float bestNdot = -2.0f;
    int bestS = -1; float bestSdot = -2.0f;
    for (int k = 0; k < deg; k++) {
        int nb = planet->neighbors[cell][k];
        if (nb < 0) continue;
        PlanetV3 mid = edge_midpoint(planet, cell, nb);
        float lx, ly;
        project_local(center, east, north, mid, &lx, &ly);
        float len = sqrtf(lx * lx + ly * ly);
        float ndot = (len > 1e-6f) ? (ly / len) : 0.0f;
        if (ndot > bestNdot) { bestNdot = ndot; bestN = k; }
        if (-ndot > bestSdot) { bestSdot = -ndot; bestS = k; }
    }
    if (bestN >= 0 && bestN == bestS) bestS = -1;
    *outN = bestN;
    *outS = bestS;
}

LocalChunkEdgeRole local_chunk_edge_role(const Planet *planet, int cell, int neighborSlot) {
    int deg = planet->degree[cell];
    if (neighborSlot < 0 || neighborSlot >= deg) return LOCAL_CHUNK_EDGE_CLEAN;
    int nb = planet->neighbors[cell][neighborSlot];
    if (nb < 0) return LOCAL_CHUNK_EDGE_CLEAN;

    int aN, aS;
    pick_north_south_slot(planet, cell, &aN, &aS);
    LocalChunkEdgeRole roleFromA;
    if      (neighborSlot == aN) roleFromA = LOCAL_CHUNK_EDGE_NORTH;
    else if (neighborSlot == aS) roleFromA = LOCAL_CHUNK_EDGE_SOUTH;
    else                         return LOCAL_CHUNK_EDGE_CLEAN;

    int degB = planet->degree[nb];
    int slotOfCellInB = -1;
    for (int k = 0; k < degB; k++) {
        if (planet->neighbors[nb][k] == cell) { slotOfCellInB = k; break; }
    }
    if (slotOfCellInB < 0) return LOCAL_CHUNK_EDGE_CLEAN;

    int bN, bS;
    pick_north_south_slot(planet, nb, &bN, &bS);
    bool bAgrees = (slotOfCellInB == bN) || (slotOfCellInB == bS);
    return bAgrees ? roleFromA : LOCAL_CHUNK_EDGE_CLEAN;
}

LocalChunkCrossing local_chunk_cross_border(const Planet *planet, int cell, int res,
                                             int col, int row, int neighborSlot) {
    LocalChunkCrossing out = { 0, 0, 0, false };
    int deg = planet->degree[cell];
    if (neighborSlot < 0 || neighborSlot >= deg) return out;
    int nb = planet->neighbors[cell][neighborSlot];
    if (nb < 0 || col < 0 || col >= res || row < 0 || row >= res) return out;

    out.cell = nb;
    out.row  = row;

    LocalChunkEdgeRole role = local_chunk_edge_role(planet, cell, neighborSlot);
    if (role == LOCAL_CHUNK_EDGE_CLEAN) {
        out.col = col;
        out.ok  = true;
        return out;
    }

    float phaseSelf = (cell < nb) ? 0.0f : 0.5f;
    float phaseNb   = (cell < nb) ? 0.5f : 0.0f;
    int destCol = (int)floorf((float)col + (phaseSelf - phaseNb));
    if (destCol < 0) destCol = 0;
    if (destCol >= res) destCol = res - 1;
    out.col = destCol;
    out.ok  = true;
    return out;
}

int local_chunk_debug_check_reciprocity(const Planet *planet, int *out_mismatches) {
    int total = 0, mismatches = 0;
    for (int cell = 0; cell < planet->cell_count; cell++) {
        int deg = planet->degree[cell];
        for (int slot = 0; slot < deg; slot++) {
            int nb = planet->neighbors[cell][slot];
            if (nb <= cell) continue;

            int aN, aS;
            pick_north_south_slot(planet, cell, &aN, &aS);
            bool aCandidate = (slot == aN || slot == aS);

            int degB = planet->degree[nb];
            int slotInNb = -1;
            for (int k = 0; k < degB; k++) {
                if (planet->neighbors[nb][k] == cell) { slotInNb = k; break; }
            }
            int bN, bS;
            pick_north_south_slot(planet, nb, &bN, &bS);
            bool bCandidate = slotInNb >= 0 && (slotInNb == bN || slotInNb == bS);

            if (!aCandidate && !bCandidate) continue;

            total++;
            if (local_chunk_edge_role(planet, cell, slot) == LOCAL_CHUNK_EDGE_CLEAN) mismatches++;
        }
    }
    if (out_mismatches) *out_mismatches = mismatches;
    return total;
}

LocalChunkFields *local_chunk_create(const Planet *planet,
                                      const WorldFields *fields,
                                      unsigned int world_seed,
                                      int generator_index,
                                      int cell, int res) {
    if (!planet || !fields || cell < 0 || cell >= fields->count || res < 2) return NULL;

    LocalChunkFields *f = (LocalChunkFields *)calloc(1, sizeof(LocalChunkFields));
    if (!f) return NULL;

    f->cell            = cell;
    f->res             = res;
    f->world_seed      = world_seed;
    f->seed            = local_chunk_cache_key(world_seed, cell);
    f->planet          = planet;
    f->world           = *fields;
    f->generator_index = generator_index;

    f->self_elevation   = fields->elevation[cell];
    f->self_temperature = fields->temperature[cell];
    f->self_humidity    = fields->humidity[cell];
    f->self_terrain     = fields->terrain[cell];

    int deg = planet->degree[cell];
    f->degree = deg;
    for (int k = 0; k < LOCAL_CHUNK_MAX_NEIGHBORS; k++) f->neighbors[k].cell = -1;
    for (int k = 0; k < deg; k++) {
        int nb = planet->neighbors[cell][k];
        LocalChunkNeighbor *n = &f->neighbors[k];
        n->cell            = nb;
        n->elevation       = fields->elevation[nb];
        n->temperature      = fields->temperature[nb];
        n->humidity        = fields->humidity[nb];
        n->terrain         = fields->terrain[nb];
        n->flows_into_me   = fields->downhill[nb] == cell;
        n->i_flow_into_it  = fields->downhill[cell] == nb;
    }

    size_t cells = (size_t)res * (size_t)res;
    f->elevation  = (float *)calloc(cells, sizeof(float));
    f->river_mask = (unsigned char *)calloc(cells, sizeof(unsigned char));
    if (!f->elevation || !f->river_mask) { local_chunk_destroy(f); return NULL; }

    return f;
}

void local_chunk_destroy(LocalChunkFields *f) {
    if (!f) return;
    free(f->elevation);
    free(f->river_mask);
    free(f);
}

static void stage_elevation(LocalChunkFields *f) {
    PlanetV3 center = f->planet->pos[f->cell];
    PlanetV3 tan, bit;
    tangent_frame(center, &tan, &bit);

    float maxDist = 0.0f;
    for (int k = 0; k < f->degree; k++) {
        int nb = f->neighbors[k].cell;
        if (nb < 0) continue;
        PlanetV3 mid = edge_midpoint(f->planet, f->cell, nb);
        float lx, ly;
        project_local(center, tan, bit, mid, &lx, &ly);
        float d = sqrtf(lx * lx + ly * ly);
        if (d > maxDist) maxDist = d;
    }
    if (maxDist < 1e-6f) maxDist = 1.0f;
    float extent = maxDist * 1.15f;
    f->extent = extent;

    const unsigned int detailSeed = f->world_seed + 9001u;
    const float kDetailFreq = 55.0f, kDetailAmp = 0.20f;
    const int   kDetailOctaves = 4;

    const WorldGenerator *gen = world_gen_get(f->generator_index);
    int res = f->res;
    for (int gy = 0; gy < res; gy++) {
        for (int gx = 0; gx < res; gx++) {
            float x = ((gx + 0.5f) / res * 2.0f - 1.0f) * extent;
            float y = ((gy + 0.5f) / res * 2.0f - 1.0f) * extent;
            PlanetV3 worldPt = v3norm(v3add(v3add(center, v3scale(tan, x)), v3scale(bit, y)));
            WorldGenSample s = gen->sample(gen, f->world.params, worldPt, f->cell);
            float detail = world_noise_fbm(worldPt, kDetailFreq, kDetailOctaves, detailSeed) * kDetailAmp;
            f->elevation[gy * res + gx] = s.elevation + detail;
        }
    }
}

static void rasterize_river_path(LocalChunkFields *f, float ax, float ay, float bx, float by,
                                  float extent, unsigned int pathSeed) {
    int res = f->res;
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    float amp  = extent * 0.08f * (0.6f + world_noise_hashf(0, 0, 0, pathSeed));
    float freq = 2.0f + world_noise_hashf(1, 0, 0, pathSeed) * 2.0f;

    float px = (len > 1e-6f) ? -dy / len : 0.0f;
    float py = (len > 1e-6f) ?  dx / len : 0.0f;

    int steps = res * 2;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float x = ax + dx * t, y = ay + dy * t;
        float envelope = sinf((float)M_PI * t);
        float meander = world_noise_fbm((PlanetV3){ x * freq, y * freq, 0.0f }, 1.0f, 3, pathSeed);
        float wiggle = amp * envelope * meander;
        x += px * wiggle; y += py * wiggle;

        int gx = (int)((x / extent * 0.5f + 0.5f) * res);
        int gy = (int)((y / extent * 0.5f + 0.5f) * res);
        for (int oy = -1; oy <= 1; oy++) {
            for (int ox = -1; ox <= 1; ox++) {
                int cx = gx + ox, cy = gy + oy;
                if (cx < 0 || cx >= res || cy < 0 || cy >= res) continue;
                f->river_mask[cy * res + cx] = 1;
            }
        }
    }
}

static void stage_rivers(LocalChunkFields *f) {
    PlanetV3 center = f->planet->pos[f->cell];
    PlanetV3 tan, bit;
    tangent_frame(center, &tan, &bit);

    float extent = f->extent;
    if (extent < 1e-6f) return;

    float lxs[LOCAL_CHUNK_MAX_NEIGHBORS], lys[LOCAL_CHUNK_MAX_NEIGHBORS];
    for (int k = 0; k < f->degree; k++) {
        int nb = f->neighbors[k].cell;
        if (nb < 0) continue;
        PlanetV3 mid = edge_midpoint(f->planet, f->cell, nb);
        project_local(center, tan, bit, mid, &lxs[k], &lys[k]);
    }

    int outflowK = -1;
    for (int k = 0; k < f->degree; k++) {
        if (f->neighbors[k].cell >= 0 && f->neighbors[k].i_flow_into_it) { outflowK = k; break; }
    }
    bool haveOutflow = outflowK >= 0 && (f->world.river[f->cell] > 0 || f->world.river[f->neighbors[outflowK].cell] > 0);
    float termX = 0.0f, termY = 0.0f;
    if (haveOutflow) { termX = lxs[outflowK]; termY = lys[outflowK]; }

    bool anyInflow = false;
    for (int k = 0; k < f->degree; k++) {
        LocalChunkNeighbor *n = &f->neighbors[k];
        if (n->cell < 0 || !n->flows_into_me) continue;
        if (f->world.river[f->cell] == 0 && f->world.river[n->cell] == 0) continue;

        unsigned int pathSeed = world_noise_hash3(f->cell, n->cell,
                                                    haveOutflow ? f->neighbors[outflowK].cell : -1,
                                                    f->world_seed);
        rasterize_river_path(f, lxs[k], lys[k], termX, termY, extent, pathSeed);
        anyInflow = true;
    }

    if (haveOutflow && !anyInflow) {
        unsigned int pathSeed = world_noise_hash3(f->cell, f->neighbors[outflowK].cell, -2, f->world_seed);
        rasterize_river_path(f, 0.0f, 0.0f, termX, termY, extent, pathSeed);
    }
}

static void stage_biome_context(LocalChunkFields *f) {
    (void)f;
}

static const LocalChunkStage STAGES[] = {
    { "Elevation",      stage_elevation },
    { "Rivers",         stage_rivers },
    { "Biome context",  stage_biome_context },
};
static const int STAGE_COUNT = (int)(sizeof(STAGES) / sizeof(STAGES[0]));

int local_chunk_stage_count(void) { return STAGE_COUNT; }

const LocalChunkStage *local_chunk_stage_get(int index) {
    if (index < 0 || index >= STAGE_COUNT) return NULL;
    return &STAGES[index];
}

void local_chunk_run(LocalChunkFields *f) {
    if (!f) return;
    for (int i = 0; i < STAGE_COUNT; i++) STAGES[i].run(f);
}

static Color lerp_color(Color a, Color b, float t) {
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return (Color){
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t),
        255,
    };
}

static Color elevation_color(float e, float sea) {
    if (e < sea - 0.16f) return (Color){ 20, 40, 90, 255 };
    if (e < sea)         return lerp_color((Color){20,40,90,255}, (Color){60,120,190,255},
                                            (e - (sea - 0.16f)) / 0.16f);
    float t = world_noise_clamp01((e - sea) / 0.7f);
    if (t < 0.5f) return lerp_color((Color){40,110,55,255}, (Color){150,140,80,255}, t / 0.5f);
    return lerp_color((Color){150,140,80,255}, (Color){235,235,240,255}, (t - 0.5f) / 0.5f);
}

static void draw_tile_cells(const LocalChunkFields *f, float x, float y, float fw, float fh,
                             float *out_lo, float *out_hi) {
    int res = f->res;
    float cw = fw / (float)res, ch = fh / (float)res;
    float sea = f->world.params->sea_level;

    float lo = f->elevation[0], hi = f->elevation[0];
    BeginScissorMode((int)x, (int)y, (int)ceilf(fw), (int)ceilf(fh));
    for (int gy = 0; gy < res; gy++) {
        for (int gx = 0; gx < res; gx++) {
            int i = gy * res + gx;
            if (f->elevation[i] < lo) lo = f->elevation[i];
            if (f->elevation[i] > hi) hi = f->elevation[i];
            Color c = f->river_mask[i] ? (Color){ 60, 150, 230, 255 }
                                        : elevation_color(f->elevation[i], sea);
            DrawRectangle((int)(x + gx * cw), (int)(y + gy * ch),
                          (int)ceilf(cw), (int)ceilf(ch), c);
        }
    }
    EndScissorMode();
    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
}

void local_chunk_draw_in_rect(const LocalChunkFields *f, float x, float y, float fw, float fh) {
    DrawRectangle((int)x, (int)y, (int)fw, (int)fh, (Color){ 10, 10, 16, 255 });
    if (!f || fw < 1.0f || fh < 1.0f) return;

    float lo, hi;
    draw_tile_cells(f, x, y, fw, fh, &lo, &hi);

    DrawText(TextFormat("elev %+.2f .. %+.2f  (sea %+.2f)", lo, hi, f->world.params->sea_level),
              (int)x + 4, (int)(y + fh) - 18, 12, (Color){ 230, 230, 235, 255 });
}

LocalChunkGroup local_chunk_create_group(const Planet *planet,
                                          const WorldFields *fields,
                                          unsigned int world_seed,
                                          int generator_index,
                                          int cell, int res) {
    LocalChunkGroup g;
    memset(&g, 0, sizeof(g));

    LocalChunkFields *center = local_chunk_create(planet, fields, world_seed, generator_index, cell, res);
    if (!center) return g;
    local_chunk_run(center);
    g.tiles[g.count++] = center;

    for (int k = 0; k < center->degree; k++) {
        int nb = center->neighbors[k].cell;
        if (nb < 0) continue;
        LocalChunkFields *t = local_chunk_create(planet, fields, world_seed, generator_index, nb, res);
        if (!t) continue;
        local_chunk_run(t);
        g.tiles[g.count++] = t;
    }
    return g;
}

void local_chunk_destroy_group(LocalChunkGroup *g) {
    if (!g) return;
    for (int i = 0; i < g->count; i++) local_chunk_destroy(g->tiles[i]);
    memset(g, 0, sizeof(*g));
}

typedef struct { float x, y; } LC2;

static bool point_in_poly(const LC2 *poly, int n, float px, float py) {
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;
        if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

#define LOCAL_CHUNK_GROUP_SUPER_RES 140

void local_chunk_draw_group_in_rect(const LocalChunkGroup *g, float x, float y, float fw, float fh) {
    DrawRectangle((int)x, (int)y, (int)fw, (int)fh, (Color){ 10, 10, 16, 255 });
    if (!g || g->count == 0 || !g->tiles[0] || fw < 1.0f || fh < 1.0f) return;

    const LocalChunkFields *center = g->tiles[0];
    const Planet *planet = center->planet;
    PlanetV3 centerPos = planet->pos[center->cell];
    PlanetV3 sTan, sBit;
    tangent_frame(centerPos, &sTan, &sBit);

    LC2 poly[1 + LOCAL_CHUNK_MAX_NEIGHBORS][LOCAL_CHUNK_MAX_NEIGHBORS];
    int polyN[1 + LOCAL_CHUNK_MAX_NEIGHBORS];
    LC2 center2[1 + LOCAL_CHUNK_MAX_NEIGHBORS];
    float maxReach = 0.0f;

    for (int i = 0; i < g->count; i++) {
        const LocalChunkFields *t = g->tiles[i];
        int deg = planet->degree[t->cell];
        polyN[i] = deg;
        project_local(centerPos, sTan, sBit, planet->pos[t->cell], &center2[i].x, &center2[i].y);
        for (int k = 0; k < deg; k++) {
            PlanetV3 corner = planet->corner_pos[planet->cell_corners[t->cell][k]];
            project_local(centerPos, sTan, sBit, corner, &poly[i][k].x, &poly[i][k].y);
            float d = sqrtf(poly[i][k].x * poly[i][k].x + poly[i][k].y * poly[i][k].y);
            if (d > maxReach) maxReach = d;
        }
    }
    if (maxReach < 1e-6f) maxReach = 1.0f;
    float span = maxReach * 2.0f * 1.05f;

    int super = LOCAL_CHUNK_GROUP_SUPER_RES;
    float cw = fw / (float)super, ch = fh / (float)super;

    for (int gy = 0; gy < super; gy++) {
        for (int gx = 0; gx < super; gx++) {
            float sx = ((gx + 0.5f) / super * 2.0f - 1.0f) * (span * 0.5f);
            float sy = ((gy + 0.5f) / super * 2.0f - 1.0f) * (span * 0.5f);
            float shared_y = -sy;

            int hit = -1;
            for (int i = 0; i < g->count; i++) {
                if (point_in_poly(poly[i], polyN[i], sx, shared_y)) { hit = i; break; }
            }
            if (hit < 0) {
                float best = 1e30f;
                for (int i = 0; i < g->count; i++) {
                    float dx = sx - center2[i].x, dy = shared_y - center2[i].y;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < best) { best = d2; hit = i; }
                }
            }

            const LocalChunkFields *t = g->tiles[hit];
            PlanetV3 tCenter = planet->pos[t->cell];
            PlanetV3 tTan, tBit;
            tangent_frame(tCenter, &tTan, &tBit);
            PlanetV3 worldPt = v3norm(v3add(v3add(centerPos, v3scale(sTan, sx)), v3scale(sBit, shared_y)));
            float lx, ly;
            project_local(tCenter, tTan, tBit, worldPt, &lx, &ly);

            int res = t->res;
            int ti = (int)((lx / t->extent * 0.5f + 0.5f) * res);
            int tj = (int)((ly / t->extent * 0.5f + 0.5f) * res);
            if (ti < 0) ti = 0; if (ti >= res) ti = res - 1;
            if (tj < 0) tj = 0; if (tj >= res) tj = res - 1;
            int idx = tj * res + ti;

            Color c = t->river_mask[idx] ? (Color){ 60, 150, 230, 255 }
                                          : elevation_color(t->elevation[idx], t->world.params->sea_level);
            if (hit == 0) c = lerp_color(c, (Color){ 255, 245, 200, 255 }, 0.12f);
            DrawRectangle((int)(x + gx * cw), (int)(y + gy * ch), (int)ceilf(cw), (int)ceilf(ch), c);
        }
    }

    for (int i = 0; i < g->count; i++) {
        float px = x + fw * 0.5f + center2[i].x / (span * 0.5f) * (fw * 0.5f);
        float py = y + fh * 0.5f - center2[i].y / (span * 0.5f) * (fh * 0.5f);
        DrawText(TextFormat("#%d", g->tiles[i]->cell), (int)px - 14, (int)py - 6, 12,
                  (Color){ 255, 255, 255, 220 });
    }
}

void local_chunk_draw_square_in_rect(const LocalChunkFields *f, float x, float y, float fw, float fh) {
    DrawRectangle((int)x, (int)y, (int)fw, (int)fh, (Color){ 10, 10, 16, 255 });
    if (!f || fw < 1.0f || fh < 1.0f) return;

    const Planet *planet = f->planet;
    int cell = f->cell;
    PlanetV3 center = planet->pos[cell];
    PlanetV3 east, north;
    local_chunk_north_frame(center, &east, &north);

    PlanetV3 gridTan, gridBit;
    tangent_frame(center, &gridTan, &gridBit);
    float extent = f->extent;
    int res = f->res;

    int northSlot, southSlot;
    pick_north_south_slot(planet, cell, &northSlot, &southSlot);
    LocalChunkEdgeRole northRole = (northSlot >= 0) ? local_chunk_edge_role(planet, cell, northSlot)
                                                     : LOCAL_CHUNK_EDGE_CLEAN;
    LocalChunkEdgeRole southRole = (southSlot >= 0) ? local_chunk_edge_role(planet, cell, southSlot)
                                                     : LOCAL_CHUNK_EDGE_CLEAN;

    float gridH = fh - 30.0f;
    if (gridH < 1.0f) gridH = fh;
    float cw = fw / (float)res, ch = gridH / (float)res;
    const float edgeBand = 0.15f;

    for (int gy = 0; gy < res; gy++) {
        for (int gx = 0; gx < res; gx++) {
            float u = (gx + 0.5f) / res * 2.0f - 1.0f;
            float v = (gy + 0.5f) / res * 2.0f - 1.0f;
            float dx = u * extent;
            float dyNorth = -v * extent;

            PlanetV3 worldPt = v3norm(v3add(v3add(center, v3scale(east, dx)), v3scale(north, dyNorth)));
            float lx, ly;
            project_local(center, gridTan, gridBit, worldPt, &lx, &ly);
            int gi = (int)((lx / extent * 0.5f + 0.5f) * res);
            int gj = (int)((ly / extent * 0.5f + 0.5f) * res);
            if (gi < 0) gi = 0; if (gi >= res) gi = res - 1;
            if (gj < 0) gj = 0; if (gj >= res) gj = res - 1;
            int idx = gj * res + gi;

            Color c = f->river_mask[idx] ? (Color){ 60, 150, 230, 255 }
                                          : elevation_color(f->elevation[idx], f->world.params->sea_level);

            if (v < -1.0f + edgeBand && northRole == LOCAL_CHUNK_EDGE_NORTH)
                c = lerp_color(c, (Color){ 255, 200, 80, 255 }, 0.25f);
            if (v > 1.0f - edgeBand && southRole == LOCAL_CHUNK_EDGE_SOUTH)
                c = lerp_color(c, (Color){ 255, 130, 80, 255 }, 0.25f);

            DrawRectangle((int)(x + gx * cw), (int)(y + gy * ch), (int)ceilf(cw), (int)ceilf(ch), c);
        }
    }

    Color gridLine = (Color){ 0, 0, 0, 60 };
    for (int i = 0; i <= res; i++) {
        DrawLine((int)(x + i * cw), (int)y, (int)(x + i * cw), (int)(y + gridH), gridLine);
        DrawLine((int)x, (int)(y + i * ch), (int)(x + fw), (int)(y + i * ch), gridLine);
    }

    int labelY = (int)(y + gridH) + 2;
    if (northRole == LOCAL_CHUNK_EDGE_NORTH) {
        int sampleCol = res / 2;
        LocalChunkCrossing cr = local_chunk_cross_border(planet, cell, res, sampleCol, 0, northSlot);
        DrawRectangle((int)(x + sampleCol * cw), (int)y, (int)ceilf(cw), (int)ceilf(ch),
                      (Color){ 255, 220, 60, 255 });
        DrawText(TextFormat("N col %d -> #%d col %d", sampleCol, cr.cell, cr.col),
                  (int)x + 2, labelY, 11, (Color){ 255, 220, 60, 255 });
        labelY += 14;
    }
    if (southRole == LOCAL_CHUNK_EDGE_SOUTH) {
        int sampleCol = res / 2;
        LocalChunkCrossing cr = local_chunk_cross_border(planet, cell, res, sampleCol, res - 1, southSlot);
        DrawRectangle((int)(x + sampleCol * cw), (int)(y + gridH - ch), (int)ceilf(cw), (int)ceilf(ch),
                      (Color){ 255, 140, 60, 255 });
        DrawText(TextFormat("S col %d -> #%d col %d", sampleCol, cr.cell, cr.col),
                  (int)x + 2, labelY, 11, (Color){ 255, 140, 60, 255 });
    }
}

void local_chunk_draw_connected_in_rect(const LocalChunkGroup *g, float x, float y, float fw, float fh) {
    DrawRectangle((int)x, (int)y, (int)fw, (int)fh, (Color){ 10, 10, 16, 255 });
    if (!g || g->count == 0 || !g->tiles[0] || fw < 1.0f || fh < 1.0f) return;

    const LocalChunkFields *center = g->tiles[0];
    const Planet *planet = center->planet;
    int centerCell = center->cell;
    PlanetV3 centerPos = planet->pos[centerCell];
    PlanetV3 east, north;
    local_chunk_north_frame(centerPos, &east, &north);

    static const float offX[6] = {  0.0f, +1.0f, +1.0f,  0.0f, -1.0f, -1.0f };
    static const float offY[6] = { -1.0f, -0.5f, +0.5f, +1.0f, +0.5f, -0.5f };

    float squareSize = fminf(fw, fh) / 3.0f;
    float cx = x + fw * 0.5f, cy = y + fh * 0.5f;

    #define PXROUND(v) ((int)((v) + 0.5f))

    int cLeft = PXROUND(cx - squareSize * 0.5f), cTop = PXROUND(cy - squareSize * 0.5f);
    int cRight = PXROUND(cx + squareSize * 0.5f), cBottom = PXROUND(cy + squareSize * 0.5f);
    draw_tile_cells(center, (float)cLeft, (float)cTop, (float)(cRight - cLeft), (float)(cBottom - cTop), NULL, NULL);

    for (int i = 1; i < g->count; i++) {
        const LocalChunkFields *t = g->tiles[i];

        int slot = -1;
        for (int k = 0; k < center->degree; k++) {
            if (planet->neighbors[centerCell][k] == t->cell) { slot = k; break; }
        }
        if (slot < 0) continue;

        PlanetV3 mid = edge_midpoint(planet, centerCell, t->cell);
        float lx, ly;
        project_local(centerPos, east, north, mid, &lx, &ly);
        float angle = atan2f(lx, ly);
        if (angle < 0.0f) angle += 2.0f * (float)M_PI;
        int dir = ((int)floorf(angle / ((float)M_PI / 3.0f) + 0.5f)) % 6;

        LocalChunkEdgeRole role = local_chunk_edge_role(planet, centerCell, slot);
        bool staggered = (role == LOCAL_CHUNK_EDGE_NORTH || role == LOCAL_CHUNK_EDGE_SOUTH);

        float tcx = cx + offX[dir] * squareSize, tcy = cy + offY[dir] * squareSize;
        int left = PXROUND(tcx - squareSize * 0.5f), top = PXROUND(tcy - squareSize * 0.5f);
        int right = PXROUND(tcx + squareSize * 0.5f), bottom = PXROUND(tcy + squareSize * 0.5f);
        draw_tile_cells(t, (float)left, (float)top, (float)(right - left), (float)(bottom - top), NULL, NULL);

        Color outline = staggered ? (Color){ 255, 200, 80, 255 } : (Color){ 70, 75, 90, 220 };
        DrawRectangleLines(left, top, right - left, bottom - top, outline);
        DrawText(TextFormat("#%d", t->cell), left + 3, top + 2, 11, (Color){ 255, 255, 255, 220 });
    }

    DrawRectangleLines(cLeft, cTop, cRight - cLeft, cBottom - cTop, (Color){ 255, 255, 255, 255 });
    DrawText(TextFormat("#%d", centerCell), cLeft + 3, cTop + 2, 11, (Color){ 255, 255, 255, 255 });

    #undef PXROUND
}
