#include "services/stores/store.h"
#include <stddef.h>
#include "raylib.h"

void Store_init(Store *s, int32_t hint, int32_t slot_size, const char *tag, StoreOps ops) {
    s->slots = Array_new(hint, slot_size);
    s->by_name = Table_new(hint, NULL, NULL);
    s->count = 0;
    s->tag = tag;
    s->ops = ops;
}

void Store_shutdown(Store *s) {
    if (!s->slots) return;
    for (int32_t i = 1; i <= s->count; i++) {
        StoreSlot *slot = Store_at(s, i);
        if (s->ops.unload && Store_is_resident(s, i)) s->ops.unload(slot);
        slot->refs = 0;
    }
    Table_free(&s->by_name);
    Array_free(&s->slots);
    s->count = 0;
}

StoreSlot *Store_at(const Store *s, int32_t id) {
    if (!s->slots || id < 1 || id > s->count) return NULL;
    return (StoreSlot *)Array_get(s->slots, id - 1);
}

int32_t Store_find(const Store *s, const char *name) {
    if (!s->by_name || !name) return 0;
    return (int32_t)(intptr_t)Table_get(s->by_name, name);
}

int32_t Store_add(Store *s, const void *slot) {
    if (!s->slots) return 0;
    if (s->count == Array_length(s->slots)) {
        Array_resize(s->slots, s->count ? s->count * 2 : 1);
    }
    Array_put(s->slots, s->count, (void *)slot);
    s->count++;
    Table_put(s->by_name, ((const StoreSlot *)slot)->name, (void *)(intptr_t)s->count);
    return s->count;
}

// TODO Resources released released with an alias might not be clearing the table on release 
void Store_alias(Store *s, const char *name, int32_t id) {
    if (!s->by_name || !Store_at(s, id)) return;
    Table_put(s->by_name, name, (void *)(intptr_t)id);
}

bool Store_acquire(Store *s, int32_t id) {
    StoreSlot *slot = Store_at(s, id);
    if (!slot) return false;
    if (slot->refs == 0 && s->ops.load && !s->ops.load(slot)) {
        TraceLog(LOG_WARNING, "%s: failed to load '%s'", s->tag, slot->name);
        return false;
    }
    slot->refs++;
    return true;
}

void Store_release(Store *s, int32_t id) {
    StoreSlot *slot = Store_at(s, id);
    if (!slot || slot->refs == 0) return;
    if (--slot->refs == 0 && s->ops.unload) s->ops.unload(slot);
}

bool Store_is_resident(const Store *s, int32_t id) {
    StoreSlot *slot = Store_at(s, id);
    if (!slot) return false;
    return s->ops.is_resident ? s->ops.is_resident(slot) : slot->refs > 0;
}

const char *Store_name(const Store *s, int32_t id) {
    StoreSlot *slot = Store_at(s, id);
    return slot ? slot->name : NULL;
}
