/* r32 UI mouse protocol from upstream sw_cursor.c (cbe01e510079).
 * UI callers only publish coordinates. Only the main guest getter dispatches
 * messages, at most once per completed presentation frame. No controller key
 * synthesis, item drag, snapping, or automatic front-end navigation here. */
static pthread_mutex_t ui_touch_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { float x,y;uint32_t phase;uint64_t serial; } ui_touch;
/* Opt-in, at most four taps and twelve records per tap in this process.
 * All receipt state shares ui_touch_lock. No arbitrary memory or text dump. */
static unsigned ui_diag_armed,ui_diag_started,ui_diag_lines,ui_diag_seen;
static uint64_t ui_diag_seq;
int xblr_set_ui_touch_receipt_budget(uint32_t taps) {
    if(taps>4)return XBLR_EXEC_ARGUMENT;
    pthread_mutex_lock(&ui_touch_lock);
    ui_diag_armed=taps;
    if(!taps)ui_diag_lines=12;
    pthread_mutex_unlock(&ui_touch_lock);return 0;
}
static void ui_trace(unsigned once,const char *event,uint64_t frame,uint64_t root,int w,int h,int x,int y) {
    pthread_mutex_lock(&ui_touch_lock);
    if(ui_diag_seq&&ui_diag_lines<12&&(!once||!(ui_diag_seen&once))) {
        ui_diag_seen|=once;++ui_diag_lines;
        xblr_guest_thread *t=xblr_current_thread();
        LOG("ui-receipt seq=%llu record=%u event=%s tid=%ld main=%d frame=%llu phase=%u root=%#llx w=%d h=%d x=%d y=%d",
            (unsigned long long)ui_diag_seq,ui_diag_lines,event,(long)syscall(SYS_gettid),t?t->is_main:-1,
            (unsigned long long)frame,ui_touch.phase,(unsigned long long)root,w,h,x,y);
    }
    pthread_mutex_unlock(&ui_touch_lock);
}
/* phase: 0 idle, 1 hovering, 2 released and queued, 3 cancel requested. */
int xblr_ui_touch(uint32_t phase,float nx,float ny) {
    if(phase>3||!isfinite(nx)||!isfinite(ny)||nx<0||nx>1||ny<0||ny>1)return XBLR_EXEC_ARGUMENT;
    pthread_mutex_lock(&ui_touch_lock);
    int rc=0;
    if(phase==3) {ui_touch.phase=3;++ui_touch.serial;}
    else if(phase==0) {
        if(ui_touch.phase)rc=XBLR_EXEC_BUSY;
        else {ui_touch.x=nx;ui_touch.y=ny;ui_touch.phase=1;++ui_touch.serial;
            ui_diag_seq=0;
            if(ui_diag_armed&&ui_diag_started<4){--ui_diag_armed;ui_diag_seq=++ui_diag_started;ui_diag_lines=0;ui_diag_seen=0;}
        }
    } else if(ui_touch.phase==1) {
        if(ui_touch.x!=nx||ui_touch.y!=ny)++ui_touch.serial;
        ui_touch.x=nx;ui_touch.y=ny;if(phase==2)ui_touch.phase=2;
    } else rc=XBLR_EXEC_BUSY;
    pthread_mutex_unlock(&ui_touch_lock);
    if(!rc&&(phase==0||phase==3))ui_trace(0,phase==0?"enqueue":"cancel-request",atomic_load(&frames),0,0,0,0,0);
    return rc;
}
static uint64_t ui_hash(const char *s) {
    uint64_t h=0xcbf29ce484222325ull;for(;*s;s++)h=(h^(uint8_t)*s)*0x100000001b3ull;return h;
}
static void ui_mouse_send(module_t *m,uint64_t root,const char *action,int x,int y) {
    /* Preserve original payload offsets +4=x,+8=y and UI virtual slot +0x28. */
    uint64_t payload[16]={0},msg[8]={0},vt,fn;
    payload[0]=(uint64_t)(uint32_t)x<<32;payload[1]=(uint32_t)y;
    msg[0]=ui_hash("InputMessage");msg[1]=ui_hash(action);msg[2]=(uintptr_t)payload;
    memcpy(&vt,(void*)root,8);memcpy(&fn,(void*)(vt+0x28),8);
    memcpy(m->base+0x2d128f8,&x,4);memcpy(m->base+0x2d128fc,&y,4);
    uint64_t args[2]={root,(uintptr_t)msg};
    if(strcmp(action,"MouseMove"))ui_trace(0,action,atomic_load(&frames),root,0,0,x,y);
    xblr_invoke_guest((void*)fn,args,2);
    ui_trace(0,!strcmp(action,"MouseMove")?"move-return":!strcmp(action,"LeftMouseButtonDown")?"down-return":"up-return",atomic_load(&frames),root,0,0,x,y);
}
/* Claude: front-end fixes from iOS 2ebf4e8 sw_cursor.c (class page / mode page). Panel state is fed by
 * xblr_ui_observe (ui_frontend_ohos.inc).
 * Class page (HDCharacterSelectorWidget, state +0x13f4: 0 idle / 1 stepping out / 2 out / 3 returning;
 * set-state main+0xc2cf24): while one hero steps out the game ignores clicks on every other hero
 * (global flag main+0x2f143b0). Before the click reaches the game, the hero stepping out and not
 * tapped is sent back (3), and a tapped hero still returning goes straight to idle (0).
 * Mode page: a tap outside the two cards (rows 899-1259 of 2160, the Bliss row excepted) = back (B);
 * a tap on a card records the side for the accept check in the front-end pump. */
