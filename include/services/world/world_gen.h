#ifndef WORLD_WORLD_GEN_H
#define WORLD_WORLD_GEN_H

#include <stdbool.h>
#include "services/world/planet.h"
#include "components/tile.h"

typedef struct WorldGenParams {
    unsigned int seed;
    float sea_level;
    float mountain_level;
    float noise_scale;
    float warmth;

    float rain_shadow;
    float moisture_reach;

    bool  enable_hydrology;
    float river_density;

    int   plate_count;
} WorldGenParams;

WorldGenParams world_gen_default_params(unsigned int seed);

typedef struct WorldGenSample {
    float elevation;
    float temperature;
    float humidity;
} WorldGenSample;

typedef struct WorldGenerator {
    const char *name;
    WorldGenSample (*sample)(const struct WorldGenerator *self,
                             const WorldGenParams *params,
                             PlanetV3 pos, int cell);
    void *state;
} WorldGenerator;

int                    world_gen_count(void);
const WorldGenerator  *world_gen_get(int index);
const char            *world_gen_name(int index);

TileTerrain tile_classify(const WorldGenParams *params,
                          float elevation, float temperature, float humidity);

#endif
