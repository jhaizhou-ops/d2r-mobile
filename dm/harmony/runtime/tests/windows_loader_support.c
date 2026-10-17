// Freestanding Windows test adapter only. Same loader C runs against real images;
// this bounded arena replaces libc allocation, never guest functions or HLE.
#include <stddef.h>
#include <stdint.h>
static unsigned char arena[16*1024*1024];
static size_t used;
void *calloc(size_t count,size_t size) {
    if(size && count>SIZE_MAX/size)return NULL;
    size_t bytes=count*size;
    size_t off=(used+15)&~(size_t)15;
    if(off>sizeof arena||bytes>sizeof arena-off)return NULL;
    unsigned char *result=arena+off;for(size_t i=0;i<bytes;++i)result[i]=0;
    used=off+bytes;return result;
}
void free(void *ptr) { (void)ptr; }
void *memchr(const void *src,int value,size_t n) {
    const unsigned char *s=src;while(n--) { if(*s==(unsigned char)value)return (void*)s;++s; }return NULL;
}
int memcmp(const void *a,const void *b,size_t n) {
    const unsigned char *p=a,*q=b;while(n--) { if(*p!=*q)return *p-*q;++p;++q; }return 0;
}
int strcmp(const char *a,const char *b) {
    while(*a && *a==*b) { ++a;++b; }return (unsigned char)*a-(unsigned char)*b;
}
