/* 技能图标选择算法；表由用户原文件在本地生成。
 * Skill glyph selection algorithm; tables are generated locally from user files. */
static sw_ta_glyph unknown(uint32_t id,const char *reason) {
    return (sw_ta_glyph){SW_TA_UNKNOWN,-1,-1,-1,-1,-1,id,reason};
}
int sw_ta_frame_at(int atlas,int frame,sw_ta_frame *out) {
    if(!out || atlas<0 || atlas>=SW_TA_ATLASES || frame<0)return 0;
    const sw_ta_atlas *a=&sw_ta_atlases[atlas];
    if(frame>=a->count || frame*a->pitch+a->frame_width>a->width)return 0;
    *out=(sw_ta_frame){frame*a->pitch,0,a->frame_width,a->height};return 1;
}
sw_ta_glyph sw_ta_skill(uint32_t id) {
    sw_ta_glyph g=unknown(id,"unknown_skill");
    if(id==UINT32_MAX){g.status=SW_TA_EMPTY;g.reason="empty_slot";return g;}
    for(size_t i=0;i<sizeof rows/sizeof rows[0];i++)if(rows[i].id==id) {
        sw_ta_frame frame;
        if(!sw_ta_frame_at(rows[i].atlas,rows[i].frame,&frame) || !sw_ta_frame_at(rows[i].atlas,rows[i].frame+1,&frame))return g;
        g.status=SW_TA_GLYPH;g.atlas=rows[i].atlas;g.normal_frame=rows[i].frame;g.pressed_frame=rows[i].frame+1;g.reason="official_raw_id";return g;
    }
    return g;
}
sw_ta_glyph sw_ta_current(const sw_tg_snapshot *s,uint64_t button,uint64_t now) { return sw_ta_current_set(s,button,now,0); }
sw_ta_glyph sw_ta_current_set(const sw_tg_snapshot *s,uint64_t button,uint64_t now,int skill_set) {
    sw_ta_glyph g=unknown(UINT32_MAX,"empty_unknown");
    if(!s || !s->supported || !s->valid || !s->ready || s->source!=SW_TG_SOURCE_NATIVE_GETTER ||
       s->identity_scope!=SW_TG_IDENTITY_NATIVE || !s->captured_ns || now<s->captured_ns ||
       now-s->captured_ns>SW_TG_MAX_AGE_NS || (s->selected_bank!=0 && s->selected_bank!=16))return g;
    uint32_t logical=0;
    switch(button){case SW_BTN_A:logical=0x2000;break;case SW_BTN_B:logical=0x1000;break;
        case SW_BTN_X:logical=0x8000;break;case SW_BTN_Y:logical=0x4000;break;case SW_BTN_R:logical=0x200;break;default:return g;}
    int slot=-1;
    for(int i=0;i<SW_TG_FACETS;i++)if(s->mapping[i].logical==logical){if(slot>=0)return g;slot=(int)(skill_set?s->mapping[i].alt:s->mapping[i].normal);}
    if(slot<0 || slot>=(skill_set?12:6))return g;
    g=sw_ta_skill(s->raw_skill_id[slot]);g.slot=slot;g.effective_slot=s->selected_bank+slot;return g;
}
