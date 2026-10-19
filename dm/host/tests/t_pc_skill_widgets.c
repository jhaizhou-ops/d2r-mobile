// A5/A6 PC skill boundaries: native endpoint sinks with real memory and guards.
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame=1,busy,checks,failures,reads;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
uint64_t dm_guest_frame(void){return 123;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
#define I(p,o,v) (*(int*)((p)+(o))=(v))
static struct {uint64_t p;size_t n;} regions[40];static unsigned nr;
static void region(void *p,size_t n){regions[nr++]=(typeof(*regions)){(uint64_t)p,n};}
static uint64_t mutate_at,mutate_target,mutate_value;static size_t mutate_size;
static uint64_t denied,cancel_at;static int cancel_kind;static uint64_t *root_pointer,*hover_pointer;static int32_t *xy_pointer;
static int read_memory(uint64_t p,void *out,size_t n){
 reads++;if(p==denied)return 0;
 if(mutate_at&&p==mutate_at){mutate_at=0;memcpy((void*)mutate_target,&mutate_value,mutate_size);}
 for(unsigned i=0;i<nr;i++)if(p>=regions[i].p&&n<=regions[i].n&&p-regions[i].p<=regions[i].n-n){
  if(cancel_kind&&p==cancel_at){int k=cancel_kind;cancel_kind=0;if(k==1)atomic_fetch_add(&g_ui_gen,1);if(k==2)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(k==3)*root_pointer=0;if(k==4)*hover_pointer=0;if(k==5)(*xy_pointer)++;if(k==6)busy=1;if(k==7)ingame=0;if(k==8)atomic_store(&g_load_up,1);}
  memcpy(out,(void*)p,n);return 1;
 }
 return 0;
}
static int model_calls,toggle_calls,select_calls,close_calls,selection_result=1,selected_skill,selected_left,selected_assoc,model_cancel;static uint64_t expected_model;static int *side_value;
static void set_model(uint64_t m,uint64_t key,const int *side){CHECK(m==expected_model&&key==0x7d8f57e3857c067dull&&(*side==1||*side==2),"Setter usa modelo/clave/lado originales");model_calls++;*side_value=*side;if(model_cancel)atomic_fetch_add(&g_ui_gen,1);}
static int toggle(int id,int flag){CHECK(id==3&&!flag,"Apertura conserva toggle nativo (3,0)");toggle_calls++;return 1;}
static int select_skill(int id,int left,int assoc){select_calls++;selected_skill=id;selected_left=left;selected_assoc=assoc;return selection_result;}
static void close_panel(int id,int flag){CHECK(id==3&&!flag,"Cierre conserva ID3 y flag0");close_calls++;}
#ifdef DM_TEST_SKILL_BASELINE
static int route_click(module_t*m,uint64_t w,int pc,pc_tooltip_reader r,const pc_skill_widget_api*a){(void)m;(void)w;(void)pc;(void)r;(void)a;return 0;}
static int route_hud(module_t*m,uint64_t*r,int pc,pc_tooltip_reader read){(void)m;(void)r;(void)pc;(void)read;return 0;}
#else
#define route_click pc_skill_click_route
#define route_hud pc_skill_hud_route
#endif
int main(void){
 module_t m={.size=0x2f12d38};m.base=calloc(1,m.size);if(!m.base)return 2;region(m.base,m.size);
 FILE*f=fopen(getenv("DM_TEST_MAIN")?getenv("DM_TEST_MAIN"):"build/main.orig.bin","rb");if(!f||fread(m.base,1,m.size,f)!=m.size)return 2;fclose(f);
 _Alignas(8) uint8_t root[0x200]={0},fm[0x200]={0},panel[0x200]={0},widget[0xd00]={0},player[0x200]={0},inventory[0x100]={0},skills[32]={0},left[0x50]={0},right[0x50]={0},record1[0x40]={0},record2[0x40]={0},metadata[0x1600]={0},model[16]={0},entry[24]={0},wrapper[24]={0};
 uint64_t bucket=(uint64_t)entry;int cap=100,side=2;int16_t id1=42,id2=13;char name[64]="HUDPanel";uint64_t children[]={(uint64_t)fm,(uint64_t)panel},leaves[]={(uint64_t)widget};
 void*objects[]={root,fm,panel,widget,player,inventory,skills,left,right,record1,record2,metadata,model,entry,wrapper,&bucket,&cap,&side,name,children,leaves};size_t sizes[]={sizeof root,sizeof fm,sizeof panel,sizeof widget,sizeof player,sizeof inventory,sizeof skills,sizeof left,sizeof right,sizeof record1,sizeof record2,sizeof metadata,sizeof model,sizeof entry,sizeof wrapper,sizeof bucket,sizeof cap,sizeof side,sizeof name,sizeof children,sizeof leaves};for(unsigned i=0;i<sizeof objects/sizeof*objects;i++)region(objects[i],sizes[i]);
 uint64_t base=(uint64_t)m.base,r=(uint64_t)root,w=(uint64_t)widget;
 Q(m.base,0x2f05458,root);Q(root,0xd0,fm);Q(root,0x58,children);Q(root,0x60,2);root[0x50]=root[0x51]=root[0xd9]=1;
 Q(fm,0,base+0x20b7f88);Q(fm,0x178,widget);Q(panel,8,name);Q(panel,0x30,root);Q(panel,0x58,leaves);Q(panel,0x60,1);panel[0x50]=panel[0x51]=1;
 Q(widget,0,base+0x208a5f8);Q(widget,0x30,panel);widget[0x50]=widget[0x51]=1;I(widget,0xa88,1);I(widget,0xb08,42);I(widget,0xb0c,-1);I(widget,0xb18,-1);
 I(player,8,1);player[0x1bd]=1;Q(player,0x90,inventory);I(inventory,0,0x01020304);Q(player,0x100,skills);Q(skills,0,left);Q(skills,8,left);Q(skills,16,right);Q(left,0,record1);Q(left,8,right);I(left,0x38,1);I(left,0x44,-1);Q(right,0,record2);I(right,0x38,1);I(right,0x44,7);memcpy(record1,&id1,2);memcpy(record2,&id2,2);
 Q(m.base,0x2d603a0+8,player);I(m.base,0x2d60380,1);Q(m.base,0x2d64a18+16,metadata);Q(metadata,0x1500,&cap);
 Q(m.base,0x2f05450,model);Q(model,0,1);Q(model,8,&bucket);Q(entry,8,0x7d8f57e3857c067dull);Q(entry,16,wrapper);Q(wrapper,16,&side);expected_model=(uint64_t)model;side_value=&side;
 atomic_store(&g_load_up,0);atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);g_pc_options_open.pointer=1;g_pc_options_open.input_epoch=7;g_pc_pointer_hover=(pc_pointer_hover_state){.root=r,.valid=1,.generation=12,.epoch=7};root_pointer=(uint64_t*)(m.base+0x2f05458);hover_pointer=(uint64_t*)(fm+0x178);xy_pointer=(int32_t*)(m.base+0x2d128f8);
 g_pc_skill_event=(pc_skill_event_scope){r,ui_hash("LeftMouseButtonUp"),0,0,1};
 pc_skill_widget_api api={set_model,toggle,select_skill,close_panel};
 CHECK(pc_skill_widget_abi(&m),"Preimágenes coinciden con original verificado");
 side=0;CHECK(pc_skill_click_route(&m,w,desktop,read_memory,&api)==1&&side==1,"HUD inicializa modelo lado0 con setter nativo");model_calls=toggle_calls=0;
 for(int button=1;button<=2;button++){
  I(widget,0xa88,button);CHECK(route_click(&m,w,desktop,read_memory,&api)==1,"HUD abre selector mediante modelo/toggle originales");CHECK(side==button,"HUD pasa lado original 1/2");
  uint64_t regs[31],saved[31];for(unsigned i=0;i<31;i++)regs[i]=0xabc00+i;regs[19]=w;memcpy(saved,regs,sizeof regs);CHECK(route_hud(&m,regs,desktop,read_memory),"HUD obtiene selección activa sin binding asignado");CHECK(regs[21]==(button==1?42:13)&&regs[0]==(uint32_t)(button==1?-1:7),"Icono usa skill y asociación de su lado");int intact=1;for(unsigned i=0;i<31;i++)if(i!=0&&i!=21&&regs[i]!=saved[i])intact=0;CHECK(intact,"A6 conserva todos los otros registros");
 }
 CHECK(model_calls==2&&toggle_calls==2&&!select_calls&&!close_calls,"HUD no selecciona ni cierra");
 strcpy(name,"SkillSelect");I(widget,0xa88,0);
 for(side=1;side<=2;side++){CHECK(route_click(&m,w,desktop,read_memory,&api)==2,"Popup selecciona mediante setter original");CHECK(selected_skill==42&&selected_left==(side==1)&&selected_assoc==-1,"Selección usa lado de apertura, ID y asociación nativos");}
 CHECK(select_calls==2&&close_calls==2,"Sólo selección válida cierra ID3");selection_result=0;CHECK(!route_click(&m,w,desktop,read_memory,&api)&&close_calls==2,"Setter fallido conserva selector abierto");selection_result=1;side=2;
 I(widget,0xb08,99);CHECK(!pc_skill_click_route(&m,w,desktop,read_memory,&api),"ID ausente no llega al setter");I(widget,0xb08,42);I(widget,0xb0c,99);CHECK(!pc_skill_click_route(&m,w,desktop,read_memory,&api),"Asociación ajena no llega al setter");I(widget,0xb0c,-1);
 strcpy(name,"HUDPanel");I(widget,0xa88,1);uint64_t regs[31]={0};regs[19]=w;
 Q(skills,8,0);CHECK(pc_skill_hud_route(&m,regs,desktop,read_memory)&&regs[21]==UINT32_MAX&&regs[0]==UINT32_MAX,"Selección vacía oculta icono por continuación nativa");Q(skills,8,left);
 g_pc_options_open.pointer=0;Q(fm,0x178,0);Q(inventory,0x40,player);CHECK(pc_skill_hud_route(&m,regs,desktop,read_memory),"HUD se actualiza con teclado/sin hover/mano ocupada");CHECK(!pc_skill_click_route(&m,w,desktop,read_memory,&api),"La misma condición no permite acción");g_pc_options_open.pointer=1;Q(fm,0x178,widget);Q(inventory,0x40,0);
#define CLICK() pc_skill_click_route(&m,w,desktop,read_memory,&api)
#define HUD() pc_skill_hud_route(&m,regs,desktop,read_memory)
#define DENY(t) do{int a=model_calls+toggle_calls+select_calls+close_calls;CHECK(!CLICK(),t);CHECK(a==model_calls+toggle_calls+select_calls+close_calls,"Rechazo no invoca primitivas");}while(0)
 g_pc_skill_event.valid=0;DENY("C0 fuera del evento físico Up conserva RET");g_pc_skill_event.valid=1;g_pc_skill_event.action=ui_hash("RightMouseButtonUp");DENY("RMB no ejecuta la acción de LMB");g_pc_skill_event.action=ui_hash("LeftMouseButtonUp");
 desktop=0;reads=0;DENY("Legacy conserva RET");CHECK(!HUD()&&!reads,"Legacy no lee memoria");desktop=1;
 ingame=0;DENY("Frontend no abre selector");CHECK(!HUD(),"Frontend no adapta HUD");ingame=1;busy=1;DENY("Busy conserva fallback");busy=0;atomic_store(&g_load_up,1);DENY("Carga conserva fallback");atomic_store(&g_load_up,0);
 g_pc_pointer_hover.x++;DENY("XY obsoleto no activa botón");g_pc_pointer_hover.x--;g_pc_pointer_hover.epoch++;DENY("Época antigua no activa botón");g_pc_pointer_hover.epoch--;g_pc_pointer_hover.generation++;DENY("Generación antigua no activa botón");g_pc_pointer_hover.generation--;
 Q(fm,0x178,0);DENY("Sin hover no se inventa selección");Q(fm,0x178,widget);Q(inventory,0x40,player);DENY("Mano ocupada bloquea acción");Q(inventory,0x40,0);m.base[0x2d128ec]=1;DENY("Ec nativo conserva fallback");m.base[0x2d128ec]=0;
 I(widget,0xa88,3);DENY("Botón de binding queda nativo");CHECK(!HUD(),"Binding no se suplanta por selección");I(widget,0xa88,1);
 strcpy(name,"InventoryPanel");DENY("Otro dominio no activa selector");strcpy(name,"HUDPanel");Q(widget,0,base+DM_VT_SLOT_B);DENY("Otra clase no activa habilidades");Q(widget,0,base+0x208a5f8);
 for(unsigned i=0;i<3;i++){uint8_t*n=((uint8_t*[]){root,panel,widget})[i];for(unsigned j=0;j<2;j++){n[0x50+j]=0;DENY("Cadena oculta/inactiva conserva fallback");n[0x50+j]=1;}}
 Q(widget,0x30,widget);DENY("Ciclo de padres se rechaza");Q(widget,0x30,panel);Q(model,0,1048577);DENY("Modelo excesivo se rechaza");Q(model,0,1);Q(entry,8,1);Q(entry,0,entry);DENY("Ciclo de modelo se rechaza");Q(entry,0,0);Q(entry,8,0x7d8f57e3857c067dull);
 for(int kind=0;kind<=5;kind++){if(kind>=1&&kind<=4)continue;player[0x1bd]=kind;DENY("Tipo de descriptor nativo inválido no llama setter");}player[0x1bd]=1;
 for(unsigned i=0;i<8;i++){size_t at=((size_t[]){0x3e4594,0x3e4764,0x3e2dd4,0x3703ec,0x459ad0,0x454f40,0x43f170,0x3e2f70})[i];m.base[at]^=1;DENY("Preimagen alterada no activa frontera");CHECK(!HUD(),"HUD rechaza ABI distinta");m.base[at]^=1;}
 uint64_t required[]={w,w+0xa88,w+0xb08,w+0xb0c,w+0xb18,base+0x2f05458,r+0xd0,(uint64_t)fm+0x178,(uint64_t)fm+0x190,base+0x2d20e4c,(uint64_t)player+0x1bd,(uint64_t)inventory+0x40,(uint64_t)model,(uint64_t)wrapper+16};
 for(unsigned i=0;i<sizeof required/sizeof*required;i++){denied=required[i];DENY("Lectura inválida conserva fallback");}denied=0;
 cancel_at=(uint64_t)inventory+0x40;for(int k=1;k<=6;k++){cancel_kind=k;DENY("Cambio durante snapshot invalida acción");atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);*root_pointer=r;*hover_pointer=w;*xy_pointer=0;busy=0;}
 model_cancel=1;int before_toggle=toggle_calls;CHECK(!CLICK()&&before_toggle==toggle_calls,"Setter que cambia generación no abre un root viejo");model_cancel=0;atomic_store(&g_ui_gen,12);
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;unsetenv("DM_MAC_POINTER_LOG");
 uint64_t clear_regs[31]={0};clear_regs[0]=w;clear_regs[19]=(uint64_t)fm;
 // A7/A8 producers use original binary metadata and frames, not host vectors.
 uint8_t *skill_records=calloc(500,0x2ec);CHECK(skill_records!=NULL,"Memoria de tabla original para filtro");if(!skill_records)return 2;region(skill_records,500*0x2ec);for(int i=0;i<500;i++){int16_t v=i;memcpy(skill_records+i*0x2ec,&v,2);}
 _Alignas(8) uint8_t native_frame[0x300]={0};region(native_frame,sizeof native_frame);uint64_t fp=(uint64_t)native_frame+0x200;
 Q(metadata,0x11e8,skill_records);Q(metadata,0x11f0,500);Q(panel,0,base+0x20c5480);strcpy(name,"SkillSelect");
 uint64_t learned[31],common_regs[31];for(unsigned i=0;i<31;i++)learned[i]=common_regs[i]=0x123000+i;
 learned[20]=(uint64_t)left;learned[23]=common_regs[23]=(uint64_t)panel;learned[29]=common_regs[29]=fp;common_regs[0]=common_regs[25]=base+0x2f12d20;common_regs[1]=fp-0xe0;
 struct{int id,l,r;} capabilities[]={{0,1,1},{1,0,0},{2,1,1},{3,0,1},{4,1,1},{5,0,0},{357,0,0},{358,0,0},{359,0,0},{360,0,0},{361,0,0},{362,0,0},{363,0,0},{364,0,0},{370,0,0},{395,1,1}};
 CHECK(pc_skill_filter_abi(&m),"Filtro valida preimágenes nativas de productor y capacidad");
 for(unsigned i=0;i<sizeof capabilities/sizeof*capabilities;i++){
  int id=capabilities[i].id;uint8_t*record=skill_records+id*0x2ec;record[0x27]=capabilities[i].l<<6;record[0x28]=capabilities[i].r<<7;Q(left,0,record);learned[8]=(uint64_t)record;I(native_frame,0x120,id);common_regs[8]=(uint32_t)id;
  for(int pick_side=1;pick_side<=2;pick_side++){
   side=pick_side;I(native_frame,0x84,pick_side);int skip=!(pick_side==1?capabilities[i].l:capabilities[i].r);
#ifdef DM_TEST_SKILL_FILTER_BASELINE
   int a7=0,a8=0;
#else
   int a7=pc_skill_filter_route(&m,learned,desktop,0,read_memory),a8=pc_skill_filter_route(&m,common_regs,desktop,1,read_memory);
#endif
   CHECK(a7==skip,"A7 coincide con capacidad del validador nativo por lado");CHECK(a8==skip,"A8 usa la misma capacidad sin blacklist de IDs");
  }
 }
 uint8_t*blocked=skill_records+357*0x2ec;Q(left,0,blocked);learned[8]=(uint64_t)blocked;I(native_frame,0x120,357);common_regs[8]=357;side=1;I(native_frame,0x84,1);
