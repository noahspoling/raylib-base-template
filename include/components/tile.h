#ifndef COMPONENTS_TILE_H
#define COMPONENTS_TILE_H

#include "raylib.h"

typedef enum {
    TILE_TERRAIN_OCEAN = 0,
    TILE_TERRAIN_SHALLOW,
    TILE_TERRAIN_GRASS,
    TILE_TERRAIN_FOREST,
    TILE_TERRAIN_ROCK,
    TILE_TERRAIN_SNOW,
    TILE_TERRAIN_DESERT,
    TILE_TERRAIN_TUNDRA,
    TILE_TERRAIN_SAVANNA,
    TILE_TERRAIN_RAINFOREST,
    TILE_TERRAIN_TAIGA,
    TILE_TERRAIN_SWAMP,
    TILE_TERRAIN_RIVER,
    TILE_TERRAIN_LAKE,
    TILE_TERRAIN_COUNT
} TileTerrain;

typedef struct TileComp {
    int   cell;
    float elevation;
    float temperature;
    float humidity;
    float rainfall;
    float flow;
    float water_level;
    int   region;
    int   plate;
    unsigned char fault;
    float stress;
    unsigned char river;
    unsigned char terrain;
} TileComp;

extern const Color TILE_TERRAIN_COLORS[TILE_TERRAIN_COUNT];

const char *tile_terrain_name(int terrain);

#endif
