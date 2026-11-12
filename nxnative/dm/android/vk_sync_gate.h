// Exact adjacency gate, independent of Vulkan and shared by the recording harness.
#pragma once
#include <stdint.h>
typedef struct { uint64_t serial, marked; uintptr_t cb; int pending; } vk_sync_gate;
static inline void vk_sync_gate_command(vk_sync_gate *g) { ++g->serial; g->pending = 0; }
static inline void vk_sync_gate_mark(vk_sync_gate *g, uintptr_t cb, int raster_follows) {
    g->marked = g->serial; g->cb = cb; g->pending = raster_follows;
}
static inline int vk_sync_gate_take(vk_sync_gate *g, uintptr_t cb) {
    int ok = g->pending && g->cb == cb && g->marked == g->serial;
    g->pending = 0; return ok;
}
