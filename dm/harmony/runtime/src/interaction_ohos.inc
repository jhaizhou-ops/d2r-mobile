static pthread_mutex_t interaction_lock=PTHREAD_MUTEX_INITIALIZER;
static xblr_interaction_snapshot interaction={.abi_version=1,.struct_bytes=sizeof(xblr_interaction_snapshot),
    .capabilities=XBLR_INTERACTION_INPUT|XBLR_INTERACTION_UI_TAP|XBLR_INTERACTION_CANCEL};
_Static_assert(sizeof(xblr_interaction_snapshot)==96,"interaction ABI1 size");
/* Same panel set and navigation values as original sw_tc_navigation/front_ios.
 * This is runtime observation state, not a second gesture implementation. */
static _Atomic uint32_t interaction_panels;
static _Atomic int interaction_loading=-1;
/* Getter traffic is not a presentation clock. Cache tree reads per frame;
 * relevant UI messages invalidate even in that same frame. */
static _Atomic uint64_t interaction_revision;
static void xblr_interaction_observe(const uint64_t m[3]) {
    if(m[0]==ui_hash("FrontEndNavigation")&&m[1]==ui_hash("ToFrontEnd")){
        atomic_store(&interaction_panels,0);atomic_store(&interaction_loading,1);atomic_fetch_add(&interaction_revision,1);return;
    }
    if(m[0]!=ui_hash("PanelManager"))return;
    if(m[1]==ui_hash("UnloadAll")){atomic_store(&interaction_panels,0);atomic_fetch_add(&interaction_revision,1);return;}
    int open=m[1]==ui_hash("OpenPanel")||m[1]==ui_hash("OpenExclusivePanel");
    int close=m[1]==ui_hash("ClosePanel")||m[1]==ui_hash("UnloadPanel");
    if(!open&&!close)return;
    char name[64];if(!xblr_guest_string(name,sizeof name,m[2]))return;
    atomic_fetch_add(&interaction_revision,1);
    if(!strcmp(name,"LoadScreenPanel"))atomic_store(&interaction_loading,open);
    static const char *names[]={"PlayerInventoryExpansionLayout","SkillsTreePanel","PauseLayout","PauseLayoutGarden","CharacterStatsPanel","QuestLogPanelExpansion","BankExpansionLayout","HoradricCubeLayout","WaypointsPanelExpansion","NpcDialogPanel","NpcTradePanel","ButtonBindingModal","SkillSelect","YouDiedModal"};
    for(unsigned i=0;i<14;i++)if(!strcmp(name,names[i])){
        if(open)atomic_fetch_or(&interaction_panels,1u<<i);else atomic_fetch_and(&interaction_panels,~(1u<<i));break;
    }
}
/* ui_panel uses root direct children in r37. A failed/incomplete read cannot
 * become a negative HUD observation; never retain guest pointers across frames. */
static int interaction_scene(uint64_t base,int *game) {
    uint64_t root=0,ch=0,n=0;
    if(!xblr_guest_read(&root,base+0x2f05458,8)||!root||root>UINT64_MAX-0x68||
       !xblr_guest_read(&ch,root+0x58,8)||!xblr_guest_read(&n,root+0x60,8)||n>256||(!ch&&n)||ch>UINT64_MAX-n*8)return 0;
    int hud=0,load=0;
    for(uint64_t i=0;i<n;i++){
        uint64_t p=0,nm=0;char name[64];
        if(!xblr_guest_read(&p,ch+i*8,8))return 0;if(!p)continue;
        if(p>UINT64_MAX-16||!xblr_guest_read(&nm,p+8,8))return 0;if(!nm)continue;
        if(!xblr_guest_string(name,sizeof name,nm))return 0;
        if(!strcmp(name,"HUDPanel"))hud=1;if(!strcmp(name,"LoadScreenPanel"))load=1;
    }
    int loading=atomic_load(&interaction_loading);
    /* Original fallback searches nested load panels. Until a real load message
     * is observed, root-only absence cannot prove that loading has ended. */
    if(hud&&loading<0&&!load)return 0;
    *game=hud&&!(loading<0?load:loading);return 1;
}
int xblr_get_interaction_snapshot(xblr_interaction_snapshot *out,size_t bytes) {
    if(!out||bytes!=sizeof *out)return XBLR_EXEC_ARGUMENT;
    if(pthread_mutex_trylock(&interaction_lock))return XBLR_EXEC_BUSY;
    *out=interaction;pthread_mutex_unlock(&interaction_lock);return 0;
}
static void xblr_publish_interaction_force(int force) {
    xblr_guest_thread *t=xblr_current_thread();if(!t||!t->is_main)return;
    uint32_t w=0,h=0;int game=0,scene_valid=0;uint64_t frame=atomic_load_explicit(&frames,memory_order_relaxed);
    static uint64_t last_frame,last_revision;static int published;
    uint64_t revision=atomic_load(&interaction_revision);
    if(!force&&published&&last_frame==frame&&last_revision==revision)return;
    published=1;last_frame=frame;last_revision=revision;
    for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")&&g_mod[i].size>=0x2f05460){
        uint64_t base=(uintptr_t)g_mod[i].base;
        if(!xblr_guest_read(&w,base+0x211da38,4)||!xblr_guest_read(&h,base+0x211da3c,4))w=h=0;
        if(w&&h)scene_valid=interaction_scene(base,&game);
        break;
    }
    pthread_mutex_lock(&interaction_lock);
    interaction.generation++;interaction.present_frame=frame;
    interaction.capabilities=XBLR_INTERACTION_INPUT|XBLR_INTERACTION_UI_TAP|XBLR_INTERACTION_CANCEL;
    interaction.valid=0;interaction.viewport_width=interaction.viewport_height=0;
    interaction.ui_x=interaction.ui_y=interaction.ui_width=interaction.ui_height=0;
    interaction.mode=interaction.navigation=interaction.enabled_targets=0;
    if(w&&h&&w<=16384&&h<=16384){
        interaction.valid|=XBLR_INTERACTION_VIEWPORT|XBLR_INTERACTION_UI_RECT;
        interaction.viewport_width=w;interaction.viewport_height=h;
        /* Harmony currently has no sw_uiscreen height override: identity rect. */
        interaction.ui_width=interaction.ui_height=1;
        if(scene_valid){uint32_t panels=atomic_load(&interaction_panels);
            interaction.valid|=XBLR_INTERACTION_MODE|XBLR_INTERACTION_TARGETS;
            interaction.mode=game?XBLR_INTERACTION_GAME:XBLR_INTERACTION_FRONTEND;
            interaction.navigation=!game?1:panels&(1u<<13)?2:panels==(1u<<9)?3:panels?1:0;
            if(game&&!interaction.navigation&&atomic_load_explicit(&world_tick_game,memory_order_acquire))interaction.capabilities|=XBLR_INTERACTION_WORLD_TAP;
            interaction.enabled_targets=interaction.navigation?(interaction.navigation==3?1u<<10:0):((1u<<12)-1)&~(1u<<11);
        }
    }
    pthread_mutex_unlock(&interaction_lock);
}
static void xblr_publish_interaction(void){xblr_publish_interaction_force(0);}
