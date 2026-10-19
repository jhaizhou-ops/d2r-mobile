// Cola raw PC y liberaciones ligadas al VK del DOWN; sin app ni preferencias.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,checks,failures,consumed,ui_calls;
static uint64_t capture_root;static module_t capture_module;
static int continues;static void continue_original(void){continues++;}
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return 1;}int dm_padmode_busy(void){return 0;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static int ui(const pc_key_event*e,void*c){(void)e;(void)c;ui_calls++;return consumed;}
static int capture_ui(const pc_key_event*e,void*c){(void)e;(void)c;return pc_native_capture_active(&capture_module,capture_root);}
static int match(const void*r,const void*p,int mods){const pc_binding_record*b=r;return (b->key&0xfff)==((const uint32_t*)p)[4] && (b->key&0x7000)==mods;}
static pc_binding_record records[138];
static void reset(void){memset(g_pc_key_actions,0,sizeof g_pc_key_actions);memset(g_pc_key_down,0,sizeof g_pc_key_down);memset(g_pc_key_counts,0,sizeof g_pc_key_counts);memset(g_pc_binding_desired,0,sizeof g_pc_binding_desired);g_pc_binding_read=g_pc_binding_write=0;consumed=ui_calls=0;for(int i=0;i<138;i++)records[i]=(pc_binding_record){UINT32_MAX,-1,0};}
static void key(int vk,int mods,int down){pc_key_event e={vk,mods,down};pc_key_process(&e,records,match,ui,NULL);}
static void original_profiles(const char *path) {
 FILE *file=fopen(path,"rb");CHECK(file!=NULL,"Abre imagen original para validar presets reales");if(!file)return;
 const long offsets[]={0x2087ff4,0x2088558};
 for(unsigned preset=0;preset<2;preset++) {
  int loaded=!fseek(file,offsets[preset],SEEK_SET)&&fread(records,sizeof records,1,file)==1;
  CHECK(loaded,"Lee138 registros originales Classic y Resurrected");if(!loaded)continue;
  CHECK(records[42].action==22&&records[90].action==21,"Presets reales conservan orden nativo no numérico");
  CHECK(pc_profile_ready_state(1,1,1,0,records),"Cada preset original cargado habilita controles e inventario");
  records[42].key='K'|0x2000;CHECK(pc_profile_ready_state(1,1,1,0,records),"Reasignar tecla y modificadores conserva perfil válido");
  pc_binding_record first=records[0];records[0]=records[137];records[137]=first;
  CHECK(pc_profile_ready_state(1,1,1,0,records),"Orden de registros no define integridad de bindings");
  records[137]=records[0];CHECK(!pc_profile_ready_state(1,1,1,0,records),"Duplicar par acción/tipo rechaza perfil incompleto");
 }
 fclose(file);
}
int main(int argc,char **argv){if(argc!=2){fprintf(stderr,"Uso: %s main.orig.bin\n",argv[0]);return 2;}original_profiles(argv[1]);reset();records[0]=(pc_binding_record){14,'1',1};records[1]=(pc_binding_record){14,'1',0};key('1',0,1);CHECK(ui_calls==1&&g_pc_binding_write==1&&g_pc_key_counts[14]==1,"UI recibe primero; primaria/secundaria idénticas generan un solo DOWN");key('1',0,1);CHECK(ui_calls==2&&g_pc_binding_write==1,"Repeat llega a UI sin repetir acción");consumed=1;key('1',0x2000,0);CHECK(g_pc_binding_write==2&&!g_pc_key_counts[14]&&!g_pc_key_down['1'],"UP consumido y modificadores distintos liberan acción del DOWN");
 reset();records[0]=(pc_binding_record){14,'K',1};consumed=1;key('K',0,1);key('K',0,0);CHECK(ui_calls==2&&!g_pc_binding_write,"Editor nativo consume raw sin ejecutar gameplay");
 reset();records[0]=(pc_binding_record){14,'K'|0x2000,1};key('K',0x2000,1);records[0]=(pc_binding_record){35,'K',1};key('K',0,0);CHECK(g_pc_binding_write==2&&g_pc_binding_queue[1].action==14&&!g_pc_binding_queue[1].down,"Remap durante pulsación no cambia destinatario del UP");
 reset();records[0]=(pc_binding_record){14,'K',1};records[1]=(pc_binding_record){14,'L',0};key('K',0,1);key('L',0,1);key('K',0,0);CHECK(g_pc_binding_write==1&&g_pc_key_counts[14]==1,"Dos teclas sostienen la misma acción hasta la última liberación");key('L',0,0);CHECK(g_pc_binding_write==2&&!g_pc_key_counts[14],"Última tecla genera un único UP");
 reset();records[0]=(pc_binding_record){68,0x104,1};key(0x104,0,1);key(0x104,0,0);CHECK(g_pc_binding_write==2&&g_pc_binding_queue[0].action==68,"VK rueda y acción68 atraviesan matcher sin truncarse");
 records[0]=(pc_binding_record){14,0x100|0x2000,1};records[1]=(pc_binding_record){35,0x102,0};pc_mouse_reserved_refresh(records);CHECK(dm_ui_pc_mouse_reserved(2)&&!dm_ui_pc_mouse_reserved(3)&&dm_ui_pc_mouse_reserved(4)&&!dm_ui_pc_mouse_reserved(0)&&!dm_ui_pc_mouse_reserved(31),"Snapshot reserva sólo NSEvent2..4 con bindings nativos válidos");
 desktop=0;CHECK(!dm_ui_pc_key('A',0,1)&&!dm_ui_pc_controls(),"Legacy no consume entrada ni menú PC");desktop=1;CHECK(!dm_ui_pc_key(0,0,1)&&!dm_ui_pc_key(0x105,0,1)&&!dm_ui_pc_key('A',0x8000,1),"Productor rechaza VK/modificadores no soportados");CHECK(dm_ui_pc_key('A',0x1000,1)&&g_pc_key_write-g_pc_key_read==1,"Productor sólo encola transición raw");atomic_store(&g_pc_profile_ready,1);CHECK(dm_ui_pc_controls()&&atomic_load(&g_pc_controls_request),"Menú se solicita atómicamente para guest");
 reset();records[0]=(pc_binding_record){14,0x103,1};pc_key_event wheel={.wheel=1,.steps=3};pc_key_wheel_process(&wheel,records,match,ui,NULL);CHECK(ui_calls==1&&g_pc_binding_write==6&&!g_pc_key_counts[14],"Rueda pasauna vezporUIyproduce trespulsosnativos sin KeyDownUIduplicado");reset();records[0]=(pc_binding_record){14,0x103,1};consumed=1;pc_key_wheel_process(&wheel,records,match,ui,NULL);CHECK(ui_calls==1&&!g_pc_binding_write,"Scroll consumido por lista/editor nunca llega a gameplay");consumed=0;wheel.momentum=1;pc_key_wheel_process(&wheel,records,match,ui,NULL);CHECK(ui_calls==2&&!g_pc_binding_write,"Momentum sigueUIy jamás activa acción");wheel.momentum=0;wheel.steps=-1;records[0]=(pc_binding_record){68,0x104,1};pc_key_wheel_process(&wheel,records,match,ui,NULL);CHECK(g_pc_binding_write==2&&g_pc_binding_queue[0].action==68,"Rueda inversa conserva VK104 nativo");
 CHECK(dm_ui_pc_wheel(1,0,1,.5f,-.1f),"Wheelproducer conserva coordenadaUInegativa para validar en guest");
 for(int i=0;i<3;i++){reset();int vk=0x10+i,mods=0x1000<<i;records[0]=(pc_binding_record){36,vk,1};key(vk,mods,1);key(vk,0,0);CHECK(g_pc_binding_write==2&&g_pc_binding_queue[0].action==36,"Modificador físico standalone no se rechaza por su propio bit AppKit");}
 memset(&g_pc_controls_open,0,sizeof g_pc_controls_open);CHECK(pc_controls_step(1,0,0,0,10)==1,"Controles abre SettingsPanel por mensaje original de Opciones");CHECK(!pc_controls_step(0,0,0,0,11),"No reenvía apertura mientras se construye panel");CHECK(pc_controls_step(0,1,0,0,12)==2,"Sólo después de visible envía CheckChanges KeyBindingMenu");CHECK(!pc_controls_step(0,1,0,0,13),"CheckChanges no se repite ni salta confirmación de cambios");CHECK(!pc_controls_step(0,1,1,0,14)&&!g_pc_controls_open.phase,"Menú nativo visible completa solicitud");CHECK(!pc_controls_step(1,1,0,1,20)&&pc_controls_step(0,1,0,0,21)==2,"Modal bloquea hasta que el jugador responde");CHECK(!pc_controls_step(1,1,1,0,30)&&!g_pc_controls_open.phase,"Menú ya abierto no se reconstruye");
 pc_key_event escape={.vk=0x1b,.down=1};CHECK(!pc_death_escape_event(&escape,0,continue_original)&&!continues,"Esc en otros paneles conserva dispatch normal");CHECK(pc_death_escape_event(&escape,1,continue_original)&&continues==1,"Esc inicial en YouDied delega Continue original");CHECK(pc_death_escape_event(&escape,1,continue_original)&&continues==1,"Repeat sostenido no repite continuación");escape.down=0;CHECK(pc_death_escape_event(&escape,0,continue_original)&&!g_pc_death_escape&&continues==1,"UP tras continuar se consume sin abrir pausa");CHECK(!pc_death_escape_event(&escape,1,continue_original),"UP sin DOWN propio nunca continúa muerte");
 uint8_t capture_tree[160]={0},capture_menu[512]={0};uint64_t capture_children[]={(uint64_t)capture_menu};capture_root=(uint64_t)capture_tree;*(uint64_t*)(capture_tree+0x58)=(uint64_t)capture_children;*(uint64_t*)(capture_tree+0x60)=1;*(uint64_t*)capture_menu=0x20c3680;*(const char**)(capture_menu+8)="KeyBindingMenu";capture_menu[0x50]=capture_menu[0x51]=1;*(int32_t*)(capture_menu+0x194)=14;capture_menu[0x191]=1;capture_menu[0x192]=0xa5;capture_menu[0x193]=0x7f;
 for(int type=0;type<=1;type++){capture_menu[0x190]=type;CHECK(pc_native_capture_active(&capture_module,capture_root),"Tipo nativo byte0/1 reconoce captura con padding191..193 no cero");reset();records[0]=(pc_binding_record){14,'K',1};pc_key_event down={.vk='K',.down=1};pc_key_process(&down,records,match,capture_ui,NULL);CHECK(!g_pc_binding_write&&!g_pc_key_counts[14],"DOWN capturado con padding no dispara gameplay aunque root no consuma");}
 capture_menu[0x190]=2;CHECK(!pc_native_capture_active(&capture_module,capture_root),"Tipo de mando2 sigue excluido aunque padding no sea cero");capture_menu[0x190]=1;*(int32_t*)(capture_menu+0x194)=69;CHECK(!pc_native_capture_active(&capture_module,capture_root),"Índice69 conserva captura inactiva con padding no cero");

 for(int i=0;i<138;i++)records[i]=(pc_binding_record){(uint32_t)(i/2),-1,(uint32_t)(i&1?0:1)};
 CHECK(pc_profile_ready_state(1,1,1,0,records),"Partida ToGame/HUD sin carga y138 pares válidos habilita edición");
 CHECK(!pc_profile_ready_state(1,0,1,0,records)&&!pc_profile_ready_state(1,1,0,0,records)&&!pc_profile_ready_state(1,1,1,1,records),"Frontend, transición y carga no acreditan perfil aunque tabla permanezca válida");
 CHECK(!pc_profile_ready_state(0,1,1,0,records),"Legacy no usa readiness PC");records[137].type=2;CHECK(!pc_profile_ready_state(1,1,1,0,records),"Readiness valida también tipo del último registro");records[137].type=0;records[137].action=67;CHECK(!pc_profile_ready_state(1,1,1,0,records),"Par incompleto o tabla sin cargar no habilita editor");
 atomic_store(&g_pc_profile_ready,0);atomic_store(&g_pc_controls_request,0);CHECK(!dm_ui_pc_controls()&&!atomic_load(&g_pc_controls_request),"API AppKit rechaza sin leer UI ni encolar apertura frontend");
 printf("PC RAW KEYS %d comprobaciones, %d fallos\n",checks,failures);return!!failures;}
