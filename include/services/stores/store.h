#ifndef SERVICES_STORES_STORE_H
#define SERVICES_STORES_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include "array.h"
#include "table.h"

// Common header every slot type must start with, so the store can
// read the name and refcount without knowing the concrete slot type.
typedef struct {
    const char *name;   // interned (Atom)
    int32_t refs;
} StoreSlot;

// Per-store hooks. load runs on the 0 -> 1 acquire, unload on the 1 -> 0
// release and at shutdown for anything still resident.
typedef struct {
    bool (*load)(StoreSlot *slot);
    void (*unload)(StoreSlot *slot);
    bool (*is_resident)(const StoreSlot *slot);
} StoreOps;

typedef struct {
    Array_T slots;
    Table_T by_name;
    int32_t count;
    const char *tag;    // log prefix, e.g. "TEXTURES"
    StoreOps ops;
} Store;

void Store_init(Store *s, int32_t hint, int32_t slot_size, const char *tag, StoreOps ops);
void Store_shutdown(Store *s);

// Ids are 1-based; 0 means "none". Returns NULL for out-of-range ids.
StoreSlot *Store_at(const Store *s, int32_t id);

// name must already be interned with Atom_string.
int32_t Store_find(const Store *s, const char *name);
// Appends a slot (copied; must begin with StoreSlot whose name is interned)
// and indexes it by name. Returns the new id.
int32_t Store_add(Store *s, const void *slot);
// Makes name resolve to an existing id.
void Store_alias(Store *s, const char *name, int32_t id);

bool Store_acquire(Store *s, int32_t id);
void Store_release(Store *s, int32_t id);
bool Store_is_resident(const Store *s, int32_t id);
const char *Store_name(const Store *s, int32_t id);

#endif
