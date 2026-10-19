#include "../dm_custom.c"
module_t g_mod[MAX_MOD]; int g_nmod=1;
struct dm_flags g_flag;uint64_t g_t0_ns;
static int checks,failures,wheels;static uint64_t frame=4,keys;
static unsigned char root[256],panel[512],scroll[256],view[256],rows[256],box[2][4096],text[2][256],focus[256];
static uint64_t root_ch[1],panel_ch[2],scroll_ch[1],view_ch[1],row_ch[4],toggle_vt[96],panel_vt[96],focus_vt[96];
static uint64_t ptr(void*p){return (uint64_t)(uintptr_t)p;}
static void relayout(uint64_t w){(void)w;}
uint64_t now_ns(void){return 0;}
uint64_t dm_guest_frame(void){return frame;}
uint32_t dm_ui_gen(void){return 1;}
uint64_t dm_wref_get(dm_wref*r){return r->w;}
int dm_ui_top_chk_on(void){return 0;}
void dm_ui_top_chk(uint64_t r,const char*n,uint64_t w){(void)r;(void)n;(void)w;}
int dm_padmode_current(void){return 1;}
uint64_t dm_padmode_page_keys(uint64_t mask){uint64_t out=keys&mask;keys=0;return out;}
void dm_ui_wheel_on(uint64_t w,int n){(void)w;wheels+=n;}
int dm_lang_index(void){return 2;}
int dm_lang_saved(void){return 2;}
int dm_voice_saved(void){return 9;}
int dm_sys_lang(void){return 2;}
int dm_3d_quality_mode_get(void){return 0;}
float dm_3d_scale_cur(void){return 1;}
void dm_3d_quality_mode_set(int v){(void)v;}
const char*dm_save_commitdir(void){return NULL;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}else printf("PASS %s\n",t);}while(0)
static void widget(unsigned char*w,const char*n,uint64_t*vt,int y){*(uint64_t*)w=ptr(vt);*(const char**)(w+8)=n;w[0x50]=w[0x51]=1;int32_t*r=(int32_t*)(w+0x70);r[0]=100;r[1]=y;r[2]=200;r[3]=120;}
static void children(unsigned char*w,uint64_t*ch,int n){*(uint64_t*)(w+0x58)=ptr(ch);*(uint64_t*)(w+0x60)=n;}
static void tick(uint64_t k){frame+=4;keys=k;dm_custom_ui_pump(ptr(root));}
int main(void){
 setenv("DM_APP","1",1);strcpy(g_mod[0].name,"main");g_mod[0].base=(uint8_t*)(uintptr_t)(ptr(toggle_vt)-DM_VT_TOGGLE);toggle_vt[2]=focus_vt[2]=ptr(relayout);panel_vt[3]=(uint64_t)g_mod[0].base+0xae5f28;
 widget(root,"Root",focus_vt,0);widget(panel,"VideoOptionsPanel",panel_vt,0);widget(scroll,"Scroll",focus_vt,0);widget(view,"View",focus_vt,-700);widget(rows,"TouchCustomRows",focus_vt,0);widget(focus,"TouchCustomSel",focus_vt,0);
 *(uint64_t*)(rows+0x30)=ptr(view);*(uint64_t*)(view+0x30)=ptr(scroll);*(int32_t*)(scroll+0x7c)=600;
 root_ch[0]=ptr(panel);panel_ch[0]=ptr(scroll);panel_ch[1]=ptr(focus);scroll_ch[0]=ptr(view);view_ch[0]=ptr(rows);children(root,root_ch,1);children(panel,panel_ch,2);children(scroll,scroll_ch,1);children(view,view_ch,1);children(rows,row_ch,4);
 widget(box[0],"TouchCustom_language_index",toggle_vt,0);widget(text[0],"TouchCustom_language_index_Text",focus_vt,0);widget(box[1],"TouchCustom_auto_mana",toggle_vt,1700);widget(text[1],"TouchCustom_auto_mana_Text",focus_vt,1700);
 for(int i=0;i<2;i++){row_ch[i*2]=ptr(box[i]);row_ch[i*2+1]=ptr(text[i]);}atomic_store(&g_loaded,1);atomic_store(&g_lang_explicit,2);for(int i=0;i<NK;i++){atomic_store(&g_val[i],k_def[i]);atomic_store(&g_pend[i],-1);}atomic_store(&g_val[0],2);
 tick(0);CHECK(wheels>0,"Reproduce seguimiento de fila0 al estar lista desplazada");wheels=0;dm_custom_pointer_input();tick(0);CHECK(!wheels,"Rueda de usuario conserva desplazamiento sin recentrar fila0");CHECK(*(int32_t*)(focus+0x70)==99999,"Puntero oculta marco de selección obsoleto");
 frame+=36000;tick(0);CHECK(!wheels,"El foco de puntero no caduca ni vuelve arriba por timeout");int before=atomic_load(&g_pend[0]);tick(DM_BTN_A);CHECK(atomic_load(&g_pend[0])==before&&!wheels,"Confirmar tras rueda no cambia fila antigua fuera de pantalla");
 tick(DM_BTN_DOWN);CHECK(wheels<0&&!atomic_load(&g_pointer_focus),"Navegar explícitamente a otra fila recupera seguimiento");CHECK(*(int32_t*)(focus+0x70)!=99999,"Navegación restaura marco en fila seleccionada");wheels=0;dm_custom_pointer_input();tick(0);CHECK(!wheels,"Nuevo gesto vuelve a ceder foco al puntero");
 printf("CUSTOM SCROLL %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
