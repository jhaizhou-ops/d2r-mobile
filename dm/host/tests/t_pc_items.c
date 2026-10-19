// PC 物品/NPC 边界回归；宿主策略执行原函数体，guest 调用使用显式测试替身。
// PC item/NPC boundary regression; host policy runs the production bodies, guest calls use explicit test doubles.
#define DM_PC_ITEMS_TEST_PROFILE 1
#include "../dm_pc_items.c"
#undef dm_padmode_busy
module_t g_mod[MAX_MOD];int g_nmod;
static int enabled=1,in_game=1,transition,panels,checks,failures,flips,activations,hits,cells,modified_calls,merc_calls,merc_col,merc_row,vault_capture,vault_activations;
int dm_ui_pc_desktop(void){
#ifdef DM_TEST_PC_ITEMS_DISABLED
 return 0;
#else
 return enabled;
#endif
}
int dm_uiscreen_ingame(void){return in_game;}int dm_padmode_busy(void){return transition;}int dm_ui_panel_state(void){return panels;}
void dm_vault_ui_returning(uint32_t id){(void)id;}int dm_vault_ui_pointer(int phase,int x,int y){(void)x;(void)y;if(phase<0)vault_capture=0;else if(phase==DM_UP_LDOWN)vault_capture=1;else if(phase==DM_UP_LUP&&vault_capture){vault_capture=0;vault_activations++;}return 0;}
int dm_vault_ui_over(int x,int y){(void)x;(void)y;return 0;}
void dm_pages_request_flip(int d){flips+=d;}void dm_belt_use_merc(int c,int r){merc_calls++;merc_col=c;merc_row=r;}void dm_vault_request_deposit(uint32_t id){(void)id;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL: %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((uint8_t*)(p)+(o))=(uint64_t)(v))
static uint64_t address(void*p){return(uint64_t)(uintptr_t)p;}
_Alignas(16) static uint8_t root[0x200],manager[0x200],panel[0x300],widget[0x700],list[0x600],row[0x600],player[0x200],inventory[0x100],held[0x100];
static uint64_t children[3],leaves[2],rows[1],point,target_cell,received_cell;
static dm_pc_items_snapshot sample;
static void publish(void){dm_pc_items_publish(&sample);}
static int hit(uint64_t w,uint64_t xy){(void)w;hits++;return xy==point;}
static uint64_t cell(uint64_t w,uint64_t xy){(void)w;(void)xy;cells++;return target_cell;}
static int accept(uint64_t w,uint64_t c){(void)w;return c!=UINT64_MAX;}
static void activate(uint64_t w,uint64_t c){(void)w;activations++;received_cell=c;}
static int live(module_t*m,uint64_t w){(void)m;(void)w;return 1;}
static int modified(module_t*m,uint64_t w,uint64_t c,int right,int mods){(void)m;(void)w;(void)c;(void)right;modified_calls++;return mods!=0;}
static void policy_cases(void){
 const int cases[][7]={
 {0,0,0,0,0,0,PC_ITEM_NORMAL},{0,0x1000,0,0,0,0,PC_ITEM_BELT},
 {0,0x2000,0,0,0,DM_PS_VENDOR,PC_ITEM_SELL},{1,0x1000,0,0,1,DM_PS_VENDOR,PC_ITEM_BUY_BULK},
 {1,0x1000,0,2,0,0,PC_ITEM_MERC},{0,0x2000,0,3,0,0,PC_ITEM_TO_BAG},
 {0,0x2000,0,4,0,DM_PS_BANK,PC_ITEM_TO_BAG},{0,0x2000,0,0,0,DM_PS_CUBE,PC_ITEM_TO_CUBE},
 {0,0x2000,0,0,0,DM_PS_BANK,PC_ITEM_TO_BANK},{0,0x2000,0,0,0,DM_PS_VAULT,PC_ITEM_TO_VAULT},
 {0,0x2000,1,0,0,DM_PS_VENDOR,PC_ITEM_NORMAL},{1,0,0,2,0,0,PC_ITEM_NORMAL}};
 for(unsigned i=0;i<sizeof cases/sizeof*cases;i++){const int*c=cases[i];CHECK(pc_item_modifier_plan(c[0],c[1],c[2],c[3],c[4],c[5])==c[6],"modifier policy preserves native domain/context");}
 pc_item_drag drag={.id=77,.pending=1,.wait=3};CHECK(!pc_item_drag_take(&drag,1,0)&&drag.wait==2,"async release waits for server acknowledgement");
 CHECK(pc_item_drag_take(&drag,1,77)&&!drag.pending,"matching held item permits one placement");CHECK(!pc_item_drag_take(&drag,1,77),"acknowledgement does not duplicate placement");
 drag=(pc_item_drag){.id=77,.pending=1};CHECK(!pc_item_drag_take(&drag,1,88)&&!drag.id,"different held item cancels release");
 drag=(pc_item_drag){.id=77,.pending=1};CHECK(!pc_item_drag_take(&drag,0,77)&&!drag.id,"invalid context cancels release");
 drag=(pc_item_drag){.id=77,.pending=1,.wait=1};CHECK(!pc_item_drag_take(&drag,1,0)&&!drag.pending,"bounded acknowledgement timeout");
}
// 执行生产修改键编排；只替换原版域函数，不写输入模块私有状态。
// Run production modifier orchestration, doubling only native domain calls without input-module private state.
_Alignas(16) static uint8_t action_item[0x100],action_data[0x100];
static int hand_occupied,trade_calls,sell_arg,bulk_arg,transfer_calls,destination,belt_calls,deposits,belt_ok=1,merc_ok=1;
static uint64_t action_player(uint32_t index){CHECK(index==0,"native player getter receives local index");return address(player);}
static uint64_t action_held(void){return hand_occupied?address(held):0;}
static uint64_t action_query(uint64_t w,uint64_t *cell){(void)w;(void)cell;return address(action_item);}
static int action_trade(uint64_t p,uint64_t item,int sell,int bulk,int one,int reserved){CHECK(p==address(player)&&item==address(action_item)&&one==1&&!reserved,"native trade arguments preserved");trade_calls++;sell_arg=sell;bulk_arg=bulk;return 1;}
static int action_belt(uint64_t item){(void)item;return belt_ok;}
static int action_merc(uint64_t item){(void)item;return merc_ok;}
static void action_put(uint64_t item,uint64_t p,uint8_t page,int auto_slot,void *target){CHECK(item==address(action_item)&&p==address(player)&&page==0&&auto_slot==1&&target,"belt uses native auto-slot descriptor");belt_calls++;}
static void action_transfer(uint64_t w,uint64_t *cell,int page){CHECK(w==address(widget)&&cell,"native transfer receives current widget and cell");transfer_calls++;destination=page;}
static void action_deposit(uint32_t id){CHECK(id==77,"vault receives exact item identity");deposits++;}
static void modifier_cases(module_t *m){
 pc_item_modifier_api api={action_player,action_held,action_query,action_trade,action_belt,action_merc,action_put,action_transfer,action_deposit};
 Q(action_item,0x10,action_data);*(uint32_t*)(action_item+8)=77;*(uint32_t*)(m->base+0x2d20e4c)=0;
 panels=DM_PS_BANK;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x2000,&api)&&transfer_calls==1&&destination==4,"Ctrl inventory to bank invokes D8 once");
 action_data[0x55]=3;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x2000,&api)&&transfer_calls==2&&!destination,"Ctrl cube to inventory uses destination zero");action_data[0x55]=0;
 panels=DM_PS_CUBE;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x2000,&api)&&transfer_calls==3&&destination==3,"Ctrl inventory to cube uses destination three");
 hand_occupied=1;CHECK(!pc_item_modifier_run(m,address(widget),0,0,0x2000,&api)&&transfer_calls==3,"occupied hand preserves native click behavior");hand_occupied=0;
 panels=0;belt_ok=0;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x1000,&api)&&!belt_calls,"failed belt eligibility consumes without pickup");belt_ok=1;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x1000,&api)&&belt_calls==1,"Shift potion invokes native belt placement once");
 panels=DM_PS_VENDOR;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x2000,&api)&&trade_calls==1&&sell_arg&&!bulk_arg,"Ctrl sale preserves native confirmation policy");
 *(uint32_t*)(widget+0x5c8)=1;*(uint32_t*)(widget+0x5c4)=99;*(uint32_t*)(m->base+0x2d5ebfc)=99;
 CHECK(pc_item_modifier_run(m,address(widget),0,1,0x1000,&api)&&trade_calls==2&&!sell_arg&&bulk_arg,"Shift vendor RMB invokes native bulk purchase once");
 *(uint32_t*)(widget+0x5c4)=100;CHECK(!pc_item_modifier_run(m,address(widget),0,1,0x1000,&api)&&trade_calls==2,"another actor never substitutes active vendor");*(uint32_t*)(widget+0x5c8)=0;
 panels=0;*(uint32_t*)(action_item+0xc)=2;merc_ok=0;CHECK(pc_item_modifier_run(m,address(widget),4,1,0x1000,&api)&&!merc_calls,"merc eligibility failure does not drink potion");merc_ok=1;
 CHECK(pc_item_modifier_run(m,address(widget),4,1,0x1000,&api)&&merc_calls==1&&!merc_col&&merc_row==1,"belt index four maps to merc column zero row one");*(uint32_t*)(action_item+0xc)=0;
 panels=DM_PS_VAULT|DM_PS_BANK;CHECK(pc_item_modifier_run(m,address(widget),0,0,0x2000,&api)&&deposits==1&&transfer_calls==3,"Chronicle deposit never becomes cube transfer");panels=0;
}
int main(int argc,char**argv){
 if(argc!=2){fprintf(stderr,"usage: %s main.orig.bin\n",argv[0]);return 2;}
 FILE*f=fopen(argv[1],"rb");if(!f)return 2;fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 if(size<0x2f05460)return 2;module_t m={.size=(size_t)size,.text_end=0x1b80000};m.base=malloc(m.size);if(!m.base||fread(m.base,1,m.size,f)!=m.size)return 2;fclose(f);
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;uint64_t base=address(m.base),r=address(root),w=address(widget);
 Q(m.base,0x2f05458,r);Q(root,0xd0,manager);Q(root,0x58,children);Q(root,0x60,2);root[0x50]=root[0x51]=root[0xd9]=root[0xb9]=1;
 children[0]=address(manager);children[1]=address(panel);Q(manager,0,base+0x20b7f88);Q(manager,0x178,widget);Q(manager,0x170,panel);
 Q(panel,8,"PlayerInventoryExpansionLayout");Q(panel,0x30,root);Q(panel,0x58,leaves);Q(panel,0x60,1);panel[0x50]=panel[0x51]=1;leaves[0]=w;
 Q(widget,0,base+DM_VT_SLOT_B);Q(widget,0x30,panel);widget[0x50]=widget[0x51]=1;Q(m.base,0x211da38,(uint64_t)900<<32|1600);
 Q(m.base,0x2d128f8,(uint64_t)675<<32|400);Q(m.base,0x2d12920,held);*(uint32_t*)held=4;
 sample=(dm_pc_items_snapshot){.root=r,.epoch=1,.generation=1,.x=400,.y=675,.width=1600,.height=900,.valid=1,.ingame=1,.hd=1,.event_scope=1};publish();
 policy_cases();modifier_cases(&m);CHECK(pc_item_left_abi(&m)&&pc_item_modifier_abi(&m)&&pc_npc_click_abi(&m)&&pc_item_hold_abi(&m)&&pc_held_draw_abi(&m)&&pc_grid_hover_abi(&m)&&pc_item_tooltip_abi(&m),"all ABI guards match original image");
 // 原队列只在物品面板内保护持有物；HUD 腰带不能提供全局保护。
 // The original queue retains held items only under an item panel; the HUD belt cannot provide global protection.
 *(uint32_t*)inventory=0x01020304;Q(inventory,0x40,held);
 uint64_t vt=base+DM_VT_SLOT_B,old_f8=*(uint64_t*)(vt+0xf8);const uint32_t families[]={0x3b3a64,0x3d710c,0x3925a0,0x3afdd4,0x39db7c};
 for(unsigned i=0;i<sizeof families/sizeof *families;i++){Q((void*)vt,0xf8,base+families[i]);CHECK(pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"all typed item families retain held under visible item panel");}Q((void*)vt,0xf8,old_f8);
 panel[0x50]=0;CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"hidden item panel cannot suppress original queue");panel[0x50]=1;
 CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b748,address(inventory),address(held),1),"only original queue instance is eligible");
 Q(inventory,0x40,0);CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"acknowledged empty hand restores original reconciliation");Q(inventory,0x40,held);
 Q((void*)vt,0xf8,base+0x3b3a64);dm_pc_items_cancel();CHECK(pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"blur cancels gestures without automatically dropping held item");sample.valid=0;publish();CHECK(pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"pointer outside viewport does not authorize controller auto-drop");sample.valid=1;sample.epoch++;publish();
 CHECK(!pc_item_hold_guard(&m,1,base+0x2d5b740,address(inventory),address(held),1),"passive retention rejects stale root before dereference");
 transition=1;CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"mode transaction still preserves original reconciliation");transition=0;
 sample.loading=1;publish();CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"loading never uses passive retention");sample.loading=0;publish();
 in_game=0;CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"frontend never uses passive retention");in_game=1;
 *(uint32_t*)held=0;CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"passive retention requires an item unit");*(uint32_t*)held=4;
 *(uint32_t*)inventory=0;CHECK(!pc_item_hold_guard(&m,r,base+0x2d5b740,address(inventory),address(held),1),"passive retention requires original inventory signature");*(uint32_t*)inventory=0x01020304;
 Q((void*)vt,0xf8,old_f8);
 // Vault 接收新上下文前必须取消旧捕获，随后正常新手势仍可使用。
 // Vault must cancel an old capture before a new context, while later fresh gestures still work.
 dm_pc_items_pointer(DM_UP_LDOWN,400,675);dm_pc_items_cancel();sample.epoch++;publish();dm_pc_items_pointer(DM_UP_LUP,400,675);
 CHECK(!vault_activations&&!vault_capture,"rearmed UP cannot activate Vault capture from before blur");
 dm_pc_items_pointer(DM_UP_LDOWN,400,675);sample.generation++;publish();dm_pc_items_pointer(DM_UP_LUP,400,675);
 CHECK(!vault_activations,"generation replacement invalidates Vault capture too");
 dm_pc_items_pointer(DM_UP_LDOWN,400,675);dm_pc_items_pointer(DM_UP_END,0,0);sample.epoch++;publish();
 dm_pc_items_pointer(DM_UP_LDOWN,400,675);dm_pc_items_pointer(DM_UP_LUP,400,675);
 CHECK(vault_activations==1,"END plus publisher epoch bump rearms one fresh physical gesture");
 uint32_t payload[8]={0,400,675};uint64_t message[]={ui_hash("InputMessage"),ui_hash("LeftMouseButtonDown"),address(payload)};point=(uint64_t)675<<32|400;
 pc_item_left_api api={hit,cell,accept,activate,live,NULL,NULL};
 for(int i=0;i<6;i++){target_cell=(uint64_t)(unsigned)i<<32|i;CHECK(pc_item_left_route(&m,r,w,message,1,&api)&&received_cell==target_cell&&activations==i+1,"one native F8 per valid slot, including cell zero");}
 CHECK(!root[0xb8],"host leaves consumed flag to original branch");int before=activations;
 root[0xb8]=1;CHECK(!pc_item_left_route(&m,r,w,message,1,&api)&&activations==before,"already consumed message never activates");root[0xb8]=0;
 target_cell=UINT64_MAX;CHECK(!pc_item_left_route(&m,r,w,message,1,&api),"invalid cell never activates");target_cell=0;
 sample.modifiers=0x2000;publish();api.modified=modified;CHECK(pc_item_left_route(&m,r,w,message,1,&api)&&modified_calls==1&&activations==before,"modifier consumes once without a second pickup");
 message[1]=ui_hash("RightMouseButtonDown");sample.modifiers=0;publish();CHECK(!pc_item_right_route(&m,r,w,message,0,1,&api),"plain RMB preserves original use/equip");
 sample.modifiers=0x1000;publish();CHECK(pc_item_right_route(&m,r,w,message,0,1,&api)&&modified_calls==2,"Shift RMB delegates modifier policy");
 sample.event_scope=0;publish();CHECK(!pc_item_right_route(&m,r,w,message,0,1,&api),"persistent hover cannot invoke a physical modifier");sample.event_scope=1;publish();
 dm_pc_items_cancel();CHECK(!pc_item_right_route(&m,r,w,message,0,1,&api),"cancel invalidates previous published event");publish();CHECK(!pc_item_right_route(&m,r,w,message,0,1,&api),"old queued epoch cannot rearm after cancellation");publish();CHECK(!pc_item_right_route(&m,r,w,message,0,1,&api),"repeated stale publication stays cancelled");sample.epoch++;publish();
 dm_pc_items_cancel();dm_pc_items_publish(NULL);dm_pc_items_cancel();dm_pc_items_publish(NULL);publish();
 CHECK(!pc_item_right_route(&m,r,w,message,0,1,&api),"repeated cancellation across absent snapshots cannot lower epoch fence");sample.epoch++;publish();
 message[1]=ui_hash("LeftMouseButtonDown");api.modified=NULL;
 int old=hits;sample.loading=1;publish();CHECK(!pc_item_left_route(&m,r,w,message,1,&api)&&hits==old,"loading rejects before hit callback");sample.loading=0;publish();
 sample.busy=1;publish();CHECK(!pc_item_left_route(&m,r,w,message,1,&api),"mode transition rejects input");sample.busy=0;publish();
 widget[0x50]=0;CHECK(!pc_item_left_route(&m,r,w,message,1,&api),"hidden slot rejects input");widget[0x50]=1;
 Q(m.base,0x2f05458,0);CHECK(!pc_item_left_route(&m,r,w,message,1,&api),"stale root rejects input");Q(m.base,0x2f05458,r);
 uint32_t old_word=*(uint32_t*)(m.base+0x3ca3b0);*(uint32_t*)(m.base+0x3ca3b0)=0;CHECK(!pc_item_left_route(&m,r,w,message,1,&api),"different native ABI rejects input");*(uint32_t*)(m.base+0x3ca3b0)=old_word;
 uint64_t regs[31]={0};regs[0]=0x1234;dm_ui_pc_held_draw(regs);CHECK(regs[0]==point,"held draw changes only native point argument");
 sample.x=1600;publish();regs[0]=0x1234;dm_ui_pc_held_draw(regs);CHECK(regs[0]==0x1234,"outside viewport preserves original draw argument");sample.x=400;publish();
 dm_pc_items_cancel();regs[0]=0x1234;dm_ui_pc_held_draw(regs);CHECK(regs[0]==0x1234,"blur invalidates held draw");sample.epoch++;publish();
 Q(panel,0,base+0x208a2b0);Q(panel,0x248,list);leaves[0]=address(list);Q(list,0x58,rows);Q(list,0x60,1);rows[0]=address(row);Q(row,0,base+0x20b84d0);row[0x50]=row[0x51]=list[0x50]=list[0x51]=1;
 uint64_t callback[]={ui_hash("DialogMessage"),ui_hash("ListItemClicked")};Q(row,0x568,callback);Q(row,0x598,list);Q(manager,0x178,row);
 regs[0]=1;regs[19]=address(panel);regs[20]=address(message);dm_ui_pc_npc_click(regs);CHECK(regs[8]==1,"live NPC row dispatches original children");
 Q(list,0x60,0);dm_ui_pc_npc_click(regs);CHECK(!regs[8],"stale row/list pointer does not bypass modal consumption");Q(list,0x60,1);
 callback[1]=ui_hash("OtherAction");dm_ui_pc_npc_click(regs);CHECK(!regs[8],"only original ListItemClicked protocol is allowed");callback[1]=ui_hash("ListItemClicked");
 message[1]=ui_hash("RightMouseButtonDown");dm_ui_pc_npc_click(regs);CHECK(!regs[8],"NPC RMB preserves original branch");message[1]=ui_hash("LeftMouseButtonDown");
 sample.valid=0;publish();dm_ui_pc_npc_click(regs);CHECK(!regs[8],"missing physical snapshot preserves native NPC branch");sample.valid=1;publish();
 Q(panel,8,"BankExpansionLayout");Q(panel,0x248,0);leaves[0]=w;
 // BankTabs 需要较大的原版对象，测试显式分配。 | BankTabs needs the larger native object; use an explicitly allocated fixture.
 uint8_t *tabs=calloc(1,0xe00);if(!tabs)return 2;Q(tabs,0,base+0x20ba3a8);Q(tabs,8,"BankTabs");tabs[0x50]=tabs[0x51]=1;leaves[0]=address(tabs);
 *(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=42;Q(m.base,0x2d603a0+8*42,player);*(uint32_t*)player=0;*(uint32_t*)(player+8)=42;Q(player,0x90,inventory);*(uint32_t*)inventory=0x01020304;Q(inventory,0x40,0);
 dm_pc_items_message("PanelManager","ClosePanel","TouchPersonalStashNext");CHECK(flips==1,"personal callback uses existing native category and accepted bus");
 dm_pc_items_message("PanelManager","ClosePanel","TouchPersonalStashPrevious");CHECK(flips==0,"personal directions are inverse");
 *(int32_t*)(tabs+0xdc4)=1;dm_pc_items_message("PanelManager","ClosePanel","TouchPersonalStashNext");CHECK(flips==0,"shared tab never uses personal pager");*(int32_t*)(tabs+0xdc4)=0;
 Q(inventory,0x40,held);dm_pc_items_message("PanelManager","ClosePanel","TouchPersonalStashNext");CHECK(flips==0,"occupied hand blocks personal page flip");Q(inventory,0x40,0);
 enabled=0;publish();uint64_t saved[31];for(int i=0;i<31;i++)regs[i]=0x100+i;memcpy(saved,regs,sizeof regs);dm_ui_pc_item_left(regs);CHECK(!regs[8],"legacy selects original item branch");for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==saved[i],"legacy gate preserves all other registers");
 dm_ui_pc_item_right(regs);CHECK(!regs[8],"legacy RMB selects original virtual");dm_ui_pc_grid_hover(regs);CHECK(!regs[8],"legacy focus preserves original clear");
 free(tabs);free(m.base);printf("PC items: %d checks, %d failures\n",checks,failures);return failures!=0;
}
