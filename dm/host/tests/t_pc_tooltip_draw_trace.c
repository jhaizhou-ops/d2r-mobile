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
 CHECK(m.base[0x2d128f0]==1&&!*(uint64_t*)(fm+0x190)&&*(uint64_t*)(slot+0x560)==12,"Diagnóstico no cambia flags, foco ni texto");
 Q(inv,0x40,item);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.held_valid&&s.held_id==3,"Objeto sostenido se lee sin llamar getter");Q(inv,0x40,0);
 value[8]=1;CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.model==1,"Booleano de supresión se distingue de ausente");Q(model,8,0);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.model==-1,"Modelo ausente queda desconocido");Q(model,8,&bucket);value[8]=0;
 Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&!s.id_valid&&s.slot==-1,"Grid registra texto pero no inventa ID del hover sin resolver coordenadas");Q(slot,0,(uint64_t)m.base+DM_VT_SLOT);
 Q(slot,0x558,1);CHECK(pc_tooltip_capture(1,1,&m,read_fixture,&s)&&s.first==-1,"Texto no legible no se desreferencia ni se sustituye");Q(slot,0x558,text);
 Q(slot,0,1);CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Clase desconocida impide leer campos específicos");Q(slot,0,(uint64_t)m.base+DM_VT_SLOT);
 Q(root,0x60,1);CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Widget fuera de árbol no se trata como hover vivo");Q(root,0x60,2);
 Q(root,0x60,257);CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Árbol malformado se descarta");Q(root,0x60,2);
 slot[0x51]=0;CHECK(!pc_tooltip_capture(1,1,&m,read_fixture,&s),"Widget oculto no gasta presupuesto de tooltip");slot[0x51]=1;
 uint8_t byte;CHECK(!pc_tooltip_read(1,&byte,1),"Lector de producción protege direcciones indefinidas");
 unsetenv("DM_MAC_POINTER_LOG");pc_tooltip_trace_tick((void*)1,0);CHECK(1,"Sin env la llamada real no accede a módulo inválido");setenv("DM_MAC_POINTER_LOG","",1);pc_tooltip_trace_tick((void*)1,0);CHECK(1,"Env vacío tampoco accede al módulo");
 pc_tooltip_trace_submit(0,10);CHECK(!g_pc_tooltip_submits,"OBS21 desactivado no cuenta ni lee juego");pc_tooltip_trace_submit(1,11);CHECK(g_pc_tooltip_submits==1&&g_pc_tooltip_submit_frame==11,"Contador OBS registra solicitudes y frame sin texto");
 unsigned logs=0,repeats=0;uint64_t last=0;for(int i=0;i<1000;i++)pc_tooltip_trace_take(&logs,&last,&repeats,123);CHECK(logs==3,"Hover estable produce sólo tres muestras, no spam por frame");
 CHECK(pc_tooltip_trace_take(&logs,&last,&repeats,124)&&logs==4,"Cambiar objeto/estado permite muestra nueva");for(int i=0;i<1000;i++)pc_tooltip_trace_take(&logs,&last,&repeats,200+i);CHECK(logs==48,"Límite total mantiene acotados incluso cambios continuos");
 Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);Q(root,0x60,2);Q(fm,0x178,slot);slot[0x50]=slot[0x51]=1;Q(slot,0x558,text);Q(slot,0x560,167);
 *(uint32_t*)(m.base+0x3b1d24)=0x94006ad6;*(uint32_t*)(m.base+0x3cc974)=0xd0014a29;*(uint32_t*)(m.base+0x3cc978)=0x3963c129;
 uint64_t regs[31],saved[31];for(int i=0;i<31;i++)regs[i]=0x111000+i;regs[8]=(uint64_t)m.base+0x3b1d28;regs[20]=(uint64_t)slot;regs[9]=1;memcpy(saved,regs,sizeof regs);
 reads=0;CHECK(!pc_item_tooltip_draw_trace_capture(0,1,(void*)1,regs,read_fixture,&s)&&!reads,"Draw sin opt-in no accede ni al módulo inválido");
 CHECK(!pc_item_tooltip_draw_trace_capture(1,0,(void*)1,regs,read_fixture,&s)&&!reads,"Draw legacy no accede al módulo");
 CHECK(pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s)&&s.hover==(uint64_t)slot&&s.text_length==167,"DrawLR captura el widget real con texto preparado");
 m.base[0x2d128f0]=0;CHECK(pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s)&&!s.f0&&regs[9]==1,"Entrada W9 original y globalF0 se distinguen sin mutación");
 CHECK(!memcmp(saved,regs,sizeof regs),"Snapshot conserva los31 registros originales");
 regs[9]=0;CHECK(pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s),"f0 cero también llega al diagnóstico antes de earlyreturn A2");regs[9]=1;
 regs[8]--;reads=0;CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s)&&!reads,"Caller ajeno no lee memoria");regs[8]++;
 regs[20]++;CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s),"Widget distinto del hover se descarta");regs[20]--;
 Q(slot,0,(uint64_t)m.base+DM_VT_SLOT);CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s),"Equipo no hereda diagnóstico GridDraw");Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);
 m.base[0x3b1d24]^=1;CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s),"BL GridDraw mutado falla cerrado");m.base[0x3b1d24]^=1;
 m.base[0x3cc978]^=1;CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s),"Instrucción lecturaF0 mutada falla cerrada");m.base[0x3cc978]^=1;
 CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,NULL,read_fixture,&s),"Regs NULL no se desreferencia");
 Q(fm,0x178,1);CHECK(!pc_item_tooltip_draw_trace_capture(1,1,&m,regs,read_fixture,&s),"Hover ilegible se rechaza con lector protegido");Q(fm,0x178,slot);
 pc_tooltip_draw_trace_state irrelevant={0};regs[20]++;for(int i=0;i<12;i++)pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&irrelevant,&s);regs[20]--;
 CHECK(!irrelevant.attempts,"Draws sin hover del widget no consumen capturas completas");
 CHECK(pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&irrelevant,&s),"Doce draws de otro widget conservan la primera muestra de hover real");
 pc_tooltip_draw_trace_state nulls={0};Q(slot,0x560,0);CHECK(pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&nulls,&s)&&!s.text_length&&!s.selected,"Texto/selectedNULL siguen observables en el hover válido");Q(slot,0x560,167);
 pc_tooltip_draw_trace_state invalid={0};Q(m.base,0x2f05458,1);CHECK(!pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&invalid,&s)&&!invalid.attempts,"Root ilegible no consume presupuesto del futuro hover");Q(m.base,0x2f05458,root);
 pc_tooltip_draw_trace_state state={0};reads=0;unsigned accepted=0;for(int i=0;i<1000;i++)accepted+=pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&state,&s);
 CHECK(state.attempts==12&&accepted==3&&state.count==3,"Draw estable hace máximo12 capturas y3 muestras");int capped=reads;CHECK(!pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&state,&s)&&reads==capped,"Tras presupuesto no hay más lecturas protegidas");
 state=(pc_tooltip_draw_trace_state){0};accepted=0;for(int i=0;i<1000;i++){regs[9]=i&1;accepted+=pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&state,&s);}CHECK(accepted==12&&state.attempts==12,"Estados alternos siguen máximo12 capturas/logs");
 state=(pc_tooltip_draw_trace_state){0};reads=0;regs[8]--;CHECK(!pc_item_tooltip_draw_trace_collect(1,1,&m,regs,read_fixture,&state,&s)&&!state.attempts&&!reads,"Callers ajenos no consumen presupuesto");regs[8]++;
 CHECK(!pc_item_tooltip_draw_trace_collect(0,1,(void*)1,regs,read_fixture,&state,&s)&&!state.attempts&&!reads,"Collect desactivado no lee módulo ni gasta presupuesto");
 pc_tooltip_a2_raw_state rawstate={0};pc_tooltip_a2_raw raw;reads=0;
 CHECK(!pc_item_tooltip_a2_raw_collect(0,1,(void*)1,regs,read_fixture,&rawstate,&raw)&&!reads,"RAWA2 sin opt-in no toca memoria ni contexto inválido");
 CHECK(!pc_item_tooltip_a2_raw_collect(1,0,(void*)1,regs,read_fixture,&rawstate,&raw)&&!reads,"RAWA2 legacy no accede a contexto");
 CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&raw.snapshot&&raw.kind==0&&raw.group==0&&!strcmp(raw.reason,"snapshot_ok"),"RAWDraw registra entrada antes del filtro diagnóstico con razón positiva");
 CHECK(raw.entry_f0==regs[9]&&!raw.global_f0&&raw.data.text_length==167&&raw.data.held_valid,"RAW usa registros originales y lector corregido del jugador local");
 rawstate=(pc_tooltip_a2_raw_state){0};regs[8]=(uint64_t)m.base+0x3b0910;unsigned rawlogs=0;
 for(int i=0;i<1000;i++)rawlogs+=pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw);
 CHECK(rawlogs==2&&rawstate.total==2,"RAWA2 Update repetido queda en dos muestras por grupo/clase/hover");
 regs[8]=(uint64_t)m.base+0x3b1d28;CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&raw.group==0,"Update no consume presupuesto Draw");
 rawstate=(pc_tooltip_a2_raw_state){0};Q(fm,0x178,1);rawlogs=0;for(int i=0;i<1000;i++)rawlogs+=pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw);
 CHECK(rawlogs==2&&!strcmp(raw.reason,"hover_diff")&&!raw.same,"RAW conserva razón de hover distinto y queda acotado");Q(fm,0x178,slot);
 CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&raw.same&&raw.snapshot,"Hover ajeno no consume las muestras de hover útil");
 rawstate=(pc_tooltip_a2_raw_state){0};m.base[0x3b1d24]^=1;CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&!strcmp(raw.reason,"draw_bl")&&!raw.snapshot,"RAWA2 informa BL distinto en lugar de silenciarlo");m.base[0x3b1d24]^=1;
 rawstate=(pc_tooltip_a2_raw_state){0};Q(root,0x60,257);CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&!strcmp(raw.reason,"snapshot_rejected")&&!raw.snapshot,"RAWA2 distingue rechazo profundo de ausencia de entrada");Q(root,0x60,2);
 rawstate=(pc_tooltip_a2_raw_state){0};m.base[0x3cc978]^=1;CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&!strcmp(raw.reason,"f0_bytes"),"RAWA2 registra rechazo de bytesF0");m.base[0x3cc978]^=1;
 rawstate=(pc_tooltip_a2_raw_state){0};slot[0x50]=0;CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&!strcmp(raw.reason,"widget_flags"),"RAWA2 registra flags invisibles sin habilitar widget");slot[0x50]=1;
 rawstate=(pc_tooltip_a2_raw_state){0};regs[20]=1;CHECK(pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&!strcmp(raw.reason,"widget_class"),"Self ilegible sigue siendo observable con lector seguro");regs[20]=(uint64_t)slot;
 rawstate=(pc_tooltip_a2_raw_state){0};rawlogs=0;uint64_t rawcallers[]={0x3b1d28,0x3b0910,0x3b0b68,0x3b0fa4,0x3d4fe4,0x123},rawtypes[]={DM_VT_SLOT_B,DM_VT_SLOT,0x123};
 for(unsigned i=0;i<6;i++)for(unsigned j=0;j<3;j++)for(unsigned k=0;k<2;k++){regs[8]=(uint64_t)m.base+rawcallers[i];Q(slot,0,(uint64_t)m.base+rawtypes[j]);Q(fm,0x178,k?(uint64_t)slot:1);for(unsigned n=0;n<5;n++)rawlogs+=pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw);}
 CHECK(rawlogs==72&&rawstate.total==72,"RAWA2 cubre caller/clase/hover sin superar72 muestras aun repitiendo cada combinación");regs[8]=(uint64_t)m.base+0x3b1d28;Q(slot,0,(uint64_t)m.base+DM_VT_SLOT_B);Q(fm,0x178,slot);
 rawstate=(pc_tooltip_a2_raw_state){0};rawstate.total=72;reads=0;CHECK(!pc_item_tooltip_a2_raw_collect(1,1,&m,regs,read_fixture,&rawstate,&raw)&&!reads,"RAWA2 agotado no realiza lecturas adicionales");
 CHECK(regs[9]==saved[9]&&regs[20]==saved[20]&&!*(uint64_t*)(fm+0x190)&&*(uint64_t*)(slot+0x560)==167,"RAWA2 no cambia entrada, selección ni texto");
 int pipefd[2];CHECK(pipe(pipefd)==0,"Pipe de prueba captura stderr real");int olderr=dup(2);dup2(pipefd[1],2);close(pipefd[1]);fcntl(pipefd[0],F_SETFL,O_NONBLOCK);
 unsetenv("DM_MAC_POINTER_LOG");pc_item_tooltip_draw_trace((void*)1);pc_item_tooltip_a2_raw_trace((void*)1);setenv("DM_MAC_POINTER_LOG","",1);pc_item_tooltip_draw_trace((void*)1);pc_item_tooltip_a2_raw_trace((void*)1);fflush(stderr);dup2(olderr,2);close(olderr);char logbyte;errno=0;ssize_t got=read(pipefd[0],&logbyte,1);close(pipefd[0]);CHECK(got==0|| (got<0&&errno==EAGAIN),"Callback diagnóstico sin env o vacío no emite bytes ni toca regs inválidos");
 unsetenv("DM_MAC_POINTER_LOG");CHECK(!*(uint64_t*)(fm+0x190)&&*(uint64_t*)(slot+0x560)==167,"Diagnóstico no selecciona ni altera texto");
 free(m.base);printf("PC TOOLTIP DRAW TRACE %d comprobaciones, %d fallos\n",checks,failures);return !!failures;
}
