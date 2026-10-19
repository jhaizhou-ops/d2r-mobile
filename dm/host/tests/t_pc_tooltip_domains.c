// Dominios de tooltip físico: actor nativo y foco vivo, sin funciones guest.
#include "../dm_cursor.c"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame=1,busy,checks,failures,reads;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
uint64_t dm_guest_frame(void){return 42;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
static struct {uint64_t p;size_t n;} regions[32];static unsigned nr;
static void region(void *p,size_t n){regions[nr++]=(typeof(*regions)){(uint64_t)p,n};}
static uint64_t denied,cancel_address;static int cancel_mode;static uint64_t *cancel_root,*cancel_hover,*cancel_held;static int32_t *cancel_x;
static int fixture_read(uint64_t p,void *out,size_t n){reads++;if(p==denied)return 0;for(unsigned i=0;i<nr;i++)if(p>=regions[i].p&&n<=regions[i].n&&p-regions[i].p<=regions[i].n-n){if(cancel_mode&&p==cancel_address){int mode=cancel_mode;cancel_mode=0;if(mode==1)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(mode==2)atomic_fetch_add(&g_ui_gen,1);if(mode==3)*cancel_root=0;if(mode==4)*cancel_hover=0;if(mode==5)(*cancel_x)++;if(mode==6)busy=1;if(mode==7)atomic_store(&g_load_up,1);if(mode==8)ingame=0;if(mode==9)g_pc_options_open.pointer=0;if(mode==10)*cancel_held=1;}memcpy(out,(void*)p,n);return 1;}return 0;}
int main(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;region(m.base,m.size);
 FILE*f=fopen("dm/host/tests/build/main.orig.bin","rb");if(!f)f=fopen("build/main.orig.bin","rb");if(!f||fread(m.base,1,m.size,f)!=m.size)return 2;fclose(f);
 uint8_t root[0x200]={0},fm[0x200]={0},panel[0x200]={0},grid[0x700]={0},local[0x200]={0},shared[0x200]={0},npc[0x200]={0},inventory[0x100]={0},other[0x200]={0},map[16]={0},node[24]={0},wrapper[24]={0};uint32_t shared_id=2;
 char panel_name[64]="PlayerInventoryExpansionLayout";uint64_t children[]={(uint64_t)fm,(uint64_t)panel},leaves[]={(uint64_t)grid},bucket=(uint64_t)node;
 void *objects[]={root,fm,panel,grid,local,shared,npc,inventory,other,map,node,wrapper,&shared_id,panel_name,children,leaves,&bucket};size_t sizes[]={sizeof root,sizeof fm,sizeof panel,sizeof grid,sizeof local,sizeof shared,sizeof npc,sizeof inventory,sizeof other,sizeof map,sizeof node,sizeof wrapper,sizeof shared_id,sizeof panel_name,sizeof children,sizeof leaves,sizeof bucket};for(unsigned i=0;i<sizeof objects/sizeof*objects;i++)region(objects[i],sizes[i]);
 uint64_t base=(uint64_t)m.base,r=(uint64_t)root,w=(uint64_t)grid;
 Q(m.base,0x2f05458,root);Q(root,0xd0,fm);Q(root,0x58,children);Q(root,0x60,2);root[0x50]=root[0x51]=root[0xd9]=1;
 Q(fm,0,base+0x20b7f88);Q(fm,0x170,panel);Q(fm,0x178,grid);Q(panel,8,panel_name);Q(panel,0x30,root);Q(panel,0x58,leaves);Q(panel,0x60,1);panel[0x50]=panel[0x51]=1;
 Q(grid,0,base+DM_VT_SLOT_B);Q(grid,0x30,panel);grid[0x50]=grid[0x51]=1;grid[0x630]=4;*(uint32_t*)(grid+0x5c4)=1;
 *(uint32_t*)(local+8)=1;*(uint32_t*)(shared+8)=2;*(uint32_t*)npc=1;*(uint32_t*)(npc+8)=99;Q(local,0x90,inventory);*(uint32_t*)inventory=0x01020304;
 Q(m.base,0x2d603a0+8,local);Q(m.base,0x2d603a0+16,shared);Q(m.base,0x2d607a0+8*99,npc);*(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=1;
 Q(m.base,0x2f05450,map);Q(map,0,1);Q(map,8,&bucket);Q(node,8,0x8d4d18a675312bd4ull);Q(node,16,wrapper);Q(wrapper,16,&shared_id);*(uint32_t*)(m.base+0x2d5ebfc)=99;*(uint32_t*)(m.base+0x2d5ec00)=1;
 atomic_store(&g_load_up,0);atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);g_pc_options_open.pointer=1;g_pc_options_open.input_epoch=7;g_pc_pointer_hover=(pc_pointer_hover_state){.root=r,.valid=1,.generation=12,.epoch=7};
 uint64_t regs[31];for(unsigned i=0;i<31;i++)regs[i]=0x11100+i;regs[0]=w;regs[19]=(uint64_t)fm;regs[20]=(uint64_t)panel;
 const struct{const char*name;uint32_t id,type;} cases[]={{"PlayerInventoryExpansionLayout",1,0},{"PlayerInventoryOriginalLayout",1,0},{"BankExpansionLayout",1,0},{"BankOriginalLayout",1,0},{"BankExpansionLayout",2,0},{"BankOriginalLayout",2,0},{"VendorPanelLayout",99,1}};
 for(unsigned i=0;i<sizeof cases/sizeof*cases;i++){
  strcpy(panel_name,cases[i].name);*(uint32_t*)(grid+0x5c4)=cases[i].id;*(uint32_t*)(grid+0x5c8)=cases[i].type;
  m.base[0x2d128f0]=1;uint64_t callers[]={0x3b0910,0x3b0b68,0x3b0fa4,0x3b1d28};for(unsigned k=0;k<4;k++)CHECK(pc_item_tooltip_route(&m,r,w,base+callers[k],1,1,fixture_read),"A2 caller Update/Draw admite dominio y actor originales");
  m.base[0x2d128f0]=0;CHECK(pc_grid_hover_route(&m,regs,1,fixture_read),"A3 preserva hover válido del dominio, incluido Shared/NPC");
 }
#define A2() pc_item_tooltip_route(&m,r,w,base+0x3b1d28,desktop,1,fixture_read)
#define A3() pc_grid_hover_route(&m,regs,desktop,fixture_read)
#define DENY(t) do{m.base[0x2d128f0]=1;CHECK(!A2(),t);m.base[0x2d128f0]=0;CHECK(!A3(),t);}while(0)
 strcpy(panel_name,"BankExpansionLayout");*(uint32_t*)(grid+0x5c4)=1;*(uint32_t*)(grid+0x5c8)=0;
 grid[0x630]=3;DENY("Grid de cubo bajo Bank no se confunde con alijo");grid[0x630]=4;
 Q(m.base,0x2f05450,0);m.base[0x2d128f0]=1;CHECK(A2(),"Personal local no depende del modelo Shared");m.base[0x2d128f0]=0;CHECK(A3(),"Hover local Bank no necesita modelo Shared");Q(m.base,0x2f05450,map);
 *(uint32_t*)(grid+0x5c4)=2;shared_id=3;DENY("Actor Shared ajeno al ID de página se rechaza");shared_id=2;
 Q(node,8,0x111);DENY("Otro modelo no se confunde con Shared");Q(node,0,node);DENY("Ciclo de modelo Shared se limita");Q(node,0,0);Q(node,8,0x8d4d18a675312bd4ull);
 Q(wrapper,16,1);DENY("Valor Shared ilegible falla cerrado");Q(wrapper,16,&shared_id);Q(map,0,1048577);DENY("Tabla modelo excesiva falla cerrado");Q(map,0,1);Q(map,8,1);DENY("Buckets ilegibles no se desreferencian");Q(map,8,&bucket);
 shared_id=UINT32_MAX;DENY("ID Shared inválido falla cerrado");shared_id=2;Q(m.base,0x2d603a0+16,0);DENY("Shared sin actor real no habilita tooltip");Q(m.base,0x2d603a0+16,shared);
 m.base[0x3b5930]^=1;DENY("ABI de lector Shared diferente se rechaza");m.base[0x3b5930]^=1;
 *(uint32_t*)(grid+0x5c4)=99;*(uint32_t*)(grid+0x5c8)=1;DENY("Bank no acepta owner NPC aunque esté vivo");strcpy(panel_name,"VendorPanelLayout");
 *(uint32_t*)(m.base+0x2d5ebfc)=98;DENY("NPC distinto del vendedor activo se rechaza");*(uint32_t*)(m.base+0x2d5ebfc)=99;*(uint32_t*)(m.base+0x2d5ec00)=0;DENY("Interacción NPC finalizada conserva fallback");*(uint32_t*)(m.base+0x2d5ec00)=1;
 Q(m.base,0x2d607a0+8*99,0);DENY("Vendedor sin actor vivo conserva fallback");Q(m.base,0x2d607a0+8*99,npc);*(uint32_t*)(grid+0x5c4)=1;*(uint32_t*)(grid+0x5c8)=0;DENY("Tienda no trata al jugador como vendedor");
 strcpy(panel_name,"PlayerInventoryExpansionLayout");*(uint32_t*)(grid+0x5c4)=2;DENY("Inventario no acepta actor Shared como propio");*(uint32_t*)(grid+0x5c4)=1;
 // Consultas posteriores de precio y compra: cada caller tiene preimagen y dominio exactos.
 struct {const char *name;uint32_t id,type;uint64_t caller;uint64_t vt;} prices[]={
  {"PlayerInventoryExpansionLayout",1,0,0x3dd184,DM_VT_SLOT_B},
  {"PlayerInventoryOriginalLayout",1,0,0x3dd184,DM_VT_SLOT},
  {"VendorPanelLayout",99,1,0x407c38,DM_VT_SLOT_B},
  {"VendorPanelLayout",99,1,0x409490,DM_VT_SLOT_B},
  {"VendorPanelLayout",99,1,0x4094a0,DM_VT_SLOT_B},
  {"VendorPanelLayout",99,1,0x40b08c,DM_VT_SLOT_B}};
 *(uint32_t*)(m.base+0x2d5ec00)=1;m.base[0x2d5ff53]=1;
 for(unsigned i=0;i<sizeof prices/sizeof*prices;i++){
  strcpy(panel_name,prices[i].name);*(uint32_t*)(grid+0x5c4)=prices[i].id;*(uint32_t*)(grid+0x5c8)=prices[i].type;Q(grid,0,base+prices[i].vt);m.base[0x2d128f0]=1;
  uint64_t lr=base+prices[i].caller;
#define PRICE() pc_item_tooltip_route(&m,r,w,lr,desktop,1,fixture_read)
  CHECK(PRICE(),"Getter de comercio nativo admite hover físico vigente en su dominio");
  Q(inventory,0x40,other);CHECK(!PRICE(),"Precio con mano ocupada conserva fallback");Q(inventory,0x40,0);
  Q(fm,0x178,other);CHECK(!PRICE(),"Precio sin hover propio conserva fallback");Q(fm,0x178,grid);
  g_pc_pointer_hover.epoch++;CHECK(!PRICE(),"Precio con epoch antiguo conserva fallback");g_pc_pointer_hover.epoch--;
  m.base[0x2d5ff53]=0;CHECK(!PRICE(),"Precio fuera de comercio conserva fallback");m.base[0x2d5ff53]=1;
  *(uint32_t*)(m.base+0x2d5ec00)=0;CHECK(!PRICE(),"Precio sin interacción NPC conserva fallback");*(uint32_t*)(m.base+0x2d5ec00)=1;
  m.base[prices[i].caller-4]^=1;CHECK(!PRICE(),"BL de precio alterado se rechaza");m.base[prices[i].caller-4]^=1;
  m.base[prices[i].caller+8]^=1;CHECK(!PRICE(),"Preparación posterior al getter de comercio alterada se rechaza");m.base[prices[i].caller+8]^=1;
  Q(m.base,0x2d607a0+8*99,0);CHECK(!PRICE(),"Precio sin actor NPC vivo se rechaza");Q(m.base,0x2d607a0+8*99,npc);
  denied=base+0x2d5ff53;CHECK(!PRICE(),"Flag de comercio ilegible conserva fallback");denied=0;
  denied=(uint64_t)npc;CHECK(!PRICE(),"NPC ilegible conserva fallback");denied=0;
  g_pc_pointer_hover.x++;CHECK(!PRICE(),"Precio con XY anterior conserva fallback");g_pc_pointer_hover.x--;
  if(i>=3){Q(grid,0,base+DM_VT_SLOT);CHECK(!PRICE(),"Caller de compra no acepta clase equipada");Q(grid,0,base+DM_VT_SLOT_B);}
  uint64_t endpoint=i<3?prices[i].caller+12:(i<5?0x4094bc:0x40b0a4);
  m.base[endpoint]^=1;CHECK(!PRICE(),"BL de formato o compra posterior alterado se rechaza");m.base[endpoint]^=1;
  uint64_t regs_price[31],saved_price[31];for(unsigned k=0;k<31;k++)regs_price[k]=0x45600+k;regs_price[8]=lr;regs_price[9]=1;regs_price[20]=w;memcpy(saved_price,regs_price,sizeof regs_price);g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;dm_ui_pc_item_tooltip(regs_price);
  int intact=regs_price[9]==0;for(unsigned k=0;k<31;k++)if(k!=9&&regs_price[k]!=saved_price[k])intact=0;CHECK(intact,"Callback de precio sólo cambia W9 local");
  lr++;CHECK(!PRICE(),"Caller vecino no añade permiso de precio");lr--;
  strcpy(panel_name,!i?"BankExpansionLayout":i==1?"VendorPanelLayout":"PlayerInventoryExpansionLayout");CHECK(!PRICE(),"Getter de precio fuera de su dominio exacto se rechaza");
  strcpy(panel_name,prices[i].name);desktop=0;reads=0;CHECK(!PRICE()&&!reads,"Precio legacy no lee memoria de juego");desktop=1;
#undef PRICE
 }
 strcpy(panel_name,"PlayerInventoryExpansionLayout");*(uint32_t*)(grid+0x5c4)=1;*(uint32_t*)(grid+0x5c8)=0;Q(grid,0,base+DM_VT_SLOT_B);
 const char *foreign[]={"HoradricCubeLayout","HirelingInventoryPanel","VendorPanel","UnknownPanel"};for(unsigned i=0;i<4;i++){strcpy(panel_name,foreign[i]);DENY("Dominios no autorizados conservan política original");}strcpy(panel_name,"BankOriginalLayout");
 Q(panel,8,1);DENY("Nombre ilegible no se usa como cadena C");Q(panel,8,panel_name);memset(panel_name,'A',sizeof panel_name);DENY("Nombre sin terminación dentro del límite falla cerrado");strcpy(panel_name,"BankOriginalLayout");
 Q(grid,0x30,1);DENY("Padre ilegible no se desreferencia");Q(grid,0x30,panel);Q(panel,0x30,grid);DENY("Ciclo de padres se limita");Q(panel,0x30,root);Q(panel,0x58,1);DENY("Array hijos ilegible conserva fallback");Q(panel,0x58,leaves);Q(root,0x60,257);DENY("Raíz con exceso de hijos conserva fallback");Q(root,0x60,2);
 for(unsigned i=0;i<3;i++){uint8_t*n=((uint8_t*[]){root,panel,grid})[i];for(unsigned k=0;k<2;k++){n[0x50+k]=0;DENY("Nodo oculto/inactivo no habilita descripción");n[0x50+k]=1;}}
 uint64_t three[]={(uint64_t)fm,(uint64_t)panel,1};region(three,sizeof three);Q(root,0x58,three);Q(root,0x60,3);DENY("Vecino ilegible falla cerrado, sin crash");three[2]=(uint64_t)other;Q(other,8,panel_name);other[0x50]=other[0x51]=1;char modal_name[]="ConfirmationModal";region(modal_name,sizeof modal_name);Q(other,8,modal_name);Q(other,0x58,leaves);Q(other,0x60,1);DENY("Modal con contenido visible mantiene bloqueo");Q(other,0x60,0);m.base[0x2d128f0]=1;CHECK(A2(),"Modal vacío conserva semántica original sin bloqueo");m.base[0x2d128f0]=0;CHECK(A3(),"Modal vacío no inventa contenido");Q(other,0x58,1);Q(other,0x60,1);DENY("Contenido modal ilegible no habilita tooltip");Q(root,0x58,children);Q(root,0x60,2);
 Q(inventory,0x40,other);DENY("Mano local ocupada conserva fallback en Bank");Q(inventory,0x40,0);*(uint32_t*)inventory=0;DENY("Inventario local inválido no se sustituye por Shared");*(uint32_t*)inventory=0x01020304;
 *(uint32_t*)(m.base+0x2d60380)=UINT32_MAX;DENY("Jugador local ausente no habilita otro actor");*(uint32_t*)(m.base+0x2d60380)=1;
 Q(fm,0x178,0);DENY("HoverNULL no se inventa");Q(fm,0x178,grid);root[0xd9]=0;DENY("Fuera de partida se conserva fallback");root[0xd9]=1;
 desktop=0;reads=0;DENY("Legacy no adapta Bank");CHECK(!reads,"Legacy hace cero lecturas de juego");desktop=1;ingame=0;DENY("Frontend no adapta tooltip");ingame=1;busy=1;DENY("Transición de modo conserva fallback");busy=0;atomic_store(&g_load_up,1);DENY("Carga conserva fallback");atomic_store(&g_load_up,0);
 g_pc_pointer_hover.x++;DENY("XY físico antiguo no habilita tooltip");g_pc_pointer_hover.x--;g_pc_pointer_hover.epoch++;DENY("Época antigua no habilita tooltip");g_pc_pointer_hover.epoch--;g_pc_pointer_hover.generation++;DENY("Generación antigua no habilita tooltip");g_pc_pointer_hover.generation--;g_pc_options_open.pointer=0;DENY("Teclado sin puntero vigente conserva política nativa");g_pc_options_open.pointer=1;
 CHECK(pc_item_tooltip_context(&m,r,w,pc_tooltip_read),"Lector de producción acepta fixture actual protegido por MachVM");
 m.base[0x2d128ec]=1;DENY("Native ec activo conserva política original");m.base[0x2d128ec]=0;
 cancel_address=(uint64_t)inventory+0x40;cancel_root=(uint64_t*)(m.base+0x2f05458);cancel_hover=(uint64_t*)(fm+0x178);cancel_x=(int32_t*)(m.base+0x2d128f8);cancel_held=(uint64_t*)(inventory+0x40);
 for(int mode=1;mode<=10;mode++)for(int gate=0;gate<2;gate++){
  cancel_mode=mode;m.base[0x2d128f0]=gate?0:1;CHECK(!(gate?A3():A2()),"Cambio durante lecturas cancela Bank");atomic_store(&g_pc_tabbar_epoch,7);atomic_store(&g_ui_gen,12);*cancel_root=r;*cancel_hover=w;*cancel_x=0;*cancel_held=0;busy=0;atomic_store(&g_load_up,0);ingame=1;g_pc_options_open.pointer=1;
 }
 const uint64_t required[]={w,w+0x5c4,w+0x30,(uint64_t)panel+8,(uint64_t)root+0xd0,(uint64_t)fm+0x178,(uint64_t)inventory+0x40,base+0x2d60380};for(unsigned i=0;i<sizeof required/sizeof*required;i++){denied=required[i];DENY("Lectura esencial denegada conserva fallback");}denied=0;
 Q(grid,0,base+DM_VT_SLOT);DENY("Bank sólo adapta Grid nativo, no otro Slot");Q(grid,0,base+DM_VT_SLOT_B);*(uint32_t*)(grid+0x5c4)=UINT32_MAX;DENY("Owner centinela no se consulta");*(uint32_t*)(grid+0x5c4)=1;
 int pipefd[2];CHECK(pipe(pipefd)==0,"Pipe captura stderr de callbacks reales");int olderr=dup(2);dup2(pipefd[1],2);close(pipefd[1]);fcntl(pipefd[0],F_SETFL,O_NONBLOCK);unsetenv("DM_MAC_POINTER_LOG");g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;
 for(unsigned i=2;i<sizeof cases/sizeof*cases;i++){
  strcpy(panel_name,cases[i].name);*(uint32_t*)(grid+0x5c4)=cases[i].id;*(uint32_t*)(grid+0x5c8)=cases[i].type;
  uint64_t a2regs[31],saved[31];for(unsigned k=0;k<31;k++)a2regs[k]=0x22200+k;a2regs[8]=base+0x3b1d28;a2regs[9]=1;a2regs[20]=w;memcpy(saved,a2regs,sizeof saved);m.base[0x2d128f0]=1;dm_ui_pc_item_tooltip(a2regs);int intact=!a2regs[9];for(unsigned k=0;k<31;k++)if(k!=9&&a2regs[k]!=saved[k])intact=0;CHECK(intact,"Callback A2 Bank/Shared/Vendor real sólo modifica W9 local");
  uint64_t a3regs[31];memcpy(a3regs,regs,sizeof regs);memcpy(saved,a3regs,sizeof saved);m.base[0x2d128f0]=0;dm_ui_pc_grid_hover(a3regs);intact=a3regs[8]==1;for(unsigned k=0;k<31;k++)if(k!=8&&a3regs[k]!=saved[k])intact=0;CHECK(intact,"Callback A3 Bank/Shared/Vendor real sólo decide en X8 local");
 }
 fflush(stderr);dup2(olderr,2);close(olderr);char byte;errno=0;ssize_t got=read(pipefd[0],&byte,1);close(pipefd[0]);CHECK(got==0||(got<0&&errno==EAGAIN),"Callbacks funcionales sin env producen cero bytes diagnósticos");
 strcpy(panel_name,"BankExpansionLayout");*(uint32_t*)(grid+0x5c4)=2;*(uint32_t*)(grid+0x5c8)=0;struct timespec begin,end;clock_gettime(CLOCK_MONOTONIC,&begin);int accepted=0;for(int i=0;i<1000;i++)accepted+=pc_item_tooltip_context(&m,r,w,pc_tooltip_read);clock_gettime(CLOCK_MONOTONIC,&end);CHECK(accepted==1000,"Mil contextos Shared con lector real permanecen válidos");printf("Contexto Shared MachVM %.3f microsegundos/media (informativo)\n",((end.tv_sec-begin.tv_sec)*1e9+end.tv_nsec-begin.tv_nsec)/1e6);
 CHECK(!m.base[0x2d128f0]&&!m.base[0x2d128ec]&&!*(uint64_t*)(fm+0x190)&&*(uint64_t*)(fm+0x178)==w,"No hay escrituras de entrada ni foco");
 free(m.base);printf("PC TOOLTIP DOMAINS %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
