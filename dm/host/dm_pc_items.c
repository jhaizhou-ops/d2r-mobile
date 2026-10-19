// PC 物品和 NPC 边界：原版 3.3.94012 的 3ca3b0/F8、3db0b4、3cc87c、abcc50。
// PC item and NPC boundary: original 3.3.94012 3ca3b0/F8, 3db0b4, 3cc87c and abcc50.
#include "dm_pc_items.h"
#include "hle.h"
#include "dm_cursor.h"
#include "dm_actions.h"
#include "dm_uiscreen.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

extern int dm_ui_pc_desktop(void);
#ifndef DM_PC_ITEMS_TEST_PROFILE
__attribute__((weak)) int dm_ui_pc_desktop(void) { return 0; }
#endif
// 输入模块可补充技能 hover；独立构建不改变技能分支。
// The input module may add skill hover; a standalone build leaves that branch unchanged.
#ifndef DM_PC_ITEMS_TEST_SKILLS
__attribute__((weak)) int dm_ui_pc_skill_grid_hover(uint64_t *regs) { (void)regs;return 0; }
#endif
static int desktop(void) { return dm_ui_pc_desktop(); }
static dm_pc_items_snapshot pointer;
static _Atomic uint32_t cancellation;
static uint32_t published_cancellation,rearm_epoch;
static int awaiting_epoch;
static struct { uint64_t root;uint32_t generation,epoch,cancellation; } vault_context;
static _Atomic uint32_t g_ui_gen,g_pc_tabbar_epoch;
static _Atomic int g_load_up,g_vault_drop;
static struct {int modifiers;} g_pc_options_event_scope;
static module_t *main_module(void) { for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main"))return &g_mod[i];return NULL; }
static int pc_tooltip_read(uint64_t,void*,size_t);
static int pc_options_event_allowed(void) {
    if(!desktop()||!pointer.valid||!pointer.root||!pointer.ingame||pointer.loading||pointer.busy||pointer.event_scope==2||published_cancellation!=atomic_load(&cancellation))return 0;
    module_t *m=main_module();uint64_t root;
    return m&&m->base&&m->size>=0x2f05460&&pc_tooltip_read((uint64_t)m->base+0x2f05458,&root,8)&&root==pointer.root;
}
extern int dm_padmode_busy(void);
static int pc_items_busy(void) { return pointer.busy||dm_padmode_busy(); }
#define dm_padmode_busy pc_items_busy
static uint64_t ui_hash(const char *s) { uint64_t h=0xcbf29ce484222325ull;for(;*s;s++)h=(h^(uint8_t)*s)*0x100000001b3ull;return h; }
#define DM_VT_SLOT 0x2089fa0
#define DM_VT_SLOT_B 0x2089ab8
#define TAB_SEL(w) (*(int32_t*)((w)+0xdc4))
#define UI_POINTER_CANCEL 10

typedef int (*pc_tooltip_reader)(uint64_t,void*,size_t);
static int pc_tooltip_read(uint64_t address,void *out,size_t size) {
#if TARGET_OS_OSX
    vm_size_t got=0;return address>=0x1000&&size&&address<=UINT64_MAX-size&&!vm_read_overwrite(mach_task_self(),address,size,(vm_address_t)out,&got)&&got==size;
#else
    (void)address;(void)out;(void)size;return 0;
#endif
}
static int pc_tooltip_field(pc_tooltip_reader read,uint64_t object,uint64_t offset,void *out,size_t size) {return object>=0x1000&&object<=UINT64_MAX-offset&&read(object+offset,out,size);}
static int pc_tooltip_owned(pc_tooltip_reader read,uint64_t node,uint64_t wanted,int depth,unsigned *budget) {
    if(!node||depth>14||!*budget)return 0;if(node==wanted)return 1;--*budget;
    uint64_t children[2];if(!pc_tooltip_field(read,node,0x58,children,sizeof children)||children[1]>256||(!children[0]&&children[1]))return 0;
    for(uint64_t i=0;i<children[1];i++){uint64_t child=0;if(!pc_tooltip_field(read,children[0],8*i,&child,8))return 0;if(pc_tooltip_owned(read,child,wanted,depth+1,budget))return 1;}return 0;
}
static uint64_t pc_tooltip_unit(pc_tooltip_reader read,uint64_t base,uint32_t type,uint32_t id) {
    uint64_t unit=0;if(type>=6||!pc_tooltip_field(read,base,0x2d603a0+((uint64_t)type<<10)+8*(id&127),&unit,8))return 0;
    for(unsigned i=0;unit&&i<128;i++){uint32_t fields[3];if(!pc_tooltip_field(read,unit,0,fields,sizeof fields))return 0;if(fields[0]==type&&fields[2]==id)return unit;uint64_t next;if(!pc_tooltip_field(read,unit,0x158,&next,8)||next==unit)return 0;unit=next;}return 0;
}
static uint64_t pc_tooltip_local_player(pc_tooltip_reader read,uint64_t base,uint32_t index) {
    uint32_t id;if(index>7||!pc_tooltip_field(read,base,0x2d60380+4*index,&id,4)||id==UINT32_MAX)return 0;
    return pc_tooltip_unit(read,base,0,id);
}
static uint64_t pc_tooltip_inventory(pc_tooltip_reader read,uint64_t player) {
    uint64_t shared=0,inventory=0;uint32_t refs=0,magic=0;
    if(!pc_tooltip_field(read,player,0xa0,&shared,8))return 0;
    if(shared&&!pc_tooltip_field(read,shared,8,&refs,4))return 0;
    if(shared&&refs){if(!pc_tooltip_field(read,player,0x98,&inventory,8))return 0;}
    if(!inventory&&!pc_tooltip_field(read,player,0x90,&inventory,8))return 0;
    return pc_tooltip_field(read,inventory,0,&magic,4)&&magic==0x01020304?inventory:0;
}

static int ui_has(uint64_t root,uint64_t wanted,int depth) { unsigned budget=2048;return pc_tooltip_owned(pc_tooltip_read,root,wanted,depth,&budget); }
static int pc_tooltip_name(pc_tooltip_reader,uint64_t,char[96]);
static uint64_t pc_items_find(uint64_t node,const char *name,int depth,unsigned *budget) {
    if(!node||depth>14||!*budget)return 0;--*budget;char current[96];if(!pc_tooltip_name(pc_tooltip_read,node,current))return 0;
    if(!strcmp(name,current))return node;
    uint64_t children[2];if(!pc_tooltip_field(pc_tooltip_read,node,0x58,children,sizeof children)||children[1]>256)return 0;
    for(uint64_t i=0;i<children[1];i++){uint64_t child;if(!pc_tooltip_field(pc_tooltip_read,children[0],i*8,&child,8))return 0;uint64_t found=pc_items_find(child,name,depth+1,budget);if(found)return found;}
    return 0;
}
static uint64_t ui_find(uint64_t node,const char *name,int depth) { unsigned budget=2048;return pc_items_find(node,name,depth,&budget); }
static uint64_t ui_panel(uint64_t root,const char *name) { return ui_find(root,name,0); }
static int pc_tooltip_no_modal(pc_tooltip_reader,uint64_t);
static int modal_any(uint64_t root,int depth,void *unused) { (void)depth;(void)unused;return !pc_tooltip_no_modal(pc_tooltip_read,root); }
extern void dm_vault_ui_returning(uint32_t);
extern int dm_vault_ui_pointer(int,int,int);
extern void dm_pages_request_flip(int);
static void ui_send_mouse_arg(module_t *m,uint64_t root,uint64_t action,int x,int y,int argument) {
    struct{uint32_t flags;int32_t x,y,padding,arg;} payload={0,x,y,0,argument};
    uint64_t message[]={ui_hash("InputMessage"),action,(uint64_t)&payload};
    uint64_t vt=0,handler=0;if(!pc_tooltip_field(pc_tooltip_read,root,0,&vt,8)||!pc_tooltip_field(pc_tooltip_read,vt,0x28,&handler,8))return;
    uint64_t base=(uint64_t)m->base;if(handler>=base&&handler-base<m->text_end)((void(*)(uint64_t,const void*))handler)(root,message);
}
typedef struct {
    int (*hit)(uint64_t,uint64_t);
    uint64_t (*cell)(uint64_t,uint64_t);
    int (*accept)(uint64_t,uint64_t);
    void (*activate)(uint64_t,uint64_t);
    int (*live)(module_t *,uint64_t);
    int (*modified)(module_t *,uint64_t,uint64_t,int,int);
    void (*remember)(module_t *,uint64_t,uint64_t,uint64_t,uint64_t);
} pc_item_left_api;
static int pc_item_left_abi(module_t *m) {
    static const uint32_t entry[]={0xa9ba6ffc,0xa90167fa,0xa9025ff8,0xa90357f6},action[]={0xd28ccde8,0xf2a5c888,0xf2db5fe8,0xf2f7bf08},consume[]={0xf9422f68,0xb4000068,0x52800029,0x3902e109};
    return m&&m->base&&m->size>=0x2f05460&&!memcmp(m->base+0x3ca3b0,entry,sizeof entry)&&!memcmp(m->base+0x3ca468,action,sizeof action)&&!memcmp(m->base+0x3ca590,consume,sizeof consume);
}
static int pc_item_actor_live(module_t *m,uint64_t widget,uint64_t (*find)(uint64_t,uint32_t,uint32_t,uint32_t)) {
    static const uint32_t lookup[]={0xf861d800,0xb5000080,0x14000009,0xf940ac00};
    if(!m||!widget||!find||m->size<0x2f05460||memcmp(m->base+0x45f5f0,lookup,sizeof lookup))return 0;
    uint64_t base=(uint64_t)m->base,vt=*(uint64_t*)widget;
    if(vt<base||vt-base>m->size||m->size-(vt-base)<0x100)return 0;
    uint64_t activate=*(uint64_t*)(vt+0xf8);if(activate!=base+0x3d710c&&activate!=base+0x3b3a64)return 1;
    uint32_t id=*(uint32_t*)(widget+0x5c4),type=*(uint32_t*)(widget+0x5c8);
    return type<6&&id!=UINT32_MAX&&find(base+0x2d603a0+((uint64_t)type<<10),id&0x7f,id,type)!=0;
}
static int pc_item_native_live(module_t *m,uint64_t widget) {
    static const uint32_t player[]={0x71001c1f,0x54000168,0xb0014808,0x910e0108};
    if(!pc_item_actor_live(m,widget,(uint64_t(*)(uint64_t,uint32_t,uint32_t,uint32_t))(m->base+0x45f5f0))||memcmp(m->base+0x45f9d0,player,sizeof player))return 0;
    return ((uint64_t(*)(uint32_t))(m->base+0x45f9d0))(*(uint32_t*)(m->base+0x2d20e4c))!=0;
}
enum {PC_ITEM_NORMAL,PC_ITEM_BELT,PC_ITEM_SELL,PC_ITEM_BUY_BULK,PC_ITEM_MERC,PC_ITEM_TO_BAG,PC_ITEM_TO_CUBE,PC_ITEM_TO_BANK,PC_ITEM_TO_VAULT};
static int pc_item_modifier_plan(int right,int modifiers,int held,int page,int owner,int panels) {
    if(held)return PC_ITEM_NORMAL;
    if(right){if(!(modifiers&0x1000))return PC_ITEM_NORMAL;if(owner==1&&(panels&DM_PS_VENDOR))return PC_ITEM_BUY_BULK;return page==2?PC_ITEM_MERC:PC_ITEM_NORMAL;}
    if(modifiers&0x2000){
        if(owner==0&&page==0&&(panels&DM_PS_VENDOR))return PC_ITEM_SELL;
        if(page==3||page==4)return PC_ITEM_TO_BAG;
        if(owner==0&&page==0){if(panels&DM_PS_VAULT)return PC_ITEM_TO_VAULT;if(panels&DM_PS_CUBE)return PC_ITEM_TO_CUBE;if(panels&DM_PS_BANK)return PC_ITEM_TO_BANK;}
    }
    return (modifiers&0x1000)&&owner==0&&page==0?PC_ITEM_BELT:PC_ITEM_NORMAL;
}
static int pc_item_modifier_abi(module_t *m) {
    static const struct{uint32_t at,words[4];} code[]={
        {0x45f9d0,{0x71001c1f,0x54000168,0xb0014808,0x910e0108}},
        {0x433e50,{0xd10283ff,0xa9046ffc,0xa90567fa,0xa9065ff8}},
        {0x3c1b28,{0xd10103ff,0xa90157f6,0xa9024ff4,0xa9037bfd}},
        {0x3c1cd0,{0xd101c3ff,0xa90267fa,0xa9035ff8,0xa90457f6}},
        {0x3bf794,{0xf81e0ff3,0xa9017bfd,0x910043fd,0x528009a1}},
        {0x3bf860,{0xd10143ff,0xf90013f5,0xa9034ff4,0xa9047bfd}},
        {0x3b5b14,{0xd10103ff,0xf9000bf5,0xa9024ff4,0xa9037bfd}},
        {0x2aa7c0,{0x90015348,0xf9449100,0xd65f03c0,0}}
    };
    if(!m||m->size<0x2f05460)return 0;
    for(unsigned i=0;i<sizeof code/sizeof *code;i++)if(memcmp(m->base+code[i].at,code[i].words,16))return 0;
    return 1;
}
static void pc_item_trace(const char *stage,uint64_t widget,uint64_t cell,int modifiers) {
    static unsigned count;const char *enabled=getenv("DM_MAC_POINTER_LOG");if(!enabled||!*enabled||count>=64)return;
    const char *name=*(const char**)(widget+8);LOG("PC_ITEM %u stage=%s widget=%.80s cell=%d,%d mods=%04x",++count,stage,name?name:"(unnamed)",(int32_t)cell,(int32_t)(cell>>32),modifiers);
}
typedef struct {
    uint64_t (*player)(uint32_t),(*held)(void),(*query)(uint64_t,uint64_t*);
    int (*trade)(uint64_t,uint64_t,int,int,int,int),(*can_belt)(uint64_t),(*can_merc)(uint64_t);
    void (*put_belt)(uint64_t,uint64_t,uint8_t,int,void*),(*transfer)(uint64_t,uint64_t*,int),(*deposit)(uint32_t);
} pc_item_modifier_api;
static int pc_item_modifier_run(module_t *m,uint64_t widget,uint64_t cell,int right,int modifiers,const pc_item_modifier_api *api) {
    if(!(modifiers&0x3000)||!api||!api->player||!api->held||!api->query)return 0;
    uint64_t player=api->player(*(uint32_t*)(m->base+0x2d20e4c));
    if(!player)return 1;
    uint64_t held=api->held();if(held)return 0;
    uint64_t item=api->query(widget,&cell);if(!item)return 0;
    uint64_t data=*(uint64_t*)(item+0x10);int page=*(uint32_t*)(item+0xc)==2?2:data?*(uint8_t*)(data+0x55):-1;
    int owner=*(uint32_t*)(widget+0x5c8),panels=dm_ui_panel_state();
    if(owner==1&&*(uint32_t*)(widget+0x5c4)!=*(uint32_t*)(m->base+0x2d5ebfc))panels&=~DM_PS_VENDOR;
    int action=pc_item_modifier_plan(right,modifiers,0,page,owner,panels);
    if(!action)return 0;
    if(!pc_options_event_allowed())return 1;
    pc_item_trace(right?"modifier-right":"modifier-left",widget,cell,modifiers);
    if(action==PC_ITEM_SELL||action==PC_ITEM_BUY_BULK){if(!api->trade)return 1;api->trade(player,item,action==PC_ITEM_SELL,action==PC_ITEM_BUY_BULK,1,0);}
    else if(action==PC_ITEM_BELT){if(!api->can_belt||!api->put_belt)return 1;if(api->can_belt(item)&1){struct{uint32_t slot;uint8_t fixed;} target={0,0};api->put_belt(item,player,(uint8_t)page,1,&target);}}
    else if(action==PC_ITEM_MERC){if(!api->can_merc)return 1;if(api->can_merc(item)&1){int index=(int)(uint32_t)cell,col=index&3,row=index>>2;if(col>=0&&col<4&&row>=0&&row<4)dm_belt_use_merc(col,row);}}
    else if(action==PC_ITEM_TO_VAULT){if(api->deposit)api->deposit(*(uint32_t*)(item+8));}
    else if(api->transfer)api->transfer(widget,&cell,action==PC_ITEM_TO_CUBE?3:action==PC_ITEM_TO_BANK?4:0);
    return 1;
}
static int pc_item_modifier(module_t *m,uint64_t widget,uint64_t cell,int right,int modifiers) {
    if(!(modifiers&0x3000)||!pc_item_modifier_abi(m))return 0;
    uint64_t base=(uint64_t)m->base,vt=*(uint64_t*)widget,query=*(uint64_t*)(vt+0xc8),transfer=*(uint64_t*)(vt+0xd8);
    if(query<base||query-base>=m->text_end)return 0;
    if(transfer<base||transfer-base>=m->text_end)transfer=0;
    void dm_vault_request_deposit(uint32_t);
    pc_item_modifier_api api={.player=(void*)(m->base+0x45f9d0),.held=(void*)(m->base+0x2aa7c0),.query=(void*)query,.trade=(void*)(m->base+0x433e50),.can_belt=(void*)(m->base+0x3c1b28),.can_merc=(void*)(m->base+0x3bf794),.put_belt=(void*)(m->base+0x3c1cd0),.transfer=(void*)transfer,.deposit=dm_vault_request_deposit};
    return pc_item_modifier_run(m,widget,cell,right,modifiers,&api);
}
static int pc_item_left_functions(module_t *,uint64_t,pc_item_left_api *);
typedef struct {uint64_t root,source,cell,target,target_cell,release_xy;uint32_t id,epoch,generation;int x,y,pressed,pending,vault,wait,consume_up;} pc_item_drag;
static pc_item_drag g_pc_item_drag;
static void pc_item_remember(module_t *m,uint64_t root,uint64_t widget,uint64_t cell,uint64_t xy) {
    if(!pc_item_modifier_abi(m)||!pc_options_event_allowed())return;
    uint64_t held=((uint64_t(*)(void))(m->base+0x2aa7c0))();if(held)return;
    uint64_t base=(uint64_t)m->base,query=*(uint64_t*)(*(uint64_t*)widget+0xc8);if(query<base||query-base>=m->text_end)return;
    uint64_t item=((uint64_t(*)(uint64_t,uint64_t*))query)(widget,&cell);if(!item)return;
    uint64_t fn=*(uint64_t*)(*(uint64_t*)widget+0xf8),path=*(uint64_t*)(item+0x38);
    if(fn==base+0x3b3a64&&path)cell=*(uint32_t*)(path+0x10)|((uint64_t)*(uint32_t*)(path+0x14)<<32);
    g_pc_item_drag=(pc_item_drag){.root=root,.source=widget,.cell=cell,.id=*(uint32_t*)(item+8),.epoch=atomic_load(&g_pc_tabbar_epoch),.generation=atomic_load(&g_ui_gen),.x=(int32_t)xy,.y=(int32_t)(xy>>32),.pressed=1};
}
static int pc_item_drag_context(module_t *m,uint64_t root) {
    return desktop()&&pc_item_modifier_abi(m)&&root&&root==g_pc_item_drag.root&&*(uint64_t*)(m->base+0x2f05458)==root&&g_pc_item_drag.epoch==atomic_load(&g_pc_tabbar_epoch)&&g_pc_item_drag.generation==atomic_load(&g_ui_gen)&&dm_uiscreen_ingame()&&*(uint8_t*)(root+0xd9)&&!atomic_load(&g_load_up)&&!modal_any(root,1,NULL)&&ui_has(root,g_pc_item_drag.source,0);
}
static int pc_item_drag_take(pc_item_drag *state,int context,uint32_t held) {
    if(!context){memset(state,0,sizeof *state);return 0;}
    if(!state->pending)return 0;
    if(!held){if(state->wait>0&&!--state->wait)state->pending=0;return 0;}
    if(held!=state->id){memset(state,0,sizeof *state);return 0;}
    state->pending=0;return 1;
}
static int pc_item_family(module_t *m,uint64_t widget) {
    uint64_t base=(uint64_t)m->base,vt=*(uint64_t*)widget;
    if(vt<base||vt-base>m->size||m->size-(vt-base)<0x100)return 0;
    uint64_t fn=*(uint64_t*)(vt+0xf8);
    return fn==base+0x3b3a64||fn==base+0x3d710c||fn==base+0x3925a0||fn==base+0x3afdd4||fn==base+0x39db7c;
}
static int pc_item_drag_target(const pc_item_left_api *api,uint64_t target,uint64_t xy,uint64_t *cell) {
    if(!(api->hit(target,xy)&1))return 0;
    *cell=api->cell(target,xy);
    return *cell!=UINT64_MAX&&(api->accept(target,*cell)&1);
}
static void pc_item_drag_dispatch(module_t *m,uint64_t root,uint64_t held,int (*functions)(module_t*,uint64_t,pc_item_left_api*)) {
    if(!g_pc_item_drag.id)return;
    if(!pc_item_drag_context(m,root)){atomic_store(&g_vault_drop,0);dm_vault_ui_returning(0);memset(&g_pc_item_drag,0,sizeof g_pc_item_drag);return;}
    if(!g_pc_item_drag.pending)return;
    if(!pc_item_drag_take(&g_pc_item_drag,1,held?*(uint32_t*)(held+8):0))return;
    uint64_t target=g_pc_item_drag.target,cell=g_pc_item_drag.target_cell;int vault=g_pc_item_drag.vault;
    if(!target||!ui_has(root,target,0)||!pc_item_family(m,target)||!*(uint8_t*)(target+0x50)||!*(uint8_t*)(target+0x51))return;
    pc_item_left_api api;if(!functions(m,target,&api)||!api.live(m,target)||!pc_item_drag_context(m,root)||!ui_has(root,target,0))return;
    if(vault){if(!(api.accept(target,cell)&1))return;}
    else if(!pc_item_drag_target(&api,target,g_pc_item_drag.release_xy,&cell))return;
    if(!pc_item_drag_context(m,root)||!ui_has(root,target,0))return;
    if(vault){atomic_store(&g_vault_drop,(int)g_pc_item_drag.id);dm_vault_ui_returning(g_pc_item_drag.id);}
    pc_item_trace(vault?"vault-putback":"drag-release",target,cell,0);
    api.activate(target,cell);
}
static void pc_item_drag_pump(module_t *m,uint64_t root) {
    uint64_t held=g_pc_item_drag.id&&g_pc_item_drag.pending&&pc_item_drag_context(m,root)?((uint64_t(*)(void))(m->base+0x2aa7c0))():0;
    pc_item_drag_dispatch(m,root,held,pc_item_left_functions);
}
static int pc_item_pointer_before(module_t *m,uint64_t root,int kind,int x,int y) {
    if(!desktop())return 0;
    if(kind==DM_UP_END||kind==UI_POINTER_CANCEL||!pc_options_event_allowed()){atomic_store(&g_vault_drop,0);dm_vault_ui_returning(0);memset(&g_pc_item_drag,0,sizeof g_pc_item_drag);return 0;}
    if(kind==DM_UP_LDOWN||kind==DM_UP_RDOWN){atomic_store(&g_vault_drop,0);dm_vault_ui_returning(0);}
    if(kind==DM_UP_LUP&&g_pc_item_drag.consume_up){g_pc_item_drag.consume_up=0;return 1;}
    if(!g_pc_item_drag.id||!pc_item_drag_context(m,root))return 0;
    int dm_vault_ui_over(int,int);
    if((kind!=DM_UP_LDOWN&&kind!=DM_UP_LUP)||!(dm_ui_panel_state()&DM_PS_VAULT)||!dm_vault_ui_over(x,y))return 0;
    if(kind==DM_UP_LUP&&!g_pc_item_drag.pressed)return 0;
    g_pc_item_drag.pending=1;g_pc_item_drag.target=g_pc_item_drag.source;g_pc_item_drag.target_cell=g_pc_item_drag.cell;g_pc_item_drag.vault=1;g_pc_item_drag.wait=90;
    if(kind==DM_UP_LDOWN)g_pc_item_drag.consume_up=1;
    else {ui_send_mouse_arg(m,root,ui_hash("LeftMouseButtonUp"),g_pc_item_drag.x,g_pc_item_drag.y,0);}
    g_pc_item_drag.pressed=0;pc_item_drag_pump(m,root);return 1;
}
static void pc_item_pointer_after(module_t *m,uint64_t root,int kind,int x,int y) {
    if(kind!=DM_UP_LUP||!g_pc_item_drag.pressed)return;
    g_pc_item_drag.pressed=0;
    if(!pc_item_drag_context(m,root)||!pc_options_event_allowed())return;
    int64_t dx=(int64_t)x-g_pc_item_drag.x,dy=(int64_t)y-g_pc_item_drag.y;if(dx*dx+dy*dy<16)return;
    uint64_t manager=*(uint64_t*)(root+0xd0),target=manager&&ui_has(root,manager,0)?*(uint64_t*)(manager+0x178):0;
    if(!target||!ui_has(root,target,0)||!pc_item_family(m,target))return;
    pc_item_left_api api;if(!pc_item_left_functions(m,target,&api)||!api.live(m,target))return;
    uint64_t xy=(uint32_t)x|((uint64_t)(uint32_t)y<<32);if(!(api.hit(target,xy)&1))return;
    g_pc_item_drag.pending=1;g_pc_item_drag.target=target;g_pc_item_drag.release_xy=xy;g_pc_item_drag.vault=0;g_pc_item_drag.wait=90;pc_item_drag_pump(m,root);
}
static int pc_item_context(module_t *m,uint64_t root,uint64_t widget) {
    if(!pc_options_event_allowed()||!root||*(uint64_t*)(m->base+0x2f05458)!=root||!ui_has(root,widget,0)||*(uint8_t*)(root+0xb8)||!*(uint8_t*)(widget+0x50)||!*(uint8_t*)(widget+0x51))return 0;
    uint64_t manager=*(uint64_t*)(root+0xd0);
    return manager&&ui_has(root,manager,0)&&*(uint64_t*)manager==(uint64_t)m->base+0x20b7f88&&*(uint64_t*)(manager+0x178)==widget;
}
static int pc_item_tooltip_abi(module_t *m) {
    static const uint32_t getter[]={0xf81c0ff7,0xa90157f6,0xa9024ff4,0xa9037bfd},cursor[]={0xd0014a29,0x3963c129},equipped[]={0xd10203ff,0xa9045ff8,0xa90557f6,0xa9064ff4},grid[]={0xd104c3ff,0x6d093bef,0x6d0a33ed,0x6d0b2beb};
    return m&&m->base&&m->size>=0x2f05460&&!memcmp(m->base+0x3cc87c,getter,sizeof getter)&&!memcmp(m->base+0x3cc974,cursor,sizeof cursor)&&!memcmp(m->base+0x3d4f80,equipped,sizeof equipped)&&!memcmp(m->base+0x3b06c8,grid,sizeof grid);
}
enum {PC_TIP_INVENTORY=1,PC_TIP_BANK,PC_TIP_VENDOR};
static int pc_tooltip_name(pc_tooltip_reader read,uint64_t widget,char name[96]) {
    uint64_t text=0;if(!pc_tooltip_field(read,widget,8,&text,8))return 0;
    name[0]=0;if(!text)return 1;
    for(unsigned i=0;i<96;i+=16){
        if(pc_tooltip_field(read,text,i,name+i,16)){if(memchr(name+i,0,16))return 1;continue;}
        for(unsigned j=0;j<16;j++){if(!pc_tooltip_field(read,text,i+j,name+i+j,1))return 0;if(!name[i+j])return 1;}
    }
    return 0;
}
static int pc_tooltip_domain(pc_tooltip_reader read,uint64_t root,uint64_t widget) {
    uint64_t node=widget,panel=0;
    for(unsigned i=0;node&&i<64;i++){
        uint8_t flags[2];uint64_t parent=0;
        if(!pc_tooltip_field(read,node,0x50,flags,2)||!flags[0]||!flags[1])return 0;
        if(node==root){
            char name[96];unsigned budget=2048;
            if(!panel||!pc_tooltip_owned(read,root,widget,0,&budget)||!pc_tooltip_name(read,panel,name))return 0;
            if(!strcmp(name,"PlayerInventoryExpansionLayout")||!strcmp(name,"PlayerInventoryOriginalLayout"))return PC_TIP_INVENTORY;
            if(!strcmp(name,"BankExpansionLayout")||!strcmp(name,"BankOriginalLayout"))return PC_TIP_BANK;
            return !strcmp(name,"VendorPanelLayout")?PC_TIP_VENDOR:0;
        }
        if(!pc_tooltip_field(read,node,0x30,&parent,8)||parent==node)return 0;
        panel=node;node=parent;
    }
    return 0;
}
static int pc_tooltip_visible_leaf(pc_tooltip_reader read,uint64_t widget,unsigned depth,unsigned *budget) {
    if(!widget)return 0;if(depth>14||!*budget)return -1;--*budget;
    uint8_t flags[2];uint64_t children[2];
    if(!pc_tooltip_field(read,widget,0x50,flags,2))return -1;
    if(!flags[0]||!flags[1])return 0;
    if(!pc_tooltip_field(read,widget,0x58,children,sizeof children)||children[1]>256||(!children[0]&&children[1]))return -1;
    if(!children[1])return depth>1;
    for(uint64_t i=0;i<children[1];i++){
        uint64_t child;if(!pc_tooltip_field(read,children[0],8*i,&child,8))return -1;
        int visible=pc_tooltip_visible_leaf(read,child,depth+1,budget);if(visible)return visible;
    }
    return 0;
}
static int pc_tooltip_no_modal(pc_tooltip_reader read,uint64_t root) {
    uint64_t children[2];if(!pc_tooltip_field(read,root,0x58,children,sizeof children)||children[1]>256||(!children[0]&&children[1]))return 0;
    for(uint64_t i=0;i<children[1];i++){
        uint64_t panel;uint8_t visible;char name[96];
        if(!pc_tooltip_field(read,children[0],8*i,&panel,8))return 0;
        if(!panel)continue;
        if(!pc_tooltip_field(read,panel,0x50,&visible,1))return 0;
        if(!visible)continue;
        if(!pc_tooltip_name(read,panel,name))return 0;
        size_t length=strlen(name);if(length<5||strcmp(name+length-5,"Modal"))continue;
        unsigned budget=2048;if(pc_tooltip_visible_leaf(read,panel,1,&budget)!=0)return 0;
    }
    return 1;
}
static int pc_tooltip_shared_id(module_t *m,pc_tooltip_reader read,uint32_t *id) {
    static const uint32_t hash[]={0xd292b9aa,0xf2a0024a,0xf2c9d72a,0xf2e3f0ca};
    static const uint32_t keycode[]={0xd2857a89,0xf2aea629,0xf2c314c9,0xf2f1a9a9};
    static const uint32_t chain[]={0xf9400908,0xf9400908,0xb9400108};
    if(memcmp(m->base+0x3b58b4,hash,sizeof hash)||memcmp(m->base+0x3b58d8,keycode,sizeof keycode)||memcmp(m->base+0x3b5930,chain,sizeof chain))return 0;
    uint64_t map,head[2],node;
    if(!pc_tooltip_field(read,(uint64_t)m->base,0x2f05450,&map,8)||!pc_tooltip_field(read,map,0,head,sizeof head)||!head[0]||head[0]>1048576||!pc_tooltip_field(read,head[1],8*((head[0]-1)&0x1f864eb9001295cdull),&node,8))return 0;
    for(unsigned i=0;node&&i<64;i++){
        uint64_t record[3];if(!pc_tooltip_field(read,node,0,record,sizeof record))return 0;
        if(record[1]==0x8d4d18a675312bd4ull){uint64_t value;return pc_tooltip_field(read,record[2],0x10,&value,8)&&pc_tooltip_field(read,value,0,id,4)&&*id!=UINT32_MAX;}
        if(record[0]==node)return 0;node=record[0];
    }
    return 0;
}
static int pc_tooltip_domain_owner(module_t *m,pc_tooltip_reader read,uint64_t widget,int domain,uint64_t player) {
    uint64_t vt;uint32_t owner[2];
    if(!pc_tooltip_field(read,widget,0,&vt,8)||(vt!=(uint64_t)m->base+DM_VT_SLOT&&vt!=(uint64_t)m->base+DM_VT_SLOT_B)||
       (domain!=PC_TIP_INVENTORY&&vt!=(uint64_t)m->base+DM_VT_SLOT_B)||
       !pc_tooltip_field(read,widget,0x5c4,owner,sizeof owner)||owner[0]==UINT32_MAX)return 0;
    uint64_t actor=pc_tooltip_unit(read,(uint64_t)m->base,owner[1],owner[0]);if(!actor)return 0;
    if(domain==PC_TIP_INVENTORY)return owner[1]==0&&actor==player;
    if(domain==PC_TIP_BANK){uint32_t id;uint8_t page;return owner[1]==0&&pc_tooltip_field(read,widget,0x630,&page,1)&&page==4&&
        (actor==player||(pc_tooltip_shared_id(m,read,&id)&&id==owner[0]));}
    uint32_t npc,interacting;
    return domain==PC_TIP_VENDOR&&owner[1]==1&&pc_tooltip_field(read,(uint64_t)m->base,0x2d5ebfc,&npc,4)&&npc==owner[0]&&
        pc_tooltip_field(read,(uint64_t)m->base,0x2d5ec00,&interacting,4)&&interacting;
}
static int pc_item_tooltip_context(module_t *m,uint64_t root,uint64_t widget,pc_tooltip_reader read) {
    int dm_padmode_busy(void);
    if(!m||!m->base||m->size<0x2f05460||!root||!widget||!read||(!dm_uiscreen_ingame()||!pointer.ingame)||dm_padmode_busy()||(atomic_load(&g_load_up)>0||pointer.loading))return 0;
    uint32_t generation=atomic_load(&g_ui_gen),epoch=atomic_load(&g_pc_tabbar_epoch);dm_pc_items_snapshot point=pointer;
    if(!pc_options_event_allowed()||!point.valid||point.root!=root||point.generation!=generation||point.epoch!=epoch)return 0;
    uint64_t base=(uint64_t)m->base,current_root,manager,vt,hover;uint8_t ingame,native_ec;int xy[2];unsigned budget=2048;
    if(!pc_tooltip_field(read,base,0x2f05458,&current_root,8)||current_root!=root||!pc_tooltip_field(read,root,0xd9,&ingame,1)||!ingame||
       !pc_tooltip_field(read,root,0xd0,&manager,8)||!pc_tooltip_field(read,manager,0,&vt,8)||vt!=base+0x20b7f88||
       !pc_tooltip_field(read,manager,0x178,&hover,8)||hover!=widget||!pc_tooltip_owned(read,root,manager,0,&budget)||
       !pc_tooltip_field(read,base,0x2d128ec,&native_ec,1)||native_ec||
       !pc_tooltip_field(read,base,0x2d128f8,xy,sizeof xy)||xy[0]!=point.x||xy[1]!=point.y)return 0;
    int domain=pc_tooltip_domain(read,root,widget);if(!domain||!pc_tooltip_no_modal(read,root))return 0;
    uint32_t local;uint64_t held;
    if(!pc_tooltip_field(read,base,0x2d20e4c,&local,4))return 0;
    uint64_t player=pc_tooltip_local_player(read,base,local),inventory=pc_tooltip_inventory(read,player);
    if(!inventory||!pc_tooltip_field(read,inventory,0x40,&held,8)||held||!pc_tooltip_domain_owner(m,read,widget,domain,player))return 0;
    uint64_t current_manager,current_hover;
    if(!pc_tooltip_field(read,base,0x2f05458,&current_root,8)||current_root!=root||!pc_tooltip_field(read,root,0xd0,&current_manager,8)||current_manager!=manager||
       !pc_tooltip_field(read,manager,0x178,&current_hover,8)||current_hover!=widget||!pc_tooltip_field(read,base,0x2d128f8,xy,sizeof xy)||xy[0]!=point.x||xy[1]!=point.y||
       !pc_tooltip_field(read,base,0x2d128ec,&native_ec,1)||native_ec)return 0;
    return !dm_padmode_busy()&&dm_uiscreen_ingame()&&atomic_load(&g_load_up)<=0&&epoch==atomic_load(&g_pc_tabbar_epoch)&&generation==atomic_load(&g_ui_gen)&&
        pc_options_event_allowed()&&pointer.root==root&&pointer.epoch==epoch&&pointer.generation==generation&&pointer.x==point.x&&pointer.y==point.y;
}
static int pc_item_tooltip_route(module_t *m,uint64_t root,uint64_t widget,uint64_t caller,int desktop,uint64_t f0,pc_tooltip_reader read) {
    if(!desktop||!f0||!pc_item_tooltip_abi(m)||!pc_item_tooltip_context(m,root,widget,read))return 0;
    uint64_t base=(uint64_t)m->base,vt;uint32_t call_word=0;size_t call=0;
    if(!pc_tooltip_field(read,widget,0,&vt,8))return 0;
    if(vt==base+DM_VT_SLOT&&caller==base+0x3d4fe4){call=0x3d4fe0;call_word=0x97ffde27;}
    if(vt==base+DM_VT_SLOT_B){if(caller==base+0x3b0910){call=0x3b090c;call_word=0x94006fdc;}else if(caller==base+0x3b0b68){call=0x3b0b64;call_word=0x94006f46;}else if(caller==base+0x3b0fa4){call=0x3b0fa0;call_word=0x94006e37;}else if(caller==base+0x3b1d28){
        static const uint32_t draw[]={0xa90167fa,0xa9025ff8,0xa90357f6};
        if(memcmp(m->base+0x3b1cb8,draw,sizeof draw))return 0;call=0x3b1d24;call_word=0x94006ad6;
    }}
    if(caller==base+0x3dd184||caller==base+0x407c38||caller==base+0x409490||caller==base+0x4094a0||caller==base+0x40b08c){
        static const uint32_t selling[]={0xaa1303e0,0xaa1f03e1,0x91156274,0x97ffbdbf,0xaa0003e1,0xaa1403e0,0x52800022,0x97fe4fb4};
        static const uint32_t buying[]={0xaa1303e0,0xaa1f03e1,0x91156274,0x97ff1312,0xaa0003e1,0xaa1403e0,0x2a1f03e2,0x97fda507};
        static const uint32_t right[]={0xaa1503e0,0xaa1f03e1,0x97ff0cfc,0xb4ffed00,0xaa1503e0,0xaa1f03e1,0x97ff0cf8,0xb4ffed80,0xaa0003e1,0xaa1603e0,0x2a1f03e2,0x2a1f03e3,0x52800024,0x2a1f03e5,0x9400aa65};
        static const uint32_t left[]={0xaa1503e0,0xaa1f03e1,0x97ff05fd,0x71000a9f,0x1a9f17e4,0xaa1603e1,0xaa0003e2,0x2a1f03e3,0x2a1f03e5,0x97ffff14};
        int sell=caller==base+0x3dd184,price=sell||caller==base+0x407c38,domain=pc_tooltip_domain(read,root,widget);uint8_t trading;uint32_t npc,interacting;
        if(price){if(memcmp(m->base+(sell?0x3dd174:0x407c28),sell?selling:buying,sizeof selling))return 0;}
        else if(vt!=base+DM_VT_SLOT_B||
                (caller==base+0x40b08c?memcmp(m->base+0x40b080,left,sizeof left):memcmp(m->base+0x409484,right,sizeof right)))return 0;
        if(domain!=(sell?PC_TIP_INVENTORY:PC_TIP_VENDOR)||
           !pc_tooltip_field(read,base,0x2d5ff53,&trading,1)||!trading||
           !pc_tooltip_field(read,base,0x2d5ec00,&interacting,4)||!interacting||
           !pc_tooltip_field(read,base,0x2d5ebfc,&npc,4)||npc==UINT32_MAX||!pc_tooltip_unit(read,base,1,npc))return 0;
        return pc_item_tooltip_context(m,root,widget,read);
    }
    if(!call||*(uint32_t*)(m->base+call)!=call_word)return 0;
    return 1;
}
void dm_ui_pc_item_tooltip(uint64_t *regs) {
    if(!regs||!regs[9]||!desktop())return;module_t *m=main_module();uint64_t root=0;
    if(!m||m->size<0x2f05460||!pc_tooltip_field(pc_tooltip_read,(uint64_t)m->base,0x2f05458,&root,8))return;
    if(pc_item_tooltip_route(m,root,regs[20],regs[8],1,regs[9],pc_tooltip_read))regs[9]=0;
}
static int pc_grid_hover_abi(module_t *m) {
    static const uint32_t prologue[]={0xf81b0ff9,0xa9015ff8,0xa90257f6,0xa9034ff4};
    static const uint32_t before[]={0xd00112a8,0x3963c108,0x34000088,0xd00112a8,0x3963b108,0x340001e8,0xf940ba74};
    static const uint32_t after[]={0x3945a268,0x34000168,0xf9400008,0x9105c275};
    return m&&m->base&&m->size>=0x2f05460&&!memcmp(m->base+0xabcc50,prologue,sizeof prologue)&&
        !memcmp(m->base+0xabcc88,before,sizeof before)&&!memcmp(m->base+0xabcca8,after,sizeof after)&&
        *(uint32_t*)(m->base+0xabccd4)==0xa9007ebf&&*(uint32_t*)(m->base+0xabccd8)==0xf940ca69;
}
static int pc_grid_hover_ancestors(uint64_t root,uint64_t widget,pc_tooltip_reader read) {
    uint64_t node=widget;
    for(unsigned i=0;node&&i<64;i++){
        uint8_t flags[2];uint64_t parent;
        if(!pc_tooltip_field(read,node,0x50,flags,2)||!flags[0]||!flags[1])return 0;
        if(node==root)return 1;
        if(!pc_tooltip_field(read,node,0x30,&parent,8)||parent==node)return 0;
        node=parent;
    }
    return 0;
}
static int pc_grid_hover_route(module_t *m,const uint64_t *regs,int desktop,pc_tooltip_reader read) {
    if(!desktop||!regs||!read||!pc_grid_hover_abi(m))return 0;
    uint32_t generation=atomic_load(&g_ui_gen),epoch=atomic_load(&g_pc_tabbar_epoch);
    uint64_t base=(uint64_t)m->base,root,manager,hover,container,vt;uint8_t f0,ec;
    if(!pc_tooltip_field(read,base,0x2f05458,&root,8)||!root||
       !pc_tooltip_field(read,root,0xd0,&manager,8)||manager!=regs[19]||
       !pc_tooltip_field(read,manager,0,&vt,8)||vt!=base+0x20b7f88||
       !pc_tooltip_field(read,manager,0x178,&hover,8)||!hover||hover!=regs[0]||
       !pc_tooltip_field(read,manager,0x170,&container,8)||container!=regs[20]||
       !pc_tooltip_field(read,hover,0,&vt,8)||vt!=base+DM_VT_SLOT_B||
       !pc_tooltip_field(read,base,0x2d128f0,&f0,1)||f0||
       !pc_tooltip_field(read,base,0x2d128ec,&ec,1)||ec)return 0;
    if(!pc_options_event_allowed()||!pc_grid_hover_ancestors(root,hover,read)||!pc_item_tooltip_context(m,root,hover,read))return 0;
    unsigned budget=2048;
    if(container&&(!pc_tooltip_owned(read,root,container,0,&budget)||!pc_grid_hover_ancestors(root,container,read)))return 0;
    uint64_t current_container=0;uint8_t current_f0=1,current_ec=1;
    return pc_options_event_allowed()&&pc_grid_hover_ancestors(root,hover,read)&&
        pc_tooltip_field(read,manager,0x170,&current_container,8)&&current_container==container&&
        pc_tooltip_field(read,base,0x2d128f0,&current_f0,1)&&!current_f0&&
        pc_tooltip_field(read,base,0x2d128ec,&current_ec,1)&&!current_ec&&
        generation==atomic_load(&g_ui_gen)&&epoch==atomic_load(&g_pc_tabbar_epoch)&&
        pc_item_tooltip_context(m,root,hover,read)&&!m->base[0x2d128f0]&&!m->base[0x2d128ec]&&
        generation==atomic_load(&g_ui_gen)&&epoch==atomic_load(&g_pc_tabbar_epoch);
}
void dm_ui_pc_grid_hover(uint64_t *regs) {
    if(regs)regs[8]=dm_ui_pc_skill_grid_hover(regs)||pc_grid_hover_route(main_module(),regs,desktop(),pc_tooltip_read);
}
static int pc_item_left_route(module_t *m,uint64_t root,uint64_t widget,const uint64_t *message,int desktop,const pc_item_left_api *api) {
    int dm_padmode_busy(void);
    if(!desktop||!pc_options_event_allowed()||pointer.event_scope!=1||!pc_item_left_abi(m)||!root||!widget||!message||!api||!api->hit||!api->cell||!api->accept||!api->activate||!api->live)return 0;
    if(message[0]!=ui_hash("InputMessage")||message[1]!=ui_hash("LeftMouseButtonDown")||!message[2]||*(uint64_t*)(m->base+0x2f05458)!=root)return 0;
    if(!dm_uiscreen_ingame()||!*(uint8_t*)(root+0xd9)||atomic_load(&g_load_up)||dm_padmode_busy()||*(uint8_t*)(root+0xb8)||!ui_has(root,widget,0))return 0;
    if(!*(uint8_t*)(widget+0x50)||!*(uint8_t*)(widget+0x51))return 0;
    uint64_t manager=*(uint64_t*)(root+0xd0);if(!manager||!ui_has(root,manager,0)||*(uint64_t*)manager!=(uint64_t)m->base+0x20b7f88||*(uint64_t*)(manager+0x178)!=widget)return 0;
    if(!api->live(m,widget)||!pc_item_context(m,root,widget))return 0;
    uint64_t xy;memcpy(&xy,(const uint8_t*)(uintptr_t)message[2]+4,8);
    if(!(api->hit(widget,xy)&1)||!pc_item_context(m,root,widget))return 0;
    uint64_t cell=api->cell(widget,xy);if(cell==UINT64_MAX||!pc_item_context(m,root,widget)||!(api->accept(widget,cell)&1))return 0;
    if(!pc_options_event_allowed()||*(uint64_t*)(m->base+0x2f05458)!=root||!ui_has(root,widget,0)||*(uint8_t*)(root+0xb8))return 0;
    manager=*(uint64_t*)(root+0xd0);if(!manager||!ui_has(root,manager,0)||*(uint64_t*)(manager+0x178)!=widget)return 0;
    if(!api->live(m,widget)||!pc_item_context(m,root,widget))return 0;
    if(api->modified&&api->modified(m,widget,cell,0,g_pc_options_event_scope.modifiers))return 1;
    if(!pc_item_context(m,root,widget))return 0;
    if(api->remember)api->remember(m,root,widget,cell,xy);
    if(!pc_item_context(m,root,widget))return 0;
    if(api->remember)pc_item_trace("primary",widget,cell,g_pc_options_event_scope.modifiers);
    api->activate(widget,cell);return 1;
}
static int pc_item_left_functions(module_t *m,uint64_t widget,pc_item_left_api *api) {
    if(!m||!m->base||!widget||!api)return 0;
    uint64_t base=(uint64_t)m->base,vt=*(uint64_t*)widget;
    if(vt<base||vt-base>m->size||m->size-(vt-base)<0x100)return 0;
    uint64_t fn[4]={*(uint64_t*)(vt+0x48),*(uint64_t*)(vt+0x98),*(uint64_t*)(vt+0x90),*(uint64_t*)(vt+0xf8)};
    for(unsigned i=0;i<4;i++)if(fn[i]<base||fn[i]-base>=m->text_end)return 0;
    *api=(pc_item_left_api){(int(*)(uint64_t,uint64_t))fn[0],(uint64_t(*)(uint64_t,uint64_t))fn[1],(int(*)(uint64_t,uint64_t))fn[2],(void(*)(uint64_t,uint64_t))fn[3],pc_item_native_live,pc_item_modifier,pc_item_remember};return 1;
}
void dm_ui_pc_item_left(uint64_t *regs) {
    if(!regs)return;
    module_t *m=main_module();uint64_t widget=regs[20],root=pc_item_left_abi(m)?*(uint64_t*)(m->base+0x2f05458):0;pc_item_left_api api;
    regs[8]=0;
    if(!desktop()||!root||!widget||!ui_has(root,widget,0)||!pc_item_left_functions(m,widget,&api))return;
    regs[8]=pc_item_left_route(m,root,widget,(const uint64_t*)(uintptr_t)regs[22],1,&api);
}
static int pc_item_right_route(module_t *m,uint64_t root,uint64_t widget,const uint64_t *message,uint64_t cell,int desktop,const pc_item_left_api *api) {
    if(!desktop||!pc_options_event_allowed()||pointer.event_scope!=1||!(g_pc_options_event_scope.modifiers&0x1000)||!pc_item_left_abi(m)||!root||!widget||!message||!api||!api->modified||!api->live||!api->hit||!api->accept)return 0;
    if(*(uint64_t*)(m->base+0x2f05458)!=root||!ui_has(root,widget,0)||message[0]!=ui_hash("InputMessage")||message[1]!=ui_hash("RightMouseButtonDown")||!message[2]||cell==UINT64_MAX)return 0;
    if(!dm_uiscreen_ingame()||!*(uint8_t*)(root+0xd9)||atomic_load(&g_load_up)||*(uint8_t*)(root+0xb8)||!*(uint8_t*)(widget+0x50)||!*(uint8_t*)(widget+0x51))return 0;
    uint64_t manager=*(uint64_t*)(root+0xd0);if(!manager||!ui_has(root,manager,0)||*(uint64_t*)manager!=(uint64_t)m->base+0x20b7f88||*(uint64_t*)(manager+0x178)!=widget)return 0;
    if(!api->live(m,widget)||!pc_item_context(m,root,widget))return 0;
    uint64_t xy;memcpy(&xy,(const uint8_t*)(uintptr_t)message[2]+4,8);
    if(!(api->hit(widget,xy)&1)||!pc_item_context(m,root,widget)||!(api->accept(widget,cell)&1)||!pc_item_context(m,root,widget))return 0;
    if(*(uint64_t*)(m->base+0x2f05458)!=root||!ui_has(root,widget,0)||*(uint8_t*)(root+0xb8))return 0;
    manager=*(uint64_t*)(root+0xd0);if(!manager||!ui_has(root,manager,0)||*(uint64_t*)(manager+0x178)!=widget)return 0;
    return api->modified(m,widget,cell,1,g_pc_options_event_scope.modifiers);
}
void dm_ui_pc_item_right(uint64_t *regs) {
    if(!regs)return;
    module_t *m=main_module();uint64_t widget=regs[20],cell=regs[1],root=pc_item_left_abi(m)?*(uint64_t*)(m->base+0x2f05458):0;
    regs[8]=0;pc_item_left_api api;
    if(desktop()&&root&&widget&&ui_has(root,widget,0)&&pc_item_left_functions(m,widget,&api))regs[8]=pc_item_right_route(m,root,widget,(const uint64_t*)(uintptr_t)regs[22],cell,1,&api);
}
static int pc_held_draw_abi(module_t *m) {
    static const uint32_t draw[]={0xd104c3ff,0xa90d6ffc,0xa90e67fa,0xa90f5ff8},item[]={0xd10403ff,0xfd005be8,0xa90c5ff8,0xa90d57f6},project[]={0xf000f6e8,0x9128e108,0x2d400500,0xf0016628},before[]={0xb40000a0,0x97e34dc6,0x14000004};
    return m&&m->base&&m->size>=0x2f05460&&!memcmp(m->base+0xabcf34,draw,sizeof draw)&&!memcmp(m->base+0x2ab480,item,sizeof item)&&!memcmp(m->base+0xabd41c,before,sizeof before)&&!memcmp(m->base+0x23e760,project,sizeof project);
}
void dm_ui_pc_held_draw(uint64_t *regs) {
    module_t *m=main_module();if(!regs||!pc_options_event_allowed()||!pointer.ingame||pointer.loading||pointer.busy||!pc_held_draw_abi(m))return;
    dm_pc_items_snapshot saved=pointer;uint32_t token=atomic_load(&cancellation);uint64_t root=0,held=0;uint8_t hd=0;int dimensions[2];
    if(!pc_tooltip_field(pc_tooltip_read,(uint64_t)m->base,0x2f05458,&root,8)||root!=saved.root||
       !pc_tooltip_field(pc_tooltip_read,(uint64_t)m->base,0x2d12920,&held,8)||!held||
       !pc_tooltip_field(pc_tooltip_read,root,0xb9,&hd,1)||!!hd!=!!saved.hd||
       !pc_tooltip_field(pc_tooltip_read,(uint64_t)m->base,0x211da38,dimensions,sizeof dimensions)||dimensions[0]!=saved.width||dimensions[1]!=saved.height||
       saved.width<=0||saved.height<=0||saved.x<0||saved.y<0||saved.x>=saved.width||saved.y>=saved.height)return;
    uint32_t type;if(!pc_tooltip_field(pc_tooltip_read,held,0,&type,4)||type!=4)return;
    uint64_t point=(uint32_t)saved.x|((uint64_t)(uint32_t)saved.y<<32);
    if(!hd){int classic[2];if(saved.classic_width<=0||saved.classic_height<=0||!pc_tooltip_field(pc_tooltip_read,(uint64_t)m->base,0x2080c08,classic,sizeof classic)||classic[0]!=saved.classic_width||classic[1]!=saved.classic_height)return;point=((uint64_t(*)(uint64_t))(m->base+0x23e760))(point);}
    uint64_t current_root=0;
    if(pc_tooltip_field(pc_tooltip_read,(uint64_t)m->base,0x2f05458,&current_root,8)&&current_root==root&&token==atomic_load(&cancellation)&&pc_options_event_allowed()&&pointer.root==root&&pointer.epoch==saved.epoch&&pointer.generation==saved.generation&&pointer.x==saved.x&&pointer.y==saved.y)regs[0]=point;
}
static int pc_npc_click_abi(module_t *m) {
    static const uint32_t panel[]={0xd10103ff,0xa90157f6,0xa9024ff4,0xa9037bfd},hit[]={0xf9400268,0xf9402508,0xd63f0100},children[]={0x39414268,0x34000968,0xf9403269,0xb4000929},row[]={0xa9bd57f6,0xa9014ff4,0xa9027bfd,0x910083fd};
    return m&&m->base&&m->size>=0x2f05460&&!memcmp(m->base+0x3dadc8,panel,sizeof panel)&&!memcmp(m->base+0x3db0a8,hit,sizeof hit)&&!memcmp(m->base+0x3db198,children,sizeof children)&&!memcmp(m->base+0xaca6e8,row,sizeof row);
}
static int pc_npc_click_guard(module_t *m,uint64_t root,uint64_t panel,const uint64_t *message,int inside,int desktop) {
    int dm_padmode_busy(void);
    if(!desktop||!(inside&1)||pointer.event_scope!=1||!pc_options_event_allowed()||!pc_npc_click_abi(m)||!root||!panel||!message||*(uint64_t*)(m->base+0x2f05458)!=root)return 0;
    if(message[0]!=ui_hash("InputMessage")||message[1]!=ui_hash("LeftMouseButtonDown")||!message[2]||*(uint8_t*)(root+0xb8)||!dm_uiscreen_ingame()||!*(uint8_t*)(root+0xd9)||atomic_load(&g_load_up)||dm_padmode_busy())return 0;
    if(!ui_has(root,panel,0)||*(uint64_t*)panel!=(uint64_t)m->base+0x208a2b0||!*(uint8_t*)(panel+0x50)||!*(uint8_t*)(panel+0x51))return 0;
    uint64_t manager=*(uint64_t*)(root+0xd0);if(!manager||!ui_has(root,manager,0)||*(uint64_t*)manager!=(uint64_t)m->base+0x20b7f88)return 0;
    uint64_t row=*(uint64_t*)(manager+0x178);if(!row||!ui_has(panel,row,0)||*(uint64_t*)row!=(uint64_t)m->base+0x20b84d0||!*(uint8_t*)(row+0x50)||!*(uint8_t*)(row+0x51))return 0;
    uint64_t list=*(uint64_t*)(panel+0x248),callback=*(uint64_t*)(row+0x568);
    if(!list||!ui_has(panel,list,0)||!ui_has(list,row,0)||*(uint64_t*)(row+0x598)!=list||!callback)return 0;
    return *(uint64_t*)callback==ui_hash("DialogMessage")&&*(uint64_t*)(callback+8)==ui_hash("ListItemClicked");
}
void dm_ui_pc_npc_click(uint64_t *regs) {
    if(!regs)return;
    module_t *m=main_module();uint64_t root=pc_npc_click_abi(m)?*(uint64_t*)(m->base+0x2f05458):0;
    int bridge=pc_npc_click_guard(m,root,regs[19],(const uint64_t*)(uintptr_t)regs[20],(int)regs[0],desktop());
    regs[8]=bridge;
}
static int pc_item_hold_abi(module_t *m) {
    static const uint32_t entry[]={0xd10203ff,0xf90023f7,0xa90557f6,0xa9064ff4};
    static const uint32_t after[]={0x911dc108,0x08dffd08,0x36002468,0xd0014ac8};
    return m&&m->base&&m->size>=0x2f05460&&!memcmp(m->base+0x3b855c,entry,sizeof entry)&&!memcmp(m->base+0x3b864c,after,sizeof after);
}
static int pc_item_hold_tree(module_t *m,uint64_t widget,int depth) {
    if(!widget||depth>16||!*(uint8_t*)(widget+0x50)||!*(uint8_t*)(widget+0x51))return 0;
    if(pc_item_family(m,widget))return 1;
    uint64_t count=*(uint64_t*)(widget+0x60),children=*(uint64_t*)(widget+0x58);if(!children||count>256)return 0;
    for(uint64_t i=0;i<count;i++)if(pc_item_hold_tree(m,*(uint64_t*)(children+8*i),depth+1))return 1;
    return 0;
}
static int pc_item_hold_guard(module_t *m,uint64_t root,uint64_t queue,uint64_t inventory,uint64_t held,int desktop) {
    // 被动保护不依赖指针：失焦/离开视口不能触发自动丢弃，显式 DROP 仍由原版处理。
    // Passive retention is pointer-independent: blur/viewport exit cannot trigger auto-drop; explicit DROP remains native.
    int dm_padmode_busy(void);
    if(!desktop||!pc_item_hold_abi(m)||!root||queue!=(uint64_t)m->base+0x2d5b740||!inventory||!held||*(uint64_t*)(m->base+0x2f05458)!=root)return 0;
    if(*(uint32_t*)inventory!=0x1020304||*(uint64_t*)(inventory+0x40)!=held||*(uint32_t*)held!=4||!dm_uiscreen_ingame()||!*(uint8_t*)(root+0xd9)||atomic_load(&g_load_up)||dm_padmode_busy())return 0;
    uint64_t count=*(uint64_t*)(root+0x60),children=*(uint64_t*)(root+0x58);if(!children||count>256)return 0;
    for(uint64_t i=0;i<count;i++){
        uint64_t panel=*(uint64_t*)(children+8*i);if(!panel)continue;
        const char *name=*(const char**)(panel+8);if(name&&!strcmp(name,"HUDPanel"))continue;
        if(pc_item_hold_tree(m,panel,0))return 1;
    }
    return 0;
}
void dm_ui_pc_item_hold(uint64_t *regs) {
    if(!regs)return;
    module_t *m=main_module();uint64_t root=pc_item_hold_abi(m)?*(uint64_t*)(m->base+0x2f05458):0,held=regs[8];
    int protect=pc_item_hold_guard(m,root,regs[19],regs[23],held,desktop());
    regs[8]=protect;
}
static int pc_personal_page_direction(const char *category,const char *action,const char *arg) {
    if(!category||strcmp(category,"PanelManager")||!action||strcmp(action,"ClosePanel")||!arg)return 0;
    return !strcmp(arg,"TouchPersonalStashPrevious")?-1:!strcmp(arg,"TouchPersonalStashNext")?1:0;
}
static int pc_personal_page_route(module_t *m,uint64_t root,const char *category,const char *action,const char *arg,int desktop,pc_tooltip_reader read,void (*flip)(int)) {
    int dm_padmode_busy(void);
    if(!desktop||!read||!flip)return 0;
    int direction=pc_personal_page_direction(category,action,arg);
    if(!direction||!m||!m->base||m->size<0x2f05460||!root||*(uint64_t*)(m->base+0x2f05458)!=root||!dm_uiscreen_ingame()||dm_padmode_busy()||atomic_load(&g_load_up)>0||!pc_options_event_allowed())return 0;
    uint32_t generation=atomic_load(&g_ui_gen),epoch=atomic_load(&g_pc_tabbar_epoch);
    uint64_t bank=ui_panel(root,"BankExpansionLayout"),tabs=bank?ui_find(bank,"BankTabs",0):0;
    if(!*(uint8_t*)(root+0xd9)||!bank||!*(uint8_t*)(bank+0x50)||!*(uint8_t*)(bank+0x51)||!tabs||!*(uint8_t*)(tabs+0x50)||!*(uint8_t*)(tabs+0x51)||*(uint64_t*)tabs!=(uint64_t)m->base+0x20ba3a8||TAB_SEL(tabs)!=0||modal_any(root,1,NULL))return 0;
    uint64_t player=pc_tooltip_local_player(read,(uint64_t)m->base,*(uint32_t*)(m->base+0x2d20e4c)),inventory=pc_tooltip_inventory(read,player),held=0;
    if(!inventory||!pc_tooltip_field(read,inventory,0x40,&held,8)||held)return 0;
    if(epoch!=atomic_load(&g_pc_tabbar_epoch)||generation!=atomic_load(&g_ui_gen)||*(uint64_t*)(m->base+0x2f05458)!=root||!pc_options_event_allowed()||dm_padmode_busy()||!dm_uiscreen_ingame()||atomic_load(&g_load_up)>0||!ui_has(root,bank,0)||!ui_has(bank,tabs,0)||TAB_SEL(tabs)!=0)return 0;
    flip(direction);return 1;
}
void dm_pc_items_message(const char *category,const char *action,const char *arg) {
    if(!pc_personal_page_direction(category,action,arg)||!desktop())return;
    module_t *m=main_module();if(!m||m->size<0x2f05460)return;
    void dm_pages_request_flip(int);
    pc_personal_page_route(m,*(uint64_t*)(m->base+0x2f05458),category,action,arg,1,pc_tooltip_read,dm_pages_request_flip);
}

int dm_pc_items_enabled(void) { return desktop(); }
void dm_pc_items_publish(const dm_pc_items_snapshot *sample) {
    uint32_t token=atomic_load(&cancellation);
    if(token!=published_cancellation){if(!awaiting_epoch||(int32_t)(pointer.epoch-rearm_epoch)>0)rearm_epoch=pointer.epoch;awaiting_epoch=1;}
    int stale=sample&&awaiting_epoch&&(int32_t)(sample->epoch-rearm_epoch)<=0;
    if(sample&&awaiting_epoch&&!stale)awaiting_epoch=0;
    pointer=sample?*sample:(dm_pc_items_snapshot){0};if(stale)pointer.valid=0;published_cancellation=token;
    atomic_store(&g_ui_gen,pointer.generation);atomic_store(&g_pc_tabbar_epoch,pointer.epoch);atomic_store(&g_load_up,pointer.loading);
    g_pc_options_event_scope.modifiers=pointer.event_scope==1?(int)pointer.modifiers:0;
}
void dm_pc_items_cancel(void) { atomic_fetch_add(&cancellation,1); }
uint32_t dm_pc_items_vault_drop(void) { return pc_options_event_allowed()?(uint32_t)atomic_load(&g_vault_drop):0; }
void dm_pc_items_pump(void) {
    if(!desktop())return;
    module_t *m=main_module();if(!m||!pc_options_event_allowed()){atomic_store(&g_vault_drop,0);dm_vault_ui_returning(0);memset(&g_pc_item_drag,0,sizeof g_pc_item_drag);return;}
    pc_item_drag_pump(m,pointer.root);
}
int dm_pc_items_pointer(int phase,int x,int y) {
    if(!desktop())return 0;
    if(phase==DM_UP_END||phase==UI_POINTER_CANCEL){dm_pc_items_cancel();atomic_store(&g_vault_drop,0);dm_vault_ui_returning(0);memset(&g_pc_item_drag,0,sizeof g_pc_item_drag);dm_vault_ui_pointer(-1,0,0);return 0;}
    module_t *m=main_module();if(!m||!pc_options_event_allowed())return 0;
    if(pc_item_pointer_before(m,pointer.root,phase,x,y))return 1;
    if(phase!=DM_UP_LDOWN&&phase!=DM_UP_LUP&&phase!=DM_UP_MOVE)return 0;
    // Vault 不持有跨 root/epoch 的捕获；只在游戏线程取消它。
    // Vault never carries a capture across root/epoch changes; cancel it on the game thread only.
    uint32_t serial=atomic_load(&cancellation);
    if(vault_context.root!=pointer.root||vault_context.generation!=pointer.generation||vault_context.epoch!=pointer.epoch||vault_context.cancellation!=serial){
        dm_vault_ui_pointer(-1,0,0);
        vault_context.root=pointer.root;vault_context.generation=pointer.generation;vault_context.epoch=pointer.epoch;vault_context.cancellation=serial;
    }
    return pc_options_event_allowed()&&dm_vault_ui_pointer(phase,x,y);
}
void dm_pc_items_pointer_after(int phase,int x,int y) { module_t *m=main_module();if(m&&pc_options_event_allowed())pc_item_pointer_after(m,pointer.root,phase,x,y); }
