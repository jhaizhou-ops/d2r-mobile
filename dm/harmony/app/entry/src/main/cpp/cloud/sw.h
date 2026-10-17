/* HarmonyOS shim for the iOS host header used by sw_cloudsync.c (copied unchanged from upstream sw/host). */
#pragma once
#include <stdio.h>
#include <string.h>
#define LOG(...) do { fprintf(stderr, "[cloud] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define st_mtimespec st_mtim
