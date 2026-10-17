/* Original w_go request lifecycle. Candidate main-thread internal API; the
 * public queue and capability remain disabled until the complete bridge passes.
 * now=monotonic ns; frame=present count; tick=real server tick, never frame. */
typedef struct {uint64_t now,frame,stick,game;uint32_t tick;int tick_valid;} xblr_world_clock;
static struct {
 int on,lx,ly,replans,empty,direct,alt,alt_on,ax,ay,tx,ty,i;
 uint32_t type,id,still_tick;int still_valid;
 uint64_t until,stick,game,next_frame,release_frame,release_player;
 int release_x,release_y;
 xblr_world_route route;
} world_go;
static uint64_t world_player(module_t *m){uint32_t id;if(!world_field(&id,(uintptr_t)m->base,0x2d20e4c,4))return 0;uint64_t a=id;return world_call(m,0x45f9d0,&a,1);}
static int world_move(module_t *m,uint64_t player,int x,int y){if(x<0||y<0||x>65535||y>65535)return 0;uint64_t a[4]={3,player,(uint32_t)x,(uint32_t)y};world_call(m,0x4ad870,a,4);return 1;}
static int world_release(module_t *m,uint64_t player){
 if(!world_go.release_player)return 1;
 uint64_t previous=world_go.release_player;world_go.release_player=0;
 if(player!=previous)return 0;
 uint64_t a[5]={player,2,(uint32_t)world_go.release_x,(uint32_t)world_go.release_y,0};world_call(m,0x4ace20,a,5);return 1;
}
static int world_interact(module_t *m,uint64_t player,uint64_t unit,const xblr_world_clock *c){
 int x,y;if(!world_unit_xy(unit,&x,&y)||!world_release(m,player))return 0;
 uint64_t manager=world_call(m,0x2acecc,0,0),saved=0;
 if(!manager||manager>UINT64_MAX-0xd10||!world_field(&saved,manager,0xd10,8)||!world_write_target_slot(manager+0xd10,unit))return 0;
 uint64_t a[5]={player,7,(uint32_t)x,(uint32_t)y,0};world_call(m,0x4ace20,a,5);
 /* Restore even when the game declines the press. A failed restore is fatal:
  * continuing with the wrong target would corrupt subsequent original input. */
 if(!world_write_target_slot(manager+0xd10,saved))xblr_execution_stop(XBLR_EXEC_HOST_FAILURE);
 world_go.release_player=player;world_go.release_x=x;world_go.release_y=y;world_go.release_frame=c->frame+3;return 1;
}
/* -1 incomplete, 0 no requirement, 1 same-room required. */
static int world_needs_room(module_t *m,uint64_t unit){
 uint32_t f[2];uint8_t ver;if(!world_field(f,unit,0,8))return -1;if(f[0]!=2)return 0;
 if(!world_field(&ver,unit,0x1bd,1))return -1;if(ver<1||ver>3)return 0;
 uint64_t table=0,n=0,records=0;uint8_t action;
 if(!world_field(&table,(uintptr_t)m->base,0x2d64a18+ver*16,8))return -1;
 if(!table)return 0;
 if(!world_field(&n,table,0x1550,8))return -1;if(f[1]>=n)return 0;
 if(!world_field(&records,table,0x1548,8)||!records||!world_field(&action,records,(uint64_t)f[1]*0x168+0x15e,1))return -1;
 return action==16;
}
static int world_same_room(uint64_t player,uint64_t unit){
 uint64_t a=0,b=0,ar=0,br=0;
 if(!world_field(&a,player,0x38,8)||!world_field(&b,unit,0x38,8)||!a||!b||!world_field(&ar,a,0x20,8)||!world_field(&br,b,0,8))return -1;
 return br&&ar==br;
}
static struct {uint32_t id;uint64_t level,at;int x,y,tries;} world_tile_retry;
static int world_level(uint64_t player,uint64_t *level){
 uint64_t path=0,room=0,data=0;
 if(!world_field(&path,player,0x38,8)||!path||!world_field(&room,path,0x20,8)||!room||!world_field(&data,room,0x18,8)||!data||!world_field(level,data,0x90,8))return 0;
 return *level!=0;
}
static int world_near_object(module_t *m,uint64_t player,int ox,int oy,uint64_t *out){
 uint64_t best=0;int bestd=-1;unsigned remaining=4096;
 for(unsigned bucket=0;bucket<128;bucket++){
  uint64_t unit;if(!world_field(&unit,(uintptr_t)m->base,0x2d603a0+2*0x400+bucket*8,8))return 0;
  while(unit){
   uint32_t type;uint64_t next;int x,y;
   if(!remaining--||!world_field(&type,unit,0,4)||type!=2||!world_field(&next,unit,0x158,8)||!world_unit_xy(unit,&x,&y))return 0;
   if(world_plan_abs(x-ox)<=2&&world_plan_abs(y-oy)<=2){
    uint64_t select[5]={unit,0,0,UINT64_MAX,UINT64_MAX},category[3]={0x165,player,unit};
    if((world_call(m,0x464a70,select,5)&1)&&world_call(m,0x2f7b7c,category,3)==2){int d=(x-ox)*(x-ox)+(y-oy)*(y-oy);if(bestd<0||d<bestd){bestd=d;best=unit;}}
   }
   unit=next;
  }
 }
 *out=best;return 1;
}
static int world_tile_retry_pump(module_t *m,uint64_t player,int px,int py,const xblr_world_clock *c){
 if(!world_tile_retry.at||c->frame<world_tile_retry.at)return 0;world_tile_retry.at=0;
 uint64_t level=0,act=0;
 if(world_go.on||!world_level(player,&level)||level!=world_tile_retry.level||world_plan_abs(px-world_tile_retry.x)>4||world_plan_abs(py-world_tile_retry.y)>4)return 0;
 if(!world_near_object(m,player,world_tile_retry.x,world_tile_retry.y,&act))return -1;
 if(!act){
  if(world_tile_retry.tries>=2)return 0;
  if(world_find(m,5,world_tile_retry.id,&act)<=0)return 0;
  world_tile_retry.tries++;world_tile_retry.at=c->frame+45;
 }
 return world_interact(m,player,act,c)?1:-1;
}
static int world_replan(uint64_t player,int px,int py,int x,int y){
 uint64_t path=0,room=0;if(!world_field(&path,player,0x38,8)||!path||!world_field(&room,path,0x20,8))return -1;
 xblr_world_route route;int rc=world_plan(room,px,py,x,y,world_go.type,&route);if(rc<0)return -1;
 world_go.route=route;world_go.i=0;world_go.tx=x;world_go.ty=y;world_go.next_frame=0;return rc;
}
/* Return 0 while selecting another side, 1 interaction issued, -1 unsafe. */
static int world_arrive(module_t *m,uint64_t player,uint64_t unit,int ox,int oy,const xblr_world_clock *c){
 int need=world_needs_room(m,unit),same=need>0?world_same_room(player,unit):1;if(need<0||same<0)return -1;
 if(need&&!same&&world_go.alt<4){
  static const int offset[4][2]={{0,3},{0,-3},{-3,0},{3,0}};
  world_go.ax=ox+offset[world_go.alt][0];world_go.ay=oy+offset[world_go.alt][1];world_go.alt++;world_go.alt_on=1;
  world_go.route.n=world_go.i=world_go.replans=world_go.empty=world_go.direct=world_go.still_valid=0;world_go.next_frame=0;world_go.until=c->now+8000000000ull;return 0;
 }
 uint64_t act=unit;
 if(world_go.type==5){
  uint64_t manager=world_call(m,0x2acecc,0,0);uint32_t id;
  if(!manager||!world_field(&id,(uintptr_t)m->base,0x2d20e4c,4))return -1;
  uint64_t a[2]={manager,id};uint64_t nt=world_call(m,0x2f7a0c,a,2);
  if(nt&&nt!=unit){uint32_t type;int x,y;if(!world_field(&type,nt,0,4))return -1;
   if(type==2){if(!world_unit_xy(nt,&x,&y))return -1;if(world_plan_abs(x-ox)<=3&&world_plan_abs(y-oy)<=3)act=nt;}
  }
  if(act==unit){uint64_t level;if(!world_level(player,&level))return -1;world_tile_retry.id=world_go.id;world_tile_retry.level=level;world_tile_retry.at=c->frame+45;world_tile_retry.x=ox;world_tile_retry.y=oy;world_tile_retry.tries=0;}
 }
 world_go.on=0;return world_interact(m,player,act,c)?1:-1;
}
static int world_arrive_checked(module_t *m,uint64_t player,uint64_t unit,int x,int y,const xblr_world_clock *c){int rc=world_arrive(m,player,unit,x,y,c);if(rc<0)world_go.on=0;return rc;}
static int world_go_begin(module_t *m,const xblr_world_target *target,const xblr_world_clock *c){
 xblr_guest_thread *thread=xblr_current_thread();if(!thread||!thread->is_main||!m||!c||!target||!target->unit||target->type>5||!c->tick_valid||!c->game)return 0;
 uint64_t player=world_player(m);if(world_go.game&&world_go.game!=c->game)world_go.release_player=0;if(!player||!world_release(m,player))return 0;
 memset(&world_go,0,sizeof world_go);memset(&world_tile_retry,0,sizeof world_tile_retry);world_go.on=1;world_go.type=target->type;world_go.id=target->id;world_go.until=c->now+15000000000ull;world_go.stick=c->stick;world_go.game=c->game;return 1;
}
/* -1 cancelled/unsafe, 0 pending/idle, 1 interaction accepted by bridge.
 * A nonzero return never proves the original game completed the interaction. */
