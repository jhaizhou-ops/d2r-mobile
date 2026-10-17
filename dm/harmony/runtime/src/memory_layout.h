#ifndef XBLR_MEMORY_LAYOUT_H
#define XBLR_MEMORY_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
typedef struct xblr_address_range {uint64_t base,size;} xblr_address_range;
/* Horizon's 39-bit user VA. Outside it QueryMemory returns Inaccessible,
 * including an unsigned size extending to the end of the 64-bit address space. */
int xblr_free_region(uint64_t address,const xblr_address_range *regions,size_t count,
    uint64_t *base,uint64_t *size,uint64_t *state);
#endif
