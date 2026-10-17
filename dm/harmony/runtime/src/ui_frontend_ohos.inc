/* Selected original r32 sw_cursor front-end routing, cbe01e510079.
 * Observe original UI messages; do not replace game transition conditions.
 * Scripted input retains precedence through SW_KEYS / SW_FE_AUTO. */
enum {FE_MENU=1,FE_MODAL=2,FE_TUTORIAL=4,FE_NAME=8,FE_OPTIONS=16,FE_SELECT=32};
/* Claude (iOS 2ebf4e8 pad-mode rules): never re-open the character list behind the player (that undid every selection:
 * "press A several times"). Picking a character in the list (CharacterSelect:SelectCharacter collapses it) is followed by
 * one A on the main menu = Play, so one tap enters the game. No character -> straight to offline creation. */
static _Atomic uint64_t fe_select_frame;
static void fe_name_panel(int open);
static _Atomic unsigned fe_pending;
static _Atomic int fe_ribbon,fe_confirm;
void xblr_ui_observe(uint64_t message) {
    if(!message)return;
    uint64_t m[3];if(!xblr_guest_read(m,message,sizeof m))return;
    xblr_interaction_observe(m);
    if(m[0]==ui_hash("FrontEndNavigation")) {
        if(m[1]==ui_hash("ToGame")||m[1]==ui_hash("ToCharacterCreate"))atomic_store(&fe_ribbon,0);
        return;
    }
    if(m[0]==ui_hash("CharacterSelect")&&m[1]==ui_hash("SelectCharacter")){atomic_fetch_or(&fe_pending,FE_SELECT);atomic_store(&fe_select_frame,0);return;}
    if(m[0]==ui_hash("ConsoleCharacterCreate")&&m[1]==ui_hash("ToggleCharacterOptions")){
        char arg[4];if(xblr_guest_string(arg,sizeof arg,m[2])&&!strcmp(arg,"1"))atomic_fetch_or(&fe_pending,FE_OPTIONS);return;
    }
    if(m[0]!=ui_hash("PanelManager"))return;
    if(m[1]==ui_hash("UnloadAll")){atomic_store(&fe_cc_open,0);atomic_store(&fe_opt_open,0);atomic_store(&fe_modal_open,0);atomic_store(&fe_ribbon,0);atomic_store(&fe_confirm,0);fe_name_panel(0);atomic_fetch_and(&fe_pending,~FE_NAME);return;}
    if(!m[2])return;
    char name[64];if(!xblr_guest_string(name,sizeof name,m[2]))return;
    int opened=m[1]==ui_hash("OpenPanel");
    int closed=m[1]==ui_hash("ClosePanel")||m[1]==ui_hash("UnloadPanel");
    if(!opened&&!closed)return;
    if(!strcmp(name,"CharacterCreateNamePanel")){fe_name_panel(opened);if(opened)atomic_fetch_or(&fe_pending,FE_NAME);else atomic_fetch_and(&fe_pending,~FE_NAME);}
    if(!strcmp(name,"ConfirmationModal")){atomic_store(&fe_confirm,opened);atomic_store(&fe_modal_open,opened);}
    if(!strcmp(name,"CharacterCreatePanel"))atomic_store(&fe_cc_open,opened);
    if(!strcmp(name,"CharacterOptionsPanel")){atomic_store(&fe_opt_open,opened);if(opened)atomic_store(&fe_opt_side,0);}
    if(closed&&!strcmp(name,"MainMenuPanel"))atomic_store(&fe_ribbon,0);
    if(!opened)return;
    if(!strcmp(name,"MainMenuButtonRibbon")){atomic_store(&fe_ribbon,1);atomic_fetch_or(&fe_pending,FE_MENU);}
    else if(!strcmp(name,"OnlinePlayModeModal"))atomic_fetch_or(&fe_pending,FE_MODAL);
    else if(!strcmp(name,"TutorialTooltip"))atomic_fetch_or(&fe_pending,FE_TUTORIAL);
}
static int fe_has_character(const char *dir,int depth) {
    DIR *d=opendir(dir);if(!d)return 0;struct dirent *e;int found=0;
    while(!found&&(e=readdir(d))) {
        if(e->d_name[0]=='.')continue;
        char p[1024];int n=snprintf(p,sizeof p,"%s/%s",dir,e->d_name);if(n<0||(size_t)n>=sizeof p)continue;
        size_t l=strlen(e->d_name);if(l>4&&!strcmp(e->d_name+l-4,".d2s"))found=1;
        else if(depth<3){struct stat st;if(!stat(p,&st)&&S_ISDIR(st.st_mode))found=fe_has_character(p,depth+1);}
    }
    closedir(d);return found;
}
static uint64_t fe_find(uint64_t w,const char *name,unsigned depth,unsigned *left) {
    if(!w||depth>6||!*left)return 0;--*left;
    uint64_t nm,ch,n;char label[64];
    if(!xblr_guest_read(&nm,w+8,8)||!xblr_guest_read(&ch,w+0x58,8)||!xblr_guest_read(&n,w+0x60,8)){*left=0;return 0;}
    if(nm&&xblr_guest_string(label,sizeof label,nm)&&!strcmp(label,name))return w;
    if(!ch||n>256)return 0;
    for(uint64_t i=0;i<n&&*left;i++){uint64_t child;if(!xblr_guest_read(&child,ch+i*8,8)){*left=0;return 0;}uint64_t r=fe_find(child,name,depth+1,left);if(r)return r;}
    return 0;
}
#include "ui_name_ohos.inc"
static void fe_send_offline(uint64_t root) {
    char payload[64]="1";uint64_t msg[8]={0},vt,fn;
    msg[0]=ui_hash("FrontEndNavigation");msg[1]=ui_hash("ToCharacterCreate");msg[2]=(uintptr_t)payload;msg[3]=msg[4]=1;
    if(!xblr_guest_read(&vt,root,8)||!xblr_guest_read(&fn,vt+0x28,8)||!fn)return;
    uint64_t args[2]={root,(uintptr_t)msg};xblr_invoke_guest((void*)fn,args,2);
}
uint64_t xblr_pump_ui_frontend(void) {
    xblr_guest_thread *t=xblr_current_thread();if(!t||!t->is_main)return 0;
    xblr_publish_interaction();
    static int enabled=-1;static uint64_t next,until,mask,last=UINT64_MAX;
    if(enabled<0){const char *v=getenv("SW_FE_AUTO");enabled=v?atoi(v)!=0:!getenv("SW_KEYS");}
    if(!enabled)return 0;
    uint64_t fr=atomic_load_explicit(&frames,memory_order_relaxed);
    if(fr==last||fr<next)return fr<until?mask:0;last=fr;
    module_t *m=NULL;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){m=&g_mod[i];break;}
    uint64_t root=0;if(m&&m->size>=0x2f05460)xblr_guest_read(&root,(uintptr_t)m->base+0x2f05458,8);
    if(!root)return fr<until?mask:0;
    {uint64_t bk=atomic_exchange(&fe_bind_key,0);if(bk){mask=bk;until=fr+4;next=fr+10;return mask;}}
    unsigned pending=atomic_load(&fe_pending);
    if(pending&FE_TUTORIAL){atomic_fetch_and(&fe_pending,~FE_TUTORIAL);mask=2;until=fr+3;next=fr+8;return mask;}
    if(atomic_exchange(&fe_opt_back,0)){mask=2;until=fr+3;next=fr+8;LOG("前端路径：模式页点了牌子以外 → 返回（B）");return mask;}
    /* Mode page card tapped (iOS): the original accept reads the expert card's toggle ([panel+0x211] && [[panel+0x1e0]+0xa88]).
     * Accept (A) only when it matches the card the player tapped; otherwise click the expert card once more (hover, down, up)
     * to flip it, at most twice, never accepting the wrong mode. */
    {static int st,tries,ck;static uint64_t at;
     if((pending&FE_OPTIONS)&&!st){atomic_fetch_and(&fe_pending,~FE_OPTIONS);st=1;tries=0;ck=0;at=fr+4;}
     if(st&&fr>=at){unsigned left=8192;uint64_t pn=ui_find_w(root,"CharacterOptionsPanel",0,&left),hw=0;uint8_t used=0,exp=0;
        if(pn){xblr_guest_read(&hw,pn+0x1e0,8);xblr_guest_read(&used,pn+0x211,1);if(hw)xblr_guest_read(&exp,hw+0xa88,1);}
        if(!pn||!atomic_load(&fe_opt_open)||atomic_load(&fe_modal_open))st=0;
        else if(ck){int32_t vw=0,vh=0;memcpy(&vw,m->base+0x211da38,4);memcpy(&vh,m->base+0x211da3c,4);int x=vw/2+908*vh/2160,y=1079*vh/2160;
            ui_mouse_send(m,root,"MouseMove",x,y);if(ck==2)ui_mouse_send(m,root,"LeftMouseButtonDown",x,y);else if(ck==3)ui_mouse_send(m,root,"LeftMouseButtonUp",x,y);
            if(++ck>3){ck=0;at=fr+4;}}
        else {int want=atomic_load(&fe_opt_side),cur=used&&exp;
            if(cur==want){st=0;mask=1;until=fr+3;next=fr+8;LOG("前端路径：模式页点了%s牌（原版状态一致）→ 接受（A）",want?"专家":"标准");return mask;}
            if(tries++<2){ck=1;LOG("前端路径：模式页点了%s牌，原版仍是%s → 再点一下专家牌",want?"专家":"标准",cur?"专家":"标准");}
            else {st=0;LOG("前端路径：模式页状态翻两次仍不一致，不替玩家接受");}}}
     if(st)return fr<until?mask:0;}
    if(pending&FE_NAME){atomic_fetch_and(&fe_pending,~FE_NAME);mask=1;until=fr+3;next=fr+8;return mask;}
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    int name=fe_name_poll((uintptr_t)m->base,root,(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000);
    if(name){if(name>0){mask=1ull<<10;until=fr+3;next=fr+8;}return fr<until?mask:0;}
    if(pending&FE_MODAL){atomic_fetch_and(&fe_pending,~(FE_MODAL|FE_MENU));fe_send_offline(root);next=fr+8;return 0;}
    if(pending&FE_MENU){
        atomic_fetch_and(&fe_pending,~FE_MENU);const char *save=getenv("SW_SAVE");
        if(save&&!fe_has_character(save,0))fe_send_offline(root);else {mask=4;until=fr+3;}
        next=fr+8;return fr<until?mask:0;
    }
    if(pending&FE_SELECT){
        /* Wait until the list has collapsed back to the main menu ribbon, then Play (A). Give up after ~3 s. */
        uint64_t f0=atomic_load(&fe_select_frame);if(!f0){atomic_store(&fe_select_frame,fr);f0=fr;}
        if(fr-f0>180){atomic_fetch_and(&fe_pending,~FE_SELECT);return fr<until?mask:0;}
        if(fr-f0>=10&&atomic_load(&fe_ribbon)&&!atomic_load(&fe_confirm)){
            unsigned left=512;uint64_t list=fe_find(root,"CharacterSelectPanel",0,&left);uint8_t visible=1;
            if(list&&!xblr_guest_read(&visible,list+0x50,1))visible=1;
            if(left&&(!list||!visible)){atomic_fetch_and(&fe_pending,~FE_SELECT);mask=1;until=fr+3;next=fr+30;LOG("前端路径：选定角色 → 开始游戏（A）");}
        }
        return fr<until?mask:0;
    }
    if(atomic_load(&fe_ribbon)&&!atomic_load(&fe_confirm)) {
        /* Deleted the last character: go straight to offline creation (only re-routing kept from the old list re-open). */
        static uint64_t bare_frames;const char *save=getenv("SW_SAVE");
        if(save&&!fe_has_character(save,0)){if(!bare_frames)bare_frames=fr;else if(fr-bare_frames>=20){bare_frames=0;atomic_store(&fe_ribbon,0);fe_send_offline(root);next=fr+30;}}
        else bare_frames=0;
    }
    return fr<until?mask:0;
}
