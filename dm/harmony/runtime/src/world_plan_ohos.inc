/* Original sw_world BFS/line compression on copied room metadata and bounded
 * cell reads. Main-thread-only candidate; no movement or guest calls. */
#define WORLD_PLAN_W 192
#define WORLD_PLAN_CAP 24000
#define WORLD_PLAN_SEG 30
#define WORLD_PLAN_MASK 0x0d01
typedef struct {uint64_t identity,cells;int32_t x,y,w,h,stride;} xblr_world_room;
typedef struct {int n,nearest;unsigned expanded;uint16_t node[24][2];} xblr_world_route;
static struct {
 xblr_world_room rooms[96];unsigned count;int ox,oy,failed,last;
 uint8_t blocked[WORLD_PLAN_W*WORLD_PLAN_W],from[WORLD_PLAN_W*WORLD_PLAN_W];
 uint32_t queue[WORLD_PLAN_W*WORLD_PLAN_W];
} world_planner;
static int world_plan_rooms(uint64_t first) {
 world_planner.count=0;world_planner.last=world_planner.failed=0;
 if(!first)return 0;world_planner.rooms[world_planner.count++].identity=first;
 for(unsigned i=0;i<world_planner.count;i++){
  xblr_world_room *r=&world_planner.rooms[i];int32_t bounds[4];uint64_t adj=0,grid=0;uint32_t n=0;
  if(!world_field(bounds,r->identity,0x80,sizeof bounds)||bounds[2]<=0||bounds[3]<=0||
     !world_field(&adj,r->identity,0,8)||!world_field(&n,r->identity,0x40,4)||n>32||(n&&!adj)||
     !world_field(&grid,r->identity,0x38,8))return 0;
  r->x=bounds[0];r->y=bounds[1];r->w=bounds[2];r->h=bounds[3];r->cells=0;r->stride=0;
  if(grid&&(!world_field(&r->stride,grid,8,4)||r->stride<r->w||!world_field(&r->cells,grid,0x20,8)))return 0;
  for(unsigned k=0;k<n;k++){
   uint64_t next;if(!world_field(&next,adj,k*8,8))return 0;if(!next)continue;
   unsigned q=0;while(q<world_planner.count&&world_planner.rooms[q].identity!=next)q++;
   if(q==world_planner.count){if(q==96)return 0;world_planner.rooms[world_planner.count++].identity=next;}
  }
 }
 return 1;
}
static uint16_t world_plan_cell(int x,int y) {
 for(unsigned t=0;t<world_planner.count;t++){
  unsigned i=(world_planner.last+t)%world_planner.count;xblr_world_room *r=&world_planner.rooms[i];
  int64_t dx=(int64_t)x-r->x,dy=(int64_t)y-r->y;
  if(dx<0||dy<0||dx>=r->w||dy>=r->h)continue;
  world_planner.last=i;if(!r->cells)return 0xffff;
  uint64_t index=(uint64_t)dy*(uint32_t)r->stride+(uint64_t)dx;uint16_t cell;
  if(index>UINT64_MAX/2||!world_field(&cell,r->cells,index*2,2)){world_planner.failed=1;return 0xffff;}
  return cell;
 }
 return 0xffff;
}
static int world_plan_blocked(int x,int y){
 int ix=x-world_planner.ox,iy=y-world_planner.oy;if(ix<0||iy<0||ix>=WORLD_PLAN_W||iy>=WORLD_PLAN_W)return 1;
 uint8_t *m=&world_planner.blocked[iy*WORLD_PLAN_W+ix];if(!*m)*m=world_plan_cell(x,y)&WORLD_PLAN_MASK?2:1;return *m==2;
}
static int world_plan_free(int x,int y){return !(world_plan_blocked(x,y)||world_plan_blocked(x+1,y)||world_plan_blocked(x-1,y)||world_plan_blocked(x,y+1)||world_plan_blocked(x,y-1));}
static int world_plan_abs(int n){return n<0?-n:n;}
static int world_plan_line(int x0,int y0,int x1,int y1){
 int dx=world_plan_abs(x1-x0),dy=world_plan_abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,e=dx-dy;
 for(;;){if(!world_plan_free(x0,y0))return 0;if(x0==x1&&y0==y1)return 1;int e2=2*e,mx=0,my=0;if(e2>-dy){e-=dy;mx=1;}if(e2<dx){e+=dx;my=1;}
  if(mx&&my&&(!world_plan_free(x0+sx,y0)||!world_plan_free(x0,y0+sy)))return 0;if(mx)x0+=sx;if(my)y0+=sy;}
}
/* -1 incomplete snapshot, 0 safe no route, 1 completed route (possibly empty
 * when already near). No partial output on unsafe input or failed read. */
