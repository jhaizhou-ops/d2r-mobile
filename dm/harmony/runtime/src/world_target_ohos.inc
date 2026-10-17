/* r37 sw_world w_hit, using unchanged r32 game ABI via Harmony's guest gate.
 * Main thread only. All host-side object walks are copied and bounded; a
 * malformed/incomplete walk rejects the request instead of choosing a partial
 * best target. This picker does not issue movement or interaction commands. */
typedef struct {uint32_t type,id;uint64_t unit;} xblr_world_target;
static uint64_t world_call(module_t *m,uint32_t off,const uint64_t *args,unsigned count) {
    return xblr_invoke_guest(m->base+off,args,count);
}
/* Validate the original lookup prefix before entering its unbounded guest
 * chain walk. -1 means unsafe, 0 absent, 1 found. */
static int world_find(module_t *m,uint32_t type,uint32_t id,uint64_t *out) {
    if(type>5)return -1;
    uint64_t bucket=(uintptr_t)m->base+0x2d603a0+type*0x400,unit;
    if(!world_field(&unit,bucket,(id&127)*8,8))return -1;
    unsigned remaining=4096;
    while(unit){
        uint32_t fields[3];uint64_t next;
        if(!remaining--||!world_field(fields,unit,0,sizeof fields))return -1;
        if(fields[0]==type&&fields[2]==id){
            uint64_t a[4]={bucket,id&127,id,type};
            uint64_t found=world_call(m,0x45f5f0,a,4);
            if(found!=unit)return -1;*out=found;return 1;
        }
        if(!world_field(&next,unit,0x158,8))return -1;unit=next;
    }
    *out=0;return 0;
}
static int world_project(module_t *m,uint64_t scene,uint64_t unit,float out[2],int top) {
    uint64_t a[4]={scene,unit,(uintptr_t)out,(unsigned)top};out[0]=out[1]=0;
    return world_call(m,0x90d110,a,4)&&isfinite(out[0])&&isfinite(out[1]);
}
/* 1 box, 0 legitimately absent, -1 unreadable/incomplete. */
static int world_sprite_box(module_t *m,uint64_t unit,int32_t out[4]) {
    uint64_t table=0,array=0,count=0,state=0;uint32_t fields[4],mode;
    if(!world_field(fields,unit,0,sizeof fields)||!world_field(&table,unit,0x78,8))return -1;
    if(!table)return 0;
    if(!world_field(&array,table,0x28,8)||!world_field(&count,table,0x30,8)||count>4096||(count&&!array))return -1;
    if(!count)return 0;
    mode=fields[3];if(fields[0]<=1){if(!world_field(&state,unit,0x40,8))return -1;if(state&&!world_field(&mode,unit,0x58,4))return -1;}
    uint64_t a[6]={unit,(uintptr_t)&fields[0],(uintptr_t)&fields[1],(uintptr_t)&mode,(uintptr_t)m->base+0x1d5a900,0};
    world_call(m,0x5b23f0,a,6);
    for(uint64_t i=0;i<count;i++){
        uint64_t entry[2],r=0;if(!world_field(entry,array,i*16,sizeof entry))return -1;
        if((uint32_t)entry[0]!=mode)continue;
        if(!entry[1])return 0;
        if(!world_field(&r,entry[1],0x18,8))return -1;
        if(!r)return 0;
        if(!world_field(out,r,8,16))return -1;
        return out[1]>out[0]&&out[3]>out[2]&&(int64_t)out[1]-out[0]<2000&&(int64_t)out[3]-out[2]<2000;
    }
    return 0;
}
/* 1 candidate, 0 not projectable, -1 incomplete guest snapshot. */
static int world_candidate(module_t *m,uint64_t scene,uint64_t unit,unsigned type,float minimum,xblr_world_candidate *c) {
    float foot[2],head[2];if(!world_project(m,scene,unit,foot,0))return 0;
    if(type==4||!world_project(m,scene,unit,head,1)){head[0]=foot[0];head[1]=foot[1];}
    memset(c,0,sizeof *c);c->identity=unit;c->foot_x=foot[0];c->foot_y=foot[1];
    c->height=fabsf(foot[1]-head[1]);c->width=c->height*(type==1?.44f:1.0f);
    c->cx=(foot[0]+head[0])/2;c->cy=(foot[1]+head[1])/2;
    if(type==0){c->cx=foot[0];c->cy=foot[1];c->width=c->height;c->height=0;}
    if(type!=2)return 1;
    uint64_t data=0,record=0;if(!world_field(&data,unit,0x10,8))return -1;
    if(!data)return 1;if(!world_field(&record,data,0,8))return -1;if(!record)return 1;
    uint32_t size[2];int32_t box[4];float scale=minimum/.10f*.00206f;
    if(!world_field(size,record,0x90,8)||!world_field(box,record,0x14c,16))return -1;
    if(size[0]>=1&&size[0]<=32&&size[1]>=1&&size[1]<=32){float n=(size[0]+size[1])/2.0f+2;c->diamond_half_width=n*16*scale;c->diamond_half_height=n*8*scale;}
    if(box[2]>0&&box[3]>0&&box[2]<=400&&box[3]<=400&&box[0]>=-400&&box[0]<=400&&box[1]>=-400&&box[1]<=400){
        c->selection_width=box[2]*scale;c->selection_height=box[3]*scale;
        c->selection_x=foot[0]+box[0]*scale+c->selection_width/2;c->selection_y=foot[1]+box[1]*scale+c->selection_height/2;
    }else {
        int sprite=world_sprite_box(m,unit,box);if(sprite<0)return -1;
        if(sprite){
        int64_t l=(int64_t)box[0]-16,r=(int64_t)box[1]+16,top=(int64_t)box[2]-16,bottom=(int64_t)box[3]+16;
        if(l<-40)l=-40;if(r>40)r=40;if(top<-110)top=-110;
        if(r>l&&bottom>top){c->selection_width=(r-l)*scale;c->selection_height=(bottom-top)*scale;c->selection_x=foot[0]+(l+r)/2.0f*scale;c->selection_y=foot[1]+(top+bottom)/2.0f*scale;}
        }
    }
    return 1;
}
/* 1 completed (out.unit==0 means empty ground); 0 unsafe/not ready. */
static int world_pick(module_t *m,float sx,float sy,float minimum,xblr_world_target *out) {
    xblr_guest_thread *thread=xblr_current_thread();if(!thread||!thread->is_main||!m||!out||m->size<0x2f0fef0||!isfinite(sx)||!isfinite(sy)||!isfinite(minimum)||minimum<=0)return 0;
    uint64_t base=(uintptr_t)m->base,scene=0,labels[2];uint32_t player_id;
    if(!world_field(&player_id,base,0x2d20e4c,4)||!world_field(&scene,base,0x2e2a698,8)||!scene||!world_field(labels,base,0x2f0fee0,16)||labels[1]>512||(labels[1]&&!labels[0]))return 0;
    uint64_t player_arg=player_id,player=world_call(m,0x45f9d0,&player_arg,1);if(!player)return 0;
    xblr_world_hit best={0};
    for(uint64_t i=0;labels[0]&&i<labels[1];i++){
        int32_t r[5];if(!world_field(r,labels[0],i*0x148,sizeof r))return 0;
        if(r[2]<=0||r[3]<=0||r[4]==-1)continue;
        float d;if(!world_in_rect(sx,sy,r[0]+r[2]/2.0f,r[1]+r[3]/2.0f,r[2],r[3],minimum,&d))continue;
        uint64_t u=0;int found=world_find(m,4,(uint32_t)r[4],&u);uint32_t mode;if(found<0)return 0;if(!found)continue;if(!world_field(&mode,u,0xc,4))return 0;
        if(mode==3&&(!best.identity||d<best.distance_squared))best=(xblr_world_hit){u,d,0};
    }
    unsigned remaining=4096;
    for(unsigned type=0;type<=5;type++){if(type==3)continue;
        for(unsigned bucket=0;bucket<128;bucket++){
            uint64_t unit;if(!world_field(&unit,base,0x2d603a0+type*0x400+bucket*8,8))return 0;
            while(unit){
                if(!remaining--)return 0;uint32_t fields[4];uint64_t next;
                if(!world_field(fields,unit,0,16)||fields[0]!=type||!world_field(&next,unit,0x158,8))return 0;
                uint64_t u=unit;unit=next;if((type==4&&fields[3]!=3)||(type==0&&u==player))continue;
                uint64_t selectable[5]={u,0,0,UINT64_MAX,UINT64_MAX};if(!(world_call(m,0x464a70,selectable,5)&1))continue;
                uint64_t category_args[3]={0x165,player,u};int category=(int)world_call(m,0x2f7b7c,category_args,3);
                if(type==0?category!=9:type==1?category!=5:type==2?category!=2:0)continue;
                xblr_world_candidate c;int candidate=world_candidate(m,scene,u,type,minimum,&c);
                if(candidate<0)return 0;
                if(candidate)world_hit_consider(&best,&c,sx,sy,minimum);
            }
        }
    }
    xblr_world_target result={0};
    if(best.identity){uint32_t f[3];if(!world_field(f,best.identity,0,12))return 0;result=(xblr_world_target){f[0],f[2],best.identity};}
    *out=result;return 1;
}
