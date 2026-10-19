static float hover_x,hover_y;static int hover_allowed,hover_calls;
void dm_world_pc_hover(float x,float y,int allowed){hover_x=x;hover_y=y;hover_allowed=allowed;hover_calls++;}
void dm_world_pc_skill_tick(void){}
void dm_world_pc_skill(int slot,int down){(void)slot;(void)down;}
void dm_world_pc_skill_cancel(void){}
static float skill_pointer_x,skill_pointer_y;static int skill_pointer_allowed;
void dm_world_pc_skill_pointer(float x,float y,int allowed){skill_pointer_x=x;skill_pointer_y=y;skill_pointer_allowed=allowed;}
#include <stdio.h>
void dm_custom_pointer_input(void) { }
#include <stdlib.h>
#include "../dm.h"
#define dm_ui_item_at_now test_ui_item_at_now
#define dm_ui_touch test_ui_touch
#define dm_ui_tab_go test_ui_tab_go
#define dm_ui_pc_world_at test_ui_pc_world_at
#define dm_ui_pc_world_active test_ui_pc_world_active
#define dm_ui_ptr test_ui_ptr
#define dm_ui_ptr_at test_ui_ptr
#define dm_ui_ptr_at_mods test_ui_ptr_mods
#define dm_ui_pc_binding test_ui_pc_binding
#define dm_ui_pc_panel test_ui_pc_panel
#define dm_ui_pc_cancel_binding test_ui_pc_cancel_binding
static int live_oom;
static void *live_test_malloc(size_t n){return live_oom?NULL:malloc(n);}
#define malloc live_test_malloc
#include "../touch_mac.c"
#undef malloc
#undef dm_ui_item_at_now
#undef dm_ui_touch
#undef dm_ui_tab_go
#undef dm_ui_pc_world_at
#undef dm_ui_pc_world_active
#undef dm_ui_ptr
#undef dm_ui_ptr_at
#undef dm_ui_ptr_at_mods
#undef dm_ui_pc_binding
#undef dm_ui_pc_panel
#undef dm_ui_pc_cancel_binding
#include "../../ios/Sources/dm_touch_controls.c"

static uint64_t test_now;
uint64_t now_ns(void) { return test_now; }
static int pc_panel, pc_requests;
static int binding_open,binding_cancels;
static int test_pc_policy=1,mouse_native_result=1;
int test_ui_pc_world_active(void){return test_pc_policy;}
static int test_pc_desktop,test_pc_world=1,mouse_native_calls,mouse_native_side[128],mouse_native_phase[128],ptr_events,ptr_kind[128];
static float mouse_native_x[128],mouse_native_y[128],ptr_x[128],ptr_y[128];
static int ptr_mods[128],ptr_arg[128];
int test_ui_pc_world_at(float nx,float ny){(void)nx;(void)ny;return test_pc_world;}
void test_ui_ptr(int kind,float nx,float ny,int arg){if(ptr_events<128){ptr_kind[ptr_events]=kind;ptr_x[ptr_events]=nx;ptr_y[ptr_events]=ny;ptr_mods[ptr_events]=0;ptr_arg[ptr_events++]=arg;}}
void test_ui_ptr_mods(int kind,float nx,float ny,int arg,int mods){int at=ptr_events;test_ui_ptr(kind,nx,ny,arg);if(at<128)ptr_mods[at]=mods;}
int dm_world_pc_mouse(int side,int phase,float nx,float ny){int n=mouse_native_calls++;if(n<128){mouse_native_side[n]=side;mouse_native_phase[n]=phase;mouse_native_x[n]=nx;mouse_native_y[n]=ny;}return mouse_native_result;}

