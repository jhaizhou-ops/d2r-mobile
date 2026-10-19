#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    DM_CTL_MOVE_UP, DM_CTL_MOVE_DOWN, DM_CTL_MOVE_LEFT, DM_CTL_MOVE_RIGHT,
    DM_CTL_A, DM_CTL_B, DM_CTL_X, DM_CTL_Y, DM_CTL_LS, DM_CTL_RS,
    DM_CTL_L, DM_CTL_R, DM_CTL_ZL, DM_CTL_ZR, DM_CTL_PLUS, DM_CTL_MINUS,
    DM_CTL_LEFT, DM_CTL_UP, DM_CTL_RIGHT, DM_CTL_DOWN, DM_CTL_CONFIRM, DM_CTL_BACK,
    DM_CTL_PRIMARY, DM_CTL_SECONDARY, DM_CTL_SCROLL_UP, DM_CTL_SCROLL_DOWN, DM_CTL_INVENTORY, DM_CTL_CHARACTER, DM_CTL_SKILLS, DM_CTL_QUESTS,
    DM_CTL_POTION_1, DM_CTL_POTION_2, DM_CTL_POTION_3, DM_CTL_POTION_4, DM_CTL_ZOOM, DM_CTL_COUNT
};
enum { DM_CONTROL_UNBOUND = -1, DM_CONTROL_MOUSE = 256, DM_CONTROL_WHEEL_UP = 512, DM_CONTROL_WHEEL_DOWN = 513,
       DM_CONTROL_SHIFT = 1 << 10, DM_CONTROL_CONTROL = 2 << 10, DM_CONTROL_OPTION = 4 << 10 };
void dm_controls_init(const char *directory);
typedef int (*dm_controls_mouse_reserved_fn)(int input);
void dm_controls_mouse_get(int bindings[2][2]);
int dm_controls_mouse_lookup(int input);
int dm_controls_mouse_validate(const int bindings[2][2],dm_controls_mouse_reserved_fn reserved,char *error,size_t capacity);
int dm_controls_mouse_commit(const int bindings[2][2],dm_controls_mouse_reserved_fn reserved,char *error,size_t capacity);
void dm_controls_defaults(int bindings[DM_CTL_COUNT][2]);
void dm_controls_get(int bindings[DM_CTL_COUNT][2]);
int dm_controls_validate(const int bindings[DM_CTL_COUNT][2], char *error, size_t capacity);
int dm_controls_commit(const int bindings[DM_CTL_COUNT][2], char *error, size_t capacity);
int dm_controls_lookup(int input);
uint64_t dm_controls_button(int action);
void dm_controls_label(int input, char *out, size_t capacity);
void dm_controls_action_label(int action, char *out, size_t capacity);
int dm_controls_display_action(int row);
unsigned dm_controls_revision(void);
const char *dm_controls_glyph_path(const char *original);
void dm_controls_refresh_glyphs(void);
int dm_controls_primary_input(int action);
#ifdef __OBJC__
#import <Cocoa/Cocoa.h>
int dm_controls_event_input(NSEvent *event);
int dm_controls_virtual_key(NSEvent *event);
int dm_controls_virtual_modifiers(NSEvent *event);
BOOL dm_controls_modifier_down(NSEvent *event);
BOOL dm_controls_text_responder(NSResponder *responder);
void dm_controls_show(NSWindow *parent, void (^on_apply)(void));
void dm_controls_show_mouse(NSWindow *parent,dm_controls_mouse_reserved_fn reserved,void (^on_apply)(void));
#endif
