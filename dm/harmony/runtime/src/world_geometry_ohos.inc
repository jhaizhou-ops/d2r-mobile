/* Pure geometry from r37 sw_world.c w_in_rect/w_hit. Adapter supplies copied
 * guest fields; no guest pointers, movement, or simulated mouse events here.
 * squared distance preserves original sqrt distance ordering. */
typedef struct {
    uint64_t identity;
    float cx,cy,width,height;
    float foot_x,foot_y,diamond_half_width,diamond_half_height;
    float selection_x,selection_y,selection_width,selection_height;
} xblr_world_candidate;
typedef struct {uint64_t identity;float distance_squared;int rank;} xblr_world_hit;
static int world_in_rect(float x,float y,float cx,float cy,float w,float h,float minimum,float *d) {
    if(!isfinite(cx)||!isfinite(cy)||!isfinite(w)||!isfinite(h)||w<0||h<0)return 0;
    if(w<minimum)w=minimum;if(h<minimum)h=minimum;
    float dx=x-cx,dy=y-cy;if(dx<0)dx=-dx;if(dy<0)dy=-dy;
    if(dx>w/2||dy>h/2)return 0;*d=dx*dx+dy*dy;return isfinite(*d);
}
static void world_hit_consider(xblr_world_hit *best,const xblr_world_candidate *c,float x,float y,float minimum) {
    if(!c->identity||!isfinite(x)||!isfinite(y)||!isfinite(minimum)||minimum<=0)return;
    float d=0;int rank=0,in=world_in_rect(x,y,c->cx,c->cy,c->width,c->height,minimum,&d);
    if(!in&&c->diamond_half_width>0&&c->diamond_half_height>0&&
       isfinite(c->diamond_half_width)&&isfinite(c->diamond_half_height)){
        float dx=x-c->foot_x,dy=y-c->foot_y;if(dx<0)dx=-dx;if(dy<0)dy=-dy;
        if(dx/c->diamond_half_width+dy/c->diamond_half_height<=1){d=dx*dx+dy*dy;in=isfinite(d);}
    }
    if(!in&&c->selection_width>0&&c->selection_height>0){
        in=world_in_rect(x,y,c->selection_x,c->selection_y,c->selection_width,c->selection_height,minimum,&d);rank=1;
    }
    if(in&&(!best->identity||rank<best->rank||(rank==best->rank&&d<best->distance_squared))){
        best->identity=c->identity;best->rank=rank;best->distance_squared=d;
    }
}