#define FILTER(k) pc_skill_filter_route(&m,(k)?common_regs:learned,desktop,(k),read_memory)
#define FILTER_DENY(t) do{CHECK(!FILTER(0)&&!FILTER(1),t);}while(0)
 blocked[0x27]=0x80;blocked[0x28]=0x40;CHECK(FILTER(0)&&FILTER(1),"Otros bits no se interpretan como capacidad izquierda");side=2;I(native_frame,0x84,2);CHECK(FILTER(0)&&FILTER(1),"Otros bits no se interpretan como capacidad derecha");side=1;I(native_frame,0x84,1);blocked[0x27]=blocked[0x28]=0;
 blocked[0x27]=0x40;CHECK(!FILTER(0)&&!FILTER(1),"Una capacidad real de357 se conservaría: no hay exclusión por ID");blocked[0x27]=0;
 panel[0x50]=panel[0x51]=0;g_pc_options_open.pointer=0;Q(fm,0x178,0);Q(inventory,0x40,player);g_pc_pointer_hover.epoch++;CHECK(FILTER(0)&&FILTER(1),"OnOpen filtra oculto/sinhover/conheld y no depende de epoch");panel[0x50]=panel[0x51]=1;g_pc_options_open.pointer=1;Q(fm,0x178,widget);Q(inventory,0x40,0);g_pc_pointer_hover.epoch--;
 desktop=0;reads=0;FILTER_DENY("Legacy no filtra candidatos");CHECK(!reads,"Filtros legacy hacen cero lecturas de memoria");desktop=1;
 uint64_t saved_regs[31];memcpy(saved_regs,learned,sizeof saved_regs);g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;CHECK(dm_ui_pc_skill_candidate(learned)&&!memcmp(saved_regs,learned,sizeof saved_regs),"Callback A7 real sólo devuelve bool, preserva31GPR");memcpy(saved_regs,common_regs,sizeof saved_regs);CHECK(dm_ui_pc_skill_common(common_regs)&&!memcmp(saved_regs,common_regs,sizeof saved_regs),"Callback A8 real sólo devuelve bool, preserva31GPR");
 learned[29]=common_regs[29]=0;reads=0;FILTER_DENY("Frame inválido conserva fallback");CHECK(!reads,"Frame fuera de rango no causa lecturas VM");learned[29]=common_regs[29]=fp;
 root[0xd9]=0;FILTER_DENY("Frontend conserva llenado original");root[0xd9]=1;Q(panel,0,base+0x208a5f8);FILTER_DENY("Otra clase no filtra");Q(panel,0,base+0x20c5480);strcpy(name,"OtherPanel");FILTER_DENY("Otro panel no filtra");strcpy(name,"SkillSelect");Q(root,0x60,1);FILTER_DENY("Panel fuera del árbol no filtra");Q(root,0x60,2);
 for(int native_side=0;native_side<=3;native_side++){if(native_side==1||native_side==2)continue;I(native_frame,0x84,native_side);FILTER_DENY("Lado fuera del enum1/2 conserva fallback");}I(native_frame,0x84,1);side=2;FILTER_DENY("Modelo y frame incoherentes conservan fallback");side=1;
 Q(metadata,0x11f0,0);FILTER_DENY("Tabla sin cantidad conserva fallback");Q(metadata,0x11f0,1048577);FILTER_DENY("Tabla excesiva conserva fallback");Q(metadata,0x11f0,500);Q(metadata,0x11e8,UINT64_MAX-100);FILTER_DENY("Array con overflow conserva fallback");Q(metadata,0x11e8,skill_records);
 I(native_frame,0x120,500);common_regs[8]=500;CHECK(!FILTER(1),"A8 no indexa más allá de la cantidad");I(native_frame,0x120,-1);common_regs[8]=UINT32_MAX;CHECK(!FILTER(1),"A8 rechaza ID negativo");I(native_frame,0x120,357);common_regs[8]=358;CHECK(!FILTER(1),"A8 rechaza ID desparejado de W8");common_regs[8]=357;
 common_regs[0]++;CHECK(!FILTER(1),"A8 sólo observa vector common original");common_regs[0]--;common_regs[25]++;CHECK(!FILTER(1),"A8 valida X25 original del vector");common_regs[25]--;common_regs[1]+=4;CHECK(!FILTER(1),"A8 exige el puntero X1 del slot nativo de ID");common_regs[1]-=4;
 learned[8]+=0x2ec;CHECK(!FILTER(0),"A7 rechaza record distinto del nodo");learned[8]-=0x2ec;Q(left,0,record1);CHECK(!FILTER(0),"A7 no confunde record ajeno a tabla");Q(left,0,blocked);learned[20]=(uint64_t)right;CHECK(!FILTER(0),"A7 no usa record de otro nodo");learned[20]=(uint64_t)left;Q(skills,0,right);CHECK(!FILTER(0),"A7 exige nodo del jugador actual");Q(skills,0,left);
 int16_t bad_id=356;memcpy(blocked,&bad_id,2);FILTER_DENY("ID del registro debe coincidir con índice de tabla");bad_id=357;memcpy(blocked,&bad_id,2);
 const size_t filter_code[]={0xc06378,0xc06394,0xc06398,0xc064dc,0xc065a8,0xc065b8,0xc068c8,0xc066b0,0xc068e4,0xc068f8,0xc0690c,0xc08cc8,0x6db98c,0x6db9a8};for(unsigned i=0;i<sizeof filter_code/sizeof*filter_code;i++){m.base[filter_code[i]]^=1;FILTER_DENY("Código nativo distinto conserva fallback");m.base[filter_code[i]]^=1;}
 for(int k=0;k<=5;k++){if(k>=1&&k<=4)continue;player[0x1bd]=k;FILTER_DENY("Descriptor clase fuera del rango nativo conserva fallback");}player[0x1bd]=1;
 uint64_t filter_read[]={fp-0x17c,(uint64_t)panel,(uint64_t)panel+8,(uint64_t)player+0x1bd,(uint64_t)metadata+0x11e8,(uint64_t)metadata+0x11f0,(uint64_t)blocked,(uint64_t)blocked+0x27,base+0x2f05458};for(unsigned i=0;i<sizeof filter_read/sizeof*filter_read;i++){denied=filter_read[i];FILTER_DENY("Lectura esencial ilegible conserva fallback");}denied=0;
 cancel_at=(uint64_t)blocked+0x27;cancel_kind=3;FILTER_DENY("Raíz que cambia durante metadatos conserva fallback");*root_pointer=r;

 // FocusManager clear must preserve the physical hover of the PC skill selector.
 uint8_t *hover_record=skill_records+395*0x2ec;hover_record[0x28]=0x80;Q(left,0,hover_record);I(left,0x38,0);I(widget,0xa88,0);I(widget,0xb08,395);side=2;
 uint64_t hover_regs[31],hover_saved[31];for(unsigned i=0;i<31;i++)hover_regs[i]=0xbaa00+i;hover_regs[0]=w;hover_regs[19]=(uint64_t)fm;hover_regs[20]=0;memcpy(hover_saved,hover_regs,sizeof hover_regs);
 int hover_result=dm_ui_pc_skill_grid_hover(hover_regs);CHECK(hover_result==1,"A3 conserva hover físico de Miasma con nivel base 0");int hover_intact=!memcmp(hover_regs,hover_saved,sizeof hover_regs);CHECK(hover_intact,"A3 Skills conserva los 31 GPR");
 dm_ui_pc_grid_hover(hover_regs);CHECK(hover_regs[8]==1,"Dispatched A3 wrapper preserves skill hover without the item module");hover_regs[8]=hover_saved[8];CHECK(!memcmp(hover_regs,hover_saved,sizeof hover_regs),"A3 wrapper changes only the branch-result register");

