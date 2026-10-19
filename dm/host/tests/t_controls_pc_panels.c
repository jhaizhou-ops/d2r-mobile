#include "../dm_cursor.c"
#include <stdio.h>
module_t g_mod[MAX_MOD]; int g_nmod;
static int ingame=1, loaded=1, checks, failures; static uint64_t frame;
uint64_t dm_guest_frame(void){return frame;}
int dm_uiscreen_ingame(void){return ingame;}
int dm_loadscreen_down(void){return loaded;}
int dm_padmode_busy(void){return 0;}
static unsigned char root[256], switcher[512], tabs[4096], page[256], modal[256], leaf[256];
static uint64_t children[3], switch_children[2], modal_children[1], vtable[8];
static uint64_t ptr(void *p){return (uint64_t)(uintptr_t)p;}
static void widget(unsigned char *w,const char *name){*(const char **)(w+8)=name;w[0x50]=w[0x51]=1;}
static module_t module;
static int pc_enabled,armed=-1;
int dm_ui_pc_desktop(void){return pc_enabled;}
void dm_elysium_arm(int on){armed=on;}
static int native_id=-1,native_arg=-1;
static int native_toggle(int id,int arg){native_id=id;native_arg=arg;return 1;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
static void reset(void){
 memset(root,0,sizeof root);memset(switcher,0,sizeof switcher);memset(tabs,0,sizeof tabs);memset(page,0,sizeof page);memset(modal,0,sizeof modal);memset(leaf,0,sizeof leaf);
 widget(root,"Root");widget(switcher,"UISwitcher");widget(tabs,"UISwitcherTabs");widget(page,"Hidden");widget(modal,"ConfirmationModal");widget(leaf,"Text");page[0x50]=modal[0x50]=0;
 *(uint64_t *)(switcher)=ptr(vtable);vtable[5]=0x404fd8;
 children[0]=ptr(switcher);children[1]=ptr(page);children[2]=ptr(modal);*(uint64_t *)(root+0x58)=ptr(children);*(uint64_t *)(root+0x60)=3;
 switch_children[0]=ptr(tabs);*(uint64_t *)(switcher+0x58)=ptr(switch_children);*(uint64_t *)(switcher+0x60)=1;
 modal_children[0]=ptr(leaf);*(uint64_t *)(modal+0x58)=ptr(modal_children);*(uint64_t *)(modal+0x60)=1;
 const int ids[7]={21,14,2,1,4,30,9};memcpy(switcher+0x168,ids,sizeof ids);
 *(int32_t *)(switcher+0x188)=7;*(int32_t *)(switcher+0x1b8)=-1;frame+=200;ingame=loaded=1;
 atomic_store(&g_tab_direct,0);atomic_store(&g_fe_key_mask,0);atomic_store(&g_pstate,0);
}
int main(void){
 const int expected[]={14,2,1,4,9};for(int p=0;p<5;p++){native_id=native_arg=-1;CHECK(pc_panel_native(p,native_toggle)&&native_id==expected[p]&&native_arg==0,"Panel PC usa toggle original independiente, sin UISwitcher");}
 CHECK(!pc_panel_native(-1,native_toggle)&&!pc_panel_native(5,native_toggle),"Destino fuera de rango no alcanza código original");
 for(int target=0;target<4;target++){reset();dm_ui_pc_panel(target);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_tab_direct)&&g_tab_k==target+1,"I/C/T/Q usan índice original mediante g_tab_direct");}
 reset();dm_ui_pc_panel(DM_PC_OPTIONS);pc_panel_pump(&module,ptr(root),frame);CHECK(g_tab_k==6,"Opciones usa última página incluyendo mercenario oculto");
 reset();const int short_ids[5]={14,2,1,4,9};memcpy(switcher+0x168,short_ids,sizeof short_ids);*(int32_t *)(switcher+0x188)=5;dm_ui_pc_panel(DM_PC_OPTIONS);pc_panel_pump(&module,ptr(root),frame);CHECK(g_tab_k==4,"Opciones respeta número real de páginas");
 reset();switcher[0x50]=0;dm_ui_pc_panel(DM_PC_INVENTORY);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)==DM_BTN_PLUS&&!atomic_load(&g_tab_direct),"En mundo abre funciones antes de seleccionar página");
 atomic_store(&g_fe_key_mask,0);pc_panel_pump(&module,ptr(root),frame+1);CHECK(!atomic_load(&g_fe_key_mask),"Espera respuesta sin repetir teclas ni lanzar habilidad R");switcher[0x50]=1;pc_panel_pump(&module,ptr(root),frame+2);CHECK(atomic_load(&g_tab_direct)&&g_tab_k==3,"Tras abrir UISwitcher selecciona inventario");
 reset();widget(page,"PlayerInventoryExpansionLayout");*(int32_t *)(switcher+0x1b8)=3;dm_ui_pc_panel(DM_PC_INVENTORY);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)==DM_BTN_B&&!atomic_load(&g_tab_direct),"Repetir inventario abierto pide cierre original");
 reset();modal[0x50]=1;dm_ui_pc_panel(DM_PC_SKILLS);pc_panel_pump(&module,ptr(root),frame);CHECK(!atomic_load(&g_fe_key_mask)&&!atomic_load(&g_tab_direct),"Confirmación modal impide cambiar de página");
 reset();loaded=0;dm_ui_pc_panel(DM_PC_SKILLS);pc_panel_pump(&module,ptr(root),frame);CHECK(!atomic_load(&g_tab_direct),"Carga impide ejecutar atajos de panel");
 reset();vtable[5]=0x1234;dm_ui_pc_panel(DM_PC_SKILLS);pc_panel_pump(&module,ptr(root),frame);CHECK(!atomic_load(&g_tab_direct),"Clase no reconocida no recibe offsets ni selección");

 reset();*(uint64_t *)(switcher+0x60)=0;children[1]=ptr(tabs);children[2]=ptr(page);*(uint64_t *)(root+0x60)=3;
 const int native_ids[7]={21,14,2,1,4,30,9};memcpy(switcher+0x168,native_ids,sizeof native_ids);*(int32_t *)(switcher+0x188)=7;
 dm_ui_pc_panel(DM_PC_CHARACTER);pc_panel_pump(&module,ptr(root),frame);
 CHECK(atomic_load(&g_tab_direct)&&g_tab_k==2,"Tabs fuera de UISwitcher y página Party oculta no impiden Personaje");
 reset();const int reordered[5]={1,9,4,14,2};memcpy(switcher+0x168,reordered,sizeof reordered);*(int32_t *)(switcher+0x188)=5;dm_ui_pc_panel(DM_PC_CHARACTER);pc_panel_pump(&module,ptr(root),frame);CHECK(g_tab_k==4,"Personaje se localiza por ID aunque cambie orden de páginas");
 reset();widget(page,"PlayerInventoryOriginalLayout");*(int32_t *)(switcher+0x1b8)=3;dm_ui_pc_panel(DM_PC_INVENTORY);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)==DM_BTN_B&&!atomic_load(&g_tab_direct),"Inventario clásico activo cierra por ID nativo");
 reset();widget(page,"QuestLogPanelOriginal");*(int32_t *)(switcher+0x1b8)=1;dm_ui_pc_panel(DM_PC_QUESTS);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)==DM_BTN_B&&!atomic_load(&g_tab_direct),"Misiones clásicas activas cierran por ID nativo");
 reset();widget(page,"PlayerInventoryOriginalLayout");switcher[0x50]=0;dm_ui_pc_panel(DM_PC_INVENTORY);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)!=DM_BTN_B,"Un widget clásico inactivo no provoca cierre si selector está oculto");
 reset();const int reordered_classic[5]={9,1,4,2,14};memcpy(switcher+0x168,reordered_classic,sizeof reordered_classic);*(int32_t *)(switcher+0x188)=5;*(int32_t *)(switcher+0x1b8)=1;widget(page,"PlayerInventoryOriginalLayout");dm_ui_pc_panel(DM_PC_INVENTORY);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)==DM_BTN_B&&!atomic_load(&g_tab_direct),"Inventario clásico reordenado se cierra por ID activo");
 reset();widget(page,"PlayerInventoryExpansionLayout");dm_ui_pc_panel(DM_PC_INVENTORY);pc_panel_pump(&module,ptr(root),frame);CHECK(atomic_load(&g_fe_key_mask)!=DM_BTN_B&&atomic_load(&g_tab_direct),"Widget visible sin página activa no provoca B por nombre");
 reset();widget(modal,"ButtonBindingModal");atomic_store(&g_sendq_on,0);CHECK(pc_cancel_binding(ptr(root))&&atomic_load(&g_sendq_on)&&!strcmp(g_sendq,"PanelManager:ClosePanel:ButtonBindingModal")&&!atomic_load(&g_fe_key_mask),"Cancelar binding encola cierre original sin tecla de asignación");atomic_store(&g_sendq_on,0);modal[0x50]=0;CHECK(!pc_cancel_binding(ptr(root))&&!atomic_load(&g_sendq_on),"Modal oculto no recibe cierre");CHECK(!pc_cancel_binding(0),"Raíz ausente no provoca cancelación");
 unsigned char create[256]={0},check[2048]={0};uint64_t create_children[1]={ptr(check)};
 reset();pc_enabled=1;widget(create,"CharacterCreatePanel");widget(check,"TouchBliss");*(uint64_t *)check=DM_VT_TOGGLE;*(uint64_t *)(create+0x58)=ptr(create_children);*(uint64_t *)(create+0x60)=1;children[0]=ptr(create);children[1]=ptr(page);children[2]=ptr(modal);*(int32_t *)(check+0x6e8)=4;
 atomic_store(&g_fe_bliss,0);armed=-1;pc_bliss_read(&module,ptr(root));CHECK(dm_fe_bliss()==1&&armed==1,"Creación PC arma Elíseo sin CharacterOptionsPanel ni toque activo");
 armed=-1;pc_bliss_read(&module,ptr(root));CHECK(armed==-1,"Lectura estable no rearma cada frame");*(int32_t *)(check+0x6e8)=0;pc_bliss_read(&module,ptr(root));CHECK(dm_fe_bliss()==0&&armed==0,"Desmarcar nativo desarma Elíseo");
 armed=-1;create[0x50]=0;*(int32_t *)(check+0x6e8)=4;pc_bliss_read(&module,ptr(root));CHECK(armed==-1,"Creador oculto no contamina la próxima selección");create[0x50]=1;*(uint64_t *)check=123;pc_bliss_read(&module,ptr(root));CHECK(armed==-1,"Clase incorrecta no lee estado específico");pc_enabled=0;*(uint64_t *)check=DM_VT_TOGGLE;pc_bliss_read(&module,ptr(root));CHECK(armed==-1,"Fuera de PC se conserva el armado anterior");
 printf("CONTROLS PC PANELS %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
