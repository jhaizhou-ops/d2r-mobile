// 关闭按钮热区回归：运行生产吸附算法，自造控件及命中函数，不需要游戏资源。
// Close-button target regression: runs production snapping with authored widgets and hit functions, without game assets.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
uint64_t dm_guest_frame(void) { return 1; }
static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
static int hit(uint64_t w, uint64_t xy) {
    int x = (int32_t)xy, y = (int32_t)(xy >> 32);
    return x >= *(int *)(w+0x70) && x <= *(int *)(w+0x70)+*(int *)(w+0x78) && y >= *(int *)(w+0x74) && y <= *(int *)(w+0x74)+*(int *)(w+0x7c);
}
static void widget(unsigned char *w, uint64_t vt, const char *name, int x, int y, int width, int height) {
    *(uint64_t *)w=vt; *(const char **)(w+8)=name; w[0x50]=w[0x51]=1;
    *(int *)(w+0x70)=x; *(int *)(w+0x74)=y; *(int *)(w+0x78)=width; *(int *)(w+0x7c)=height;
}
int main(void) {
    unsigned char *base=calloc(1,0x2f05460); if (!base) return 2;
    g_nmod=1; strcpy(g_mod[0].name,"main"); g_mod[0].base=base;
    uint64_t btn=(uint64_t)(base+0x20b7b00), panel=(uint64_t)(base+0x20bc048);
    *(uint64_t *)(btn+0x48)=*(uint64_t *)(panel+0x48)=(uint64_t)hit;
    _Alignas(8) unsigned char root[128]={0}, p[128]={0}, a[128]={0}, b[128]={0};
    uint64_t pc=(uint64_t)p, children[2]={(uint64_t)a,(uint64_t)b};
    widget(p,panel,"Inventory",100,100,1800,1500); widget(a,btn,"TouchClose",1700,150,30,30); widget(b,btn,"Other",1400,150,30,30);
    *(uint64_t *)(root+0x58)=(uint64_t)&pc; *(uint64_t *)(root+0x60)=1;
    *(uint64_t *)(p+0x58)=(uint64_t)children; *(uint64_t *)(p+0x60)=2;
    *(uint64_t *)(base+0x2f05458)=(uint64_t)root;
    for (int scale=1;scale<=2;scale++) {
        int H=2160/scale; *(int *)(base+0x211da38)=3000/scale; *(int *)(base+0x211da3c)=H;
        widget(p,panel,"Inventory",100/scale,100/scale,1800/scale,1500/scale);
        widget(a,btn,"TouchClose",1700/scale,150/scale,30/scale,30/scale);
        widget(b,btn,"Other",1400/scale,150/scale,30/scale,30/scale);
        int x=1840/scale,y=165/scale; CHECK(ui_snap(&g_mod[0],(uint64_t)root,&x,&y)==2); CHECK(!strcmp(g_click_name,"TouchClose"));
        x=1715/scale; y=290/scale; CHECK(ui_snap(&g_mod[0],(uint64_t)root,&x,&y)==2);
        x=1900/scale;y=165/scale; CHECK(ui_snap(&g_mod[0],(uint64_t)root,&x,&y)==0);
        x=1530/scale;y=165/scale; CHECK(ui_snap(&g_mod[0],(uint64_t)root,&x,&y)==0);
        x=1415/scale;y=165/scale; CHECK(ui_snap(&g_mod[0],(uint64_t)root,&x,&y)==1); CHECK(!strcmp(g_click_name,"Other"));
        a[0x50]=0; x=1840/scale;y=165/scale; CHECK(ui_snap(&g_mod[0],(uint64_t)root,&x,&y)==0); a[0x50]=1;
    }
    free(base); printf("t_close_snap: %d checks, %d failures\n",checks,fails); return !!fails;
}
