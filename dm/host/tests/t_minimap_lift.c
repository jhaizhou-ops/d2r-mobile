// Replay native layout replacement through the production lift pump and move guard.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
static int busy, ingame=1, uy=314; static uint64_t frame=100;
uint64_t dm_guest_frame(void) { return frame; }
int dm_padmode_busy(void) { return busy; }
int dm_uiscreen_ingame(void) { return ingame; }
void dm_ui_rect(int *x,int *y,int *w,int *h) { *x=0; *y=uy; *w=2360; *h=1326; }
static int checks,fails;
#define CHECK(c) do { checks++; if(!(c)) { fails++; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
static _Alignas(8) unsigned char root[160], maps[2][160], extra[160];
static uint64_t children[2];
static uint64_t ptr(void *p) { return (uint64_t)(uintptr_t)p; }
static int32_t *y(void *p) { return (int32_t *)((unsigned char *)p+0x74); }
static void widget(void *v,const char *name,int y0) {
 unsigned char *p=v; memset(p,0,160); *(const char **)(p+8)=name;
 *(int32_t *)(p+0x74)=y0; *(int32_t *)(p+0x78)=1280; *(int32_t *)(p+0x7c)=720;
 *(int32_t *)(p+0x84)=-110; p[0x50]=1;
}
static void tick(void) { frame+=6; ui_lift_pump(ptr(root),frame); }
int main(void) {
 unsigned char *base=calloc(1,0x2f05460); if(!base)return 2;
 g_nmod=1; strcpy(g_mod[0].name,"main"); g_mod[0].base=base;
 *(uint64_t *)(base+0x2f05458)=ptr(root); *(uint64_t *)(root+0x58)=ptr(children); *(uint64_t *)(root+0x60)=1;
 widget(maps[0],"AutoMap",78); children[0]=ptr(maps[0]); tick(); CHECK(*y(maps[0])==-433);
 // Mode leave restores the old layout. The native transaction then rebuilds it twice.
 // Its second pass may carry the first pass's geometry into the replacement widget.
 for(int k=0;k<20;k++) {
  unsigned char *old=maps[k%2], *next=maps[(k+1)%2];
  mv_restore(ptr(root),MV_LIFT); CHECK(*y(old)==78);
  busy=1; tick(); CHECK(*y(old)==78);
  memcpy(next,old,160); children[0]=ptr(next); tick(); CHECK(*y(next)==78);
  busy=0; tick(); CHECK(*y(next)==-433);
  for(int j=0;j<3;j++)tick(); CHECK(*y(next)==-433);
 }
 mv_restore(ptr(root),MV_LIFT); CHECK(*y(maps[0])==78);
 // Pruning a destroyed widget must not leave the insertion index pointing past the new entry.
 g_nmv=0; widget(maps[0],"AutoMap",78); children[0]=ptr(maps[0]); tick();
 widget(extra,"HUDWarnings",12); children[0]=ptr(extra); tick(); CHECK(*y(extra)==12); tick(); CHECK(*y(extra)==-499);
 tick(); CHECK(*y(extra)==-499); CHECK(g_nmv==1 && g_mv[0].val==-499);
 mv_restore(ptr(root),MV_LIFT); CHECK(*y(extra)==12);
 // Native relayout changes the baseline, while UI offset changes only the applied delta.
 children[0]=ptr(extra); *y(extra)=24; tick(); CHECK(*y(extra)==-487);
 uy=157; tick(); CHECK(*y(extra)==-231); tick(); CHECK(*y(extra)==-231);
 uy=0; tick(); CHECK(*y(extra)==24 && mv_count(MV_LIFT)==0);
 uy=314; tick(); CHECK(*y(extra)==-487);
 ingame=0; tick(); CHECK(*y(extra)==24 && mv_count(MV_LIFT)==0);
 free(base); printf("t_minimap_lift: %d checks, %d failures\n",checks,fails); return fails!=0;
}
