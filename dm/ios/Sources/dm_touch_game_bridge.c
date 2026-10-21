#include "dm_touch_game_state.h"
#include "dm_touch_game_identity.h"
#include "../../host/dm.h"
enum { OBS_BINDING_GETTER=6, MAPPING_OFFSET=0x2d13cbc, BINDING_OFFSET=0x2d5f0c0 };
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static dm_tg_snapshot latest;
static dm_tg_group group;
static dm_tg_hud_cohort hud_cohort;
static int scene_game;
static uint64_t scene_epoch,world_epoch;
static const module_t *main_module;
void dm_touch_game_set_scene(int game,uint64_t scene,uint64_t world) {
    pthread_mutex_lock(&lock);
    if(world_epoch!=world)dm_tg_hud_reset(&hud_cohort,world);
    if(scene_game!=game || scene_epoch!=scene || world_epoch!=world) {
        scene_game=game;scene_epoch=scene;world_epoch=world;memset(&group,0,sizeof group);
        int supported=latest.supported;memset(&latest,0,sizeof latest);latest.supported=supported;
        latest.scene_epoch=scene;latest.world_epoch=world;latest.portal.slot=-1;
    }
    pthread_mutex_unlock(&lock);
}
int dm_touch_game_snapshot(dm_tg_snapshot *out) {
    if(!out)return 0;
    pthread_mutex_lock(&lock);*out=latest;pthread_mutex_unlock(&lock);return out->valid;
}
/* 只读render实例门；不学习cohort，不续反馈TTL，不写客体。未知一律原render。 | Read-only render instance gate; does not learn the cohort, does not extend the feedback TTL, does not write to the guest. Anything unknown gets the original render. */
int dm_touch_game_hide_own_glyph(uint64_t widget) {
    int match=0;pthread_mutex_lock(&lock);
    const module_t *m=main_module;uint64_t t=now_ns();
    if(m && scene_game && scene_epoch && world_epoch && hud_cohort.confirmed && hud_cohort.seen==63 &&
       hud_cohort.world_epoch==world_epoch && latest.supported && latest.valid && latest.ready &&
       latest.identity_scope==DM_TG_IDENTITY_NATIVE && latest.source==DM_TG_SOURCE_NATIVE_GETTER &&
       latest.scene_epoch==scene_epoch && latest.world_epoch==world_epoch &&
       latest.hud_identity==hud_cohort.hud_identity && latest.group_seen==63 &&
       latest.captured_ns && t>=latest.captured_ns && t-latest.captured_ns<=DM_TG_MAX_AGE_NS) {
        for(unsigned i=0;i<6;i++)if(widget && widget==hud_cohort.widget[i]) {
            /* 一次读完[widget,widget+c91)：VT、enum（+b18）、alwaysGetAlt（+c90）。读失败=原render。object是静态的（在lock内），不占客体线程的栈。 | Read [widget,widget+c91) in one go: VT, enum (+b18), alwaysGetAlt (+c90). Read failure = original render. object is static (inside the lock) and does not use the guest thread's stack. */
            static unsigned char object[0xc91];uint64_t vt=0;uint32_t slot=UINT32_MAX;
            if(widget<=UINT64_MAX-0xc90 && dm_tg_vm_read(widget,object,sizeof object)) {
                memcpy(&vt,object,8);memcpy(&slot,object+0xb18,4);
                if(vt==(uint64_t)m->base+0x208a5f8 && slot==i && !object[0xc90])match=1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&lock);return match;
}
/* 诊断（DM_BINDING_DIAG=1）：各返回点计数，每 3 秒一行；身份确认失败再按原因码细分（identity.c） | Diagnostic (DM_BINDING_DIAG=1): counts per return point, one line every 3 seconds; identity confirmation failures are further broken down by reason code (identity.c) */
static unsigned dg[8], dg_id[8]; static uint64_t dg_last;
static void diag_tick(void) {
    static int on=-1; if(on<0) on=getenv("DM_BINDING_DIAG")!=NULL; if(!on) return;
    uint64_t t=now_ns(); if(t-dg_last<3000000000ull) return; dg_last=t;
    LOG("绑定诊断：调用 %u｜寄存器不符 %u｜槽越界 %u｜组不符 %u｜不在局内 %u｜身份未确认 %u（原因 3:%u 4:%u 5:%u 6:%u 7:%u，LR 偏移 %#llx 槽 %u）｜成功 %u｜scene_game %d world %llu",
        dg[0],dg[1],dg[2],dg[3],dg[4],dg[5],dg_id[3],dg_id[4],dg_id[5],dg_id[6],dg_id[7],(unsigned long long)dm_tg_identity_lr,dm_tg_identity_slot,dg[6],scene_game,(unsigned long long)world_epoch);
}
static int live_check;   /* 真游戏里才开（要调游戏函数）；单测的假模块上不开 | Turned on only in the real game (it calls game functions); not turned on for the unit tests' fake module */
void dm_touch_game_live_check(int on) { live_check=on; }
/* 槽里绑的技能玩家现在还有没有（摘掉给技能的装备后，那颗钮回到空位；装备穿回来原版把绑定接回、钮自己回来——所以不清原版绑定表，只在快照里按空位报）。
   判法照原版选用技能时那段 main+0x43f1ac–0x43f23c：技能链表 [玩家+0x100] 里找「技能号、物品号（槽 +4；-1 = 自己学的）都相同」的节点，
   等级 = 节点 +0x38（自己学的再加装备加成 0x5b1710），夹到版本上限后 > 0 才算有。读不到玩家 / 版本不认 = 不改判。只在游戏线程调（观测门里）
   Whether the player still has the skill bound in the slot (after taking off the gear that grants the skill, that button goes back to an empty slot; when the gear is put back on, the original game reconnects the binding and the button comes back by itself -- so the original binding table is not cleared, and the slot is only reported as empty in the snapshot).
      The check follows the original game's code for selecting a skill at main+0x43f1ac–0x43f23c: in the skill linked list [player+0x100], find the node whose "skill number and item number (slot +4; -1 = self-learned) are both the same",
      level = node +0x38 (for a self-learned one, plus the gear bonus 0x5b1710), and it counts as owned only if > 0 after clamping to the version cap. Player unreadable / version not recognized = verdict unchanged. Called only on the game thread (inside the observation gate) */
static int skill_live(const unsigned char *b,uint32_t id,int32_t item) {
    /* 原版通用操作不在角色的已学 / 物品技能链里，可用性留给原版判断；不豁免怪物技能或已卸下装备的技能。 | Native general actions are not in the character's learned / item skill chain; leave their availability to the game. Monster skills and skills from removed gear are not exempted. */
    switch(id) {
    case 357: case 358: case 360: case 361: case 362: case 363: case 364: case 370:
        return 1;
    default: break;
    }
    if(id==UINT32_MAX || id==359 || id==219 || id==220)return 1;   /* 空位、回城卷轴 / 书（原版取槽时自己换号，按数量判）不归这里 | Empty slots and town portal scrolls / tomes (the original game swaps the number itself when fetching the slot, judged by quantity) are not handled here */
    uint64_t pl=((uint64_t(*)(uint32_t))(b+0x45f9d0))(*(const uint32_t*)(b+0x2d20e4c)); if(!pl)return 1;
    uint8_t ver=*(uint8_t*)(pl+0x1bd); if(ver<1 || ver>3)return 1;
    uint64_t l=*(uint64_t*)(pl+0x100),n=l?*(uint64_t*)l:0;
    for(int k=0;n && k<1024;k++,n=*(uint64_t*)(n+8)) { uint64_t r=*(uint64_t*)n;
        if(!r || *(int16_t*)r!=(int16_t)id || *(int32_t*)(n+0x44)!=item)continue;
        int lv=*(int32_t*)(n+0x38); if(item==-1)lv+=((int(*)(uint64_t,uint64_t,int))(b+0x5b1710))(pl,n,0);
        int cap=**(int32_t**)(*(uint64_t*)(b+0x2d64a18+(uint64_t)ver*16)+0x1500); if(lv<0)lv=0; if(lv>cap)lv=cap;
        return lv>0; }
    { int dm_swapskill_equipped(int32_t); return item!=-1 && dm_swapskill_equipped(item); }   /* 副手组技能：物品还穿在换手那组上 = 按下会先换武器再放（dm_actions.c），照常显示、角标「II」 | Off-hand set skill: the item is still worn on the weapon-swap set = pressing will swap weapons first and then cast (dm_actions.c); shown as usual, with corner badge "II" */
}
static void binding_getter(const uint64_t *regs) {
    const module_t *m=main_module;
    dg[0]++; diag_tick();
    if(!m || !regs || regs[10]!=(uint64_t)(m->base+BINDING_OFFSET) || regs[8]%DM_TG_STRIDE){dg[1]++;return;}
    uint64_t effective=regs[8]/DM_TG_STRIDE;
    uint32_t input_slot=(uint32_t)regs[19]; // PRE-ldr原getter arg，不是ID。 | PRE-ldr arg of the original getter, not an ID.
    if(input_slot>=DM_TG_SLOTS || effective>=28 || effective<input_slot){dg[2]++;return;}
    uint64_t selected_bank=effective-input_slot;
    if(selected_bank!=0 && selected_bank!=16){dg[3]++;return;}
    unsigned dead=0;   /* 绑着但玩家现在没有的技能（位 = 槽）。要调游戏函数，放在取锁之前 | Skills that are bound but the player does not currently have (bit = slot). This calls a game function, so it is placed before taking the lock */
    for(int i=0;live_check && i<DM_TG_SLOTS;i++) { uint32_t id;int32_t item;const unsigned char *e=(const unsigned char*)m->base+BINDING_OFFSET+(selected_bank+i)*DM_TG_STRIDE;memcpy(&id,e,4);memcpy(&item,e+4,4);
        static uint32_t gone[32]; int live=skill_live((const unsigned char*)m->base,id,item); if(!live)dead|=1u<<i;
        if(gone[selected_bank+i]!=(live?UINT32_MAX:id)) { gone[selected_bank+i]=live?UINT32_MAX:id; if(!live)LOG("技能钮：槽 %d 绑的技能 %u（物品 %d）玩家现在没有，按空位显示",(int)selected_bank+i,id,item); else LOG("技能钮：槽 %d 的技能回来了，照常显示",(int)selected_bank+i); } }
    pthread_mutex_lock(&lock);
    if(!scene_game || !world_epoch) {dg[4]++;pthread_mutex_unlock(&lock);return;} // menu/nav不学cohort。 | menu/nav do not learn the cohort.
    uint64_t caller=0,hud=0;
#ifndef DM_TG_TEST_SYNTHETIC_IDENTITY
    /* 不读内存的前置筛：HUD updater在main3e2f68把arg放进w23、3e2f6c再mov w0,w23后BL原Get；原Get只动x19-x21，
       被调函数按AAPCS保x23，所以HUD这条调用到门时必有x23==x19（arg）。不等=不是HUD updater，同「savedLR不符」一样IGNORE，省掉读栈。
       Pre-filter that reads no memory: at main3e2f68 the HUD updater puts arg into w23, at 3e2f6c it does mov w0,w23 and then BL to the original Get; the original Get only touches x19-x21,
              and the callee preserves x23 per AAPCS, so when this HUD call reaches the gate x23==x19 (arg) must hold. Not equal = not the HUD updater; IGNORE just like "savedLR mismatch", saving the stack read. */
    if(regs[23]!=regs[19]) {dg[5]++;dg_id[4]++;pthread_mutex_unlock(&lock);return;}
#endif
#ifdef DM_TG_TEST_SYNTHETIC_IDENTITY
    int scope=dm_touch_game_test_identity(regs,&caller,&hud); // 仅独立SDK_STUB构建；不与生产符号碰撞。 | Only for the standalone SDK_STUB build; does not collide with production symbols.
#else
    int scope=dm_tg_hud_identity(&hud_cohort,regs,(uint64_t)m->base,scene_game,world_epoch,dm_tg_vm_read,&caller,&hud);
#endif
    if(scope==DM_TG_IDENTITY_UNCONFIRMED) {dg[5]++;if(dm_tg_identity_reason>=0&&dm_tg_identity_reason<8)dg_id[dm_tg_identity_reason]++;pthread_mutex_unlock(&lock);return;} // cache/unknown/readFail全IGNORE，不清HUD快照/不续时。 | cache/unknown/readFail are all IGNORE; do not clear the HUD snapshot / do not extend the time.
    unsigned char mapping[72],bindings[DM_TG_SLOTS*DM_TG_STRIDE]={0};
    memcpy(mapping,m->base+MAPPING_OFFSET,sizeof mapping);
    for(int i=0;i<DM_TG_SLOTS;i++)memcpy(bindings+i*DM_TG_STRIDE,m->base+BINDING_OFFSET+(selected_bank+i)*DM_TG_STRIDE,4);
    uint32_t observed_id;memcpy(&observed_id,m->base+BINDING_OFFSET+regs[8],4);
    dm_tg_snapshot s;
    dm_tg_parse(&s,mapping,sizeof mapping,bindings,sizeof bindings,(int)selected_bank,(int)input_slot,(int)effective,observed_id,now_ns());
    for(int i=0;i<DM_TG_SLOTS;i++)if(s.valid && (dead>>i&1) && !(s.portal.valid && s.portal.slot==i))s.raw_skill_id[i]=UINT32_MAX;
    s.scene_epoch=scene_epoch;s.world_epoch=world_epoch;
    dm_tg_group_observe(&group,&s,scene_game,scene_epoch,caller,hud,scope);latest=group.completed;dg[6]++;
    pthread_mutex_unlock(&lock);
}
int dm_touch_game_attach(void) {
    main_module=NULL;
    pthread_mutex_lock(&lock);int supported=gate_obs_present(OBS_BINDING_GETTER);memset(&latest,0,sizeof latest);
    latest.supported=supported;latest.scene_epoch=scene_epoch;latest.world_epoch=world_epoch;latest.portal.slot=-1;memset(&group,0,sizeof group);
    pthread_mutex_unlock(&lock);
    if(!supported)return 0;
    if(g_nmod<1 || strcmp(g_mod[0].name,"main") || !g_mod[0].base ||
       g_mod[0].size<BINDING_OFFSET+32*DM_TG_STRIDE || g_mod[0].size<MAPPING_OFFSET+72)return 0;
    main_module=&g_mod[0];gate_obs_set(OBS_BINDING_GETTER,binding_getter);return 1;
}
