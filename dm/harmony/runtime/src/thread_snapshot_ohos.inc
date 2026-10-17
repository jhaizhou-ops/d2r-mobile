/* Included in the generated thread service, after its private registry.
 * Bound traversal to 64 rows; never dereference guest addresses.
 * Copy under the registry lock; host formatting/logging happens after return. */
_Static_assert(sizeof(xblr_thread_observation)==80,"thread observation ABI");
_Static_assert(sizeof(xblr_thread_snapshot)==5144,"thread snapshot ABI");
_Static_assert(offsetof(xblr_thread_snapshot,threads)==24,"thread rows offset");
int xblr_get_thread_snapshot(xblr_thread_snapshot *out,size_t bytes) {
    if(!out||bytes!=sizeof *out)return XBLR_EXEC_ARGUMENT;
    if(pthread_mutex_trylock(&G))return XBLR_EXEC_BUSY;
    memset(out,0,sizeof *out);
    out->abi_version=1;out->struct_bytes=sizeof *out;
    out->monotonic_ns=now_ns();
    thr_t *t=g_all;
    for(;t&&out->count<64;t=t->all) {
        xblr_thread_observation *r=&out->threads[out->count++];
        r->id=t->id;r->entry=t->entry;r->handle=t->handle;
        r->started=t->started;r->exited=t->exited;r->wait_kind=t->wkind;
        /* Sync-object waits do not set waddr: never expose a stale lock address. */
        r->wait_address=t->wkind>=1&&t->wkind<=3?t->waddr:0;
        r->wait_key=t->wkind==2?t->wkey:0;
        r->wait_sequence=t->wkind?t->wseq:0;
        r->svc_count=atomic_load_explicit(&t->nsvc,memory_order_relaxed);
        r->last_lr=atomic_load_explicit(&t->last_lr,memory_order_relaxed);
        r->last_svc=(uint32_t)atomic_load_explicit(&t->last_svc,memory_order_relaxed);
    }
    out->truncated=t!=NULL;
    pthread_mutex_unlock(&G);
    return 0;
}
