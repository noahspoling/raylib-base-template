#include "services/world/world_pipeline.h"
#include <stdint.h>
#include "components/tile.h"

#include <stdlib.h>
#include <string.h>

void world_biomes_classify(WorldFields *f) {
    const Planet *p = f->planet;
    int32_t       n   = f->count;
    float         sea = f->params->sea_level;

    for (int32_t i = 0; i < n; i++) {
        f->terrain[i] = (uint8_t)tile_classify(f->params, f->elevation[i],
                                                     f->temperature[i], f->humidity[i]);
        f->water_level[i] = (f->elevation[i] < sea) ? sea : WORLD_NO_WATER;
    }

    uint8_t *snap = (uint8_t *)malloc((size_t)n);
    if (!snap) return;
    for (int32_t pass = 0; pass < 2; pass++) {
        memcpy(snap, f->terrain, (size_t)n);
        for (int32_t i = 0; i < n; i++) {
            if (f->elevation[i] < sea) continue;
            int32_t deg = p->degree[i], same = 0;
            int32_t counts[TILE_TERRAIN_COUNT];
            for (int32_t t = 0; t < TILE_TERRAIN_COUNT; t++) counts[t] = 0;
            for (int32_t k = 0; k < deg; k++) {
                int32_t nb = p->neighbors[i][k];
                if (nb < 0 || f->elevation[nb] < sea) continue;
                counts[snap[nb]]++;
                if (snap[nb] == snap[i]) same++;
            }
            if (same == 0) {
                int32_t best = snap[i], bc = 0;
                for (int32_t t = 0; t < TILE_TERRAIN_COUNT; t++)
                    if (counts[t] > bc) { bc = counts[t]; best = t; }
                if (bc > 0) f->terrain[i] = (uint8_t)best;
            }
        }
    }
    free(snap);
}
