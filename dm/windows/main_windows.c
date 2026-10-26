// Windows 入口把命令行转换成 UTF-8，再交给同一公共宿主；不分发任何游戏资源。
// Windows entry converts the command line to UTF-8 and invokes the shared host; no game resources are distributed.
#include "dm_windows.h"
#include "performance.h"
#include <shellapi.h>
#include <locale.h>
int dm_main(int argc, char **argv);
int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    if (!setlocale(LC_ALL, ".UTF8")) { fprintf(stderr, "[dm] UTF-8 locale unavailable\n"); return 1; }
    _set_fmode(_O_BINARY);
    if (dm_windows_setenv("DM_WINDOW", "1", 0) || dm_windows_setenv("DM_AUDIO", "1", 0) ||
        dm_windows_setenv("DM_APP", "1", 0)) return 1;
    if (dm_windows_performance_defaults()) return 1;
    int argc;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wide) { fprintf(stderr, "[dm] CommandLineToArgvW failed\n"); return 1; }
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    if (!argv) { LocalFree(wide); return 1; }
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        argv[i] = n ? malloc((size_t)n) : NULL;
        if (!argv[i]) { fprintf(stderr, "[dm] command-line conversion failed\n"); abort(); }
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], n, NULL, NULL);
    }
    LocalFree(wide);
    int result = dm_main(argc, argv);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv); return result;
}
