// 诊断直接读写 Windows CONTEXT；不假定 Linux mcontext 的布局。
// Diagnostics read and write Windows CONTEXT directly, without assuming Linux mcontext layout.
#pragma once
#include <stdint.h>
#include <windows.h>
typedef struct { CONTEXT *context; uint64_t esr; } ucontext_t;
