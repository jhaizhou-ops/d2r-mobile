#include "sw_touch_game_state.h"
#include "../../host/sw_input.h"
#include <string.h>
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint64_t sdk(uint32_t logical) {
    switch(logical) {
        case 0x1000:return SW_BTN_B; case 0x2000:return SW_BTN_A;
        case 0x4000:return SW_BTN_Y; case 0x8000:return SW_BTN_X;
        case 0x200:return SW_BTN_R; case 0x800:return SW_BTN_ZR;
        default:return 0;
    }
}
static int portal(uint32_t id) { return id==359 || id==219 || id==220; }
int sw_tg_parse(sw_tg_snapshot *out,const void *mapping,size_t mapping_bytes,
                const void *bank,size_t bank_bytes,int selected_bank,int getter_input_slot,
                int observed_index,uint32_t observed_id,uint64_t captured_ns) {
    if(!out)return 0;
    memset(out,0,sizeof(*out)); out->supported=1;out->portal.slot=-1;
    if(!mapping || !bank || mapping_bytes<SW_TG_FACETS*12 || bank_bytes<SW_TG_SLOTS*SW_TG_STRIDE ||
       (selected_bank!=0 && selected_bank!=16) || getter_input_slot<0 || getter_input_slot>=SW_TG_SLOTS ||
       observed_index!=selected_bank+getter_input_slot) return 0;
    sw_tg_snapshot s=*out; unsigned normals=0,alts=0; uint64_t logicals=0;
    const unsigned char *m=mapping,*b=bank;
    for(int i=0;i<SW_TG_FACETS;i++) {
        sw_tg_mapping *t=&s.mapping[i]; t->logical=u32(m+i*12);t->normal=u32(m+i*12+4);t->alt=u32(m+i*12+8);
        uint64_t bit=sdk(t->logical);
        if(!bit || (logicals&bit) || t->normal>=6 || t->alt<6 || t->alt>=12 ||
           (normals&(1u<<t->normal)) || (alts&(1u<<(t->alt-6))))return 0;
        logicals|=bit;normals|=1u<<t->normal;alts|=1u<<(t->alt-6);
    }
    for(int i=0;i<SW_TG_SLOTS;i++)s.raw_skill_id[i]=u32(b+i*SW_TG_STRIDE);
    if(s.raw_skill_id[observed_index-selected_bank]!=observed_id)return 0;
    /* 先替代栏，再普通栏；每栏按原slot顺序确定唯一入口。 */
    for(int pass=0;pass<2 && !s.portal.valid;pass++)for(int slot=pass?0:6;slot<(pass?6:12);slot++) {
        if(!portal(s.raw_skill_id[slot]))continue;
        for(int i=0;i<SW_TG_FACETS;i++)if((pass?s.mapping[i].normal:s.mapping[i].alt)==(uint32_t)slot) {
            s.portal=(sw_tg_portal){sdk(s.mapping[i].logical)|(pass?0:SW_BTN_ZL),s.raw_skill_id[slot],slot,1};break;
        }
        if(s.portal.valid)break;
    }
    s.valid=1;s.source=SW_TG_SOURCE_NATIVE_GETTER;s.selected_bank=selected_bank;s.getter_input_slot=getter_input_slot;
    s.observed_index=observed_index;s.captured_ns=captured_ns;*out=s;return 1;
}

uint64_t sw_tg_portal_mask(const sw_tg_snapshot *s,uint64_t host_now_ns) {
    if(!s || !s->supported || !s->valid || !s->ready || !s->portal.valid || !s->captured_ns ||
       host_now_ns<s->captured_ns || host_now_ns-s->captured_ns>SW_TG_MAX_AGE_NS)return 0;
    return s->portal.buttons;
}

static uint64_t identity_add(uint64_t h,uint64_t value) {
    for(int i=0;i<8;i++) { h^=(unsigned char)(value>>(i*8)); h*=1099511628211ull; }
    return h;
}
static uint64_t binding_identity(const sw_tg_snapshot *s,uint64_t caller,uint64_t hud,int scope) {
    uint64_t h=identity_add(1469598103934665603ull,(uint64_t)s->selected_bank);
    h=identity_add(h,s->world_epoch);h=identity_add(h,caller);h=identity_add(h,hud);h=identity_add(h,(uint64_t)scope);
    for(int i=0;i<SW_TG_FACETS;i++) {h=identity_add(h,s->mapping[i].logical);h=identity_add(h,s->mapping[i].normal);h=identity_add(h,s->mapping[i].alt);}
    for(int i=0;i<SW_TG_SLOTS;i++)h=identity_add(h,s->raw_skill_id[i]);
    return h?h:1;
}
int sw_tg_group_observe(sw_tg_group *g,const sw_tg_snapshot *table,int game,uint64_t scene,
                        uint64_t caller,uint64_t hud,int scope) {
    if(!g || !table)return 0;
    sw_tg_snapshot s=*table;s.ready=0;s.scene_epoch=scene;s.caller_identity=caller;s.hud_identity=hud;s.identity_scope=scope;
    if(!s.valid || !game || !scene || !caller || !hud ||
       (scope!=SW_TG_IDENTITY_NATIVE && scope!=SW_TG_IDENTITY_SYNTHETIC) ||
       s.getter_input_slot<0 || s.getter_input_slot>=SW_TG_SLOTS) {
        memset(g,0,sizeof(*g));g->completed=s;return 0;
    }
    s.binding_identity=binding_identity(&s,caller,hud,scope);s.secondary_group=s.getter_input_slot>=6;
    int same=g->pending.binding_identity==s.binding_identity && g->pending.scene_epoch==scene &&
        g->pending.world_epoch==s.world_epoch && g->pending.selected_bank==s.selected_bank && g->pending.caller_identity==caller && g->pending.hud_identity==hud && g->pending.identity_scope==scope &&
        !memcmp(g->pending.mapping,s.mapping,sizeof s.mapping) && !memcmp(g->pending.raw_skill_id,s.raw_skill_id,sizeof s.raw_skill_id);
    if(!same) { memset(g,0,sizeof(*g));g->completed=s; }
    uint32_t bit=1u<<(s.getter_input_slot%6);
    if(!same || g->pending.secondary_group!=s.secondary_group || (g->seen&bit)) {g->seen=0;g->started_ns=s.captured_ns;}
    g->seen|=bit;s.group_seen=g->seen;s.group_started_ns=g->started_ns;g->pending=s;
    if(g->seen!=63)return 0;
    s.ready=1;g->completed=s;return 1;
}
