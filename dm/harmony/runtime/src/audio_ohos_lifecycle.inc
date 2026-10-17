/* Included after the original audio queue/renderer helpers. All device and
 * silent-consumer transitions share g_aos_lock with the frontend focus path.
 * sink is the actual per-output consumer; g_suspended is the requested focus.
 */
enum { XBLR_AUDIO_NONE, XBLR_AUDIO_RENDERER, XBLR_AUDIO_SILENT };

static int xblr_audio_silent_start_locked(aout *a) {
    atomic_store_explicit(&a->null_run,1,memory_order_release);
    int e=pthread_create(&a->null_th,NULL,null_sink,a);
    if(e){atomic_store_explicit(&a->null_run,0,memory_order_release);LOG("audio silent consumer start failed: %d",e);return e;}
    a->sink=XBLR_AUDIO_SILENT;return 0;
}
static int xblr_audio_sink_stop_locked(aout *a) {
    int e=0;
    if(a->sink==XBLR_AUDIO_RENDERER) {
        e=(int)OH_AudioRenderer_Pause(a->au);
        if(e) {
            OH_AudioStream_State state=AUDIOSTREAM_STATE_INVALID;
            /* The platform may already have paused the renderer on an audio
             * interruption. Only a confirmed non-running state is accepted. */
            int query=(int)OH_AudioRenderer_GetCurrentState(a->au,&state);
            if(!query&&(state==AUDIOSTREAM_STATE_PAUSED||state==AUDIOSTREAM_STATE_STOPPED))e=0;
            else LOG("OHAudio pause failed: %d query=%d state=%d",e,query,(int)state);
        }
    } else if(a->sink==XBLR_AUDIO_SILENT) {
        atomic_store_explicit(&a->null_run,0,memory_order_release);
        e=pthread_join(a->null_th,NULL);
        if(e)LOG("audio silent consumer join failed: %d",e);
    }
    if(!e)a->sink=XBLR_AUDIO_NONE;
    return e;
}
static int xblr_audio_renderer_start_locked(aout *a) {
    int e=(int)OH_AudioRenderer_Start(a->au);
    if(e)LOG("OHAudio start failed: %d",e);
    else a->sink=XBLR_AUDIO_RENDERER;
    return e;
}
static Result xblr_audio_start(aout *a) {
    pthread_mutex_lock(&g_aos_lock);
    if(atomic_load_explicit(&a->state,memory_order_acquire)==STATE_STARTED) {pthread_mutex_unlock(&g_aos_lock);return RES_OK;}
    atomic_store_explicit(&a->state,STATE_STARTED,memory_order_release);
    int e=a->au&&!g_suspended?xblr_audio_renderer_start_locked(a):xblr_audio_silent_start_locked(a);
    if(e)atomic_store_explicit(&a->state,STATE_STOPPED,memory_order_release);
    pthread_mutex_unlock(&g_aos_lock);
    return e?RES_FAIL:RES_OK;
}
static int xblr_audio_stop(aout *a) {
    pthread_mutex_lock(&g_aos_lock);
    atomic_store_explicit(&a->state,STATE_STOPPED,memory_order_release);
    int e=xblr_audio_sink_stop_locked(a);
    pthread_mutex_unlock(&g_aos_lock);return e;
}
static int xblr_audio_suspend_all(int on) {
    pthread_mutex_lock(&g_aos_lock);
    int changed=g_suspended!=on,failed=0;g_suspended=on;
    for(int i=0;i<AOMAX;++i) {
        aout *a=g_aos[i];
        if(!a||!a->au||atomic_load_explicit(&a->state,memory_order_acquire)!=STATE_STARTED)continue;
        if(on&&a->sink==XBLR_AUDIO_RENDERER) {
            if(xblr_audio_sink_stop_locked(a)){failed=1;continue;}
            if(xblr_audio_silent_start_locked(a)){atomic_store_explicit(&a->state,STATE_STOPPED,memory_order_release);failed=1;}
        } else if(!on&&a->sink==XBLR_AUDIO_SILENT) {
            if(xblr_audio_sink_stop_locked(a)){failed=1;continue;}
            if(xblr_audio_renderer_start_locked(a)) {
                /* Interruption still owns the device: retain the original
                 * explicit realtime silent-consumer policy and retry on the
                 * next foreground notification, independently for each output. */
                failed=1;
                if(xblr_audio_silent_start_locked(a))atomic_store_explicit(&a->state,STATE_STOPPED,memory_order_release);
            }
        }
    }
    if(changed||failed)LOG("audio focus=%s%s",on?"background":"foreground",failed?" transition failed (see OHAudio error)":"");
    pthread_mutex_unlock(&g_aos_lock);return failed;
}
