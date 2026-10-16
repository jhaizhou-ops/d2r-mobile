#pragma once
#include "nvn_obj.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct { tex_t t; int ok; view_t v; int has_v; void *view; uint64_t view_id, view_rid, view_ser, view_dirty; } treg_t;
struct nvn_descriptor_pool {
    int capacity, sampler;
    treg_t *textures;
    _Atomic uint64_t *serials;
    samp_par *samplers;
    uint8_t *sampler_ok;
    void **sampler_views;
    uint64_t *sampler_rids;
    struct nvn_descriptor_pool *next;
};

// 调用方须先释放 Metal 引用，再清空 CPU 存储。
// Metal references must be released by the caller before clearing CPU storage.
static inline void nvn_descriptors_clear(nvn_descriptor_pool *p) {
    if (!p) return;
    p->capacity = 0;
    free(p->textures); p->textures = NULL;
    free(p->serials); p->serials = NULL;
    free(p->samplers); p->samplers = NULL;
    free(p->sampler_ok); p->sampler_ok = NULL;
    free(p->sampler_views); p->sampler_views = NULL;
    free(p->sampler_rids); p->sampler_rids = NULL;
}

static inline nvn_descriptor_pool *nvn_descriptors_create(int capacity, int sampler) {
    if (capacity <= 0 || capacity > (1 << 20)) return NULL;
    nvn_descriptor_pool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->capacity = capacity; p->sampler = sampler;
    if (sampler) {
        p->samplers = calloc(capacity, sizeof *p->samplers);
        p->sampler_ok = calloc(capacity, sizeof *p->sampler_ok);
        p->sampler_views = calloc(capacity, sizeof *p->sampler_views);
        p->sampler_rids = calloc(capacity, sizeof *p->sampler_rids);
        if (p->samplers && p->sampler_ok && p->sampler_views && p->sampler_rids) return p;
    } else {
        p->textures = calloc(capacity, sizeof *p->textures);
        p->serials = calloc(capacity, sizeof *p->serials);
        if (p->textures && p->serials) {
            for (int i = 0; i < capacity; i++) atomic_init(&p->serials[i], 0);
            return p;
        }
    }
    nvn_descriptors_clear(p); free(p); return NULL;
}

static inline treg_t *nvn_descriptor_texture(nvn_descriptor_pool *p, int id) {
    return p && !p->sampler && id >= 0 && id < p->capacity ? &p->textures[id] : NULL;
}

static inline void nvn_descriptor_write_texture(nvn_descriptor_pool *p, int id, const tex_t *t, const view_t *v) {
    treg_t *r = nvn_descriptor_texture(p, id);
    if (!r) return;
    r->ok = t != NULL; if (t) r->t = *t;
    r->has_v = v != NULL; if (v) r->v = *v;
    atomic_store_explicit(&p->serials[id], t ? t->serial : 0, memory_order_release);
}

static inline uint64_t nvn_descriptor_serial(const nvn_descriptor_pool *p, uint64_t handle) {
    unsigned id = (unsigned)(handle & 0xfffff);
    return p && !p->sampler && id && id < (unsigned)p->capacity
        ? atomic_load_explicit(&p->serials[id], memory_order_relaxed) : 0;
}

static inline int nvn_descriptor_write_sampler(nvn_descriptor_pool *p, int id, const samp_par *s) {
    if (!p || !p->sampler || id < 0 || id >= p->capacity || !s) return 0;
    p->samplers[id] = *s; p->sampler_ok[id] = 1;
    return 1;
}
