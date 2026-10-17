/* Safe snapshots for sw_world unit coordinates and collision cells. These
 * offsets come from r37 sw_world.c / unchanged r32 original game structures.
 * Failure blocks a cell or rejects a unit; it never invents a walkable path. */
static int world_field(void *out,uint64_t object,uint64_t offset,size_t bytes) {
    if(!object||object>UINT64_MAX-offset)return 0;
    return xblr_guest_read(out,object+offset,bytes);
}
static int world_unit_xy(uint64_t unit,int *x,int *y) {
    uint32_t type=0;uint64_t path=0;
    if(!world_field(&type,unit,0,4)||type>5||!world_field(&path,unit,0x38,8)||!path)return 0;
    int nx,ny;
    if(type==2||type==4||type==5){int32_t pair[2];if(!world_field(pair,path,0x10,sizeof pair))return 0;nx=pair[0];ny=pair[1];}
    else {uint16_t pair[4];if(!world_field(pair,path,0,sizeof pair))return 0;nx=pair[1];ny=pair[3];}
    *x=nx;*y=ny;return 1;
}
/* Returns success only when this room covers the cell and all metadata can be
 * read. Caller may try another room; exhaustion means blocked (0xffff). */
static __attribute__((unused)) int world_room_cell(uint64_t room,int x,int y,uint16_t *value) {
    int32_t bounds[4];uint64_t grid=0,cells=0;int32_t stride=0;
    if(!world_field(bounds,room,0x80,sizeof bounds)||bounds[2]<=0||bounds[3]<=0)return 0;
    int64_t dx=(int64_t)x-bounds[0],dy=(int64_t)y-bounds[1];
    if(dx<0||dy<0||dx>=bounds[2]||dy>=bounds[3])return 0;
    if(!world_field(&grid,room,0x38,8)||!world_field(&stride,grid,8,4)||stride<bounds[2]||
       !world_field(&cells,grid,0x20,8)||!cells)return 0;
    uint64_t index=(uint64_t)dy*(uint32_t)stride+(uint64_t)dx;
    if(index>UINT64_MAX/2)return 0;
    return world_field(value,cells,index*2,2);
}