static int native_calls,native_action[256],native_down[256];
int test_ui_pc_binding(int action,int down){if(native_calls<256){native_action[native_calls]=action;native_down[native_calls++]=down;}return 1;}
int dm_ui_pc_desktop(void) { return test_pc_desktop; }
int test_ui_pc_cancel_binding(void) { if(!binding_open)return 0;binding_cancels++;return 1; }
void test_ui_pc_panel(int panel) { pc_panel = panel; pc_requests++; }
static int test_in, test_paused, test_hit, clicks, walks, ui_events, tabs, result = -1;
static int test_hud, aims;
static int32_t polled_rx, polled_ry;
static float aim_x, aim_y;
static unsigned click_seq;
static uint64_t polled_buttons;
static float click_x, click_y, walk_x, walk_y;
static int test_ui_y, test_ui_h, test_real_h;
int test_ui_item_at_now(float x, float y) { (void)x; (void)y; return test_hit; }
void test_ui_touch(int op, float x, float y) { (void)op; (void)x; (void)y; ui_events++; }
void test_ui_tab_go(void) { tabs++; }
void (*dm_pad_hook)(uint64_t, uint64_t *, int32_t *, int32_t *, int32_t *, int32_t *);
uint64_t guest_ns(void) { return 0; }
uint64_t dm_guest_frame(void) { return 0; }   // 界面鼠标 / 前端路径按出图帧节拍走；本单测不测它们 | the UI mouse / front-end paths run on the presented-frame cadence; this unit test does not test them
int dm_world_mouse_aim(float nx, float ny, int32_t *rx, int32_t *ry) { aims++; aim_x = nx; aim_y = ny; *rx = 12345; *ry = -23456; return 1; }
int dm_world_has_target(void) { return 1; }   // 点世界（dm_world.c）的桩：有目标 = 主按钮照常发键 | stub for world tapping (dm_world.c): has a target = the primary button sends its key as usual
unsigned dm_world_tap(float nx, float ny) { (void)nx; (void)ny; return 0; }
void dm_world_walk(float nx, float ny) { walks++; walk_x = nx; walk_y = ny; } unsigned dm_world_click(float nx, float ny) { clicks++; click_x = nx; click_y = ny; return ++click_seq; } int dm_update_click_at(float nx, float ny) { (void)nx; (void)ny; return 0; } int dm_world_tap_result(unsigned q) { (void)q; return result; }
uint64_t dm_padmode_legend_click(float nx, float ny) { (void)nx; (void)ny; return 0; } int dm_padmode_hud_click(float nx, float ny, uint64_t *b) { (void)nx; (void)ny; if (test_hud) *b = DM_BTN_A; return test_hud; } int dm_padmode_legend_has(uint32_t g) { (void)g; return 0; } int dm_game_paused(void) { return test_paused; }
int dm_shop_open(void) { return 0; }   // 商人页（dm_actions.c）的桩：没开 | stub for the vendor page (dm_actions.c): not open
int dm_padmode_current(void) { return 0; }   // 手柄 / 触屏模式（dm_padmode.c）的桩：触屏 | Stub for controller / touch mode (dm_padmode.c): touch
uint64_t dm_padmode_page_keys(uint64_t m) { (void)m; return 0; } void dm_padmode_legend_hide(const char *l, int a) { (void)l; (void)a; } int dm_padmode_busy(void) { return 0; } uint64_t dm_padmode_page_held(void) { return 0; } void dm_padmode_page_pulse(uint64_t m) { (void)m; } void dm_padmode_ident_hint(int on) { (void)on; } void dm_padmode_legend(int a, const char *t) { (void)a; (void)t; } const char *dm_str(uint8_t *b, const char *k) { (void)b; return k; }   // 手柄页键 / 提示条 / 取串的桩（个人仓库手柄翻页用，触屏下不走） | Stubs for controller page keys / button legend / string fetch (used by controller paging of the personal stash, not reached under touch)
int dm_swapskill_wset(void) { return -1; }
int dm_pages_cur(void) { return 0; } int dm_pages_count(void) { return 5; } void dm_pages_request_flip(int d) { (void)d; }   // 个人仓库多页（dm_pages.c）桩 | stubs for the multi-page personal stash (dm_pages.c)
void dm_vault_ui_flip(int dir) { (void)dir; }
int dm_vault_ui_pointer(int phase,int x,int y) { (void)phase;(void)x;(void)y;return 0; }
void dm_vault_ui_returning(uint32_t id) { (void)id; }
int dm_vault_ui_pager_hit(int x, int y) { (void)x; (void)y; return 0; }
void dm_vault_request_deposit(uint32_t id) { (void)id; }
uint32_t dm_vault_ui_held(void) { return 0; }
int dm_vault_ui_over(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_tip_gate(int x, int y) { (void)x; (void)y; return 0; }
int dm_pouch_tip_gate(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_blocks(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_tap(int x, int y) { (void)x; (void)y; return 0; }
int dm_vault_ui_drag(int phase, int x, int y) { (void)phase; (void)x; (void)y; return 0; }
uint32_t dm_swapskill_itemsig(void) { return 0; }   // 副手组技能当前武器组（dm_actions.c）的桩：读不到 | stub for the current weapon set of the swap-set skill (dm_actions.c): cannot be read
_Atomic int g_dm_frz_want, g_dm_frozen; void dm_freeze_frame(int want) { (void)want; }   // 面板冻结（diag.c）的桩 | stub for the panel freeze (diag.c)
void dm_elysium_arm(int on) { (void)on; }   // 极乐世界勾选（dm_elysium.c）的桩 | stub for the Elysium checkbox (dm_elysium.c)
void dm_custom_on_uimsg(const char *cat, const char *act, const char *arg) { (void)cat; (void)act; (void)arg; }   // 「自定义」设置（dm_custom.c）的桩 | stub for the "Custom" settings (dm_custom.c)
void dm_belt_put(unsigned id) { (void)id; }
void dm_ui_rect(int *x, int *y, int *w, int *h) { *x = *w = 0; *y = test_ui_y; *h = test_ui_h; }
void dm_uiscreen_set_panel(int on) { (void)on; }
int dm_screen_real(int *w, int *h) { *w = 1600; *h = test_real_h; return test_real_h != 0; }
int dm_uiscreen_ingame(void) { return test_in; }
void dm_uiscreen_set_ingame(int on) { (void)on; }   // 界面矩形（dm_uiscreen.c）的桩：没开真全屏 | stub for the UI rectangle (dm_uiscreen.c): true full screen not enabled
void dm_inv_sort(int page) { (void)page; }
int dm_diag_difficulty(void) { return -1; }
uint64_t dm_fs_commit_count(void) { return 0; }
void dm_held_equip(int bodyloc) { (void)bodyloc; }
int dm_rift_merc_unlocked(void) { return 1; }
int dm_rift_merc_slot_empty(int loc) { (void)loc; return 0; }
void dm_rift_merc_locked_hint(void) {}
void dm_stash_take(int count) { (void)count; }
void dm_shop_buy(void) {}
int dm_cursor_item(void *out) { (void)out; return 0; }
void dm_shop_sell_cursor(void) {}
module_t g_mod[MAX_MOD]; int g_nmod;
static void (*ui_cb)(const char *, const char *, const char *);
static uint32_t test_tick;
uint32_t dm_cur_tick(void) { return test_tick; }
static int test_loadscreen_down;
int dm_loadscreen_down(void) { return test_loadscreen_down; }   // 真判定在 diag.c；单测随读取画面消息手动置位 | the real check is in diag.c; the unit test sets it by hand following the loading screen messages
int dm_loadscreen_up(void) { return 0; }      // 单测里读取画面不挡起局：不触发冻结 | in the unit test the loading screen does not block starting a run: the freeze is not triggered
void dm_hold_for_loadscreen(void) {}
void uimsg_subscribe(void (*f)(const char *, const char *, const char *)) { ui_cb = f; }
static int checks, fails;
#define CHECK(c, s) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", s); } else printf("PASS %s\n", s); } while (0)


// 真鼠标经公开事件入口推进；世界查询的答案由下一次宿主轮询提供，不绕过事件队列。
// Drive the real mouse through its public event entry point; the next host poll supplies the world-pick answer, without bypassing the event queue.
static void live_poll(void) {
    uint64_t b = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    dm_touch_host_step(0, &b, &lx, &ly, &rx, &ry); polled_buttons = b; polled_rx = rx; polled_ry = ry;
}
static void live_reset(void) {
    unsetenv("DM_TOUCH_SCRIPT"); memset(&g_touch, 0, sizeof g_touch); memset(&g_lm, 0, sizeof g_lm); memset(&g_rm, 0, sizeof g_rm);
    atomic_store(&g_live.head, 0); atomic_store(&g_live.tail, 0);
    g_live_on = g_live_based = 0; g_live_btn = 0;
    test_pc_policy=mouse_native_result=1;test_pc_desktop=0;test_pc_world=1;memset(g_pc_mouse,0,sizeof g_pc_mouse);mouse_native_calls=ptr_events=0;
    test_now = 1000000000ull; test_in = 1; test_paused = 0; test_hit = 3;
    test_hud = test_ui_y = test_ui_h = test_real_h = 0;
    native_calls=0; clicks = walks = ui_events = tabs = aims = 0; result = -1; click_seq = 0;
    dm_touch_host_aim(0, 0, 0); pc_requests = 0; binding_open=binding_cancels=0;
    dm_touch_host_live_enable(); live_poll();
}
static void live_event(int op, float x, float y) { test_now += 33333334ull; dm_touch_host_live(op, x, y); live_poll(); }
static void test_live_mouse(void) {
    live_reset();
    live_event(0, .7f, .4f);
    CHECK(clicks == 1 && click_x == .7f && click_y == .4f, "mouse down immediately queues one world pick");
    CHECK(walks == 0, "pending hit test does not walk through a target");
    result = 0; live_poll();
    CHECK(walks == 1, "empty-ground result starts walking before release");
    int n = walks;
    live_poll(); CHECK(walks == n, "repeated polls in one tick do not flood walk requests");
    test_now += 33333334ull; live_event(1, .8f, .5f);
    CHECK(walks == n + 1 && walk_x == .8f && walk_y == .5f, "held pointer updates within one 30 Hz tick, without a 300 ms wait");
    n = walks; live_event(2, 0, 0); test_now += 400000000ull; live_poll();
    CHECK(clicks == 1 && walks == n, "release neither repeats the click nor continues held movement");

    live_reset(); dm_touch_host_live(0, .7f, .4f); dm_touch_host_live(2, 0, 0); live_poll();
    CHECK(clicks == 1, "down and up in the same poll still produce one immediate click");
    result = 0; live_poll(); CHECK(walks == 1 && walk_x == .7f && walk_y == .4f, "quick ground click retains its destination after release");

    live_reset(); live_event(0, .7f, .4f); result = 2; live_poll();
    test_now += 400000000ull; live_poll(); live_event(2, 0, 0);
    CHECK(clicks == 1 && walks == 0, "holding an NPC click does not replace interaction with ground movement");
    live_reset(); live_event(0, .7f, .4f); result = 6; live_poll();
    test_now += 33333334ull; live_poll(); CHECK(walks == 1, "held monster steering updates without waiting for the attack interval");
    test_now += 200000000ull; live_poll(); CHECK(walks == 2, "holding a monster continues to request attack or steering updates");

    live_reset(); live_event(0, .7f, .4f); test_paused = 1; result = 0; live_poll();
    test_paused = 0; test_now += 400000000ull; live_poll(); live_event(2, 0, 0);
    CHECK(walks == 0 && ui_events == 0, "pause cancels pending world input and consumes its release");
    live_reset(); live_event(0, .7f, .4f); dm_tc_set_navigation_mode(&g_touch.state, 1); result = 0; live_poll();
    dm_tc_set_navigation_mode(&g_touch.state, 0); test_now += 400000000ull; live_poll(); live_event(2, 0, 0);
    CHECK(walks == 0 && ui_events == 0, "opening a panel cancels world input until a new press");
    live_reset(); live_event(0, .7f, .4f); test_in = 0; result = 0; live_poll(); live_event(2, 0, 0);
    CHECK(walks == 0 && ui_events == 0, "leaving the game does not leak a world release into the menu");

    live_reset(); test_ui_y = 100; test_ui_h = 800; test_real_h = 1000;
    live_event(0, .7f, .4f); result = 0; live_poll();
    CHECK(fabsf(click_y - .42f) < .0001f && fabsf(walk_y - .42f) < .0001f, "fullscreen world down converts interface coordinates to screen coordinates");
    live_event(1, .8f, .6f);
    CHECK(fabsf(walk_y - .58f) < .0001f, "fullscreen held movement uses the same coordinate conversion");
    live_reset(); test_hud = 1; live_event(0, .7f, .9f);
    CHECK(clicks == 0 && (polled_buttons & DM_BTN_A), "HUD press remains a held gamepad button");
    live_event(2, 0, 0); test_now += 200000000ull; live_poll();
    CHECK(clicks == 0 && walks == 0 && !(polled_buttons & DM_BTN_A), "HUD release clears its gamepad button without walking");

    live_reset(); test_in = 0; live_event(0, .2f, .2f); live_event(2, 0, 0);
    CHECK(clicks == 0 && walks == 0 && ui_events == 2, "front-end menu clicks retain their original UI path");
    live_reset(); test_in = 0; test_hit = 2; live_event(0, .2f, .2f); live_event(2, 0, 0);
    CHECK(clicks == 0 && tabs == 1, "front-end tabs retain their gamepad-key path");
    live_reset(); test_hit = 0; live_event(0, .7f, .4f); live_event(2, 0, 0);
    CHECK(clicks == 0 && walks == 0 && ui_events == 2, "UI widgets in an unrecognised panel still click on release");
    live_reset(); dm_tc_set_navigation_mode(&g_touch.state, 1); test_hit = 1;
    live_event(0, .7f, .4f); test_now += 40000000ull; live_event(1, .8f, .5f); live_event(2, 0, 0);
    CHECK(clicks == 0 && walks == 0, "inventory drag never enters the immediate world-click path");
}

static void test_right_mouse(void) {
    live_reset(); live_event(3, .7f, .4f);
    CHECK((polled_buttons & DM_BTN_X) && !(polled_buttons & DM_BTN_B) && aims == 1, "Derecho en el mundo activa la habilidad original X/U y apunta al cursor");
    CHECK(aim_x == .7f && aim_y == .4f && polled_rx == 12345 && polled_ry == -23456, "La dirección original recibe el punto de pantalla solicitado");
    live_event(4, .8f, .3f);
    CHECK((polled_buttons & DM_BTN_X) && aim_x == .8f && aim_y == .3f, "Arrastrar derecho actualiza el apuntado sin soltar la habilidad");
    live_event(5, 0, 0); CHECK(!(polled_buttons & (DM_BTN_X | DM_BTN_B)), "Soltar derecho termina la habilidad");
    live_reset(); test_in = 0; live_event(3, .2f, .2f);
    CHECK((polled_buttons & DM_BTN_B) && !(polled_buttons & DM_BTN_X) && !aims, "Derecho en menús mantiene Volver");
    live_reset(); dm_tc_set_navigation_mode(&g_touch.state, 1); test_hit = 0; live_event(3, .2f, .2f);
    CHECK(polled_buttons & DM_BTN_B, "Derecho en inventario mantiene Volver");
    dm_tc_set_navigation_mode(&g_touch.state, 0); test_hit = 3; live_poll();
    CHECK(!(polled_buttons & (DM_BTN_X | DM_BTN_B)) && !aims, "Cerrar inventario con derecho mantenido no lanza una habilidad");
    live_event(4, .7f, .4f); CHECK(!(polled_buttons & DM_BTN_X), "Mover derecho tras cerrar un panel requiere una pulsación nueva");
    live_event(5, 0, 0); live_event(3, .7f, .4f); CHECK(polled_buttons & DM_BTN_X, "Una pulsación nueva en el mundo sí activa la habilidad");
    test_paused = 1; live_poll(); CHECK(!(polled_buttons & DM_BTN_X), "Pausa cancela una habilidad mantenida");
    test_paused = 0; live_poll(); CHECK(!(polled_buttons & DM_BTN_X), "Reanudar no reactiva una habilidad cancelada");
    live_reset(); dm_touch_host_live(3, .7f, .4f); dm_touch_host_live(5, 0, 0); live_poll();
    CHECK(polled_buttons & DM_BTN_X, "Una pulsación y liberación entre dos muestras conserva una acción");
    live_poll(); CHECK(!(polled_buttons & DM_BTN_X), "La pulsación breve termina después de una muestra");
    live_reset(); test_ui_y = 100; test_ui_h = 800; test_real_h = 1000; live_event(3, .7f, .4f);
    CHECK(fabsf(aim_y - .42f) < .0001f, "Apuntado derecho usa la misma conversión de pantalla completa que el izquierdo");
}

static void pc_sample(uint64_t keys) {
    int32_t lx=0,ly=0,rx=0,ry=0; dm_touch_host_step(0,&keys,&lx,&ly,&rx,&ry); polled_buttons=keys;polled_rx=rx;polled_ry=ry;
}
static void test_pc_keyboard(void) {
    const uint64_t skills[6]={DM_BTN_A,DM_BTN_B,DM_BTN_Y,DM_BTN_X,DM_BTN_R,DM_BTN_ZR};
    for(int i=0;i<6;i++) { live_reset(); dm_touch_host_aim(skills[i],.8f,.2f); CHECK(!aims,"Publicar cursor no escribe el juego desde AppKit"); pc_sample(skills[i]); CHECK(aims==1 && aim_x==.8f && aim_y==.2f && polled_rx==12345,"Cada habilidad numérica apunta al cursor en tick del juego"); }
    live_reset(); test_ui_y=100;test_ui_h=800;test_real_h=1000;dm_touch_host_aim(DM_BTN_X,.7f,.4f);pc_sample(DM_BTN_X);CHECK(fabsf(aim_y-.42f)<.0001f,"Apuntado numérico conserva conversión de pantalla completa");
    live_reset();dm_touch_host_aim(DM_BTN_X,.7f,.4f);dm_tc_set_navigation_mode(&g_touch.state,1);pc_sample(DM_BTN_X);CHECK(!aims,"Habilidades no apuntan al mundo con inventario abierto");
    live_reset();dm_touch_host_aim(DM_BTN_X,.7f,.4f);test_paused=1;pc_sample(DM_BTN_X);CHECK(!aims,"Habilidades no apuntan mientras el juego está pausado");
    live_reset();dm_touch_host_aim(DM_BTN_X,.7f,.4f);dm_touch_host_aim(0,0,0);pc_sample(DM_BTN_X);CHECK(!aims,"Liberar foco cancela apuntado pendiente");
    for(int i=0;i<4;i++){live_reset();pc_sample(1ull<<(42+i));CHECK(pc_requests==1 && pc_panel==(i==0?DM_PC_INVENTORY:i==1?DM_PC_CHARACTER:i==2?DM_PC_SKILLS:DM_PC_QUESTS),"I/C/T/Q encolan la página original correcta");CHECK(!(polled_buttons&(15ull<<42)),"Bits de atajos PC no llegan al juego");pc_sample(1ull<<(42+i));CHECK(pc_requests==1,"Mantener atajo no abre y cierra repetidamente");pc_sample(0);}
    live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);pc_sample(1ull<<45);CHECK((polled_buttons&DM_BTN_L)&&!pc_requests,"Q en menús conserva pestaña anterior sin abrir misiones");pc_sample(0);
    live_reset();test_pc_desktop=1;dm_tc_set_navigation_mode(&g_touch.state,1);pc_sample(1ull<<45);CHECK(!pc_requests&&!native_calls&&!polled_buttons,"PC69 no usa el atajo de misiones de la tabla Mac35 inactiva");pc_sample(0);
    live_reset();pc_sample(1ull<<41);CHECK(pc_requests==1&&pc_panel==DM_PC_OPTIONS&&!(polled_buttons&DM_BTN_PLUS),"Esc en mundo abre opciones originales, sin lanzar habilidad ni abrir inventario");pc_sample(0);
    live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);binding_open=1;pc_sample(1ull<<41);CHECK(binding_cancels==1&&!polled_buttons,"Escape cancela asignación sin emitir B ni otra casilla");binding_open=0;pc_sample(1ull<<41);CHECK(binding_cancels==1&&!polled_buttons,"Escape mantenido no repite cancelación ni se filtra tras cierre");pc_sample(0);
    live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);binding_open=1;pc_sample(DM_BTN_B);CHECK(!binding_cancels&&(polled_buttons&DM_BTN_B),"Tecla explícita 2 conserva asignación original de casilla B");pc_sample(0);pc_sample((1ull<<41)|DM_BTN_B);CHECK(binding_cancels==1&&!(polled_buttons&DM_BTN_B),"Escape tiene prioridad frente a asignación simultánea");pc_sample(0);
    live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);binding_open=1;live_event(3,.4f,.4f);CHECK(binding_cancels==1&&!(polled_buttons&(DM_BTN_B|DM_BTN_X)),"Derecho cancela modal sin asignar casilla2");live_event(4,.5f,.5f);CHECK(binding_cancels==1&&!polled_buttons,"Derecho mantenido no repite cierre");binding_open=0;dm_tc_set_navigation_mode(&g_touch.state,0);live_poll();CHECK(!(polled_buttons&(DM_BTN_B|DM_BTN_X)),"Cerrar modal no convierte derecho mantenido en habilidad");live_event(5,0,0);live_event(3,.7f,.4f);CHECK(polled_buttons&DM_BTN_X,"Derecho nuevo en mundo mantiene habilidad secundaria");live_event(5,0,0);
}
static void test_pc_potions_navigation(void) {
    const uint64_t potion[4]={DM_MAC_POTION_1,DM_MAC_POTION_2,DM_MAC_POTION_3,DM_MAC_POTION_4};
    const uint64_t nav[4]={DM_MAC_NAV_LEFT,DM_MAC_NAV_UP,DM_MAC_NAV_DOWN,DM_MAC_NAV_RIGHT};
    const uint64_t native[4]={DM_BTN_LEFT,DM_BTN_UP,DM_BTN_DOWN,DM_BTN_RIGHT};
    for(int i=0;i<4;i++) {
        live_reset();pc_sample(potion[i]);CHECK(polled_buttons==native[i],"Poción dedicada solo emite su columna nativa en el mundo");pc_sample(0);
        live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);pc_sample(potion[i]);CHECK(!polled_buttons,"Poción QWER no navega ni consume en un panel");
        dm_tc_set_navigation_mode(&g_touch.state,0);pc_sample(potion[i]);CHECK(!polled_buttons,"Poción mantenida desde un menú no se activa al cerrar");pc_sample(0);pc_sample(potion[i]);CHECK(polled_buttons==native[i],"Nueva pulsación de poción después del menú sí se activa");pc_sample(0);
        live_reset();pc_sample(nav[i]);CHECK(!polled_buttons,"Flecha de navegación no bebe una poción en el mundo");
        dm_tc_set_navigation_mode(&g_touch.state,1);pc_sample(nav[i]);CHECK(!polled_buttons,"Flecha mantenida desde el mundo no se hereda al panel");pc_sample(0);pc_sample(nav[i]);CHECK(polled_buttons==native[i],"Flecha nueva navega el panel con el botón original");
        dm_tc_set_navigation_mode(&g_touch.state,0);pc_sample(nav[i]);CHECK(!polled_buttons,"Cerrar panel con flecha mantenida no consume poción");pc_sample(0);
    }
    live_reset();test_in=0;pc_sample(DM_MAC_NAV_DOWN);CHECK(polled_buttons==DM_BTN_DOWN,"Flechas navegan también la selección de personaje");pc_sample(0);pc_sample(DM_MAC_POTION_3);CHECK(!polled_buttons,"Pociones nunca llegan al menú principal");pc_sample(0);
    live_reset();test_paused=1;pc_sample(DM_MAC_POTION_1);CHECK(!polled_buttons,"Pausa bloquea pociones dedicadas");test_paused=0;pc_sample(DM_MAC_POTION_1);CHECK(!polled_buttons,"Reanudar con Q mantenida no bebe");pc_sample(0);
    live_reset();pc_sample(DM_BTN_LEFT);CHECK(polled_buttons==DM_BTN_LEFT,"DPAD físico y perfil personalizado antiguo conservan comportamiento");pc_sample(0);
}
static void test_native_pc_mouse(void) {
 live_reset();test_pc_desktop=1;live_event(0,.7f,.4f);
 CHECK(mouse_native_calls==1&&mouse_native_side[0]==0&&mouse_native_phase[0]==0&&!clicks&&!walks&&!polled_buttons,"Izquierdo PC pulsa mano seleccionada sin mando ni pathfinder legado");
 dm_touch_host_live(1,.8f,.5f);dm_touch_host_live(2,0,0);live_poll();
 CHECK(mouse_native_calls==3&&mouse_native_phase[1]==1&&mouse_native_x[1]==.8f&&mouse_native_phase[2]==2,"MOVE final PC llega antes de UP incluso en el mismo poll");
 int calls=mouse_native_calls;test_now+=400000000;live_poll();CHECK(mouse_native_calls==calls,"Soltar termina movimiento PC sin nuevas órdenes");
 live_reset();test_pc_desktop=1;live_event(3,.7f,.4f);CHECK(mouse_native_calls==1&&mouse_native_side[0]==1&&mouse_native_phase[0]==0&&!polled_buttons,"Derecho PC usa secundaria seleccionada sin casilla X fija");
 test_pc_world=0;live_poll();CHECK(mouse_native_calls==2&&mouse_native_phase[1]==2,"Entrar en HUD o panel cancela mano mantenida");
 test_pc_world=1;live_poll();live_event(5,0,0);CHECK(mouse_native_calls==2&&!ptr_events,"Salir de HUD mientras se mantiene no hereda otro lanzamiento");
 live_reset();test_pc_desktop=1;test_pc_world=0;test_hud=1;live_event(0,.7f,.9f);live_event(2,0,0);
 CHECK(!mouse_native_calls&&!polled_buttons&&ptr_events==4&&ptr_kind[1]==DM_UP_LDOWN&&ptr_kind[3]==DM_UP_LUP,"HUD PC recibe botones de ratón nativos en lugar de emulación de mando");
 live_reset();test_pc_desktop=1;test_pc_world=0;test_in=0;live_event(3,.2f,.2f);live_event(5,0,0);
 CHECK(!mouse_native_calls&&!polled_buttons&&ptr_events==4&&ptr_kind[1]==DM_UP_RDOWN&&ptr_kind[3]==DM_UP_RUP,"Derecho en UI PC conserva eventos originales sin B");
 live_reset();test_pc_desktop=1;test_ui_y=100;test_ui_h=800;test_real_h=1000;live_event(0,.7f,.4f);
 CHECK(fabsf(mouse_native_y[0]-.42f)<.0001f,"Mouse PC convierte rect UI a pantalla una sola vez");
 dm_touch_host_aim(DM_BTN_Y,.6f,.7f);live_poll();CHECK(skill_pointer_allowed&&skill_pointer_x==.6f&&fabsf(skill_pointer_y-.66f)<.0001f,"QuickCast publica cursor fullscreen válido para callback posterior");
 test_pc_world=0;live_poll();CHECK(!skill_pointer_allowed,"QuickCast no apunta a botones del HUD");
}

