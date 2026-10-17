#include "memory_layout.h"
int xblr_free_region(uint64_t address,const xblr_address_range *regions,size_t count,
    uint64_t *base,uint64_t *size,uint64_t *state) {
    const uint64_t limit=1ull<<39;
    for(size_t i=0;i<count;++i)
        if(regions[i].base>=limit||regions[i].size>limit-regions[i].base)return -1;
    if(address>=limit){*base=limit;*size=0-limit;*state=0x10;return 1;}
    uint64_t low=0,high=limit;
    for(size_t i=0;i<count;++i) {
        uint64_t b=regions[i].base,n=regions[i].size;
        if(!n)continue;
        if(address>=b&&address-b<n)return 0;
        if(b>address&&b<high)high=b;
        if(b+n<=address&&b+n>low)low=b+n;
    }
    *base=low;*size=high-low;*state=0;return 1;
}
