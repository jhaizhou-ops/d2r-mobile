// COFF 的 $a/$m/$z 排序界定公共登记表；整个宿主静态库一起链接。
// COFF $a/$m/$z ordering delimits shared registration tables; the host static library links whole.
#include <stdint.h>
#define BOUNDS(name) \
    __attribute__((used, section(#name "$a"), aligned(8))) const uint64_t dm_section_##name##_start = 0; \
    __attribute__((used, section(#name "$z"), aligned(8))) const uint64_t dm_section_##name##_end = 0
BOUNDS(swhle);
BOUNDS(swforce);
BOUNDS(swnvn);
