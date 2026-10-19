// Production queue: each transition keeps its coordinates across multiple producers.
#include <stdlib.h>
static int pointer_oom;
static void *pointer_test_malloc(size_t n){return pointer_oom?NULL:malloc(n);}
#define malloc pointer_test_malloc
#define DM_PC_ITEMS_CONNECTED 0
#include "../dm_cursor.c"
#undef malloc
void dm_custom_pointer_input(void){}
static int checks,failures;
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static void *producer(void *opaque){int id=(int)(intptr_t)opaque;for(int i=0;i<500;i++)dm_ui_ptr_at(DM_UP_LDOWN,(float)(id*100+i)/1000,.25f,id*1000+i);return NULL;}
static int cancel_calls,cancel_kind[3],cancel_arg[3];
static void cancelled(int kind,int arg,void *context){(void)context;if(cancel_calls<3){cancel_kind[cancel_calls]=kind;cancel_arg[cancel_calls]=arg;}cancel_calls++;}
static int route_x,route_y,route_sets,route_hovers,route_moves,route_buttons,route_bad_xy;
static void route_set(int x,int y){route_sets++;route_x=x;route_y=y;}
static void route_message(uint64_t root,void *opaque){(void)root;uint64_t *msg=opaque;uint8_t *p=(void*)(uintptr_t)msg[2];int x,y;memcpy(&x,p+4,4);memcpy(&y,p+8,4);if(msg[1]==ui_hash("MouseMove"))route_moves++;else{route_buttons++;route_bad_xy+=x!=route_x||y!=route_y;}}
static int route_native_move(void *opaque){uint8_t *p=opaque;int x,y;memcpy(&x,p+4,4);memcpy(&y,p+8,4);if(x!=route_x||y!=route_y){route_set(x,y);route_hovers++;uint64_t msg[3]={ui_hash("InputMessage"),ui_hash("MouseMove"),(uintptr_t)p};route_message(123,msg);}return 0;}
static void test_native_mouse_route(void){
 ui_mouse_emit(123,ui_hash("MouseMove"),100,200,0,route_native_move,route_message,route_set);
 CHECK(route_sets==1&&route_hovers==1&&route_moves==1&&!route_buttons,"MouseMove PC cambia XY dentro del original y despacha hover una sola vez");
 ui_mouse_emit(123,ui_hash("MouseMove"),100,200,0,route_native_move,route_message,route_set);
 CHECK(route_sets==1&&route_hovers==1&&route_moves==1,"Coordenadas iguales no fabrican un segundo hover o MouseMove");
 ui_mouse_emit(123,ui_hash("LeftMouseButtonDown"),300,400,0,route_native_move,route_message,route_set);
 ui_mouse_emit(123,ui_hash("LeftMouseButtonUp"),500,600,0,route_native_move,route_message,route_set);
 CHECK(route_sets==3&&route_hovers==3&&route_moves==3&&route_buttons==2&&!route_bad_xy,"DOWN y UP rápidos actualizan hover con su XY propio antes del botón");
 ui_mouse_emit(123,ui_hash("RightMouseButtonDown"),500,600,0,route_native_move,route_message,route_set);
 CHECK(route_hovers==3&&route_moves==3&&route_buttons==3,"Botón sobre hover vigente no duplica MouseMove");
 ui_mouse_emit(123,ui_hash("MouseMove"),700,800,0,NULL,route_message,route_set);
 CHECK(route_sets==4&&route_hovers==3&&route_moves==4,"Ruta legacy conserva setter directo y mensaje original");
 FILE *f=fopen(getenv("DM_TEST_MAIN")?getenv("DM_TEST_MAIN"):"build/main.orig.bin","rb");CHECK(f!=NULL,"Imagen original disponible para comprobar ABI MouseMove");if(f){fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);uint8_t *b=malloc(n);CHECK(b&&fread(b,1,n,f)==n,"Fixture ABI completo");fclose(f);if(b){module_t m={.base=b,.size=n};CHECK(pc_mouse_move_abi(&m),"MouseMove/setter/hover coinciden con prólogos originales");uint8_t root[0x100]={0},fm[0x200]={0},blocking[0x80]={0},confirmation[0x80]={0};uint64_t children[2]={(uintptr_t)blocking,(uintptr_t)confirmation};
memcpy(root+0xd0,&(uint64_t){(uintptr_t)fm},8);memcpy(root+0x58,&(uint64_t){(uintptr_t)children},8);memcpy(root+0x60,&(uint64_t){2},8);memcpy(blocking+8,&(uint64_t){(uintptr_t)"BlockingPanel"},8);memcpy(confirmation+8,&(uint64_t){(uintptr_t)"ConfirmationModal"},8);memcpy(b+0x2f05458,&(uint64_t){(uintptr_t)root},8);root[0xb9]=1;memcpy(b+0x211da38,&(int){3024},4);memcpy(b+0x211da3c,&(int){1700},4);
for(int f0=0;f0<2;f0++)for(int ec=0;ec<2;ec++){b[0x2d128f0]=f0;b[0x2d128ec]=ec;int native=pc_mouse_move_context(&m,(uintptr_t)root,314,687);CHECK(native==(f0&&!ec),"Solo flags elegibles pueden usar callback que entrega MouseMove");int moves=route_moves,hovers=route_hovers;route_x=route_y=0;ui_mouse_emit(123,ui_hash("MouseMove"),314,687,0,native?route_native_move:NULL,route_message,route_set);CHECK(route_moves==moves+1&&route_hovers==hovers+native&&route_x==314&&route_y==687,"Cada combinación entrega un solo MouseMove, sin perder XY ni duplicar hover");}
b[0x2d128f0]=1;b[0x2d128ec]=0;memcpy(b+0x2d128f8,&(int){314},4);memcpy(b+0x2d128fc,&(int){687},4);CHECK(!pc_mouse_move_context(&m,(uintptr_t)root,314,687),"XY ya escrito por otro productor conserva MouseMove directo en vez de callback vacío");int equal_moves=route_moves;ui_mouse_emit(123,ui_hash("MouseMove"),314,687,0,pc_mouse_move_context(&m,(uintptr_t)root,314,687)?route_native_move:NULL,route_message,route_set);CHECK(route_moves==equal_moves+1,"Hover estacionario entrega exactamente un mensaje raíz");memcpy(b+0x2d128f8,&(int){0},4);memcpy(b+0x2d128fc,&(int){0},4);blocking[0x51]=1;CHECK(!pc_mouse_move_context(&m,(uintptr_t)root,314,687),"BlockingPanel sin ConfirmationModal usa entrega directa");confirmation[0x51]=1;CHECK(pc_mouse_move_context(&m,(uintptr_t)root,314,687),"ConfirmationModal visible permite hover original");blocking[0x51]=0;
CHECK(!pc_mouse_move_context(&m,(uintptr_t)root,100,-118),"Franja superior 16:10 preserva Y UI negativa sin clamp nativo");int before_moves=route_moves,before_buttons=route_buttons;ui_mouse_emit(123,ui_hash("MouseMove"),100,-118,0,NULL,route_message,route_set);ui_mouse_emit(123,ui_hash("LeftMouseButtonDown"),100,-118,0,NULL,route_message,route_set);ui_mouse_emit(123,ui_hash("LeftMouseButtonUp"),100,-118,0,NULL,route_message,route_set);CHECK(route_moves==before_moves+1&&route_buttons==before_buttons+2&&route_y==-118&&!route_bad_xy,"Hover DOWN y UP en franja negativa conservan XY exacto hasta mensaje raíz");CHECK(!pc_mouse_move_context(&m,(uintptr_t)root,3024,100)&&!pc_mouse_move_context(&m,(uintptr_t)root,100,1700),"Límites derecho e inferior conservan XY exacto mediante ruta directa");root[0xb9]=0;memcpy(b+0x2080c08,&(int){800},4);memcpy(b+0x2080c0c,&(int){600},4);CHECK(!pc_mouse_move_context(&m,(uintptr_t)root,314,687),"Modo clásico no clampa botón UI687 a599");CHECK(pc_mouse_move_context(&m,(uintptr_t)root,314,599),"Interior clásico admite hover nativo");
b[0xb189e0]^=1;CHECK(!pc_mouse_move_abi(&m),"ABI distinta nunca se invoca como handler original");free(b);}}
}
int main(void){
 test_native_mouse_route();
 ui_pointer_event e;
 dm_ui_ptr_at_mods(DM_UP_LDOWN,.8f,-.05f,17,0x1000);dm_ui_ptr_at_mods(DM_UP_MOVE,.4f,.3f,0,0x2000);dm_ui_ptr_at_mods(DM_UP_LUP,.2f,.6f,19,0x4000);
 CHECK(ui_pointer_pop(&e)&&e.kind==DM_UP_LDOWN&&e.arg==17&&e.modifiers==0x1000,"DOWN conserva Shift sin contaminar argumento nativo");
 float first_xy[2];memcpy(first_xy,&e.precise_xy,8);CHECK(first_xy[0]==.8f&&first_xy[1]==-.05f,"DOWN conserva XY precisa junto al modificador de ese evento");
 pc_options_event_scope scope=pc_options_event_begin(&e);CHECK(g_pc_options_event_scope.modifiers==0x1000,"Guest publica sólo los modificadores de la entrada actual");g_pc_options_event_scope=scope;
 CHECK(ui_pointer_pop(&e)&&e.kind==DM_UP_LUP&&e.arg==19&&e.modifiers==0x4000,"UP conserva Alt propio aunque MOVE anterior publique Ctrl");CHECK(!ui_pointer_pop(&e)&&e.modifiers==0x2000,"Hover sin FIFO conserva modificadores de su MOVE físico");
 dm_ui_ptr_at(DM_UP_LDOWN,.8f,.4f,7);dm_ui_ptr(DM_UP_MOVE,.2f,.6f,0);dm_ui_ptr_at(DM_UP_LUP,.2f,.6f,9);
 CHECK(ui_pointer_pop(&e)&&e.kind==DM_UP_LDOWN&&e.arg==7&&e.xy==ui_pointer_xy(.8f,.4f),"DOWN rápido conserva origen aunque MOVE/UP ya estén publicados");
 CHECK(ui_pointer_pop(&e)&&e.kind==DM_UP_LUP&&e.arg==9&&e.xy==ui_pointer_xy(.2f,.6f),"UP conserva destino propio");CHECK(!ui_pointer_pop(&e),"Cada evento se consume una vez");
 dm_ui_ptr(DM_UP_MOVE,.7f,.3f,0);dm_ui_ptr(DM_UP_LUP,0,0,0);dm_ui_ptr(DM_UP_MOVE,.1f,.9f,0);
 CHECK(ui_pointer_pop(&e)&&e.xy==ui_pointer_xy(.7f,.3f),"Contrato legado de coordenadas previas se captura al encolar");
 dm_ui_ptr_at(DM_UP_HOVER,.1234567f,-.06951111f,0);CHECK(!ui_pointer_pop(&e)&&e.precise,"Hover PC conserva posición exacta sin depender de cola pendiente");float position[2];memcpy(position,&e.precise_xy,sizeof position);CHECK(position[0]==.1234567f&&position[1]==-.06951111f,"Franja superior 16:10 conserva Y UI negativa y precisión float");
 dm_ui_ptr_at(DM_UP_LUP,.7f,-.06951111f,0);ui_pointer_pop(&e);memcpy(position,&e.precise_xy,sizeof position);CHECK(e.precise&&position[1]<0,"UP PC no se clampa a fila superior de interfaz");
 pthread_t threads[2];pthread_create(&threads[0],NULL,producer,(void*)1);pthread_create(&threads[1],NULL,producer,(void*)2);pthread_join(threads[0],NULL);pthread_join(threads[1],NULL);
 int seen[2][500]={{0}},count=0;while(ui_pointer_pop(&e)){int id=e.arg/1000-1,index=e.arg%1000;CHECK(id>=0&&id<2&&index>=0&&index<500,"Payload completo pertenece al productor original");if(id>=0&&id<2&&index>=0&&index<500){seen[id][index]++;CHECK(e.xy==ui_pointer_xy((float)((id+1)*100+index)/1000,.25f),"Kind, argumento y XY no se mezclan entre productores");}count++;}
 CHECK(count==1000,"MPSC conserva las 1000 entradas sin publicar antes del payload");for(int p=0;p<2;p++)for(int i=0;i<500;i++)CHECK(seen[p][i]==1,"Cada entrada MPSC llega exactamente una vez");
 for(int i=0;i<1000;i++)dm_ui_ptr_at(DM_UP_END,-1,-1,0);
 dm_ui_ptr_at(DM_UP_LUP,.4f,.5f,17);count=0;int end=0,up=0;while(ui_pointer_pop(&e)){count++;end+=e.kind==DM_UP_END;up+=e.kind==DM_UP_LUP&&e.arg==17;}
 CHECK(count==2&&end==1&&up==1,"Mil END contiguos no generan backlog ni descartan UP");
 for(int i=0;i<600;i++)dm_ui_ptr_at(i%2?DM_UP_LUP:DM_UP_LDOWN,.4f,.5f,i);
 count=0;while(ui_pointer_pop(&e)){CHECK(e.arg==count&&e.kind==(count%2?DM_UP_LUP:DM_UP_LDOWN),"FIFO crece sin perder orden de transiciones críticas");count++;}
 CHECK(count==600,"La cola conserva más de256 transiciones sin descartar liberaciones");
 for(int i=0;i<256;i++)dm_ui_ptr_at(i==0?DM_UP_LDOWN:i==1?DM_UP_KEYDOWN:DM_UP_SCROLL,.4f,.5f,i==1?65:i);
 pointer_oom=1;dm_ui_ptr_at(DM_UP_LUP,.4f,.5f,0);pointer_oom=0;
 count=0;while(ui_pointer_pop(&e)){if(e.kind==UI_POINTER_CANCEL){CHECK(count==256,"OOM mantiene prefijo FIFO antes de cancelar");ui_pointer_cancel_held(cancelled,NULL);}else{ui_pointer_track(e.kind,e.arg);count++;}}
 CHECK(cancel_calls==2&&cancel_kind[0]==DM_UP_LUP&&cancel_kind[1]==DM_UP_KEYUP&&cancel_arg[1]==65,"OOM libera solo botones y teclas efectivamente entregados");
 ui_pointer_cancel_held(cancelled,NULL);CHECK(cancel_calls==2&&!g_up_hold&&!g_up_extra&&g_up_capacity==256,"Cancelación es idempotente y libera buffer de overflow");
 printf("UI POINTER QUEUE %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
