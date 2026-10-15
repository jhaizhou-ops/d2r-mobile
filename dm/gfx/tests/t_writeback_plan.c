#include "../nvn/nvn_vk_writeback_plan.h"
#include <stdio.h>
#include <string.h>

static uint32_t writeback_test_random(uint32_t *state) {
    *state^=*state<<13;*state^=*state>>17;*state^=*state<<5;return *state;
}
int writeback_plan_test(void) {
    NvnVkWritebackSpan fixed[]={{100,32,1},{112,16,1},{132,16,1},{100,32,1},
        {164,16,1},{180,16,2},{180,0,1}};
    size_t count=nvn_vk_writeback_plan(fixed,sizeof fixed/sizeof *fixed);
    if(count!=3 || fixed[0].addr!=100 || fixed[0].size!=48 ||
       fixed[1].addr!=164 || fixed[1].size!=16 || fixed[2].pool_serial!=2)return 1;
    NvnVkWritebackSpan boundary[]={{UINT64_MAX-16,16,7},{UINT64_MAX-8,8,7}};
    if(nvn_vk_writeback_plan(boundary,2)!=1 || boundary[0].size!=16)return 2;
    NvnVkWritebackSpan invalid={UINT64_MAX-1,2,1};
    if(nvn_vk_writeback_plan(&invalid,1)!=SIZE_MAX || nvn_vk_writeback_plan(NULL,1)!=SIZE_MAX ||
       nvn_vk_writeback_plan(NULL,0)!=0)return 3;
    uint32_t state=0x61ca47u;
    for(unsigned trial=0;trial<2000;trial++) {
        NvnVkWritebackSpan spans[64];unsigned char expected[3][512]={{0}},actual[3][512]={{0}};
        size_t n=writeback_test_random(&state)%65;
        for(size_t i=0;i<n;i++) {
            uint64_t addr=writeback_test_random(&state)%512;
            uint64_t size=writeback_test_random(&state)%((512-addr)<80?(512-addr)+1:81);
            uint64_t serial=writeback_test_random(&state)%3;
            spans[i]=(NvnVkWritebackSpan){addr,size,serial};
            memset(expected[serial]+addr,1,size);
        }
        size_t planned=nvn_vk_writeback_plan(spans,n);
        if(planned>n)return 4;
        for(size_t i=0;i<planned;i++) {
            NvnVkWritebackSpan span=spans[i];
            if(!span.size || span.pool_serial>=3 || span.addr+span.size>512)return 5;
            if(i && spans[i-1].pool_serial==span.pool_serial &&
               spans[i-1].addr+spans[i-1].size>=span.addr)return 6;
            for(uint64_t j=span.addr;j<span.addr+span.size;j++) {
                if(actual[span.pool_serial][j]++)return 7;
            }
        }
        if(memcmp(expected,actual,sizeof expected))return 8;
    }
    fprintf(stderr,"WRITEBACK_PLAN_TEST randomized_cases=2000 pool_identity=1 exact_union=1 gap_preserved=1 overflow_rejected=1 passed=1\n");
    return 0;
}
#ifndef DM_WRITEBACK_TEST_EMBEDDED
int main(void) {return writeback_plan_test();}
#endif
