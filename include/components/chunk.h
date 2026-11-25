#ifndef CHUNK_H
#define CHUNK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define CHUNK_SIZE  64
#define CHUNK_TILES (CHUNK_SIZE * CHUNK_SIZE)

typedef struct Tile {
    uint8_t terrain;
    uint8_t flags;
    int16_t height;
} Tile;

typedef struct Chunk {
    int32_t cx, cy;
    Tile    tiles[CHUNK_TILES];
} Chunk;

#define CHUNK_SERIAL_VERSION     1
#define CHUNK_SERIAL_HEADER_SIZE (4 + 2 + 2 + 4 + 4)
#define CHUNK_SERIAL_TILE_SIZE   (1 + 1 + 2)
#define CHUNK_SERIAL_SIZE        (CHUNK_SERIAL_HEADER_SIZE + CHUNK_TILES * CHUNK_SERIAL_TILE_SIZE)

size_t chunk_serialize(const Chunk *c, uint8_t *buf, size_t cap);
bool   chunk_deserialize(Chunk *out, const uint8_t *buf, size_t len);

bool chunk_save(const Chunk *c, const char *path);
bool chunk_load(Chunk *out, const char *path);

#endif
