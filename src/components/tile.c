#include "components/tile.h"

const Color TILE_TERRAIN_COLORS[TILE_TERRAIN_COUNT] = {
    [TILE_TERRAIN_OCEAN]   = {  28,  62, 128, 255 },
    [TILE_TERRAIN_SHALLOW] = {  52, 118, 176, 255 },
    [TILE_TERRAIN_GRASS]   = {  96, 158,  74, 255 },
    [TILE_TERRAIN_FOREST]  = {  40,  98,  50, 255 },
    [TILE_TERRAIN_ROCK]    = { 120, 110, 102, 255 },
    [TILE_TERRAIN_SNOW]    = { 242, 246, 250, 255 },
    [TILE_TERRAIN_DESERT]  = { 214, 178,  96, 255 },
    [TILE_TERRAIN_TUNDRA]  = { 156, 156, 132, 255 },
    [TILE_TERRAIN_SAVANNA]    = { 182, 170,  86, 255 },
    [TILE_TERRAIN_RAINFOREST] = {  22,  84,  40, 255 },
    [TILE_TERRAIN_TAIGA]      = {  58, 100,  76, 255 },
    [TILE_TERRAIN_SWAMP]      = {  70,  92,  62, 255 },
    [TILE_TERRAIN_RIVER]      = {  70, 150, 210, 255 },
    [TILE_TERRAIN_LAKE]       = {  46, 106, 168, 255 },
};

const char *tile_terrain_name(int terrain) {
    switch (terrain) {
        case TILE_TERRAIN_OCEAN:   return "Ocean";
        case TILE_TERRAIN_SHALLOW: return "Shallows";
        case TILE_TERRAIN_GRASS:   return "Grassland";
        case TILE_TERRAIN_FOREST:  return "Forest";
        case TILE_TERRAIN_ROCK:    return "Rock";
        case TILE_TERRAIN_SNOW:    return "Snow";
        case TILE_TERRAIN_DESERT:  return "Desert";
        case TILE_TERRAIN_TUNDRA:  return "Tundra";
        case TILE_TERRAIN_SAVANNA:    return "Savanna";
        case TILE_TERRAIN_RAINFOREST: return "Rainforest";
        case TILE_TERRAIN_TAIGA:      return "Taiga";
        case TILE_TERRAIN_SWAMP:      return "Swamp";
        case TILE_TERRAIN_RIVER:      return "River";
        case TILE_TERRAIN_LAKE:       return "Lake";
        default:                   return "?";
    }
}
