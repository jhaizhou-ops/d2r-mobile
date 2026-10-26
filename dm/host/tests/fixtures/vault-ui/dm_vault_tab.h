// 仅供控件测试的合成表，不含游戏条目或生成资源。
// Synthetic widget-test table, containing no game entries or generated resources.
#include <stdint.h>
#define VT_NPART 1
static const char *vt_part_name[VT_NPART] = {"test-part"};
typedef struct { uint8_t kind, part, tier; uint16_t row, lvl; int16_t set; const char *key, *icon; } vt_row;
static const vt_row vt_rows[] = {{0, 0, 0, 0, 0, 0, "test-item", "test-icon"}};
#define VT_NROW 1
static const char *vt_set_key[] = {"test-set"};
#define VT_NSET 1
