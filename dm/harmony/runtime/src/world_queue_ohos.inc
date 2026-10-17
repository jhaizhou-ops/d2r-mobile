/* ABI1 world request queue. Calls are gated by WORLD_TAP capability and all
 * four independent observation-valid bits. WORLD_TAP is published only by a
 * fully valid in-game snapshot after the real server tick identity was seen. Enqueue threads never call/read/write guest objects. */
static pthread_mutex_t world_request_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {float x,y;uint64_t sequence;int pending,cancel;} world_request;
static uint64_t world_sequence,world_active_request;
static _Atomic uint64_t world_stick_generation;
static _Atomic int world_focus=1,world_stick_active;
void xblr_world_note_stick(int32_t lx,int32_t ly){atomic_store(&world_stick_active,lx||ly);if(lx||ly)atomic_fetch_add_explicit(&world_stick_generation,1,memory_order_relaxed);}
void xblr_cancel_world(void){pthread_mutex_lock(&world_request_lock);world_request.cancel=1;pthread_mutex_unlock(&world_request_lock);}
static void world_set_focus(int focused){atomic_store(&world_focus,focused);if(!focused)xblr_cancel_world();}
int xblr_world_tap(float nx,float ny,uint64_t *request){
 if(!request||!isfinite(nx)||!isfinite(ny)||nx<0||nx>1||ny<0||ny>1)return XBLR_EXEC_ARGUMENT;
 pthread_mutex_lock(&interaction_lock);
 int ready=atomic_load(&world_focus)&&!atomic_load(&world_stick_active)&&(interaction.capabilities&XBLR_INTERACTION_WORLD_TAP)&&(interaction.valid&15)==15&&interaction.mode==XBLR_INTERACTION_GAME&&!interaction.navigation;
 pthread_mutex_unlock(&interaction_lock);if(!ready)return XBLR_EXEC_BUSY;
 pthread_mutex_lock(&world_request_lock);
 if(world_sequence==UINT64_MAX){pthread_mutex_unlock(&world_request_lock);return XBLR_EXEC_BUSY;}
 uint64_t sequence=++world_sequence;world_request.x=nx;world_request.y=ny;world_request.sequence=sequence;world_request.pending=1;world_request.cancel=0;
 pthread_mutex_unlock(&world_request_lock);*request=sequence;return 0;
}
static void world_complete(uint64_t sequence,int result){if(!sequence)return;pthread_mutex_lock(&interaction_lock);interaction.completed_request=sequence;interaction.request_result=result;pthread_mutex_unlock(&interaction_lock);}
static void xblr_world_pump(void){
 xblr_guest_thread *thread=xblr_current_thread();if(!thread||!thread->is_main)return;
 static uint64_t last_frame=UINT64_MAX;uint64_t frame=atomic_load_explicit(&frames,memory_order_relaxed);if(last_frame==frame)return;last_frame=frame;
 pthread_mutex_lock(&world_request_lock);
 int pending=world_request.pending,cancel=world_request.cancel;float nx=world_request.x,ny=world_request.y;uint64_t sequence=world_request.sequence;
 world_request.pending=world_request.cancel=0;pthread_mutex_unlock(&world_request_lock);
 if(!pending&&!cancel&&!world_active_request&&!world_go.release_player&&!world_tile_retry.at)return;
 /* A pending/cancelled action rechecks current scene, including same frame. */
 xblr_publish_interaction_force(1);
 module_t *m=0;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){m=&g_mod[i];break;}
 struct timespec ts;int time_valid=clock_gettime(CLOCK_MONOTONIC,&ts)==0;uint64_t game=0;uint32_t tick=0;
 xblr_world_clock c={.now=time_valid?(uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec:0,.frame=frame,.stick=atomic_load_explicit(&world_stick_generation,memory_order_relaxed)};
 int tick_valid=m&&world_tick_read(m,&game,&tick);c.tick_valid=time_valid&&tick_valid;c.game=game;c.tick=tick;
 pthread_mutex_lock(&interaction_lock);
 int ready=atomic_load(&world_focus)&&!atomic_load(&world_stick_active)&&(interaction.capabilities&XBLR_INTERACTION_WORLD_TAP)&&(interaction.valid&15)==15&&interaction.mode==XBLR_INTERACTION_GAME&&!interaction.navigation;
 uint32_t width=interaction.viewport_width,height=interaction.viewport_height;pthread_mutex_unlock(&interaction_lock);
 if(cancel||!ready||!c.tick_valid){
  if(m)world_go_pump(m,&c,1);world_complete(world_active_request,cancel?-2:-1);world_active_request=0;if(pending)world_complete(sequence,cancel?-2:-1);return;
 }
 if(pending){
  if(world_active_request||world_go.release_player||world_tile_retry.at){world_go_pump(m,&c,1);world_complete(world_active_request,-2);world_active_request=0;}
  xblr_world_target target;
  if(!width||!height||!world_pick(m,nx*width,ny*height,height*.10f,&target)){world_complete(sequence,-1);return;}
  if(!target.unit){world_complete(sequence,0);return;}
  if(!world_go_begin(m,&target,&c)){world_complete(sequence,-1);return;}
  world_active_request=sequence;
 }
 int rc=world_go_pump(m,&c,0);
 if(rc<0){world_complete(world_active_request,-1);world_active_request=0;}
 else if(rc>0){int result=world_go.type==4?1:world_go.type==1?2:world_go.type==2?3:world_go.type==0?5:4;world_complete(world_active_request,result);world_active_request=0;}
}
