// 对齐分配必须匹配释放器：UCRT 的 _aligned_malloc 不能交给普通 free。
// Aligned allocations must use matching deallocators: UCRT _aligned_malloc cannot be passed to ordinary free.
#pragma once
#include <stdlib.h>
#ifdef _WIN32
#include <malloc.h>
static inline void *dm_aligned_alloc(size_t alignment, size_t size) { return _aligned_malloc(size, alignment); }
static inline void dm_aligned_free(void *memory) { _aligned_free(memory); }
#else
static inline void *dm_aligned_alloc(size_t alignment, size_t size) { return aligned_alloc(alignment, size); }
static inline void dm_aligned_free(void *memory) { free(memory); }
#endif
