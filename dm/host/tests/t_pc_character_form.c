// Presentación PC: sólo la clase admitida por el callback original muestra Upsell.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int desktop=1,ingame,busy,checks,failures,calls,change,change_set;
int dm_ui_pc_desktop(void){return desktop;}int dm_uiscreen_ingame(void){return ingame;}int dm_padmode_busy(void){return busy;}
#define CHECK(c,t)do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
#define Q(p,o,v) (*(uint64_t*)((uint8_t*)(p)+(o))=(uint64_t)(v))
static int read_field(uint64_t p,void*out,size_t n){memcpy(out,(void*)p,n);if(change){change=0;atomic_fetch_add(&g_ui_gen,1);}return 1;}
static int unavailable(uint64_t p,void*out,size_t n){(void)p;(void)out;(void)n;return 0;}
static int toggle(module_t*m,uint64_t w,int on,int slot){(void)m;calls++;CHECK(slot==0x50||slot==0x58,"Usa setters originales de activo y visible");*(uint8_t*)(w+(slot==0x50?0x50:0x51))=on;if(change_set){change_set=0;atomic_fetch_add(&g_ui_gen,1);}return 1;}
int main(void){
 module_t m={.size=0x2f14300};m.base=calloc(1,m.size);if(!m.base)return 2;
 uint8_t root[0x200]={0},panel[0x200]={0},description[0x100]={0},button[0x100]={0},node[0x60]={0};uint64_t children[]={(uintptr_t)panel},rows[]={(uintptr_t)description,(uintptr_t)button};
 Q(m.base,0x2f05458,root);Q(root,0x58,children);Q(root,0x60,1);root[0xd8]=1;Q(panel,0,(uint64_t)m.base+0x20c6e40);Q(panel,8,"CharacterCreatePanel");Q(panel,0x58,rows);Q(panel,0x60,2);panel[0x50]=panel[0x51]=1;Q(description,8,"UpsellDescription");Q(button,8,"UpsellButton");
 Q(m.base,0x2f14240,"Paladin");Q(m.base,0x2f14248,7);Q(m.base,0x2f142c0,node);Q(node,0x20,"Paladin");Q(node,0x28,7);node[0x38]=3;atomic_store(&g_ui_gen,10);
#define REFRESH() pc_create_refresh(&m,(uintptr_t)root,desktop,read_field,toggle)
 description[0x50]=description[0x51]=button[0x50]=button[0x51]=1;
 CHECK(atomic_load(&g_load_up)==-1&&pc_create_context(&m,(uintptr_t)root,desktop,(uintptr_t)panel),"Frontend frío conserva el estado inicial −1 y permite presentación");
 CHECK(REFRESH()&&calls==4&&!description[0x50]&&!button[0x51],"Paladín oculta ambos avisos y deja el combo fuera de esta política");
 CHECK(REFRESH()&&calls==4,"Estado repetido no repite setters ni callbacks");
 atomic_store(&g_load_up,0);CHECK(REFRESH()&&calls==4,"Retorno con carga 0 también permite presentación sin repetir setters");
 const char*names[]={"Amazon","Sorceress","Necromancer","Paladin","Barbarian","Druid","Assassin","Warlock"};
 for(int c=0;c<8;c++){Q(m.base,0x2f14240,names[c]);Q(m.base,0x2f14248,strlen(names[c]));Q(node,0x20,names[c]);Q(node,0x28,strlen(names[c]));node[0x38]=c;CHECK(pc_create_class(&m,read_field)==c,"Lee el enum de clase del mapa nativo, no una licencia supuesta");CHECK(REFRESH()&&button[0x50]==(c==7)&&description[0x51]==(c==7),"Sólo Brujo conserva visible y activo su callback original");}
 CHECK(REFRESH()&&button[0x50],"Brujo permanece accesible sin forzar estado por frame");Q(m.base,0x2f14248,0);CHECK(REFRESH()&&!button[0x50],"Sin selección oculta aviso comercial");
 Q(m.base,0x2f14248,7);int before=calls;desktop=0;CHECK(!REFRESH()&&calls==before,"Fuera de PC no cambia presentación original");desktop=1;
 ingame=1;CHECK(!REFRESH()&&calls==before,"Partida activa no ejecuta presentación frontend");ingame=0;root[0xd9]=1;CHECK(!REFRESH(),"Transición ToGame no cambia controles");root[0xd9]=0;busy=1;CHECK(!REFRESH(),"Cambio de modo espera");busy=0;atomic_store(&g_load_up,1);CHECK(!REFRESH(),"Carga no cambia controles");atomic_store(&g_load_up,0);
 CHECK(!pc_create_refresh(&m,1,1,read_field,toggle),"Root ajeno se rechaza antes de recorrerlo");
 Q(m.base,0x2f05458,0);CHECK(!REFRESH(),"Root anterior no se reutiliza");Q(m.base,0x2f05458,root);Q(panel,0,0);CHECK(!REFRESH(),"Clase de panel distinta no recibe setter");Q(panel,0,(uint64_t)m.base+0x20c6e40);panel[0x50]=0;CHECK(!REFRESH(),"Panel oculto no recibe setter");panel[0x50]=1;
 CHECK(!pc_create_refresh(&m,(uintptr_t)root,1,unavailable,toggle)&&calls==before,"Lectura indefinida no inventa clase ni licencia");change=1;CHECK(!REFRESH()&&calls==before,"Cambio de generación durante lectura cancela antes de mutar UI");
 before=calls;change_set=1;CHECK(!REFRESH()&&calls==before+1,"Cambio de generación en setter detiene el segundo setter y el otro widget");CHECK(REFRESH()&&button[0x50]&&button[0x51],"Nueva generación revalida y completa la presentación vigente");
 uint8_t left[0x60]={0},right[0x60]={0};Q(left,0x20,"Amazon");Q(left,0x28,6);left[0x38]=0;Q(right,0x20,"Warlock");Q(right,0x28,7);right[0x38]=7;Q(node,0x20,"Paladin");Q(node,0x28,7);node[0x38]=3;Q(node,8,left);Q(node,0x10,right);
 Q(m.base,0x2f14240,"Amazon");Q(m.base,0x2f14248,6);CHECK(pc_create_class(&m,read_field)==0,"Búsqueda nativa recorre hijo izquierdo por clave");Q(m.base,0x2f14240,"Warlock");Q(m.base,0x2f14248,7);CHECK(pc_create_class(&m,read_field)==7,"Búsqueda nativa recorre hijo derecho por clave");Q(node,0x20,"Warlock");Q(node,0x28,7);Q(node,8,0);Q(node,0x10,0);
 node[0x38]=8;CHECK(pc_create_class(&m,read_field)==-2&&!REFRESH(),"Enum desconocido no se convierte en disponible ni oculto");node[0x38]=7;Q(node,0x10,node);Q(m.base,0x2f14240,"ZZ");Q(m.base,0x2f14248,2);CHECK(pc_create_class(&m,read_field)==-2,"Ciclo en mapa termina sin seguir punteros indefinidos");
 free(m.base);printf("PC CHARACTER FORM %d comprobaciones, %d fallos\n",checks,failures);return!!failures;
}
