// 提供提取桥接所需的安静日志定义；依据 web/toolchain/nsp-extract-spike 的模块与调用约定。
// Provide quiet logging definitions for the extraction bridge using the module and calling
// contracts in web/toolchain/nsp-extract-spike.
#ifndef NSP_QUIET_H
#define NSP_QUIET_H
/* 在格式化由密钥派生的参数之前，将上游诊断编译掉。 */
/* Compile upstream diagnostics out BEFORE formatting key-derived arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define printf(...) ((int)0)
#define fprintf(...) ((int)0)
void nsp_zero(void *ptr, size_t size);
void *nsp_resize_keys(void *ptr, size_t old_size, size_t new_size);
#endif
