#ifndef WORLD_WORLD_GEN_H
#define WORLD_WORLD_GEN_H

#include <stdint.h>
#include <stdbool.h>
#include "services/world/planet.h"
#include "components/tile.h"

typedef struct WorldGenParams {
    uint32_t seed;
    float sea_level;
    float mountain_level;
    float noise_scale;
    float warmth;

    float rain_shadow;
    float moisture_reach;

    bool  enable_hydrology;
    float river_density;

    uint8_t   plate_count;
} WorldGenParams;

WorldGenParams world_gen_default_params(uint32_t seed);

typedef struct WorldGenSample {
    float elevation;
    float temperature;
    float humidity;
} WorldGenSample;

typedef struct WorldGenerator {
    const char *name;
    WorldGenSample (*sample)(const struct WorldGenerator *self,
                             const WorldGenParams *params,
                             PlanetV3 pos, int32_t cell);
    void *state;
} WorldGenerator;

int32_t                world_gen_count(void);
const WorldGenerator  *world_gen_get(int32_t index);
const char            *world_gen_name(int32_t index);

TileTerrain tile_classify(const WorldGenParams *params,
                          float elevation, float temperature, float humidity);

#endif
