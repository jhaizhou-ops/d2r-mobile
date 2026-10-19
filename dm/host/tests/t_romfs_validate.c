#include "../romfs_validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int check(int ok, const char *s) { if (!ok) fprintf(stderr, "失败：%s\n", s); return ok; }
int main(void) {
    // Las aserciones históricas comparan mensajes chinos: fijar el idioma evita depender del Mac.
    setenv("DM_LANG", "zh-Hans", 1);
    const char *src = getenv("DM_ROMFS"); if (!src) src = getenv("HOME");
    if (!src) return 1;
    char cmd[4096], why[512], rep[2048], sd[4096]; snprintf(sd, sizeof sd, "%s/data", src);
    if (!check(dm_romfs_validate(sd, why, sizeof why), "原版 RomFS 合格")) { fprintf(stderr, "%s\n", why); return 1; }
    if (!check(dm_romfs_report(sd, rep, sizeof rep) == 0, "完整清单：原版 RomFS 无缺")) return 1;
    {   // 空目录：18 个档案一个都没有。期望值取自 romfs_validate.c 的 need / size 两张表：00-00000001.data 1073741824 字节 = 1.00 GB、17-00000002.data 4496514 字节 = 4 MB | Empty directory: none of the 18 archives is present. Expected values come from the need / size tables in romfs_validate.c: 00-00000001.data 1073741824 bytes = 1.00 GB, 17-00000002.data 4496514 bytes = 4 MB
        char r1[] = "/tmp/dm-romfs-test.XXXXXX"; if (!mkdtemp(r1)) return 1;
        if (!check(!dm_romfs_validate(r1, why, sizeof why) && !strcmp(why, "缺少 00-00000001.data"), "空目录被拒，报第一个档案")) { fprintf(stderr, "%s\n", why); return 1; }
        int bad = dm_romfs_report(r1, rep, sizeof rep);
        if (!check(bad == 18 && strstr(rep, "已有 0 / 18") && strstr(rep, "00-00000001.data（1.00 GB）") && strstr(rep, "17-00000002.data（4 MB）") && !strstr(rep, "没拷完"), "完整清单：缺 18 个逐个列出")) { fprintf(stderr, "%s\n", rep); return 1; }
        setenv("DM_LANG", "es-ES", 1);
        if (!check(!dm_romfs_validate(r1, why, sizeof why) && !strcmp(why, "Falta 00-00000001.data"), "El rechazo español conserva el nombre exacto del archivo")) { fprintf(stderr, "%s\n", why); return 1; }
        bad = dm_romfs_report(r1, rep, sizeof rep);
        if (!check(bad == 20 && strstr(rep, "0 / 20 archivos presentes.") && strstr(rep, "00-00000001.data (1.00 GB)") && strstr(rep, "07-00000002.data") && !strstr(rep, "Incompletos:"), "La lista española incluye los dos archivos de voces")) { fprintf(stderr, "%s\n", rep); return 1; }
        setenv("DM_LANG", "zh-Hans", 1);
        // 配置、密钥、清单这些小文件不在核对范围里：放一份内容不对的进去，结论与给玩家的清单都不变，清单里不出现它们的名字
        // Small files such as the config, key and manifests are outside the scope of the check: put in a copy with wrong contents, and neither the verdict nor
        // the list shown to the player changes, and their names do not appear in the list
        snprintf(cmd, sizeof cmd, "printf bad > '%s/.build.config' && printf x > '%s/build_key.txt' && printf x > '%s/archive_manifest.psv'", r1, r1, r1); if (system(cmd)) return 1;
        bad = dm_romfs_report(r1, rep, sizeof rep);
        if (!check(bad == 18 && !strstr(rep, "build") && !strstr(rep, "psv"), "完整清单：不提小文件")) { fprintf(stderr, "%s\n", rep); return 1; }
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", r1); system(cmd);
    }
    {   // 只有档案文件（没有配置、密钥、任何清单）就合格；再核字节数与剔零档案：用符号链接搭一份，把一个小档案截短 / 换成「剔零 + 区段表」 | Archive files alone (no config, key or any manifest) are enough to pass; then check byte counts and zero-stripped archives: build a copy with symlinks, then truncate one small archive / replace it with "zero-stripped + extent table"
        char r2[] = "/tmp/dm-romfs-size.XXXXXX"; if (!mkdtemp(r2)) return 1;
        snprintf(cmd, sizeof cmd, "for f in '%s'/data/*.data; do ln -s \"$f\" '%s/' || exit 1; done", src, r2); if (system(cmd)) return 1;
        if (!check(dm_romfs_validate(r2, why, sizeof why), "只有档案文件的目录合格")) { fprintf(stderr, "%s\n", why); return 1; }
        for (int variant = 0; variant < 2; variant++) {
            setenv("DM_LANG", variant ? "es-MX" : "es-ES", 1);
            const char *name = variant ? "13-00000001.data" : "07-00000001.data";
            char link[4096], original[4096];
            snprintf(link, sizeof link, "%s/%s", r2, name);
            snprintf(original, sizeof original, "%s/data/%s", src, name);
            if (!check(dm_romfs_validate(r2, why, sizeof why), "Los archivos españoles íntegros se aceptan")) return 1;
            if (unlink(link)) return 1;
            if (!check(!dm_romfs_validate(r2, why, sizeof why) && strstr(why, name), "Se rechaza la ausencia de la voz española")) return 1;
            if (!check(dm_romfs_report(r2, rep, sizeof rep) == 1 && strstr(rep, name), "El informe identifica la voz española que falta")) return 1;
            FILE *truncated = fopen(link, "wb"); if (!truncated) return 1;
            fputc(0, truncated); fclose(truncated);
            if (!check(!dm_romfs_validate(r2, why, sizeof why) && strstr(why, name), "Se rechaza el archivo de voz español truncado")) return 1;
            if (unlink(link) || symlink(original, link)) return 1;
        }
        setenv("DM_LANG", "zh-Hans", 1);
        snprintf(cmd, sizeof cmd, "rm '%s/04-00000002.data' && head -c 1000000 '%s/data/04-00000002.data' > '%s/04-00000002.data'", r2, src, r2); if (system(cmd)) return 1;
        if (!check(!dm_romfs_validate(r2, why, sizeof why) && strstr(why, "还没拷完"), "拷到一半的档案被拒")) { fprintf(stderr, "%s\n", why); return 1; }
        int bad = dm_romfs_report(r2, rep, sizeof rep);
        if (!check(bad == 1 && strstr(rep, "已有 17 / 18") && strstr(rep, "没拷完：04-00000002.data（1 MB / 9 MB）"), "完整清单：没拷完的单列，带现有 / 应有（1000000 字节 = 1 MB、8915855 字节 = 9 MB，按 1048576 折算取整）")) { fprintf(stderr, "%s\n", rep); return 1; }
        snprintf(cmd, sizeof cmd, "printf 'size 8915855\\n1000000 7915855\\n' > '%s/04-00000002.data.holes'", r2); if (system(cmd)) return 1;
        if (!check(dm_romfs_validate(r2, why, sizeof why), "剔零档案（前 1000000 字节 + 区段表）合格")) { fprintf(stderr, "%s\n", why); return 1; }
        snprintf(cmd, sizeof cmd, "printf 'size 8915855\\n1000000 100\\n' > '%s/04-00000002.data.holes'", r2); if (system(cmd)) return 1;
        if (!check(!dm_romfs_validate(r2, why, sizeof why) && strstr(why, "还没拷完"), "区段表与实际长度对不上被拒")) { fprintf(stderr, "%s\n", why); return 1; }
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", r2); system(cmd);
    }
    return 0;
}
