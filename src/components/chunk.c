#include "components/chunk.h"
#include "raylib.h"
#include <stdlib.h>
#include <string.h>

static const uint8_t CHUNK_MAGIC[4] = { 'C', 'H', 'N', 'K' };

static uint8_t *put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t chunk_serialize(const Chunk *c, uint8_t *buf, size_t cap) {
    if (!c || !buf || cap < CHUNK_SERIAL_SIZE) return 0;

    uint8_t *p = buf;
    memcpy(p, CHUNK_MAGIC, 4); p += 4;
    p = put_u16(p, CHUNK_SERIAL_VERSION);
    p = put_u16(p, CHUNK_SIZE);
    p = put_u32(p, (uint32_t)c->cx);
    p = put_u32(p, (uint32_t)c->cy);

    for (int32_t i = 0; i < CHUNK_TILES; i++) {
        const Tile *t = &c->tiles[i];
        *p++ = t->terrain;
        *p++ = t->flags;
        p = put_u16(p, (uint16_t)t->height);
    }
    return (size_t)(p - buf);
}

bool chunk_deserialize(Chunk *out, const uint8_t *buf, size_t len) {
    if (!out || !buf || len < CHUNK_SERIAL_SIZE) return false;

    const uint8_t *p = buf;
    if (memcmp(p, CHUNK_MAGIC, 4) != 0) return false;
    p += 4;
    if (get_u16(p) != CHUNK_SERIAL_VERSION) return false;
    p += 2;
    if (get_u16(p) != CHUNK_SIZE) return false;
    p += 2;

    out->cx = (int32_t)get_u32(p); p += 4;
    out->cy = (int32_t)get_u32(p); p += 4;

    for (int32_t i = 0; i < CHUNK_TILES; i++) {
        Tile *t = &out->tiles[i];
        t->terrain = *p++;
        t->flags   = *p++;
        t->height  = (int16_t)get_u16(p); p += 2;
    }
    return true;
}

bool chunk_save(const Chunk *c, const char *path) {
    uint8_t *buf = malloc(CHUNK_SERIAL_SIZE);
    if (!buf) return false;
    size_t n = chunk_serialize(c, buf, CHUNK_SERIAL_SIZE);
    bool ok = n > 0 && SaveFileData(path, buf, (int)n);
    free(buf);
    return ok;
}

bool chunk_load(Chunk *out, const char *path) {
    int size = 0;
    unsigned char *data = LoadFileData(path, &size);
    if (!data) return false;
    bool ok = size > 0 && chunk_deserialize(out, data, (size_t)size);
    UnloadFileData(data);
    return ok;
}
