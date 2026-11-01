#include "dm_touch_game_state.h"
#include "../../host/dm.h"
#include "../../host/dm_input.h"
#include <assert.h>
module_t g_mod[MAX_MOD];int g_nmod;
static int present,checks,identity_scope=DM_TG_IDENTITY_UNCONFIRMED;
int dm_touch_game_test_identity(const uint64_t *regs,uint64_t *caller,uint64_t *hud) {(void)regs;*caller=77;*hud=88;return identity_scope;}
static void (*hook)(const uint64_t *);
int gate_obs_present(int id) {assert(id==6);return present;}
void gate_obs_set(int id,void (*fn)(const uint64_t *)) {assert(id==6);hook=fn;}
uint64_t now_ns(void) {return 123456;}
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x);return 1;}}while(0)
#ifdef RED_BASELINE
int dm_touch_game_attach(void){return 0;}
int dm_touch_game_snapshot(dm_tg_snapshot *out){memset(out,0,sizeof(*out));return 0;}
#endif
int main(int argc,char **argv) {
    CHECK(argc==3);dm_tg_snapshot s;CHECK(dm_touch_game_attach()==0);CHECK(!dm_touch_game_snapshot(&s) && !s.supported);
    size_t size=0x2d5f0c0+32*28;unsigned char *mem=calloc(1,size);CHECK(mem!=NULL);
    g_mod[0].base=mem;g_mod[0].size=size;strcpy(g_mod[0].name,"main");g_nmod=1;present=1;
    CHECK(dm_touch_game_attach()==1);CHECK(hook!=NULL);CHECK(!dm_touch_game_snapshot(&s) && s.supported);
    FILE *f=fopen(argv[1],"rb");CHECK(f!=NULL);CHECK(fread(mem+0x2d13cbc,1,72,f)==72);fclose(f);
    f=fopen(argv[2],"rb");CHECK(f!=NULL);CHECK(fread(mem+0x2d5f0c0,1,896,f)==896);fclose(f);
    uint64_t regs[31]={0};regs[10]=(uint64_t)(mem+0x2d5f0c0);regs[8]=7*28;regs[19]=7;
    uint64_t saved[31];memcpy(saved,regs,sizeof regs);unsigned char table[896];memcpy(table,mem+0x2d5f0c0,896);
    hook(regs);CHECK(!memcmp(saved,regs,sizeof regs));CHECK(!memcmp(table,mem+0x2d5f0c0,896));
    CHECK(!dm_touch_game_snapshot(&s) && !s.valid && !s.ready);
    CHECK(!s.ready && s.identity_scope==DM_TG_IDENTITY_UNCONFIRMED);
    dm_touch_game_set_scene(1,3,1);identity_scope=DM_TG_IDENTITY_SYNTHETIC;
    regs[8]=6*28;regs[19]=6;hook(regs);CHECK(dm_touch_game_snapshot(&s) && !s.ready);
    for(int i=7;i<12;i++) {regs[8]=i*28;regs[19]=i;hook(regs);}
    CHECK(dm_touch_game_snapshot(&s) && s.ready && s.secondary_group && s.group_seen==63 && s.scene_epoch==3);
    s.portal.buttons=0;CHECK(dm_touch_game_snapshot(&s) && s.portal.buttons==258);
    regs[8]=16*28;regs[19]=0;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.selected_bank==16 && !s.portal.buttons);
    regs[8]=16*28;regs[19]=16;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.selected_bank==16 && !s.portal.buttons); // otherbank显式arg16反例 | counterexample: otherbank with explicit arg16
    regs[8]=23*28;regs[19]=7;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.selected_bank==16 && !s.portal.buttons);
    regs[8]=7*28;regs[19]=7;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.selected_bank==0 && s.portal.buttons==258);
    regs[8]=7*28;regs[19]=6;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.selected_bank==0 && s.portal.buttons==258); // offset-arg=1 IGNORE
    const uint64_t bad[]={1,12*28,15*28,28*28,UINT64_MAX};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){regs[8]=bad[i];hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.supported && s.portal.buttons==258);}
    regs[8]=7*28;regs[19]=7;regs[10]++;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.portal.buttons==258);
    regs[10]--;regs[19]=6;hook(regs);CHECK(dm_touch_game_snapshot(&s) && s.portal.buttons==258);
    dm_touch_game_set_scene(0,4,1);CHECK(!dm_touch_game_snapshot(&s) && !s.ready && s.scene_epoch==4);
    regs[8]=7*28;regs[19]=7;hook(regs);CHECK(!dm_touch_game_snapshot(&s) && !s.ready);
    g_mod[0].size=size-1;CHECK(dm_touch_game_attach()==0);CHECK(!dm_touch_game_snapshot(&s) && s.supported);
    g_mod[0].size=size;strcpy(g_mod[0].name,"sdk");CHECK(dm_touch_game_attach()==0);
    free(mem);printf("PASS adapter synthetic ABI+read-only+negative %d/%d checks\n",checks,checks);return 0;
}

int dm_swapskill_equipped(int32_t item) { (void)item; return 0; }   /* 副手组技能（宿主 dm_actions.c）：单测里没有穿着的物品 | Secondary weapon set skills (host dm_actions.c): there are no worn items in the unit test */
int dm_swapskill_offhand_gives(int skill) { (void)skill; return 0; }
