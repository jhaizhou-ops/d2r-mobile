/* r37 sw_cursor name acceptance: confirmed keyboard text is not yet a widget
 * update. Never accept on timeout or an unreadable/mismatched widget. */
static pthread_mutex_t fe_name_lock=PTHREAD_MUTEX_INITIALIZER;
static int fe_name_open,fe_name_done,fe_name_seen;
static uint64_t fe_name_since;
static char fe_name_text[64];
static void fe_name_panel(int open) {
    pthread_mutex_lock(&fe_name_lock);
    fe_name_open=open;fe_name_done=fe_name_seen=0;
    {extern int g_swkbd_name_panel;g_swkbd_name_panel=open;}   /* keyboard: blank name -> random name (hle_swkbd_ohos.c) */
    memset(fe_name_text,0,sizeof fe_name_text);
    pthread_mutex_unlock(&fe_name_lock);
}
void xblr_ui_text_entered(const char *text) {
    pthread_mutex_lock(&fe_name_lock);
    size_t n=0;if(text)while(n<sizeof fe_name_text&&text[n])++n;
    if(fe_name_open&&n&&n<sizeof fe_name_text){memcpy(fe_name_text,text,n+1);fe_name_done=1;fe_name_seen=0;}
    pthread_mutex_unlock(&fe_name_lock);
}
/* -1 waiting, 0 no pending/expired, 1 exact widget acknowledgement. */
static int fe_name_poll(uint64_t base,uint64_t root,uint64_t now) {
    pthread_mutex_lock(&fe_name_lock);
    if(!fe_name_done){pthread_mutex_unlock(&fe_name_lock);return 0;}
    if(!fe_name_seen){fe_name_seen=1;fe_name_since=now;}
    unsigned left=512;uint64_t pn=fe_find(root,"CharacterCreateNamePanel",0,&left);
    uint64_t it=pn?fe_find(pn,"InputText",0,&left):0,vt=0,p=0,n=0;
    size_t wanted=strlen(fe_name_text);char actual[64];int same=0;
    if(it&&xblr_guest_read(&vt,it,8)&&vt==base+0x20b8718&&
       xblr_guest_read(&p,it+0x558,8)&&xblr_guest_read(&n,it+0x560,8)&&n==wanted&&
       xblr_guest_read(actual,p,wanted)){
        same=1;for(size_t i=0;i<wanted;i++)if(actual[i]!=fe_name_text[i])same=0;
    }
    int result=same?1:(now-fe_name_since>2000?0:-1);
    if(result>=0){fe_name_done=fe_name_seen=0;memset(fe_name_text,0,sizeof fe_name_text);}
    pthread_mutex_unlock(&fe_name_lock);return result;
}