static void *mods_producer(void *opaque){int id=(int)(intptr_t)opaque;for(int i=0;i<120;i++)dm_touch_host_pc_mouse_mods(2+id,i%2?2:0,(float)i/200,(float)id/10,-1,((i%7)+1)<<12);return NULL;}
static void test_pc_mouse_modifiers(void) {
 live_reset();test_pc_desktop=1;test_pc_world=0;
 dm_touch_host_pc_mouse_mods(0,0,.2f,-.05f,-1,0x1000);
 dm_touch_host_pc_mouse_mods(0,1,.3f,.4f,-1,0x2000);
 dm_touch_host_pc_mouse_mods(0,2,.7f,.8f,-1,0x4000);
 live_poll();
 CHECK(ptr_events==5&&ptr_kind[1]==DM_UP_LDOWN&&ptr_kind[4]==DM_UP_LUP,"Cola PC conserva DOWN, MOVE y UP antes de un solo poll");
 CHECK(ptr_mods[0]==0x1000&&ptr_mods[1]==0x1000&&ptr_mods[2]==0x2000&&ptr_mods[3]==0x4000&&ptr_mods[4]==0x4000,"Modificadores se entregan por evento, sin leer estado posterior");
 CHECK(ptr_x[1]==.2f&&ptr_y[1]==-.05f&&ptr_x[4]==.7f&&ptr_y[4]==.8f,"DOWN firmado y UP preciso conservan su propio XY junto al snapshot");
 CHECK(!ptr_arg[1]&&!ptr_arg[4]&&!mouse_native_calls&&!polled_buttons,"Mods viajan separados del argumento nativo y no emulan mando ni cast");
 live_reset();test_pc_desktop=1;test_pc_world=0;
 dm_touch_host_pc_mouse_mods(1,0,.4f,.5f,-1,0xffff);live_poll();CHECK(ptr_kind[1]==DM_UP_RDOWN&&ptr_mods[1]==0x7000,"Derecho nativo conserva sólo los tres bits Shift/Ctrl/Alt");
 dm_touch_host_pc_mouse(1,2,0,0,-1);live_poll();CHECK(ptr_kind[3]==DM_UP_RUP&&!ptr_mods[3]&&ptr_x[3]==.4f&&ptr_y[3]==.5f,"Wrapper legacy conserva XY anterior y libera sin modificadores sintéticos");
 live_reset();test_pc_desktop=1;test_pc_world=0;
 dm_touch_host_pc_mouse_mods(0,0,.4f,.5f,-1,0x1000);dm_touch_host_pc_mouse_mods(0,1,.5f,.5f,-1,0x1000);dm_touch_host_pc_mouse_mods(0,1,.6f,.5f,-1,0x2000);dm_touch_host_pc_mouse_mods(0,1,.7f,.5f,-1,0x2000);dm_touch_host_pc_mouse_mods(0,2,.8f,.5f,-1,0);
 live_poll();CHECK(ptr_events==6&&ptr_mods[2]==0x1000&&ptr_x[2]==.5f&&ptr_mods[3]==0x2000&&ptr_x[3]==.7f,"Coalescencia sólo fusiona MOVE contiguos con iguales modificadores");
 live_reset();test_pc_desktop=1;test_pc_world=0;binding_open=1;dm_touch_host_pc_mouse_mods(1,0,.4f,.5f,-1,0x1000);dm_touch_host_pc_mouse_mods(1,2,.4f,.5f,-1,0x2000);live_poll();CHECK(binding_cancels==1&&!ptr_events&&!mouse_native_calls&&!polled_buttons,"Cancelación del modal mantiene prioridad y no convierte modificadores en asignación");
 live_reset();pthread_t producers[2];pthread_create(&producers[0],NULL,mods_producer,(void*)0);pthread_create(&producers[1],NULL,mods_producer,(void*)1);pthread_join(producers[0],NULL);pthread_join(producers[1],NULL);
 int seen[2]={0};live_mouse_event event;unsigned head;while(live_mouse_peek(&event,&head)){int id=event.pc_button-2;CHECK(id>=0&&id<2,"MPSC conserva identidad física del productor");if(id>=0&&id<2){int i=seen[id]++;CHECK(event.op==(i%2?2:0)&&event.pc_mods==((i%7)+1)<<12&&event.nx==(float)i/200&&event.ny==(float)id/10&&event.pc_precise,"Crecimiento de FIFO conserva tupla XY/modificadores y orden de cada productor");}live_mouse_consume(head);}
 CHECK(seen[0]==120&&seen[1]==120&&g_live_capacity==64,"Ambos productores completan sin perder UP y liberan la cola ampliada");
}
static void test_pc_top_strip(void) {
 live_reset();test_pc_desktop=1;test_ui_y=124;test_ui_h=900;test_real_h=1024;float ui_y=(.06f*1024-124)/900;
 live_event(0,.5f,ui_y);CHECK(mouse_native_calls==1&&mouse_native_side[0]==0&&fabsf(mouse_native_y[0]-.06f)<.00001f&&!ptr_events,"Franja superior 16:10 recibe clic mundo exacto .06 sin clamp ni mensajes UI");live_event(2,0,0);
 dm_touch_host_pointer(.5f,ui_y,1);int prevmouse=mouse_native_calls;live_poll();CHECK(hover_allowed==DM_PC_POINTER_WORLD&&hover_x==.5f&&fabsf(hover_y-.06f)<.00001f&&mouse_native_calls==prevmouse,"Hover sin botones conserva franja superior y no emite órdenes");CHECK(skill_pointer_allowed&&fabsf(skill_pointer_y-.06f)<.00001f,"QuickCast en franja superior conserva cursor real válido");
 test_pc_world=0;live_poll();CHECK(hover_allowed==DM_PC_POINTER_UI,"Puntero válido sobre HUD/UI conserva selección vacía explícita");CHECK(!skill_pointer_allowed,"Modal bloquea QuickCast incluso en franja superior");dm_touch_host_pointer(0,0,0);live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE,"Salir del frame comunica liberación sin click ni cast");
 dm_touch_host_pointer(.5f,.5f,1);test_paused=1;live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE,"Pausa libera propiedad aunque el puntero permanezca dentro");test_paused=0;
 dm_touch_host_pointer(NAN,.5f,1);live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE,"Coordenada no finita no se publica como snapshot UI");
 dm_touch_host_pointer(.5f,3,1);live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE,"Puntero fuera del render no bloquea selección vacía");
 dm_touch_host_pointer(.5f,.5f,1);test_pc_world=0;dm_tc_set_navigation_mode(&g_touch.state,1);live_poll();CHECK(hover_allowed==DM_PC_POINTER_UI&&!skill_pointer_allowed,"Panel UI válido mantiene vacío sin permitir QuickCast");dm_tc_set_navigation_mode(&g_touch.state,0);
 test_in=0;live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE,"Frontend libera propiedad de hover");test_in=1;live_poll();int calls=hover_calls;test_pc_desktop=0;live_poll();CHECK(hover_calls==calls+1&&hover_allowed==DM_PC_POINTER_NONE,"Salir de PC publica una liberación explícita");calls=hover_calls;live_poll();CHECK(hover_calls==calls,"Legacy posterior no recibe snapshots ni locks vacíos");test_pc_desktop=1;
 live_event(0,.5f,.3f);live_event(1,.5f,ui_y);CHECK(ptr_events>=3&&ptr_kind[ptr_events-1]==DM_UP_MOVE&&ptr_y[ptr_events-1]==ui_y,"MOVE de arrastre UI conserva coordenada negativa hasta productor preciso");live_event(2,0,0);
 live_reset();test_pc_desktop=1;dm_touch_host_pc_mouse(0,0,.5f,.4f,-1);live_poll();CHECK(!mouse_native_calls&&!ptr_events,"Botón físico retirado de ataque no actúa en mundo ni cae en ClickCatcher UI");dm_touch_host_pc_mouse(0,2,0,0,-1);live_poll();
 test_pc_world=0;dm_touch_host_pc_mouse(0,0,.5f,.4f,-1);live_poll();CHECK(ptr_events==2&&ptr_kind[1]==DM_UP_LDOWN,"Izquierdo retirado de ataque conserva clic UI físico");dm_touch_host_pc_mouse(0,2,0,0,-1);live_poll();
}
static void test_pc_pointer_cancel(void){
 live_reset();test_pc_desktop=1;test_pc_world=0;dm_touch_host_pointer(.5f,.8f,1);live_poll();
 CHECK(hover_allowed==DM_PC_POINTER_UI,"Snapshot UI válido antes de cancelación interna");
 g_live_cancel=1;live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE&&!g_key_aim.valid,"Cancelación interna invalida el puntero y libera hover sin republicarlo");
 CHECK(!mouse_native_calls,"Cancelación de hover UI no emite órdenes del mundo");
 dm_touch_host_pointer(.6f,.8f,1);live_poll();CHECK(hover_allowed==DM_PC_POINTER_UI&&g_key_aim.valid,"Nuevo snapshot físico posterior a cancel recupera hover UI");
 dm_touch_host_pointer(0,0,0);live_poll();CHECK(hover_allowed==DM_PC_POINTER_NONE,"PointerLeave posterior vuelve a liberar sin bloqueo persistente");
 live_reset();dm_touch_host_aim(DM_BTN_X,.7f,.4f);g_live_cancel=1;pc_sample(DM_BTN_X);CHECK(aims==1&&aim_x==.7f&&aim_y==.4f,"Cancelación OOM legacy conserva la muestra de apuntado ajena al perfil PC");
}
static void test_pc_mouse_alternatives(void){
 live_reset();test_pc_desktop=1;dm_touch_host_pc_mouse(3,0,.6f,.4f,0);dm_touch_host_pc_mouse(4,0,.6f,.4f,0);live_poll();
 int before=mouse_native_calls;dm_touch_host_pc_mouse(3,2,0,0,0);live_poll();CHECK(mouse_native_calls==before,"Soltar una alternativa física no libera la otra todavía mantenida");dm_touch_host_pc_mouse(4,2,0,0,0);live_poll();CHECK(mouse_native_calls==before+1&&mouse_native_phase[mouse_native_calls-1]==2,"Última alternativa física libera la mano seleccionada");
}
static void *live_producer(void *opaque){int id=(int)(intptr_t)opaque;for(int i=0;i<200;i++)dm_touch_host_pc_mouse(2+id,i%2?2:0,(float)i,.5f,-1);return NULL;}
static void test_live_queue_full(void){
 live_reset();test_pc_desktop=1;live_event(0,.5f,.4f);
 for(int i=0;i<1000;i++)dm_touch_host_pc_mouse(0,1,.6f+i*.0001f,.4f,0);
 dm_touch_host_pc_mouse(0,2,0,0,0);live_poll();
 CHECK(!g_pc_mouse[0].down&&!g_pc_mouse[0].kind,"Cola saturada conserva UP tras ráfaga de MOVE");
 CHECK(mouse_native_phase[mouse_native_calls-1]==2,"La última orden de ráfaga termina la mano, sin nuevo DOWN");
 CHECK(g_live_capacity==64&&!g_live_extra,"Al drenar se libera almacenamiento adicional sin punteros pendientes");
 live_reset();test_pc_desktop=1;
 for(int i=0;i<80;i++){dm_touch_host_pc_mouse(0,0,.5f,.4f,0);dm_touch_host_pc_mouse(0,2,0,0,0);}
 live_poll();CHECK(!g_pc_mouse[0].down&&mouse_native_calls==160,"Más de64 transiciones críticas mantienen orden completo sin pérdida");
 live_reset();test_pc_desktop=1;live_event(0,.5f,.4f);
 for(int i=0;i<32;i++){dm_touch_host_pc_mouse(3,0,.5f,.4f,-1);dm_touch_host_pc_mouse(3,2,0,0,-1);}
 live_oom=1;dm_touch_host_pc_mouse(0,2,0,0,0);live_oom=0;live_poll();
 CHECK(!g_pc_mouse[0].down&&!g_pc_mouse[0].kind&&mouse_native_calls==2&&mouse_native_phase[1]==2,"OOM cancela mano tras prefijo aceptado, sin lanzamiento duplicado");
 live_poll();CHECK(mouse_native_calls==2&&!g_live_cancel,"Cancelación OOM se consume una sola vez");
 live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);test_hit=0;
 for(int i=0;i<64;i++)dm_touch_host_live(0,.4f,.4f);
 live_oom=1;dm_touch_host_live(2,0,0);live_oom=0;live_poll();
 test_now+=33333334;live_poll();CHECK(!g_touch.state.finger[0].active&&!g_touch.state.buttons,"OOM legacy no reactiva DOWN del lote pendiente tras cancelar");
 live_reset();pthread_t producers[2];pthread_create(&producers[0],NULL,live_producer,(void*)0);pthread_create(&producers[1],NULL,live_producer,(void*)1);pthread_join(producers[0],NULL);pthread_join(producers[1],NULL);
 int seen[2]={0},total=0;unsigned head;live_mouse_event event;
 while(live_mouse_peek(&event,&head)){int id=event.pc_button-2;CHECK(id>=0&&id<2&&event.nx==seen[id]&&event.op==(seen[id]%2?2:0),"Productores live conservan payload y orden independiente");if(id>=0&&id<2)seen[id]++;live_mouse_consume(head);total++;}
 CHECK(total==400&&seen[0]==200&&seen[1]==200,"FIFO live creciente conserva400 transiciones concurrentes");
 live_reset();dm_touch_host_pc_mouse(2,1,.1f,.2f,-1);live_mouse_peek(&event,&head);
 dm_touch_host_pc_mouse(2,1,.9f,.8f,-1);live_mouse_consume(head);live_mouse_peek(&event,&head);
 CHECK(event.nx==.9f&&event.ny==.8f,"MOVE nuevo no modifica el payload ya copiado por el consumidor");live_mouse_consume(head);
 g_live_cancel=1;live_mouse_peek(&event,&head);dm_touch_host_pc_mouse(2,0,.3f,.4f,-1);live_mouse_consume(head);
 CHECK(live_mouse_peek(&event,&head)&&event.op==0&&event.nx==.3f,"Cancelar OOM no consume un DOWN publicado durante la cancelación");live_mouse_consume(head);
}
static void test_native_pc_keyboard(void) {
 live_reset();test_pc_desktop=1;pc_sample(DM_BTN_A|DM_BTN_B|DM_BTN_LS);CHECK(polled_buttons==(DM_BTN_A|DM_BTN_B|DM_BTN_LS)&&!native_calls,"PC69 no reinterpreta botones reales de mando como perfil Mac35");
 pc_sample((3ull<<40)|(15ull<<42)|DM_MAC_NAV_MASK|DM_MAC_POTION_MASK|DM_MAC_ZOOM);CHECK(!polled_buttons&&!native_calls,"Señales del perfil Mac35 inactivo no se filtran a PC69");pc_sample(0);
}

