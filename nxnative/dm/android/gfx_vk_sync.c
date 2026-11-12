// Exact GS barrier adjacency and bounded diagnostic aggregation. Never log from draw/barrier hooks.
#include "gfx_vk.h"
#include <unistd.h>
#include <time.h>

enum { LOCS = 96, SCOPES = 32, GROUPS = 2048, QUEUE = 128, DOMAINS = 3 };
_Atomic int g_vk_sync_active, g_vk_sync_level;
static _Atomic int coalesce;
typedef struct { const char *file; uint32_t line, kind; uint64_t n; } location;
typedef struct { uint32_t src, dst, access_src, access_dst, flags, type; uint64_t n; } scope;
typedef struct {
    uint64_t frame, commands[DOMAINS], draws[DOMAINS], barriers[DOMAINS], memory[DOMAINS], buffers[DOMAINS], images[DOMAINS], transitions[DOMAINS];
    uint64_t passes, dropped_setups, empty_ends, gs, eligible, removed, rejected, overflow, query_overflow, queue_drops;
    uint32_t nloc, nscope, variant;
    location loc[LOCS]; scope scopes[SCOPES];
} frame_stats;
typedef struct { vk_sync_pass meta, key; uint64_t hash, family; _Atomic uint64_t passes, gpu_ns, gpu_n; _Atomic int ready, resource_varies; } group;
static group groups[GROUPS]; // Entries are immutable after release publication; IDs never reused.
static unsigned ngroup;
static frame_stats queue[QUEUE]; static _Atomic unsigned qwrite, qread;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static _Thread_local struct {
    vk_sync_gate gate; VkCommandBuffer main, upload; frame_stats stats;
    vk_sync_pass pass; uint32_t last_id, end_line; int open;
} T;
static uint64_t hash_bytes(uint64_t h, const void *v, size_t n) {
    const unsigned char *p = v; for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull; return h;
}
static void addloc(const char *file, unsigned line, unsigned kind) {
    if (!g_vk_sync_level) return;
    frame_stats *s = &T.stats;
    for (unsigned i = 0; i < s->nloc; ++i) if (s->loc[i].file == file && s->loc[i].line == line && s->loc[i].kind == kind) { s->loc[i].n++; return; }
    if (s->nloc == LOCS) { s->overflow++; return; }
    s->loc[s->nloc++] = (location){file, line, kind, 1};
}
static void addscope(uint32_t src, uint32_t dst, uint32_t a, uint32_t b, uint32_t flags, unsigned type) {
    if (g_vk_sync_level < 2) return;
    frame_stats *s = &T.stats;
    for (unsigned i = 0; i < s->nscope; ++i) {
        scope *x = &s->scopes[i]; if (x->src == src && x->dst == dst && x->access_src == a && x->access_dst == b && x->flags == flags && x->type == type) { ++x->n; return; }
    }
    if (s->nscope == SCOPES) { ++s->overflow; return; }
    s->scopes[s->nscope++] = (scope){src, dst, a, b, flags, type, 1};
}
static const char *basefile(const char *p) { const char *b = strrchr(p, '/'); const char *c = strrchr(p, '\\'); return b ? b + 1 : c ? c + 1 : p; }
static void dump_groups(FILE *f) {
    for (unsigned i = 0; i < GROUPS; ++i) {
        group *g = &groups[i]; if (!atomic_load_explicit(&g->ready, memory_order_acquire)) continue;
        uint64_t n = atomic_load(&g->gpu_n), ns = atomic_load(&g->gpu_ns), p = atomic_load(&g->passes);
        fprintf(f, "G\t%u\t%016llx\t%016llx\t%llu\t%llu\t%llu\t%u\t%u\t%u\t%u\n", i + 1,
            (unsigned long long)g->hash, (unsigned long long)g->family, (unsigned long long)p,
            (unsigned long long)n, (unsigned long long)ns, g->meta.w, g->meta.h, g->meta.overflow | (atomic_load(&g->resource_varies) ? 4u:0u), g->meta.end_line);
        // Aggregate group metadata, never one log per draw or pass instance.
        for (unsigned j = 0; j < g->meta.nshader; ++j) {
            vk_sync_shader *x = &g->meta.shader[j]; fprintf(f, "S\t%u\t%016llx\t%016llx\t%016llx\t%u\t%u\t%u\n", i+1,
                (unsigned long long)x->key[0], (unsigned long long)x->key[1], (unsigned long long)x->key[2], x->variant, x->topo, x->draws);
        }
        for (unsigned j = 0; j < SYNC_ATTACHMENTS; ++j) {
            vk_sync_attachment *a = &g->meta.att[j]; if (!a->fmt) continue;
            fprintf(f, "A\t%u\t%u\t%llx\t%llx\t%llx\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", i+1,j,
                (unsigned long long)a->pool,(unsigned long long)a->off,(unsigned long long)a->size,a->fmt,a->w,a->h,a->samples,a->aspect,a->level,a->layer,a->layers,a->load,a->store);
        }
        for (unsigned j = 0; j < g->meta.nresource; ++j) {
            vk_sync_resource *r = &g->meta.resource[j]; fprintf(f,"R\t%u\t%llx\t%llx\t%llx\t%u\t%u\t%u\n",i+1,
                (unsigned long long)r->pool,(unsigned long long)r->off,(unsigned long long)r->size,r->stage,r->access,r->kind);
        }
    }
    fflush(f);
}
static void *reporter(void *unused) {
    (void)unused; pthread_setname_np(pthread_self(), "vk-sync-report");
    const char *root = getenv("DM_ROOT"); char path[1024];
    snprintf(path,sizeof path,"%s/vk-sync-frames.tsv",root ? root : "."); FILE *f = fopen(path,"w");
    snprintf(path,sizeof path,"%s/vk-sync-categories.tsv",root ? root : "."); FILE *c = fopen(path,"w");
    snprintf(path,sizeof path,"%s/vk-sync-groups.tsv",root ? root : "."); FILE *g = fopen(path,"w");
    if (!f || !c || !g) { if(f)fclose(f);if(c)fclose(c);if(g)fclose(g); VKLOG("[RED] cannot open synchronization diagnostics"); return NULL; }
    fprintf(f,"frame\tvariant\tpasses\tgs\teligible\tremoved\trejected\toverflow\tquery_overflow\tqueue_drops\tempty_ends\tdropped_setups\tmain_commands\tmain_draws\tmain_barriers\tmain_memory\tmain_buffer\tmain_image\tmain_transitions\tupload_commands\tupload_barriers\tother_commands\tother_barriers\n");
    unsigned since = 0;
    for (;;) {
        unsigned r = atomic_load_explicit(&qread,memory_order_relaxed), w = atomic_load_explicit(&qwrite,memory_order_acquire);
        if (r == w) { struct timespec ts={0,10000000}; nanosleep(&ts,NULL); continue; }
        frame_stats s = queue[r % QUEUE]; atomic_store_explicit(&qread,r+1,memory_order_release);
        fprintf(f,"%llu\t%u\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu",
            (unsigned long long)s.frame,s.variant,(unsigned long long)s.passes,(unsigned long long)s.gs,(unsigned long long)s.eligible,
            (unsigned long long)s.removed,(unsigned long long)s.rejected,(unsigned long long)s.overflow,(unsigned long long)s.query_overflow,
            (unsigned long long)s.queue_drops,(unsigned long long)s.empty_ends,(unsigned long long)s.dropped_setups);
        fprintf(f,"\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",
            (unsigned long long)s.commands[0],(unsigned long long)s.draws[0],(unsigned long long)s.barriers[0],(unsigned long long)s.memory[0],
            (unsigned long long)s.buffers[0],(unsigned long long)s.images[0],(unsigned long long)s.transitions[0],
            (unsigned long long)s.commands[1],(unsigned long long)s.barriers[1],(unsigned long long)s.commands[2],(unsigned long long)s.barriers[2]);
        if (g_vk_sync_level >= 2) {
            // Per-frame category aggregates preserve scene selection without individual event output.
            for (unsigned i=0;i<s.nloc;++i) fprintf(c,"L\t%llu\t%s\t%u\t%u\t%llu\n",(unsigned long long)s.frame,basefile(s.loc[i].file),s.loc[i].line,s.loc[i].kind,(unsigned long long)s.loc[i].n);
            for (unsigned i=0;i<s.nscope;++i) { scope *x=&s.scopes[i]; fprintf(c,"B\t%llu\t%x\t%x\t%x\t%x\t%x\t%u\t%llu\n",(unsigned long long)s.frame,x->src,x->dst,x->access_src,x->access_dst,x->flags,x->type,(unsigned long long)x->n); }
        }
        if (++since % 60 == 0) { fflush(f); fflush(c); }
        if (g_vk_sync_level >= 2 && since % 300 == 0) { rewind(g); dump_groups(g); }
    }
    return NULL;
}
static void init_once(void) {
    const char *s=getenv("DM_VK_SYNC_STATS"), *e=getenv("DM_VK_GS_BARRIER_COALESCE");
    g_vk_sync_level=s ? atoi(s):0; coalesce=e && atoi(e); g_vk_sync_active=g_vk_sync_level || coalesce;
    if (g_vk_sync_level) { pthread_t t; if(pthread_create(&t,NULL,reporter,NULL)) die("cannot create Vulkan sync reporter"); pthread_detach(t); }
}
void vk_sync_init(void) { pthread_once(&once,init_once); }
void vk_sync_register_cb(VkCommandBuffer cb,int upload) { if(upload) T.upload=cb; else T.main=cb; }
void vk_sync_command(VkCommandBuffer cb,unsigned kind) {
    if (!g_vk_sync_active) return;
    if (T.gate.pending) ++T.stats.rejected;
    vk_sync_gate_command(&T.gate);
    if (g_vk_sync_level) { unsigned d=cb==T.main ? 0:cb==T.upload ? 1:2; ++T.stats.commands[d]; if(kind==1) ++T.stats.draws[d]; }
}
void vk_sync_cancel(void) { vk_sync_gate_command(&T.gate); }
void vk_sync_barrier(VkCommandBuffer cb,VkPipelineStageFlags src,VkPipelineStageFlags dst,VkDependencyFlags flags,
    uint32_t nm,const VkMemoryBarrier *m,uint32_t nb,const VkBufferMemoryBarrier *b,uint32_t ni,const VkImageMemoryBarrier *im) {
    vk_sync_command(cb,0); if(!g_vk_sync_level) return;
    unsigned d=cb==T.main ? 0:cb==T.upload ? 1:2;
    ++T.stats.barriers[d];T.stats.memory[d]+=nm;T.stats.buffers[d]+=nb;T.stats.images[d]+=ni;
    for(unsigned i=0;i<nm;++i)addscope(src,dst,m[i].srcAccessMask,m[i].dstAccessMask,flags,0);
    for(unsigned i=0;i<nb;++i)addscope(src,dst,b[i].srcAccessMask,b[i].dstAccessMask,flags,1);
    for(unsigned i=0;i<ni;++i){T.stats.transitions[d]+=im[i].oldLayout!=im[i].newLayout;addscope(src,dst,im[i].srcAccessMask,im[i].dstAccessMask,flags,2);}
}
void vk_sync_full_note(const char *file,unsigned line) { addloc(file,line,0); }
void vk_sync_full_at(VkCommandBuffer cb,const char *file,unsigned line) { vk_sync_full_note(file,line); (vk_img_barrier_all)(cb); }
void vk_sync_gs_mark(VkCommandBuffer cb,int raster_follows) {
    if(!g_vk_sync_active)return; ++T.stats.gs; vk_sync_gate_mark(&T.gate,(uintptr_t)cb,raster_follows);
}
void vk_sync_begin_barrier(VkCommandBuffer cb) {
    int had=T.gate.pending, ok=vk_sync_gate_take(&T.gate,(uintptr_t)cb);
    if(ok)++T.stats.eligible;else if(had)++T.stats.rejected;
    if(ok && coalesce){++T.stats.removed;return;}
    vk_sync_full_at(cb,"begin_pass",0);
}
void vk_sync_lifecycle(const char *file,unsigned line,int state) {
    if(!g_vk_sync_level)return;
    if(state==1){addloc(file,line,1);T.end_line=line;}else if(state==2)++T.stats.dropped_setups;else ++T.stats.empty_ends;
}
void vk_sync_frame(uint64_t frame) {
    vk_sync_init(); vk_sync_gate_command(&T.gate);
    if(g_vk_sync_level){
        T.stats.frame=frame;T.stats.variant=coalesce;
        unsigned w=atomic_load_explicit(&qwrite,memory_order_relaxed),r=atomic_load_explicit(&qread,memory_order_acquire);
        if(w-r<QUEUE){queue[w%QUEUE]=T.stats;atomic_store_explicit(&qwrite,w+1,memory_order_release);uint64_t drop=T.stats.queue_drops;memset(&T.stats,0,sizeof T.stats);T.stats.queue_drops=drop;}
        else ++T.stats.queue_drops;
    }
    const char *e=getenv("DM_VK_GS_BARRIER_COALESCE");coalesce=e && atoi(e);g_vk_sync_active=g_vk_sync_level || coalesce;
}
void vk_sync_pass_begin(const vk_sync_pass *p) { if(g_vk_sync_level<2)return;T.pass=*p;T.open=1; }
void vk_sync_pass_shader(const uint64_t keys[3],uint32_t variant,uint32_t topo) {
    if(g_vk_sync_level<2 || !T.open)return;
    for(unsigned i=0;i<T.pass.nshader;++i){vk_sync_shader *s=&T.pass.shader[i];if(!memcmp(s->key,keys,24)&&s->variant==variant&&s->topo==topo){++s->draws;return;}}
    if(T.pass.nshader==SYNC_SHADERS){T.pass.overflow|=1;return;}
    vk_sync_shader *s=&T.pass.shader[T.pass.nshader++];memcpy(s->key,keys,24);s->variant=variant;s->topo=topo;s->draws=1;
}
void vk_sync_pass_resource(uint64_t pool,uint64_t off,uint64_t size,uint32_t stage,uint32_t access,uint32_t kind) {
    if(g_vk_sync_level<2||!T.open||!size)return;
    vk_sync_resource r;memset(&r,0,sizeof r);r.pool=pool;r.off=off;r.size=size;r.stage=stage;r.access=access;r.kind=kind;
    for(unsigned i=0;i<T.pass.nresource;++i)if(!memcmp(&T.pass.resource[i],&r,sizeof r))return;
    if(T.pass.nresource==SYNC_RESOURCES){T.pass.overflow|=2;return;}
    T.pass.resource[T.pass.nresource++]=r;
}
uint32_t vk_sync_pass_end(void) {
    T.last_id=0; ++T.stats.passes;
    if(g_vk_sync_level<2||!T.open)return 0;T.open=0;T.pass.end_line=T.end_line;
    // Canonical order makes identities independent of pass ordinal and descriptor collection order.
    for(unsigned i=0;i<T.pass.nshader;++i)for(unsigned j=i+1;j<T.pass.nshader;++j)if(memcmp(&T.pass.shader[i],&T.pass.shader[j],sizeof(vk_sync_shader))>0){vk_sync_shader x=T.pass.shader[i];T.pass.shader[i]=T.pass.shader[j];T.pass.shader[j]=x;}
    for(unsigned i=0;i<T.pass.nresource;++i)for(unsigned j=i+1;j<T.pass.nresource;++j)if(memcmp(&T.pass.resource[i],&T.pass.resource[j],sizeof(vk_sync_resource))>0){vk_sync_resource x=T.pass.resource[i];T.pass.resource[i]=T.pass.resource[j];T.pass.resource[j]=x;}
    // Workload identity must not contain per-frame UBO/SSBO ring addresses.
    // Keep immutable representative resources and explicitly mark variation.
    vk_sync_pass key=T.pass;key.end_line=0;key.nresource=0;key.overflow&=1u;
    memset(key.resource,0,sizeof key.resource);
    for(unsigned a=0;a<SYNC_ATTACHMENTS;++a)key.att[a].pool=key.att[a].off=key.att[a].size=0;
    uint64_t h=hash_bytes(1469598103934665603ull,&key,sizeof key);unsigned start=(unsigned)h%GROUPS;
    for(unsigned k=0;k<GROUPS;++k){unsigned i=(start+k)%GROUPS;group *g=&groups[i];
        if(!atomic_load_explicit(&g->ready,memory_order_acquire)){
            g->meta=T.pass;g->key=key;g->hash=h;g->family=h;
            atomic_store_explicit(&g->ready,1,memory_order_release);++ngroup;
        }
        if(g->hash==h && !memcmp(&g->key,&key,sizeof key)){
            if(g->meta.nresource!=T.pass.nresource || memcmp(g->meta.resource,T.pass.resource,sizeof T.pass.resource) || memcmp(g->meta.att,T.pass.att,sizeof T.pass.att))atomic_store(&g->resource_varies,1);
            atomic_fetch_add(&g->passes,1);T.last_id=i+1;return i+1;
        }
    }
    ++T.stats.overflow;return 0;
}
uint32_t vk_sync_last_id(void){return T.last_id;}
void vk_sync_dispatch_end(void){vk_sync_pass_end();--T.stats.passes;}
void vk_sync_gpu(uint32_t id,uint64_t ns){if(!id||id>GROUPS)return;group *g=&groups[id-1];atomic_fetch_add(&g->gpu_ns,ns);atomic_fetch_add(&g->gpu_n,1);}
void vk_sync_query_overflow(void){++T.stats.query_overflow;}
