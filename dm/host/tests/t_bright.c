// 默认曝光回归：洞穴、室外、读取画面都保持原版亮度；只有合法的显式诊断值才能加亮。
// Exposure regression: caves, outdoors and loading screens retain original brightness; only a valid explicit diagnostic override boosts it.
#include "../dm.h"
#include "../dm_hud.h"
#include <sys/wait.h>
#include <unistd.h>

float dm_bright_k(void);
module_t g_mod[MAX_MOD];
int g_nmod;
static int area, loading;
uint64_t now_ns(void) { static uint64_t t; return t += 34000000; }
int dm_uiscreen_ingame(void) { return 1; }
int dm_loadscreen_up(void) { return loading; }
void dm_hud_get(dm_hud_info *out) { *out = (dm_hud_info){ .valid = 1, .area = area }; }

int main(void) {
    const struct { const char *value; float expected; } cases[] = {
        { NULL, 1 }, { "1", 1 }, { "2.5", 2.5f }, { "4", 4 }, { "2e0", 2 },
        { "", 1 }, { "0", 1 }, { "-1", 1 }, { "0.99", 1 }, { "4.0000001", 1 }, { "5", 1 },
        { "nan", 1 }, { "inf", 1 }, { "-inf", 1 }, { "1e999", 1 }, { "1e-999", 1 },
        { "2junk", 1 }, { "2 3", 1 }
    };
    const int areas[] = { 8, 12, 1, 109, 120, 0 };   // 洞穴、地穴、城镇、山顶、未知区域 | cave, pit, towns, summit, unknown area
    int fails = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (!pid) {
            if (cases[i].value) setenv("DM_BRIGHT_K", cases[i].value, 1); else unsetenv("DM_BRIGHT_K");
            for (unsigned frame = 0; frame < 180; frame++) {
                area = areas[(frame / 30) % (sizeof areas / sizeof *areas)]; loading = frame % 30 == 29;
                float actual = dm_bright_k();
                if (actual != cases[i].expected) {
                    fprintf(stderr, "DM_BRIGHT_K=%s area=%d loading=%d: expected %g, got %g\n",
                        cases[i].value ? cases[i].value : "(unset)", area, loading, cases[i].expected, actual);
                    _exit(1);
                }
            }
            _exit(0);
        }
        int status;
        if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) fails++;
    }
    printf("t_bright: %zu cases, %d failures\n", sizeof cases / sizeof *cases, fails);
    return fails != 0;
}
