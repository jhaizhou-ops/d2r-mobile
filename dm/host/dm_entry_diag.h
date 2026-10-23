// 首次进局错误诊断的限流状态；依据现有 PanelManager 消息，不写游戏状态。
// Rate-limit state for run-entry errors; uses existing PanelManager messages without writing game state.
#ifndef DM_ENTRY_DIAG_H
#define DM_ENTRY_DIAG_H
#include <string.h>
typedef struct { unsigned attempts, snapshots, repeats; int fatal_open; } dm_entry_diag;
enum { DM_ENTRY_NONE, DM_ENTRY_LOAD, DM_ENTRY_READY, DM_ENTRY_FATAL, DM_ENTRY_CLOSED };
static int dm_entry_diag_event(dm_entry_diag *s, const char *cat, const char *act, const char *arg) {
    if (!cat || !act || !arg || strcmp(cat,"PanelManager")) return DM_ENTRY_NONE;
    int open=!strcmp(act,"OpenPanel") || !strcmp(act,"OpenExclusivePanel");
    int close=!strcmp(act,"ClosePanel") || !strcmp(act,"UnloadPanel");
    if (!strcmp(arg,"LoadScreenPanel")) { if(open) { s->attempts++; s->fatal_open=0; s->repeats=0; return DM_ENTRY_LOAD; } return close ? DM_ENTRY_READY : DM_ENTRY_NONE; }
    if (strcmp(arg,"FatalErrorPanel")) return DM_ENTRY_NONE;
    if (open) { if(s->fatal_open) { if(s->repeats < 1000000) s->repeats++; return DM_ENTRY_NONE; } s->fatal_open=1; s->repeats=0; if(s->snapshots < 4) { s->snapshots++; return DM_ENTRY_FATAL; } }
    if (close && s->fatal_open) { s->fatal_open=0; return DM_ENTRY_CLOSED; }
    return DM_ENTRY_NONE;
}
#endif