static _Atomic int fe_cc_open,fe_opt_open,fe_modal_open,fe_opt_side,fe_opt_back;
static uint64_t ui_find_w(uint64_t w,const char *name,unsigned depth,unsigned *left) {
    if(!w||depth>10||!*left)return 0;--*left;
    uint64_t nm,ch,n;char label[64];
    if(!xblr_guest_read(&nm,w+8,8)||!xblr_guest_read(&ch,w+0x58,8)||!xblr_guest_read(&n,w+0x60,8)){*left=0;return 0;}
    if(nm&&xblr_guest_string(label,sizeof label,nm)&&!strcmp(label,name))return w;
    if(!ch||n>512)return 0;
    for(uint64_t i=0;i<n&&*left;i++){uint64_t c;if(!xblr_guest_read(&c,ch+i*8,8)){*left=0;return 0;}uint64_t r=ui_find_w(c,name,depth+1,left);if(r)return r;}
    return 0;
}
static void ui_cc_fix(module_t *m,uint64_t root,int x,int y) {
    static const char *const cls[8]={"Amazon","Assassin","Necromancer","Barbarian","Paladin","Sorceress","Warlock","Druid"};
    unsigned left=8192;uint64_t pn=ui_find_w(root,"CharacterCreatePanel",0,&left);if(!pn)return;
    uint64_t ws[8]={0},xy=(uint64_t)(uint32_t)x|((uint64_t)(uint32_t)y<<32);int hit=-1;
    for(int i=0;i<8;i++){left=4096;ws[i]=ui_find_w(pn,cls[i],0,&left);
        if(ws[i]&&hit<0){uint64_t vt=0,fn=0;if(xblr_guest_read(&vt,ws[i],8)&&vt&&xblr_guest_read(&fn,vt+0x48,8)&&fn){uint64_t a[2]={ws[i],xy};if(xblr_invoke_guest((void*)fn,a,2)&1)hit=i;}}}
    if(hit<0)return;
    for(int i=0;i<8;i++){int32_t st=0;if(i==hit||!ws[i]||!xblr_guest_read(&st,ws[i]+0x13f4,4)||st!=1)continue;
        uint64_t a[2]={ws[i],3};xblr_invoke_guest(m->base+0xc2cf24,a,2);LOG("前端路径：职业选择页改选 %s → 让出列中的 %s 归队",cls[hit],cls[i]);}
    {int32_t st=0;if(xblr_guest_read(&st,ws[hit]+0x13f4,4)&&st==3){uint64_t a[2]={ws[hit],0};xblr_invoke_guest(m->base+0xc2cf24,a,2);}}
}
/* Skill rebind dialog (iOS 2ebf4e8 ui_bind_modal_tap): the game's ButtonBindingModal only takes pad keys. A tap on a
 * cell (SkillButtonN row 1, AltSkillButtonN row 2; column = cells left of it + 1) is eaten and becomes the key of that
 * column (A, B, Y, X, R; row 2 with ZL); a tap outside the dialog = cancel (MINUS); blank inside = ignored. */
