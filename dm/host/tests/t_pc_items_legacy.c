// 缺少 PC profile 时的独立链接回归；不需要游戏数据。
// Standalone link regression without a PC profile; no game data required.
#include "../dm_pc_items.h"
#include "../dm.h"
#include <string.h>
module_t g_mod[MAX_MOD];int g_nmod;
int dm_uiscreen_ingame(void){abort();}int dm_padmode_busy(void){abort();}int dm_ui_panel_state(void){abort();}
void dm_belt_use_merc(int c,int r){(void)c;(void)r;abort();}
void dm_vault_ui_returning(uint32_t id){(void)id;abort();}int dm_vault_ui_pointer(int p,int x,int y){(void)p;(void)x;(void)y;abort();}
void dm_vault_request_deposit(uint32_t id){(void)id;abort();}void dm_pages_request_flip(int d){(void)d;abort();}
int dm_vault_ui_over(int x,int y){(void)x;(void)y;abort();}
#ifdef DM_PC_TEST_PROFILE_LINK
int dm_ui_pc_desktop(void){return 1;}
int dm_ui_pc_skill_grid_hover(uint64_t *regs){(void)regs;return 1;}
static int weak_wrapper_calls;
__attribute__((weak)) void dm_ui_pc_grid_hover(uint64_t *regs){weak_wrapper_calls++;if(regs)regs[8]=dm_ui_pc_skill_grid_hover(regs);}
#endif
int main(void){
#ifdef DM_PC_TEST_PROFILE_LINK
 uint64_t regs[31]={0};dm_ui_pc_grid_hover(regs);
 if(dm_pc_items_enabled()!=1||regs[8]!=1||weak_wrapper_calls)return 1;
 puts("PC profile/skill extension: strong items wrapper overrides input fallback");return 0;
#endif
 if(dm_pc_items_enabled())return 1;
 dm_pc_items_snapshot sample={.root=1,.valid=1,.event_scope=1,.ingame=1};dm_pc_items_publish(&sample);
 void(*gates[])(uint64_t*)={dm_ui_pc_item_left,dm_ui_pc_item_right,dm_ui_pc_item_hold,dm_ui_pc_npc_click,dm_ui_pc_held_draw,dm_ui_pc_item_tooltip,dm_ui_pc_grid_hover};
 int checks=1;
 for(unsigned gate=0;gate<sizeof gates/sizeof*gates;gate++){
  uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x100+i;memcpy(saved,regs,sizeof regs);gates[gate](regs);
  for(int i=0;i<31;i++){if(i==8&&gate!=4&&gate!=5){if(regs[i])return 2;}else if(regs[i]!=saved[i])return 3;checks++;}
 }
 dm_pc_items_pump();if(dm_pc_items_pointer(3,1,1))return 4;dm_pc_items_cancel();dm_pc_items_message("PanelManager","ClosePanel","TouchPersonalStashNext");
 printf("PC items missing-profile legacy: %d checks, 0 failures\n",checks);return 0;
}