static int world_plan(uint64_t room,int px,int py,int x,int y,unsigned type,xblr_world_route *out){
 xblr_guest_thread *thread=xblr_current_thread();
 if(!thread||!thread->is_main||!out||type>5||px<0||py<0||x<0||y<0||px>65535||py>65535||x>65535||y>65535)return -1;
 if(!world_plan_rooms(room))return -1;
 xblr_world_route result={0};
 int ox=(px+x)/2-WORLD_PLAN_W/2,oy=(py+y)/2-WORLD_PLAN_W/2,sxi=px-ox,syi=py-oy,goal=-1;
 if(sxi<0||syi<0||sxi>=WORLD_PLAN_W||syi>=WORLD_PLAN_W||world_plan_abs(x-px)>=WORLD_PLAN_W-8||world_plan_abs(y-py)>=WORLD_PLAN_W-8){*out=result;return 0;}
 memset(world_planner.from,0,sizeof world_planner.from);memset(world_planner.blocked,0,sizeof world_planner.blocked);world_planner.ox=ox;world_planner.oy=oy;
 uint32_t *q=world_planner.queue;uint8_t *from=world_planner.from;
 static const int8_t D[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
 int h=0,t=0,best=-1,bestd=1<<30;q[t++]=syi*WORLD_PLAN_W+sxi;from[syi*WORLD_PLAN_W+sxi]=9;
 while(h<t&&h<WORLD_PLAN_CAP&&!world_planner.failed){
  int c=q[h++],cx=c%WORLD_PLAN_W+ox,cy=c/WORLD_PLAN_W+oy;
  if(world_plan_abs(cx-x)<=2&&world_plan_abs(cy-y)<=2){goal=c;break;}
  int ddx=world_plan_abs(cx-x),ddy=world_plan_abs(cy-y),dd=(ddx>ddy?ddx:ddy)*64+ddx+ddy;
  if(dd<bestd){bestd=dd;best=c;}
  for(int d=0;d<8;d++){
   int nx=cx+D[d][0],ny=cy+D[d][1],ix=nx-ox,iy=ny-oy;
   if(ix<0||iy<0||ix>=WORLD_PLAN_W||iy>=WORLD_PLAN_W||from[iy*WORLD_PLAN_W+ix])continue;
   if(!world_plan_free(nx,ny))continue;if(d>=4&&(!world_plan_free(cx+D[d][0],cy)||!world_plan_free(cx,cy+D[d][1])))continue;
   from[iy*WORLD_PLAN_W+ix]=d+1;q[t++]=iy*WORLD_PLAN_W+ix;
  }
 }
 result.expanded=h;if(world_planner.failed)return -1;
 if(goal<0&&h>=t&&best>=0&&type==2){result.nearest=1;if(best!=syi*WORLD_PLAN_W+sxi)goal=best;}
 if(goal>=0){
  int n=0,c=goal;while(from[c]!=9&&n<WORLD_PLAN_W*WORLD_PLAN_W/2){q[WORLD_PLAN_W*WORLD_PLAN_W-1-n++]=c;int d=from[c]-1;c-=D[d][1]*WORLD_PLAN_W+D[d][0];}
  if(from[c]!=9)return -1;
  uint32_t *path=q+WORLD_PLAN_W*WORLD_PLAN_W-n;int ax=px,ay=py,k=0;
  while(k<n&&result.n<24){
   int best=k;for(int m=n-1<k+WORLD_PLAN_SEG?n-1:k+WORLD_PLAN_SEG;m>k;m--)if(world_plan_line(ax,ay,path[m]%WORLD_PLAN_W+ox,path[m]/WORLD_PLAN_W+oy)){best=m;break;}
   ax=path[best]%WORLD_PLAN_W+ox;ay=path[best]/WORLD_PLAN_W+oy;result.node[result.n][0]=ax;result.node[result.n][1]=ay;result.n++;k=best+1;
  }
  if(k<n)result.n=0;
 }
 if(world_planner.failed)return -1;*out=result;return goal>=0?1:0;
}
