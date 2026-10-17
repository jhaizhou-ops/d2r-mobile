// Only supports the native Windows test DLL for the same input_gate.c; no CRT dependency.
// This does not compile or emulate the AArch64 guest/runtime on Windows.
#include <stddef.h>
void *memset(void *dst, int byte, size_t count)
{ unsigned char *d = dst; while (count--) *d++ = (unsigned char)byte; return dst; }
void *memcpy(void *dst, const void *src, size_t count)
{ unsigned char *d = dst; const unsigned char *s = src; while (count--) *d++ = *s++; return dst; }
