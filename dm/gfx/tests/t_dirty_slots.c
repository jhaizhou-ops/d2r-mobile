#include "../nvn/nvn_dirty.h"
#include "../nvn/nvn_vk_pool_upload.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-52s 期望 %-4lld → 真结果 %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static void *page_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

static void *page_map_n(size_t pages) {
    void *p = mmap(NULL, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

int main(void) {
    crash_install();

    uint8_t guest[512], mapped[512];
    for (size_t i = 0; i < sizeof guest; i++) guest[i] = (uint8_t)(i * 3 + 1);
    memset(mapped, 0xcd, sizeof mapped);
    uint64_t base = (uint64_t)(uintptr_t)guest;
    PoolUploadRange writes[] = {{base + 90, 20}, {base + 140, 20}};
    uint64_t copied = nvn_vk_pool_copy_guest_span(mapped, base, 13, base + 100, 100,
                                                   writes, sizeof writes / sizeof writes[0]);
    EXPECT("脏页镜像只复制非 GPU 所有区间字节", 70, copied);
    int copy_exact = 1;
    for (size_t i = 0; i < sizeof mapped; i++) {
        int expected_copy = i >= 113 && i < 213;
        size_t source = expected_copy ? i - 13 : 0;
        int gpu_owned = expected_copy && ((source >= 100 && source < 110) ||
                                         (source >= 140 && source < 160));
        uint8_t expected = expected_copy && !gpu_owned ? guest[source] : 0xcd;
        if (mapped[i] != expected) copy_exact = 0;
    }
    EXPECT("上传镜像偏移和 GPU 写入保护逐字节正确", 1, copy_exact);

    for (int i = 0; i < 65; i++) {
        void *p = page_map();
        nvn_dirty_add_range(p, 4096);
        nvn_dirty_remove_range(p);
        munmap(p, 4096);
    }

    void *reused = page_map();
    nvn_dirty_add_range(reused, 4096);
    nvn_dirty_protect(reused, 4096);
    uint64_t stamp = nvn_dirty_now();
    ((volatile uint8_t *)reused)[0] = 1;
    EXPECT("累计 65 次销池后新池写入被判脏", 1, nvn_dirty_since(reused, 4096, stamp));
    nvn_dirty_remove_range(reused);
    munmap(reused, 4096);

    void *live[300];
    for (int i = 0; i < 300; i++) {
        live[i] = page_map();
        nvn_dirty_add_range(live[i], 4096);
    }
    void *overflow = live[299];
    nvn_dirty_protect(overflow, 4096);
    stamp = nvn_dirty_now();
    ((volatile uint8_t *)overflow)[0] = 2;
    EXPECT("同时存活超过上限的池保守判脏", 1, nvn_dirty_since(overflow, 4096, stamp));
    for (int i = 0; i < 300; i++) {
        nvn_dirty_remove_range(live[i]);
        munmap(live[i], 4096);
    }

    /* A contiguous pre-write range must unprotect in one syscall while
     * preserving per-page generations and leaving adjacent pages protected. */
    const size_t pages = 16, first = 4, count = 8;
    uint8_t *batch = page_map_n(pages);
    nvn_dirty_add_range(batch, pages * 4096);
    nvn_dirty_protect(batch, pages * 4096);
    stamp = nvn_dirty_now();
    uint64_t mp_calls0, mp_pages0, mp_ns0, mp_calls1, mp_pages1, mp_ns1;
    uint64_t faults0, preps0, faults1, preps1;
    nvn_dirty_mp_stats(&mp_calls0, &mp_pages0, &mp_ns0);
    nvn_dirty_stats(&faults0, &preps0);
    dm_guest_write_prepare(batch + first * 4096, count * 4096);
    nvn_dirty_mp_stats(&mp_calls1, &mp_pages1, &mp_ns1);
    nvn_dirty_stats(&faults1, &preps1);
    EXPECT("连续脏页预解保护只调用一次 mprotect", 1, mp_calls1 - mp_calls0);
    EXPECT("批量预解保护覆盖准确页数", count, mp_pages1 - mp_pages0);
    EXPECT("批量预解保护逐页分配写代号", count, preps1 - preps0);
    EXPECT("批量预解保护不触发写缺页", 0, faults1 - faults0);
    size_t dirty_cursor = 0, dirty_offset = 0, dirty_length = 0;
    EXPECT("脏页迭代找到连续页段", 1,
           nvn_dirty_next_range(batch, pages * 4096, stamp, &dirty_cursor,
                                &dirty_offset, &dirty_length));
    EXPECT("脏页段起点保持页内偏移", first * 4096, dirty_offset);
    EXPECT("脏页段长度保持精确", count * 4096, dirty_length);
    EXPECT("脏页段之后没有额外范围", 0,
           nvn_dirty_next_range(batch, pages * 4096, stamp, &dirty_cursor,
                                &dirty_offset, &dirty_length));
    size_t clipped_cursor = 0, clipped_offset = 0, clipped_length = 0;
    const size_t clipped_start = (first - 1) * 4096 + 128;
    const size_t clipped_size = (count + 1) * 4096 - 64;
    EXPECT("未对齐绑定区间只返回相交脏页", 1,
           nvn_dirty_next_range(batch + clipped_start, clipped_size, stamp,
                                &clipped_cursor, &clipped_offset, &clipped_length));
    EXPECT("未对齐脏段偏移精确裁剪", 4096 - 128, clipped_offset);
    EXPECT("未对齐脏段长度不越过绑定区间", count * 4096, clipped_length);
    for (size_t i = 0; i < pages; i++) {
        char name[96];
        snprintf(name, sizeof name, "批量预解保护第 %zu 页脏状态", i);
        EXPECT(name, i >= first && i < first + count,
               nvn_dirty_since(batch + i * 4096, 4096, stamp));
    }
    uint64_t guard_faults0, guard_preps0, guard_faults1, guard_preps1;
    nvn_dirty_stats(&guard_faults0, &guard_preps0);
    batch[(first - 1) * 4096] = 0x5a;
    nvn_dirty_stats(&guard_faults1, &guard_preps1);
    EXPECT("批量范围外的保护页仍由真实写缺页解保护", 1, guard_faults1 - guard_faults0);
    EXPECT("批量范围外写入仍被逐页标脏", 1,
           nvn_dirty_since(batch + (first - 1) * 4096, 4096, stamp));
    EXPECT("批量预解保护后边界页写入未被改写", 0x5a,
           batch[(first - 1) * 4096]);
    dirty_cursor = 0;
    EXPECT("相邻脏页合并为单个连续范围", 1,
           nvn_dirty_next_range(batch, pages * 4096, stamp, &dirty_cursor,
                                &dirty_offset, &dirty_length));
    EXPECT("合并范围包含新缺页页", (first - 1) * 4096, dirty_offset);
    EXPECT("合并范围精确覆盖相邻脏页", (count + 1) * 4096, dirty_length);
    uint8_t untracked[7] = {0};
    dirty_cursor = 0;
    EXPECT("未跟踪内存保守返回全范围", 1,
           nvn_dirty_next_range(untracked, sizeof untracked, stamp, &dirty_cursor,
                                &dirty_offset, &dirty_length));
    EXPECT("未跟踪范围从零开始", 0, dirty_offset);
    EXPECT("未跟踪范围不漏字节", sizeof untracked, dirty_length);
    nvn_dirty_remove_range(batch);
    munmap(batch, pages * 4096);
    return g_fail != 0;
}
