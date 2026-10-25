// 储藏箱加宽的物品栏右移：＋ 关储藏箱时原版同一帧关掉又单独打开物品栏，挪到单开位置并把我们挪过的 x 记成它自己的原位，单开的物品栏关掉时写回那个 x。
// 位置守卫写回时控件 x 不是我们上次写的值，不能就此丢掉条目：要等 x 回到我们的值再写回原位，否则每次 ＋ 关储藏箱物品栏都多右移一次（实跑：219 → 446 → 673 → 900 → 1127）。
// Inventory right shift of the widened stash: closing the stash with + makes the original game close the inventory and open it alone in the same frame, moving it to the stand-alone position and recording our shifted x as its own original position, which it writes back when the stand-alone inventory closes.
// When the position guard restores and the widget x is not the value we last wrote, the entry must not be dropped: wait until x is our value again and then restore the original, otherwise every + close of the stash shifts the inventory right once more (real run: 219 -> 446 -> 673 -> 900 -> 1127).
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD]; int g_nmod;
uint64_t dm_guest_frame(void) { return 100; }
uint64_t now_ns(void) { return 0; }
int dm_padmode_busy(void) { return 0; }
int dm_uiscreen_ingame(void) { return 1; }
void dm_ui_rect(int *x,int *y,int *w,int *h) { *x=0; *y=0; *w=2360; *h=1326; }
void dm_uiscreen_set_ingame(int on) { (void)on; }
void dm_custom_on_uimsg(const char *cat,const char *act,const char *arg) { (void)cat; (void)act; (void)arg; }
void dm_elysium_arm(int on) { (void)on; }
static int checks,fails;
#define CHECK(c) do { checks++; if(!(c)) { fails++; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
static _Alignas(8) unsigned char root[160], inv[160];
static uint64_t children[1];
static uint64_t ptr(void *p) { return (uint64_t)(uintptr_t)p; }
static int32_t *x(void) { return (int32_t *)(inv+0x70); }
enum { HOME = -8, SHIFT = 227, ALONE = -816 };   // 实跑的值：原位、右移量（196 × 缩放 1.16）、单开物品栏的 x | Values from the real run: original x, shift (196 × scale 1.16), x of the stand-alone inventory
int main(void) {
 unsigned char *base=calloc(1,0x2f05460); if(!base)return 2;
 g_nmod=1; strcpy(g_mod[0].name,"main"); g_mod[0].base=base;
 *(uint64_t *)(base+0x2f05458)=ptr(root); *(uint64_t *)(root+0x58)=ptr(children); *(uint64_t *)(root+0x60)=1;
 *(const char **)(inv+8)="PlayerInventoryExpansionLayout"; *x()=HOME; inv[0x50]=1; children[0]=ptr(inv);
 // 用 B 关：x 还是我们的值，当场写回 | Closed with B: x is still our value, restored at once
 mv_offset(MV_WIDE,ptr(inv),0,SHIFT,0); CHECK(*x()==HOME+SHIFT);
 mv_restore(ptr(root),MV_WIDE); CHECK(*x()==HOME && mv_count(MV_WIDE)==0);
 for(int k=0;k<4;k++) {
  // 用 ＋ 关：开储藏箱右移，原版接着把物品栏挪到单开位置 | Closed with +: shifted while the stash is open, then the original game moves the inventory to the stand-alone position
  mv_offset(MV_WIDE,ptr(inv),0,SHIFT,0); CHECK(*x()==HOME+SHIFT);
  *x()=ALONE;
  mv_restore(ptr(root),MV_WIDE); CHECK(*x()==ALONE);   // 原版的位置不动 | the original game's position is left alone
  mv_restore(ptr(root),MV_WIDE); CHECK(*x()==ALONE);
  // 单开的物品栏关掉：原版写回它记的 x（我们挪过的值），这时要写回原位 | The stand-alone inventory closes: the original game writes back the x it recorded (our shifted value); now the original must be restored
  *x()=HOME+SHIFT;
  mv_restore(ptr(root),MV_WIDE); CHECK(*x()==HOME && mv_count(MV_WIDE)==0);
 }
 // 等的期间原版重排出别的原位：下次开储藏箱按新原位挪 | While waiting the original game lays out a new original position: the next stash open shifts from it
 mv_offset(MV_WIDE,ptr(inv),0,SHIFT,0); *x()=ALONE; mv_restore(ptr(root),MV_WIDE);
 *x()=40; mv_offset(MV_WIDE,ptr(inv),0,SHIFT,0); CHECK(*x()==40+SHIFT);
 mv_restore(ptr(root),MV_WIDE); CHECK(*x()==40 && mv_count(MV_WIDE)==0);
 free(base); printf("t_widestash_restore: %d checks, %d failures\n",checks,fails); return fails!=0;
}
