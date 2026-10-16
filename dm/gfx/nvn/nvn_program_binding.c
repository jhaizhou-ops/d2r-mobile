#include "nvn_program_binding.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static pthread_mutex_t binding_mutex = PTHREAD_MUTEX_INITIALIZER;
static nvn_program_binding *bindings[4096];
static unsigned binding_count;

static void binding_error(const char *message) {
    fprintf(stderr, "NVN program binding: %s\n", message);
    abort();
}

const prog_t *nvn_program_bind(const prog_t *current, const prog_t *source, unsigned mask) {
    if (mask & ~63u) binding_error("invalid stage mask");
    if (current && current->magic != NVN_BINDING_MAGIC) binding_error("invalid previous state");
    nvn_program_binding key = {0};
    if (current) {
        const nvn_program_binding *old = (const nvn_program_binding *)current;
        memcpy(key.source, old->source, sizeof key.source);
        memcpy(key.source_serial, old->source_serial, sizeof key.source_serial);
    }
    unsigned active = 0;
    uint64_t hash = 1469598103934665603ull;
    for (unsigned stage = 0; stage < 6; ++stage) {
        if (mask & (1u << stage)) {
            key.source[stage] = source;
            key.source_serial[stage] = source ? source->serial : 0;
        }
        if (key.source[stage]) active++;
        hash = (hash ^ (uintptr_t)key.source[stage]) * 1099511628211ull;
        hash = (hash ^ key.source_serial[stage]) * 1099511628211ull;
    }
    if (!active) return NULL;
    unsigned bucket = hash & 4095;
    pthread_mutex_lock(&binding_mutex);
    for (nvn_program_binding *b = bindings[bucket]; b; b = b->next) {
        if (!memcmp(b->source, key.source, sizeof key.source) &&
            !memcmp(b->source_serial, key.source_serial, sizeof key.source_serial)) {
            pthread_mutex_unlock(&binding_mutex);
            return &b->program;
        }
    }
    // 预扫描会保留状态指针，因此组合活到进程结束；明确限制增长，不淘汰仍在使用的快照。
    // Pre-scans retain state pointers, so entries live for the process lifetime.
    // Bound growth explicitly rather than evicting a still-referenced snapshot.
    if (binding_count == 131072) binding_error("combination cache exhausted");
    nvn_program_binding *b = malloc(sizeof *b);
    if (!b) binding_error("allocation failed");
    *b = key;
    b->program.magic = NVN_BINDING_MAGIC;
    b->program.serial = nvn_new_serial();
    b->next = bindings[bucket]; bindings[bucket] = b; binding_count++;
    pthread_mutex_unlock(&binding_mutex);
    return &b->program;
}
