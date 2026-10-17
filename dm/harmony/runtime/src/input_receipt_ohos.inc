/* Included by generated hle_hid_ohos.c after NpadState/input_lock.
 * Optional observation only: never changes the state returned to the guest.
 * All access uses the existing input_lock. One fprintf keeps a record whole;
 * it runs only while this explicitly enabled finite diagnostic is active. */
typedef struct {
    uint64_t last_ns;
    NpadState state;
    uint32_t id,style,seen;
} xblr_input_receipt_slot;
static xblr_input_receipt_slot input_receipt_slots[67];
static uint64_t input_receipt_generation;
static uint32_t input_receipt_remaining,input_receipt_records;

int xblr_set_input_receipt_budget(uint32_t record_limit) {
    if(record_limit>256)return XBLR_EXEC_ARGUMENT;
    pthread_mutex_lock(&input_lock);
    ++input_receipt_generation;
    input_receipt_remaining=record_limit;input_receipt_records=0;
    memset(input_receipt_slots,0,sizeof input_receipt_slots);
    pthread_mutex_unlock(&input_lock);
    return 0;
}

/* Called only after fill has produced the exact final guest output, with the
 * input mutex held. Unsupported/disconnected id/style pairs return all zeros
 * and are logged as such, never as a consumed nonzero host button state. */
static void input_receipt_return(const NpadState *s,uint32_t id,uint32_t style) {
    if(!input_receipt_remaining)return;
    unsigned slot=id<=NPAD_HANDHELD&&(style==STYLE_FULLKEY||style==STYLE_HANDHELD)
        ?id*2+(style==STYLE_HANDHELD):66;
    xblr_input_receipt_slot *r=&input_receipt_slots[slot];
    NpadState actual=*s;actual.sampling=0;
    uint64_t ns=now_ns();
    int changed=!r->seen||r->id!=id||r->style!=style||memcmp(&actual,&r->state,sizeof actual);
    int witness=actual.attributes&ATTR_CONNECTED && ns>=r->last_ns && ns-r->last_ns>=1000000000ull;
    if(!changed&&!witness)return;
    r->seen=1;r->id=id;r->style=style;r->state=actual;r->last_ns=ns;
    ++input_receipt_records;--input_receipt_remaining;
    fprintf(stderr,"[xblr] hid-consumed generation=%llu record=%u remaining=%u time_ns=%llu sampling=%lld id=%u style=%u buttons=%#llx lx=%d ly=%d rx=%d ry=%d attributes=%#x\n",
        (unsigned long long)input_receipt_generation,input_receipt_records,input_receipt_remaining,
        (unsigned long long)ns,(long long)s->sampling,id,style,(unsigned long long)s->buttons,
        s->lx,s->ly,s->rx,s->ry,s->attributes);
}
