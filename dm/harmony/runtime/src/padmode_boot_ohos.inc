/* Startup subset of r37 sw_padmode: seven guarded equal-length strings.
 * Harmony RO remains R outside this pre-nnMain transaction. Dynamic mode
 * switching and translated string table replacement are separate dependencies. */
static const struct {uint32_t off;const char *pad,*touch;} padmode_prefixes[]={
 {0x1d21db5,"Controller/Console/Switch/","0touch_console_switch_lay/"},
 {0x1d2c33f,"Controller/Console/","Ztouch_console_lay/"},
 {0x1d3b705,"Controller/","Xtouch_lay/"},
 {0x1d0f5b8,"Controller/ControllerButtons/%s/%s","Ztouch_controller_buttons_ui/%s_%s"},
 {0x1d076c7,"Controller/Cursor/Idle","Xtouch_cursor_lay/Idle"},
 {0x1d3acbc,"Controller/Cursor/Hold","Xtouch_cursor_lay/Hold"},
 {0x1d4d5ea,"Controller/Cursor/Held","Xtouch_cursor_lay/Held"}
};
int xblr_padmode_boot(void) {
    if(!getenv("SW_ROMFS_OVERLAY"))return 0;
    /* Claude: ASTC-only texture packs carry a placeholder ctrl_touch and no
     * touch layouts; switching to touch_* paths would load missing assets. */
    {char p[PATH_MAX],head[16]={0};snprintf(p,sizeof p,"%s/ctrl_touch",getenv("SW_ROMFS_OVERLAY"));
     FILE *f=fopen(p,"rb");if(f){size_t r=fread(head,1,12,f);fclose(f);if(r==12&&!memcmp(head,"astc-overlay",12)){LOG("padmode：ASTC 贴图补充包，无触屏布局，保持手柄布局");return 0;}}}
    module_t *m=NULL;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){m=&g_mod[i];break;}
    if(!m||sysconf(_SC_PAGESIZE)!=4096)return XBLR_EXEC_HOST_FAILURE;
    uintptr_t pages[7];unsigned count=0;
    for(unsigned i=0;i<7;i++){
        const size_t n=strlen(padmode_prefixes[i].pad);char actual[64];uint32_t off=padmode_prefixes[i].off;
        if(n!=strlen(padmode_prefixes[i].touch)||n>=sizeof actual||off>m->size||n+1>m->size-off||
           !xblr_guest_read(actual,(uintptr_t)m->base+off,n+1)||strcmp(actual,padmode_prefixes[i].pad))return XBLR_EXEC_HOST_FAILURE;
        uintptr_t page=((uintptr_t)m->base+off)&~(uintptr_t)4095;
        if((((uintptr_t)m->base+off+n)&~(uintptr_t)4095)!=page)return XBLR_EXEC_HOST_FAILURE;
        unsigned j=0;while(j<count&&pages[j]!=page)j++;if(j==count)pages[count++]=page;
    }
    unsigned opened=0;
    for(;opened<count;opened++)if(mprotect((void*)pages[opened],4096,PROT_READ|PROT_WRITE))break;
    if(opened!=count){while(opened)mprotect((void*)pages[--opened],4096,PROT_READ);return XBLR_EXEC_HOST_FAILURE;}
    for(unsigned i=0;i<7;i++)memcpy(m->base+padmode_prefixes[i].off,padmode_prefixes[i].touch,strlen(padmode_prefixes[i].pad));
    int result=0;for(unsigned i=0;i<count;i++)if(mprotect((void*)pages[i],4096,PROT_READ))result=XBLR_EXEC_HOST_FAILURE;
    return result;
}
