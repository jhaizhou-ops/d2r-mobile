// 普通文字框里的注释颜色码。
// 物品注释（dm/ios/tools/annot_strings.py）把带颜色码的注释写进了原版名字键：说明框（TooltipsPanel）、聊天（ChatListWidget）认颜色码，
// 文字框 TextBoxWidget 只有样式里设了 pipeCharacterBehavior: handle 才认（聊天区那几行）；不设的（编年史列表等）把 U+E07E 原样画成「*5」、
// 换行画成空格（游戏加载时 ÿc 已换成私用区字符 U+E07E；编年史列表真机实测）。
// 生成器同时出一张对照表（覆盖层变体目录根上的 dm_textbox_plain.tsv，规则见 dm_textbox_core.c）。这里挂在文字框「文字变了」的入口
// （main+0xadc434，325 个调用点；观测门 14，x0 = 控件，原文在 +0x88 / 长 +0x90，原函数随后按 +0x88 重排）：按表改写原文。
// 注释开关关着时覆盖层里没有这张表，门直接返回，所有文字框原样。只在游戏主线程（界面线程）上触发。
// Annotation color codes in ordinary text boxes.
// Item annotations (dm/ios/tools/annot_strings.py) wrote annotations with color codes into the original game's name keys: tooltips (TooltipsPanel) and chat (ChatListWidget) understand color codes,
// while the text box TextBoxWidget only does when its style sets pipeCharacterBehavior: handle (the chat area lines); those without it (chronicle list etc.) draw U+E07E literally as "*5" and
// draw newlines as spaces (at load time the game has already replaced ÿc with the private-use character U+E07E; measured on a real device in the chronicle list).
// The generator also emits a mapping table (dm_textbox_plain.tsv at the root of the overlay variant directory; rules in dm_textbox_core.c). Here we hook the text box "text changed" entry
// (main+0xadc434, 325 call sites; observation gate 14, x0 = widget, source text at +0x88 / length +0x90, the original function then re-lays out from +0x88): rewrite the source text by the table.
// With the annotation switch off the overlay has no such table, the gate returns immediately, and all text boxes stay as is. Fires only on the game main thread (UI thread).
#include "hle.h"
#include "dm.h"
#include "dm_textbox_core.h"
#include "dm_cursor.h"
#include "dm_i18n.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_loaded;   // 0 没读，1 有表，-1 没有表 | 0 not read, 1 table present, -1 no table
static uint8_t *g_b;
static uint64_t g_calls, g_hits, g_ns, g_max_ns, g_t0;
static int g_collection_loaded;

static int collection_scope(uint64_t widget,uint64_t root,int pc,int spanish) {
    if(!pc||!spanish||!widget||!root)return 0;
    const char *name=*(const char **)(widget+8);
    if(!name||strcmp(name,"ItemSlotText"))return 0;
    int panel=0;
    for(int depth=0;widget && depth<32;depth++) {
        if(widget==root)return panel;
        if(!*(const uint8_t *)(widget+0x50))return 0;
        name=*(const char **)(widget+8);
        if(name && (!strcmp(name,"ChronicleUniquesListPanel") || !strcmp(name,"ChronicleSetsListPanel") || !strcmp(name,"ChronicleRunewordsListPanel")))panel=1;
        uint64_t parent=*(const uint64_t *)(widget+0x30);if(parent==widget)return 0;widget=parent;
    }
    return 0;
}
static int collection_ensure_loaded(void) {
    if(!g_collection_loaded){char path[1200];int count=dm_overlay_file("dm_collection_names.tsv",path,sizeof path)?dm_collection_names_load(path):-1;g_collection_loaded=count>0?1:-1;
        LOG("Colección PC: mapa de presentación %s (%d entradas)",count>0?"cargado":"ausente",count>0?count:0);}
    return g_collection_loaded>0;
}

