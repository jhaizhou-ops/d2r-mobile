// 动态符号查询由 Windows 加载器提供；dladdr 当前只报告映像基址，不报告符号名。
// Dynamic symbol lookup uses the Windows loader; dladdr currently reports image bases, not symbol names.
#pragma once
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void *)0)
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
#ifdef __cplusplus
extern "C" {
#endif
void *dlopen(const char *path, int flags);
void *dlsym(void *library, const char *symbol);
int dlclose(void *library);
char *dlerror(void);
int dladdr(const void *address, Dl_info *info);
#ifdef __cplusplus
}
#endif
