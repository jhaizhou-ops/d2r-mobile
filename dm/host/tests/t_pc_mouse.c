// Frontera PC real: proyección, objetivo y fases usan las APIs originales.
#include "../dm_world.c"
static int checks,failures,screen_calls,project_calls,classic_calls,clear_calls,target_calls,click_calls;
static uint64_t screen_input,selected_target,clicked_player;static int clicked_action,clicked_x,clicked_y,selected_index,project_ok=1,classic_ok=1;
static float projected_x=642,projected_y=1308;
#define CHECK(c,t) do{checks++;if(!(c)){failures++;fprintf(stderr,"FAIL %s\n",t);}}while(0)
static uint64_t screen(uint64_t xy){screen_calls++;screen_input=xy;return (uint32_t)123|(uint64_t)(uint32_t)-45<<32;}
static int project(void *scene,float x,float y,float *out,uint64_t *camera,int mode){project_calls++;CHECK(scene==(void*)0xabc && x==123 && y==-45 && *camera==0x987 && mode==1,"ABI conserva escena, cámara, floats y modo");out[0]=projected_x;out[2]=projected_y;return project_ok;}
static int classic(int *x,int *y){classic_calls++;CHECK(*x==1000&&*y==250,"Clásico recibe píxeles originales");*x=99;*y=88;return classic_ok;}
static void clear(int index){clear_calls++;selected_index=index;selected_target=0;}
static void target(int index,uint64_t unit){target_calls++;selected_index=index;selected_target=unit;}
static int click(uint64_t player,int action,int x,int y,uint64_t flags){click_calls++;clicked_player=player;clicked_action=action;clicked_x=x;clicked_y=y;CHECK(!flags,"Click PC no añade flags de mando");return 0;}
static int scene_order,scene_acquire=1;static _Atomic uint8_t scene_guard;
static int acquire(void *guard){CHECK(guard==&scene_guard && scene_order++==0,"Acquire recibe guard original antes del constructor");return scene_acquire;}
static void construct(void *scene){CHECK(scene==(void*)0xabc&&scene_order++==1,"Constructor precede cámara/proyección");}
static void destroy(void *scene){(void)scene;}
static int at_exit(void(*fn)(void*),void *scene,void *dso){CHECK(fn==destroy&&scene==(void*)0xabc&&dso==(void*)0x123&&scene_order++==2,"Registrar destructor nativo conserva sus tres argumentos");return 0;}
static void release(void *guard){CHECK(guard==&scene_guard&&scene_order++==3,"Release sigue al registro del destructor");atomic_store(&scene_guard,1);}
static uint64_t cast_flags;
static int cast_click(uint64_t player,int action,int x,int y,uint64_t flags){clicked_player=player;clicked_action=action;clicked_x=x;clicked_y=y;cast_flags=flags;click_calls++;return 0;}
static int held_skill=10,held_assoc=-1,selection_calls,selection_ok=1,mouse_ok=1,mouse_calls,last_side,last_phase;
static int select_skill(int skill,int left,int assoc){(void)left;selection_calls++;if(selection_ok){held_skill=skill;held_assoc=assoc;}return selection_ok;}
static int selected_skill(int left,int *skill,int *assoc){(void)left;*skill=held_skill;*assoc=held_assoc;return 1;}
static int skill_mouse(int side,int phase,float x,float y){last_side=side;last_phase=phase;mouse_calls++;CHECK(phase==2||(x==.6f&&y==.7f),"Quickcast conserva puntero exacto en DOWN");return mouse_ok;}
static void test_skill_restore(void){
 pc_skill_api api={select_skill,skill_mouse,selected_skill};pc_skill_state state={0};
 for(int slot=0;slot<16;slot++){held_skill=10;CHECK(pc_skill_begin(&state,&api,slot,395,-1,0,123,1,.6f,.7f)&&state.slot==slot,"Las dieciséis habilidades comparten apuntado exacto y restauración");pc_skill_finish(&state,&api,123,1);}
 for(int left=0;left<2;left++){
  held_skill=10;held_assoc=-1;CHECK(pc_skill_begin(&state,&api,2,395,99,left,123,1,.6f,.7f)&&state.active&&held_skill==395&&last_side==!left&&last_phase==0,"QuickCast selecciona banco/mano y lanza DOWN original");
  pc_skill_finish(&state,&api,123,1);CHECK(!state.active&&held_skill==10&&held_assoc==-1&&last_phase==2,"UP restaura exactamente selección y origen previos");
 }
 held_skill=10;pc_skill_begin(&state,&api,2,395,-1,0,123,1,.6f,.7f);held_skill=42;int count=selection_calls;
 pc_skill_finish(&state,&api,123,1);CHECK(held_skill==42&&selection_calls==count,"Cambiar selección durante QuickCast no se deshace al soltar");
 held_skill=10;pc_skill_begin(&state,&api,2,395,-1,0,123,1,.6f,.7f);count=selection_calls;pc_skill_finish(&state,&api,123,0);CHECK(selection_calls==count,"Cambiar armas impide restaurar el banco previo");
 held_skill=10;pc_skill_begin(&state,&api,2,395,-1,0,123,1,.6f,.7f);count=selection_calls;pc_skill_finish(&state,&api,124,1);CHECK(selection_calls==count,"Cambiar jugador nunca restaura sobre otra partida");
 selection_ok=0;count=mouse_calls;CHECK(!pc_skill_begin(&state,&api,2,395,-1,0,123,1,.6f,.7f)&&mouse_calls==count&&!state.active,"Selección nativa rechazada no castea");selection_ok=1;
 held_skill=10;mouse_ok=0;CHECK(!pc_skill_begin(&state,&api,2,395,-1,0,123,1,.6f,.7f)&&!state.active&&held_skill==10,"Fallo de proyección restaura selección inmediatamente");mouse_ok=1;
 count=selection_calls;pc_skill_finish(&state,&api,123,1);CHECK(selection_calls==count,"Cancelar dos veces no reescribe selección");
 count=mouse_calls;CHECK(!pc_skill_begin(&state,&api,2,395,-1,1,0,1,.6f,.7f)&&mouse_calls==count,"Sin jugador no selecciona ni envía DOWN");
 CHECK(!pc_skill_begin(&state,&api,2,-1,-1,1,123,1,.6f,.7f)&&!pc_skill_begin(&state,&api,2,395,-1,2,123,1,.6f,.7f)&&mouse_calls==count,"Skill/mano inválidas no llegan a lanzamiento");
}
static void test_quickcast_boundary(void){
 pc_mouse_context c={.player=123,.index=0};pc_mouse_api api={.click=cast_click};int tile[2]={5736,5943};
 for(int quick=0;quick<2;quick++)for(int side=0;side<2;side++)for(int phase=0;phase<3;phase++){
  int expected=quick&&phase!=2;
  CHECK(pc_mouse_emit(&c,&api,side,phase,tile,quick?395:-1),"Frontera física/QuickCast conserva despacho nativo");
  CHECK(cast_flags==(uint64_t)expected,"QuickCast DOWN/HELD reproduce bit nativo; UP conserva modificador físico");
  CHECK(clicked_player==123&&clicked_action==side*3+phase&&clicked_x==5736&&clicked_y==5943,"Intención no cambia actor, fases ni destino");
  printf("NATIVE_QUICKCAST %d %d %d %llu\n",quick,side,phase,(unsigned long long)cast_flags);
 }
 int before=click_calls;c.player=0;CHECK(!pc_mouse_emit(&c,&api,0,0,tile,1)&&click_calls==before,"Sin actor no se añade intención de lanzamiento");c.player=123;
 CHECK(!pc_mouse_emit(&c,&api,0,0,tile,-2)&&!pc_mouse_emit(&c,&api,2,0,tile,395)&&!pc_mouse_emit(&c,&api,0,3,tile,1),"Intención/fase/botón ajenos fallan cerrados");
 CHECK(!pc_mouse_emit(NULL,&api,0,0,tile,1)&&!pc_mouse_emit(&c,NULL,0,0,tile,1)&&!pc_mouse_emit(&c,&api,0,0,NULL,1),"Entradas nulas no despachan");
 CHECK(click_calls==before,"Los casos inválidos no llegan al motor");
 for(int stand=0;stand<2;stand++)for(int side=0;side<2;side++)for(int phase=0;phase<3;phase++){
  c.standstill=stand;CHECK(pc_mouse_emit(&c,&api,side,phase,tile,-1)&&cast_flags==(uint64_t)stand,"Acción StandStill reasignable se transmite a ambas manos y UP");
  printf("NATIVE_STANDSTILL %d %d %d %llu\n",stand,side,phase,(unsigned long long)cast_flags);
  CHECK(pc_mouse_emit(&c,&api,side,phase,tile,395)&&cast_flags==(uint64_t)(phase==2?stand:1),"QuickCast normal conserva StandStill sólo al liberar");
 }
 const int excluded[]={357,358,370};
 for(unsigned i=0;i<sizeof excluded/sizeof *excluded;i++)for(int side=0;side<2;side++){
  CHECK(pc_mouse_emit(&c,&api,side,0,tile,excluded[i])&&!cast_flags,"Exclusiones exactas del helper nativo no fuerzan lanzamiento");
 }
 c.standstill=0;for(int skill=359;skill<365;skill++)CHECK(pc_mouse_emit(&c,&api,0,0,tile,skill)&&cast_flags==1,"Predicado original no amplía exclusiones a IDs vecinos");
 const int predicate[]={0,2,3,4,356,357,358,359,364,369,370,371,395};
 for(unsigned i=0;i<sizeof predicate/sizeof *predicate;i++){int skill=predicate[i];pc_mouse_emit(&c,&api,0,0,tile,skill);printf("NATIVE_PREDICATE %d %llu\n",skill,(unsigned long long)cast_flags);}


}
static int pixel_hit_calls,pixel_prefer_calls,prefer_result;static uint64_t hovered;
static int pixel_hit(uint64_t u,int x,int y,int mode,int skill){pixel_hit_calls++;CHECK(mode==0&&skill==-1,"Hover usa comprobación espacial original, sin elegibilidad de mando");(void)u;if(mode!=0)return 1;return x>=85&&x<115&&y>=190&&y<210;}
static int pixel_prefer(uint64_t manager,uint64_t player,uint64_t candidate,uint64_t best){pixel_prefer_calls++;CHECK(manager==123&&player==456&&candidate&&best,"Comparador recibe manager/jugador/candidato/anterior");return prefer_result;}
static int hover_locked,hover_set_calls,hover_clear_calls;
static uint64_t hover_get(int index){CHECK(index==2,"Hover conserva índice local");return hovered;}
static void hover_set(int index,uint64_t u){CHECK(index==2,"Setter hover conserva índice");hovered=u;hover_locked=1;hover_set_calls++;}
static void hover_clear(int index){CHECK(index==2,"Clear hover conserva índice");hovered=0;hover_locked=0;hover_clear_calls++;}
static void test_pixel_pick(void){
 uint64_t heads[6*128]={0};uint8_t units[2][0x160]={{0}};heads[128]=(uintptr_t)units[0];*(uint64_t*)(units[0]+0x158)=(uintptr_t)units[1];
 pc_pick_api api={pixel_hit,pixel_prefer};
 int clicks=click_calls;
 CHECK(pc_pick_units(heads,123,456,115,200,&api)==0,"Borde derecho original excluido: sin radio ni snap táctil");
 CHECK(pc_pick_units(heads,123,456,100,190,&api)==(uintptr_t)units[0],"Hit exacto conserva borde superior incluido");
 for(int result=0;result<4;result++){prefer_result=result;CHECK(pc_pick_units(heads,123,456,100,200,&api)==(uintptr_t)units[result&1],"Prioridad original usa solo bit0 entre hits exactos");}
 pc_hover_apply(2,heads[128],hover_set);CHECK(hovered==heads[128],"Hover sin DOWN publica A");
 pc_hover_apply(2,0,hover_set);CHECK(!hovered,"Snapshot de suelo físico mantiene A vacío sin botón");
 CHECK(hover_locked,"El vacío físico conserva latchA para impedir autoselección posterior");
 int sets=hover_set_calls;hover_locked=0;pc_hover_apply(2,0,hover_set);
 CHECK(hovered==0&&hover_locked&&hover_set_calls==sets+1,"A ya vacío también debe bloquear autoselección, sin retorno prematuro");
 hovered=heads[128];hover_locked=0;sets=hover_set_calls;pc_hover_apply(2,hovered,hover_set);
 CHECK(hover_locked&&hover_set_calls==sets+1,"Target idéntico al automático toma propiedad física explícita");
 hover_clear(2);CHECK(!hover_locked&&!hovered,"Leave/modal libera con clear original sin conservar latch vacío");
 CHECK(click_calls==clicks,"Hover y limpieza nunca emiten movimiento, ataque ni cast");
}
static uint32_t hover_generation=19;static uint64_t hover_mutate_at;static int hover_mutate;
static uint32_t hover_gen(void){return hover_generation;}
static int hover_reader(uint64_t address,void *out,size_t n){if(hover_mutate&&address==hover_mutate_at){hover_mutate=0;hover_generation++;}return pc_trace_read(address,out,n);}
static void test_hover_context(void){
 module_t m={.size=0x2f05460};m.base=calloc(1,m.size);CHECK(m.base!=NULL,"Imagen de contexto disponible");if(!m.base)return;
 _Alignas(8) uint8_t root[0x200]={0},other[0x200]={0};uint32_t actor[3]={0,0,7};uint32_t ids[3]={99,100,7};uint64_t ptr=(uintptr_t)root;
 memcpy(m.base+0x2d60380,ids,sizeof ids);memcpy(m.base+0x2f05458,&ptr,8);root[0x50]=root[0x51]=root[0xd9]=1;
 pc_hover_context c,unchanged={.root=123};
 CHECK(pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&c)&&c.root==ptr&&c.index==2&&c.id==7&&c.generation==19,"Snapshot requiere raíz actual y jugador local antes de publicar UI");
 CHECK(pc_hover_context_same(&m,(uintptr_t)actor,&c,hover_reader,hover_gen),"Contexto estable se revalida");
 pc_hover_owner owner={.index=-1};int emitted=click_calls;
 pc_hover_publish(&owner,&c,DM_PC_POINTER_WORLD,0x44444,hover_set);CHECK(hovered==0x44444&&hover_locked&&owner.index==2,"Mundo válido conserva target físico");
 pc_hover_publish(&owner,&c,DM_PC_POINTER_UI,0x44444,hover_set);CHECK(!hovered&&hover_locked&&owner.player==7,"UI/HUD válido publica NULL aunque haya target anterior");
 int sets=hover_set_calls;pc_hover_publish(&owner,&c,99,0x44444,hover_set);CHECK(hover_set_calls==sets,"Estado de puntero desconocido no publica");
 pc_hover_release(&owner,7,hover_clear);CHECK(!hover_locked&&owner.index==-1,"PointerLeave/cancel libera el slot propio");
 int clears=hover_clear_calls;pc_hover_release(&owner,7,hover_clear);CHECK(hover_clear_calls==clears,"Liberación repetida no toca slot ajeno");
 pc_hover_publish(&owner,&c,DM_PC_POINTER_UI,0,hover_set);clears=hover_clear_calls;pc_hover_release(&owner,8,hover_clear);CHECK(hover_clear_calls==clears&&owner.index==-1,"Cambio de jugador descarta propiedad sin limpiar selección nueva");
 CHECK(click_calls==emitted,"Snapshot UI y cancelación no generan órdenes ni lanzamiento");
 for(int state=0;state<4;state++)CHECK(pc_hover_pointer(state,.5f,.8f)==(state==DM_PC_POINTER_UI||state==DM_PC_POINTER_WORLD),"Sólo snapshots físicos UI/mundo son publicables");
 CHECK(!pc_hover_pointer(DM_PC_POINTER_UI,NAN,.5f)&&!pc_hover_pointer(DM_PC_POINTER_UI,.5f,INFINITY)&&!pc_hover_pointer(DM_PC_POINTER_UI,-.1f,.5f)&&!pc_hover_pointer(DM_PC_POINTER_WORLD,.5f,1.1f),"Fuera de render/no finito libera en vez de bloquear vacío");
 for(int flag=0;flag<3;flag++){int at=flag<2?0x50+flag:0xd9;root[at]=0;CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged)&&unchanged.root==123,"Raíz oculta/inactiva/frontend no publica y conserva salida del lector");root[at]=1;}
 ptr=0;memcpy(m.base+0x2f05458,&ptr,8);CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Raíz ausente falla cerrada");ptr=1;memcpy(m.base+0x2f05458,&ptr,8);CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Raíz ilegible no se desreferencia");ptr=UINT64_MAX-8;memcpy(m.base+0x2f05458,&ptr,8);CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Raíz con overflow se rechaza");ptr=(uintptr_t)root;memcpy(m.base+0x2f05458,&ptr,8);
 actor[0]=1;CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Actor no jugador no toma índice local");actor[0]=0;actor[2]=8;CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Jugador no registrado no publica");actor[2]=7;
 hover_mutate_at=(uintptr_t)root+0x50;hover_mutate=1;CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Generación que cambia durante lectura invalida snapshot");hover_generation=19;
 ptr=(uintptr_t)other;memcpy(other,root,sizeof root);memcpy(m.base+0x2f05458,&ptr,8);CHECK(!pc_hover_context_same(&m,(uintptr_t)actor,&c,hover_reader,hover_gen),"Otra raíz no reutiliza snapshot físico");ptr=(uintptr_t)root;memcpy(m.base+0x2f05458,&ptr,8);
 hover_generation++;CHECK(!pc_hover_context_same(&m,(uintptr_t)actor,&c,hover_reader,hover_gen),"Otra generación no reutiliza snapshot");hover_generation=19;
 CHECK(!pc_hover_context_read(NULL,(uintptr_t)actor,hover_reader,hover_gen,&unchanged)&&!pc_hover_context_read(&m,0,hover_reader,hover_gen,&unchanged)&&!pc_hover_context_read(&m,1,hover_reader,hover_gen,&unchanged),"Módulo/actor ausentes o ilegibles se rechazan");
 CHECK(!pc_hover_context_read(&m,UINT64_MAX-1,hover_reader,hover_gen,&unchanged)&&!pc_hover_context_read(&m,(uintptr_t)actor,NULL,hover_gen,&unchanged)&&!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,NULL,&unchanged),"Overflow o callbacks ausentes fallan cerrados");
 m.size--;CHECK(!pc_hover_context_read(&m,(uintptr_t)actor,hover_reader,hover_gen,&unchanged),"Imagen truncada no lee contexto");free(m.base);
}
static void test_pixel_projection(void){
 int out[2];
 CHECK(pc_classic_pick(400,300,801,0,0,0,1234,5678,out)&&out[0]==1634&&out[1]==5970,"Pick clásico conserva paneo sin perspectiva");
 CHECK(pc_classic_pick(-40,-50,801,1,1,0,1234,5678,out)&&out[0]==1137&&out[1]==5621,"Pick clásico trunca división negativa como ARM");
 CHECK(pc_classic_pick(400,300,801,1,1,1,1234,5678,out)&&out[0]==1834&&out[1]==5963,"Panel lateral clásico conserva el desplazamiento original");
 CHECK(!pc_classic_pick(INT32_MAX,INT32_MIN,801,1,1,0,0,0,out),"Aritmética clásica fuera de rango se rechaza");
 pc_mouse_api api={screen,project,classic,clear,target,click};pc_mouse_context c={2000,1000,1,2,(void*)0xabc,0x987,0x1234,0};
 int tile[2],pick[2];projected_x=20.5f;projected_y=17;
 CHECK(pc_mouse_project(&c,&api,.5f,.25f,tile,pick)&&tile[0]==10&&tile[1]==8&&pick[0]==28&&pick[1]==150,"Hit nativo conserva fracciones HD antes de convertir a ISO");
 projected_x=-.4f;projected_y=0;
 CHECK(pc_mouse_project(&c,&api,.5f,.25f,tile,pick)&&pick[0]==-3&&pick[1]==-1,"Hit HD negativo usa truncado float32 original");
 projected_x=642;projected_y=1308;
}
static int frame_width=3024,frame_height=1900,frame_ok=1,frame_pixel_y;
static int frame_real(int *w,int *h){*w=frame_width;*h=frame_height;return frame_ok;}
static uint64_t frame_screen(uint64_t xy){return xy;}
static int frame_project(void *scene,float x,float y,float *out,uint64_t *camera,int mode){
 (void)scene;(void)x;(void)camera;CHECK(mode==1,"Proyección del puntero conserva modo nativo");frame_pixel_y=(int)y;out[0]=y*2;out[2]=0;return 1;
}
static void test_full_frame_domain(void){
 pc_mouse_context c={.width=3024,.height=1700,.hd=1,.scene=(void*)1};pc_mouse_api api={.screen=frame_screen,.project=frame_project};int tile[2],pick[2];
 const float ui_y[]={.3373183608055115f,.2113485783338547f};const int expected_y[]={773,559};
 for(int i=0;i<2;i++){
  float screen_y=(ui_y[i]*1700+200)/1900;CHECK(pc_mouse_frame(&c,frame_real)&&c.height==1900,"Contexto world usa render1900 y no alturaUI1700");
  CHECK(pc_mouse_project(&c,&api,.5275f,screen_y,tile,pick)&&frame_pixel_y==expected_y[i],"NY convertido una vez llega al píxel real de QA35 cuerpo/etiqueta");
  CHECK(tile[0]==expected_y[i],"El resultado del proyector nativo recibe el píxel completo");
 }
 frame_height=1700;CHECK(pc_mouse_frame(&c,frame_real)&&pc_mouse_project(&c,&api,.5f,.5f,tile,pick)&&frame_pixel_y==850,"Render16:9 sin reducciónUI conserva comportamiento");
 frame_ok=0;c.height=999;CHECK(!pc_mouse_frame(&c,frame_real)&&c.height==999,"Consulta fallida no reutiliza dimensiones parciales");frame_ok=1;
 frame_height=0;CHECK(!pc_mouse_frame(&c,frame_real),"Frame vacío no proyecta");frame_height=16385;CHECK(!pc_mouse_frame(&c,frame_real),"Frame fuera de rango no proyecta");frame_height=1900;
}
static void test_cached_release(void){
 pc_mouse_hold held[2]={{0}};int tile[2]={0};int previous_project=project_calls;
 CHECK(!pc_mouse_hold_phase(&held[0],11,2,tile),"UP sin DOWN no reutiliza coordenadas");
 for(int side=0;side<2;side++){
  tile[0]=5705+side;tile[1]=5968+side;CHECK(pc_mouse_hold_phase(&held[side],11,0,tile),"DOWN activa caché independiente por botón");
  tile[0]=5717+side;tile[1]=5974+side;CHECK(pc_mouse_hold_phase(&held[side],11,1,tile),"HELD actualiza último tile válido");
 }
 for(int side=0;side<2;side++){
  tile[0]=tile[1]=0;CHECK(pc_mouse_hold_phase(&held[side],11,2,tile)&&tile[0]==5717+side&&tile[1]==5974+side,"UP conserva último destino de su botón y jugador");
  click(123,side*3+2,tile[0],tile[1],0);CHECK(clicked_action==side*3+2&&clicked_x==5717+side&&clicked_y==5974+side,"Liberación2/5 entrega XY nativos sin cambiar fases");
  CHECK(!held[side].active&&!pc_mouse_hold_phase(&held[side],11,2,tile),"Cancelar o soltar dos veces no emite segunda liberación");
 }
 tile[0]=6000;tile[1]=6100;CHECK(pc_mouse_hold_phase(&held[0],11,0,tile),"Nuevo gesto rearma caché");
 tile[0]=tile[1]=0;CHECK(!pc_mouse_hold_phase(&held[0],12,2,tile)&&!held[0].active,"Cambio de jugador descarta liberación anterior");
 CHECK(!pc_mouse_hold_phase(&held[0],12,1,tile),"HELD de otro jugador no hereda destino");
 tile[0]=7000;tile[1]=7100;CHECK(pc_mouse_hold_phase(&held[0],12,0,tile),"Nuevo jugador inicia gesto propio");
 tile[0]=tile[1]=0;CHECK(pc_mouse_hold_phase(&held[0],12,2,tile)&&tile[0]==7000&&tile[1]==7100,"Cancelación por contexto conserva UP original y último tile, sin reproyección");
 CHECK(project_calls==previous_project,"Caché/cancelación/UP no llaman al proyector");
 CHECK(!pc_mouse_hold_phase(&held[0],12,3,tile)&&!pc_mouse_hold_phase(NULL,12,0,tile),"Fase inválida o estado ausente se rechaza");
}
static int fresh_valid;static float fresh_x=.43f,fresh_y=.58f;
static int fresh_pointer(float *x,float *y){*x=fresh_x;*y=fresh_y;return fresh_valid;}
static void test_mouse_hand(void) {
 pc_skill_pointer cached={.1f,.2f,1};fresh_valid=0;CHECK(!pc_skill_pointer_refresh(&cached,fresh_pointer)&&!cached.allowed,"QuickCast cacheado como permitido se invalida si ahora apunta dentro de Inventario/UI");
 fresh_valid=1;CHECK(pc_skill_pointer_refresh(&cached,fresh_pointer)&&cached.x==.43f&&cached.y==.58f,"QuickCast toma coordenadas actuales cuando el puntero vuelve a mundo descubierto");fresh_x=NAN;CHECK(!pc_skill_pointer_refresh(&cached,fresh_pointer)&&!cached.allowed,"Snapshot actual no finito no puede lanzar");fresh_x=.43f;
 _Alignas(8) uint8_t actor[0x200]={0},inventory[0x80]={0},item[0x40]={0};uint64_t inv=(uintptr_t)inventory,held=(uintptr_t)item;uint32_t magic=0x1020304,type=4,id=77;
 memcpy(actor+0x90,&inv,8);memcpy(inventory,&magic,4);memcpy(item,&type,4);memcpy(item+8,&id,4);pc_mouse_hand_state state;
 CHECK(!pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)&&!state.item,"Inventario original con mano vacía distingue intención continua");
 memcpy(inventory+0x40,&held,8);CHECK(pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)==1&&state.inventory==inv&&state.item==held&&state.id==77,"Snapshot held se obtiene del contenedor original del jugador");
 for(int side=0;side<2;side++){CHECK(pc_mouse_down_intent(side,-1,0)==0,"Mano vacía conserva intención física de ambos botones");CHECK(pc_mouse_down_intent(side,-1,1)==(side?-1:1),"Held sólo admite one-shot izquierdo, nunca RMB");CHECK(pc_mouse_down_intent(side,395,1)==-1,"QuickCast no transforma held en una orden drop");CHECK(pc_mouse_down_intent(side,-1,-1)==-1,"Lectura inválida rechaza antes del emisor nativo");}
 pc_mouse_context c={.player=123};pc_mouse_api api={.click=cast_click};int tile[]={5705,5969};int calls=click_calls;
 CHECK(pc_mouse_drop_emit(&c,&api,tile)==DM_PC_MOUSE_ONESHOT&&click_calls==calls+1&&clicked_action==0&&!cast_flags,"Emisor held produce un DOWN original y marca retorno one-shot2");
 CHECK(!pc_mouse_drop_emit(NULL,&api,tile)&&!pc_mouse_drop_emit(&c,NULL,tile)&&!pc_mouse_drop_emit(&c,&api,NULL),"Emisor sin contexto/API/tile no llama al juego");
 *(uint32_t*)inventory=0;CHECK(pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)<0,"Contenedor de otra versión no se acepta por dirección");*(uint32_t*)inventory=magic;
 *(uint32_t*)item=1;CHECK(pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)<0,"Held de otra clase no se despacha");*(uint32_t*)item=4;
 *(uint64_t*)(inventory+0x40)=1;CHECK(pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)<0,"Referencia ilegible falla mediante lector VM");*(uint64_t*)(inventory+0x40)=held;
 *(uint64_t*)(actor+0x90)=1;CHECK(pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)<0,"Inventario ilegible nunca se desreferencia directamente");*(uint64_t*)(actor+0x90)=inv;
 *(uint32_t*)actor=1;CHECK(pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,&state)<0,"Actor ajeno a jugador se rechaza");*(uint32_t*)actor=0;
 CHECK(pc_mouse_hand_read(UINT64_MAX-8,pc_trace_read,&state)<0&&pc_mouse_hand_read(1,pc_trace_read,&state)<0&&pc_mouse_hand_read(0,pc_trace_read,&state)<0,"NULL, overflow y memoria inválida no pasan al click");
 CHECK(pc_mouse_hand_read((uintptr_t)actor,NULL,&state)<0&&pc_mouse_hand_read((uintptr_t)actor,pc_trace_read,NULL)<0,"Lector/salida ausentes fallan cerrados");
}
int main(void){
 test_mouse_hand();
 test_hover_context();
 test_full_frame_domain();
 test_cached_release();
 test_pixel_projection();test_pixel_pick();
 test_skill_restore();test_quickcast_boundary();
 pc_scene_api init={acquire,construct,at_exit,release};
 pc_scene_prepare(&scene_guard,(void*)0xabc,destroy,(void*)0x123,&init);CHECK(scene_order==4&&atomic_load(&scene_guard)==1,"Escena se construye una vez con protocolo original");
 pc_scene_prepare(&scene_guard,(void*)0xabc,destroy,(void*)0x123,&init);CHECK(scene_order==4,"Escena inicializada no se reconstruye");
 atomic_store(&scene_guard,0);scene_order=0;scene_acquire=0;pc_scene_prepare(&scene_guard,(void*)0xabc,destroy,(void*)0x123,&init);CHECK(scene_order==1,"Acquire cero continúa sin constructor ni release");
 pc_mouse_api api={screen,project,classic,clear,target,click};pc_mouse_context c={2000,1000,1,2,(void*)0xabc,0x987,0x1234,0};
 for(int side=0;side<2;side++)for(int phase=0;phase<3;phase++){
  int previous=project_calls;CHECK(pc_mouse_call(&c,&api,side,phase,.5f,.25f),"Cada fase se entrega aunque el retorno nativo sea cero");
  CHECK(clicked_action==side*3+phase&&clicked_player==c.player,"DOWN/HELD/UP: 0/1/2 izquierda, 3/4/5 derecha");
  CHECK(phase==2 ? project_calls==previous : clicked_x==321&&clicked_y==654,"Soltar no reproyecta; lanzar conserva destino exacto");
 }
 CHECK(screen_input==((uint64_t)250<<32|1000)&&selected_index==2,"Puntero respeta viewport y jugador local");
 CHECK(clear_calls==4&&!target_calls,"Suelo vacío limpia objetivo nativo");
 c.target=0x123456;CHECK(pc_mouse_call(&c,&api,1,0,.5f,.25f)&&target_calls==1&&selected_target==c.target,"Unidad bajo cursor entra en selector nativo");
 c.hd=0;CHECK(pc_mouse_call(&c,&api,0,0,.5f,.25f)&&classic_calls==1&&clicked_x==99&&clicked_y==88,"Clásico usa conversión original independiente");
 c.hd=1;int previous=click_calls;project_ok=0;CHECK(!pc_mouse_call(&c,&api,1,0,.5f,.25f)&&click_calls==previous,"Proyección fallida no lanza");project_ok=1;
 projected_x=NAN;CHECK(!pc_mouse_call(&c,&api,1,0,.5f,.25f)&&click_calls==previous,"Resultado no finito no se convierte a int");projected_x=642;
 CHECK(!pc_mouse_call(&c,&api,1,0,NAN,.25f)&&!pc_mouse_call(&c,&api,1,0,1.01f,.25f),"Posición inválida no dispara");
 c.width=0;CHECK(!pc_mouse_call(&c,&api,0,0,.5f,.25f)&&pc_mouse_call(&c,&api,0,2,NAN,NAN),"UP funciona sin viewport válido");
 c.index=3;CHECK(!pc_mouse_call(&c,&api,0,0,.5f,.25f),"Índice local ajeno se rechaza");c.index=0;c.player=0;CHECK(!pc_mouse_call(&c,&api,0,2,0,0),"Sin jugador no llama al motor");
 FILE *f=fopen("build/main.orig.bin","rb");if(!f){perror("fixture original");return 2;}fseek(f,0,SEEK_END);size_t n=ftell(f);rewind(f);uint8_t *binary=malloc(n);CHECK(fread(binary,1,n,f)==n,"Imagen original completa");fclose(f);module_t m={0};m.base=binary;m.size=n;
 CHECK(pc_skill_abi(&m),"Firmas de slot/getters/selección/QuickCast nativos verificadas");
 for(size_t at=0x4acfec;at<0x4acff8;at+=4){binary[at]^=1;CHECK(!pc_skill_abi(&m),"Cambiar transformación del bit nativo bloquea QuickCast");binary[at]^=1;}
 for(size_t at=0x4ae9c8;at<0x4ae9d8;at+=4){binary[at]^=1;CHECK(!pc_skill_abi(&m),"Cambiar decisión nativa cast/caminar bloquea QuickCast");binary[at]^=1;}
 for(size_t at=0x2b228c;at<0x2b22b0;at+=4){binary[at]^=1;CHECK(!pc_skill_abi(&m),"Predicado QuickCast alterado bloquea habilidad temporal");binary[at]^=1;}
 const size_t standsites[]={0x369c30,0x369c40,0x2c1acc};for(unsigned i=0;i<3;i++){binary[standsites[i]]^=1;CHECK(!pc_mouse_abi(&m),"StandStill desconocido no se lee con otra ABI");binary[standsites[i]]^=1;}
 const size_t dropsites[]={0x4ad3e0,0x466424,0x3b8bac};for(unsigned i=0;i<3;i++)for(int j=0;j<4;j++){size_t at=dropsites[i]+j*4;binary[at]^=1;CHECK(!pc_mouse_abi(&m),"Mutación de ruta held bloquea la ABI PC");binary[at]^=1;}
 CHECK(pc_mouse_abi(&m),"Las firmas coinciden con ejecutable original");binary[0x4ace20]^=1;CHECK(!pc_mouse_abi(&m),"Otra ABI se rechaza antes de llamar");free(binary);
 printf("PC MOUSE %d comprobaciones, %d fallos\n",checks,failures);return failures?1:0;
}
