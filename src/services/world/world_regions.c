#include "services/world/world_pipeline.h"
#include <stdint.h>
#include "components/tile.h"

#include "heap.h"

#include <stdlib.h>
#include <math.h>

#define REGION_TARGET_CELLS 6000
#define REGION_MIN_COUNT    5
#define REGION_MAX_COUNT    120

typedef struct { int32_t cell; float cost; } RCell;

static int rcmp(const void *a, const void *b) {
    float ca = ((const RCell *)a)->cost, cb = ((const RCell *)b)->cost;
    return ca < cb ? -1 : (ca > cb ? 1 : 0);
}

static float rhash(int32_t i, uint32_t seed) {
    uint32_t h = (uint32_t)i * 2654435761u ^ (seed * 40503u + 0x9e3779b9u);
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return (float)(h & 0xffffffu) / (float)0xffffffu;
}

static float enter_cost(const WorldFields *f, int32_t nb, float mountain, uint32_t seed) {
    float e   = f->elevation[nb];
    float mtn = (e - (mountain - 0.20f)) / 0.30f;
    if (mtn < 0.0f) mtn = 0.0f; else if (mtn > 1.0f) mtn = 1.0f;
    float cost = 1.0f + 5.0f * mtn;
    if (f->river[nb] > 0 || f->terrain[nb] == TILE_TERRAIN_LAKE)
        cost += 3.0f + 1.5f * (float)f->river[nb];
    return cost * (0.7f + 0.6f * rhash(nb, seed + 7u));
}

void world_regions_run(WorldFields *f) {
    const Planet *p   = f->planet;
    int32_t       n           = f->count;
    float         sea         = f->params->sea_level;
    float         mountain    = f->params->mountain_level;
    uint32_t      seed = f->params->seed;

    for (int32_t i = 0; i < n; i++) f->region[i] = -1;

    int32_t land = 0, lowland = 0;
    float low_thresh = sea + (mountain - sea) * 0.55f;
    for (int32_t i = 0; i < n; i++) {
        if (f->elevation[i] < sea) continue;
        land++;
        if (f->elevation[i] < low_thresh) lowland++;
    }
    if (land == 0) return;
    if (lowland == 0) { low_thresh = 1e9f; lowland = land; }

    float *dist  = (float *)malloc((size_t)n * sizeof(float));
    if (!dist) return;
    for (int32_t i = 0; i < n; i++) dist[i] = 1e30f;

    Heap_T pq = Heap_new(1024, (int32_t)sizeof(RCell), rcmp);

    int32_t desired = land / REGION_TARGET_CELLS;
    if (desired < REGION_MIN_COUNT) desired = REGION_MIN_COUNT;
    if (desired > REGION_MAX_COUNT) desired = REGION_MAX_COUNT;
    float pth = (float)desired / (float)lowland;
    int32_t next_id = 0;
    for (int32_t i = 0; i < n; i++) {
        if (f->elevation[i] < sea || f->elevation[i] >= low_thresh) continue;
        if (rhash(i, seed) < pth) {
            f->region[i] = next_id++;
            dist[i] = 0.0f;
            RCell r = { i, 0.0f };
            Heap_push(pq, &r);
        }
    }
    if (next_id == 0) {
        for (int32_t i = 0; i < n; i++)
            if (f->elevation[i] >= sea) {
                f->region[i] = next_id++;
                dist[i] = 0.0f;
                RCell r = { i, 0.0f };
                Heap_push(pq, &r);
                break;
            }
    }

    RCell cur;
    while (Heap_pop(pq, &cur)) {
        int32_t c = cur.cell;
        if (cur.cost > dist[c]) continue;
        int32_t deg = p->degree[c], rid = f->region[c];
        for (int32_t k = 0; k < deg; k++) {
            int32_t nb = p->neighbors[c][k];
            if (nb < 0 || f->elevation[nb] < sea) continue;
            float nd = cur.cost + enter_cost(f, nb, mountain, seed);
            if (nd < dist[nb]) {
                dist[nb] = nd;
                f->region[nb] = rid;
                RCell r = { nb, nd };
                Heap_push(pq, &r);
            }
        }
    }
    Heap_free(&pq);
    free(dist);

    int32_t *queue = (int32_t *)malloc((size_t)n * sizeof(int32_t));
    if (queue) {
        for (int32_t i = 0; i < n; i++) {
            if (f->elevation[i] < sea || f->region[i] != -1) continue;
            int32_t rid = next_id++, qh = 0, qt = 0;
            f->region[i] = rid;
            queue[qt++] = i;
            while (qh < qt) {
                int32_t c = queue[qh++], deg = p->degree[c];
                for (int32_t k = 0; k < deg; k++) {
                    int32_t nb = p->neighbors[c][k];
                    if (nb < 0 || f->elevation[nb] < sea || f->region[nb] != -1) continue;
                    f->region[nb] = rid;
                    queue[qt++] = nb;
                }
            }
        }
        free(queue);
    }
}
