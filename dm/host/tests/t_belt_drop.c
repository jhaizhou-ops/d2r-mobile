// 自造控件回归：展开腰带与装备位重叠时，使用生产判定及落点归一化。
// Authored-widget regression: production overlap selection and point normalization for expanded belt/body-slot overlap.
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #c); } } while (0)
static uint64_t ptr(void *p) { return (uint64_t)(uintptr_t)p; }
static int rect_hit(uint64_t w, uint64_t xy) {
    int x = (int32_t)xy, y = (int32_t)(xy >> 32);
    int *r = (int *)(uintptr_t)(w + 0x70);
    return x >= r[0] && y >= r[1] && x < r[0]+r[2] && y < r[1]+r[3];
}
static uint64_t vt[10];
static void widget(unsigned char *w, const char *name, int x, int y, int width, int height) {
    memset(w, 0, 160); *(uint64_t *)w = ptr(vt); *(const char **)(w+8) = name; w[0x50] = 1;
    int *r = (int *)(w+0x70); r[0]=x; r[1]=y; r[2]=width; r[3]=height;
}
int main(void) {
    _Alignas(8) unsigned char panel[160], belt[160], body[160], wrapper[160], other[160];
    uint64_t children[2], nested; vt[9] = (uint64_t)(uintptr_t)rect_hit;
    widget(panel,"PlayerInventoryExpansionLayout",0,0,0,0);
    widget(wrapper,"Wrapper",0,0,0,0); widget(other,"OtherPanel",0,0,0,0);
    *(uint64_t *)(panel+0x58)=ptr(children); *(uint64_t *)(panel+0x60)=2;
    int sizes[] = {720, 1360, 2160};
    for (int k=0;k<3;k++) {
        int vh=sizes[k], width=(int)(450.0*vh/2160), x0=500, y0=300;
        widget(belt,"Belt",x0,y0,width,width);
        // 模拟命中函数重叠；装备区先遍历到也不得抢走腰带上排。
        // Model overlapping hit functions; body-first traversal must not steal upper belt rows.
        widget(body,"slot_ring",x0,y0,width,width);
        for (int order=0;order<2;order++) {
            children[order]=ptr(body); children[1-order]=ptr(belt);
            for (int row=0;row<4;row++) for (int col=0;col<4;col++) {
                int x=x0+(2*col+1)*width/8, y=y0+(2*row+1)*width/8, px=x, py=y;
                uint64_t got=drop_belt_overlap(ptr(panel),x,y,1,"slot_ring"); CHECK(got==ptr(belt));
                if (got) slot_center(got,&px,&py,3000,vh,1);
                CHECK(px==x && py==y); // 包含第 3 列第 3 格。Includes column 3, row 3.
                CHECK(drop_belt_overlap(ptr(panel),x,y,0,"slot_ring")==0);
                CHECK(drop_belt_overlap(ptr(panel),x,y,1,"grid")==0);
            }
        }
        CHECK(!drop_belt_overlap(ptr(panel),x0-1,y0,1,"slot_ring"));
        CHECK(!drop_belt_overlap(ptr(panel),x0+width,y0,1,"slot_ring"));
        CHECK(!drop_belt_overlap(ptr(other),x0+10,y0+10,1,"slot_ring"));
        belt[0x50]=0; CHECK(!drop_belt_overlap(ptr(panel),x0+10,y0+10,1,"slot_ring")); belt[0x50]=1;
        panel[0x50]=0; CHECK(!drop_belt_overlap(ptr(panel),x0+10,y0+10,1,"slot_ring")); panel[0x50]=1;
        nested=ptr(belt); *(uint64_t *)(wrapper+0x58)=ptr(&nested); *(uint64_t *)(wrapper+0x60)=1;
        children[0]=ptr(wrapper); children[1]=ptr(body); wrapper[0x50]=0;
        CHECK(!drop_belt_overlap(ptr(panel),x0+10,y0+10,1,"slot_ring")); wrapper[0x50]=1;
        CHECK(drop_belt_overlap(ptr(panel),x0+10,y0+10,1,"slot_ring")==ptr(belt));
    }
    CHECK(!drop_belt_overlap(0,0,0,1,"slot_ring")); CHECK(!drop_belt_overlap(ptr(panel),0,0,1,NULL));
    printf("t_belt_drop: %d checks, %d failures\n",checks,failures); return failures!=0;
}
