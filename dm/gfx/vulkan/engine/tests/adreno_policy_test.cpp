// 直接调用生产的 Adreno 机型判断与开关优先级；只做字符串/整数运算，不需要 Vulkan 或 GPU。
// Call the production Adreno family check and switch precedence directly; string and integer work only, no Vulkan or GPU.
#include "../xbg_adreno_policy.h"
#include <stdio.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)

int main(){
    // 设备名解析：只认 "Adreno" 之后的三位数字，认不出来算 0。
    // Device-name parsing: only three digits after "Adreno" count; anything else is 0.
    CHECK(xbg_adreno_model(nullptr)==0);
    CHECK(xbg_adreno_model("")==0);
    CHECK(xbg_adreno_model("Adreno")==0);
    CHECK(xbg_adreno_model("Adreno (TM) 730")==730);
    CHECK(xbg_adreno_model("Adreno (TM) 740")==740);
    CHECK(xbg_adreno_model("Adreno 750")==750);
    CHECK(xbg_adreno_model("adreno 740")==740);
    CHECK(xbg_adreno_model("Adreno (TM) 830")==830);
    CHECK(xbg_adreno_model("Adreno 642L")==642);
    CHECK(xbg_adreno_model("Adreno 740 (v2)")==740);
    CHECK(xbg_adreno_model("Adreno X1-85")==0);
    CHECK(xbg_adreno_model("Adreno 7400")==0);
    CHECK(xbg_adreno_model("Mali-G715")==0);

    // 只有高通 740–799 这一族缺省关掉两个改写。
    // Only Qualcomm's 740-799 family defaults both rewrites off.
    CHECK(xbg_adreno_rewrites_fault(0x5143,"Adreno (TM) 740"));
    CHECK(xbg_adreno_rewrites_fault(0x5143,"Adreno 750"));
    CHECK(xbg_adreno_rewrites_fault(0x5143,"Adreno (TM) 799"));
    CHECK(!xbg_adreno_rewrites_fault(0x5143,"Adreno (TM) 730"));
    CHECK(!xbg_adreno_rewrites_fault(0x5143,"Adreno (TM) 735"));
    CHECK(!xbg_adreno_rewrites_fault(0x5143,"Adreno (TM) 830"));
    CHECK(!xbg_adreno_rewrites_fault(0x5143,nullptr));
    CHECK(!xbg_adreno_rewrites_fault(0x5143,"Adreno X1-85"));
    CHECK(!xbg_adreno_rewrites_fault(0x10002,"Adreno (TM) 740"));
    CHECK(!xbg_adreno_rewrites_fault(0,"Adreno 750"));

    // 优先级：标记文件 > 机型判断 > 环境变量。
    // Precedence: marker file > device family > environment.
    CHECK(xbg_adreno_rewrite_option(true,'1',nullptr,false));
    CHECK(xbg_adreno_rewrite_option(true,'1',"0",false));
    CHECK(xbg_adreno_rewrite_option(true,'1',nullptr,true));
    CHECK(xbg_adreno_rewrite_option(true,'1',"0",true));
    CHECK(!xbg_adreno_rewrite_option(true,'0',"1",false));
    CHECK(!xbg_adreno_rewrite_option(true,'0',"1",true));
    CHECK(xbg_adreno_rewrite_option(true,EOF,"0",false)); // 空标记文件读回 EOF，与其它开关一样当作开 | an empty marker reads EOF, on like the other switches
    CHECK(xbg_adreno_rewrite_option(false,-1,"1",false));
    CHECK(!xbg_adreno_rewrite_option(false,-1,"1",true));
    CHECK(!xbg_adreno_rewrite_option(false,-1,nullptr,true));
    CHECK(!xbg_adreno_rewrite_option(false,-1,"0",false));
    CHECK(!xbg_adreno_rewrite_option(false,-1,nullptr,false));
    CHECK(xbg_adreno_rewrite_option(false,-1,"2",false));

    if(failures){printf("FAIL %d check(s)\n",failures);return 1;}
    printf("PASS Adreno policy: 740/750 default off, 730/830 and other vendors unchanged, marker file still wins\n");
    return 0;
}
