// Diagnóstico opt-in de tooltip: sólo lecturas acotadas, sin app ni juego.
#include "../dm_cursor.c"
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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
 Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);Q(slot,0x30,root);root[0x50]=root[0x51]=slot[0x50]=slot[0x51]=1;
 Q(m.base,DM_VT_SLOT_B+0x20,(uint64_t)m.base+0x3b1cb4);uint32_t tail[]={0xa90167fa,0xa9025ff8,0xa90357f6};memcpy(m.base+0x3b1cb8,tail,sizeof tail);
 atomic_store(&g_ui_gen,12);atomic_store(&g_pc_tabbar_epoch,7);g_pc_options_open.pointer=1;g_pc_options_open.input_epoch=7;
 g_pc_pointer_hover=(pc_pointer_hover_state){.root=(uint64_t)root,.x=100,.y=200,.valid=1,.generation=12,.epoch=7};*(int32_t*)(m.base+0x2d128f8)=100;*(int32_t*)(m.base+0x2d128fc)=200;
 CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s),"Hover Grid válido para armar watch");pc_grid_draw_watch_update(&m,100,read_fixture,&s);CHECK(g_pc_grid_draw_watch.valid,"Watch exige puntero físico vigente");
 uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x12000+i;regs[0]=(uint64_t)slot;memcpy(saved,regs,sizeof regs);pc_grid_draw_entry e;
