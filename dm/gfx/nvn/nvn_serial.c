#include "nvn_obj.h"
#include <stdatomic.h>

// 客体对象与宿主组合共用序号空间，避免回放缓存键冲突。
// Guest objects and host combinations share a serial space to avoid replay-cache key collisions.
uint64_t nvn_new_serial(void) { static _Atomic uint64_t n; return ++n; }
