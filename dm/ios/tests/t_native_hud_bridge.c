#include "dm_touch_game_state.h"
#include "../../host/dm.h"
#ifdef RED_BASELINE
#define SCENE(g,s,w) dm_touch_game_set_scene(g,s)
#define WORLD(s) 0
#else
#define SCENE(g,s,w) dm_touch_game_set_scene(g,s,w)
#define WORLD(s) ((s).world_epoch)
#endif
module_t g_mod[MAX_MOD];int g_nmod;
static void (*hook)(const uint64_t *);
static int checks,clock_reads,x23_off; /* x23_off!=0：让到门时 x23 != x19（不是 HUD updater 那条调用） | x23_off!=0: make x23 != x19 on reaching the gate (not the HUD updater call path) */static uint64_t clock_ns=1000;
int gate_obs_present(int id){return id==6;}
void gate_obs_set(int id,void (*fn)(const uint64_t *)){if(id==6)hook=fn;}
uint64_t now_ns(void){clock_reads++;return ++clock_ns;}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x);exit(1);}}while(0)
static void emit(uint64_t *regs,uint64_t *frame,unsigned char widget[7][0xc98],int slot,int arg,uint64_t caller){frame[1]=(uint64_t)widget[slot];frame[3]=caller;regs[19]=(uint64_t)arg;regs[23]=(uint64_t)arg+(uint64_t)x23_off;regs[8]=(uint64_t)arg*28;uint64_t before[31],stack_before[8];unsigned char widgets_before[7][0xc98];memcpy(before,regs,sizeof before);memcpy(stack_before,frame,sizeof stack_before);memcpy(widgets_before,widget,sizeof widgets_before);hook(regs);CHECK(!memcmp(before,regs,sizeof before) && !memcmp(stack_before,frame,sizeof stack_before) && !memcmp(widgets_before,widget,sizeof widgets_before));}
int main(int argc,char **argv){
 CHECK(argc==3);size_t size=0x2d5f0c0+896;unsigned char *mem=calloc(1,size);CHECK(mem!=NULL);g_nmod=1;g_mod[0].base=mem;g_mod[0].size=size;strcpy(g_mod[0].name,"main");
 FILE *f=fopen(argv[1],"rb");CHECK(f!=NULL);CHECK(fread(mem+0x2d13cbc,1,72,f)==72);fclose(f);f=fopen(argv[2],"rb");CHECK(f!=NULL);CHECK(fread(mem+0x2d5f0c0,1,896,f)==896);fclose(f);
 unsigned char original_mapping[72],original_table[896];memcpy(original_mapping,mem+0x2d13cbc,72);memcpy(original_table,mem+0x2d5f0c0,896);
 unsigned char widget[7][0xc98]={{0}};for(uint32_t i=0;i<7;i++){uint32_t slot=i<6?i:5;memcpy(widget[i]+0xb18,&slot,4);}
 uint64_t regs[31]={0},frame[8]={0};regs[29]=(uint64_t)&frame[2];regs[10]=(uint64_t)(mem+0x2d5f0c0);uint64_t hud_lr=(uint64_t)mem+0x3e2f74;dm_tg_snapshot s,before;
 CHECK(dm_touch_game_attach() && hook!=NULL);SCENE(1,2,1);
 for(int i=0;i<6;i++)emit(regs,frame,widget,i,i,hud_lr);
 CHECK(!dm_touch_game_snapshot(&s) && !s.ready && clock_reads==0); // 首cohort不借第六体变ready。 | the first cohort does not borrow the sixth body to become ready.
 for(int i=0;i<6;i++)emit(regs,frame,widget,i,i,hud_lr);
 CHECK(dm_touch_game_snapshot(&s) && s.ready && s.identity_scope==DM_TG_IDENTITY_NATIVE && s.group_seen==63 && s.portal.buttons==258 && WORLD(s)==1);
 before=s;int reads=clock_reads;
 for(int i=0;i<12;i++)emit(regs,frame,widget,i%6,i,(uint64_t)mem+0x2fa1d0);
 CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s) && clock_reads==reads); // cache不撤、不续时。 | cache is not revoked and its time is not extended.
 x23_off=1;for(int i=0;i<6;i++)emit(regs,frame,widget,i,i+6,hud_lr);x23_off=0;
 CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s) && clock_reads==reads); // 前置筛：栈帧里的返回地址与控件都对、只有 x23 != x19，也当非 HUD 调用忽略——不切组、不续时。 | pre-filter: the return address in the stack frame and the widget are both right and only x23 != x19, still ignored as a non-HUD call - no set switch, no time extension.
 emit(regs,frame,widget,5,16,hud_lr);CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s));
 emit(regs,frame,widget,5,0,hud_lr);CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s));
 widget[5][0xc90]=1;emit(regs,frame,widget,5,11,hud_lr);widget[5][0xc90]=0;CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s));
 uint64_t fp=regs[29];regs[29]=1;emit(regs,frame,widget,5,11,hud_lr);regs[29]=fp;CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s));
 emit(regs,frame,widget,6,11,hud_lr);CHECK(dm_touch_game_snapshot(&s) && !memcmp(&s,&before,sizeof s));
 CHECK(dm_tg_portal_mask(&s,s.captured_ns+250000001)==0); // cache不能延长fresh窗口。 | cache must not extend the fresh window.
 for(int i=0;i<6;i++)emit(regs,frame,widget,i,i+6,hud_lr);
 CHECK(dm_touch_game_snapshot(&s) && s.ready && s.secondary_group && s.group_started_ns>before.captured_ns && s.hud_identity==before.hud_identity);
 uint64_t cohort=s.hud_identity;SCENE(0,3,1);emit(regs,frame,widget,6,11,hud_lr);CHECK(!dm_touch_game_snapshot(&s));
 SCENE(1,4,1);emit(regs,frame,widget,6,11,hud_lr);CHECK(!dm_touch_game_snapshot(&s)); // 菜单六体不追认/回game不重学。 | the six menu bodies are not retroactively accepted / returning to game does not relearn.
 for(int i=0;i<6;i++)emit(regs,frame,widget,i,i,hud_lr);CHECK(dm_touch_game_snapshot(&s) && s.ready && s.hud_identity==cohort);
 SCENE(0,5,2);for(int i=0;i<6;i++)emit(regs,frame,widget,i,i,hud_lr);CHECK(!dm_touch_game_snapshot(&s));
 SCENE(1,6,3);for(int i=0;i<6;i++)emit(regs,frame,widget,i,i,hud_lr);CHECK(!dm_touch_game_snapshot(&s));
 for(int i=0;i<6;i++)emit(regs,frame,widget,i,i,hud_lr);CHECK(dm_touch_game_snapshot(&s) && s.ready && WORLD(s)==3 && s.hud_identity!=cohort);
 CHECK(!memcmp(original_mapping,mem+0x2d13cbc,72) && !memcmp(original_table,mem+0x2d5f0c0,896));
 free(mem);printf("PASS production strong HUD Mach ABI -> group/ignore/world bridge %d/%d checks\n",checks,checks);return 0;
}

int dm_swapskill_equipped(int32_t item) { (void)item; return 0; }   /* 副手组技能（宿主 dm_actions.c）：单测里没有穿着的物品 | Secondary weapon set skills (host dm_actions.c): there are no worn items in the unit test */
int dm_swapskill_offhand_gives(int skill) { (void)skill; return 0; }