#define ENTRY() pc_grid_draw_entry_capture(1,1,&m,regs,read_fixture,&g_pc_grid_draw_watch,101,&e)
 reads=0;CHECK(!pc_grid_draw_entry_capture(0,1,(void*)1,regs,read_fixture,&g_pc_grid_draw_watch,101,&e)&&!reads,"Sin flag no lee módulo inválido");CHECK(!pc_grid_draw_entry_capture(1,0,(void*)1,regs,read_fixture,&g_pc_grid_draw_watch,101,&e)&&!reads,"Legacy no lee estado");
 Q(fm,0x178,0);Q(fm,0x170,0);Q(fm,0x190,0);CHECK(ENTRY()&&!e.hover&&!e.selected,"Draw conserva la señal hoverNULL posterior al watch");
 CHECK(e.owner_unit==(uint64_t)player&&e.player==(uint64_t)player&&e.local_index==7&&e.local_id==7,"Owner y localplayer usan lookup por IDs original");CHECK(e.ancestors==2&&e.chain_complete&&e.draw==(uint64_t)m.base+0x3b1cb4,"Cadena y métodoDraw se capturan");CHECK(!memcmp(saved,regs,sizeof regs),"Se conservan31registros originales");
 root[0x51]=0;CHECK(ENTRY()&&!e.chain[1].flags[1],"Ancestro oculto se informa sin silenciar Draw");root[0x51]=1;
 Q(m.base,0x2d603a0+8*7,0);CHECK(ENTRY()&&!e.owner_unit&&!e.player,"LookupNULL anterior al earlyexit sigue observable");Q(m.base,0x2d603a0+8*7,player);
 *(uint32_t*)(m.base+0x2d20e4c)=8;CHECK(ENTRY()&&e.local_index==8&&e.local_id==UINT32_MAX&&!e.player,"Índice local inválido se informa sin salirse de tabla");*(uint32_t*)(m.base+0x2d20e4c)=7;
 g_pc_pointer_hover.x++;reads=0;CHECK(!ENTRY()&&!reads,"CambioXY invalida antes de lecturas");g_pc_pointer_hover.x--;
 g_pc_pointer_hover.epoch++;CHECK(!ENTRY(),"Cambioepoch invalida watch");g_pc_pointer_hover.epoch--;
 atomic_fetch_add(&g_ui_gen,1);CHECK(!ENTRY(),"Cambio generación invalida watch");atomic_fetch_sub(&g_ui_gen,1);
 g_pc_options_open.pointer=0;CHECK(!ENTRY(),"Sin modo físico no se usa watch previo");g_pc_options_open.pointer=1;
 CHECK(!pc_grid_draw_entry_capture(1,1,&m,regs,read_fixture,&g_pc_grid_draw_watch,161,&e)&&!pc_grid_draw_entry_capture(1,1,&m,regs,read_fixture,&g_pc_grid_draw_watch,99,&e),"Watch exige ventana60frames sin underflow");
 Q(m.base,0x2f05458,1);CHECK(!ENTRY(),"Root cambiado no se desreferencia");Q(m.base,0x2f05458,root);
 m.base[0x3b1cb8]^=1;CHECK(!ENTRY(),"Prólogo cambiado falla cerrado");m.base[0x3b1cb8]^=1;
 Q(slot,0,1);CHECK(!ENTRY(),"Clase desconocida no interpreta Grid");Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);
 Q(root,0x30,slot);CHECK(ENTRY()&&e.ancestors==16&&!e.chain_complete,"Ciclo ancestral se limita16");Q(root,0x30,0);
 pc_tooltip_draw_trace_state state={0};regs[0]++;reads=0;for(int i=0;i<1000;i++)pc_grid_draw_entry_collect(1,1,&m,regs,read_fixture,&g_pc_grid_draw_watch,101,&state,&e);CHECK(!state.attempts&&!reads,"Otros widgets no consumen capturas ni lecturas");regs[0]--;
 unsigned accepted=0;for(int i=0;i<1000;i++)accepted+=pc_grid_draw_entry_collect(1,1,&m,regs,read_fixture,&g_pc_grid_draw_watch,101,&state,&e);CHECK(state.attempts==12&&accepted==3,"Doce capturas y tres muestras estables");int capped=reads;CHECK(!pc_grid_draw_entry_collect(1,1,&m,regs,read_fixture,&g_pc_grid_draw_watch,101,&state,&e)&&reads==capped,"Presupuesto agotado evita lecturas");
 int pipefd[2];CHECK(pipe(pipefd)==0,"Pipe captura stderr del callback real");int olderr=dup(2);dup2(pipefd[1],2);close(pipefd[1]);fcntl(pipefd[0],F_SETFL,O_NONBLOCK);
 unsetenv("DM_MAC_POINTER_LOG");dm_ui_pc_grid_draw_obs((void*)1);setenv("DM_MAC_POINTER_LOG","",1);dm_ui_pc_grid_draw_obs((void*)1);fflush(stderr);dup2(olderr,2);close(olderr);char output[8192];errno=0;ssize_t got=read(pipefd[0],output,sizeof output-1);close(pipefd[0]);CHECK(got==0||(got<0&&errno==EAGAIN),"Callback real sin env/vacío no emite bytes ni toca regs inválidos");
 g_mod[0]=m;strcpy(g_mod[0].name,"main");g_nmod=1;frame=101;uint8_t *before=malloc(m.size);if(!before)return 2;memcpy(before,m.base,m.size);uint8_t root_before[sizeof root],fm_before[sizeof fm],slot_before[sizeof slot];memcpy(root_before,root,sizeof root);memcpy(fm_before,fm,sizeof fm);memcpy(slot_before,slot,sizeof slot);
 CHECK(pipe(pipefd)==0,"Pipe captura diagnóstico opt-in real");olderr=dup(2);dup2(pipefd[1],2);close(pipefd[1]);fcntl(pipefd[0],F_SETFL,O_NONBLOCK);setenv("DM_MAC_POINTER_LOG","fixture",1);for(int i=0;i<1000;i++)dm_ui_pc_grid_draw_obs(regs);fflush(stderr);dup2(olderr,2);close(olderr);got=read(pipefd[0],output,sizeof output-1);close(pipefd[0]);unsetenv("DM_MAC_POINTER_LOG");unsigned rows=0;if(got>0){output[got]=0;for(char *p=output;(p=strstr(p,"PC_GRID_DRAW_ENTRY"));p++)rows++;}CHECK(rows==3,"Callback real limita mil Draw a tres líneas estables");CHECK(got>0&&strstr(output,"hover=0 selected=0")&&strstr(output,"owner=7/0")&&strstr(output,"chainComplete=1"),"Callback real informa hoverNULL, owner y cadena sin veto");
 CHECK(!memcmp(saved,regs,sizeof regs)&&!memcmp(before,m.base,m.size)&&!memcmp(root_before,root,sizeof root)&&!memcmp(fm_before,fm,sizeof fm)&&!memcmp(slot_before,slot,sizeof slot),"Callback real preserva registros y memoria observada");free(before);
 free(m.base);printf("PC GRID DRAW ENTRY %d comprobaciones, %d fallos\n",checks,failures);return !!failures;
}
