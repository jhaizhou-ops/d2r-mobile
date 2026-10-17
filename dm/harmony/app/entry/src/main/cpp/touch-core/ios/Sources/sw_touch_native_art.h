#pragma once
#include "sw_touch_game_state.h"
enum { SW_TA_UNKNOWN=0, SW_TA_EMPTY=1, SW_TA_GLYPH=2, SW_TA_ATLASES=9 };
typedef struct { const char *asset; int width,height,frame_width,pitch,count; } sw_ta_atlas;
typedef struct { int x,y,width,height; } sw_ta_frame;
typedef struct { int status,atlas,normal_frame,pressed_frame,slot,effective_slot; uint32_t raw_skill_id; const char *reason; } sw_ta_glyph;
extern const sw_ta_atlas sw_ta_atlases[SW_TA_ATLASES];
extern const char sw_ta_manifest_sha[],sw_ta_geometry_sha[],sw_ta_binding_sha[];
sw_ta_glyph sw_ta_skill(uint32_t raw_skill_id);
sw_ta_glyph sw_ta_current(const sw_tg_snapshot *s,uint64_t sdk_button,uint64_t host_now_ns);
/* 同上，skill_set=1 取第二套（alt 槽，原版按住 ZL） */
sw_ta_glyph sw_ta_current_set(const sw_tg_snapshot *s,uint64_t sdk_button,uint64_t host_now_ns,int skill_set);
int sw_ta_frame_at(int atlas,int frame,sw_ta_frame *out);
