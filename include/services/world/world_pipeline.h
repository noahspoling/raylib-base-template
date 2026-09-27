#ifndef WORLD_PIPELINE_H
#define WORLD_PIPELINE_H

#include <stdint.h>
#include "services/world/planet.h"
#include "services/world/world_gen.h"
#include "kdtree.h"

#define WORLD_NO_WATER (-1e30f)

typedef struct WorldFields {
    const Planet         *planet;
    const WorldGenParams *params;
    int32_t               count;

    float         *elevation;
    float         *temperature;
    float         *humidity;
    float         *rainfall;
    float         *moisture;
    int32_t       *downhill;
    float         *flow;
    uint8_t       *river;
    float         *water_level;
    int32_t       *region;
    uint8_t       *terrain;
    int8_t        *plate;
    uint8_t       *fault;
    float         *stress;
} WorldFields;

enum { WORLD_FAULT_NONE = 0, WORLD_FAULT_CONVERGENT, WORLD_FAULT_DIVERGENT, WORLD_FAULT_TRANSFORM };

PlanetV3 world_climate_wind_at(PlanetV3 pos);

PlanetV3 world_climate_wind(const Planet *p, int32_t cell);

typedef struct WindField {
    int32_t    count;
    PlanetV3  *pos;
    PlanetV3  *wind;
    KDTree_T   index;
} WindField;

WindField world_wind_field_build(int32_t count);
void      world_wind_field_free(WindField *wf);

PlanetV3  world_wind_sample(const WindField *wf, PlanetV3 pos);

void world_climate_run(WorldFields *f, const WindField *wind);

typedef struct PlateField {
    int32_t         count;
    PlanetV3       *seed;
    PlanetV3       *axis;
    float          *speed;
    uint8_t        *oceanic;
    KDTree_T        index;
} PlateField;

PlateField world_plate_field_build(int32_t count, uint32_t seed);
void       world_plate_field_free(PlateField *pf);

void world_tectonics_run(WorldFields *f, const PlateField *pf);

void world_biomes_classify(WorldFields *f);

void world_hydrology_run(WorldFields *f);

void world_regions_run(WorldFields *f);

#endif
