// Gate A2: habilita sólo la consulta física del Update; el estado del juego sigue intacto.
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
 Q(widget,0x30,panel);Q(panel,0x30,root);root[0x50]=root[0x51]=1;*(uint32_t*)(widget+0x5c4)=7;Q(widget,0,base+DM_VT_SLOT);*(uint32_t*)(player+8)=7;Q(player,0x90,inv);*(uint32_t*)inv=0x01020304;Q(m.base,0x2d603a0+8*7,player);*(uint32_t*)(m.base+0x2d20e4c)=7;m.base[0x2d128f0]=1;
 atomic_store(&g_load_up,0);atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);g_pc_options_open.pointer=1;g_pc_options_open.input_epoch=7;g_pc_pointer_hover=(pc_pointer_hover_state){.root=r,.valid=1,.generation=12,.epoch=7};
 uint64_t caller=base+0x3d4fe4;
#define ROUTE() pc_item_tooltip_route(&m,r,w,caller,desktop,1,read_fixture)
 *(uint32_t*)(m.base+0x2d60380+7*4)=7;
 CHECK(pc_item_tooltip_abi(&m),"Getter, Update equipo y Update grid coinciden con cuatro palabras originales");
 CHECK(ROUTE(),"Equipo en inventario actual puede generar tooltip bajo hover físico sin seleccionar");
 *(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=1;*(uint32_t*)(player+8)=1;Q(m.base,0x2d603a0+8,player);*(uint32_t*)(widget+0x5c4)=1;
 CHECK(ROUTE(),"Índice local0 resuelve unitid1 mediante tabla original sin doblar lookup");
 Q(inv,0x40,other);CHECK(!ROUTE(),"Mapping0→1 con mano ocupada conserva veto A2");Q(inv,0x40,0);
 *(uint32_t*)(m.base+0x2d60380)=UINT32_MAX;CHECK(!ROUTE(),"Mapping sin jugador−1 no habilita A2");*(uint32_t*)(m.base+0x2d60380)=1;
 *(uint32_t*)(m.base+0x2d20e4c)=8;CHECK(!ROUTE(),"Índice8 fuera tabla no habilita A2");*(uint32_t*)(m.base+0x2d20e4c)=UINT32_MAX;CHECK(!ROUTE(),"Índice−1 no habilita A2");
 *(uint32_t*)(m.base+0x2d20e4c)=7;*(uint32_t*)(player+8)=7;*(uint32_t*)(widget+0x5c4)=7;Q(m.base,0x2d603a0+8,0);

 Q(panel,8,"PlayerInventoryOriginalLayout");CHECK(ROUTE(),"Inventario clásico conserva la misma frontera");Q(panel,8,"PlayerInventoryExpansionLayout");
 Q(widget,0,base+DM_VT_SLOT_B);uint64_t callers[]={0x3b0910,0x3b0b68,0x3b0fa4};for(unsigned i=0;i<3;i++){caller=base+callers[i];CHECK(ROUTE(),"Cada uno de los tres BL dentro del Update grid admite su hover físico");}caller=base+0x3d4fe4;CHECK(!ROUTE(),"LR de equipo no habilita clase grid");Q(widget,0,base+DM_VT_SLOT);
 Q(widget,0,base+DM_VT_SLOT_B);caller=base+0x3b1d28;CHECK(ROUTE(),"Draw grid validado usa hover físico con las mismas guardas A2");
 Q(inv,0x40,other);CHECK(!ROUTE(),"Draw con mano ocupada se rechaza");Q(inv,0x40,0);
 Q(fm,0x178,other);CHECK(!ROUTE(),"Draw sin hover propio se rechaza");Q(fm,0x178,widget);
 g_pc_pointer_hover.epoch++;CHECK(!ROUTE(),"Draw con epoch anterior se rechaza");g_pc_pointer_hover.epoch--;
 g_pc_pointer_hover.x++;CHECK(!ROUTE(),"Draw con XY anterior se rechaza");g_pc_pointer_hover.x--;
 m.base[0x3b1d24]^=1;CHECK(!ROUTE(),"Draw BL distinto falla cerrado");m.base[0x3b1d24]^=1;
 m.base[0x3b1cb8]^=1;CHECK(!ROUTE(),"Draw prólogo distinto falla cerrado");m.base[0x3b1cb8]^=1;
 caller++;CHECK(!ROUTE(),"Draw LR vecino no recibe permiso");caller--;
 Q(widget,0,base+DM_VT_SLOT);CHECK(!ROUTE(),"Draw Grid no habilita claseEquipo");Q(widget,0,base+DM_VT_SLOT_B);
Q(widget,0,base+DM_VT_SLOT);caller=base+0x3d4fe4;
 CHECK(!pc_item_tooltip_route(&m,r,w,caller,1,0,read_fixture),"f0 ya cero conserva ruta original sin intervención");desktop=0;CHECK(!ROUTE(),"Móvil y mando conservan CBNZ original");desktop=1;
 ingame=0;CHECK(!ROUTE(),"Frontend no admite consulta física de inventario");ingame=1;busy=1;CHECK(!ROUTE(),"Transición de modo no usa hover previo");busy=0;atomic_store(&g_load_up,1);CHECK(!ROUTE(),"Carga impide habilitar tooltip");atomic_store(&g_load_up,0);
 Q(m.base,0x2f05458,other);CHECK(!ROUTE(),"Root anterior no se desreferencia");Q(m.base,0x2f05458,root);Q(panel,0x60,0);CHECK(!ROUTE(),"Widget retirado pierde permiso");Q(panel,0x60,1);
 for(unsigned i=0;i<2;i++){widget[0x50+i]=0;CHECK(!ROUTE(),"Widget oculto/inactivo conserva getter original");widget[0x50+i]=1;panel[0x50+i]=0;CHECK(!ROUTE(),"Panel oculto/inactivo conserva getter original");panel[0x50+i]=1;}
 Q(panel,8,"VendorPanel");CHECK(!ROUTE(),"Tienda/alijo ajenos no heredan tratamiento de inventario");Q(panel,8,"PlayerInventoryExpansionLayout");caller=base+0x3d4fe8;CHECK(!ROUTE(),"Otro caller incluso dentro de Update queda intacto");caller=base+0x3d4fe4;
 Q(fm,0x178,other);CHECK(!ROUTE(),"Hover ajeno no reactiva selección de mando");Q(fm,0x178,widget);Q(fm,0,base+0x20b7f80);CHECK(!ROUTE(),"Clase FocusManager distinta queda intacta");Q(fm,0,base+0x20b7f88);
 g_pc_options_open.pointer=0;CHECK(!ROUTE(),"Teclado sin nuevo movimiento físico mantiene su foco nativo");g_pc_options_open.pointer=1;g_pc_options_open.input_epoch++;CHECK(!ROUTE(),"Blur invalida modo físico");g_pc_options_open.input_epoch--;
 g_pc_pointer_hover.generation++;CHECK(!ROUTE(),"Cambio de generación invalida hover");g_pc_pointer_hover.generation--;g_pc_pointer_hover.epoch++;CHECK(!ROUTE(),"Cancelación invalida hover");g_pc_pointer_hover.epoch--;g_pc_pointer_hover.root=1;CHECK(!ROUTE(),"Hover de root anterior queda fuera");g_pc_pointer_hover.root=r;g_pc_pointer_hover.valid=0;CHECK(!ROUTE(),"Sin hover publicado no se cambia rama");g_pc_pointer_hover.valid=1;g_pc_pointer_hover.x=1;CHECK(!ROUTE(),"XY antiguo frente al cursor actual no habilita tooltip");g_pc_pointer_hover.x=0;
 Q(inv,0x40,other);CHECK(!ROUTE(),"Objeto sostenido preserva todas las guardas de texto originales");Q(inv,0x40,0);*(uint32_t*)inv=0;CHECK(!ROUTE(),"Inventario no válido no se adivina");*(uint32_t*)inv=0x01020304;
 for(unsigned i=0;i<8;i++){size_t at=((size_t[]){0x3cc87c,0x3cc974,0x3d4f80,0x3b06c8,0x3d4fe0,0x3b090c,0x3b0b64,0x3b0fa0})[i];if(i>=5){Q(widget,0,base+DM_VT_SLOT_B);caller=base+callers[i-5];}m.base[at]^=1;CHECK(!ROUTE(),"ABI de función o BL cambiado falla cerrado");m.base[at]^=1;}Q(widget,0,base+DM_VT_SLOT);caller=base+0x3d4fe4;
 root_field=(uint64_t*)(m.base+0x2f05458);hover_field=(uint64_t*)(fm+0x178);xy_field=(int32_t*)(m.base+0x2d128f8);
 for(int mode=1;mode<=8;mode++){cancel_read=mode;CHECK(!pc_item_tooltip_route(&m,r,w,caller,1,1,read_cancel),"Cambio de epoch/generación/root/hover/XY/modo/carga/partida durante lectura impide habilitar tooltip");atomic_store(&g_pc_tabbar_epoch,7);atomic_store(&g_ui_gen,12);*root_field=r;*hover_field=w;*xy_field=0;busy=0;atomic_store(&g_load_up,0);ingame=1;}
 CHECK(!pc_item_tooltip_route(&m,r,w,caller,1,1,read_unknown),"Lectura de inventario indefinida conserva el getter original");
 uint64_t modal_children[]={(uint64_t)widget},root_three[]={(uint64_t)fm,(uint64_t)panel,(uint64_t)other};Q(other,8,"ConfirmationModal");other[0x50]=other[0x51]=1;Q(other,0x58,modal_children);Q(other,0x60,1);Q(root,0x58,root_three);Q(root,0x60,3);CHECK(!ROUTE(),"Confirmación visible bloquea el tratamiento sin cerrarla");Q(root,0x58,children);Q(root,0x60,2);
 CHECK(m.base[0x2d128f0]==1&&!*(uint64_t*)(fm+0x190)&&!*(uint64_t*)(inv+0x40),"No escribe modo de entrada, selected ni mano");
 uint64_t regs[31],before[31];for(unsigned i=0;i<31;i++)regs[i]=0x1000+i;regs[9]=1;regs[8]=caller;regs[20]=w;memcpy(before,regs,sizeof regs);g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;dm_ui_pc_item_tooltip(regs);CHECK(!regs[9],"Callback real permite fallthrough por decisión x9");for(unsigned i=0;i<31;i++)if(i!=9)CHECK(regs[i]==before[i],"Callback conserva todos los demás registros");
 regs[9]=1;desktop=0;dm_ui_pc_item_tooltip(regs);CHECK(regs[9]==1,"Callback no PC mantiene valor original de w9");desktop=1;regs[8]++;dm_ui_pc_item_tooltip(regs);CHECK(regs[9]==1,"Callback de caller ajeno mantiene CBNZ");dm_ui_pc_item_tooltip(NULL);
 free(m.base);printf("PC ITEM TOOLTIP %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
