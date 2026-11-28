#ifndef SERVICES_STORES_ATLAS_STORE_H
#define SERVICES_STORES_ATLAS_STORE_H

#include "raylib.h"
#include "components/atlas.h"
#include "services/stores/store.h"

#define ATLAS_STORE_HINT 128

typedef struct {
    StoreSlot base;     // must be first
    Atlas atlas;
} AtlasSlot;

void AtlasStore_init(void);
void AtlasStore_shutdown(void);

#endif // SERVICES_STORES_ATLAS_STORE_H
