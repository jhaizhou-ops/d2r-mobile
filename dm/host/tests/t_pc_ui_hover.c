// Physical PC hover in root fallback: native leave, fresh hit and cancellable capture.
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,checks,failures,clears,actions;
int dm_ui_pc_desktop(void){return desktop;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static uint8_t root[0x200],manager[0x200],container[0x600],a[0x600],b[0x600];
static uint64_t ptr(void *p){return(uint64_t)(uintptr_t)p;}
static void focus(uint64_t pair,uint64_t target,uint64_t widget){CHECK(pair==ptr(manager)+0x168&&!target&&!widget,"API original recibe par hover y ambos destinos nulos");clears++;*(uint64_t*)(manager+0x170)=target;*(uint64_t*)(manager+0x178)=widget;}
static void set_xy(int x,int y){(void)x;(void)y;}
static void message(uint64_t r,void *opaque){CHECK(r==ptr(root),"El mensaje conserva raíz actual");uint64_t *msg=opaque;int *p=(void*)(uintptr_t)msg[2];if(msg[1]==ui_hash("MouseMove")){if(!*(uint64_t*)(manager+0x178)&&p[2]>=0)*(uint64_t*)(manager+0x178)=ptr(p[1]<150?a:b);}else if(msg[1]==ui_hash("LeftMouseButtonDown")&&*(uint64_t*)(manager+0x178)==ptr(b))actions++;}
static void selected(void){*(uint64_t*)(manager+0x170)=ptr(container);*(uint64_t*)(manager+0x178)=ptr(a);*(uint64_t*)(manager+0x190)=ptr(b);}
int main(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;
 uint64_t roots[]={ptr(manager),ptr(container)},rows[]={ptr(a),ptr(b)};*(uint64_t*)(root+0x58)=ptr(roots);*(uint64_t*)(root+0x60)=2;*(uint64_t*)(root+0xd0)=ptr(manager);*(uint64_t*)(container+0x58)=ptr(rows);*(uint64_t*)(container+0x60)=2;*(uint64_t*)manager=(uintptr_t)m.base+0x20b7f88;*(uint64_t*)(m.base+0x2f05458)=ptr(root);
 const uint32_t code[]={0xf81c0ff7,0xa90157f6,0xa9024ff4,0xa9037bfd};memcpy(m.base+0xabe064,code,sizeof code);selected();
 ui_mouse_emit(ptr(root),ui_hash("MouseMove"),200,300,0,NULL,message,set_xy);ui_mouse_emit(ptr(root),ui_hash("LeftMouseButtonDown"),200,300,0,NULL,message,set_xy);CHECK(!actions&&*(uint64_t*)(manager+0x178)==ptr(a),"Fallback anterior deja hover ajeno y no activa la fila bajo el clic");
 CHECK(pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),200,300,0,focus)&&clears==1,"Primer movimiento fallback sale del hover anterior por API nativa");ui_mouse_emit(ptr(root),ui_hash("MouseMove"),200,300,0,NULL,message,set_xy);ui_mouse_emit(ptr(root),ui_hash("LeftMouseButtonDown"),200,300,0,NULL,message,set_xy);CHECK(actions==1&&*(uint64_t*)(manager+0x178)==ptr(b),"Hit raíz nuevo entrega clic a su fila física una sola vez");CHECK(*(uint64_t*)(manager+0x190)==ptr(b),"Refresh de hover no escribe selección del teclado");
 for(int i=0;i<120;i++)CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),200,300,0,focus),"Hover estacionario conserva estado original");CHECK(clears==1,"No hay borrado de foco en cada frame");
 CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("LeftMouseButtonDown"),201,300,0,focus)&&clears==1,"Botón no fabrica otro movimiento ni entrega otra acción");CHECK(pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),100,300,0,focus)&&clears==2,"Cambio físico posterior invalida hover de la fila anterior");
 selected();CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),101,300,1,focus)&&clears==2,"Callback nativo elegible conserva su propio reset y evita leave duplicado");selected();desktop=0;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),102,300,0,focus)&&clears==2,"Legacy no entra en política PC");desktop=1;
 CHECK(pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),102,300,0,focus)&&clears==3,"Volver a PC requiere hit físico fresco");selected();atomic_fetch_add(&g_ui_gen,1);CHECK(pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),102,300,0,focus)&&clears==4,"Nueva generación de paneles invalida hover incluso con XY iguales");
 selected();g_pc_options_event_scope=(pc_options_event_scope){.epoch=atomic_load(&g_pc_tabbar_epoch),.active=1};atomic_fetch_add(&g_pc_tabbar_epoch,1);CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),103,300,0,focus)&&clears==4,"Evento anterior a pérdida de foco no altera hover");g_pc_options_event_scope=(pc_options_event_scope){0};CHECK(pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),102,300,0,focus)&&clears==5,"Nuevo input tras pérdida de foco invalida cache de época anterior");
 selected();CHECK(pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),100,-118,0,focus),"Franja negativa conserva salida del hover superior sin clamp");ui_mouse_emit(ptr(root),ui_hash("MouseMove"),100,-118,0,NULL,message,set_xy);CHECK(!*(uint64_t*)(manager+0x178),"Fuera del área de widgets no se inventa hover sobre primera fila");
 selected();CHECK(!pc_pointer_hover_refresh(&m,1,ui_hash("MouseMove"),104,300,0,focus),"Root obsoleto se rechaza antes de leerlo");*(uint64_t*)(root+0xd0)=1;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),105,300,0,focus),"Manager retirado no se desreferencia");*(uint64_t*)(root+0xd0)=ptr(manager);
 *(uint64_t*)(manager+0x178)=1;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),106,300,0,focus),"Hover retirado no invoca virtuales de objeto destruido");selected();*(uint64_t*)(manager+0x170)=1;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),107,300,0,focus),"Contenedor retirado no invoca callbacks nativos");selected();*(uint64_t*)manager+=8;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),108,300,0,focus),"Clase distinta no se usa como FocusManager");*(uint64_t*)manager-=8;
 m.base[0xabe064]^=1;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),109,300,0,focus),"Cambio de ABI impide llamada original");m.base[0xabe064]^=1;CHECK(!pc_pointer_hover_refresh(&m,ptr(root),ui_hash("MouseMove"),110,300,0,NULL),"Callback nulo no se invoca");
 free(m.base);printf("PC UI HOVER %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