#define SKILL_HOVER() pc_skill_hover_route(&m,hover_saved,desktop,read_memory)
#define HOVER_DENY(t) CHECK(!SKILL_HOVER(),t)
 g_pc_skill_event.valid=0;CHECK(SKILL_HOVER(),"Hover no requiere eventoUp ni gesto de selección");g_pc_skill_event.valid=1;
 desktop=0;reads=0;HOVER_DENY("Hover legacy conserva clear");CHECK(!reads,"Hover legacy hace cero lecturas");desktop=1;
 I(widget,0xa88,1);HOVER_DENY("HUD no se incluye en preservaciónSkills");I(widget,0xa88,0);
 side=0;HOVER_DENY("Popup sin modelo de lado conserva clear");side=1;hover_record[0x27]=0;HOVER_DENY("Habilidad sin capacidad izquierda conserva clear");hover_record[0x27]=0x40;CHECK(SKILL_HOVER(),"La capacidad izquierda real conserva hover");side=2;
 Q(inventory,0x40,player);HOVER_DENY("Mano ocupada conserva clear");Q(inventory,0x40,0);
 I(widget,0xb0c,99);HOVER_DENY("Asociación ajena conserva clear");I(widget,0xb0c,-1);Q(skills,0,right);HOVER_DENY("Nodo ausente del jugador conserva clear");Q(skills,0,left);
 Q(left,0,record1);HOVER_DENY("Record ajeno conserva clear");Q(left,0,hover_record);
 m.base[0x2d128f0]=1;HOVER_DENY("F0 original1 conserva su flujo");m.base[0x2d128f0]=0;m.base[0x2d128ec]=1;HOVER_DENY("EC original conserva clear");m.base[0x2d128ec]=0;
 hover_saved[19]++;HOVER_DENY("Otro FocusManager no conserva hover");hover_saved[19]--;hover_saved[20]=(uint64_t)panel;HOVER_DENY("Contenedor fuera del contrato conserva clear");Q(fm,0x170,panel);CHECK(SKILL_HOVER(),"Contenedor visible propio conserva hover");Q(fm,0x170,0);hover_saved[20]=0;
 for(unsigned i=0;i<3;i++){uint8_t*n=((uint8_t*[]){root,panel,widget})[i];for(unsigned j=0;j<2;j++){n[0x50+j]=0;HOVER_DENY("Ancestro oculto/inactivo conserva clear");n[0x50+j]=1;}}
 g_pc_pointer_hover.epoch++;HOVER_DENY("Hover epoch obsoleto conserva clear");g_pc_pointer_hover.epoch--;g_pc_pointer_hover.generation++;HOVER_DENY("Hover generación obsoleta conserva clear");g_pc_pointer_hover.generation--;g_pc_pointer_hover.x++;HOVER_DENY("Hover XY obsoleto conserva clear");g_pc_pointer_hover.x--;g_pc_options_open.pointer=0;HOVER_DENY("Sin puntero físico conserva clear");g_pc_options_open.pointer=1;
 Q(metadata,0x11f0,0);HOVER_DENY("Tabla vacía no conserva hover");Q(metadata,0x11f0,500);
 cancel_at=(uint64_t)inventory+0x40;for(int k=1;k<=8;k++){cancel_kind=k;HOVER_DENY("Cancelación/modo cambiado durante lectura de held conserva clear");atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);*root_pointer=r;*hover_pointer=w;*xy_pointer=0;busy=0;ingame=1;atomic_store(&g_load_up,0);}
 uint64_t hover_targets[]={(uint64_t)&side,(uint64_t)metadata+0x11f0,(uint64_t)root+0x60,(uint64_t)left,base+0x2d20e4c,base+0x2d60380,(uint64_t)hover_record+0x28,(uint64_t)widget+0xb08,(uint64_t)fm+0x170};
 uint64_t hover_values[]={1,499,1,(uint64_t)record1,8,UINT32_MAX,0,42,(uint64_t)panel};size_t hover_widths[]={4,8,8,8,4,4,1,4,8};
 for(unsigned i=0;i<sizeof hover_targets/sizeof*hover_targets;i++){uint64_t old_value=0;memcpy(&old_value,(void*)hover_targets[i],hover_widths[i]);mutate_at=(uint64_t)inventory+0x40;mutate_target=hover_targets[i];mutate_value=hover_values[i];mutate_size=hover_widths[i];HOVER_DENY("Cambio de modelo/tabla/árbol/nodo/actor/capacidad/widget/container invalida snapshot");memcpy((void*)hover_targets[i],&old_value,hover_widths[i]);mutate_at=0;}

 _Alignas(8) uint8_t hover_modal[0x100]={0},hover_modal_leaf[0x100]={0};char hover_modal_name[]="ConfirmationModal";uint64_t hover_root_children[]={(uint64_t)fm,(uint64_t)panel,(uint64_t)hover_modal},hover_modal_children[]={(uint64_t)hover_modal_leaf};
 region(hover_modal,sizeof hover_modal);region(hover_modal_leaf,sizeof hover_modal_leaf);region(hover_modal_name,sizeof hover_modal_name);region(hover_root_children,sizeof hover_root_children);region(hover_modal_children,sizeof hover_modal_children);
 Q(hover_modal,8,hover_modal_name);hover_modal[0x50]=hover_modal[0x51]=hover_modal_leaf[0x50]=hover_modal_leaf[0x51]=1;Q(hover_modal,0x58,hover_modal_children);Q(hover_modal,0x60,1);Q(root,0x58,hover_root_children);Q(root,0x60,3);HOVER_DENY("Modal visible conserva clear y su prioridad nativa");Q(root,0x58,children);Q(root,0x60,2);
 CHECK(SKILL_HOVER(),"Tras rechazos válidos el hover nativo vuelve a funcionar");
 // Restore the fixture; production never changes these tables.
 Q(left,0,record1);Q(panel,0,0);strcpy(name,"HUDPanel");I(widget,0xa88,1);Q(metadata,0x11e8,0);Q(metadata,0x11f0,0);side=1;
 int pipes[2];CHECK(pipe(pipes)==0,"Pipe para evidencia de no I/O");int old=dup(2);dup2(pipes[1],2);close(pipes[1]);uint64_t cb[31]={0};cb[19]=w;CHECK(dm_ui_pc_skill_hud(cb),"Callback A6 usa lector MachVM real");uint64_t clear_saved[31];clear_regs[8]=1;memcpy(clear_saved,clear_regs,sizeof clear_saved);int clear_result=dm_ui_pc_skill_grid_hover(clear_regs);int clear_intact=!clear_result&&!memcmp(clear_regs,clear_saved,sizeof clear_regs);CHECK(clear_intact,"A3 Skills conserva rechazo funcional, registros y diag off");desktop=0;uint64_t before[31];memcpy(before,cb,sizeof cb);dm_ui_pc_skill_click(cb);CHECK(!dm_ui_pc_skill_hud(cb)&&!memcmp(before,cb,sizeof cb),"Callbacks legacy preservan registros");desktop=1;fflush(stderr);dup2(old,2);close(old);char byte;fcntl(pipes[0],F_SETFL,O_NONBLOCK);ssize_t got=read(pipes[0],&byte,1);close(pipes[0]);CHECK(got==0||(got<0&&errno==EAGAIN),"Callbacks sin env no emiten diagnóstico");
 CHECK(!m.base[0x2d128f0]&&!m.base[0x2d128ec]&&!*(uint64_t*)(fm+0x190)&&*(int*)(widget+0xb18)==-1,"Sin escrituras host de foco/F0/binding");
 free(skill_records);free(m.base);printf("PC SKILL WIDGETS %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