static void test_pc_inventory_world(void) {
 live_reset();test_pc_desktop=1;test_pc_world=1;test_ui_y=200;test_ui_h=1700;test_real_h=1900;dm_touch_host_pointer(.43f,.53f,1);float px=-1,py=-1;
 CHECK(dm_touch_host_world_pointer(&px,&py)&&px==.43f&&fabsf(py-.5794737f)<.000001f,"Consulta física fresca usa UI y una sola conversión Y para QuickCast");
 test_pc_world=0;px=py=-1;CHECK(!dm_touch_host_world_pointer(&px,&py)&&px==-1&&py==-1,"Al abrir inventario sobre XY anterior, la consulta actual rechaza esa casilla sin reutilizar allowed");test_pc_world=1;
 test_pc_policy=0;CHECK(!dm_touch_host_world_pointer(&px,&py),"Consulta actual falla con Bank/Vendor/modal aunque XY anterior fuera mundo");test_pc_policy=1;
 dm_touch_host_pointer(.43f,.53f,0);CHECK(!dm_touch_host_world_pointer(&px,&py),"PointerLeave invalida la consulta actual de QuickCast");
 CHECK(!dm_touch_host_world_pointer(NULL,&py)&&!dm_touch_host_world_pointer(&px,NULL),"Consulta de puntero con salidas nulas falla cerrada");
 live_reset();test_pc_desktop=1;test_pc_world=1;dm_tc_set_navigation_mode(&g_touch.state,1);test_ui_y=200;test_ui_h=1700;test_real_h=1900;
 live_event(0,.43f,.53f);live_event(1,.45f,.55f);live_event(2,0,0);
 CHECK(fabsf(mouse_native_y[0]-.5794737f)<.000001f,"Inventario usa una sola conversión UI901+offset200→render1101/1900");
 CHECK(mouse_native_calls>=3&&mouse_native_phase[0]==0&&mouse_native_phase[mouse_native_calls-1]==2&&!ptr_events,"Inventario no modal y punto descubierto conservan DOWN/HELD/UP de mundo");
 live_reset();test_pc_desktop=1;test_pc_world=1;dm_tc_set_navigation_mode(&g_touch.state,1);mouse_native_result=DM_PC_MOUSE_ONESHOT;
 live_event(0,.43f,.53f);mouse_native_result=1;live_event(0,.43f,.53f);live_event(1,.45f,.55f);live_poll();live_event(2,0,0);
 CHECK(mouse_native_calls==1&&!ptr_events,"Drop sostenido queda one-shot hasta UP aunque acuse vacíe held antes de HELD");
 live_event(0,.43f,.53f);live_event(2,0,0);CHECK(mouse_native_calls>=3&&mouse_native_phase[1]==0,"Un nuevo flanco tras soltar vuelve a permitir gesto normal");
 live_reset();test_pc_desktop=1;test_pc_world=0;dm_tc_set_navigation_mode(&g_touch.state,1);live_event(0,.9f,.6f);live_event(2,0,0);
 CHECK(!mouse_native_calls&&ptr_events==4,"Casillas/fondo/HUD mantienen la ruta UI aunque inventario sea no modal");
 live_reset();test_pc_desktop=1;test_pc_world=1;test_pc_policy=0;dm_tc_set_navigation_mode(&g_touch.state,1);live_event(0,.43f,.53f);live_event(2,0,0);
 CHECK(!mouse_native_calls&&ptr_events==4&&!skill_pointer_allowed,"Bank/vendor/modal bloquean mundo global y QuickCast aunque XY previo apuntara al mundo");
 live_reset();dm_tc_set_navigation_mode(&g_touch.state,1);CHECK(!dm_touch_host_world_active(),"Legacy conserva navigation_mode como bloqueo del mundo");
}
int main(void) {
    const char *path = "build/touch_host.script";
    FILE *f = fopen(path, "w"); if (!f) return 2;
    fputs("0 down 0 90,1220\n0.1 move 0 150,1220\n2.1 up 0\n", f); fclose(f);
    setenv("DM_TOUCH_SCRIPT", path, 1); unsetenv("DM_TOUCH_TEST_DROP");
    uint64_t b = 0; int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    test_tick = 100;
    dm_touch_host_step(0, &b, &lx, &ly, &rx, &ry);
    ui_cb("FrontEndNavigation", "ToGame", "");
    ui_cb("PanelManager", "UnloadPanel", "LoadScreenPanel"); test_loadscreen_down = 1;
    dm_touch_host_step(0, &b, &lx, &ly, &rx, &ry);
    CHECK(lx == 0 && ly == 0, "down 只建立摇杆原点");
    for (test_tick = 101; test_tick <= 162; test_tick++) {
        b = 0; lx = ly = rx = ry = 0;
        dm_touch_host_step(0, &b, &lx, &ly, &rx, &ry);
    }
    CHECK(lx > 0 && ly == 0, "move 写入宿主左摇杆");
    test_tick = 162;
    b = 0; lx = ly = rx = ry = 0; dm_touch_host_step(0, &b, &lx, &ly, &rx, &ry);
    CHECK(lx > 0 && ly == 0, "同一拍重复采样不推进脚本时间");
    test_tick = 212;
    b = 0; lx = ly = rx = ry = 0; dm_touch_host_step(0, &b, &lx, &ly, &rx, &ry);
    CHECK(lx == 0 && ly == 0, "up 清空宿主左摇杆");
    // 模式切换事务的清理（dm_mode_items.h 登记的「出」）：触摸排下的替按键在切换时作废；进局替按一次「显示物品」那种宿主自己发的不在此列
    // Cleanup of the mode switch transaction (the "leave" registered in dm_mode_items.h): key presses on the player's behalf queued by touch are voided on a switch; ones the host sends by itself, such as the single 「显示物品」(Show Items) press on entering a run, are not among them
    { uint64_t dm_ui_auto_buttons(void); void dm_ui_key_pulse(uint64_t, int), dm_ui_auto_mode_leave(void), dm_ui_cache_mode_leave(void);
      dm_ui_key_pulse(0x40, 1000); CHECK(dm_ui_auto_buttons() == 0x40, "触摸按钮的键脉冲在途：替按键里有这颗键");
      dm_ui_auto_mode_leave(); CHECK(dm_ui_auto_buttons() == 0, "切换清理后在途的键脉冲作废");
      dm_ui_cache_mode_leave(); CHECK(1, "界面缓存清理在没有界面根时不崩"); }
    test_pc_inventory_world();
    test_native_pc_mouse();
    test_pc_mouse_modifiers();
    test_pc_top_strip();
    test_pc_pointer_cancel();
    test_pc_mouse_alternatives();
    test_native_pc_keyboard();test_live_queue_full();
    test_live_mouse();
    test_pc_potions_navigation();
    test_right_mouse();
    test_pc_keyboard();
    printf("== touch host %d 项，失败 %d ==\n", checks, fails);
    return fails != 0;
}
void dm_shop_get(dm_shop_info *out) { memset(out, 0, sizeof *out); }   // 商人读数（dm_actions.c）：全 0 | vendor readout (dm_actions.c): all 0
void dm_item_use(unsigned id) { (void)id; }
int dm_ui_hover_now(int x, int y) { (void)x; (void)y; return 0; }
void dm_item_equip(unsigned id) { (void)id; }
void dm_item_unequip(unsigned id) { (void)id; }
int dm_held_size(int *w, int *h) { (void)w; (void)h; return 0; }
int dm_item_targeting(void) { return 0; }
int dm_vendor_repair_mode(void) { return 0; }
void dm_item_repair(uint32_t id) { (void)id; }
void dm_belt_fill(void) {}
void dm_belt_use_merc(int col,int row) { (void)col;(void)row; }
void dm_item_identify(unsigned id) { (void)id; }
void dm_item_target_cancel(void) {}
void dm_item_compare(int on) { (void)on; }
void dm_held_stow(void) {}
int dm_held_stow_result(void) { return -1; }
int dm_held_pending(void) { return 0; }
void uimsg_emit(const char *cat, const char *act, const char *arg) { (void)cat; (void)act; (void)arg; }
void dm_item_identify_free(unsigned id) { (void)id; }   // 免费鉴定（dm_actions.c）的桩 | stub for free identify (dm_actions.c)
int dm_runeword_long_press(const void *ci) { (void)ci; return 0; }   // 符文之语助手（dm_runeword.c）的桩 | stub for the runeword helper (dm_runeword.c)
int dm_runeword_list_open(void) { return 0; }
void dm_runeword_button(const char *name) { (void)name; }
void dm_runeword_list_close(void) {}
