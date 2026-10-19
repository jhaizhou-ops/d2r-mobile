// La cola de mando no debe soltar automáticamente el objeto bajo un panel PC independiente.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;static int desktop=1,ingame=1,busy,checks,failures;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static uint64_t ptr(void *p){return(uint64_t)(uintptr_t)p;}
int main(void){
 FILE *f=fopen("build/main.orig.bin","rb");if(!f)f=fopen("dm/host/tests/build/main.orig.bin","rb");if(!f)return 2;fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);uint8_t *bytes=malloc(n);if(!bytes||fread(bytes,1,n,f)!=n)return 2;fclose(f);
 module_t m={.base=bytes,.size=n,.text_end=0x1b80000};uint8_t root[0x200]={0},panel[0x100]={0},grid[0x700]={0},hud[0x100]={0},belt[0x700]={0},inventory[0x100]={0},held[0x100]={0};uint64_t top[]={ptr(hud),ptr(panel)},items[]={ptr(grid)},hud_items[]={ptr(belt)};
 *(uint64_t*)(bytes+0x2f05458)=ptr(root);*(uint64_t*)(root+0x58)=ptr(top);*(uint64_t*)(root+0x60)=2;root[0xd9]=1;*(uint64_t*)(hud+8)=ptr("HUDPanel");*(uint64_t*)(hud+0x58)=ptr(hud_items);*(uint64_t*)(hud+0x60)=1;*(uint64_t*)(panel+0x58)=ptr(items);*(uint64_t*)(panel+0x60)=1;
 panel[0x50]=panel[0x51]=hud[0x50]=hud[0x51]=grid[0x50]=grid[0x51]=belt[0x50]=belt[0x51]=1;
 uint64_t base=ptr(bytes),vt=base+0x2089ab8,belt_vt=base+0x2088fc8;*(uint64_t*)grid=vt;*(uint64_t*)belt=belt_vt;*(uint64_t*)(belt_vt+0xf8)=base+0x3925a0;
 *(uint32_t*)inventory=0x1020304;*(uint64_t*)(inventory+0x40)=ptr(held);*(uint32_t*)held=4;*(uint32_t*)(held+8)=3;atomic_store(&g_load_up,0);
 CHECK(pc_item_hold_abi(&m),"La imagen fijada conserva prólogo y vecinos originales de la cola");
 const uint32_t families[]={0x3b3a64,0x3d710c,0x3925a0,0x3afdd4,0x39db7c};
 for(unsigned i=0;i<sizeof families/sizeof *families;i++){*(uint64_t*)(vt+0xf8)=base+families[i];CHECK(pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Grid/equipo/belt/tarea/pila bajo panel visible conservan held sin UISwitcher");}
 CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),0),"Legacy conserva la reconciliación nativa");panel[0x50]=0;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Sin panel de objetos, el cinturón HUD no inventa una protección global");panel[0x50]=1;
 for(int i=0;i<2;i++){panel[0x50+i]=0;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Panel oculto/inactivo no protege referencias anteriores");panel[0x50+i]=1;}
 *(uint64_t*)(vt+0xf8)=base+0x1000;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Otra clase visible no pasa por coincidencia de nombre");*(uint64_t*)(vt+0xf8)=base+0x3b3a64;
 CHECK(!pc_item_hold_guard(&m,1,base+0x2d5b740,ptr(inventory),ptr(held),1),"Raíz anterior falla antes de desreferenciarla");CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b748,ptr(inventory),ptr(held),1),"Sólo la instancia mainqueue original entra al puente");
 *(uint64_t*)(inventory+0x40)=0;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Acuse que vació la mano no deja una protección obsoleta");*(uint64_t*)(inventory+0x40)=ptr(held);busy=1;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Cambio de modo conserva guardas originales");busy=0;ingame=0;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Frontend no modifica reconciliación invitada");ingame=1;
 for(unsigned i=0;i<2;i++){size_t at=i?0x3b864c:0x3b855c;bytes[at]^=1;CHECK(!pc_item_hold_guard(&m,ptr(root),base+0x2d5b740,ptr(inventory),ptr(held),1),"Otra versión de la cola falla cerrada");bytes[at]^=1;}
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x100+i;regs[8]=ptr(held);regs[19]=base+0x2d5b740;regs[23]=ptr(inventory);memcpy(saved,regs,sizeof regs);dm_ui_pc_item_hold(regs);CHECK(regs[8]==1,"Gate usa inventario/objeto validados por el llamador original");for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==saved[i],"Gate conserva los demás registros");
 free(bytes);printf("PC ITEM HOLD %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
