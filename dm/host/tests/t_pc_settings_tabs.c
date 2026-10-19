// Política de Controls sobre mensajes y setters nativos; sin aplicación ni perfil real.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,checks,failures,shows,choices,messages,last_index,last_visible;
int dm_ui_pc_desktop(void){return desktop;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static void show(uint64_t tabs,int index,int visible){shows++;last_index=index;last_visible=visible;*(uint8_t*)(tabs+0xe88+index)=visible;}
static void choose(uint64_t tabs,int index,int sound){choices++;messages++;last_index=index;CHECK(!sound,"Selección programática conserva el parámetro de sonido a cero");*(int32_t*)(tabs+0xdc4)=index;}
static void check(void){messages++;}
int main(void){
 module_t m={0};_Alignas(8)uint8_t tabs[0xeb0]={0};uint64_t w=(uint64_t)tabs;*(uint64_t*)tabs=0x20ba3a8;*(int32_t*)(tabs+0x558)=4;
 for(int target=0;target<4;target++){
  for(int i=0;i<4;i++){uint64_t e=w+0xb50+i*32;*(uint64_t*)e=ui_hash("SettingsPanelMessage");*(uint64_t*)(e+8)=ui_hash("CheckChanges");*(const char**)(e+0x10)=i==target?"KeyBindingMenu":"VideoOptionsPanel";*(uint8_t*)(e+0x18)=1;}
  CHECK(pc_controls_tab_index(&m,w)==target,"Encuentra Controls por mensaje completo con cualquier orden físico");
  for(int ready=0;ready<2;ready++){shows=choices=messages=0;tabs[0xe88+target]=!ready;*(int32_t*)(tabs+0xdc4)=(target+1)%4;
   CHECK(pc_controls_tab_apply(&m,w,ready,1,show,choose,check),"Árbol válido se resuelve sin índice fijo");CHECK(shows==1&&last_index==target&&last_visible==ready,"Visibilidad original se actualiza al cambiar contexto");CHECK(choices==ready&&messages==ready,"Frontend no selecciona ni abre; partida emite una sola transición original");
   CHECK(pc_controls_tab_apply(&m,w,ready,0,show,choose,check)&&shows==1,"Refresco idempotente no vuelve a llamar setter");
   if(ready){CHECK(pc_controls_tab_apply(&m,w,1,1,show,choose,check)&&choices==1&&messages==2,"Selección ya actual envía sólo CheckChanges, sin doble despacho");}
  }
  desktop=0;CHECK(pc_controls_tab_visible(&m,w,target,1,0)==0,"Init legacy conserva Controls oculto");desktop=1;CHECK(pc_controls_tab_visible(&m,w,target,1,0)==1&&pc_controls_tab_visible(&m,w,target,0,0)==0,"Init PC muestra sólo con readiness actual");
 }
 uint64_t e=w+0xb50+3*32;*(uint8_t*)(e+0x18)=0;CHECK(pc_controls_tab_index(&m,w)<0,"Argumento no válido no se interpreta como Controls");*(uint8_t*)(e+0x18)=1;*(uint64_t*)e=ui_hash("PanelManager");CHECK(pc_controls_tab_index(&m,w)<0,"No confunde categoría con mismo argumento");*(uint64_t*)e=ui_hash("SettingsPanelMessage");*(uint64_t*)(e+8)=ui_hash("OpenPanel");CHECK(pc_controls_tab_index(&m,w)<0,"No confunde acción con mismo argumento");*(uint64_t*)(e+8)=ui_hash("CheckChanges");
 *(int32_t*)(tabs+0x558)=9;CHECK(pc_controls_tab_index(&m,w)<0,"Count superior a ocho falla cerrado");*(int32_t*)(tabs+0x558)=-1;CHECK(pc_controls_tab_index(&m,w)<0,"Count negativo falla cerrado");*(int32_t*)(tabs+0x558)=4;*(uint64_t*)tabs=0x20ba3b0;CHECK(pc_controls_tab_index(&m,w)<0,"No interpreta offsets privados en clase distinta");CHECK(pc_controls_tab_visible(&m,w,3,1,0)==0,"Gate no habilita índice ajeno al mensaje validado");
 module_t abi={.size=0x2f05460};abi.base=calloc(1,abi.size);if(!abi.base)return 2;
 const uint32_t show_code[]={0xa9ba6ffc,0xa90167fa,0xa9025ff8,0xa90357f6},select_code[]={0xa9be4ff4,0xa9017bfd,0x910043fd,0xb94dc408};memcpy(abi.base+0xada890,show_code,sizeof show_code);memcpy(abi.base+0xad9b80,select_code,sizeof select_code);
 CHECK(pc_controls_tab_abi(&abi),"Acepta únicamente las dos APIs nativas reconocidas");abi.base[0xada894]^=1;CHECK(!pc_controls_tab_abi(&abi),"ShowTab con versión distinta impide llamada");abi.base[0xada894]^=1;abi.base[0xad9b8c]^=1;CHECK(!pc_controls_tab_abi(&abi),"SelectTab con versión distinta impide llamada");abi.base[0xad9b8c]^=1;abi.size--;CHECK(!pc_controls_tab_abi(&abi)&&!pc_controls_tab_abi(NULL),"Módulo truncado o ausente falla cerrado");free(abi.base);
 printf("PC SETTINGS TABS %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
