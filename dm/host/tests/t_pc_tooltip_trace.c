// Diagnóstico opt-in de tooltip: sólo lecturas acotadas, sin app ni juego.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static int checks,failures,reads,desktop=1;static uint64_t frame;
int dm_ui_pc_desktop(void){return desktop;}uint64_t dm_guest_frame(void){return frame;}
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static struct {uint64_t begin;size_t size;} regions[32];static int count;
static void region(void *p,size_t n){regions[count++]=(typeof(*regions)){(uint64_t)p,n};}
static int read_fixture(uint64_t p,void *out,size_t n){reads++;for(int i=0;i<count;i++)if(p>=regions[i].begin&&n<=regions[i].size&&p-regions[i].begin<=regions[i].size-n){memcpy(out,(void*)p,n);return 1;}return 0;}
#define Q(p,o,v) (*(uint64_t*)((p)+(o))=(uint64_t)(v))
int main(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);if(!m.base)return 2;region(m.base,m.size);
 uint8_t root[0x200]={0},fm[0x200]={0},slot[0x700]={0},player[0x200]={0},inv[0x100]={0},row[0x30]={0},item[0x40]={0},model[16]={0},node[24]={0},wrapper[24]={0},holder[8]={0},value[16]={0};char text[64]="Espada larga";
 void *data[]={root,fm,slot,player,inv,row,item,model,node,wrapper,holder,value,text};size_t sizes[]={sizeof root,sizeof fm,sizeof slot,sizeof player,sizeof inv,sizeof row,sizeof item,sizeof model,sizeof node,sizeof wrapper,sizeof holder,sizeof value,sizeof text};
 for(unsigned i=0;i<sizeof data/sizeof *data;i++)region(data[i],sizes[i]);
 uint64_t children[]={(uint64_t)fm,(uint64_t)slot},bucket=(uint64_t)node,items[13]={0};region(children,sizeof children);region(&bucket,8);region(items,sizeof items);items[4]=(uint64_t)item;
 Q(m.base,0x2f05458,root);Q(root,0xd0,fm);Q(root,0x58,children);Q(root,0x60,2);Q(fm,0,(uint64_t)m.base+0x20b7f88);Q(fm,0x178,slot);Q(slot,0,(uint64_t)m.base+DM_VT_SLOT);slot[0x50]=slot[0x51]=1;
 Q(slot,0x558,text);Q(slot,0x560,12);*(uint32_t*)(slot+0x5c4)=7;*(uint32_t*)(slot+0x5d8)=4;
 *(uint32_t*)(player+8)=7;Q(player,0x90,inv);*(uint32_t*)inv=0x01020304;Q(inv,0x20,row);Q(inv,0x28,1);Q(row,0x18,items);*(uint32_t*)item=4;*(uint32_t*)(item+8)=3;
 Q(m.base,0x2d603a0+8*7,player);*(uint32_t*)(m.base+0x2d20e4c)=7;m.base[0x2d128f0]=1;
 Q(m.base,0x2f05450,model);Q(model,0,1);Q(model,8,&bucket);Q(node,8,0x825996a80be4b12eull);Q(node,16,wrapper);Q(wrapper,16,holder);Q(holder,0,value);
 *(uint32_t*)(m.base+0x2d60380+7*4)=7;
 pc_tooltip_snapshot s;reads=0;CHECK(!pc_tooltip_capture(0,1,(void*)1,read_fixture,&s)&&!reads,"Sin opt-in no lee ni el módulo ni memoria de juego");CHECK(!pc_tooltip_capture(1,0,(void*)1,read_fixture,&s)&&!reads,"Legacy no lee estado de juego");
 CHECK(!pc_tooltip_capture(1,1,NULL,read_fixture,&s)&&!reads,"Módulo indefinido no se lee");
 CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s),"Snapshot real de hover equipado se captura sin funciones guest");CHECK(s.type==DM_VT_SLOT&&s.id_valid&&s.id==3&&s.slot==4,"Identidad equipada viene de inventario existente");CHECK(s.f0==1&&!s.ec&&!s.selected&&!s.model,"Flags de entrada y booleano se registran sin corregirlos");CHECK(s.text_length==12&&s.first=='E'&&s.held_valid&&!s.held,"Longitud/primerbyte y mano vacía se conservan");
 *(uint32_t*)(m.base+0x2d20e4c)=0;*(uint32_t*)(m.base+0x2d60380)=1;*(uint32_t*)(player+8)=1;Q(m.base,0x2d603a0+8,player);
 CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.held_valid&&!s.held,"Capture resuelve índice0→unitid1 mediante memoria original");
 *(uint32_t*)(m.base+0x2d20e4c)=7;*(uint32_t*)(player+8)=7;Q(m.base,0x2d603a0+8,0);
 CHECK(m.base[0x2d128f0]==1&&!*(uint64_t*)(fm+0x190)&&*(uint64_t*)(slot+0x560)==12,"Diagnóstico no cambia flags, foco ni texto");
 Q(inv,0x40,item);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.held_valid&&s.held_id==3,"Objeto sostenido se lee sin llamar getter");Q(inv,0x40,0);
 value[8]=1;CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.model==1,"Booleano de supresión se distingue de ausente");Q(model,8,0);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.model==-1,"Modelo ausente queda desconocido");Q(model,8,&bucket);value[8]=0;
 Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&!s.id_valid&&s.slot==-1,"Grid registra texto pero no inventa ID del hover sin resolver coordenadas");Q(slot,0,(uint64_t)m.base+DM_VT_SLOT);
 Q(slot,0x558,1);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.first==-1,"Texto no legible no se desreferencia ni se sustituye");Q(slot,0x558,text);
 Q(slot,0,1);CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Clase desconocida impide leer campos específicos");Q(slot,0,(uint64_t)m.base+DM_VT_SLOT);
 Q(root,0x60,1);CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Widget fuera de árbol no se trata como hover vivo");Q(root,0x60,2);
 Q(root,0x60,257);CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Árbol malformado se descarta");Q(root,0x60,2);
 slot[0x51]=0;CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Widget oculto no gasta presupuesto de tooltip");slot[0x51]=1;
 reads=0;CHECK(!pc_tooltip_local_player(read_fixture,(uint64_t)m.base,8)&&!reads,"Índice8 falla cerrado antes de leer tabla");CHECK(!pc_tooltip_local_player(read_fixture,(uint64_t)m.base,UINT32_MAX)&&!reads,"Índice−1 no hace lookup unit");
 *(uint32_t*)(m.base+0x2d60380+7*4)=UINT32_MAX;CHECK(!pc_tooltip_local_player(read_fixture,(uint64_t)m.base,7),"Mapping−1 representa jugador inexistente");*(uint32_t*)(m.base+0x2d60380+7*4)=7;
 CHECK(!pc_tooltip_local_player(pc_tooltip_read,1,0),"Tabla ilegible no se desreferencia fuera de MachVM");
 uint8_t shared[16]={0},alternate[0x100]={0};region(shared,sizeof shared);region(alternate,sizeof alternate);*(uint32_t*)alternate=0x01020304;Q(player,0xa0,shared);Q(player,0x98,alternate);*(uint32_t*)(shared+8)=1;
 CHECK(pc_tooltip_inventory(read_fixture,(uint64_t)player)==(uint64_t)alternate,"Shared vivo usa inventario98 como original");*(uint32_t*)(shared+8)=0;CHECK(pc_tooltip_inventory(read_fixture,(uint64_t)player)==(uint64_t)inv,"Shared refs0 usa inventario90");*(uint32_t*)(shared+8)=1;Q(player,0x98,0);CHECK(pc_tooltip_inventory(read_fixture,(uint64_t)player)==(uint64_t)inv,"Shared98 NULL conserva fallback90");Q(player,0xa0,0);Q(player,0x98,0);
 uint8_t byte;CHECK(!pc_tooltip_read(1,&byte,1),"Lector de producción protege direcciones indefinidas");
 unsetenv("DM_MAC_POINTER_LOG");pc_tooltip_trace_tick((void*)1,0);CHECK(1,"Sin env la llamada real no accede a módulo inválido");setenv("DM_MAC_POINTER_LOG","",1);pc_tooltip_trace_tick((void*)1,0);CHECK(1,"Env vacío tampoco accede al módulo");
 pc_tooltip_trace_submit(0,10);CHECK(!g_pc_tooltip_submits,"OBS21 desactivado no cuenta ni lee juego");pc_tooltip_trace_submit(1,11);CHECK(g_pc_tooltip_submits==1&&g_pc_tooltip_submit_frame==11,"Contador OBS registra solicitudes y frame sin texto");
 unsigned logs=0,repeats=0;uint64_t last=0;for(int i=0;i<1000;i++)pc_tooltip_trace_take(&logs,&last,&repeats,123);CHECK(logs==3,"Hover estable produce sólo tres muestras, no spam por frame");
 CHECK(pc_tooltip_trace_take(&logs,&last,&repeats,124)&&logs==4,"Cambiar objeto/estado permite muestra nueva");for(int i=0;i<1000;i++)pc_tooltip_trace_take(&logs,&last,&repeats,200+i);CHECK(logs==48,"Límite total mantiene acotados incluso cambios continuos");
 free(m.base);printf("PC TOOLTIP TRACE %d comprobaciones, %d fallos\n",checks,failures);return !!failures;
}
