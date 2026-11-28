#include "services/stores/atlas_store.h"

// TODO: async requests & locking may need a revisit of Store's count/slots.
static Store g_store;

void AtlasStore_init(void) {
    // TODO: fill in load/unload/is_resident once atlas loading lands here.
    Store_init(&g_store, ATLAS_STORE_HINT, sizeof(AtlasSlot), "ATLASES", (StoreOps){ 0 });
}

void AtlasStore_shutdown(void) {
    Store_shutdown(&g_store);
}
