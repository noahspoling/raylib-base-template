#include "services/world/world_pipeline.h"
#include <stdint.h>
#include "components/tile.h"

#include "heap.h"
#include "ufind.h"

#include <math.h>
#include <stdlib.h>

typedef struct { int32_t cell; float key; } HCell;

static int hcell_cmp(const void *a, const void *b) {
    float ka = ((const HCell *)a)->key, kb = ((const HCell *)b)->key;
    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static int32_t touches_sea(const Planet *p, const WorldFields *f, int32_t i, float sea) {
    int32_t deg = p->degree[i];
    for (int32_t k = 0; k < deg; k++) {
        int32_t nb = p->neighbors[i][k];
        if (nb >= 0 && f->elevation[nb] < sea) return 1;
    }
    return 0;
}

void world_hydrology_run(WorldFields *f) {
    const WorldGenParams *pm = f->params;
    if (!pm->enable_hydrology) return;

    const Planet *p = f->planet;
    int32_t       n   = f->count;
    float         sea = pm->sea_level;

    float   *fill    = (float *)malloc((size_t)n * sizeof(float));
    char    *visited = (char  *)calloc((size_t)n, 1);
    int32_t *order   = (int32_t   *)malloc((size_t)n * sizeof(int32_t));
    if (!fill || !visited || !order) { free(fill); free(visited); free(order); return; }

    Heap_T pq = Heap_new(1024, (int32_t)sizeof(HCell), hcell_cmp);

    for (int32_t i = 0; i < n; i++) { fill[i] = f->elevation[i]; f->downhill[i] = -1; }

    int32_t seeds = 0;
    for (int32_t i = 0; i < n; i++) {
        if (f->elevation[i] < sea) {
            visited[i] = 1;
            HCell h = { i, f->elevation[i] };
            Heap_push(pq, &h);
            seeds++;
        }
    }
    if (seeds == 0) {
        int32_t lo = 0;
        for (int32_t i = 1; i < n; i++) if (f->elevation[i] < f->elevation[lo]) lo = i;
        visited[lo] = 1;
        HCell h = { lo, f->elevation[lo] };
        Heap_push(pq, &h);
    }

    const float eps = 1e-5f;
    int32_t ordn = 0;
    HCell cur;
    while (Heap_pop(pq, &cur)) {
        int32_t c = cur.cell;
        order[ordn++] = c;
        int32_t deg = p->degree[c];
        for (int32_t k = 0; k < deg; k++) {
            int32_t nb = p->neighbors[c][k];
            if (nb < 0 || visited[nb]) continue;
            visited[nb] = 1;
            fill[nb] = f->elevation[nb] > fill[c] ? f->elevation[nb] : fill[c] + eps;
            HCell h = { nb, fill[nb] };
            Heap_push(pq, &h);
        }
    }
    Heap_free(&pq);

    for (int32_t i = 0; i < n; i++) {
        f->downhill[i] = -1;
        if (f->elevation[i] < sea) continue;
        float best = fill[i];
        int32_t deg = p->degree[i];
        for (int32_t k = 0; k < deg; k++) {
            int32_t nb = p->neighbors[i][k];
            if (nb >= 0 && fill[nb] < best) { best = fill[nb]; f->downhill[i] = nb; }
        }
    }

    for (int32_t i = 0; i < n; i++)
        f->flow[i] = (f->elevation[i] < sea) ? 0.0f : f->rainfall[i];
    for (int32_t k = ordn - 1; k >= 0; k--) {
        int32_t c = order[k];
        int32_t r = f->downhill[c];
        if (r >= 0) f->flow[r] += f->flow[c];
    }

    uint8_t *ord = (uint8_t *)calloc((size_t)n, 1);
    uint8_t *mx1 = (uint8_t *)calloc((size_t)n, 1);
    uint8_t *mx2 = (uint8_t *)calloc((size_t)n, 1);
    for (int32_t i = 0; i < n; i++) f->river[i] = 0;
    if (ord && mx1 && mx2) {
        for (int32_t k = ordn - 1; k >= 0; k--) {
            int32_t c = order[k];
            if (f->elevation[c] < sea) continue;
            int32_t oc = (mx1[c] == 0) ? 1 : (mx1[c] == mx2[c] ? mx1[c] + 1 : mx1[c]);
            if (oc > 250) oc = 250;
            ord[c] = (uint8_t)oc;
            int32_t d = f->downhill[c];
            if (d >= 0 && f->elevation[d] >= sea) {
                if (oc > mx1[d])      { mx2[d] = mx1[d]; mx1[d] = (uint8_t)oc; }
                else if (oc > mx2[d]) { mx2[d] = (uint8_t)oc; }
            }
        }
        float density = pm->river_density < 0.0f ? 0.0f : (pm->river_density > 2.0f ? 2.0f : pm->river_density);
        int32_t min_order = (int32_t)floorf(1.0f + (float)(p->level - 3) * 0.6f
                                        - (density - 0.5f) * 2.0f + 0.5f);
        if (min_order < 1) min_order = 1;
        if (min_order > 6) min_order = 6;
        for (int32_t i = 0; i < n; i++) {
            if (f->elevation[i] < sea || ord[i] < min_order) continue;
            int32_t wdt = ord[i] - min_order + 1;
            if (wdt > 5) wdt = 5;
            f->river[i] = (uint8_t)wdt;
        }
    }
    free(ord); free(mx1); free(mx2);

    const float   lake_eps = 0.010f;
    const int32_t lake_min = 5;
    UFind_T       uf = UFind_new(n);
    for           (int32_t i = 0; i < n; i++) {
        if (f->elevation[i] < sea) continue;
        if (fill[i] - f->elevation[i] <= lake_eps) continue;
        int32_t deg = p->degree[i];
        for (int32_t k = 0; k < deg; k++) {
            int32_t nb = p->neighbors[i][k];
            if (nb < 0 || f->elevation[nb] < sea) continue;
            if (fill[nb] - f->elevation[nb] > lake_eps) UFind_union(uf, i, nb);
        }
    }
    const float lake_evap_rate = 0.35f;
    float *basin_inflow = (float *)calloc((size_t)n, sizeof(float));
    if (basin_inflow) {
        for (int32_t i = 0; i < n; i++) {
            if (f->elevation[i] < sea) continue;
            if (fill[i] - f->elevation[i] <= lake_eps) continue;
            int32_t root = UFind_find(uf, i);
            if (f->flow[i] > basin_inflow[root]) basin_inflow[root] = f->flow[i];
        }
    }
    for (int32_t i = 0; i < n; i++) {
        if (f->elevation[i] < sea) continue;
        if (fill[i] - f->elevation[i] <= lake_eps) continue;
        if (touches_sea(p, f, i, sea)) continue;
        int32_t root = UFind_find(uf, i);
        int32_t area = UFind_size(uf, i);
        float inflow = basin_inflow ? basin_inflow[root] : f->flow[i];
        if (area >= lake_min && inflow >= lake_evap_rate * (float)area)
            f->terrain[i] = (uint8_t)TILE_TERRAIN_LAKE;
    }
    free(basin_inflow);
    UFind_free(&uf);

    const float lake_grow = 0.015f;
    float *surf = (float *)malloc((size_t)n * sizeof(float));
    if (surf) {
        for (int32_t i = 0; i < n; i++)
            surf[i] = (f->terrain[i] == TILE_TERRAIN_LAKE) ? fill[i] : -1e30f;
        for (int32_t pass = 0; pass < 2; pass++) {
            for (int32_t i = 0; i < n; i++) {
                if (f->elevation[i] < sea || f->terrain[i] == TILE_TERRAIN_LAKE) continue;
                if (touches_sea(p, f, i, sea)) continue;
                int32_t deg = p->degree[i];
                float best = -1e30f;
                for (int32_t k = 0; k < deg; k++) {
                    int32_t nb = p->neighbors[i][k];
                    if (nb >= 0 && surf[nb] > best) best = surf[nb];
                }
                if (best > -1e29f && f->elevation[i] < best + lake_grow) {
                    f->terrain[i] = (uint8_t)TILE_TERRAIN_LAKE;
                    surf[i] = best;
                }
            }
        }
        for (int32_t i = 0; i < n; i++)
            if (f->terrain[i] == TILE_TERRAIN_LAKE) f->water_level[i] = surf[i];
        free(surf);
    }

    for (int32_t i = 0; i < n; i++)
        if (f->terrain[i] == TILE_TERRAIN_LAKE) f->river[i] = 0;

    free(fill);
    free(visited);
    free(order);
}