static _Atomic uint64_t fe_bind_key;
static int ui_bd_row(uint64_t nm){char s[32];if(!nm||!xblr_guest_string(s,sizeof s,nm))return 0;return !strncmp(s,"AltSkillButton",14)?2:!strncmp(s,"SkillButton",11)?1:0;}
static int ui_hit(uint64_t w,uint64_t xy){uint64_t vt=0,fn=0;if(!xblr_guest_read(&vt,w,8)||!vt||!xblr_guest_read(&fn,vt+0x48,8)||!fn)return 0;uint64_t a[2]={w,xy};return (int)(xblr_invoke_guest((void*)fn,a,2)&1);}
static int ui_bd_walk(uint64_t w,uint64_t xy,int dep){
    uint8_t vis=0;if(!w||dep>8||!xblr_guest_read(&vis,w+0x50,1)||!vis)return 0;
    uint64_t ch=0,n=0;if(!xblr_guest_read(&ch,w+0x58,8)||!xblr_guest_read(&n,w+0x60,8)||!ch||n>256)return 0;
    for(uint64_t i=0;i<n;i++){uint64_t c=0,nm=0;uint8_t cv=0;if(!xblr_guest_read(&c,ch+8*i,8)||!c||!xblr_guest_read(&cv,c+0x50,1)||!cv)continue;xblr_guest_read(&nm,c+8,8);int row=ui_bd_row(nm);
        if(!row){int r=ui_bd_walk(c,xy,dep+1);if(r)return r;continue;}
        if(!ui_hit(c,xy))continue;
        int col=1;int32_t cx=0;xblr_guest_read(&cx,c+0x70,4);
        for(uint64_t k=0;k<n;k++){uint64_t o=0,onm=0;int32_t ox=0;if(!xblr_guest_read(&o,ch+8*k,8)||!o||o==c)continue;xblr_guest_read(&onm,o+8,8);if(ui_bd_row(onm)==row&&xblr_guest_read(&ox,o+0x70,4)&&ox<cx)col++;}
        return col>=1&&col<=5?col+(row==2?5:0):0;}
    return 0;
}
static int ui_bind_modal_tap(module_t *m,uint64_t root,int x,int y){
    unsigned left=8192;uint64_t md=ui_find_w(root,"ButtonBindingModal",0,&left);uint8_t vis=0;if(!md||!xblr_guest_read(&vis,md+0x50,1)||!vis)return 0;
    uint64_t xy=(uint64_t)(uint32_t)x|((uint64_t)(uint32_t)y<<32);int slot=ui_bd_walk(md,xy,0);
    if(!slot){int32_t vh=0;memcpy(&vh,m->base+0x211da3c,4);static const int oy[8]={-32,32,-45,45,0,0,-32,32},ox[8]={0,0,0,0,-45,45,-45,45};
        for(int k=0;k<8&&!slot&&vh>0;k++){int xx=x+ox[k]*vh/2160,yy=y+oy[k]*vh/2160;if(xx<0||yy<0)continue;slot=ui_bd_walk(md,(uint64_t)(uint32_t)xx|((uint64_t)(uint32_t)yy<<32),0);}}
    if(!slot){left=4096;uint64_t bg=ui_find_w(md,"Background",0,&left);
        if(bg&&!ui_hit(bg,xy)){atomic_store(&fe_bind_key,1ull<<11);LOG("技能绑定：点在绑定框外 → 取消（MINUS）");return 1;}
        return bg?1:0;}
    static const uint64_t k[5]={1ull<<0,1ull<<1,1ull<<3,1ull<<2,1ull<<7};   /* A, B, Y, X, R */
    atomic_store(&fe_bind_key,k[(slot-1)%5]|(slot>5?1ull<<8:0));LOG("技能绑定：绑定框里点了第 %d 排第 %d 列",slot>5?2:1,(slot-1)%5+1);return 1;
}
static void xblr_ui_touch_pump(void) {
    xblr_guest_thread *t=xblr_current_thread();
    if(!t||!t->is_main){ui_trace(1,"non-main-getter",atomic_load(&frames),0,0,0,0,0);return;}
    static uint64_t last=UINT64_MAX,serial,down_root;
    static unsigned hover;static int down_x,down_y;
    uint64_t frame=atomic_load_explicit(&frames,memory_order_relaxed);if(last==frame){ui_trace(2,"same-present",frame,0,0,0,0,0);return;}
    pthread_mutex_lock(&ui_touch_lock);
    uint32_t phase=ui_touch.phase;float nx=ui_touch.x,ny=ui_touch.y;uint64_t seq=ui_touch.serial;
    pthread_mutex_unlock(&ui_touch_lock);
    if(!phase&&!down_root)return;
    module_t *m=NULL;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){m=&g_mod[i];break;}
    if(!m||m->size<0x2f05460){ui_trace(4,"module-unready",frame,0,0,0,0,0);return;}
    uint64_t root=0;int32_t w,h;memcpy(&root,m->base+0x2f05458,8);
    memcpy(&w,m->base+0x211da38,4);memcpy(&h,m->base+0x211da3c,4);
    if(!root||w<=0||h<=0){ui_trace(8,"root-viewport-unready",frame,root,w,h,0,0);return;}
    ui_trace(16,"main-root-ready",frame,root,w,h,0,0);
    last=frame;
    /* An outstanding Down always gets an Up before a new click. A replaced
     * root invalidates the old UI tree; never dereference its saved address. */
    if(down_root) {
        if(root==down_root) {
            /* Match upstream sw_ui_mouse_pump: refresh hover on the release
             * frame before Up; the game may update its hit state each frame. */
            ui_mouse_send(m,root,"MouseMove",down_x,down_y);
            ui_mouse_send(m,root,"LeftMouseButtonUp",down_x,down_y);
        }
        else ui_trace(0,"root-changed-skip-up",frame,root,w,h,0,0);
        down_root=0;hover=0;
        pthread_mutex_lock(&ui_touch_lock);if(ui_touch.serial==seq)ui_touch.phase=0;pthread_mutex_unlock(&ui_touch_lock);
        ui_trace(0,"clear-after-down",frame,root,w,h,0,0);
        return;
    }
    if(phase==3) {hover=0;pthread_mutex_lock(&ui_touch_lock);if(ui_touch.serial==seq)ui_touch.phase=0;pthread_mutex_unlock(&ui_touch_lock);ui_trace(0,"clear-cancel",frame,root,w,h,0,0);return;}
    if(seq!=serial){serial=seq;hover=0;}
    int x=(int)(nx*w),y=(int)(ny*h);if(x>=w)x=w-1;if(y>=h)y=h-1;
    ui_mouse_send(m,root,"MouseMove",x,y);
    if(hover<2){++hover;return;}
    if(phase==2&&atomic_load(&fe_opt_open)&&!atomic_load(&fe_modal_open)) {
        if((ny<899/2160.0f||ny>1259/2160.0f)&&!(ny>1290/2160.0f&&ny<1470/2160.0f)) {
            atomic_store(&fe_opt_back,1);hover=0;pthread_mutex_lock(&ui_touch_lock);if(ui_touch.serial==seq)ui_touch.phase=0;pthread_mutex_unlock(&ui_touch_lock);return;
        }
        if(ny>=899/2160.0f&&ny<=1259/2160.0f)atomic_store(&fe_opt_side,nx>=0.5f);
    }
    if(phase==2&&atomic_load(&fe_cc_open)&&!atomic_load(&fe_opt_open)&&!atomic_load(&fe_modal_open))ui_cc_fix(m,root,x,y);
    if(phase==2&&ui_bind_modal_tap(m,root,x,y)){hover=0;pthread_mutex_lock(&ui_touch_lock);if(ui_touch.serial==seq)ui_touch.phase=0;pthread_mutex_unlock(&ui_touch_lock);return;}
    if(phase==2){ui_mouse_send(m,root,"LeftMouseButtonDown",x,y);down_root=root;down_x=x;down_y=y;}
}
