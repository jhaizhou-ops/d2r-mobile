// Gate A3: conservar sólo hover físico Grid en la limpieza original de FocusManager.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame=1,busy,checks,failures,reads;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
uint64_t dm_guest_frame(void){return 42;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
static int cancel_read;static uint64_t *root_field,*hover_field;static int32_t *xy_field;
static int read_cancel(uint64_t p,void *out,size_t n){int mode=cancel_read;cancel_read=0;if(mode==1)atomic_fetch_add(&g_pc_tabbar_epoch,1);if(mode==2)atomic_fetch_add(&g_ui_gen,1);if(mode==3)*root_field=0;if(mode==4)*hover_field=0;if(mode==5)(*xy_field)++;if(mode==6)busy=1;if(mode==7)atomic_store(&g_load_up,1);if(mode==8)ingame=0;memcpy(out,(void*)p,n);return 1;}
static int read_unknown(uint64_t p,void *out,size_t n){(void)p;(void)out;(void)n;return 0;}
static int read_fixture(uint64_t p,void *out,size_t n){reads++;if(p<4096)return 0;memcpy(out,(void*)p,n);return 1;}
int main(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;
 FILE *f=fopen("build/main.orig.bin","rb");if(!f)f=fopen("dm/host/tests/build/main.orig.bin","rb");if(!f)return 2;CHECK(fread(m.base,1,m.size,f)==m.size,"ABI de fixture procede de la imagen original");fclose(f);
 uint8_t root[0x200]={0},fm[0x200]={0},panel[0x200]={0},widget[0x700]={0},player[0x200]={0},inv[0x100]={0},other[0x200]={0};
 uint64_t children[]={(uint64_t)fm,(uint64_t)panel},leaves[]={(uint64_t)widget};
 uint64_t r=(uint64_t)root,w=(uint64_t)widget,base=(uint64_t)m.base;
 Q(m.base,0x2f05458,root);Q(root,0x58,children);Q(root,0x60,2);Q(root,0xd0,fm);root[0xd9]=1;
 Q(fm,0,base+0x20b7f88);Q(fm,0x178,widget);Q(panel,8,"PlayerInventoryExpansionLayout");Q(panel,0x58,leaves);Q(panel,0x60,1);panel[0x50]=panel[0x51]=1;widget[0x50]=widget[0x51]=1;
 Q(widget,0,base+DM_VT_SLOT);*(uint32_t*)(player+8)=7;Q(player,0x90,inv);*(uint32_t*)inv=0x01020304;Q(m.base,0x2d603a0+8*7,player);*(uint32_t*)(m.base+0x2d20e4c)=7;m.base[0x2d128f0]=1;
 atomic_store(&g_load_up,0);atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);g_pc_options_open.pointer=1;g_pc_options_open.input_epoch=7;g_pc_pointer_hover=(pc_pointer_hover_state){.root=r,.valid=1,.generation=12,.epoch=7};
 Q(widget,0,base+DM_VT_SLOT_B);*(uint32_t*)(widget+0x5c4)=7;Q(widget,0x30,panel);Q(panel,0x30,root);root[0x50]=root[0x51]=1;Q(fm,0x170,panel);m.base[0x2d128f0]=0;
 *(uint32_t*)(m.base+0x2d60380+7*4)=7;uint64_t regs[31],before[31];for(unsigned i=0;i<31;i++)regs[i]=0x1000+i;regs[0]=w;regs[19]=(uint64_t)fm;regs[20]=(uint64_t)panel;memcpy(before,regs,sizeof regs);
#define ROUTE() pc_grid_hover_route(&m,regs,desktop,read_fixture)
 CHECK(pc_grid_hover_abi(&m),"ABI original FocusManager.Update, guardas, clear y destino");CHECK(ROUTE(),"Hover físico Grid f0=0 conserva container/hover antes de Draw");
 *(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=1;*(uint32_t*)(player+8)=1;Q(m.base,0x2d603a0+8,player);*(uint32_t*)(widget+0x5c4)=1;CHECK(ROUTE(),"Índice local0→ID1 real se resuelve sin doblar lookup");
 Q(inv,0x40,other);CHECK(!ROUTE(),"Arrastre conserva limpieza original");Q(inv,0x40,0);
 *(uint32_t*)(m.base+0x2d60380)=UINT32_MAX;CHECK(!ROUTE(),"Mapping local ausente no preserva hover");*(uint32_t*)(m.base+0x2d60380)=1;
 *(uint32_t*)(m.base+0x2d20e4c)=8;CHECK(!ROUTE(),"Índice fuera0..7 falla cerrado");*(uint32_t*)(m.base+0x2d20e4c)=0;
 *(uint32_t*)(widget+0x5c4)=2;CHECK(!ROUTE(),"Owner ajeno al jugador local no preserva Grid");*(uint32_t*)(widget+0x5c4)=1;*(uint32_t*)(widget+0x5c8)=1;CHECK(!ROUTE(),"Owner de otro tipo no preserva Grid");*(uint32_t*)(widget+0x5c8)=0;
 Q(fm,0x170,0);regs[20]=0;CHECK(ROUTE(),"ContainerNULL con hover vivo conserva exactamente ambos campos");Q(fm,0x170,panel);regs[20]=(uint64_t)panel;
 reads=0;desktop=0;CHECK(!ROUTE()&&!reads,"Legacy ni consulta memoria");desktop=1;CHECK(!pc_grid_hover_route(&m,NULL,1,read_fixture),"RegistroNULL se rechaza");CHECK(!pc_grid_hover_route(NULL,regs,1,read_fixture),"MóduloNULL se rechaza");CHECK(!pc_grid_hover_route(&m,regs,1,read_unknown),"Lector denegado falla cerrado");
 m.base[0x2d128f0]=1;CHECK(!ROUTE(),"f0=1 usa política original, no privilegio global");m.base[0x2d128f0]=0;m.base[0x2d128ec]=1;CHECK(!ROUTE(),"ec no cero conserva clear nativo");m.base[0x2d128ec]=0;
 for(unsigned i=0;i<3;i++){uint8_t *node=((uint8_t*[]){widget,panel,root})[i];for(unsigned j=0;j<2;j++){node[0x50+j]=0;CHECK(!ROUTE(),"Ancestro oculto/inactivo conserva Leave original");node[0x50+j]=1;}}
 Q(panel,0x30,widget);CHECK(!ROUTE(),"Ciclo ancestral no preserva hover");Q(panel,0x30,root);Q(panel,0x30,0);CHECK(!ROUTE(),"Cadena que no alcanza root se rechaza");Q(panel,0x30,root);
 regs[0]++;CHECK(!ROUTE(),"x0 distinto del hover original se rechaza");regs[0]--;regs[19]++;CHECK(!ROUTE(),"x19 distinto del manager original se rechaza");regs[19]--;regs[20]++;CHECK(!ROUTE(),"x20 distinto del container original se rechaza");regs[20]--;
 Q(fm,0x178,0);CHECK(!ROUTE(),"HoverNULL no se inventa");Q(fm,0x178,widget);Q(widget,0,base+DM_VT_SLOT);CHECK(!ROUTE(),"Equipo no hereda preservación Grid");Q(widget,0,base+DM_VT_SLOT_B);
 ingame=0;CHECK(!ROUTE(),"Frontend conserva clear");ingame=1;busy=1;CHECK(!ROUTE(),"Transición de modo conserva clear");busy=0;atomic_store(&g_load_up,1);CHECK(!ROUTE(),"Carga conserva clear");atomic_store(&g_load_up,0);
 g_pc_options_open.pointer=0;CHECK(!ROUTE(),"Teclado sin puntero físico mantiene política nativa");g_pc_options_open.pointer=1;g_pc_options_open.input_epoch++;CHECK(!ROUTE(),"Blur invalida contexto");g_pc_options_open.input_epoch--;
 g_pc_pointer_hover.valid=0;CHECK(!ROUTE(),"Puntero inválido conserva clear");g_pc_pointer_hover.valid=1;g_pc_pointer_hover.generation++;CHECK(!ROUTE(),"Generación antigua no preserva hover");g_pc_pointer_hover.generation--;g_pc_pointer_hover.epoch++;CHECK(!ROUTE(),"Época antigua no preserva hover");g_pc_pointer_hover.epoch--;g_pc_pointer_hover.x++;CHECK(!ROUTE(),"XY físico antiguo no preserva hover");g_pc_pointer_hover.x--;
 Q(panel,8,"VendorPanel");CHECK(!ROUTE(),"Panel ajeno al inventario conserva clear");Q(panel,8,"PlayerInventoryOriginalLayout");CHECK(ROUTE(),"Inventario clásico usa misma frontera");Q(panel,8,"PlayerInventoryExpansionLayout");
 uint64_t root_three[]={(uint64_t)fm,(uint64_t)panel,(uint64_t)other};Q(other,8,"ConfirmationModal");other[0x50]=other[0x51]=1;Q(other,0x58,leaves);Q(other,0x60,1);Q(root,0x58,root_three);Q(root,0x60,3);CHECK(!ROUTE(),"Modal visible conserva clear sin cerrarlo");Q(root,0x58,children);Q(root,0x60,2);
 for(unsigned i=0;i<5;i++){size_t at=((size_t[]){0xabcc50,0xabcc88,0xabcca8,0xabccd4,0xabccd8})[i];m.base[at]^=1;reads=0;CHECK(!ROUTE()&&!reads,"Bytes FocusManager alterados fallan antes de lecturas");m.base[at]^=1;}
 root_field=(uint64_t*)(m.base+0x2f05458);hover_field=(uint64_t*)(fm+0x178);xy_field=(int32_t*)(m.base+0x2d128f8);
 for(int mode=1;mode<=8;mode++){cancel_read=mode;CHECK(!pc_grid_hover_route(&m,regs,1,read_cancel),"Cambio durante lecturas cancela preservación");atomic_store(&g_pc_tabbar_epoch,7);atomic_store(&g_ui_gen,12);*root_field=r;*hover_field=w;*xy_field=0;busy=0;atomic_store(&g_load_up,0);ingame=1;}
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;dm_ui_pc_grid_hover(regs);CHECK(regs[8]==1,"Callback real autoriza ruta física vigente");for(unsigned i=0;i<31;i++)if(i!=8)CHECK(regs[i]==before[i],"Callback conserva otros treinta registros");
 desktop=0;dm_ui_pc_grid_hover(regs);CHECK(!regs[8],"Callback legacy solicita replay completo");desktop=1;dm_ui_pc_grid_hover(NULL);
 CHECK(!m.base[0x2d128f0]&&!m.base[0x2d128ec]&&!*(uint64_t*)(fm+0x190)&&*(uint64_t*)(fm+0x178)==w&&*(uint64_t*)(fm+0x170)==(uint64_t)panel,"Host no escribe f0/ec/selected/container/hover");
 free(m.base);printf("PC GRID HOVER %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
