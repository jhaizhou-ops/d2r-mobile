// Contrato de clasificación PC sin app ni estado persistente.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int checks,failures,hits;
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static uint8_t nodes[6][256];static uint64_t children[6][4];
static uint64_t node(int i,const char *name,uint64_t vt){memset(nodes[i],0,256);*(uint64_t*)nodes[i]=vt;*(const char**)(nodes[i]+8)=name;nodes[i][0x50]=nodes[i][0x51]=1;*(uint64_t*)(nodes[i]+0x58)=(uint64_t)children[i];return(uint64_t)nodes[i];}
static int hit(module_t*m,uint64_t w,uint64_t xy){(void)m;(void)xy;hits++;return*(int*)(w+0x90);}
static int keyboard_calls,controller_calls,last_action,last_type;
static const char*keyboard(int action,int type){keyboard_calls++;last_action=action;last_type=type;return"QWER";}
static const char*controller(int action,int type){controller_calls++;last_action=action;last_type=type;return"mando";}
static int constructed;static const char*label_value="Tecla";
static const char*skill_label(int action,int type){last_action=action;last_type=type;return label_value;}
static void construct(void*out,const char*label){constructed++;strcpy(out,label);}
static int test_read(uint64_t a,void *p,size_t n){return pc_tooltip_read(a,p,n);}
static void test_item_drop(void){
 module_t m={0};uint64_t root=node(0,"Root",0),hud=node(1,"HUDPanel",0),inv=node(2,"PlayerInventoryExpansionLayout",0x208a398),leaf=node(3,"grid",0x2089ab8),other=node(4,"VendorPanelLayout",0),extra=node(5,"Descendant",0);
 children[0][0]=hud;children[0][1]=inv;*(uint64_t*)(root+0x60)=2;children[2][0]=leaf;*(uint64_t*)(inv+0x60)=1;
 CHECK(pc_world_tree(&m,root,0,test_read,hit),"Inventario visible con mundo descubierto admite la política PC no modal");
 *(const char**)(inv+8)="PlayerInventoryOriginalLayout";CHECK(pc_world_tree(&m,root,0,test_read,hit),"Inventario Original conserva la misma clase y semántica no modal");*(const char**)(inv+8)="PlayerInventoryExpansionLayout";
 *(int*)(inv+0x90)=1;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Dentro del panel nunca suelta al suelo");*(int*)(inv+0x90)=0;
 *(int*)(leaf+0x90)=1;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Descendiente activo fuera del rect del panel aún bloquea drop");nodes[3][0x51]=0;CHECK(pc_world_tree(&m,root,0,test_read,hit),"Descendiente oculto no intercepta el mundo");nodes[3][0x51]=1;*(int*)(leaf+0x90)=0;
 children[1][0]=extra;*(uint64_t*)(hud+0x60)=1;*(uint64_t*)extra=0x208a5f8;*(int*)(extra+0x90)=1;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Botón HUD no se convierte en drop con mano ocupada");*(int*)(extra+0x90)=0;
 const char *blocked[]={"VendorPanelLayout","BankExpansionLayout","ConfirmationModal","SettingsPanel","NpcDialogPanel"};children[0][2]=other;*(uint64_t*)(root+0x60)=3;
 for(unsigned i=0;i<sizeof blocked/sizeof *blocked;i++){*(const char**)(other+8)=blocked[i];CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Otro panel activo bloquea globalmente la excepción drop");}
 nodes[4][0x50]=0;CHECK(pc_world_tree(&m,root,0,test_read,hit),"Panel cerrado no bloquea la consulta actual");nodes[4][0x50]=1;*(uint64_t*)(root+0x60)=2;
 nodes[2][0x51]=0;CHECK(pc_world_tree(&m,root,0,test_read,hit),"Inventario inactivo no bloquea el mundo descubierto");nodes[2][0x51]=1;*(uint64_t*)inv=0;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Nombre de inventario con otra clase falla cerrado");*(uint64_t*)inv=0x208a398;
 *(uint64_t*)(inv+0x60)=257;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Árbol grande malformado no se recorre");*(uint64_t*)(inv+0x60)=1;children[2][0]=1;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Descendiente ilegible falla antes del hit-test");children[2][0]=leaf;
 children[2][0]=inv;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Ciclo de widgets tiene presupuesto y falla cerrado");children[2][0]=leaf;
 CHECK(!pc_world_tree(&m,1,0,test_read,hit)&&!pc_world_tree(NULL,root,0,test_read,hit),"Root ilegible o módulo ausente no desreferencian memoria");
}
static int area_reads,area_mutation;static module_t *area_module;static uint64_t area_root;
static uint32_t area_gen(void){return atomic_load(&g_ui_gen);}
static int area_read(uint64_t a,void *out,size_t n){area_reads++;return pc_tooltip_read(a,out,n);}
static int area_hit(module_t *m,uint64_t w,uint64_t xy){if(area_mutation==1)atomic_fetch_add(&g_ui_gen,1);if(area_mutation==2)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(area_mutation==3)*(uint64_t*)(area_module->base+0x2f05458)=0;if(area_mutation==4)*(uint8_t*)(area_root+0x50)=0;return hit(m,w,xy);}
static void test_world_area(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);CHECK(m.base!=NULL,"Imagen privada para contrato completo de clasificación");if(!m.base)return;
 uint64_t base=(uintptr_t)m.base,root=node(0,"Root",base),hud=node(1,"HUDPanel",base),inventory=node(2,"PlayerInventoryExpansionLayout",base+0x208a398),leaf=node(3,"grid",base+0x2089ab8);
 children[0][0]=hud;children[0][1]=inventory;*(uint64_t*)(root+0x60)=2;children[2][0]=leaf;*(uint64_t*)(inventory+0x60)=1;nodes[0][0xd9]=1;*(uint64_t*)(m.base+0x2f05458)=root;*(int32_t*)(m.base+0x211da38)=3024;*(int32_t*)(m.base+0x211da3c)=1700;area_module=&m;area_root=root;
 #define AREA(enabled,callback) pc_world_area_read(&m,.43f,.53f,enabled,area_read,callback,pc_world_generation)
 CHECK(AREA(1,area_hit),"Snapshot actual root/generación/época permite mundo con inventario descubierto");
 *(int*)(inventory+0x90)=1;CHECK(!AREA(1,area_hit)&&AREA(1,NULL),"Política global mantiene apuntado posible, pero el punto cubierto sigue siendo UI");*(int*)(inventory+0x90)=0;
 area_reads=0;CHECK(!AREA(0,area_hit)&&!area_reads,"FueraPC/pausa/carga/busy no consulta árbol guest");
 for(int k=1;k<=4;k++){area_mutation=k;CHECK(!AREA(1,area_hit),"Cambio de generación/época/raíz/visibilidad durante hit invalida la consulta");*(uint64_t*)(m.base+0x2f05458)=root;nodes[0][0x50]=1;}area_mutation=0;
 g_pc_options_event_scope=(pc_options_event_scope){.active=1,.epoch=atomic_load(&g_pc_tabbar_epoch)-1};area_reads=0;CHECK(!AREA(1,area_hit)&&!area_reads,"Evento de época anterior falla antes de leer widgets");g_pc_options_event_scope=(pc_options_event_scope){0};
 *(uint64_t*)(m.base+0x2f05458)=1;CHECK(!AREA(1,area_hit),"Root ilegible no se sigue");*(uint64_t*)(m.base+0x2f05458)=root;
 nodes[0][0xd9]=0;CHECK(!AREA(1,area_hit),"Frontend no se clasifica como mundo");nodes[0][0xd9]=1;
 *(int32_t*)(m.base+0x211da3c)=0;CHECK(!AREA(1,area_hit),"Viewport inválido impide conversión de coordenadas");*(int32_t*)(m.base+0x211da3c)=1700;
 CHECK(!pc_world_area_read(&m,NAN,.5f,1,area_read,area_hit,area_gen),"Coordenadas no finitas no se convierten a enteros");
 free(m.base);
 #undef AREA
}
int main(void){test_item_drop();test_world_area();module_t m={0};uint64_t root=node(0,"Root",0),hud=node(1,"HUDPanel",0),control=node(2,"Control",0x208a5f8);children[0][0]=hud;*(uint64_t*)(root+0x60)=1;children[1][0]=control;*(uint64_t*)(hud+0x60)=1;
 const uint64_t types[]={0x208a5f8,0x2088fc8,0x20c4008,0x20c1ff8,0x20c3c28,0x20c3d98,0x20c47d8,0x20b7b00};
 for(int i=0;i<8;i++){*(uint64_t*)control=types[i];*(int*)(control+0x90)=1;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Cada clase HUD nativa bloquea mundo solo al acertar su hitbox");*(int*)(control+0x90)=0;CHECK(pc_world_tree(&m,root,0,test_read,hit),"Fuera del control HUD sigue siendo mundo");}
 *(uint64_t*)control=0x20c1328;*(int*)(control+0x90)=1;CHECK(pc_world_tree(&m,root,0,test_read,hit),"BGClickCatcher no absorbe mundo");*(uint64_t*)control=0x2088fc8;nodes[2][0x51]=0;CHECK(pc_world_tree(&m,root,0,test_read,hit),"Visibilidad heredada oculta control");nodes[2][0x51]=1;nodes[1][0x50]=0;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"HUD ausente impide mundo");nodes[1][0x50]=1;
 uint64_t panel=node(3,"SkillSelect",0),leaf=node(4,"Skill",0);children[3][0]=leaf;*(uint64_t*)(panel+0x60)=1;children[0][1]=panel;*(uint64_t*)(root+0x60)=2;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Selector PC bloquea todo cast mundo");nodes[3][0x50]=0;*(int*)(control+0x90)=0;CHECK(pc_world_tree(&m,root,0,test_read,hit),"Cerrar selector devuelve mundo");*(uint64_t*)(hud+0x60)=257;CHECK(!pc_world_tree(&m,root,0,test_read,hit),"Árbol malformado falla cerrado");
 for(int action=23;action<=26;action++)CHECK(!strcmp(pc_belt_label(1,action,1,1,keyboard,controller),"QWER")&&last_action==action&&last_type==1,"BeltPC consulta etiqueta nativa teclado preservando acción/tipo");
 CHECK(!strcmp(pc_belt_label(0,23,1,1,keyboard,controller),"mando"),"Legacy conserva lector mando");CHECK(!strcmp(pc_belt_label(1,23,1,0,keyboard,controller),"mando"),"Clase ajena a Belt no cambia lector");CHECK(!strcmp(pc_belt_label(1,22,1,1,keyboard,controller),"mando"),"Acción ajena al cinturón no cambia lector");CHECK(!strcmp(pc_belt_label(1,23,0,1,keyboard,controller),"mando"),"Tipo inesperado conserva contrato original");CHECK(keyboard_calls==4&&controller_calls==4,"Un solo lector por llamada; sin sustitución global");
 char out[32];for(int slot=0;slot<16;slot++)CHECK(pc_skill_label_build(1,slot,1,1,out,skill_label,construct)&&last_action==(slot<8?14+slot:46+slot-8)&&last_type==1&&!strcmp(out,"Tecla"),"16slots construyen etiqueta de acción original en sret");label_value=NULL;CHECK(pc_skill_label_build(1,0,1,1,out,skill_label,construct)&&!out[0],"Binding vacío construye string nativo vacío");int before=constructed;CHECK(!pc_skill_label_build(0,0,1,1,out,skill_label,construct)&&!pc_skill_label_build(1,16,1,1,out,skill_label,construct)&&!pc_skill_label_build(1,0,1,0,out,skill_label,construct)&&constructed==before,"Legacy, rango y clase ajena conservan formatter original");
 uint64_t list=node(3,"List",0),row=node(4,"Row KeyboardQuickCastEnabled",0);children[3][0]=row;*(uint64_t*)(list+0x60)=1;CHECK(pc_quickcast_visible(1,list,0,row,0)==1,"PC muestra solamente checkbox QuickCast original");CHECK(!pc_quickcast_visible(0,list,0,row,0)&&!pc_quickcast_visible(1,list,-1,row,0)&&!pc_quickcast_visible(1,list,1,row,0)&&!pc_quickcast_visible(1,list,0,control,0),"Legacy e identidad/índice incorrectos no cambian visibilidad");*(const char**)(row+8)="Row DisplayActiveSkillBindings";CHECK(!pc_quickcast_visible(1,list,0,row,0),"Otra fila oculta conserva decisión original");CHECK(pc_quickcast_visible(1,list,0,row,1)==1,"Visibilidad ya activa se conserva");
 CHECK(pc_real_point(.5f,-.1f,0,120,1920,1080,1920,1200),"Franja superior mundo16:10 acepta yUI negativo real");CHECK(pc_real_point(.5f,0,0,120,1920,1080,1920,1200)&&pc_real_point(.5f,.99f,0,120,1920,1080,1920,1200),"Interior UI conserva inversa yoffset");CHECK(!pc_real_point(.5f,-.2f,0,120,1920,1080,1920,1200)&&!pc_real_point(.5f,1,0,120,1920,1080,1920,1200)&&!pc_real_point(-.1f,.5f,0,120,1920,1080,1920,1200),"Fuera imagen real se rechaza sin clamp a UI");CHECK(!pc_real_point(NAN,.5f,0,120,1920,1080,1920,1200),"No finito nunca accede a hit-test nativo");
 uint8_t menu[512]={0};*(uint64_t*)menu=0x20c3680;*(const char**)(menu+8)="KeyBindingMenu";menu[0x50]=menu[0x51]=1;*(int*)(menu+0x194)=42;*(int*)(menu+0x190)=1;children[0][0]=(uint64_t)menu;*(uint64_t*)(root+0x60)=1;CHECK(pc_native_capture_active(&m,root),"Captura original activa se reconoce aun cuando KeyDown no consume root");*(int*)(menu+0x194)=69;CHECK(!pc_native_capture_active(&m,root),"Índice69 significa captura inactiva");*(int*)(menu+0x194)=0;*(int*)(menu+0x190)=2;CHECK(!pc_native_capture_active(&m,root),"Tipo de mando no se interpreta como captura teclado");*(int*)(menu+0x190)=0;*(uint64_t*)menu=0;CHECK(!pc_native_capture_active(&m,root),"Clase incorrecta nunca accede semántica captura");
 *(uint64_t*)menu=0x208e8d0;*(const char**)(menu+8)="YouDiedModal";CHECK(pc_death_visible(&m,root),"Continue sólo en modal original reconocido");menu[0x51]=0;CHECK(!pc_death_visible(&m,root),"Modal oculto no recibe Continue");menu[0x51]=1;*(uint64_t*)menu=0;CHECK(!pc_death_visible(&m,root),"Nombre YouDied sin clase original no basta");
 printf("PC WORLD HIT %d comprobaciones, %d fallos\n",checks,failures);return!!failures;}