static void load(void) {
    g_loaded = -1; const char *ov = getenv("DM_ROMFS_OVERLAY"); if (!ov) { LOG("文字框颜色码：没有覆盖层，不处理"); return; }
    char path[1200]; int n = dm_overlay_file("dm_textbox_plain.tsv", path, sizeof path) ? dm_textbox_load(path) : -1;
    if (n <= 0) { LOG("文字框颜色码：覆盖层里没有对照表（物品注释关着），不处理"); return; }
    g_loaded = 1; LOG("文字框颜色码：读入对照表 %d 条（%s）", n, path);
}
int dm_textbox_ensure_loaded(void) { if (!g_loaded) load(); return g_loaded > 0; }   // 不经门直接用对照表的（符文之语清单等）先调它：门不挂（DM_TEXTBOX_OFF、rules 产物）或还没触发过时表也读得到；读过直接返回 | Users of the mapping table that do not go through the gate (runeword list etc.) call this first: the table can be read even when the gate is not hooked (DM_TEXTBOX_OFF, rules build output) or has not fired yet; returns immediately if already read
static void tb_obs(const uint64_t *regs) {
    uint64_t w=regs[0];if(!w)return;
    uint64_t root=g_b?*(const uint64_t *)(g_b+0x2f05458):0;
    int collection=collection_scope(w,root,dm_ui_pc_desktop(),dm_lang_es()) && collection_ensure_loaded();
    if (!g_loaded) load(); if (g_loaded < 0 && !collection) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); if (!g_t0) g_t0 = t0;
    const char *s=*(const char **)(uintptr_t)(w+0x88);size_t n=*(const size_t *)(uintptr_t)(w+0x90);
    g_calls++; char *o = NULL; size_t on = 0; int k = 0;
    // 样式里 "options": { "pipeCharacterBehavior": "handle" } 的文字框认颜色码（+0xfc = 1，宿主同一控件开 / 不开逐字节差分）：
    // 聊天区那几行就是，注释的颜色照原样画，不动；其余（编年史列表等）不认，按表换成文字框版
    // Text boxes whose style has "options": { "pipeCharacterBehavior": "handle" } understand color codes (+0xfc = 1; byte-by-byte diff on the host of the same widget with it on / off):
    // the chat area lines are such boxes, annotation colors are drawn as is, left untouched; the rest (chronicle list etc.) do not, and are replaced with the text box version by the table
    int handles = *(const uint8_t *)(uintptr_t)(w + 0xfc) == 1;
    if(collection)k=dm_collection_names_rewrite(s,n,&o,&on);
    if (!k && g_loaded>0 && !handles) k = dm_textbox_rewrite(s, n, &o, &on);
    { static int lg = -1; if (lg < 0) lg = getenv("DM_TEXTBOX_LOG") != NULL;   // 带码进门的文字框按控件名各记一次去向（受影响面清单用） | For text boxes that enter the gate carrying codes, log the destination once per widget name (used for the list of affected surfaces)
      if (lg && s && n && memmem(s, n, "\xee\x81\xbe", 3)) { static const char *seen[64]; static int ns; const char *nm = *(const char **)(uintptr_t)(w + 8); int i = 0; while (i < ns && seen[i] != nm) i++;
          if (i == ns && ns < 64) { seen[ns++] = nm; LOG("文字框颜色码：带码的文字框「%s」→ %s", nm ? nm : "?", handles ? "认颜色码，不动" : k ? "按表改写" : "不认颜色码但不在表里（原版自带的串），不动"); } } }
    if (k) { static int lg = -1; if (lg < 0) lg = getenv("DM_TEXTBOX_LOG") != NULL;   // 测试开关 DM_TEXTBOX_LOG=1：每次替换记一行（前 200 次） | Test switch DM_TEXTBOX_LOG=1: log one line per replacement (first 200)
        if (lg && g_hits < 200) { const char *nm = *(const char **)(uintptr_t)(w + 8); LOG("文字框颜色码：%s 控件「%s」→「%s」", k == 1 ? "整串" : "子串", nm ? nm : "?", o); }
        g_hits++; ((void *(*)(uint64_t, const char *, uint64_t))(g_b + 0x136e5c))(w + 0x88, o, on); free(o); }   // 同 dm_update.c set_text 的赋值 | Same assignment as set_text in dm_update.c
    uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0; g_ns += dt; if (dt > g_max_ns) g_max_ns = dt;
    static uint64_t last; static int stat = -1; if (stat < 0) stat = getenv("DM_TEXTBOX_STAT") != NULL;   // 测试开关 DM_TEXTBOX_STAT=1：每 10 秒记一行门的次数与耗时 | Test switch DM_TEXTBOX_STAT=1: log one line of gate hit count and time spent every 10 seconds
    if (stat && t0 - last > 10000000000ull) { last = t0; double sec = (t0 - g_t0) / 1e9;
        LOG("文字框颜色码：%.0f 秒内门触发 %llu 次（%.1f 次 / 秒），替换 %llu 次，平均 %.2f 微秒、最长 %.1f 微秒", sec, g_calls, sec > 0 ? g_calls / sec : 0, g_hits, g_calls ? g_ns / 1e3 / g_calls : 0, g_max_ns / 1e3); }
}
void dm_textbox_install(uint8_t *main_base) {
    g_b = main_base; if (!gate_obs_present(OBS_TEXTBOX)) { LOG("文字框颜色码：产物没有观测门 14（r33 起），不处理"); return; }
    if (getenv("DM_TEXTBOX_OFF")) { LOG("文字框颜色码：测试开关 DM_TEXTBOX_OFF，不挂门（A/B 量帧时间用）"); return; }
    gate_obs_set(OBS_TEXTBOX, tb_obs);
}
