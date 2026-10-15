#include "../host/hle.h"
#include "input_backend.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
static int ready, checks;
static int source_ready(void *opaque) { (void)opaque; return ready; }
extern const hle_force_ent __start_dm_force[], __stop_dm_force[];
static void *binding(const char *name) {
    for (const hle_force_ent *p=__start_dm_force;p<__stop_dm_force;++p)
        if (!strcmp(p->name,name)) return p->fn;
    die("Input probe missing HID binding %s",name);
}
#define CHECK(x) do {++checks;if(!(x))die("Input fixture failed line=%d",__LINE__);}while(0)
typedef struct { int64_t dt; uint32_t attr,id; int32_t x,y,dx,dy,angle,reserved; } point_state;
typedef struct { int64_t sampling; int32_t count,reserved; point_state points[16]; } touch_state;
_Static_assert(sizeof(touch_state)==0x290,"Touch ABI");
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    g_t0_ns=now_ns();
    void (*init_touch)(void)=binding("_ZN2nn3hid21InitializeTouchScreenEv");
    void (*init_npad)(void)=binding("_ZN2nn3hid14InitializeNpadEv");
    int (*read_touch)(void *,int)=binding("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi");
    uint32_t (*get_style)(const uint32_t *)=binding("_ZN2nn3hid15GetNpadStyleSetERKj");
    void (*read_pad)(dm_android_npad_state *,const uint32_t *)=binding("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
    dm_android_input_config config={.source="device-input-fixture",.panel_width=1280,.panel_height=720,.source_ready=source_ready};
    CHECK(dm_android_input_initialize_touch()==ENODEV);
    CHECK(dm_android_input_bind(&config)==ENODEV);
    ready=1;CHECK(!dm_android_input_bind(&config));CHECK(dm_android_input_bind(&config)==EALREADY);
    init_touch();init_npad();uint32_t id=0;CHECK(get_style(&id)==0);
    dm_android_npad_state pad;read_pad(&pad,&id);CHECK(pad.attributes==0&&pad.buttons==0);
    CHECK(dm_android_input_publish_actions(1,0,0,0,0)==ENODEV);
    touch_state s;CHECK(read_touch(NULL,0)==0);CHECK(dm_android_input_read_touch(NULL,1)==-EINVAL);
    CHECK(read_touch(&s,16)==1&&s.count==0);
    dm_android_pointer p[2]={{.id=99,.x=.25f,.y=.5f},{.id=7,.x=.75f,.y=.2f}};
    CHECK(!dm_android_input_publish_touch(p,2));CHECK(read_touch(&s,16)==1&&s.count==2);
    CHECK(s.points[0].id==0&&s.points[0].attr==1&&s.points[0].x==320&&s.points[0].y==360);
    CHECK(s.points[1].id==1&&s.points[1].attr==1&&s.points[1].x==960&&s.points[1].y==144);
    dm_android_pointer reversed[2]={p[1],p[0]};CHECK(!dm_android_input_publish_touch(reversed,2));
    CHECK(read_touch(&s,1)==1&&s.points[0].id==0&&s.points[0].attr==0&&s.points[1].id==1);
    CHECK(!dm_android_input_publish_touch(p+1,1));CHECK(read_touch(&s,1)==1&&s.count==2&&s.points[0].attr==2&&s.points[1].attr==0);
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].id==1);
    dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    // A complete short tap between samples must retain both edges.
    CHECK(!dm_android_input_publish_touch(p,1));CHECK(!dm_android_input_publish_touch(NULL,0));
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==1);
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    // Reused pointer ID with a second complete tap before the first sample.
    CHECK(!dm_android_input_publish_touch(p,1));CHECK(!dm_android_input_publish_touch(NULL,0));
    p[0].x=.5f;CHECK(!dm_android_input_publish_touch(p,1));CHECK(!dm_android_input_publish_touch(NULL,0));
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==1&&s.points[0].x==320);
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==2&&s.points[0].x==320);
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==1&&s.points[0].x==640);
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==2&&s.points[0].x==640);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    CHECK(!dm_android_input_publish_touch(p,1));dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==0);
    p[0].x=-2;p[0].y=2;CHECK(!dm_android_input_publish_touch(p,1));
    CHECK(read_touch(&s,1)==1&&s.points[0].x==0&&s.points[0].y==719);
    ready=0;CHECK(dm_android_input_publish_touch(p,1)==EAGAIN);
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);CHECK(read_touch(&s,1)==1&&s.count==0);
    ready=1;p[0].x=NAN;CHECK(dm_android_input_publish_touch(p,1)==EINVAL);
    p[0]=p[1];CHECK(dm_android_input_publish_touch(p,2)==EINVAL);
    CHECK(dm_android_input_publish_touch(p,17)==EINVAL);
    CHECK(dm_android_input_set_handheld_mode(3)==EINVAL);
    dm_android_pointer all[16];for(unsigned i=0;i<16;++i)all[i]=(dm_android_pointer){.id=1000+i,.x=1,.y=0};
    CHECK(!dm_android_input_publish_touch(all,16));CHECK(read_touch(&s,1)==1&&s.count==16);
    for(unsigned i=0;i<16;++i)CHECK(s.points[i].id==i&&s.points[i].attr==1&&s.points[i].x==1279&&s.points[i].y==0);
    dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==16);CHECK(read_touch(&s,1)==1&&s.count==0);
    FILE *out=fopen(argv[1],"w");CHECK(out);
    fprintf(out,"{\"passed\":true,\"mode\":\"input-fixture\",\"checks\":%d,\"failures\":0,\"sdk_symbol_host_bindings_used\":true,\"original_sdk_hid_body_executed\":false,\"motion_event_injection_checked\":false,\"game_3d_frame_verified\":false}\n",checks);
    CHECK(!fclose(out));LOG("ANDROID_INPUT_FIXTURE passed checks=%d",checks);return 0;
}