static int world_go_pump(module_t *m,const xblr_world_clock *c,int cancel){
 xblr_guest_thread *thread=xblr_current_thread();if(!thread||!thread->is_main||!m||!c)return -1;
 uint64_t player=world_player(m);
 if(world_go.game!=c->game){world_go.on=0;world_go.release_player=0;world_tile_retry.at=0;return -1;}
 if(cancel){world_go.on=0;world_tile_retry.at=0;world_release(m,player);return -1;}
 if(world_go.release_player&&c->frame>=world_go.release_frame&&!world_release(m,player))return -1;
 if(!c->tick_valid||c->stick!=world_go.stick){world_go.on=0;world_tile_retry.at=0;world_release(m,player);return -1;}
 if(!world_go.on){int px,py;if(!player||!world_unit_xy(player,&px,&py))return 0;return world_tile_retry_pump(m,player,px,py,c);}
 if(!player||!c->tick_valid||c->now>world_go.until||c->stick!=world_go.stick){world_go.on=0;world_release(m,player);return -1;}
 uint64_t unit=0;if(world_find(m,world_go.type,world_go.id,&unit)<=0){world_go.on=0;return -1;}
 int px,py,x,y;uint32_t mode;
 if(!world_unit_xy(player,&px,&py)||!world_unit_xy(unit,&x,&y)||(world_go.type==4&&(!world_field(&mode,unit,0xc,4)||mode!=3))){world_go.on=0;return -1;}
 int ox=x,oy=y;if(world_go.alt_on){x=world_go.ax;y=world_go.ay;}
 int dx=world_plan_abs(x-px),dy=world_plan_abs(y-py);
 if(px!=world_go.lx||py!=world_go.ly||!world_go.still_valid){world_go.lx=px;world_go.ly=py;world_go.still_tick=c->tick;world_go.still_valid=1;}
 else if(world_go.next_frame&&(uint32_t)(c->tick-world_go.still_tick)>25){
  if(dx<=4&&dy<=4)return world_arrive_checked(m,player,unit,ox,oy,c);
  if(world_go.replans++<3){world_go.route.n=0;world_go.next_frame=0;world_go.still_tick=c->tick;}
  else if(world_go.alt_on)return world_arrive_checked(m,player,unit,ox,oy,c);
  else {world_go.on=0;return -1;}
 }
 int near=world_go.type==5?1:2;
 if(dx<=near&&dy<=near)return world_arrive_checked(m,player,unit,ox,oy,c);
 if(world_go.type==5&&dx<=2&&dy<=2&&!world_go.direct){world_go.route.n=world_go.i=0;world_go.direct=1;world_go.still_valid=0;world_go.next_frame=c->frame+20;world_move(m,player,x,y);return 0;}
 if(world_go.type==5&&world_go.direct)return 0;
 if(!world_go.direct&&(!world_go.route.n||world_plan_abs(x-world_go.tx)>6||world_plan_abs(y-world_go.ty)>6)){
  if(world_replan(player,px,py,x,y)<0){world_go.on=0;return -1;}
 }
 while(world_go.i<world_go.route.n&&world_plan_abs(world_go.route.node[world_go.i][0]-px)<=1&&world_plan_abs(world_go.route.node[world_go.i][1]-py)<=1){world_go.i++;world_go.next_frame=0;}
 if(world_go.i>=world_go.route.n){
  if(world_go.route.n&&world_go.empty++<8){world_go.route.n=0;world_go.next_frame=0;return 0;}
  if(dx<=4&&dy<=4){if(!world_go.direct){world_go.direct=1;world_go.still_valid=0;world_go.next_frame=c->frame+20;world_move(m,player,x,y);}return 0;}
  if(world_go.alt_on||(world_go.route.nearest&&world_go.type==2&&dx<=8&&dy<=8))return world_arrive_checked(m,player,unit,ox,oy,c);
  world_go.on=0;return -1;
 }
 world_go.empty=0;if(c->frame<world_go.next_frame)return 0;world_go.next_frame=c->frame+20;
 return world_move(m,player,world_go.route.node[world_go.i][0],world_go.route.node[world_go.i][1])?0:-1;
}
