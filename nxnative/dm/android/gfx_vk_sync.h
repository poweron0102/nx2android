// Bounded observational diagnostics; all file I/O runs on the reporter thread.
#pragma once
#include "vk_sync_gate.h"
enum { SYNC_SHADERS = 32, SYNC_RESOURCES = 48, SYNC_ATTACHMENTS = 9 };
typedef struct { uint64_t key[3]; uint32_t variant, topo, draws; } vk_sync_shader;
typedef struct { uint64_t pool, off, size; uint32_t fmt, w, h, samples, aspect, level, layer, layers, load, store; } vk_sync_attachment;
typedef struct { uint64_t pool, off, size; uint32_t stage, access, kind; } vk_sync_resource;
typedef struct {
    uint32_t w, h, samples, nshader, nresource, overflow, end_line;
    vk_sync_attachment att[SYNC_ATTACHMENTS];
    vk_sync_shader shader[SYNC_SHADERS];
    vk_sync_resource resource[SYNC_RESOURCES];
} vk_sync_pass;
extern _Atomic int g_vk_sync_active, g_vk_sync_level;
void vk_sync_init(void);
void vk_sync_cancel(void);
void vk_sync_command(VkCommandBuffer cb, unsigned kind);
void vk_sync_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags flags, uint32_t nm, const VkMemoryBarrier *m,
    uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im);
void vk_sync_full_note(const char *file, unsigned line);
void vk_sync_gs_mark(VkCommandBuffer cb, int raster_follows);
void vk_sync_begin_barrier(VkCommandBuffer cb);
void vk_sync_lifecycle(const char *file, unsigned line, int state);
void vk_sync_frame(uint64_t frame);
void vk_sync_register_cb(VkCommandBuffer cb, int upload);
void vk_sync_pass_begin(const vk_sync_pass *p);
void vk_sync_pass_shader(const uint64_t keys[3], uint32_t variant, uint32_t topo);
void vk_sync_pass_resource(uint64_t pool, uint64_t off, uint64_t size, uint32_t stage, uint32_t access, uint32_t kind);
uint32_t vk_sync_pass_end(void);
void vk_sync_dispatch_end(void);
uint32_t vk_sync_last_id(void);
void vk_sync_gpu(uint32_t id, uint64_t ns);
void vk_sync_query_overflow(void);
void vk_sync_end_at(const char *file, unsigned line);
void vk_sync_end_land_at(const char *file, unsigned line);
void vk_sync_full_at(VkCommandBuffer cb, const char *file, unsigned line);
#define vk_img_barrier_all(cb) vk_sync_full_at((cb), __FILE__, __LINE__)
