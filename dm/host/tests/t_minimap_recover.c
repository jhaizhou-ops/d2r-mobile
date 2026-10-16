// 跨幕隐藏小地图的宿主回归：跑实际采样泵与PanelManager消息发送，自造隐藏控件模拟换幕结果；不运行原生游戏。
// Host regression for a hidden minimap after Act travel: run the real poll and PanelManager message dispatch with authored hidden widgets; does not run the native game.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
static int area=40, valid=1, dead, ingame=1, pad, busy, opens, accept=1;
void dm_hud_get(dm_hud_info *h) { memset(h,0,sizeof *h); h->valid=valid; h->area=area; h->dead=dead; }
int dm_uiscreen_ingame(void) { return ingame; }
int dm_padmode_current(void) { return pad; }
int dm_padmode_busy(void) { return busy; }
void dm_ui_rect(int *x,int *y,int *w,int *h) { *x=0; *y=308; *w=2420; *h=1360; }
int dm_screen_real(int *w,int *h) { *w=2420; *h=1668; return 1; }
static _Alignas(8) unsigned char root[160], am[160], hud[160], menu[160];
static uint64_t child[3], vt[6], frame;
static void receive(uint64_t r, void *v) { (void)r; uint64_t *msg=v; if(msg[0]!=ui_hash("PanelManager")||msg[1]!=ui_hash("OpenPanel")||strcmp((char *)(uintptr_t)msg[2],"AutoMap")) abort(); opens++; if(accept) am[0x50]=1; }
static void poll(void) { ui_minimap_pump(&g_mod[0],(uint64_t)(uintptr_t)root,2420,1360,frame); frame+=30; }
static int checks,fails;
#define CHECK(c) do { checks++; if(!(c)) {fails++; printf("FAIL line %d: %s\n",__LINE__,#c);} } while(0)
int main(void) {
 unsigned char *base=calloc(1,0x2f05460); if(!base)return 2; g_nmod=1; strcpy(g_mod[0].name,"main"); g_mod[0].base=base;
 *(uint64_t *)(base+0x2f05458)=(uint64_t)(uintptr_t)root; *(int *)(base+0x2d479f4)=2; *(int *)(base+0x2d5c748)=1;
 vt[5]=(uint64_t)(uintptr_t)receive; *(uint64_t *)root=(uint64_t)(uintptr_t)vt; child[0]=(uint64_t)(uintptr_t)am; child[1]=(uint64_t)(uintptr_t)hud; child[2]=(uint64_t)(uintptr_t)menu;
 *(uint64_t *)(root+0x58)=(uint64_t)(uintptr_t)child; *(uint64_t *)(root+0x60)=3;
 *(uint64_t *)am=(uint64_t)(uintptr_t)base+0x20c58a0; *(const char **)(am+8)="AutoMap"; *(int *)(am+0x70)=-1280; *(int *)(am+0x74)=-411; *(int *)(am+0x78)=1280; *(int *)(am+0x7c)=719; *(float *)(am+0x80)=1;
 *(const char **)(hud+8)="HUDPanel"; hud[0x50]=1; *(const char **)(menu+8)="PauseLayout";
 atomic_store(&g_load_up,0); poll(); poll(); CHECK(am[0x50]==1 && opens==1);
 int old=opens; for(int i=0;i<10;i++)poll(); CHECK(opens==old);
 // 加载中、手柄、模式事务、暂停菜单、死亡都不能开；阻挡结束后再恢复。
 // Loading, controller mode, a mode transaction, pause menu and death must not open it; recover after the blocker ends.
 am[0x50]=0; atomic_store(&g_load_up,1); poll();poll(); CHECK(opens==old);
 atomic_store(&g_load_up,0); pad=1; poll();poll(); CHECK(opens==old);
 pad=0; busy=1; poll();poll(); CHECK(opens==old);
 busy=0; menu[0x50]=1; poll();poll(); CHECK(opens==old);
 menu[0x50]=0; dead=1; poll();poll(); CHECK(opens==old);
 dead=0; valid=0; poll();poll(); CHECK(opens==old);
 valid=1; ingame=0; poll();poll(); CHECK(opens==old);
 ingame=1; area=75; poll();poll(); CHECK(am[0x50]==1 && opens==old+1);
 // 异步开失败：只允许三次有间隔请求，不逐帧重发。
 // An asynchronous open failure permits only three spaced requests, never a request every frame.
 old=opens; am[0x50]=0; area=40; accept=0; for(int i=0;i<30;i++)poll(); CHECK(opens==old+3);
 old=opens; dm_minimap_mode_leave(); CHECK(g_map_recover.tries==0 && g_map_recover.widget==0);
 accept=1; *(const char **)(menu+8)="FocusManager"; menu[0x50]=1; poll();poll(); CHECK(am[0x50]==1 && opens==old+1);
 am[0x50]=0; *(int *)(base+0x2d479f4)=0; old=opens; poll();poll(); CHECK(opens==old);
 *(int *)(base+0x2d479f4)=2; hud[0x50]=0; poll();poll(); CHECK(opens==old);
 hud[0x50]=1; child[0]=0; poll();poll(); CHECK(opens==old);
 child[0]=(uint64_t)(uintptr_t)am; poll();poll(); CHECK(am[0x50]==1 && opens==old+1);
 // 不改地图位置、尺寸、档位；恢复只走游戏的OpenPanel。
 // Do not change map position, size or mode; restoration goes only through the game's OpenPanel.
 CHECK(*(int *)(am+0x74)==-411 && *(int *)(am+0x78)==1280 && *(int *)(base+0x2d479f4)==2 && *(int *)(base+0x2d5c748)==1);
 free(base);printf("t_minimap_recover: %d checks, %d failures\n",checks,fails);return fails!=0;
}
