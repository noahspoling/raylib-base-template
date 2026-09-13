#ifndef WORLD_PIPELINE_H
#define WORLD_PIPELINE_H

#include "services/world/planet.h"
#include "services/world/world_gen.h"
#include "kdtree.h"

#define WORLD_NO_WATER (-1e30f)

typedef struct WorldFields {
    const Planet         *planet;
    const WorldGenParams *params;
    int   count;

    float         *elevation;
    float         *temperature;
    float         *humidity;
    float         *rainfall;
    float         *moisture;
    int           *downhill;
    float         *flow;
    unsigned char *river;
    float         *water_level;
    int           *region;
    unsigned char *terrain;
    int           *plate;
    unsigned char *fault;
    float         *stress;
} WorldFields;

enum { WORLD_FAULT_NONE = 0, WORLD_FAULT_CONVERGENT, WORLD_FAULT_DIVERGENT, WORLD_FAULT_TRANSFORM };

PlanetV3 world_climate_wind_at(PlanetV3 pos);

PlanetV3 world_climate_wind(const Planet *p, int cell);

typedef struct WindField {
    int       count;
    PlanetV3 *pos;
    PlanetV3 *wind;
    KDTree_T  index;
} WindField;

WindField world_wind_field_build(int count);
void      world_wind_field_free(WindField *wf);

PlanetV3  world_wind_sample(const WindField *wf, PlanetV3 pos);

void world_climate_run(WorldFields *f, const WindField *wind);

typedef struct PlateField {
    int            count;
    PlanetV3      *seed;
    PlanetV3      *axis;
    float         *speed;
    unsigned char *oceanic;
    KDTree_T       index;
} PlateField;

PlateField world_plate_field_build(int count, unsigned int seed);
void       world_plate_field_free(PlateField *pf);

void world_tectonics_run(WorldFields *f, const PlateField *pf);

void world_biomes_classify(WorldFields *f);

void world_hydrology_run(WorldFields *f);

void world_regions_run(WorldFields *f);

#endif
