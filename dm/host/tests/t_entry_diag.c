// 进局错误诊断状态回归：自造消息，验证重试、重复合并和快照预算。
// Entry-error diagnostic regression: authored messages test retries, coalescing and snapshot budget.
#include "../dm_entry_diag.h"
#include <stdio.h>
static int checks,fails;
#define CHECK(x) do { checks++; if(!(x)) { fails++; printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
int main(void) {
    dm_entry_diag s={0};
    CHECK(dm_entry_diag_event(&s,"Other","OpenPanel","FatalErrorPanel")==0);
    CHECK(dm_entry_diag_event(&s,0,0,0)==0);
    CHECK(dm_entry_diag_event(&s,"PanelManager","OpenPanel","LoadScreenPanel")==DM_ENTRY_LOAD);
    CHECK(s.attempts==1);
    CHECK(dm_entry_diag_event(&s,"PanelManager","UnloadPanel","LoadScreenPanel")==DM_ENTRY_READY);
    CHECK(dm_entry_diag_event(&s,"PanelManager","OpenPanel","FatalErrorPanel")==DM_ENTRY_FATAL);
    for(int i=0;i<145;i++) CHECK(dm_entry_diag_event(&s,"PanelManager","OpenPanel","FatalErrorPanel")==0);
    CHECK(s.repeats==145 && s.snapshots==1);
    CHECK(dm_entry_diag_event(&s,"PanelManager","ClosePanel","FatalErrorPanel")==DM_ENTRY_CLOSED);
    for(int i=0;i<5;i++) {
        CHECK(dm_entry_diag_event(&s,"PanelManager","OpenExclusivePanel","LoadScreenPanel")==DM_ENTRY_LOAD);
        CHECK(s.repeats==0);
        CHECK(dm_entry_diag_event(&s,"PanelManager","OpenPanel","FatalErrorPanel")== (i<3?DM_ENTRY_FATAL:DM_ENTRY_NONE));
        CHECK(dm_entry_diag_event(&s,"PanelManager","UnloadPanel","FatalErrorPanel")==DM_ENTRY_CLOSED);
    }
    CHECK(s.snapshots==4 && s.attempts==6);
    CHECK(dm_entry_diag_event(&s,"PanelManager","ClosePanel","FatalErrorPanel")==0);
    printf("t_entry_diag: %d checks, %d failures\n",checks,fails); return !!fails;
}
