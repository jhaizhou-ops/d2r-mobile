// NpcDialog PC: el clic de fila conserva la activación original por sus hijos.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;static int desktop=1,ingame=1,busy,checks,failures;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static uint64_t ptr(void *p){return(uint64_t)(uintptr_t)p;}
int main(void){
 FILE *f=fopen("build/main.orig.bin","rb");if(!f)f=fopen("dm/host/tests/build/main.orig.bin","rb");if(!f)return 2;fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);uint8_t *bytes=malloc(n);if(!bytes||fread(bytes,1,n,f)!=n)return 2;fclose(f);
 module_t m={.base=bytes,.size=n};uint64_t base=ptr(bytes);uint8_t root[0x200]={0},panel[0x300]={0},manager[0x200]={0},list[0x600]={0},row[0x600]={0},other[0x600]={0},payload[0x40]={0};uint64_t top[]={ptr(manager),ptr(panel)},panelchildren[]={ptr(list),ptr(row)},rows[]={ptr(row)},callback[]={ui_hash("DialogMessage"),ui_hash("ListItemClicked")},message[]={ui_hash("InputMessage"),ui_hash("LeftMouseButtonDown"),ptr(payload)};
 *(uint64_t*)(bytes+0x2f05458)=ptr(root);*(uint64_t*)(root+0x58)=ptr(top);*(uint64_t*)(root+0x60)=2;*(uint64_t*)(root+0xd0)=ptr(manager);root[0xd9]=1;*(uint64_t*)manager=base+0x20b7f88;*(uint64_t*)(manager+0x178)=ptr(row);
 *(uint64_t*)panel=base+0x208a2b0;panel[0x50]=panel[0x51]=1;*(uint64_t*)(panel+0x58)=ptr(panelchildren);*(uint64_t*)(panel+0x60)=1;*(uint64_t*)(panel+0x248)=ptr(list);*(uint64_t*)(list+0x58)=ptr(rows);*(uint64_t*)(list+0x60)=1;list[0x50]=list[0x51]=1;
 *(uint64_t*)row=base+0x20b84d0;row[0x50]=row[0x51]=1;*(uint64_t*)(row+0x598)=ptr(list);*(uint64_t*)(row+0x568)=ptr(callback);atomic_store(&g_load_up,0);
 #define PASS() pc_npc_click_guard(&m,ptr(root),ptr(panel),message,1,1)
 CHECK(pc_npc_click_abi(&m),"La imagen original conserva ABI de panel, hit, hijos y fila");CHECK(PASS(),"LMB interior con fila/callback nativos permite bucle original de hijos");
 CHECK(!pc_npc_click_guard(&m,ptr(root),ptr(panel),message,0,1),"Clic exterior conserva cancelación original");CHECK(!pc_npc_click_guard(&m,ptr(root),ptr(panel),message,1,0),"Legacy conserva consumo de mando");
 const char *events[]={"RightMouseButtonDown","LeftMouseButtonUp","MouseMove","ControllerActionBegin"};for(unsigned i=0;i<sizeof events/sizeof *events;i++){message[1]=ui_hash(events[i]);CHECK(!PASS(),"RMB/UP/move/controlador no entra a activación LMB");}message[1]=ui_hash("LeftMouseButtonDown");
 *(uint64_t*)(manager+0x178)=0;CHECK(!PASS(),"Fondo sin fila mantiene consumo modal");*(uint64_t*)(manager+0x178)=ptr(other);CHECK(!PASS(),"Hover fuera del panel no se desreferencia");*(uint64_t*)(manager+0x178)=ptr(row);
 *(uint64_t*)(row+0x568)=0;CHECK(!PASS(),"Fila sin acción no propaga el clic detrás del diálogo");*(uint64_t*)(row+0x568)=ptr(callback);callback[1]=ui_hash("OtherAction");CHECK(!PASS(),"Otro protocolo de callback no se adapta");callback[1]=ui_hash("ListItemClicked");
 *(uint64_t*)(row+0x598)=ptr(other);CHECK(!PASS(),"Fila de otra lista no activa el servicio");*(uint64_t*)(row+0x598)=ptr(list);panelchildren[0]=ptr(row);CHECK(!PASS(),"Lista retirada del árbol falla antes de leerla");panelchildren[0]=ptr(list);
 *(uint64_t*)(panel+0x60)=2;*(uint64_t*)(list+0x60)=0;CHECK(!PASS(),"Fila reparentada junto a su lista con598 obsoleto no activa servicios");*(uint64_t*)(list+0x60)=1;*(uint64_t*)(panel+0x60)=1;
 for(int i=0;i<2;i++){panel[0x50+i]=0;CHECK(!PASS(),"Panel oculto/inactivo conserva ruta original");panel[0x50+i]=1;row[0x50+i]=0;CHECK(!PASS(),"Fila oculta/inactiva no se activa");row[0x50+i]=1;}
 *(uint64_t*)row+=8;CHECK(!PASS(),"Clase de fila incompatible se rechaza");*(uint64_t*)row-=8;*(uint64_t*)panel+=8;CHECK(!PASS(),"Clase de panel incompatible se rechaza");*(uint64_t*)panel-=8;
 root[0xb8]=1;CHECK(!PASS(),"Consumo previo se conserva");root[0xb8]=0;CHECK(!pc_npc_click_guard(&m,1,ptr(panel),message,1,1),"Raíz obsoleta se rechaza antes de leerla");*(uint64_t*)(root+0xd0)=1;CHECK(!PASS(),"Manager retirado no se desreferencia");*(uint64_t*)(root+0xd0)=ptr(manager);
 busy=1;CHECK(!PASS(),"Cambio de modo impide una nueva acción");busy=0;ingame=0;CHECK(!PASS(),"Frontend no recibe el adaptador de diálogo de partida");ingame=1;atomic_store(&g_load_up,1);CHECK(!PASS(),"Carga no permite activar filas");atomic_store(&g_load_up,0);
 g_pc_options_event_scope=(pc_options_event_scope){.active=1,.epoch=atomic_load(&g_pc_tabbar_epoch)};atomic_fetch_add(&g_pc_tabbar_epoch,1);CHECK(!PASS(),"Evento previo a blur no activa el diálogo");g_pc_options_event_scope=(pc_options_event_scope){0};
 const uint32_t sites[]={0x3dadc8,0x3db0a8,0x3db198,0xaca6e8};for(unsigned i=0;i<sizeof sites/sizeof *sites;i++){bytes[sites[i]]^=1;CHECK(!PASS(),"Cambio de versión falla cerrado");bytes[sites[i]]^=1;}
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x200+i;regs[0]=1;regs[19]=ptr(panel);regs[20]=ptr(message);memcpy(saved,regs,sizeof regs);dm_ui_pc_npc_click(regs);CHECK(regs[8]==1,"Puerta pública usa panel/mensaje/hit originales");for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==saved[i],"Puerta conserva los demás registros");
 free(bytes);printf("PC NPC CLICK %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
