// 药水拾取白名单回归：调用生产代码的分类与排序，不依赖游戏类型表或资源。
// Potion pickup allowlist regression: calls production classification and ranking without game type tables or assets.
#include "../dm_potpick.c"
#include <stdio.h>
static int checks, fails;
#define CHECK(x) do { checks++; if (!(x)) { fails++; printf("FAIL %d: %s\n",__LINE__,#x); } } while (0)
static uint32_t code(const char *s, unsigned char pad) { uint32_t c=0; memcpy(&c,s,3); ((unsigned char *)&c)[3]=pad; return c; }
int main(void) {
    const char *denied[]={"vps","yps","wms","tsc","isc","tbk","ibk","gps","gpm","gpl","ops","opm","opl","hp0","hp6","mp0","mp6","rvx","r01","cm1","key"};
    for (int p=0;p<2;p++) {
        for (unsigned i=0;i<sizeof denied/sizeof *denied;i++) CHECK(pot_code_rank(code(denied[i],p?' ':0))==0);
        for (int n=1;n<=5;n++) { char h[]="hp1",m[]="mp1"; h[2]=m[2]='0'+n; CHECK(pot_code_rank(code(h,p?' ':0))==n); CHECK(pot_code_rank(code(m,p?' ':0))==n); }
        CHECK(pot_code_rank(code("rvl",p?' ':0))==20); CHECK(pot_code_rank(code("rvs",p?' ':0))==19);
        CHECK(pot_code_rank(code("rvl",0))>pot_code_rank(code("rvs",0))); CHECK(pot_code_rank(code("rvs",0))>pot_code_rank(code("hp5",0)));
    }
    printf("t_potpick_allowlist: %d checks, %d failures\n",checks,fails); return !!fails;
}
