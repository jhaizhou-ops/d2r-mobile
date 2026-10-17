#pragma once
#include "xblr_interaction.h"

/* Preparation for the full gesture mapper; not enabled in the C036 surface.
 * Unknown observations must release gestures, never inherit the previous scene.
 * Runtime snapshots contain values only; this policy never reads guest memory. */
typedef struct XblTouchContext {
    uint64_t generation;
    uint32_t capabilities,mode,navigation,targets;
    float x,y,width,height;
} XblTouchContext;

static inline int XblTouchContextRead(const xblr_interaction_snapshot *s,
                                     XblTouchContext *out) {
    if(!out)return 0;
#ifdef __cplusplus
    *out=XblTouchContext{};
#else
    *out=(XblTouchContext){0};
#endif
    const uint32_t required=XBLR_INTERACTION_VIEWPORT|XBLR_INTERACTION_UI_RECT|
                            XBLR_INTERACTION_MODE|XBLR_INTERACTION_TARGETS;
    if(!s||s->abi_version!=1||s->struct_bytes!=sizeof *s||
       (s->valid&required)!=required||!s->generation||
       !s->viewport_width||!s->viewport_height||
       s->mode>XBLR_INTERACTION_GAME||s->navigation>3||
       (s->enabled_targets&~((1u<<12)-1))||
       !(s->capabilities&XBLR_INTERACTION_CANCEL))return 0;
    /* Comparisons also reject NaN and infinity. The rect is inside content. */
    if(!(s->ui_x>=0&&s->ui_y>=0&&s->ui_width>0&&s->ui_height>0&&
         s->ui_x+s->ui_width<=1&&s->ui_y+s->ui_height<=1))return 0;
    out->generation=s->generation;out->capabilities=s->capabilities;
    out->mode=s->mode;out->navigation=s->navigation;
    out->targets=(s->capabilities&XBLR_INTERACTION_INPUT)?s->enabled_targets:0;
    out->x=s->ui_x;out->y=s->ui_y;out->width=s->ui_width;out->height=s->ui_height;
    return 1;
}

static inline int XblTouchContextAllows(const XblTouchContext *c,uint32_t capability) {
    return c&&c->generation&&capability&&((c->capabilities&capability)==capability);
}

/* Surface-normalized point -> observed UI rect. Outside points are rejected,
 * rather than clamped onto a nearby actionable button. */
static inline int XblTouchContextPoint(const XblTouchContext *c,float x,float y,
                                      float *ui_x,float *ui_y) {
    if(!c||!c->generation||!ui_x||!ui_y||
       !(x>=c->x&&x<=c->x+c->width&&y>=c->y&&y<=c->y+c->height))return 0;
    *ui_x=(x-c->x)/c->width;*ui_y=(y-c->y)/c->height;return 1;
}
