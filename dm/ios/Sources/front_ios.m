#include <math.h>
// iOS 前端（front.h 的 iOS 实现）。App 在主线程建好 Metal 视图（front_ios_view），再在大栈线程上调 dm_main；
// front_run在该线程跑客体；真实触点保留dm_touch_set通路，独立控制器输出完整手柄状态。
// 原右半屏任意A占位已移除，只有控件命中会生成按键。
// iOS front end (the iOS implementation of front.h). The App builds the Metal view on the main thread (front_ios_view), then calls dm_main on a big-stack thread;
// front_run runs the guest on that thread; real touch points keep the dm_touch_set path, and the independent controller outputs the full controller state.
// The former any-A placeholder on the right half of the screen has been removed; only widget hits generate button presses.
#import <UIKit/UIKit.h>
static double g_dm_brightness = -1;
double dm_brightness_now(void) { return g_dm_brightness; }
static volatile int g_dm_thermal = -1;
int dm_thermal_now(void) { return g_dm_thermal; }   // 最近的温控档位（0 正常 / 1 微热 / 2 严重 / 3 危急；−1 = 未读）   // 最近一次读到的屏幕亮度（−1 = 未读） | Most recent thermal state (0 nominal / 1 fair / 2 serious / 3 critical; −1 = not read)   // Most recently read screen brightness (−1 = not read)
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include <signal.h>
#include "front.h"
#include "dm_input.h"
void dm_gamepad_start(void (*on_mode)(int pad)); void dm_gamepad_touched(void);   // dm_gamepad.m
#include "hle.h"
#import "DmTouchOverlay.h"
#include "dm_touch_native_art.h"
#include "dm_actions.h"
#include <stdatomic.h>
#include "dm_touch_game_state.h"
#include "../../host/dm_cursor.h"
#include "../../host/dm_hud.h"
#include "../../host/dm_world.h"
#include "../../host/dm_uiscreen.h"
#include "../../host/dm_actions.h"
#include "../../host/dm_i18n.h"
static NSString *SWT(const char *k) { return @(dm_ui(k)); }   // 我们自己的界面文字（中英表 dm/i18n/ui_text.tsv，语言跟原版设置走） | our own UI text (Chinese-English string table dm/i18n/ui_text.tsv, language follows the original game's setting)
static _Atomic int g_touch_frontend=1;   // 不在局内（启动即是）：scene.game 为假 | Not in a run (true right at launch): scene.game is false
static void hud_stack_watch(void);   // 底部 UI v4 面板栈看门（定义见 touch_ui_message 前） | Bottom UI v4 panel stack watch (defined before touch_ui_message)
// 旧生命周期实验可省略桥；生产Sources自动包含强实现。
// The old lifecycle experiment may omit the bridge; production Sources automatically include the strong implementation.
__attribute__((weak)) int dm_touch_game_attach(void) { return 0; }
__attribute__((weak)) int dm_touch_game_snapshot(dm_tg_snapshot *out) {
    if(out) { memset(out,0,sizeof(*out)); out->portal.slot=-1; }
    return 0;
}

__attribute__((weak)) void dm_touch_game_set_scene(int game,uint64_t epoch,uint64_t world) { (void)game; (void)epoch; (void)world; }
__attribute__((weak)) uint64_t dm_tg_portal_mask(const dm_tg_snapshot *s,uint64_t n) {
    (void)s; (void)n; return 0; // 旧lab的snapshot.supported=0；生产由纯C强实现覆盖。 | The old lab's snapshot.supported=0; in production the pure-C strong implementation overrides it.
}

static CAMetalLayer *g_layer;

// 仅诊断；复用 HLE 的宿主单调时钟 now_ns()，不使用可冻结的 guest_now_ns()。
// Diagnostics only; reuses HLE's host monotonic clock now_ns(), not the freezable guest_now_ns().
static BOOL touch_control_log_enabled(void) {
    static int enabled=-1;
    if(enabled<0) enabled=getenv("DM_TOUCH_CONTROL_LOG")!=NULL;
    return enabled;
}

// 真实触点 → 宿主（nn::hid GetTouchScreenStates 由宿主按 TouchScreenState 布局返回）。
// 每次触点集合变化把当前全部触点整组推给宿主：id = 手指槽号 0–15（按下时取最小空槽，抬起前不变），
// x / y = 在出图画面上的归一化坐标 [0, 1)（左上为原点）——宿主按游戏当前档位的坐标系换算（主机 1952x900 / 掌机 1568x720，
// 过热会切档，所以不在前端定死像素；宿主按游戏的 1280x720 面板坐标换算）。dm_touch / dm_touch_set 声明在 front.h，实现在 hle_touch.c。
// Real touch points → host (nn::hid GetTouchScreenStates is returned by the host in the TouchScreenState layout).
// Every time the set of touch points changes, push all current touch points to the host as a group: id = finger slot number 0–15 (lowest free slot taken on press, unchanged until lift),
// x / y = normalized coordinates on the output picture [0, 1) (origin at top left) -- the host converts according to the coordinate system of the game's current profile (docked 1952x900 / handheld 1568x720;
// overheating switches profiles, so pixels are not fixed in the front end; the host converts using the game's 1280x720 panel coordinates). dm_touch / dm_touch_set are declared in front.h and implemented in hle_touch.c.

@interface DmMetalView : UIView
- (void)cancelControls;
- (void)tickTouchControls:(double)now;
- (void)navigationMode:(int)navigation;   // 0 游戏态 1 导航态 2 导航态且死亡弹窗开着（不给手柄圆钮） | 0 game mode, 1 navigation mode, 2 navigation mode with the death popup open (no round controller buttons)
- (void)hudBlocked:(int)n;
- (void)frontendMode:(BOOL)fe;
- (void)padMode:(BOOL)pad;
- (void)restartCover:(int)mode;
- (void)captureTouchLayer:(NSString *)path;
- (void)refreshPortalBinding;
@end
static __weak DmMetalView *g_touch_view;
// 真全屏（屏幕比 16:9 窄，iPad 4:3）：3D 铺满整窗，游戏视图（界面、自绘控件、界面触点）是贴底的一块 16:9，见 front_ios_view。
// 世界那一侧（点怪 / 点箱子 / 单位投影）按整窗算：world_rect = 整窗在游戏视图坐标里的矩形；不是真全屏时就是游戏视图自己。
// True fullscreen (screen narrower than 16:9, iPad 4:3): 3D fills the whole window, and the game view (UI, custom-drawn widgets, UI touch points) is a bottom-anchored 16:9 block, see front_ios_view.
// The world side (tapping monsters / chests / unit projection) is computed on the whole window: world_rect = the whole window's rectangle in game-view coordinates; when not true fullscreen it is the game view itself.
static int g_full;
static _Atomic uint64_t g_res_now;        // 当前出图尺寸 {u32 宽, u32 高}（真全屏下随窗口变，见 front_game_res 上的说明）
static uint64_t g_res_cap;                // 报给游戏建窗口的尺寸（上限）
static CGRect world_rect(UIView *v) { UIView *b = v.superview; int x, y, w, h, rw = 0, rh = 0; if (!g_full || !b) return v.bounds; dm_ui_rect(&x, &y, &w, &h);
    return dm_screen_real(&rw, &rh) && h > 0 && h < rh ? [v convertRect:b.bounds fromView:b] : v.bounds; }   // 局外（界面矩形就是整个画面）时世界与游戏视图重合 | Out of a run (the UI rectangle is the whole picture) the world and the game view coincide
static void world_norm(UIView *v, CGFloat x, CGFloat y, float *nx, float *ny) { CGRect r = world_rect(v); *nx = (float)((x - r.origin.x) / r.size.width); *ny = (float)((y - r.origin.y) / r.size.height); }
// 测试脚本触点（DM_TOUCH_SCRIPT）：走与真手指相同的 touchesBegan/Moved/Ended 路径，只把位置换成脚本给的点。
// Test script touch points (DM_TOUCH_SCRIPT): go through the same touchesBegan/Moved/Ended path as real fingers, only replacing the position with the point given by the script.
@interface DmScriptTouch : UITouch
@property (nonatomic) CGPoint scriptPoint;
@property (nonatomic) UITouchPhase scriptPhase;   // 抬起 / 取消时置 Ended / Cancelled，与真触摸一样（扫槽兜底靠它认「已结束」） | Set to Ended / Cancelled on lift / cancel, same as a real touch (the slot-scan backstop relies on it to recognize "ended")
@end
@interface DmVolumeWatch : NSObject
@end
@implementation DmVolumeWatch
- (void)observeValueForKeyPath:(NSString *)k ofObject:(id)o change:(NSDictionary *)c context:(void *)x {
    (void)k; (void)o; (void)x; double mtl_link_tick_age_ms(void);
    LOG("前端：系统音量 %.2f → %.2f（显示节拍上次回调 %.0f ms 前）", [c[NSKeyValueChangeOldKey] doubleValue], [c[NSKeyValueChangeNewKey] doubleValue], mtl_link_tick_age_ms());
}
@end
@implementation DmScriptTouch
- (CGPoint)locationInView:(UIView *)v { (void)v; return _scriptPoint; }
- (CGPoint)previousLocationInView:(UIView *)v { (void)v; return _scriptPoint; }
- (NSTimeInterval)timestamp { return CACurrentMediaTime(); }
- (UITouchPhase)phase { return _scriptPhase; }
@end
static void touch_script_start(void);
static _Atomic int g_touch_navigation = 1;

static _Atomic int g_nav_pub; int dm_front_navigation(void) { return atomic_load(&g_nav_pub); }   // 面板 / 读取画面 / 过场开着（非零）：dm_gamepad.m 据此只在游戏态接管 ZR | A panel / loading screen / cutscene is open (non-zero): dm_gamepad.m uses this to take over ZR only in the gameplay state
static void pad_mode_changed(int pad) { dispatch_async(dispatch_get_main_queue(),^{ [g_touch_view padMode:pad]; }); }   // dm_gamepad.m 在主队列回调，照样切一次主线程（与上面同法） | dm_gamepad.m calls back on the main queue; still hop to the main thread once (same approach as above)
static void restart_cover_hook(int on) { dispatch_async(dispatch_get_main_queue(),^{ [g_touch_view restartCover:on]; }); }   // 0 撤 / 1 黑屏 / 2 透明（读取画面露出来，照样吞触点）   // 宿主在游戏主线程调，切到界面主线程开关 | 0 remove / 1 black screen / 2 transparent (the loading screen shows through, touch points still swallowed)   // Called by the host on the game main thread; switches to the UI main thread to toggle
// 一键重开遮罩：吞掉落在它上面的所有触点（不调 super，不沿响应链交给下面的游戏视图——诊断日志里见过：遮罩是游戏视图的子视图时，
// 落在遮罩上的点按经响应链交给了游戏视图，黑屏期间点到选角页的「开始」就自己进了局）
// One-tap restart mask: swallows all touch points landing on it (does not call super and does not pass them down the responder chain to the game view below -- seen in diagnostic logs: when the mask was a subview of the game view,
// taps landing on the mask were passed through the responder chain to the game view, and during the black screen a tap hit 「开始」(PLAY) on the character select page and entered a run on its own)
@interface DmSwallowView : UIView
@end
@implementation DmSwallowView
- (void)touchesBegan:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e {}
- (void)touchesMoved:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e {}
- (void)touchesEnded:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e {}
- (void)touchesCancelled:(NSSet<UITouch *> *)t withEvent:(UIEvent *)e {}
@end
@implementation DmMetalView {
    UITouch *_slot[16];   // 强引用（真机 SIGSEGV：原先不保留，某条路径没清槽时手指对象已释放，下次 pushFrom 读它就崩）。所有结束 / 取消路径都要清槽 | Strong references (real-device SIGSEGV: previously not retained, so when some path failed to clear the slot the finger object was already released and the next pushFrom crashed reading it). All end / cancel paths must clear the slot
    dm_tc_state _controls;
    DmTouchOverlay *_overlay;
    UIView *_info;   // 信息层：秘境任务条、小地图任务终点。不是触屏控件，手柄模式也要显示（只在前端页面藏），所以不挂在 _overlay 上 | info layer: the Rift objective bar and the minimap quest goal. These are not touch controls and must show in controller mode too (hidden only on front-end pages), so they are not attached to _overlay
    NSHashTable<UITouch *> *_swallow;   // 手柄模式下落下的手指：只用来切回触屏模式，整根（按下 / 移动 / 抬起）不交给手势层和界面点击 | Fingers that landed in controller mode: used only to switch back to touch mode; the whole touch (down / move / up) is not passed to the gesture layer or UI clicks
    BOOL _padMode, _feMode;   // 手柄模式 / 前端页面：两者任一成立就藏触屏控件层 | Controller mode / front-end page: the touch controls layer is hidden if either holds
    UIView *_rsCover;   // 一键重开期间的黑屏（宿主 dm_cursor.c 经 g_dm_restart_cover_hook 开关）：盖在窗口最上层（整屏，含真全屏的留边），不露主菜单，吞掉这段时间的触点 | Black screen during one-tap restart (toggled by the host's dm_cursor.c via g_dm_restart_cover_hook): covers the top of the window (whole screen, including the true-fullscreen margins), does not reveal the main menu, and swallows touch points during this period
    int _navigation;
    CALayer *_mhPtr, *_thr[8];   // _thr：画面外怪物的指针（threatsSync） | _thr: pointers for monsters off the picture (threatsSync)
      // 镜头实验：小地图 / 全屏地图上人物朝向的指针（mapHeadingSync） | camera experiment: the facing pointer on the minimap / fullscreen map (mapHeadingSync)
    CALayer *_mgLabel[12], *_mgMark[12];   // 小地图任务终点：每个终点一个名牌 / 符号层 + 一个灰十字层（mapGoalSync） | Minimap quest goals: one nameplate / symbol layer + one grey cross layer per goal (mapGoalSync)
    NSMutableDictionary<NSString *,UIImage *> *_mgImg;
    uint64_t _modifierPublishEndNS,_modifierPublishEpoch;
    uint64_t _touchEventSeq,_touchReceiptSeq,_touchPublishedEventSeq; // 仅 DM_TOUCH_CONTROL_LOG 开启时推进 | Advanced only when DM_TOUCH_CONTROL_LOG is on
    UITouch *_pendEnd; double _pendEndTime,_pendWall; BOOL _pendFire,_finalAsked;   // 松手了但宿主的答案还没回：这根手指的「抬起」先挂着（见 finishTouches） | Released but the host's answer is not back yet: this finger's "lift" is held pending (see finishTouches)
    int32_t _minLeft,_maxLeft; uint64_t _lastButtons; unsigned _touchAPresses,_touchShots; UITouch *_uiFinger; CGPoint _uiFeO; BOOL _uiFeDrag; BOOL _uiDragOn; UITouch *_mapTouch, *_mapBackTouch, *_bindTouch; UIView *_ghostDim; BOOL _ghostOn; uint64_t _bindKey; uint32_t _selId; double _statNext; double _tipUntil, _addAt; unsigned _mapWorldSeq; BOOL _mapWorldAsked; CGPoint _mapWorldPt; CGPoint _mapP0, _bindP0; double _mapT0; BOOL _pulseTicking; uint64_t _pulseEpoch,_pulseS0; double _pulseT0,_pulseSeen,_pulseFree; unsigned _itemSeq,_relSeq; BOOL _relAsked,_itemAsked; int _itemKind;
}
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)f {
    if (!(self = [super initWithFrame:f])) return nil;
    self.multipleTouchEnabled = YES;
    self.isAccessibilityElement=YES; self.accessibilityIdentifier=@"dm_touch_surface"; self.accessibilityLabel=SWT("dmA11ySurface");
    dm_tc_init(&_controls,f.size.width,f.size.height); _controls.mouse_click_mode=1;   // 功能页里非物品区的点按走界面鼠标点击 | Taps outside the item area in function pages go through UI mouse clicks
    _overlay=[[DmTouchOverlay alloc] initWithFrame:self.bounds]; _overlay.controls=&_controls; _overlay.hidden=YES;   // 启动即前端页面：进局（读取画面撤下）才显示 | Front-end page right at launch: shown only after entering a run (loading screen removed)
    [self addSubview:_overlay]; _info=[[UIView alloc] initWithFrame:self.bounds]; _info.userInteractionEnabled=NO; _info.hidden=YES; [self addSubview:_info]; g_touch_view=self; dm_gamepad_start(pad_mode_changed);
    _rsCover=[[DmSwallowView alloc] initWithFrame:self.bounds]; _rsCover.backgroundColor=UIColor.blackColor; _rsCover.hidden=YES; _rsCover.userInteractionEnabled=YES;   // 开的时候挂到窗口上（restartCover） | Attached to the window when turned on (restartCover)
    { extern void (*g_dm_restart_cover_hook)(int); g_dm_restart_cover_hook=restart_cover_hook; }
    _navigation=0; [self navigationMode:atomic_load(&g_touch_navigation)];
    return self;
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    CAMetalLayer *l=(CAMetalLayer *)self.layer;
    if(l==g_layer) l.drawableSize=CGSizeMake(self.bounds.size.width*l.contentsScale,self.bounds.size.height*l.contentsScale);   // 真全屏（iPad）时出图层挂在外层容器上，不是这一层 | In true fullscreen (iPad) the present layer hangs on the outer container, not this layer
    UIEdgeInsets si=self.safeAreaInsets; static CGFloat sl=-1,sr=-1,sb=-1; BOOL safeChanged=(si.left!=sl || si.right!=sr || si.bottom!=sb);   // 安全区变了（首次布局 / 转屏）也要重排控件 | A safe area change (first layout / rotation) also requires re-laying out the widgets
    if (safeChanged) { sl=si.left; sr=si.right; sb=si.bottom; dm_tc_set_safe_bottom((float)si.bottom); dm_tc_set_safe_insets((float)si.left,(float)si.right); }
    // 只对 iPad：底部安全区那一条不起摇杆（见 dm_tc_set_edge_bottom）。视图不贴屏幕底（留边档）时这个值自然是 0。DM_TEST_EDGE_B：Mac 上没有底部安全区，测试用
    // iPad only: the bottom safe area strip does not start the stick (see dm_tc_set_edge_bottom). When the view does not touch the bottom of the screen (margin mode) this value is naturally 0. DM_TEST_EDGE_B: Mac has no bottom safe area, for testing
    { const char *tb=getenv("DM_TEST_EDGE_B"); dm_tc_set_edge_bottom(tb ? (float)atof(tb) : UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad ? (float)si.bottom : 0); }
    { const char *tl=getenv("DM_TEST_SAFE_L"); CGFloat l=tl ? atof(tl) : si.left, hh=self.bounds.size.height; if(hh>0) dm_ui_safe_left((int)lround(l/hh*2160)); }   // 左上角佣兵头像内移到安全线内 | The top-left mercenary portrait is moved inside the safe line
    { const char *tj=getenv("DM_TEST_JOY_ISO"); if(tj) dm_tc_set_iso_fix(atoi(tj)); }   // 测量用：0 = 关掉移动摇杆斜向预修正 | For measurement: 0 = turn off the diagonal pre-correction of the movement stick
    { const char *ts=getenv("DM_TEST_SAFE_R"); CGFloat r=ts ? atof(ts) : si.right, hh=self.bounds.size.height; if(hh>0) dm_ui_safe_right((int)lround(r/hh*2160)); }   // 右上角小地图与区域名内移到安全线内；DM_TEST_SAFE_R=<点> 供 Mac 上验（Mac 窗口安全区是 0） | The top-right minimap and area name are moved inside the safe line; DM_TEST_SAFE_R=<points> is for verification on Mac (the Mac window safe area is 0)
    // 平板缩放（控件最多放大 15%）：iPad 一律开；手机按窗口短边判，iPhone Duo 内屏（短边约 669 点、接近 iPad mini）开、外屏与普通 iPhone（≤ 470 点）不开
    static int tall=-1; int t=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad || fmin(self.bounds.size.width,self.bounds.size.height)>=600; BOOL tallChanged=t!=tall;
    if (tallChanged) { tall=t; dm_tc_set_tall_scale(t); }
    if (safeChanged || tallChanged || _controls.layout.width!=self.bounds.size.width || _controls.layout.height!=self.bounds.size.height) {
        [self cancelControlsFrom:"layout"]; dm_tc_resize(&_controls,self.bounds.size.width,self.bounds.size.height);
    }
    _overlay.frame=self.bounds; _info.frame=self.bounds; [_overlay setNeedsDisplay]; if(!_rsCover.hidden && self.window) _rsCover.frame=self.window.bounds;
}
- (void)restartCover:(int)mode {   // 挂在窗口最上层、整窗大小：盖住游戏视图之外的留边与别的兄弟视图，触点全落在它身上被吞掉。2 = 透明（读取画面露出来） | Attached at the top of the window at full window size: covers the margins outside the game view and other sibling views, and all touch points land on it and are swallowed. 2 = transparent (the loading screen shows through)
    UIView *host=self.window ?: self; if(mode){ if(_rsCover.superview!=host) [host addSubview:_rsCover]; _rsCover.frame=host.bounds; [host bringSubviewToFront:_rsCover]; }
    _rsCover.backgroundColor = mode==1 ? UIColor.blackColor : UIColor.clearColor; _rsCover.hidden=!mode;
    LOG("前端：一键重开遮罩%s（挂在%s，%.0fx%.0f）", mode==1 ? "黑屏盖上" : mode==2 ? "改透明、仍吞触点" : "撤掉", host==self ? "游戏视图" : "窗口", host.bounds.size.width, host.bounds.size.height); }
- (void)hudBlocked:(int)n { _overlay.hudBlocked=n>0; _overlay.dialogOnly=n<0; dm_tc_set_joy_block(&_controls,n<0); [_overlay setNeedsDisplay]; }   // 只开着讲话文字框 / 对话菜单：点底座那一下不起摇杆（只关它，人不走） | only the speech text box / dialogue menu is open: a tap on the base starts no stick (it only closes it, the character does not move)
// 前端页面（主菜单 / 选角 / 建角 / 读取画面，即不在局内）：不显示摇杆与技能圆钮，触点直接当原生点击交给游戏界面（dm_ui_touch → 鼠标消息，dm_cursor.c）
// Front-end pages (main menu / character select / character creation / loading screen, i.e. not in a run): no stick or round skill buttons shown; touch points are given straight to the game UI as native clicks (dm_ui_touch → mouse messages, dm_cursor.c)
- (void)padMode:(BOOL)pad { _padMode=pad; _overlay.hidden=_feMode || pad; _info.hidden=_feMode; if(pad) { _tipUntil=-2; dm_tc_cancel_all(&_controls); [self pushFrom:"padmode" phase:"none"]; } }   // 手柄模式藏触屏控件层（dm_gamepad.m 回调） | Controller mode hides the touch controls layer (dm_gamepad.m callback)
- (void)frontendMode:(BOOL)fe { _feMode=fe; _overlay.hidden=fe || _padMode; _info.hidden=fe; if(fe) { _pendEnd=nil; _finalAsked=NO; for(int i_=0;i_<16;i_++) _slot[i_]=nil; dm_tc_cancel_all(&_controls); [self pushFrom:"frontend" phase:"none"]; } }
- (void)navigationMode:(int)n {
    if (_navigation==n) return;
    dm_tc_set_navigation_mode(&_controls,n); _navigation=n; atomic_store(&g_nav_pub,n);   // 内部按非零归一 | Internally normalized to non-zero
    static int pad=-1; if(pad<0) { const char *e=getenv("DM_TOUCH_NAV_PAD"); pad=e && e[0]=='1'; }   // 面板态的五个虚拟手柄圆钮：缺省不给（点按 / 双击 / 拖放 / 点页签 / 面板外点按已全部替代，圆钮还压着背包格右半边）；DM_TOUCH_NAV_PAD=1 调试时叫回来 | The five round virtual controller buttons of panel mode: not shown by default (tap / double tap / drag and drop / tab tap / tap outside the panel have replaced them all, and the round buttons also sat over the right half of the inventory cells); DM_TOUCH_NAV_PAD=1 brings them back for debugging
    uint32_t mask=n ? (dm_tc_nav_mask(pad && n!=2) | (n==3 ? 1u<<DM_TC_MENU : 0)) :   // 3 = 只开着 NPC 对话菜单：底栏十字可点（原版 ＋ 键此时 = 关对话并开功能页） | 3 = only the NPC dialog menu is open: the bottom bar cross is tappable (the original game's + key at this time = close the dialog and open the function page)
        (((1u<<DM_TC_COUNT)-1) & ~(1u<<DM_TC_MAP));   // 游戏态不要右上地图键（不要顶部图标） | No top-right map button in game mode (no top icon wanted)
    dm_tc_enable_targets(&_controls,mask); _overlay.navigationMode=n!=0; [self pushFrom:"navigation" phase:"none"];
    if(!n && !getenv("DM_TOUCH_SCRIPT_START")) touch_script_start();
    if(!n) [self itemLabelsOn];
    dm_ui_map_hold(n!=0);   // 全屏地图的面板盖满整屏，会挡在别的面板前面：面板开着先让成小地图，关掉回到玩家要的那档（宿主 ui_map_pump） | the full-screen map's panel covers the whole screen and would sit in front of other panels: while a panel is open it steps aside to the minimap, and after it closes it returns to what the player wants (host ui_map_pump)
}
// 掉落物名字默认常显：设置缺省已是「显示物品 = 开关式」（hle_fs.c settings_seed），开关式下进程启动后初始是关，
// 点一次「显示物品」键（手柄 L）即一直显示，退出重进下一局仍保持。所以每个进程只点一次：首次进到局内常态（无面板、无读取画面）1.5 秒后，
// 那时仍在局内常态才点（L 在面板里是「上一页签」，不能在面板开着时点）；被打断就等下一次回到常态再试。玩家把那一项改回「长按」后这一下只是闪一下标签，无害。
// DM_TOUCH_DEFAULTS=0 关（与设置缺省同一个开关）。
// Dropped item names shown by default: the settings default is already 「显示物品」(Show Items) = toggle style (hle_fs.c settings_seed); in toggle style it starts off after process launch,
// and one press of the 「显示物品」(Show Items) key (controller L) keeps them shown, persisting into the next run after exit and re-entry. So press only once per process: 1.5 seconds after first reaching the normal in-run state (no panel, no loading screen),
// and only if still in the normal in-run state then (in a panel L is "previous tab" and must not be pressed with a panel open); if interrupted, retry the next time the normal state is reached. If the player changed that option back to 「长按」(Hold), this press merely flashes the labels, harmless.
// DM_TOUCH_DEFAULTS=0 turns it off (same switch as the settings defaults).
- (void)itemLabelsOn {
    static int done, armed; const char *e=getenv("DM_TOUCH_DEFAULTS");
    if(done || armed || (e && !strcmp(e,"0"))) return;
    armed=1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.5*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        armed=0; if(self->_navigation || done) return;
        // 测试开关 DM_TEST_LKEY_CLASH（造「显示物品」键撞开背包的现场）：同一刻替玩家按菜单键开物品栏；=old 时 L 走改前的做法（不经裁决、同一下按出去），作对照
        // Test switch DM_TEST_LKEY_CLASH (reproduces the 「显示物品」(Show Items) key colliding and opening the inventory): press the menu key on the player's behalf at the same instant to open the inventory; with =old, L uses the pre-change approach (no arbitration, pressed out at the same time), as a control
        const char *tc=getenv("DM_TEST_LKEY_CLASH"); void dm_ui_key_pulse(uint64_t,int); void dm_ui_key_pulse_alone(uint64_t,int); done=1;
        if(tc && !strcmp(tc,"old")) { dm_ui_key_pulse(DM_BTN_PLUS,6); LOG("测试 DM_TEST_LKEY_CLASH=old：先按菜单键，33 毫秒后 L 不经裁决直接按出去（改前的做法）");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.033*NSEC_PER_SEC)),dispatch_get_main_queue(),^{ dm_ui_key_pulse(DM_BTN_L,9); }); return; }
        dm_ui_key_pulse_alone(DM_BTN_L,9);
        if(tc && !strcmp(tc,"pad")) { void dm_gamepad_inject(uint64_t,int32_t,int32_t,int32_t,int32_t); dm_gamepad_inject(DM_BTN_PLUS,0,0,0,0); LOG("测试 DM_TEST_LKEY_CLASH=pad：同一刻由虚拟手柄按菜单键（开物品栏）");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.12*NSEC_PER_SEC)),dispatch_get_main_queue(),^{ dm_gamepad_inject(0,0,0,0,0); }); }
        else if(tc) { dm_ui_key_pulse(DM_BTN_PLUS,6); LOG("测试 DM_TEST_LKEY_CLASH：同一刻替玩家按菜单键（开物品栏）"); }   // 真正发出的时刻由宿主在游戏取手柄处裁决（没有面板、没有别的键），日志在那里打 | The actual moment of sending is arbitrated by the host where the game polls the controller (no panel, no other key); the log is printed there
    });
}
- (void)recordTouch:(UITouch *)touch slot:(int)slot phase:(const char *)phase {
    if(!touch_control_log_enabled()) return;
    CGPoint p=[touch locationInView:self];
    fprintf(stderr,"[touch-receipt] stage=event eventseq=%llu mono_ns=%llu source=uitouch phase=%s slot=%d touch=%p touch_timestamp_s=%.9f x=%.6f y=%.6f buttons=%#llx lx=%d ly=%d rx=%d ry=%d mode=%s\n",
        (unsigned long long)++_touchEventSeq,(unsigned long long)now_ns(),phase,slot,(__bridge void *)touch,touch.timestamp,p.x,p.y,
        (unsigned long long)_controls.buttons,_controls.lx,_controls.ly,_controls.rx,_controls.ry,_navigation?"navigation":"game");
}
- (void)pushFrom:(const char *)source phase:(const char *)phase {
    dm_touch ts[16]; int n=0; CGSize b=self.bounds.size;
    if (b.width>0 && b.height>0) for(int i=0;i<16;i++) if(_slot[i]) {
        CGPoint p=[_slot[i] locationInView:self];
        ts[n++]=(dm_touch){(uint32_t)i,(float)fmin(fmax(p.x/b.width,0),.99999),(float)fmin(fmax(p.y/b.height,0),.99999)};
    }
    dm_touch_set(ts,n);
    BOOL diagnostic=touch_control_log_enabled();
    uint64_t publishStart=diagnostic?now_ns():0;
    // 指针登记为待写光标、再发布按键：宿主在游戏读手柄时把两者一起落地（dm_cursor.h）
    // Register the pointer as a pending cursor write, then publish the keys: the host applies both together when the game reads the controller (dm_cursor.h)
    if (_controls.pointer_valid) dm_cursor_set_screen(_controls.pointer_x,_controls.pointer_y,b.width,b.height); else dm_cursor_clear();
    dm_input_set(_controls.buttons,_controls.lx,_controls.ly,_controls.rx,_controls.ry);
    if (_controls.belt_request) { int br=_controls.belt_request; _controls.belt_request=0;   // 腰带展开 / 收起 / 喝上面几排的药（dm_actions.c） | Belt expand / collapse / drink a potion from the upper rows (dm_actions.c)
        if (br==1 || br==2) dm_belt_expand(br==1); else dm_belt_use(br&0xf, br>>4&0xf); }   // 腰带点按一律给玩家喝 | belt taps always go to the player
    if (_controls.diff_request) { dm_hud_request_players(_controls.diff_request); _controls.diff_request=0; }   // 难度选择条选中档位：宿主下一拍走原版 /players 路径 | Difficulty selector strip picked a level: the host takes the original game's /players path on the next tick
    uint64_t publishEnd=(diagnostic || _controls.portal_pending)?now_ns():0;
    if(_controls.portal_pending && _modifierPublishEpoch!=_controls.gesture_epoch) {
        _modifierPublishEpoch=_controls.gesture_epoch;_modifierPublishEndNS=publishEnd;
    }
    if(!_controls.portal_phase) {_modifierPublishEpoch=0;_modifierPublishEndNS=0;}
    _minLeft=MIN(_minLeft,_controls.lx); _maxLeft=MAX(_maxLeft,_controls.lx);
    if((_controls.buttons & DM_BTN_A) && !(_lastButtons & DM_BTN_A))_touchAPresses++;
    _lastButtons=_controls.buttons;
    self.accessibilityValue=[NSString stringWithFormat:@"%@|buttons=%llu|lx=%d|ly=%d|min=%d|max=%d|a=%u|shot=%u",_navigation?@"navigation":@"game",_controls.buttons,_controls.lx,_controls.ly,_minLeft,_maxLeft,_touchAPresses,_touchShots];
    if(diagnostic) {
        uint64_t first=_touchEventSeq>_touchPublishedEventSeq?_touchPublishedEventSeq+1:0;
        fprintf(stderr,"[touch-receipt] stage=output receiptseq=%llu eventseq=%llu event_first_seq=%llu event_last_seq=%llu mono_ns=%llu publish_end_ns=%llu source=%s phase=%s buttons=%#llx lx=%d ly=%d rx=%d ry=%d mode=%s portal_request=%llu portal_binding=%llu portal_scene=%llu portal_phase=%d portal_pending=%d modifier_publish_end_ns=%llu feedback_group_start_ns=%llu feedback_captured_ns=%llu feedback_secondary=%d feedback_seen=%u feedback_identity_scope=%d\n",
        (unsigned long long)++_touchReceiptSeq,(unsigned long long)_touchEventSeq,(unsigned long long)first,(unsigned long long)_touchEventSeq,(unsigned long long)publishStart,
        (unsigned long long)publishEnd,source,phase,(unsigned long long)_controls.buttons,_controls.lx,_controls.ly,_controls.rx,_controls.ry,_navigation?"navigation":"game",
        (unsigned long long)_controls.gesture_epoch,(unsigned long long)_controls.portal_binding_identity,(unsigned long long)_controls.portal_scene_epoch,
        _controls.portal_phase,_controls.portal_pending,(unsigned long long)_modifierPublishEndNS,
        (unsigned long long)_overlay.nativeMapping.group_started_ns,(unsigned long long)_overlay.nativeMapping.captured_ns,
        _overlay.nativeMapping.secondary_group,_overlay.nativeMapping.group_seen,_overlay.nativeMapping.identity_scope);
        _touchPublishedEventSeq=_touchEventSeq;
    }
    [_overlay setNeedsDisplay];
}
- (void)cancelControlsFrom:(const char *)source {
    _pendEnd=nil; _finalAsked=NO;
    for(int i_=0;i_<16;i_++) _slot[i_]=nil; dm_tc_cancel_all(&_controls); [self pushFrom:source phase:"none"];
    if(_uiDragOn) { _uiDragOn=NO; dm_ui_drag_end(); }
    _mapTouch=_mapBackTouch=_bindTouch=nil;
}
// 技能绑定入口（技能为空时一个单色十字，点击进入绑定技能）：游戏态下触点落在技能钮（主攻击大钮 A 与 F1–F4，含当前组）上——
//   空位：这根手指不进手势层，点一下 = 给这个位选技能（dm_ui_bind_begin，宿主开技能树）；
//   已绑定：照常进手势层（按下瞄准、松手施放、拖拽定方向）；按住不动超过 0.5 秒且位移 ≤ 10 点 = 长按换绑，取消这次施放（tickTouchControls 里判）。攻击键按住是连续攻击，不做长按。
// 返回这个点命中的技能钮要绑的手柄键（第二组带 ZL），没命中返回 0；*empty = 该位当前是空的。
// 主攻击大钮（DM_TC_MAIN，A 键）也在内（复现：主技能置空后点它的「＋」弹不出选技能——原先这里只认 F1–F4，主钮空着时这一下进了手势层当攻击按下去）；
// 宿主的绑定流程本来就认 A（绑定框第 1 列 = A = 主技能，dm_cursor.c ui_bind_modal_tap）。长按换绑只给 F1–F4（攻击键按住是连续攻击），这里只管「空位点一下」
// Skill binding entry (a monochrome cross when the skill is empty; tap to bind a skill): in game state, a touch landing on a skill button (main attack button A and F1–F4, including the current set):
//   Empty slot: this finger does not enter the gesture layer; a tap = choose a skill for this slot (dm_ui_bind_begin, the host opens the skill tree);
//   Bound: enters the gesture layer as usual (press to aim, release to cast, drag to set direction); holding still for over 0.5 seconds with movement ≤ 10 points = long-press rebind, cancelling this cast (decided in tickTouchControls). Holding the attack key is continuous attack, so it has no long press.
// Returns the gamepad key to bind for the skill button hit by this point (second set includes ZL), 0 if none hit; *empty = the slot is currently empty.
// The main attack button (DM_TC_MAIN, A key) is included too (repro: after clearing the main skill, tapping its "+" did not bring up skill selection; this used to accept only F1–F4, so with the main button empty the tap entered the gesture layer as an attack press);
// the host binding flow already accepts A (binding dialog column 1 = A = main skill, dm_cursor.c ui_bind_modal_tap). Long-press rebind is only for F1–F4 (holding the attack key is continuous attack); this only handles "tap on an empty slot"
- (uint64_t)skillKeyAt:(CGPoint)p empty:(BOOL *)empty {
    int i=dm_tc_hit(&_controls.layout,(float)p.x,(float)p.y);   // 与手势层同一个命中判定 | Same hit test as the gesture layer
    if((i==DM_TC_MAIN || (i>=DM_TC_SKILL1 && i<=DM_TC_SKILL4)) && (_controls.enabled_targets&(1u<<i))) { const dm_tc_target *t=&_controls.layout.target[i];
        dm_tg_snapshot n=_overlay.nativeMapping; uint64_t now_ns(void);
        dm_ta_glyph g=dm_ta_current_set(&n,t->button,now_ns(),_controls.skill_set); *empty=g.status==DM_TA_EMPTY;   // 与画十字用同一份读数（DmTouchOverlay） | Same reading as used for drawing the cross (DmTouchOverlay)
        return t->button|(_controls.skill_set?DM_BTN_ZL:0); }
    return 0;
}
// 右上角小地图 = 地图入口（地图在右上角；不要顶部图标 → 不画任何东西）。矩形取游戏自己的 AutoMap 面板（宿主每半秒读一次，面板关着时沿用最近一次）。
// 点击区只是矩形顶部区域名那一条（全宽 × 高 1/4，不足 30 点补到 30、不超过矩形高的 0.4），不是整块（线上「小地图消失」：整块矩形占屏宽 1/3 × 屏高 1/3，
// 微缩地图以玩家为中心画在矩形中间，那片正是玩家往右上方点地面的位置——一份诊断日志里 24 次落在矩形里的点击 20 次在中间主体、0 次在顶部那条；
// 点地面被切成全屏地图，右上角小地图没了）。矩形其余部分照常进手势层（点世界）。
// 只在游戏态认；落在里面的手指不进手势层（不占摇杆 / 技能的手指槽、不发任何移动），点一下（位移 ≤ 14 点、0.6 秒内）= 切到全屏地图（宿主调游戏自己的「透视地图尺寸」变更回调，不落盘，见 dm_cursor.c ui_map_pump）。
// 全屏地图开着时：再点小地图那一块、或点任何没落在技能键 / 摇杆区上的地方 = 回到小地图；摇杆照常走路、技能键照常放（开着全屏地图赶路是暗黑的常规玩法）。小地图常驻，不提供「关」。
// Top-right minimap = map entry (the map is at the top right; no top icon wanted → draw nothing). The rectangle comes from the game's own AutoMap panel (the host reads it every half second and keeps the latest one while the panel is closed).
// The tap area is only the area-name strip at the top of the rectangle (full width × 1/4 height, padded to 30 points if under 30, at most 0.4 of the rectangle height), not the whole block (the "minimap disappeared" issue in released builds: the whole rectangle takes 1/3 of screen width × 1/3 of screen height,
// the miniature map is drawn centered on the player in the middle of the rectangle, and that area is exactly where the player taps the ground toward the upper right -- in one diagnostic log, of 24 taps inside the rectangle 20 were in the middle body and 0 on the top strip;
// ground taps were turned into the fullscreen map and the top-right minimap was gone). The rest of the rectangle goes into the gesture layer as usual (world tap).
// Recognized only in game mode; fingers landing inside do not enter the gesture layer (no stick / skill finger slot taken, no movement sent), and one tap (displacement ≤ 14 points, within 0.6 seconds) = switch to the fullscreen map (the host calls the game's own 「透视地图尺寸」(Automap Size) change callback, not persisted, see dm_cursor.c ui_map_pump).
// With the fullscreen map open: tapping the minimap block again, or anywhere not on a skill button / stick area = back to the minimap; the stick still walks and skill buttons still cast (travelling with the fullscreen map open is normal Diablo play). The minimap is always shown; there is no way to close it.
- (BOOL)minimapRect:(CGRect *)whole tap:(CGRect *)tap {   // 小地图矩形与其中的点击区（本视图的点）；没有矩形返回 NO | Minimap rectangle and the tap area inside it (points in this view); returns NO if there is no rectangle
    float r[4]; CGSize z=self.bounds.size; if(z.width<=0 || !dm_ui_minimap_rect(r)) return NO;
    CGFloat x0=r[0]*z.width, y0=r[1]*z.height, w=(r[2]-r[0])*z.width, h=(r[3]-r[1])*z.height, band=h/4; if(band<30) band=MIN(30,h*0.4);
    if(whole) *whole=CGRectMake(x0,y0,w,h); if(tap) *tap=CGRectMake(x0,y0,w,band); return YES;
}
- (BOOL)inMinimap:(CGPoint)p {
    CGRect tap; if(_navigation || ![self minimapRect:NULL tap:&tap]) return NO;
    int control=dm_tc_hit(&_controls.layout,(float)p.x,(float)p.y);
    if(control>=0 || control==DM_TC_SWAP) return NO;   // 落在控件命中圆上的点归控件（回城钮挪到技能弧上方后在营地里被小地图矩形盖住） | Points landing on a widget's hit circle belong to the widget (after the town portal button moved above the skill arc it is covered by the minimap rectangle in camp)
    return CGRectContainsPoint(tap,p);
}
- (void)cancelControls { [self cancelControlsFrom:"focus_cancel"]; }
- (void)refreshPortalBinding {
    int dm_custom_get(const char *key);int above=dm_custom_get("touch_swap_above")!=0;
    if (_controls.layout.swap_above!=above) {
        [self cancelControlsFrom:"layout"];dm_tc_set_swap_above(above);
        dm_tc_resize(&_controls,self.bounds.size.width,self.bounds.size.height);[_overlay setNeedsDisplay];
    }
    hud_stack_watch();
    { dm_hud_info h, o=_overlay.hud; dm_hud_get(&h); h.x=h.y=0; if (memcmp(&h,&o,sizeof h)) { _overlay.hud=h; [_overlay setNeedsDisplay]; } }   // 覆盖层不画玩家坐标：不清掉的话走路时每个逻辑拍都判「变了」、整层全屏重画一遍 | The overlay does not draw player coordinates: without clearing them, every logic tick while walking would be judged "changed" and redraw the whole layer fullscreen
    dm_tg_snapshot snapshot={0};
    dm_touch_game_snapshot(&snapshot);
    _overlay.nativeMapping=snapshot;
    if(_navigation) return; // 导航态Portal禁用，不发布旧玩法bank。 | Portal is disabled in navigation mode; the old gameplay bank is not published.
    if(!snapshot.supported) return; // 旧包无OBS6，仅保留旧ZR已验域。 | Old builds have no OBS6; keep only the old verified ZR domain.
    uint64_t mask=0; (void)dm_tg_portal_mask;   // 免费回城：回城钮不再按原版手柄的回城书 / 卷轴键，点下去由宿主直接开门（dm_freetp.c，见 touchesBegan） | Free town portal: the town portal button no longer presses the original controller's Tome of Town Portal / scroll key; tapping it has the host open the portal directly (dm_freetp.c, see touchesBegan)
    if(_controls.layout.target[DM_TC_PORTAL].button!=mask || _controls.portal_binding_identity!=snapshot.binding_identity || _controls.portal_scene_epoch!=snapshot.scene_epoch) {
        dm_tc_set_portal_binding(&_controls,mask,snapshot.binding_identity,snapshot.scene_epoch);
        _modifierPublishEpoch=0;_modifierPublishEndNS=0;
        // 身份/scene变化即使mask不变也撤请求；物理_slot/其它held/joy保留。
        // On identity/scene change, withdraw the request even if mask is unchanged; physical _slot / other held / joy are kept.
        [self pushFrom:"bindings" phase:"none"];
    }
    if(_controls.portal_pending && mask && snapshot.ready && snapshot.secondary_group && snapshot.group_seen==63 &&
       _modifierPublishEpoch==_controls.gesture_epoch && _modifierPublishEndNS &&
       snapshot.group_started_ns>_modifierPublishEndNS && snapshot.captured_ns>_modifierPublishEndNS &&
       dm_tc_portal_ack(&_controls,_modifierPublishEpoch,snapshot.binding_identity,snapshot.scene_epoch))
        [self pushFrom:"portal_sequence" phase:"none"];

}
// 技能绑定：等玩家点技能期间，点按落在技能图标上（宿主命中，含吸附）= 光标到图标 + 按 Y；落在别处照常（页签、关闭钮走原来的点击）
// Skill binding: while waiting for the player to tap a skill, a tap landing on a skill icon (host hit test, snapping included) = cursor to the icon + press Y; landing elsewhere behaves as usual (tabs and the close button use the original click)
- (void)bindTapSync { float nx=0,ny=0; CGSize z=self.bounds.size;
    // 技能树一开，手柄光标停在哪个技能上游戏就出它的说明框（黑底大框盖住半个树，且常停在没学的技能上）；说明框关不掉
    // （宿主按住 TooltipTextBox / TooltipsPanel 的可见位都无效）。进「等点技能」那一刻把光标挪到面板左侧的空花格上：光标不在技能上就不出。
    // As soon as the skill tree opens, the game shows the tooltip of whichever skill the controller cursor rests on (a large black box covering half the tree, often resting on an unlearned skill); the tooltip cannot be closed
    // (the host forcing the visible bits of TooltipTextBox / TooltipsPanel has no effect). At the moment of entering "waiting for a skill tap", move the cursor to the empty ornamental cell on the left of the panel: no tooltip appears when the cursor is not on a skill.
    { static int was; int st=dm_ui_bind_active(); if(st==2 && was!=2 && z.width>0) { _controls.pointer_valid=1; _controls.pointer_x=0.155f*(float)z.width; _controls.pointer_y=0.5f*(float)z.height; } was=st; }
    int hk=dm_ui_bind_skill_hit(&nx,&ny); _controls.tap_button=0; _controls.tap_move=0;
    if(!hk) _selId=0;   // 这一下没落在加点类按钮上：选中作废 | This tap did not land on a point-spending button: the selection is voided
    if(hk) { _controls.tap_x=nx*(float)z.width; _controls.tap_y=ny*(float)z.height;
        if(hk==1 && dm_ui_bind_active()==2) _controls.tap_button=DM_BTN_Y;   // 绑定态：点技能 = 绑定 | Binding mode: tapping a skill = bind
        else if(!dm_ui_bind_active()) _controls.tap_move=1; } }   // 平时：点技能图标 / 属性「+」只选中（光标移过去、出说明），不发点击——点击当场加点且不可撤销，加点走手机层的确认按钮 | Normally: tapping a skill icon / stat "+" only selects (cursor moves there, tooltip appears) and sends no click -- a click spends the point on the spot and cannot be undone; spending points goes through the phone layer's confirm button
// 商人货架拖货的跟手图：游戏里货架上的货没有「拿起」态，拖动途中手指下什么都没有。
// 跟手图由图形回放层画（mtl_ui_ghost）：把「画这件货图标的那次绘制」在界面趟末尾平移到手指上方再画一遍——就是游戏自己那张图标，不截屏、不抠底。
// 这里只负责：起拖后每拍把按下点与手指位置交给回放层，并按它回报的矩形在货架原位盖一层暗色（高级仓库的成叠格同路：那一叠只有 1 件才盖，还有剩余就让剩下的那叠看得见）；松手（买 / 不买）或手指离开即撤。背包 / 箱子里拿起物品时游戏自己有拿起图标，不走这里——只认货架起拖。
// Follow image when dragging goods from the vendor shelf: in the game, goods on the shelf have no "picked up" state, so there is nothing under the finger during the drag.
// The follow image is drawn by the graphics replay layer (mtl_ui_ghost): "the draw that draws this goods' icon" is drawn once more at the end of the UI pass, translated to above the finger -- it is the game's own icon, no screen capture, no background cut-out.
// This code is only responsible for: after the drag starts, handing the press point and finger position to the replay layer every tick, and covering the original shelf position with a dark tint using the rectangle it reports (stacked cells of the advanced stash take the same path: cover only when the stack has just 1 item; if any remain, leave the remaining stack visible); removed on release (buy / no buy) or when the finger leaves. Picking up items in the inventory / stash has the game's own pick-up icon and does not go through here -- only drags starting from the shelf are accepted.
- (void)shopGhostSync {
    void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy); int mtl_ui_ghost_rect(float r[4]);
    const dm_tc_finger *f = (_controls.navigation_mode && _controls.pointer_slot>=0) ? &_controls.finger[_controls.pointer_slot] : NULL;
    BOOL drag = f && f->active && f->shop_drag;
    if(!drag) { if(_ghostOn) { mtl_ui_ghost(0,0,0,NULL,0,0); [_ghostDim removeFromSuperview]; _ghostDim=nil; _ghostOn=NO; } return; }
    CGSize z=self.bounds.size; if(z.width<=0) return;
    // 按下的那一点交给回放层认货（不等游戏的选中态，按下就拖也有图）；认到后它回报货的矩形，货架原位盖暗色。
    // 跟手图的中心 = 松手落点的判定点（手指上方 DM_TC_POINTER_LIFT_PT × 缩放，与拖背包物品同一条规则「落点算物品图的位置」）：
    // 原先放在「手指上方半个货高 + 14 点」，一格高的货图比判定点低约 18 点——图已经在背包第一排上，判定点还在背包上沿外，松手不买（本机钥匙实测）
    // The pressed point is given to the replay layer to identify the goods (without waiting for the game's selected state, so dragging right at press still has an image); once identified it reports the goods' rectangle, and the original shelf position is covered with a dark tint.
    // Center of the follow image = the judgment point of the release drop (DM_TC_POINTER_LIFT_PT × scale above the finger, the same rule as dragging inventory items: "the drop point is the position of the item image"):
    // previously it was placed at "half the goods' height + 14 points above the finger", so a one-cell-high goods image was about 18 points below the judgment point -- the image was already over the first inventory row while the judgment point was still outside the top edge of the inventory, and releasing did not buy (measured with a key on this machine)
    float g[4]; if(!dm_ui_shelf_grid(g)) return;
    float r[4]; BOOL have=mtl_ui_ghost_rect(r);
    mtl_ui_ghost(1,(float)(f->aim_origin_x/z.width),(float)(f->aim_origin_y/z.height),g,(float)(f->x/z.width),(float)((f->y-DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(z.width,z.height))/z.height));
    if(!_ghostOn) { _ghostOn=YES; LOG("前端：货架拖货跟手图 开（按下点 %.0f,%.0f）",f->aim_origin_x,f->aim_origin_y); }
    if(have && !_ghostDim && (_itemKind!=8 || dm_ui_stack_left()==1)) { _ghostDim=[[UIView alloc] initWithFrame:CGRectMake(r[0]*z.width,r[1]*z.height,(r[2]-r[0])*z.width,(r[3]-r[1])*z.height)];
        _ghostDim.backgroundColor=[UIColor colorWithWhite:0 alpha:.82]; _ghostDim.userInteractionEnabled=NO; [self addSubview:_ghostDim]; }
}
// 小地图任务终点（宿主 dm_world.c 找终点、dm_cursor.c 给地图几何）：
//   七个墓的真墓口 = 任务日志的塔拉夏符号；其余终点 = 原版地图名牌（深底名字）+ 下面的灰十字（原版地图上 NPC 就这么标）；
//   在地图可见范围外 = 同一种名牌只写名字，贴着那个方向的边（全屏地图避开底栏和右下技能区，小地图贴小地图框）。素材全取原版资源（tools/gen_map_goal_art.py）。
// 换算：地图上世界格 (x, y) 在 玩家点 + ((Δx−Δy)×1.6, (Δx+Δy)×0.8) × 每格像素；玩家点在全屏地图 = 玩家脚底的画面位置，在小地图 = 小地图框中心。
// 面板开着（导航态）不画——右上小地图那时也收起。层不带隐式动画，位置每帧随玩家走动更新。
// Minimap quest goals (the host's dm_world.c finds the goals, dm_cursor.c provides the map geometry):
//   the real tomb entrance among the seven tombs = the Tal Rasha symbol from the quest log; other goals = the original game's map nameplate (name on a dark base) + a grey cross below it (this is how the original game's map marks NPCs);
//   outside the map's visible range = the same nameplate with the name only, against the edge in that direction (the fullscreen map avoids the bottom bar and the bottom-right skill area, the minimap sticks to the minimap frame). All art is taken from the original game's assets (tools/gen_map_goal_art.py).
// Conversion: world cell (x, y) on the map is at player point + ((Δx−Δy)×1.6, (Δx+Δy)×0.8) × pixels per cell; the player point on the fullscreen map = the on-screen position of the player's feet, on the minimap = the center of the minimap frame.
// Not drawn while a panel is open (navigation mode) -- the top-right minimap is collapsed then too. Layers have no implicit animation; positions update every frame as the player walks.
- (UIImage *)mgImage:(NSString *)name {
    UIImage *im=_mgImg[name]; if(im) return im; if(!_mgImg) _mgImg=[NSMutableDictionary dictionary];
    im=[UIImage imageWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:[@"mapgoal/" stringByAppendingString:name]]];
    if(im) _mgImg[name]=im; return im;
}
- (void)mapGoalSync {
    static int old=-1; if(old<0) { int dm_custom_get(const char *key); old=dm_custom_get("item_notes")!=0; }   // 旧译名开关重启生效，启动读一次（名牌字跟游戏显示的一致） | The old translation names switch takes effect on restart; read once at launch (nameplate text matches what the game shows)
    dm_map_goal_view v; float cell=0; int mode=dm_ui_map_geom(&cell), n=dm_map_goals(&v); int rw=0,rh=0; dm_screen_real(&rw,&rh);
    CGRect wr=world_rect(self); float mm[4]; BOOL haveMini=dm_ui_minimap_rect(mm);
    BOOL show=mode && n>0 && !_navigation && !_info.hidden && rw>0 && cell>0 && (mode==2 || haveMini);
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    CGSize z=self.bounds.size; CGFloat k=wr.size.width/rw, c=cell*k, H=wr.size.height, lh=H*0.0236, sh=H*0.085*(mode==1?0.6:1);   // 名牌高（原版名牌字高约屏高 1/53）、符号高（的大小） | Nameplate height (the original game's nameplate glyph height is about 1/53 of screen height), symbol height (its size)
    CGPoint o; CGRect area;
    if(mode==2) { o=CGPointMake(wr.origin.x+v.sx*wr.size.width, wr.origin.y+v.sy*wr.size.height); area=CGRectMake(wr.origin.x+wr.size.width*0.04, wr.origin.y+H*0.06, wr.size.width*0.92, H*0.72); }
    else { area=CGRectMake(mm[0]*z.width, mm[1]*z.height, (mm[2]-mm[0])*z.width, (mm[3]-mm[1])*z.height); o=CGPointMake(CGRectGetMidX(area),CGRectGetMidY(area)); area=CGRectInset(area,area.size.width*0.06,area.size.height*0.08); }
    for(int i=0;i<12;i++) {
        if(!_mgLabel[i]) { _mgLabel[i]=[CALayer layer]; _mgMark[i]=[CALayer layer]; for(CALayer *l in @[_mgLabel[i],_mgMark[i]]) { l.contentsGravity=kCAGravityResizeAspect; l.hidden=YES; [_info.layer addSublayer:l]; } }
        if(!show || i>=n) { _mgLabel[i].hidden=YES; _mgMark[i].hidden=YES; continue; }
        int dx=v.x[i]-v.px, dy=v.y[i]-v.py; CGPoint p=CGPointMake(o.x+(dx-dy)*1.6*c, o.y+(dx+dy)*0.8*c);
        BOOL inside=CGRectContainsPoint(area,p) && !(mode==2 && p.x>wr.origin.x+wr.size.width*0.72 && p.y>wr.origin.y+H*0.45);   // 全屏地图右下是技能区 | The bottom right of the fullscreen map is the skill area
        int art=v.art[i], sym=art>=64; UIImage *lab=nil, *mark=nil;
        if(inside && sym) mark=[self mgImage:[NSString stringWithFormat:@"sym_%d.png",art-64]];
        else { int id=sym ? 0 : art; int lg=dm_lang_group(); lab=lg!=DM_LG_ZH ? [self mgImage:[NSString stringWithFormat:@"label_%d_%s.png",id,lg==DM_LG_ES?"es":lg==DM_LG_MX?"mx":lg==DM_LG_RU?"ru":lg==DM_LG_KO?"ko":"en"]] : (old && dm_lang_index()==8 ? [self mgImage:[NSString stringWithFormat:@"label_%d_old.png",id]] : nil);   // 简中 / 繁体用中文，西语用原版 esES / esMX 名字，俄语用原版 ruRU 名字，其余英文名牌，其余原版 enUS 名牌；旧译名只跟简中（注释一系只写 zhCN） | Simplified / Traditional Chinese use Chinese, Spanish uses the original esES / esMX names, Russian uses the original ruRU names, everything else the original enUS name plates; old names follow only Simplified Chinese (the annotation family writes only zhCN)
            lab=lab ?: [self mgImage:[NSString stringWithFormat:@"label_%d.png",id]]; if(inside) mark=[self mgImage:@"cross.png"]; }
        if(!inside) {   // 画面外：沿 玩家点 → 终点 的方向收到可见范围边上（全屏地图再让开右下技能区），只写名字 | Off-screen: clamp along the player point → goal direction to the edge of the visible range (the fullscreen map additionally avoids the bottom-right skill area), name only
            CGFloat tx=p.x>o.x ? (CGRectGetMaxX(area)-o.x)/(p.x-o.x) : p.x<o.x ? (CGRectGetMinX(area)-o.x)/(p.x-o.x) : 1e9, ty=p.y>o.y ? (CGRectGetMaxY(area)-o.y)/(p.y-o.y) : p.y<o.y ? (CGRectGetMinY(area)-o.y)/(p.y-o.y) : 1e9, t=MIN(tx,ty);
            p=CGPointMake(o.x+(p.x-o.x)*t, o.y+(p.y-o.y)*t);
            if(mode==2 && p.x>wr.origin.x+wr.size.width*0.72 && p.y>wr.origin.y+H*0.45) p.y=wr.origin.y+H*0.45; }
        CGFloat lw=lab ? lh*lab.size.width/lab.size.height : 0, cw=lh*1.4, ch=cw*18/34;
        if(lab) { _mgLabel[i].contents=(id)lab.CGImage; _mgLabel[i].frame=CGRectMake(p.x-lw/2, (mark ? p.y-ch/2-2-lh : p.y-lh/2), lw, lh); _mgLabel[i].hidden=NO; } else _mgLabel[i].hidden=YES;
        if(mark) { CGFloat mw=sym ? sh*mark.size.width/mark.size.height : cw, mh=sym ? sh : ch; _mgMark[i].contents=(id)mark.CGImage; _mgMark[i].frame=CGRectMake(p.x-mw/2, p.y-mh/2, mw, mh); _mgMark[i].hidden=NO; } else _mgMark[i].hidden=YES;
    }
    [CATransaction commit];
    static int lm=-1, ln=-1, lc; int m2=show ? mode : 0; if((m2!=lm || n!=ln) && lc<40) { lm=m2; ln=n; lc++; LOG("小地图终点：%s %d 个（地图 %d，每格 %.2f 像素 = %.2f 点）", show ? "画" : "不画", n, mode, cell, c); }
}
// 镜头实验（越肩 / 第一人称）：小地图与全屏地图上，人物十字外侧放一个指针，尖端朝镜头看的方向。指针取原版手柄表情轮盘的指针
// （mapgoal/heading.png，tools/gen_map_goal_art.py 从游戏文件取：鎏金三角嵌宝石，本来就是从中心指向一个方向），不自己画形状。
// 方向按原版斜视角换到地图上：世界 x 一格 = 地图 (1.6, 0.8)、世界 z 一格 = (−1.6, 0.8)（同 mapGoalSync）。圆心：小地图 = 小地图框正中（原版小地图
// 跟着人物居中）；全屏地图 = 人物在原版画面上的位置（地图按原版画面画，dm_cam_heading 给）
// Camera experiment (over-the-shoulder / first person): on the minimap and the fullscreen map, a pointer just outside the player cross, its tip towards where
// the camera looks. The pointer is the original controller emote wheel's (mapgoal/heading.png, taken from the game files by tools/gen_map_goal_art.py: a
// gilded, jewelled triangle that already points outward from a centre); no shape of our own. The direction maps onto the map by the original isometric
// view: one world x step = map (1.6, 0.8), one world z step = (-1.6, 0.8) (same as mapGoalSync). Centre: minimap = the middle of the minimap frame (the
// original minimap keeps the character centred); fullscreen map = the character on the original picture (the map is drawn by the original picture;
// dm_cam_heading gives it)
- (void)mapHeadingSync {
    int dm_cam_heading(float *, float *, float *, float *); float fx=0, fz=0, sx=0, sy=0, cell=0, mm[4];
    int mode=dm_ui_map_geom(&cell); UIImage *im=[self mgImage:@"heading.png"];
    BOOL on=im && mode && !_navigation && !_info.hidden && dm_cam_heading(&fx,&fz,&sx,&sy) && (mode==2 || dm_ui_minimap_rect(mm));
    if(!_mhPtr) { _mhPtr=[CALayer layer]; _mhPtr.contentsGravity=kCAGravityResizeAspect; _mhPtr.hidden=YES;
        // 光晕：原版浅金（地图名牌人名色 212,196,145），在花地图上也跳得出来（jhz：箭头不够明显、容易忽略） | glow: the original pale gold (map nameplate colour 212,196,145), so it stands out on a busy map (jhz: the arrow was too easy to miss)
        _mhPtr.shadowColor=[UIColor colorWithRed:212/255.0 green:196/255.0 blue:145/255.0 alpha:1].CGColor; _mhPtr.shadowOffset=CGSizeZero; _mhPtr.shadowRadius=4; _mhPtr.shadowOpacity=0.95;
        [_info.layer addSublayer:_mhPtr]; }
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    if(!on) _mhPtr.hidden=YES;
    else { CGSize z=self.bounds.size; CGRect wr=world_rect(self); CGPoint o; CGFloat size;
        if(mode==2) { o=CGPointMake(wr.origin.x+sx*wr.size.width, wr.origin.y+sy*wr.size.height); size=wr.size.height*0.05; }
        else { CGRect a=CGRectMake(mm[0]*z.width, mm[1]*z.height, (mm[2]-mm[0])*z.width, (mm[3]-mm[1])*z.height); o=CGPointMake(CGRectGetMidX(a),CGRectGetMidY(a)); size=a.size.height*0.14; }
        CGFloat dx=(fx-fz)*1.6, dy=(fx+fz)*0.8, dl=hypot(dx,dy); if(dl<1e-6) { dx=0; dy=-1; } else { dx/=dl; dy/=dl; }
        CGFloat r=size*0.75;   // 指针中心离人物十字的距离：让开十字、贴着它 | distance of the pointer's centre from the player cross: clear of the cross, close to it
        CGFloat iw=size*im.size.width/im.size.height, ang=atan2(dx,-dy);   // 图朝上 → 顺时针转 ang 到 (dx, dy) | the image points up -> turn clockwise by ang to (dx, dy)
        _mhPtr.contents=(id)im.CGImage; _mhPtr.bounds=CGRectMake(0,0,iw,size); _mhPtr.position=CGPointMake(o.x+dx*r, o.y+dy*r);
        _mhPtr.affineTransform=CGAffineTransformMakeRotation(ang);
        { static double lt; double t=CACurrentMediaTime(); if(t-lt>=2) { lt=t; LOG("小地图朝向指针：镜头 %.2f,%.2f → 地图方向 %.2f,%.2f 转角 %.0f° 圆心 %.0f,%.0f 指针 %.0f,%.0f 大小 %.0fx%.0f（地图 %d）", fx, fz, dx, dy, ang*180/M_PI, o.x, o.y, o.x+dx*r, o.y+dy*r, iw, size, mode); } }
        _mhPtr.hidden=NO; }
    [CATransaction commit];
}
// 镜头实验：画面外的怪物（宿主 dm_cam_threats：屏幕 x、y（0..1，已让开顶边与底栏）、方向（0 朝上、顺时针）、紧迫度、档位）在屏幕边上画指针。
// 图同小地图朝向指针（原版表情轮盘指针），按档位套原版名字配色（mapgoal/threat_<档>.png：普通银白 / 冠军蓝 / 独特暗金 / 幕首领红）；越近越大，
// 紧迫度过 0.75 轻微呼吸闪烁（同安卓 CamThreats.java 的规则）。开面板、地图全屏时不画
// Camera experiment: monsters off the picture (host dm_cam_threats: screen x, y (0..1, clear of the top edge and the bottom bar), direction (0 up, clockwise),
// urgency, tier) get a pointer on the screen edge. Same art as the minimap facing pointer (the original emote wheel pointer) in the original name colours by
// tier (mapgoal/threat_<tier>.png: normal silver / champion blue / unique gold / act boss red); larger when closer, a gentle breathing blink above 0.75 urgency (the same rules
// as Android's CamThreats.java). Not drawn with a panel open or the fullscreen map up
- (void)threatsSync {
    int dm_cam_threats(float *, int); float t[8 * 5]; float cell=0; int n=(_navigation || _info.hidden || dm_ui_map_geom(&cell)==2) ? 0 : dm_cam_threats(t, 8);
    CGSize z=self.bounds.size; double now=CACurrentMediaTime();
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for(int i=0;i<8;i++) {
        if(!_thr[i]) { _thr[i]=[CALayer layer]; _thr[i].contentsGravity=kCAGravityResizeAspect; _thr[i].hidden=YES; _thr[i].shadowColor=UIColor.blackColor.CGColor; _thr[i].shadowOffset=CGSizeZero; _thr[i].shadowRadius=3; _thr[i].shadowOpacity=0.9; [_info.layer addSublayer:_thr[i]]; }
        UIImage *im=i<n ? [self mgImage:[NSString stringWithFormat:@"threat_%d.png",(int)fminf(fmaxf(t[i*5+4],0),3)]] : nil;
        if(!im) { _thr[i].hidden=YES; continue; }
        float u=fminf(fmaxf(t[i*5+3],0),1); CGFloat h=z.height*(0.035+0.03*u), w=h*im.size.width/im.size.height;
        _thr[i].contents=(id)im.CGImage; _thr[i].bounds=CGRectMake(0,0,w,h); _thr[i].position=CGPointMake(t[i*5]*z.width, t[i*5+1]*z.height);
        _thr[i].affineTransform=CGAffineTransformMakeRotation(t[i*5+2]);
        _thr[i].opacity=u>0.75f ? (float)(0.55+0.45*fabs(sin(now*5))) : 1; _thr[i].hidden=NO; }
    [CATransaction commit];
    { static int ln=-1, lc; if(n!=ln && lc<40) { ln=n; lc++; LOG("画面外怪物指针：%d 个", n); } }
}
- (void)tickTouchControls:(double)now {
    [self mapGoalSync];
    [self mapHeadingSync];
    [self threatsSync];
    { int dm_padmode_busy(void); BOOL hide=_feMode || _padMode || dm_padmode_busy(); if(_overlay.hidden!=hide) _overlay.hidden=hide; }   // 手柄 → 触屏切换事务还没完成（界面还在重建）时触屏控件层先不露出来（dm_padmode.c） | While the controller → touch switch transaction is not yet complete (the UI is still rebuilding) the touch control layer is not revealed yet (dm_padmode.c)
    { void dm_rift_hud_draw(UIView *, CGRect, BOOL); dm_rift_hud_draw(_info, world_rect(self), _navigation || _info.hidden); }
    {   // 高价值掉落震动（宿主 dm_drophaptic.c 判档）：1 重 / 2 中 / 3 轻 | High-value drop haptics (the host's dm_drophaptic.c decides the level): 1 heavy / 2 medium / 3 light
        int dm_drop_haptic_take(void); int t = dm_drop_haptic_take();
        if (t >= 1 && t <= 3) { static UIImpactFeedbackGenerator *g[4]; if (!g[t]) g[t] = [[UIImpactFeedbackGenerator alloc] initWithStyle:t == 1 ? UIImpactFeedbackStyleHeavy : t == 2 ? UIImpactFeedbackStyleMedium : UIImpactFeedbackStyleLight];
            [g[t] impactOccurred]; LOG("掉落震动：前端震第 %d 档", t); } }
    dm_tc_set_tap_box(dm_ui_tap_box()); dm_tc_set_cur_stack(dm_ui_cur_stack_id()); dm_tc_set_hover_unid(dm_ui_hover_unid());
    { dm_hud_info hi; dm_hud_get(&hi); dm_tc_set_holding(&_controls,hi.valid && hi.holding && !dm_ui_targeting()); }   // 鉴定态下游戏的「手上物品」是那张卷轴 / 那本书，但它没在手上：不算拿着 | In identify mode the game's "item in hand" is that scroll / tome, but it is not actually in hand: not counted as holding
    if(_itemAsked && _controls.navigation_mode && _controls.pointer_slot>=0) { int ir=dm_ui_item_result(_itemSeq); if(ir>=0) { _itemAsked=NO; _itemKind=ir; dm_tc_pointer_item(&_controls,ir); } }   // 命中答案一出来就告诉手势层（物品区才驱动手柄光标、才算拖放） | Tell the gesture layer as soon as the hit answer arrives (only the item area drives the controller cursor and counts as drag and drop)
    if(_pendEnd) {   // 挂着的抬起：判类与松手落点的答案都回了就结束这根手指；0.25 秒还不回就照旧按没答上处理 | Pending lift: end this finger once both the classification and the release drop-point answers are back; if still not back after 0.25 seconds, handle as no answer as before
        BOOL ready=!(_itemAsked && dm_ui_item_result(_itemSeq)<0) && !(_finalAsked && dm_ui_item_result(_relSeq)<0);
        if(ready || CACurrentMediaTime()-_pendWall>0.25) { UITouch *pt=_pendEnd;
            // 留一行：这一下抬手等了多久、最后按什么处理（判类 1 物品区 / 2 页签 / 0 面板内控件 / 3 面板外 / 6 不点 / 7 货架 / 8 成叠格 / -1 没答上；落点 5 放下 / 3 丢 / 0 放回 / 7 卖）
            // Leave one line: how long this lift waited and how it was finally handled (classification 1 item area / 2 tab / 0 widget inside panel / 3 outside panel / 6 no tap / 7 shelf / 8 stacked cell / -1 no answer; drop point 5 put down / 3 drop / 0 put back / 7 sell)
            LOG("前端：抬手挂起 %.0f ms 后结束（%s）：判类 %d%s", (CACurrentMediaTime()-_pendWall)*1000, ready ? "答案已回" : "超时，按没答上处理", _itemAsked ? dm_ui_item_result(_itemSeq) : _itemKind, _finalAsked ? [NSString stringWithFormat:@"，松手落点 %d", dm_ui_item_result(_relSeq)].UTF8String : "");
            _pendFire=YES; [self finishTouches:[NSSet setWithObject:pt] phase:"ended"]; _pendFire=NO; _pendEnd=nil; _finalAsked=NO; }
    }
    [self bindTapSync];
    [self shopGhostSync];
    // 按住技能钮不再是换绑（按住技能本来就是持续施放）；换绑只从技能出发（技能树里选），空位十字点一下进技能树保留
    // 属性页按住「+」不放 = 连续加点（约每 0.15 秒 1 点，松手即停）
    // Holding a skill button is no longer rebinding (holding a skill is continuous casting anyway); rebinding only starts from the skill (chosen in the skill tree); tapping the cross on an empty slot to enter the skill tree is kept
    // Holding "+" on the stats page = continuous point spending (about 1 point every 0.15 seconds, stops on release)
    if(_controls.navigation_mode && !dm_ui_bind_active() && !_itemAsked && _controls.pointer_slot>=0 && _controls.pointer_slot<DM_TC_MAX_TOUCH) { dm_tc_finger *f=&_controls.finger[_controls.pointer_slot]; float nx=0,ny=0; CGSize z=self.bounds.size; uint32_t hid=dm_ui_hit_id();
        if(f->active && !f->pointer_dragged && !f->ui_drag && now-f->start_time_s>=0.5 && hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)<=10*dm_tc_scale_for_size(z.width,z.height) && dm_ui_bind_skill_hit(&nx,&ny)==2 && hid>=1 && hid<=4) {
            if(!f->pointer_long) { f->pointer_long=1; _statNext=now; _selId=hid; _controls.pointer_valid=1; _controls.pointer_x=nx*(float)z.width; _controls.pointer_y=ny*(float)z.height; LOG("前端：按住「+」（属性 %u）→ 连续加点",hid-1); }
            if(now>=_statNext) { _statNext=now+0.15; dm_stat_add((int)hid-1,1); } } }
    // 技能树里长按技能图标（按住不动 ≥0.5 秒）= 给它选一个位：光标到图标 + 按 Y，游戏弹自己的「重新绑定技能」框（原版手柄流程的第一步）
    // Long press on a skill icon in the skill tree (held still ≥0.5 seconds) = pick a slot for it: cursor to the icon + press Y, and the game pops its own 「重新绑定技能」(Press button to rebind skill) box (step one of the original game's controller flow)
    if(_controls.navigation_mode && !dm_ui_bind_active() && !_itemAsked && _controls.pointer_slot>=0 && _controls.pointer_slot<DM_TC_MAX_TOUCH) { dm_tc_finger *f=&_controls.finger[_controls.pointer_slot]; float nx=0,ny=0; CGSize z=self.bounds.size;
        if(!_pendEnd && f->active && !f->pointer_dragged && !f->pointer_long && now-f->start_time_s>=0.5 && hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)<=10*dm_tc_scale_for_size(z.width,z.height) && dm_ui_bind_skill_hit(&nx,&ny)==1) {
            dm_tc_pointer_press(&_controls,nx*(float)z.width,ny*(float)z.height,DM_BTN_Y); LOG("前端：技能树里长按技能 (%.0f,%.0f) → 开绑定框",nx*z.width,ny*z.height); } }   // 按满即弹，不等松手（dm_tc_pointer_press 会把这根手指记成「长按已处理」，松手不再算点按） | Pops as soon as the hold time is reached, without waiting for release (dm_tc_pointer_press marks this finger as "long press handled", so the release no longer counts as a tap)
    if(_mapWorldAsked) { int wr=dm_world_tap_result(_mapWorldSeq);   // 小地图那一块的点按：世界答了才定去向 | Tap on the minimap block: the destination is decided only after the world answers
        if(wr>=0) { _mapWorldAsked=NO; if(wr==0 && !_navigation) { dm_ui_map_full(1); LOG("前端：点小地图那一块 (%.0f,%.0f) → 全屏地图",_mapWorldPt.x,_mapWorldPt.y); }
            else LOG("前端：点小地图那一块 (%.0f,%.0f) → 底下有世界目标（类 %d），交互、不切地图",_mapWorldPt.x,_mapWorldPt.y,wr); } }
    if(_addAt>0 && now>=_addAt) { dm_points_info pi; dm_points_get(&pi);   // 再点同一个技能：光标到位、游戏选中了它才发加点 | Tapping the same skill again: send the point only once the cursor is in place and the game has selected it
        if(pi.tree_open && pi.sel_skill>=0 && !dm_cursor_pending()) { _addAt=0; if(pi.sel_block==0) { dm_skill_add_selected(); LOG("前端：再点同一个技能 → 加 1 点"); } else if(pi.sel_block==5) { dm_skill_add_selected(); LOG("前端：再点同一个技能 → 已到 20、这个角色还没打通过秘境，交宿主出提示"); } else LOG("前端：再点同一个技能 → 不能加（原因 %d），不发",pi.sel_block); }
        else if(now>_addAt+1.0 || !pi.tree_open) { _addAt=0; LOG("前端：再点同一个技能 → 1 秒内游戏没选中它，不发"); } }
    // 说明框只在手指按着 / 刚点过时显示：游戏给手柄光标停着的那一格一直出说明，触摸下松手后它就一直盖着（技能树盖住中间、商人页盖住页签、拖放完还留着）。
    // 说明框关不掉，办法同绑定态：到点把光标停到屏幕左缘（面板外）。手上拿着物品、绑定态、还有键没送完时不动。
    // 面板刚打开、还没点过：游戏把手柄光标缺省放在腰带上，带多排腰带时腰带就展开着盖住戒指位，点戒指点到的是药、双击把药喝了——开面板那一刻也把光标停走
    // The tooltip shows only while a finger is down / just after a tap: the game keeps showing the tooltip for the cell the controller cursor rests on, so with touch it stays covering things after release (covers the middle of the skill tree, the tabs on the vendor page, and lingers after a drag and drop).
    // The tooltip cannot be closed, so the approach is the same as binding mode: when the time is up, park the cursor at the left edge of the screen (outside the panel). Do nothing while holding an item, in binding mode, or while keys are still being delivered.
    // Panel just opened and not tapped yet: the game puts the controller cursor on the belt by default, and with a multi-row belt the belt stays expanded over the ring slot, so tapping the ring hits a potion and a double tap drinks it -- park the cursor away at the moment the panel opens too
    if(!_controls.navigation_mode || _padMode) _tipUntil=-2;   // 手柄模式：光标归原版手柄管，不发停靠请求（停靠请求一直有效，物品栏里十字键 / 摇杆挪不动光标） | Controller mode: the cursor belongs to the original controller handling, so no park request is sent (a park request stays in effect indefinitely, and the D-pad / stick could not move the cursor in the inventory)
    else if(_controls.pointer_slot>=0) _tipUntil=-1;   // 手指按着：不收（松手时重新计时） | Finger down: do not close (timing restarts on release)
    else if(_tipUntil==-2) { if(dm_ui_free_cursor()) _tipUntil=now+0.3; }   // 只在有自由光标的页停（暂停菜单 / 主菜单这类不动：冒烟玩家路径红） | Park only on pages with a free cursor (pause menu / main menu and the like stay put: smoke test player path fails)
    else if(_tipUntil>0 && now>=_tipUntil && !_controls.holding && !dm_ui_bind_active() && !dm_ui_touch_active() && !_controls.pulse_button && !dm_tc_pulse_pending(&_controls) && !dm_cursor_pending()) {   // dm_ui_touch_active：这一下点击宿主还没处理完（点绑定框格子要等它处理了才进绑定态）——不等的话光标先被挪走，游戏的绑定框就不认随后的键 | dm_ui_touch_active: the host has not finished handling this click (tapping a binding box cell enters binding mode only after it is handled) -- without waiting, the cursor is moved away first and the game's binding box does not accept the following key
        CGSize z=self.bounds.size; _controls.pointer_valid=1; _controls.pointer_x=0.02f*(float)z.width; _controls.pointer_y=0.5f*(float)z.height; _tipUntil=0; }   // 选中的标识留着：看完说明（框已收）再点同一个仍是加点 | The selected ID is kept: after reading the tooltip (already closed), tapping the same one again still adds a point
    uint64_t tb=_controls.buttons;
    dm_tc_tick(&_controls, now);
    { int fg=0; for(int i=0;i<DM_TC_MAX_TOUCH;i++) fg|=_controls.finger[i].active; dm_ui_hand_idle(!fg && !_controls.pulse_button && !dm_tc_pulse_pending(&_controls) && !dm_cursor_pending() && !dm_ui_touch_active()); }   // 手上物品守门：触摸空闲才收手上那件 | Held-item guard: the item in hand is put away only when touch is idle
    if(_controls.buttons!=tb && !_controls.pulse_button) [self pushFrom:"tick" phase:"none"];   // tick 里键位变了（第二组：主钮的键在 ZL 领先够了那一拍补上） | Key state changed in tick (second set: the main button's key is added on the tick when ZL has led long enough)
    else if(_controls.belt_request && !_controls.pulse_button) [self pushFrom:"tick" phase:"none"];   // 腰带上面几排的单击过了双击窗（dm_tc_tick 里定下的 belt_request）：当拍交宿主，不等下一次触摸 | A single tap on the upper belt rows has passed the double-tap window (belt_request decided in dm_tc_tick): hand it to the host within this tick, without waiting for the next touch
    if(_controls.pulse_button && !_pulseTicking) { [self pushFrom:"tick" phase:"none"]; [self armPulseEnd]; }   // tick 里起的脉冲（面板外点按后「按 B 到面板关掉」的后续几下） | Pulses started in tick (the follow-up presses of "press B until the panel closes" after a tap outside the panel)
    if (_controls.pointer_valid)
        dm_cursor_set_screen(_controls.pointer_x, _controls.pointer_y, self.bounds.size.width, self.bounds.size.height);
    else if (_controls.navigation_mode) dm_cursor_clear();   // 手势层放开了手柄光标（触点不在物品区）：不再推摇杆 | The gesture layer released the controller cursor (touch point not in an item area): stop pushing the stick
}
// 兜底（不当根修）：每次按下前扫一遍——槽里或单独记下的手指若已是 Ended / Cancelled（挂着待放行的 _pendEnd 除外），
// 说明某条结束路径漏清了：按取消收掉并记一行日志（这一行出现 = 还有没找到的漏清路径，带上是哪一格、哪种手指）
// Backstop (not treated as the root fix): scan before every press -- if a finger in a slot or recorded separately is already Ended / Cancelled (except _pendEnd, which is pending release),
// some end path missed its cleanup: close it as cancelled and log one line (this line appearing = there is still an unfound path that misses cleanup; it includes which slot and which kind of finger)
static BOOL touch_done(UITouch *t) { UITouchPhase ph=t.phase; return ph==UITouchPhaseEnded || ph==UITouchPhaseCancelled; }
- (void)sweepEndedSlots {
    for(int i=0;i<16;i++) if(_slot[i] && _slot[i]!=_pendEnd && touch_done(_slot[i])) {
        LOG("前端：【红】清理残留触摸槽 %d（%s，阶段 %d，控件层 %s）", i, [_slot[i] isKindOfClass:DmScriptTouch.class] ? "脚本触摸" : "真触摸", (int)_slot[i].phase, _navigation ? "面板态" : "游戏态");
        if(_uiDragOn && _controls.pointer_slot==i) { _uiDragOn=NO; dm_ui_drag_end(); }
        dm_tc_cancel_finger(&_controls,i); _slot[i]=nil; }
    if(_mapTouch && touch_done(_mapTouch)) { LOG("前端：【红】清理残留触摸（小地图那根）"); _mapTouch=nil; }
    if(_mapBackTouch && touch_done(_mapBackTouch)) { LOG("前端：【红】清理残留触摸（全屏地图那根）"); _mapBackTouch=nil; }
    if(_bindTouch && touch_done(_bindTouch)) { LOG("前端：【红】清理残留触摸（空技能位那根）"); _bindTouch=nil; }
    if(_uiFinger && touch_done(_uiFinger)) { LOG("前端：【红】清理残留触摸（界面手指）"); _uiFinger=nil;   // 宿主那头一并收：在拖就结束滚动拖，否则作废这次界面点击（相 3 清 g_ut_phase / 删除长按） | Clean up on the host side too: if dragging, end the scroll drag; otherwise void this UI click (phase 3 clears g_ut_phase / removes the long press)
        if(_uiFeDrag||_uiDragOn) { _uiFeDrag=NO; _uiDragOn=NO; dm_ui_drag_end(); } else dm_ui_touch(3,0,0); }
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    [self sweepEndedSlots];
    // 手柄模式下碰屏幕 = 回触屏模式（dm_gamepad.m），这一下只做这一件事：此刻屏上是手柄版界面，触屏控件还没显示出来（布局重载要几十帧），
    // 照常往下走的话，落点正好在看不见的回城钮 / 技能钮位置就会直接回城、放技能，面板态还会对手柄版面板发点击。
    // Touching the screen in controller mode = back to touch mode (dm_gamepad.m), and this touch does that one thing only: at this moment the
    // controller UI is on screen and the touch widgets are not shown yet (the layout reload takes a few dozen frames),
    // so if it went on as usual, a touch landing right on the invisible town portal button / skill button would directly cast town portal or a
    // skill, and in panel state a click would also be sent to the controller-layout panel.
    { BOOL wasPad=_padMode; dm_gamepad_touched();
      int dm_padmode_busy(void);
      if(wasPad || dm_padmode_busy()) { if(!_swallow) _swallow=[NSHashTable weakObjectsHashTable]; for(UITouch *t in ts) [_swallow addObject:t]; if(wasPad) LOG("前端：手柄模式下碰屏 → 只切回触屏模式，这一下不执行"); return; } }   // 切换事务进行中落下的手指同样整根不执行（控件层还没露出来） | Fingers that land while a switch transaction is in progress are likewise ignored for their whole lifetime (the control layer has not been revealed yet)
    if(atomic_load(&g_touch_frontend)) { [self uiTouch:ts phase:0]; return; }
    [self refreshPortalBinding];
    if(_pendEnd) { UITouch *pt=_pendEnd; _pendFire=YES; [self finishTouches:[NSSet setWithObject:pt] phase:"ended"]; _pendFire=NO; _pendEnd=nil; _finalAsked=NO; }   // 新手指落下前先把挂着的抬起放行 | Before a new finger lands, let the pending lift through first
    for(UITouch *t in ts) {
        int slot=-1;
        { int ex=0; _controls.belt_rows=dm_belt_rows(&ex); _controls.belt_open=ex; CGPoint q=[t locationInView:self];   // 腰带展开着点别处：先收腰带，这一下不再做别的（不开地图、不点世界、不进绑定） | Tapping elsewhere while the belt is expanded: collapse the belt first, and this tap does nothing else (no map, no world tap, no binding)
          if(dm_tc_belt_outside(&_controls,q.x,q.y)) { dm_belt_expand(0); LOG("前端：腰带展开着点别处 (%.0f,%.0f) → 只收腰带",q.x,q.y); continue; } }
        if(!_navigation && !_bindTouch && !dm_ui_bind_active()) { BOOL em=NO; CGPoint bp=[t locationInView:self]; uint64_t bk=[self skillKeyAt:bp empty:&em]; if(bk && em) { _bindTouch=t; _bindKey=bk; _bindP0=bp; _mapT0=CACurrentMediaTime(); continue; } }
        if(!_mapTouch && [self inMinimap:[t locationInView:self]]) { _mapTouch=t; _mapP0=[t locationInView:self]; _mapT0=CACurrentMediaTime(); continue; }
        for(int i=0;i<16;i++) if(!_slot[i]) {
            _slot[i]=t; CGPoint p=[t locationInView:self]; { int ex=0; _controls.belt_rows=dm_belt_rows(&ex); _controls.belt_open=ex; } { int dm_cam_mode(void); _controls.cam_on=dm_cam_mode(); } dm_tc_begin_at(&_controls,i,p.x,p.y,CACurrentMediaTime()); slot=i;
            if(!_navigation && _controls.finger[i].target==DM_TC_PORTAL) { void dm_tp_request(void); dm_tp_request(); LOG("前端：点回城钮 → 免费回城"); }   // 不要回城书 / 卷轴（宿主 dm_freetp.c） | No Tome of Town Portal / scroll needed (host dm_freetp.c)
            if(!_navigation && !_mapBackTouch && _controls.finger[i].target==DM_TC_NONE && dm_ui_map_is_full()) { _mapBackTouch=t; _mapP0=p; _mapT0=CACurrentMediaTime(); }   // 全屏地图开着、这根手指没落在任何键和摇杆区上：松手时若是点按就回小地图 | Fullscreen map open and this finger landed on no button and no stick area: if it turns out to be a tap on release, return to the minimap
            // 功能页里的指针手指：问宿主这点是不是物品区（游戏主线程下一帧作答，松手时取）
            // Pointer finger in a function page: ask the host whether this point is an item area (the game main thread answers on the next frame; fetched on release)
            if(_controls.navigation_mode && _controls.pointer_slot==i && self.bounds.size.width>0) _itemSeq=dm_ui_item_query((float)(p.x/self.bounds.size.width),(float)(p.y/self.bounds.size.height)); _itemAsked=YES; _itemKind=-1;
            break;
        }
        [self recordTouch:t slot:slot phase:"began"];
    }
    [self pushFrom:"uitouch" phase:"began"];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    (void)e;
    if(_swallow.count) { NSMutableSet *r=[ts mutableCopy]; for(UITouch *t in ts) if([_swallow containsObject:t]) [r removeObject:t]; if(!r.count) return; ts=r; }
    if(atomic_load(&g_touch_frontend)) { [self uiTouch:ts phase:1]; return; }
    UITouch *uf=_uiFinger;
    if(uf && [ts containsObject:uf]) { [self uiTouch:[NSSet setWithObject:uf] phase:1]; NSMutableSet *r=[ts mutableCopy]; [r removeObject:uf]; ts=r; if(!ts.count) return; }   // 界面手指（前端按下、按着进了局）只把它自己交给 uiTouch，局内别的手指照常 | The UI finger (pressed in the front end, still held on entering a run) hands only itself to uiTouch; other in-run fingers proceed as usual
    for(UITouch *t in ts) {
        int slot=-1;
        for(int i=0;i<16;i++) if(_slot[i]==t) { CGPoint p=[t locationInView:self]; dm_tc_move_at(&_controls,i,p.x,p.y,CACurrentMediaTime()); slot=i;
            if(_controls.cam_pitch!=0 || _controls.cam_yaw!=0) { void dm_cam_tilt(float); void dm_cam_turn(float); if(_controls.cam_pitch!=0) dm_cam_tilt(_controls.cam_pitch); if(_controls.cam_yaw!=0) dm_cam_turn(_controls.cam_yaw); _controls.cam_pitch=_controls.cam_yaw=0; }   // 镜头实验：右半屏拖动看四周（dm_touch_controls.c） | camera experiment: drag on the right half to look around
            // 界面连续手势（上下拖 = 滚动、按住滑块横拖）：手势层定型后把起手点与当前点交给宿主；认滑块时上下放宽 44 点（约 7 毫米，手指粗细）
            // Continuous UI gestures (vertical drag = scroll, hold a slider and drag horizontally): once the gesture layer has settled the type, give the start point and current point to the host; when recognizing a slider allow 44 points of vertical slack (about 7 mm, a finger's width)
            if(_controls.navigation_mode && _controls.pointer_slot==i && _controls.finger[i].ui_drag && self.bounds.size.width>0) { CGSize z=self.bounds.size; dm_tc_finger *f=&_controls.finger[i];
                if(!_uiDragOn) { _uiDragOn=YES; dm_ui_drag_lift((float)(DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(z.width,z.height)/z.height)); dm_ui_drag_begin(f->ui_drag,(float)(f->aim_origin_x/z.width),(float)(f->aim_origin_y/z.height),(float)(44.0/z.height)); }
                dm_ui_drag_move((float)(p.x/z.width),(float)(p.y/z.height)); }
            // 拖放中：问落点（物品抬在手指上方）是不是物品区，松手时用最近一次的回答
            // During drag and drop: ask whether the drop point (the item is lifted above the finger) is an item area; on release use the most recent answer
            if(_controls.navigation_mode && _controls.pointer_slot==i && _controls.finger[i].pointer_dragged && self.bounds.size.width>0) { _relSeq=dm_ui_drop_query((float)(p.x/self.bounds.size.width),(float)((p.y-DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(self.bounds.size.width,self.bounds.size.height))/self.bounds.size.height),(float)(p.y/self.bounds.size.height)); _relAsked=YES; }
            break; }
        [self recordTouch:t slot:slot phase:"moved"];
    }
    [self pushFrom:"uitouch" phase:"moved"];
    [self armPulseEnd];
}
// 前端页（主菜单 / 选角 / 建角）：点按 = 抬起处一次界面鼠标点击。纵向拖过 10 点 = 滚动（选角页只露 9 个角色、拉不下去）——
// 取消这次点击（dm_ui_touch 相 3），改走局内面板那套（dm_ui_drag_*：在起手处发游戏自己的鼠标滚轮消息，手指走多少滚多少，见 dm_cursor.c ui_drag_pump）。
// 横向拖照旧当点击（前端没有滑块）
// Front-end pages (main menu / character select / character creation): tap = one UI mouse click at the lift position. Dragging vertically past 10 points = scroll (the character select page shows only 9 characters and could not be pulled down) --
// cancel this click (dm_ui_touch phase 3) and use the in-run panel mechanism instead (dm_ui_drag_*: send the game's own mouse wheel messages at the starting point, scrolling as far as the finger moves, see dm_cursor.c ui_drag_pump).
// Horizontal drags are still treated as clicks (the front end has no sliders)
- (void)uiTouch:(NSSet<UITouch *> *)ts phase:(int)ph {   // 只认第一根手指；归一化坐标交给宿主 | Only the first finger is accepted; normalized coordinates are given to the host
    CGSize b=self.bounds.size; if(b.width<=0 || b.height<=0) return;
    for(UITouch *t in ts) {
        if(ph==0 && !_uiFinger) { _uiFinger=t; _uiFeO=[t locationInView:self]; _uiFeDrag=NO; }
        if(t!=_uiFinger) continue;
        CGPoint p=[t locationInView:self]; float dy=fabs(p.y-_uiFeO.y);
        if(ph==1 && !_uiFeDrag && !_uiDragOn && dy>10 && dy>fabs(p.x-_uiFeO.x) && dm_ui_fe_drag_ok()) {
            _uiFeDrag=YES; _uiDragOn=YES; dm_ui_touch(3,0,0); dm_ui_drag_begin(1,(float)(_uiFeO.x/b.width),(float)(_uiFeO.y/b.height),0);
            LOG("前端：纵向拖 → 滚动（起手 %.0f,%.0f），这次不点", _uiFeO.x, _uiFeO.y); }
        if(_uiFeDrag) { if(ph==1) dm_ui_drag_move((float)(p.x/b.width),(float)(p.y/b.height)); else if(ph==2) { _uiFeDrag=NO; _uiDragOn=NO; dm_ui_drag_end(); _uiFinger=nil; } continue; }
        dm_ui_touch(ph,(float)(p.x/b.width),(float)(p.y/b.height));
        if(ph==2) _uiFinger=nil;
    }
}
- (void)finishTouches:(NSSet<UITouch *> *)ts phase:(const char *)phase {
    if(_swallow.count) { NSMutableSet *r=[ts mutableCopy]; for(UITouch *t in ts) if([_swallow containsObject:t]) { [r removeObject:t]; [_swallow removeObject:t]; } if(!r.count) return; ts=r; }   // 手柄模式下落下的那根：抬起也不交 | The touch that landed in controller mode: its lift is not passed on either
    // 界面手指那一根交给 uiTouch，其余照常结束、清槽（真机 SIGSEGV：原先 _uiFinger 在就整批交走 return，局内手指的槽留下已释放的触摸）
    // Hand the UI finger to uiTouch and end the rest as usual, clearing their slots (real-device SIGSEGV: previously, if _uiFinger was present the whole batch was handed over and return, leaving released touches in the slots of in-run fingers)
    UITouch *uf=_uiFinger;   // 先存下：uiTouch 抬起时会把 _uiFinger 置空（b2：置空后再 removeObject:_uiFinger = removeObject:nil 抛异常） | Save it first: uiTouch sets _uiFinger to nil on lift (in one regression case: removeObject:_uiFinger after it was set to nil = removeObject:nil throws an exception)
    if(uf && [ts containsObject:uf]) { [self uiTouch:[NSSet setWithObject:uf] phase:2]; NSMutableSet *r=[ts mutableCopy]; [r removeObject:uf]; ts=r; }
    if(!ts.count) return;
    for(UITouch *t in ts) {
        int slot=-1;
        if(t==_mapTouch) { CGPoint p=[t locationInView:self]; _mapTouch=nil;
            if(!strcmp(phase,"ended") && !_navigation && hypot(p.x-_mapP0.x,p.y-_mapP0.y)<=14 && CACurrentMediaTime()-_mapT0<0.6) { CGSize z=self.bounds.size;
                // 小地图那一块底下也是游戏世界：NPC / 物件 / 掉落物站在那里时，点它应当是交互而不是切地图（恰西在铁匠铺前点不到）。
                // 小图态先把这一下交给世界点按（有目标 = 交互，没有目标什么都不发），答「没有目标」再切全屏；全屏态保持原样（点这一块 = 切回）。
                // Under the minimap block there is also the game world: when an NPC / object / dropped item stands there, tapping it should be an interaction rather than a map switch (Charsi could not be tapped in front of the smithy).
                // In minimap state this tap is first given to the world tap (target = interact, no target sends nothing), and only on a "no target" answer switch to fullscreen; fullscreen state is unchanged (tapping this block = switch back).
                if(dm_ui_map_is_full()) { dm_ui_map_full(0); LOG("前端：点小地图那一块 (%.0f,%.0f) → 回小地图",p.x,p.y); }
                else if(z.width>0) { { float wx,wy; world_norm(self,p.x,p.y,&wx,&wy); _mapWorldSeq=dm_world_tap(wx,wy); } _mapWorldAsked=YES; _mapWorldPt=p; } }
            continue; }
        if(t==_bindTouch) { CGPoint p=[t locationInView:self]; _bindTouch=nil;
            if(!strcmp(phase,"ended") && !_navigation && hypot(p.x-_bindP0.x,p.y-_bindP0.y)<=14) { dm_ui_bind_begin(_bindKey); LOG("前端：点空技能位 (%.0f,%.0f) → 给键 %#llx 选技能",p.x,p.y,(unsigned long long)_bindKey); }
            continue; }
        if(t==_mapBackTouch) { CGPoint p=[t locationInView:self]; _mapBackTouch=nil;
            if(!strcmp(phase,"ended") && !_navigation && dm_ui_map_is_full() && hypot(p.x-_mapP0.x,p.y-_mapP0.y)<=14 && CACurrentMediaTime()-_mapT0<0.6) { dm_ui_map_full(0); LOG("前端：全屏地图开着，点空处 (%.0f,%.0f) → 回小地图",p.x,p.y); } }
        // 松手时宿主的答案还没回就先不结束这根手指（外部 PR 指出，自己实现）：
        //   · 判类没回：原先按「别处」当鼠标点击处理，落在货架上会走到原版的价格确认；
        //   · 拖放：原先用最后一次「移动」时问的落点，快松手时那是上一个位置（一步拖过去马上松手，丢成了放回）——按松手这一刻的位置再问一次并等答案。
        // 挂起期间手势层状态不动；答案回了（或 0.25 秒超时）由 tick 放行，抬起时刻仍按真正松手的时刻算（双击 / 长按判定不受影响）
        // If the host's answer has not come back at release, do not end this finger yet (pointed out by an external PR, implemented ourselves):
        //   · classification not back: previously handled as "elsewhere", i.e. a mouse click, which on the shelf would lead to the original game's price confirmation;
        //   · drag and drop: previously used the drop point asked at the last "move", which on a quick release is the previous position (drag over in one step and release at once, and the drop became a put-back) -- ask again with the position at the moment of release and wait for the answer.
        // Gesture layer state is untouched while pending; once the answer is back (or after a 0.25-second timeout) tick lets it through, and the lift time is still the real release time (double tap / long press judgment unaffected)
        if(!_pendFire && !_pendEnd && !strcmp(phase,"ended") && _controls.navigation_mode && _controls.pointer_slot>=0 && _slot[_controls.pointer_slot]==t && self.bounds.size.width>0) {
            dm_tc_finger *pf=&_controls.finger[_controls.pointer_slot]; BOOL wait=NO;
            if(_itemAsked && dm_ui_item_result(_itemSeq)<0) wait=YES;
            else if(pf->pointer_dragged) { CGPoint ep=[t locationInView:self]; CGSize z=self.bounds.size;
                dm_tc_move_at(&_controls,_controls.pointer_slot,ep.x,ep.y,CACurrentMediaTime());   // 抬起点可能比最后一次移动更远 | The lift point may be farther than the last move
                _relSeq=dm_ui_drop_query((float)(ep.x/z.width),(float)((ep.y-DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(z.width,z.height))/z.height),(float)(ep.y/z.height)); _relAsked=YES; _finalAsked=YES; wait=YES; }
            if(wait) { _pendEnd=t; _pendEndTime=CACurrentMediaTime(); _pendWall=CACurrentMediaTime(); continue; }
        }
        for(int i=0;i<16;i++) if(_slot[i]==t) {
            // 系统取消触摸（通知横幅、系统手势）不当松手：拖着的物品放回原处、货架 / 成叠格拖出不买不取，别的手指只放掉按着的东西，不补脉冲、不点世界、不点界面
            // A system-cancelled touch (notification banner, system gesture) is not treated as a release: the dragged item is put back, a drag out of the shelf / stacked cell neither buys nor withdraws, and other fingers only release what they hold, with no make-up pulse, no world tap and no UI tap
            if(!strcmp(phase,"cancelled")) { dm_tc_finger *cf=&_controls.finger[i]; BOOL drag=_controls.navigation_mode && _controls.pointer_slot==i && (cf->pointer_dragged || cf->shop_drag);
                if(_uiDragOn && _controls.pointer_slot==i) { _uiDragOn=NO; dm_ui_drag_end(); }
                if(drag) { BOOL shop=cf->shop_drag; if(!shop) dm_tc_pointer_release(&_controls,0); dm_tc_end_at(&_controls,i,CACurrentMediaTime()); LOG("前端：拖动中被系统取消触摸 → %s",shop?"不买不取":"放回原处"); }
                else { dm_tc_cancel_finger(&_controls,i); LOG("前端：系统取消触摸（槽 %d）→ 不当松手",i); }
                if(_controls.pointer_slot<0) { _relAsked=NO; _itemAsked=NO; }
                _slot[i]=nil; slot=i; break; }
            if(_controls.navigation_mode && _controls.pointer_slot==i && _controls.finger[i].shop_drag) { int rk=_relAsked ? dm_ui_item_result(_relSeq) : 0; if(rk<0) rk=dm_ui_drop_last(); if(rk==5 && !dm_ui_drop_bag()) LOG("前端：%s拖出后松在背包格以外的物品区 → 不%s",_itemKind==8?"一叠":"货架上的货",_itemKind==8?"取":"买");   /* 装备槽、腰带格、那一叠自己身上都不成交 | Equipment slots, belt cells and that stack itself do not complete a deal */ else if((rk==5 || rk==11) && self.bounds.size.width>0) { dm_tc_finger *sf=&_controls.finger[i]; if(rk==11) { dm_ui_stack_cube_at((float)(sf->aim_origin_x/self.bounds.size.width),(float)(sf->aim_origin_y/self.bounds.size.height)); LOG("前端：高级仓库里的一叠拖到魔盒格上松手 → 1 件进魔盒"); } else if(_itemKind==8) { dm_ui_stack_take_at((float)(sf->aim_origin_x/self.bounds.size.width),(float)(sf->aim_origin_y/self.bounds.size.height)); LOG("前端：高级仓库里的一叠拖到背包松手 → 取 1 件"); } else { LOG("前端：货架上的货拖到背包松手 → 买"); dm_ui_shop_buy_at((float)(sf->aim_origin_x/self.bounds.size.width),(float)(sf->aim_origin_y/self.bounds.size.height)); } _controls.pointer_x=sf->aim_origin_x; _controls.pointer_y=sf->aim_origin_y;   /* 光标目标也改回那件货：否则宿主放回去后，落位逻辑又把它往屏幕角上推，买的那一刻已经不在货上（mini：3 次里 1 次「没选中」） | Also set the cursor target back to that item: otherwise after the host puts it back, the placement logic pushes it toward the screen corner again and at the moment of buying it is no longer on the goods (observed on the second Mac: 1 in 3 times 「没选中」(not selected)) */ } }   // 从货架起拖：落在背包类物品区 = 买光标选中的那件（光标一直留在货上）；别处松手不买 | Drag started from the shelf: dropped on an inventory-type item area = buy the item selected by the cursor (the cursor stays on the goods throughout); releasing elsewhere does not buy
            else if(_controls.navigation_mode && _controls.pointer_slot==i && _controls.finger[i].pointer_dragged && _relAsked) { int rr=dm_ui_item_result(_relSeq); if(rr<0) rr=dm_ui_drop_last(); if(rr<0) rr=0; float dx,dy;   /* 这次的落点还没答上：用上一次移动时答过的；一次都没答过 = 放回原处。不再当「物品区」在落点硬按放下 | The drop point of this time has no answer yet: use the one answered at the last move; never answered = put back. No longer force a put-down at the drop point as "item area" */ if((rr==5 && dm_ui_drop_point(&dx,&dy)) || (rr==0 && dm_ui_origin_point(&dx,&dy))) dm_tc_pointer_release_at(&_controls,dx*self.bounds.size.width,dy*self.bounds.size.height);   /* 0 = 放回原处：放回物品原位的中心（宿主在「拿起」时记下的），不是手指最初按下的点 | 0 = put back: put back at the center of the item's original position (recorded by the host at "pick up"), not the point where the finger first pressed */ else { if(rr==7) { dm_shop_sell_cursor(); rr=0; } dm_tc_pointer_release(&_controls,rr<0?1:rr); } }   // 7 = 松在商人货架上：卖手上那件（没卖掉的由随后那下 A 放回原处）   // 没答上按物品区处理（照旧放下） | 7 = released on the vendor shelf: sell the item in hand (if not sold, the following A puts it back in place)   // No answer: handle as item area (put down as before)
            _relAsked=NO;
            if(_controls.navigation_mode && _controls.pointer_slot==i && _itemAsked) { _itemAsked=NO; dm_tc_pointer_item(&_controls,dm_ui_item_result(_itemSeq)); [self bindTapSync]; }   // 松手时还没告诉过：现在取（没答上 = 按别处处理） | Not yet told at release: fetch now (no answer = handle as elsewhere)
            unsigned wt=_controls.world_tap_seq;
            unsigned mc=_controls.mouse_click_seq; unsigned tt=_controls.tab_tap_seq; unsigned us=_controls.use_seq; unsigned ids=_controls.ident_seq; unsigned sts=_controls.stack_take_seq; unsigned lps=_controls.long_press_seq; unsigned its=_controls.item_tap_seq;
            BOOL dragged=_uiDragOn && _controls.pointer_slot==i;
            if(_uiDragOn && _controls.pointer_slot==i) { _uiDragOn=NO; dm_ui_drag_end(); }
            BOOL wasPtr=_controls.pointer_slot==i; unsigned tm=_controls.tap_move_seq;
            // 按下 / 移动 / 抬起都取事件这一刻的时间（同 tick 的时钟）：面板开着时显示节拍降到 2 次 / 秒，原先取「上一拍的时刻」，0.12 秒的点按会被量成 0.5 秒、不算点按（点页签不切）
            // 即使没有最后一次移动事件，抬起位置也可能变化；换组键滑出后仍要取消。
            // Press / move / lift all take the time of the event itself (same clock as tick): with a panel open the display link drops to 2 per second, and previously "the time of the previous display-link tick" was used, so a 0.12-second tap was measured as 0.5 seconds and not counted as a tap (tapping a tab did not switch)
            if(_controls.finger[i].target==DM_TC_SWAP) { CGPoint ep=[t locationInView:self];dm_tc_move_at(&_controls,i,ep.x,ep.y,CACurrentMediaTime()); }
            if(_pendFire) dm_tc_end_at(&_controls,i,_pendEndTime); else dm_tc_end_at(&_controls,i,CACurrentMediaTime()); _slot[i]=nil; slot=i;
            // 加点用手势不用按钮：第一下点技能图标 / 属性「+」= 选中（光标过去、出说明），同一个再点一下 = 加 1 点；换目标重新从选中开始
            // Spend points with a gesture, not a button: the first tap on a skill icon / stat "+" = select (cursor moves there, tooltip appears); tapping the same one again = add 1 point; changing target starts over from select
            if(_controls.tap_move_seq!=tm) { uint32_t hid=dm_ui_hit_id();
                if(hid && hid==_selId) { if(hid<=4) { dm_stat_add((int)hid-1,1); LOG("前端：再点同一个「+」（属性 %u）→ 加 1 点",hid-1); }
                    else _addAt=_controls.last_time_s+0.3; }   // 技能：说明框可能已收（光标停在面板外），这一下先把光标送回图标，等游戏重新选中它再加（见 tick） | Skill: the tooltip may already be closed (cursor parked outside the panel); this tap first sends the cursor back to the icon and adds the point after the game reselects it (see tick)
                else { _selId=hid; LOG("前端：点加点类按钮 → 选中（标识 %#x）",hid); } }
            if(_controls.tab_tap_seq!=tt) dm_ui_tab_go();
            if(_controls.long_press_seq!=lps) dm_ui_long_press();   // 物品区长按（松手时才算）= 补充到腰带 | Long press in the item area (counted only on release) = refill to the belt
            if(_controls.item_tap_seq!=its) dm_ui_item_tap();   // 物品区单击：鉴定态下 = 鉴定这件（宿主判，不在鉴定态什么都不做） | Single tap in the item area: in identify mode = identify this item (the host decides; does nothing when not in identify mode)
            if(_controls.stack_take_seq!=sts) { dm_stash_take(1); LOG("前端：双击高级仓库里的一叠 → 取 1 件"); }
            if(_controls.ident_seq!=ids) dm_ui_identify_hovered();   // 物品区双击未鉴定物品 = 免费鉴定（宿主走游戏自己的鉴定函数） | Double tap on an unidentified item in the item area = free identify (the host calls the game's own identify function)
            if(_controls.use_seq!=us) dm_ui_use_item();   // 物品区双击 = 使用 / 购买 / 装备（宿主按光标下物品走原生入口） | Double tap in the item area = use / buy / equip (the host goes through the native entry for the item under the cursor)
            if(_controls.world_tap_seq!=wt && self.bounds.size.width>0) { float wx,wy; world_norm(self,_controls.world_tap_x,_controls.world_tap_y,&wx,&wy); dm_ui_world_tap((float)(_controls.world_tap_x/self.bounds.size.width),(float)(_controls.world_tap_y/self.bounds.size.height),wx,wy); }   // 游戏态点世界：点目标 = 交互，点空地不发令 | World tap in game mode: tapping a target = interact; tapping empty ground issues no command
            // 单纯点了一件物品（没拖、没触发使用 / 鉴定 / 取一件 / 长按）：光标留在它上面，说明框一直在，点别处 / 点另一件 / 关面板才收——同下面的技能图标。
            // 原先一律松手后 2.8 秒收，玩家读长说明读到一半框没了、要再点一次。拖放、使用之后光标停在落点上的那种仍按 2.8 秒收（不然放完东西说明框一直盖着）
            // A plain tap on an item (no drag, no use / identify / take-one / long press triggered): the cursor stays on it and the description stays up until a tap elsewhere / on another item / the panel closes - same as the skill icons below.
            // It used to be put away 2.8 seconds after release in every case, so a player reading a long description lost it halfway and had to tap again. When the cursor rests on the drop point after a drag or a use, it is still put away after 2.8 seconds (otherwise the description would keep covering things after an item is placed)
            BOOL itemTap=_controls.item_tap_seq!=its && !dragged && _controls.use_seq==us && _controls.ident_seq==ids && _controls.stack_take_seq==sts && _controls.long_press_seq==lps && _controls.mouse_click_seq==mc;
            if(wasPtr && _controls.navigation_mode) _tipUntil=(_controls.tap_move_seq!=tm || itemTap) ? -1 : _controls.last_time_s+(_controls.mouse_click_seq!=mc ? 0.05 : 2.8);   // 点的是技能图标 / 属性「+」（选中待确认）：光标留在上面，原版高亮和说明框一直在，点别处 / 点另一个 / 关面板才收   // 说明框：松手后 2.8 秒自己收（游戏的说明框要点后约 0.8 秒才出来，实测；留约 2 秒看）；点的是空处 / 普通按钮 = 立即收 | Tapped a skill icon / stat "+" (selected, awaiting confirmation): the cursor stays on it and the original game's highlight and tooltip stay until tapping elsewhere / tapping another / closing the panel   // Tooltip: closes by itself 2.8 seconds after release (the game's tooltip takes about 0.8 seconds after the tap to appear, measured; leaving about 2 seconds to read); tapped empty space / an ordinary button = close immediately
            if(_controls.mouse_click_seq!=mc && self.bounds.size.width>0) { float nx=_controls.mouse_click_x/self.bounds.size.width, ny=_controls.mouse_click_y/self.bounds.size.height; dm_ui_touch(0,nx,ny); dm_ui_touch(2,nx,ny); }   // 非物品区点按 = 界面鼠标点击 | Tap outside the item area = UI mouse click
            break; }
        [self recordTouch:t slot:slot phase:phase];
    }
    [self pushFrom:"uitouch" phase:phase];
    [self armPulseEnd];
}
// 脉冲按键（副技能松手施放 / 功能页里点格、拖放的拿起与放下）：原版按一次即放一次，前端按下后要自己收回。
// 收回按「游戏已取走」判，不按墙钟：游戏取手柄一卡（Mac 实测开功能页后约 0.4 s 没取），墙钟 0.12 s 的脉冲整段被漏掉 = 点了没反应。
// 规则（与 Mac 宿主 touch_mac.c 相同）：游戏首次取到这下按键之后再保持 0.12 s（约 3 个逻辑拍）且已取 ≥3 次、光标扣着的 A 已送达（dm_cursor_pending）才收；
//   排着的下一个脉冲（拖放的「放下」在「拿起」没收时就到了）等游戏取过 ≥2 次松开再开始——否则两下 A 连成一下。墙钟上限 2 s 兜底防卡死。
// 之前只在手指抬起时排一次收回：「放下」那一下 A 没人收、一直按着，下一次碰到卷轴 / 药水成了「按住使用」（真机 tcC / tcD）
// Pulse keys (secondary skill cast on release / tapping a cell in a function page, pick-up and put-down of drag and drop): the original game acts once per press, so the front end has to withdraw the press itself.
// Withdrawal is judged by "the game has polled it", not by wall clock: once the game's controller polling hitches (measured on Mac: about 0.4 s without polling after opening a function page), a 0.12 s wall-clock pulse is missed entirely = tapped with no response.
// Rule (same as the Mac host's touch_mac.c): withdraw only after the game first polled this press and it was then held another 0.12 s (about 3 logic ticks), it has been polled ≥3 times, and the A withheld for the cursor has been delivered (dm_cursor_pending);
//   the next queued pulse (the drag-and-drop "put down" arrives before the "pick up" is withdrawn) starts only after the game has polled the released state ≥2 times -- otherwise the two A presses merge into one. A 2 s wall-clock cap is the backstop against getting stuck.
// Previously withdrawal was scheduled only once when the finger lifted: the "put down" A was never withdrawn and stayed held, so the next touch on a scroll / potion became "hold to use" (two real-device regression cases)
- (void)armPulseEnd {
    if(_pulseTicking || (!_controls.pulse_button && !dm_tc_pulse_pending(&_controls))) return;
    _pulseTicking=YES; _pulseEpoch=0; [self pulseTick];
}
- (void)pulseTick {
    uint64_t n=dm_input_samples(); double now=CACurrentMediaTime();
    if(_controls.pulse_button) {
        if(_pulseEpoch!=_controls.pulse_epoch) { _pulseEpoch=_controls.pulse_epoch; _pulseS0=n; _pulseT0=now; _pulseSeen=0; _pulseFree=0; }   // 新脉冲：记起点 | New pulse: record the start
        if(!_pulseSeen && n>_pulseS0) _pulseSeen=now;                                                                     // 游戏第一次取到 | First time the game polled it
        if(_pulseSeen && !_pulseFree && !dm_cursor_pending()) _pulseFree=now;                                             // 按键真正送达（不再扣着）的时刻 | The moment the key is actually delivered (no longer withheld)
        double hold=_controls.pulse_hold_s;   // 「按住」脉冲（物品区双击 = 按住 A / Y）：送达之后再按满时长 | "Hold" pulse (double tap in the item area = hold A / Y): hold for the full duration after delivery
        BOOL taken=_pulseSeen && now-_pulseSeen>=0.12 && n-_pulseS0>=3 && !dm_cursor_pending() && (hold<=0 || (_pulseFree && now-_pulseFree>=hold));
        if(taken || now-_pulseT0>2.0+hold) {
            if(!taken) LOG("[touch] 【红】脉冲 2 秒内游戏没取走（取了 %llu 次，扣着 %d 下），强制收回",(unsigned long long)(n-_pulseS0),dm_cursor_pending());
            if(dm_tc_pulse_end(&_controls,_pulseEpoch)) [self pushFrom:"release_cast" phase:"none"];
            _pulseS0=n; _pulseT0=now;
        }
    } else if(dm_tc_pulse_pending(&_controls)) {
        if(n-_pulseS0>=2 || now-_pulseT0>0.5) { if(dm_tc_pulse_resume(&_controls)) [self pushFrom:"release_cast" phase:"none"]; }
    } else { _pulseTicking=NO; return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(NSEC_PER_SEC/120)),dispatch_get_main_queue(),^{ [self pulseTick]; });
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self finishTouches:ts phase:"ended"]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { (void)e; [self finishTouches:ts phase:"cancelled"]; }
- (CGPoint)scriptPoint:(NSString *)spec move:(BOOL)move {
    // 数字对 "x,y"（点）或 "@目标[+dx,dy]"：目标 = joy/swap/main/skill1-4/portal/belt1-4/menu/map/mmname（小地图点击区中心）/mmbody（小地图矩形中心）
    // Number pair "x,y" (points) or "@target[+dx,dy]": target = joy/swap/main/skill1-4/portal/belt1-4/menu/map/mmname (center of the minimap tap area)/mmbody (center of the minimap rectangle)
    CGPoint base=CGPointZero; NSString *rest=spec; BOOL absPt=[spec hasPrefix:@"="]; if(absPt) rest=[spec substringFromIndex:1];   // 「=x,y」：已是本视图里的点（tap_unit / tap_ui 自己算出来的落点），不做 DM_TOUCH_REF 换算 | "=x,y": already a point in this view (a landing point computed by tap_unit / tap_ui itself), no DM_TOUCH_REF conversion
    if([spec hasPrefix:@"@"]) {
        NSRange r=[spec rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"+"]];
        NSString *name=[spec substringWithRange:NSMakeRange(1,(r.location==NSNotFound?spec.length:r.location)-1)];
        rest=r.location==NSNotFound?@"0,0":[spec substringFromIndex:r.location+1];
        NSArray *names=@[@"main",@"skill1",@"skill2",@"skill3",@"skill4",@"portal",@"belt1",@"belt2",@"belt3",@"belt4",@"menu",@"map"];
        NSUInteger i=[names indexOfObject:name];
        if([name isEqualToString:@"joy"]) base=CGPointMake(_controls.layout.joy_x,_controls.layout.joy_y);
        else if([name isEqualToString:@"swap"]) base=CGPointMake(_controls.layout.swap.x,_controls.layout.swap.y);
        else if(i!=NSNotFound) base=CGPointMake(_controls.layout.target[i].x,_controls.layout.target[i].y);
        else if([name isEqualToString:@"mmname"] || [name isEqualToString:@"mmbody"]) { CGRect wh,tp;
            if([self minimapRect:&wh tap:&tp]) { CGRect q=[name isEqualToString:@"mmname"]?tp:wh; base=CGPointMake(CGRectGetMidX(q),CGRectGetMidY(q));
                LOG("[script] 小地图：矩形 x %.0f–%.0f y %.0f–%.0f 点，点击区 y %.0f–%.0f（高 %.0f 点）；%s = (%.0f,%.0f)",wh.origin.x,CGRectGetMaxX(wh),wh.origin.y,CGRectGetMaxY(wh),tp.origin.y,CGRectGetMaxY(tp),tp.size.height,name.UTF8String,base.x,base.y); }
            else LOG("[script] 【红】%s：还没有小地图矩形",name.UTF8String); }
        else LOG("[script] 【红】未知目标 %s",name.UTF8String);
    }
    NSArray *xy=[rest componentsSeparatedByString:@","];
    CGFloat x=[xy[0] doubleValue], y=xy.count>1?[xy[1] doubleValue]:0;
    // DM_TOUCH_REF=<宽>x<高>：脚本里的裸坐标是按这个点数的屏幕写的（真机 956x440），换算到当前视图——界面按屏高缩放、水平居中。
    // 只对居中的界面成立（面板、对话菜单、箱子、背包）；贴屏幕边的控件（选角页右列、创建新角色、垃圾桶等）在比参考更宽的屏上不在这个位置，用 tap_ui <控件名>（真机 956 宽上「创建新角色」偏了 260 像素）。
    // 拖动中（move）落点 = 手指上方 DM_TC_POINTER_LIFT_PT × 控件缩放，点数不随屏高缩放：按「落点」换算再把本机的抬升加回去。@目标 不换算
    // DM_TOUCH_REF=<width>x<height>: bare coordinates in the script are written for a screen of this many points (real device 956x440) and are converted to the current view -- the UI scales by screen height and is centered horizontally.
    // Only valid for centered UI (panels, dialog menu, chest, inventory); edge-anchored widgets (right column of the character select page, create new character, trash can, etc.) are not at this position on screens wider than the reference, so use tap_ui <widget name> (on the 956-wide real device 「创建新角色」(Create New Character) was off by 260 pixels).
    // While dragging (move) the drop point = DM_TC_POINTER_LIFT_PT × widget scale above the finger, and those points do not scale with screen height: convert by the "drop point" and then add this device's lift back. @target is not converted
    static double rw, rh; static int ref=-1;
    if(ref<0) { const char *e=getenv("DM_TOUCH_REF"); ref=e && sscanf(e,"%lfx%lf",&rw,&rh)==2 && rw>0 && rh>0; }
    if(ref && !absPt && ![spec hasPrefix:@"@"]) {
        CGFloat W=self.bounds.size.width, H=self.bounds.size.height, k=H/rh;
        if(move) y-=DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(rw,rh);
        x=W/2+(x-rw/2)*k; y=y*k;
        if(move) y+=DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(W,H);
    }
    return CGPointMake(base.x+x,base.y+y);
}
- (void)scriptPhase:(NSString *)phase slot:(int)slot at:(NSString *)spec {
    static DmScriptTouch *fingers[16];
    if(slot<0||slot>=16) return;
    if([phase isEqualToString:@"down"]) {
        // 同一脚本手指还按着又按下（tap_unit / tap_ui 自己的「落一下」与脚本步骤撞在手指 0 上，b2 真机崩溃的真因）：先替旧的抬起。
        // 原先直接顶掉，旧手指既没结束也没取消就被释放、还留在槽里 → 下次 pushFrom 读野指针。真触摸不会这样（系统总会发结束 / 取消），这是脚本自己的毛病
        // The same script finger is pressed again while still down (tap_unit / tap_ui's own "land one touch" colliding with a script step on finger 0, the real cause of a real-device crash in one regression case): lift the old one first on its behalf.
        // Previously it was simply displaced: the old finger was released without being ended or cancelled and still sat in the slot → the next pushFrom read a dangling pointer. Real touches never do this (the system always sends ended / cancelled); this is the script's own fault
        if(fingers[slot]) { LOG("[script] 【红】手指 %d 还按着又按下 → 先抬起旧的（脚本里两处在用同一根手指）",slot); fingers[slot].scriptPhase=UITouchPhaseEnded; [self touchesEnded:[NSSet setWithObject:fingers[slot]] withEvent:nil]; fingers[slot]=nil; }
        fingers[slot]=[DmScriptTouch new]; fingers[slot].scriptPoint=[self scriptPoint:spec move:NO]; fingers[slot].scriptPhase=UITouchPhaseBegan;
        [self touchesBegan:[NSSet setWithObject:fingers[slot]] withEvent:nil];
    } else if(fingers[slot] && [phase isEqualToString:@"move"]) {
        fingers[slot].scriptPoint=[self scriptPoint:spec move:YES]; fingers[slot].scriptPhase=UITouchPhaseMoved;
        [self touchesMoved:[NSSet setWithObject:fingers[slot]] withEvent:nil];
    } else if(fingers[slot] && [phase isEqualToString:@"up"]) {
        fingers[slot].scriptPhase=UITouchPhaseEnded; [self touchesEnded:[NSSet setWithObject:fingers[slot]] withEvent:nil]; fingers[slot]=nil;
    } else if(fingers[slot] && [phase isEqualToString:@"cancel"]) {   // 系统取消触摸（通知横幅等），与 touchesCancelled 同一入口 | System-cancelled touch (notification banner etc.), same entry as touchesCancelled
        fingers[slot].scriptPhase=UITouchPhaseCancelled; [self touchesCancelled:[NSSet setWithObject:fingers[slot]] withEvent:nil]; fingers[slot]=nil;
    } else { LOG("[script] 【红】无效动作 %s slot %d",phase.UTF8String,slot); return; }
    LOG("[script] %s slot=%d spec=%s buttons=%#llx lx=%d ly=%d rx=%d ry=%d",phase.UTF8String,slot,spec.UTF8String?:"",
        (unsigned long long)_controls.buttons,_controls.lx,_controls.ly,_controls.rx,_controls.ry);
}
- (void)captureTouchLayer:(NSString *)path {
    [self refreshPortalBinding];
    UIGraphicsImageRendererFormat *f=[UIGraphicsImageRendererFormat preferredFormat]; f.opaque=NO; f.scale=self.window.screen.scale;
    UIImage *im=[[[UIGraphicsImageRenderer alloc] initWithBounds:_overlay.bounds format:f] imageWithActions:^(UIGraphicsImageRendererContext *ctx){ [self->_overlay.layer renderInContext:ctx.CGContext]; if(!self->_info.hidden) [self->_info.layer renderInContext:ctx.CGContext]; }];   // 信息层（任务条 / 任务终点）原先是控件层的子层，照旧一并截进来 | the info layer (objective bar / quest goal) used to be a sublayer of the controls layer, so it is still captured together with it
    NSData *png=UIImagePNGRepresentation(im); BOOL imageOK=[png writeToFile:path atomically:YES];
    NSMutableDictionary *m=[[_overlay controlMetadata] mutableCopy]; m[@"scale"]=@(f.scale); m[@"pixel_size"]=@[@(CGImageGetWidth(im.CGImage)),@(CGImageGetHeight(im.CGImage))];
    NSError *error=nil; NSData *json=[NSJSONSerialization dataWithJSONObject:m options:NSJSONWritingPrettyPrinted error:&error];
    BOOL metadataOK=[json writeToFile:[path stringByAppendingString:@".json"] options:NSDataWritingAtomic error:&error];
    if(imageOK && metadataOK) { _touchShots++; [self pushFrom:"screenshot" phase:"none"]; fprintf(stderr,"[touch] overlay screenshot %s written\n",path.UTF8String); }
    else fprintf(stderr,"[touch] 【红】操作层截图写盘失败 PNG=%d JSON=%d %s\n",imageOK,metadataOK,error.localizedDescription.UTF8String?:"PNG write failed");
}
@end

// 底部 UI v4 的 UIKit 底栏只在「局内常态」画：维护面板开合栈，常驻 HUD 层之外任何面板开着就不画（未知面板默认让开，免挡游戏自己的画面）。
// 常驻层 = 营地无操作时 OpenPanel 后一直不关的 13 个（实测 UI 消息流）+ 存档提示 / 通知 / 组队消息 / 帮助按钮 / 手柄提示层 / 自动地图（战斗叠加）
// 名单在宿主 dm_cursor.c 的 dm_ui_panel_resident（面板状态安全阀同用一份）
// 面板开合栈搬到宿主 dm_cursor.c（dm_hud_stack_msg，Mac touch_mac.c 共用同一份）；这里只记「连续多久非零」并通知 UIKit 底栏让开
// The UIKit bottom bar of bottom UI v4 is only drawn in the normal in-game state: maintain the panel stack; whenever any panel other than the resident HUD layers is open, it is not drawn (unknown panels make way by default so they do not cover the game's own picture).
// Resident layers = the 13 panels that are opened and never closed in camp when idle (measured from the UI message stream) + save notice / notifications / party messages / help button / gamepad hint layer / automap (combat overlay).
// The list is dm_ui_panel_resident in the host's dm_cursor.c (shared with the panel state safety valve).
// The panel stack moved to the host's dm_cursor.c (dm_hud_stack_msg, shared with Mac touch_mac.c); here we only track how long it has been non-zero and tell the UIKit bottom bar to make way
int dm_hud_stack_msg(const char *act, const char *arg); void dm_hud_stack_dump(void);
static _Atomic int g_hud_blocked;   // 栈读数：开着的非常驻面板数（只开着 NPC 对话菜单时 -1） | Stack reading: number of open non-resident panels (-1 when only the NPC dialog menu is open)
static uint64_t g_hud_blocked_since;
static void hud_stack_message(const char *act,const char *arg) {
    int n=dm_hud_stack_msg(act,arg);
    uint64_t t=now_ns();
    if(n>0 && !g_hud_blocked_since) g_hud_blocked_since=t;
    if(n<=0) g_hud_blocked_since=0;
    if(atomic_exchange(&g_hud_blocked,n)!=n) dispatch_async(dispatch_get_main_queue(),^{ [g_touch_view hudBlocked:n]; });
}
// 主线程定时调：栈连续 60 秒非零就记一行（线索，不是红行）。玩家开着背包页超过一分钟就会出现（真机 5 格测量每格都有），
// 当红行会污染合入闸门与回归的红行计数；「消息上开着、屏上没有」这类真异常由宿主的面板安全阀（dm_cursor.c ui_panel_heal）判、打红行。
// 留这一行是因为树上判不出的那类（只开不关、树上一直可见的层，如 CandidateListPanel）只在这里露头：见到不认识的名字常年挂着，查它是不是常驻层
// Called periodically on the main thread: log one line if the stack is non-zero for 60 seconds in a row (a clue, not a failure line). It appears whenever the player keeps the inventory page open for over a minute (every cell of the real-device 5-cell measurement had it),
// and treating it as a failure line would pollute the failure line counts of the merge gate check and regression; real anomalies like "open per messages but not on screen" are judged by the host's panel safety valve (dm_cursor.c ui_panel_heal), which prints the failure line.
// This line is kept because the kind that cannot be judged from the tree (layers opened and never closed that stay visible in the tree, e.g. CandidateListPanel) only shows up here: if an unfamiliar name stays there for a long time, check whether it is an always-on layer
static void hud_stack_watch(void) {
    if(g_hud_blocked_since && now_ns()-g_hud_blocked_since>60000000000ull) {
        fprintf(stderr,"[hud] 面板栈已连续 60 秒非零（线索，非缺陷）：");
        dm_hud_stack_dump();
        fprintf(stderr,"\n"); g_hud_blocked_since=now_ns();
    }
}
// 原版面板状态只决定控件用途/显隐；不把MouseMove消息误当鼠标注入。
// The original game's panel state only decides widget purpose / visibility; MouseMove messages are not mistaken for mouse injection.
static void touch_ui_message(const char *cat,const char *act,const char *arg) {
    if(!strcmp(cat,"PanelManager")) hud_stack_message(act,arg);
    static dm_tc_scene scene; static uint64_t sceneEpoch=1,worldEpoch=1;
    dm_tc_scene before=scene;
    dm_tc_set_panel_state(dm_ui_panel_state());   // 储藏箱开着等状态位：读宿主从游戏面板树发布的值（变化时宿主会发 DM:PanelState 触发到这里） | State bits such as stash open: read the value the host publishes from the game's panel tree (on change the host sends DM:PanelState, which triggers this)
    int mode=dm_tc_navigation(&scene,cat,act,arg);
    // 面板态不靠名单：局内只要面板栈里还开着任何一个非常驻面板（上面 hud_stack_message 刚更新过的同一份读数，底栏让不让开也看它），就是面板态——
    // 触点走界面点击、面板外点按 = 关。原先只认 dm_tc_navigation 里那张名单，雇佣列表等不在名单里的页开着时触点还按游戏态处理：
    // 技能圆钮照样露着、点面板外是点世界，进得去出不来。只开着 NPC 对话菜单时读数是 -1，名单里本来就有它。
    // Panel mode does not rely on a list: in a run, as long as any panel other than the always-on layers is still open in the panel stack (the same reading that hud_stack_message above just updated, which also decides whether the bottom bar yields), it is panel mode --
    // touch points go to UI clicks, and a tap outside the panel = close. Previously only the list in dm_tc_navigation was accepted, so with pages not on the list such as the hire list open, touch points were still handled as game mode:
    // the round skill buttons stayed visible and a tap outside the panel was a world tap; you could get in but not out. With only the NPC dialog menu open the reading is -1, and the list already contains it.
    if(scene.game && atomic_load(&g_hud_blocked)>0) mode=1;
    static int prevMode=1;
    if(before.game!=scene.game)++worldEpoch;
    { uint32_t dm_padmode_rebuilds(void); static uint32_t rb0; uint32_t rb=dm_padmode_rebuilds(); if(rb!=rb0) { rb0=rb; ++worldEpoch; } }   // 手柄 / 触屏布局重建过（dm_padmode.c）：HUD 控件全换了，技能钮快照重新认 HUD | Controller / touch layouts were rebuilt (dm_padmode.c): all HUD widgets changed, so the skill-button snapshot re-identifies the HUD
    if(mode!=prevMode)++sceneEpoch;
    prevMode=mode;
    // 原UMS客体回调先同步scene，再排UIKit；不拿主线程异步状态当玩法确认。
    // The original UMS guest callback syncs scene first, then schedules UIKit; main-thread async state is not taken as gameplay confirmation.
    dm_touch_game_set_scene(!mode,sceneEpoch,worldEpoch);
    int fe=!scene.game;
    if(atomic_exchange(&g_touch_frontend,fe)!=fe) dispatch_async(dispatch_get_main_queue(),^{ [g_touch_view frontendMode:fe]; });
    // 测试：DM_TOUCH_SCRIPT_START=menu 时触点脚本从主菜单出现起计时（缺省从首次进局起），前端页面的玩家路径用例用
    // Test: with DM_TOUCH_SCRIPT_START=menu the touch script times from when the main menu appears (default: from first entering a run); used by player-path cases on front-end pages
    if(getenv("DM_TOUCH_SCRIPT_START") && !strcmp(cat,"PanelManager") && !strcmp(act,"OpenPanel") && !strcmp(arg,"MainMenuButtonRibbon")) dispatch_async(dispatch_get_main_queue(),^{ touch_script_start(); });
    int nmode=dm_tc_panel_mode(mode,&scene,atomic_load(&g_hud_blocked));   // 3 = 只开着 NPC 对话菜单（面板栈读数 -1）；菜单上又开了雇佣列表这类不在名单里的页就不算（那时底栏十字不该露出来） | 3 = only the NPC dialog menu is open (panel stack reading -1); if a page not on the list such as the hire list is opened on top of the menu, it does not count (the bottom bar cross should not show then)
    if(atomic_exchange(&g_touch_navigation,nmode)!=nmode) dispatch_async(dispatch_get_main_queue(),^{ [g_touch_view navigationMode:nmode]; });
    dm_uiscreen_set_panel(nmode==1);   // iPad 真全屏：面板模式下界面移到正中；没开真全屏时宿主不动 | iPad true fullscreen: in panel mode the UI moves to the center; without true fullscreen the host does nothing
}

// 主线程调用：建全屏 Metal 视图，图形后端经 front_metal_layer 取它的层出图
// ProMotion 帧率申请：iPhone 上 App 自己调 nextDrawable / present 时系统缺省按 60 Hz 给画布（真机：只加 Info.plist 的
// CADisableMinimumFrameDurationOnPhone 仍 60 帧、等画布 16.6 ms）。挂一个只表态的 CADisplayLink（回调空）申请 80–120 Hz，系统才切到 120 Hz。
// Called on the main thread: creates the fullscreen Metal view; the graphics backend gets its layer through front_metal_layer to present frames
// ProMotion frame rate request: on iPhone, when the App calls nextDrawable / present itself, the system by default hands out drawables at 60 Hz (real device: adding only Info.plist's
// CADisableMinimumFrameDurationOnPhone still gives 60 fps and a 16.6 ms drawable wait). Attach a declaration-only CADisplayLink (empty callback) requesting 80–120 Hz, and only then does the system switch to 120 Hz.
@interface DmRateHint : NSObject
- (void)tick:(CADisplayLink *)l;
@end
@implementation DmRateHint
- (void)tick:(CADisplayLink *)l {
    extern int mtl_fps_target(void);
    { static int last = -1; int f = mtl_fps_target(); if (f != last) { last = f; l.preferredFrameRateRange = f == 30 ? CAFrameRateRangeMake(30, 30, 30) : CAFrameRateRangeMake(60, 60, 60); } }   // 省电模式（30 帧）：帧率提示一起降，ProMotion 屏才会真降刷新率 | Power-saving mode (30 fps): lower the frame rate hint as well; only a ProMotion screen actually lowers the refresh rate
    DmMetalView *v = g_touch_view;
    if (v) {
        if (g_full) { static int lx = -1, ly, lw, lh, lrh, lrw; int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h);   // 界面矩形由宿主定（开机后才有、切档会变）：变了就让容器重摆游戏视图 | The UI rectangle is decided by the host (available only after boot, changes when the profile switches): on change, have the container re-lay out the game view
            if (dm_screen_real(&rw, &rh) && (x != lx || y != ly || w != lw || h != lh || rh != lrh || rw != lrw)) { lx = x; ly = y; lw = w; lh = h; lrh = rh; lrw = rw; [v.superview setNeedsLayout]; [v.superview layoutIfNeeded]; } }   // 当拍摆到位：自绘控件不比游戏画的界面晚一拍 | Laid out within the same tick: custom-drawn widgets are not one tick later than the UI drawn by the game
        [v tickTouchControls:CACurrentMediaTime()];
        [g_touch_view refreshPortalBinding];
    }
}
@end
static CADisplayLink *g_rate_link;
// 固定 60 Hz 表态 + 每帧刷新触屏绑定快照。
// 游戏视图 = 窗口里居中的、与游戏画面同宽高比的最大矩形，外面一层黑底。出图本来就是「保持画面比例居中、余下留黑边」（nvn_mtl.m present_encode），
// 而触摸层、自绘底栏、血球、技能钮、摇杆、安全区、给宿主的归一化坐标全都从游戏视图自己的 bounds 取——视图若是整个窗口，窗口比例与画面比例一不同，
// 自绘的东西就落到黑边里、触点也对不上（mini 上强制 DM_GAME_RES=2868x1320：画面 2.17:1、窗口 16:9，底栏画成两条、拖卖落背景、点箱子落空地；
// iPad 4:3 窗口对 16:9 画面同理）。画面比例取 front_game_res（已含 16:9 下限与 DM_GAME_RES）。比例差 < 1% 当作相同、铺满（与出图那边同一个容差）：
// iPhone 真机画面比例 = 屏幕比例，走这条，不套容器，行为不变。
// Fixed 60 Hz declaration + refresh the touch binding snapshot every frame.
// Game view = the largest rectangle centered in the window with the same aspect ratio as the game picture, with a black base around it. Output was always "keep the picture aspect ratio, center, black bars for the rest" (nvn_mtl.m present_encode),
// while the touch layer, custom-drawn bottom bar, orbs, skill buttons, stick, safe area and the normalized coordinates given to the host all come from the game view's own bounds -- if the view is the whole window, as soon as the window ratio differs from the picture ratio,
// custom-drawn things fall into the black bars and touch points no longer line up (on the second Mac with DM_GAME_RES=2868x1320 forced: picture 2.17:1, window 16:9, the bottom bar is drawn as two strips, drag-to-sell lands on the background, tapping a chest lands on empty ground;
// the same for an iPad 4:3 window against a 16:9 picture). The picture ratio comes from front_game_res (already includes the 16:9 floor and DM_GAME_RES). A ratio difference < 1% counts as equal and fills the window (same tolerance as the output side):
// on a real iPhone the picture ratio = the screen ratio, which takes this path with no container and unchanged behavior.
uint64_t front_game_res(int docked); void mtl_set_ui_rect_fn(void (*fn)(double *y, double *h));
static void ui_rect_frac(double *fy, double *fh) { int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); if (dm_screen_real(&rw, &rh) && rh > 0) { *fy = (double)y / rh; *fh = (double)h / rh; } }
static CGRect letterbox_rect(CGRect f) {
    if (g_full && f.size.width > 0 && f.size.height > 0) {   // 真全屏：游戏视图跟宿主的读数走（纵向位置只有那一个来源），每拍比对、变了重摆 | True fullscreen: the game view follows the host's readings (the only source for the vertical position); compared every tick and re-laid out on change
        // 局内（真实画面比 16:9 窄、界面矩形比画面矮）= 界面矩形，贴底；局外（前端各页、读取画面，游戏整体按 16:9 出）= 窗口里居中的那块 16:9。宿主还没读到画面宽高前按局外摆
        // In a run (real picture narrower than 16:9, UI rectangle shorter than the picture) = the UI rectangle, anchored to the bottom; out of a run (front-end pages, loading screen; the game outputs everything at 16:9) = the centered 16:9 block in the window. Before the host has read the picture width and height, lay out as out of a run
        int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); CGFloat k = UIScreen.mainScreen.nativeScale, vh = floor(f.size.width * 9 / 16), vy = floor((f.size.height - vh) / 2);
        if (dm_screen_real(&rw, &rh) && h > 0 && h < rh) { vy = round(f.size.height * y / rh * k) / k; vh = round(f.size.height * h / rh * k) / k; }
        else if (rw > 0 && rh > 0 && (double)rw * 9 > (double)rh * 16) { vh = round(f.size.width * rh / rw * k) / k; vy = round((f.size.height - vh) / 2 * k) / k; }   // 局外画面比 16:9 还宽时按它的比例；读数比 16:9 窄而界面矩形又没变矮 = 游戏还没写入（开机缺省 800×600），按 16:9 居中 | When the out-of-run picture is even wider than 16:9, use its aspect ratio; a reading narrower than 16:9 while the UI rectangle has not become shorter = the game has not written it yet (boot default 800×600), so center at 16:9
        return CGRectMake(f.origin.x, f.origin.y + vy, f.size.width, vh); }
    uint64_t r = front_game_res(1); double gw = (double)(uint32_t)r, gh = (double)(r >> 32);
    if (f.size.width <= 0 || f.size.height <= 0 || gw <= 0 || gh <= 0) return f;
    double ga = gw / gh, wa = f.size.width / f.size.height; if (fabs(wa / ga - 1) < 0.01) return f;
    CGFloat w = f.size.width, h = f.size.height;
    if (wa > ga) w = floor(h * ga); else h = floor(w / ga);   // 窗口更宽：左右留边；窗口更窄：上下留边 | Window wider: margins left and right; window narrower: margins top and bottom
    return CGRectMake(f.origin.x + floor((f.size.width - w) / 2), f.origin.y + floor((f.size.height - h) / 2), w, h);
}
@interface DmLetterboxView : UIView
@property (nonatomic, strong) UIView *game;
@end
// 真全屏下窗口变了尺寸（iPhone Duo 展开 / 折叠）：出图尺寸换成新窗口的原生像素（超过启动预留的就等比缩进去），游戏窗口建好之后推 31 让它重读
void dm_oe_res_changed(void);
// 只对手机：iPad 窗口不变尺寸，Mac 上的 iOS 包窗口能拖但出图一直按屏幕原生（回归与截图闸门的口径）；DM_TEST_WINDOW_PX=1 在 Mac 上打开，拖窗口验这条路
static void window_px(CGSize s, CGFloat k) {
    if (!g_full || getenv("DM_GAME_RES") || s.width <= 0 || s.height <= 0 || k <= 0) return;
    if (UIDevice.currentDevice.userInterfaceIdiom != UIUserInterfaceIdiomPhone && !getenv("DM_TEST_WINDOW_PX")) return;
    double w = round(fmax(s.width, s.height) * k), h = round(fmin(s.width, s.height) * k), cw = (uint32_t)g_res_cap, ch = (double)(g_res_cap >> 32), f = fmin(1, fmin(cw / w, ch / h));
    uint64_t v = (uint64_t)((unsigned)(h * f) & ~1u) << 32 | ((unsigned)(w * f) & ~1u), old = atomic_exchange(&g_res_now, v);
    if (old == v || !old) return;
    LOG("画面：窗口变成 %.0fx%.0f 点 → 出图 %ux%u（原 %ux%u，启动预留 %.0fx%.0f）%s", s.width, s.height, (unsigned)v, (unsigned)(v >> 32), (unsigned)old, (unsigned)(old >> 32), cw, ch,
        dm_uiscreen_res_live() ? "，通知游戏重读" : "，游戏窗口还没建好，建好后按新值");
    if (dm_uiscreen_res_live()) dm_oe_res_changed();
}
@implementation DmLetterboxView
- (void)layoutSubviews { [super layoutSubviews]; window_px(self.bounds.size, UIScreen.mainScreen.nativeScale); if (self.bounds.size.width > 0 && self.bounds.size.height > 0) self.game.frame = letterbox_rect(self.bounds);
    { static CGRect last; CGRect g = self.game.frame; if (g_full && !CGRectEqualToRect(g, last)) { last = g; int x, y, w, h, rw = 0, rh = 0; dm_ui_rect(&x, &y, &w, &h); dm_screen_real(&rw, &rh); LOG("画面：游戏视图摆到 y %.0f 高 %.0f（窗口 %.0fx%.0f；真实画面 %dx%d，界面矩形 y %d 高 %d）", g.origin.y, g.size.height, self.bounds.size.width, self.bounds.size.height, rw, rh, y, h); } }
    if (g_full && g_layer.superlayer == self.layer) { [CATransaction begin]; [CATransaction setDisableActions:YES]; g_layer.frame = self.bounds;
        g_layer.drawableSize = CGSizeMake(self.bounds.size.width * g_layer.contentsScale, self.bounds.size.height * g_layer.contentsScale); [CATransaction commit]; } }
// 真全屏：游戏视图上方那一截只有 3D，手指落在那里也交给游戏视图（纵坐标为负）——点怪 / 点箱子照常，界面与自绘控件不会被点到
// True fullscreen: the strip above the game view has only 3D; fingers landing there are also given to the game view (negative y) -- tapping monsters / chests works as usual, and the UI and custom-drawn widgets cannot be hit
- (UIView *)hitTest:(CGPoint)p withEvent:(UIEvent *)e { UIView *h = [super hitTest:p withEvent:e]; return g_full && h == self ? self.game : h; }
@end
// 测试：模拟窗口变尺寸（iPhone Duo 展开 / 折叠）。Mac 上的 iOS 包窗口不能拖（要求全屏），用这个到点把最外层视图改成给定点数，走的是同一段布局代码。
// DM_TEST_FOLD=<秒>:<宽>x<高>[,<秒>:<宽>x<高>…]，秒数从视图建好时算；配 DM_TEST_WINDOW_PX=1 让 Mac 上也跟着换出图尺寸
static UIView *test_fold(UIView *top) {
    const char *e = getenv("DM_TEST_FOLD"); if (!e) return top;
    for (const char *q = e; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : NULL) {
        double t = 0, w = 0, h = 0; if (sscanf(q, "%lf:%lfx%lf", &t, &w, &h) != 3 || w <= 0 || h <= 0) continue;
        __weak UIView *wt = top; dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(t * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            UIView *u = wt; if (!u) return; LOG("测试：视图改成 %.0fx%.0f 点（DM_TEST_FOLD）", w, h); u.frame = CGRectMake(0, 0, w, h); [u setNeedsLayout]; [u layoutIfNeeded]; });
    }
    return top;
}
UIView *front_ios_view(CGRect frame) {
    front_game_res(1);   // 「真全屏」开没开（g_full）是它第一次被调时按屏幕比例定的；下面摆视图要用，不能指望别处碰巧先调过 | whether "true fullscreen" is on (g_full) is decided from the screen aspect ratio the first time this is called; the view layout below needs it and cannot count on something else happening to have called it first
    CGRect full = frame; frame = letterbox_rect(frame);
    dm_tc_set_tall_scale(UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad);   // UIKit 点数：iPad 开启有上限的平板缩放（手机大屏另在布局时按短边判，见 DmMetalView layoutSubviews）
    DmMetalView *v = [[DmMetalView alloc] initWithFrame:frame];
    g_rate_link = [CADisplayLink displayLinkWithTarget:[DmRateHint new] selector:@selector(tick:)];
    g_rate_link.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
    [g_rate_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    g_layer = (CAMetalLayer *)v.layer;
    g_layer.device = MTLCreateSystemDefaultDevice();
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.contentsScale = UIScreen.mainScreen.nativeScale;
    g_layer.drawableSize = CGSizeMake(frame.size.width * g_layer.contentsScale, frame.size.height * g_layer.contentsScale);
    // 真全屏一律套容器：窗口会变尺寸（iPhone Duo 展开 / 折叠），容器的布局回调负责跟着换出图尺寸；启动那一刻窗口碰巧是 16:9 也要套（Mac 上的场景初值 667x375）
    if (g_full || !CGRectEqualToRect(full, frame)) { DmLetterboxView *box = [[DmLetterboxView alloc] initWithFrame:full]; box.backgroundColor = UIColor.blackColor; box.game = v; [box addSubview:v];
        if (g_full) {   // 出图层与游戏视图分离：出图层挂在容器上铺满整窗，游戏视图透明、叠在它上面 | The present layer is separated from the game view: the present layer hangs on the container and fills the whole window; the game view is transparent and stacked on top of it
            CAMetalLayer *ml = [CAMetalLayer layer]; ml.device = g_layer.device; ml.pixelFormat = MTLPixelFormatBGRA8Unorm; ml.contentsScale = g_layer.contentsScale; ml.frame = box.bounds;
            ml.drawableSize = CGSizeMake(full.size.width * ml.contentsScale, full.size.height * ml.contentsScale); [box.layer insertSublayer:ml atIndex:0];
            v.opaque = NO; v.layer.opaque = NO; v.backgroundColor = UIColor.clearColor; g_layer = ml; mtl_set_ui_rect_fn(ui_rect_frac);
            LOG("画面：窗口 %.0fx%.0f 比 16:9 窄 → 真全屏已开：出图层铺满整窗；游戏视图跟宿主的界面矩形走（局内贴底、局外居中），启动先按居中的 %.0fx%.0f 摆", full.size.width, full.size.height, frame.size.width, frame.size.height); return test_fold(box); }
        LOG("画面：窗口 %.0fx%.0f 与游戏画面比例不同 → 游戏视图 %.0fx%.0f 居中（左右各留 %.0f 点、上下各留 %.0f 点）", full.size.width, full.size.height, frame.size.width, frame.size.height, frame.origin.x - full.origin.x, frame.origin.y - full.origin.y); return test_fold(box); }
    return test_fold(v);
}

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
// 主机档 = 屏幕原生像素（界面文字原生，3D 由游戏自带的渲染比例另缩，见 hle_oe.c scale3d_tick）：
// iPhone 17 Pro Max → 2868x1320。掌机档仍按原版高度 720 × 屏幕比例（宽取 8 的倍数，1568x720），只在 DM_OPMODE=0 / 脚本切换时用到。
// DM_GAME_RES=宽x高（主机模式；掌机按 720/900 同比）覆盖：DM_GAME_RES=1600x900 即原版 16:9 保底（负对照），DM_GAME_RES=1952x900 即早先的缺省
// 窗口会变尺寸的设备（iPhone Duo 内外屏切换、展开 / 折叠时窗口当场改尺寸）：真全屏下出图尺寸跟着窗口走（window_px），
// 推一次性能模式通知（31）让游戏重读。游戏只接受不超过启动时窗口尺寸的新值（dm_uiscreen.c 的说明），所以比 16:9 窄的手机屏
// （只有折叠屏是这个形状）启动时按短边 2160 预留：外屏启动也装得下内屏。iPad、普通 iPhone 的窗口不变尺寸，启动值 = 屏幕原生，与原来相同。
uint64_t front_game_res(int docked) {
    static double aspect; static unsigned fw, fh;
    if (!aspect) {
        CGRect b = UIScreen.mainScreen.nativeBounds;
        // 测试：DM_TEST_SCREEN=<宽>x<高> 把屏幕当成给定像素的手机屏（Mac 上的 iOS 包模拟 iPhone Duo 外屏 2034x1398，配 DM_TEST_FOLD / DM_TEST_WINDOW_PX）
        const char *ts = getenv("DM_TEST_SCREEN"); unsigned tw = 0, th = 0; int phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
        if (ts && sscanf(ts, "%ux%u", &tw, &th) == 2 && tw && th) { b = CGRectMake(0, 0, tw, th); phone = 1; }
        aspect = fmax(b.size.width, b.size.height) / fmin(b.size.width, b.size.height);
        const char *e = getenv("DM_GAME_RES"); if (e && sscanf(e, "%ux%u", &fw, &fh) == 2) aspect = (double)fw / fh;
        else { fw = (unsigned)fmax(b.size.width, b.size.height); fh = (unsigned)fmin(b.size.width, b.size.height);
            // 比 16:9 窄的屏（iPad 4:3）：游戏界面按屏高缩放、按 ≥16:9 设计，4:3 下面板横向放不下。按屏宽取 16:9，画面居中、上下留黑边
            // 第二步（真全屏，缺省）：局内画面按屏幕原生比例出、3D 铺满，界面由宿主按 16:9 摆并贴底；前端各页与读取画面仍是 16:9 居中留边（dm_uiscreen）。
            // DM_IPAD_FULL=0 退回上面那种全程 16:9 加黑边
            // Screens narrower than 16:9 (iPad 4:3): the game UI scales by screen height and is designed for ≥16:9; on 4:3 the panels do not fit horizontally. Take 16:9 by screen width, center the picture, black bars top and bottom
            // Step two (true fullscreen, default): the in-run picture is output at the screen's native aspect ratio with 3D filling it, and the UI is laid out by the host at 16:9 and anchored to the bottom; front-end pages and the loading screen are still 16:9 centered with margins (dm_uiscreen).
            // DM_IPAD_FULL=0 falls back to the above: 16:9 with black bars throughout
            if (aspect < 16.0 / 9 - 0.01) { const char *fu = getenv("DM_IPAD_FULL");
                if (!fu || atoi(fu)) { g_full = 1; dm_uiscreen_enable(DM_UISCREEN_BOTTOM); } else { aspect = 16.0 / 9; fh = (unsigned)llround(fw * 9.0 / 16 / 2) * 2; } } }
        g_res_cap = (uint64_t)fh << 32 | fw;
        if (g_full && !e && phone && fh < 2160) {
            unsigned cw = (unsigned)llround(2160.0 * fw / fh / 2) * 2; g_res_cap = (uint64_t)2160 << 32 | cw;
            LOG("画面：比 16:9 窄的手机屏 %ux%u（折叠屏）→ 窗口按 %ux2160 预留，内外屏切换时出图尺寸在这以内跟着窗口变", fw, fh, cw); }
        atomic_store(&g_res_now, (uint64_t)fh << 32 | fw);
    }
    if (fw && docked) return dm_uiscreen_res_live() ? atomic_load(&g_res_now) : g_res_cap;
    uint64_t h = docked ? 900 : 720, w = (uint64_t)llround(h * aspect / 8) * 8;
    return h << 32 | w;
}

void front_run(void (*guest)(void)) {
    static char alt[65536];                       // 信号备用栈是每线程的：客体线程栈溢出时崩溃打印也要能跑 | The alternate signal stack is per-thread: the crash printout must still be able to run when a guest thread overflows its stack
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    // 本线程由 UIKit 主线程派生，继承了它的信号屏蔽字（真机实测 SIGUSR2 / SIGALRM 均被屏蔽：DM_SAMPLE 的处理函数从不运行）。
    // 客体线程要收宿主诊断信号（采样 SIGUSR2、看门狗 SIGALRM），全部解除；同步类（SEGV / BUS / ILL / TRAP）本就不受屏蔽影响
    // This thread is spawned from the UIKit main thread and inherits its signal mask (measured on a real device: SIGUSR2 / SIGALRM are both blocked: the DM_SAMPLE handler never runs).
    // Guest threads need to receive the host's diagnostic signals (sampling SIGUSR2, watchdog SIGALRM), so unblock all of them; synchronous ones (SEGV / BUS / ILL / TRAP) are unaffected by the mask anyway
    sigset_t all; sigfillset(&all); pthread_sigmask(SIG_UNBLOCK, &all, NULL);
    void uimsg_subscribe(void (*)(const char *,const char *,const char *));
    uimsg_subscribe(touch_ui_message);
    dm_touch_game_live_check(1);
    if(!dm_touch_game_attach()) LOG("【红】技能绑定读取没挂上（产物没有观测门 OBS_BINDING_GETTER，或 main 模块不符）：技能键图标 / 回城键绑定不工作");   // 产品功能缺门不许静默 | A product feature missing its gate must not be silent
    guest();
}

// 软键盘输入框（front.h）：主线程弹 UIAlertController + 文本框，客体线程阻塞到确定 / 取消。无窗口（非 boot 模式）不弹，返回 -1
// 测试脚本驱动真输入对话框用（触点脚本的 type / alert 两个动作）：当前弹着的对话框与它的结束回调。产品路径不读这两个量。
// Software keyboard input box (front.h): the main thread presents a UIAlertController + text field, and the guest thread blocks until OK / Cancel. With no window (non-boot mode) nothing is presented and -1 is returned
// For test scripts driving the real input dialog (the type / alert actions of the touch script): the dialog currently shown and its completion callback. The product path does not read these two.
static UIAlertController *g_text_alert; static void (^g_text_alert_finish)(int);
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    if (!g_layer) return -1;
    NSString *t = title && *title ? @(title) : SWT("dmInputTitle");
    NSString *msg = [NSString stringWithFormat:@"%s%s%s", sub ? sub : "", sub && *sub && guide && *guide ? "\n" : "", guide ? guide : ""];
    extern int g_swkbd_name_panel; int name_rule = g_swkbd_name_panel;   // 建角起名：按游戏的角色名规则拦（hle_swkbd.c 头注） | Naming at character creation: enforce the game's character name rules (hle_swkbd.c header comment)
    if (name_rule) { NSString *rule = SWT("dmNameRule"); t = title && *title ? @(title) : SWT("dmNameTitle"); msg = msg.length ? [msg stringByAppendingFormat:@"\n%@", rule] : rule; }
    extern int g_swkbd_numeric_max; int num_max = g_swkbd_numeric_max;   // ≥0：数量框（丢 / 存 / 取金币）——数字键盘，只留数字，超过上限当场改成上限（dm_textinput.c） | ≥0: quantity box (drop / deposit / withdraw gold) -- numeric keypad, digits only, values over the limit are changed to the limit on the spot (dm_textinput.c)
    if (num_max >= 0) { if (!(title && *title)) t = SWT("dmQtyTitle"); msg = [NSString stringWithFormat:@"0 – %d", num_max]; }
    NSString *init = initial ? @(initial) : @"", *ph = name_rule ? SWT("dmNameBlankRandom") : guide && *guide ? @(guide) : nil;   // 起名：名字框留空点「确定」= 让游戏随机取一个（原版「随机名字」键，dm_name_random），不加控件 | Naming: leaving the name field empty and tapping 「确定」(OK) = let the game pick a random one (the original game's 「随机名字」(Random Name) key, dm_name_random); no widget added
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block int r = 0; __block NSString *text = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *a = [UIAlertController alertControllerWithTitle:t message:msg preferredStyle:UIAlertControllerStyleAlert];
        // 系统浅色对话框盖在暗色游戏画面上很跳：强制深色外观，按钮用游戏里的金色字色（暗黑 2 金色文字 199 / 179 / 119），键盘也用深色。游戏风格的自绘输入框是设计点，另议
        // A light system dialog over the dark game picture is jarring: force dark appearance, use the game's gold text color for buttons (Diablo II gold text 199 / 179 / 119), and a dark keyboard too. A game-styled custom-drawn input box is a design item, to be discussed separately
        a.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
        a.view.tintColor = [UIColor colorWithRed:199 / 255.0 green:179 / 255.0 blue:119 / 255.0 alpha:1];
        // 起名：把游戏的名字规则前移到对话框（放行 1 个字母 → 游戏弹「不能少于 2 个字符」→ 要玩家自己再点名字框）。
        // 规则同游戏校验（main+0x7fed78 一套，逐字节）：2–15 字节、首尾必须是字母（汉字的字节按字母算，见 hle_charname.c）、- 与 _ 合计最多一个。
        // 字符集 / 超长 / 多余的 - _ 已在输入时拦掉，这里剩下「太短」「首尾是 - _」「还在组字」三种：不满足时「确定」置灰，说明里写出差哪一条
        // Naming: move the game's name rules forward into the dialog (letting 1 letter through → the game pops 「不能少于 2 个字符」(must be at least 2 characters) → the player has to tap the name field again).
        // Rules are the same as the game's validation (the main+0x7fed78 set, byte by byte): 2–15 bytes, first and last must be letters (bytes of Chinese characters count as letters, see hle_charname.c), at most one of - and _ combined.
        // Character set / over-length / extra - _ are already blocked while typing; what remains here is "too short", "starts or ends with - _" and "still composing": when unmet, 「确定」(OK) is greyed out and the message states which rule is unmet
        __block UIAlertAction *okAct = nil; __weak UIAlertController *wa = a;
        void (^name_check)(UITextField *) = ^(UITextField *tf) {
            if (!name_rule || !okAct) return;
            NSString *x = tf.text ?: @""; NSUInteger nb = [x lengthOfBytesUsingEncoding:NSUTF8StringEncoding]; NSString *why = nil;
            unichar c0 = x.length ? [x characterAtIndex:0] : 0, c1 = x.length ? [x characterAtIndex:x.length - 1] : 0;
            if (tf.markedTextRange) why = SWT("dmNamePickIme");
            else if (nb == 0) why = nil;   // 留空 = 随机取名 | Left empty = random naming
            else if (nb < 2) why = SWT("dmNameTooShort");
            else if (c0 == '-' || c0 == '_' || c1 == '-' || c1 == '_') why = SWT("dmNameEdge");
            okAct.enabled = why == nil;
            wa.message = why ? [msg stringByAppendingFormat:@"\n▸ %@", why] : msg;
        };
        [a addTextFieldWithConfigurationHandler:^(UITextField *f) {
            f.text = init; f.placeholder = ph; f.clearButtonMode = UITextFieldViewModeWhileEditing; f.keyboardAppearance = UIKeyboardAppearanceDark;
            if (name_rule) { f.autocorrectionType = UITextAutocorrectionTypeNo; f.autocapitalizationType = UITextAutocapitalizationTypeNone; f.spellCheckingType = UITextSpellCheckingTypeNo;
                [f addAction:[UIAction actionWithHandler:^(UIAction *act) {   // 当场按游戏规则拦：只留英文字母、汉字，'-' '_' 合计只留第一个；UTF-8 合计不超过 15 字节（超了从末尾整字去掉） | Enforce the game's rules on the spot: keep only English letters and Chinese characters; of '-' and '_' combined keep only the first; UTF-8 total at most 15 bytes (if over, remove whole characters from the end)
                    UITextField *tf = (UITextField *)act.sender; if (tf.markedTextRange) return;
                    NSMutableString *o = [NSMutableString string]; int dash = 0; NSUInteger bytes = 0;
                    for (NSUInteger i = 0; i < tf.text.length; i++) { unichar c = [tf.text characterAtIndex:i]; NSUInteger nb = 0;
                        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) nb = 1;
                        else if ((c == '-' || c == '_') && !dash) { nb = 1; dash = 1; }
                        else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) nb = 3;   // 中日韩统一表意文字（基本区 + 扩展 A） | CJK Unified Ideographs (basic block + Extension A)
                        if (!nb || bytes + nb > 15) continue;
                        [o appendFormat:@"%C", c]; bytes += nb; }
                    if (![o isEqualToString:tf.text]) tf.text = o;
                }] forControlEvents:UIControlEventEditingChanged]; }
            if (name_rule) [f addAction:[UIAction actionWithHandler:^(UIAction *act) { name_check((UITextField *)act.sender); }] forControlEvents:UIControlEventEditingChanged];   // 排在上面的过滤之后：量的是过滤后的文本 | Ordered after the filter above: what is measured is the filtered text
            if (num_max >= 0) { f.keyboardType = UIKeyboardTypeNumberPad;
                [f addAction:[UIAction actionWithHandler:^(UIAction *act) {
                    UITextField *tf = (UITextField *)act.sender; NSMutableString *o = [NSMutableString string]; long long v = 0;
                    for (NSUInteger i = 0; i < tf.text.length && o.length < 10; i++) { unichar c = [tf.text characterAtIndex:i]; if (c >= '0' && c <= '9') { [o appendFormat:@"%C", c]; v = v * 10 + (c - '0'); } }
                    if (v > num_max) [o setString:[NSString stringWithFormat:@"%d", num_max]];
                    if (![o isEqualToString:tf.text]) tf.text = o;
                }] forControlEvents:UIControlEventEditingChanged]; }
            // 输入时就按游戏给的字数上限拦（单位同 swkbd：UTF-16 单元），不等确定后才截；拼音 / 手写这类还在候选状态（有标记文本）时不动，上屏后再量
            // Enforce the game's character-count limit while typing (same unit as swkbd: UTF-16 units) instead of truncating only after OK; leave pinyin / handwriting input alone while it is still in candidate state (marked text present), and measure after it is committed
            if (max_chars > 0) [f addAction:[UIAction actionWithHandler:^(UIAction *act) {
                UITextField *tf = (UITextField *)act.sender;
                if (tf.markedTextRange || (int)tf.text.length <= max_chars) return;
                NSRange keep = [tf.text rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, (NSUInteger)max_chars)];
                if ((int)NSMaxRange(keep) > max_chars) keep = [tf.text rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, (NSUInteger)max_chars - 1)];   // 上限正落在一个字符中间：整个字符不要 | The limit falls right in the middle of a character: drop the whole character
                tf.text = [tf.text substringWithRange:keep];
            }] forControlEvents:UIControlEventEditingChanged];
        }];
        void (^finish)(int) = ^(int ok) { UIAlertController *sa = wa; g_text_alert = nil; g_text_alert_finish = nil; if (ok) text = sa.textFields.firstObject.text;   // 弱引用对话框：按钮回调 → finish → 对话框 → 按钮 是个环，对话框每弹一次留一个 | weak reference to the dialog: button callback -> finish -> dialog -> button is a cycle, leaving one dialog behind every time it is shown
            // 名字留空点确定 = 随机取名：我们自己抽一个（dm_namegen.c，约 1.8 万种、与已有角色重名就再抽），当作玩家输入的名字返回，走游戏原本的起名校验；只填进名字框不自动接受（同原版「随机名字」键，玩家看到再点创建）。
            // 原先发原版「随机名字」键，那是按职业在 12 个名字里依次轮，容易撞
            // Leaving the name empty and tapping OK = random naming: we draw one ourselves (dm_namegen.c, about 18,000 variants; redraw if it collides with an existing character), return it as the name the player typed, and go through the game's original name validation; it is only filled into the name field and not auto-accepted (same as the original game's 「随机名字」(Random Name) key: the player sees it and then taps create).
            // Previously we sent the original game's 「随机名字」(Random Name) key, which cycles through 12 names per class in order and collides easily
            if (ok && name_rule && !text.length) { char nm[32]; if (dm_name_gen(nm, sizeof nm, dm_name_taken)) { text = @(nm); extern _Atomic int g_fe_name_no_accept; atomic_store(&g_fe_name_no_accept, 1); LOG("起名：随机名 → 名字框「%s」（名字留空点确定）", nm); } else { ok = 0; LOG("起名：随机名 → 名字框「」【红】（已有同名角色，试满 64 次仍重名），名字框不动"); } }   // 日志句式同宿主 dm_actions.c，uireg two_names 按它判 | Log wording is the same as the host's dm_actions.c; uireg two_names judges by it
            r = ok; dispatch_semaphore_signal(done); };
        g_text_alert = a; g_text_alert_finish = [finish copy];
        [a addAction:[UIAlertAction actionWithTitle:SWT("dmCancel") style:UIAlertActionStyleCancel handler:^(UIAlertAction *x) { (void)x; finish(0); }]];
        okAct = [UIAlertAction actionWithTitle:SWT("dmConfirm") style:UIAlertActionStyleDefault handler:^(UIAlertAction *x) { (void)x; finish(1); }];
        [a addAction:okAct]; name_check(a.textFields.firstObject);
        // 找弹框的宿主：先找此刻的主窗口（isKeyWindow）；这一瞬间没有主窗口（iPad 分屏 / 台前调度 / 刚切回前台时焦点还没落定）就退到
        // 前台活跃场景的窗口，再退到任一窗口。原先找不到主窗口就直接当「取消」返回、一行日志都没有：数量框保持缺省的最大值，玩家点确认 = 全存 / 全取
        // （iPad 上是否走到这里要看日志，见「系统输入框没找到可弹的界面」这行）
        // Finding the host to present the alert on: first look for the current key window (isKeyWindow); if there is no key window at this instant (iPad Split View / Stage Manager / focus not yet settled right after returning to foreground), fall back to
        // a window of the foreground active scene, then to any window. Previously, failing to find the key window returned straight away as "cancel" without a single log line: the quantity box kept its default maximum, and the player tapping confirm = deposit all / withdraw all
        // (whether an iPad gets here must be checked in the log; see the line 「系统输入框没找到可弹的界面」(system input box found no UI to present on))
        UIViewController *vc = nil, *alt = nil, *any = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) {
                if (!w.rootViewController) continue;
                if (w.isKeyWindow) vc = w.rootViewController;
                else if (s.activationState == UISceneActivationStateForegroundActive && !alt) alt = w.rootViewController;
                else if (!any) any = w.rootViewController; }
        if (!vc) { vc = alt ?: any; LOG("前端：系统输入框——此刻没有主窗口，改用%s", alt ? "前台活跃场景的窗口" : any ? "任一窗口" : "（也没有）"); }
        while (vc.presentedViewController) vc = vc.presentedViewController;
        if (!vc) { LOG("前端：【红】系统输入框没找到可弹的界面，按取消返回（游戏那边的输入框保持原值）"); r = 0; dispatch_semaphore_signal(done); return; }
        [vc presentViewController:a animated:YES completion:^{
            // 测试开关 DM_TEST_KBD_TYPE="文本" 或 ":cancel"：对话框照常弹出，1.5 秒后把文本经文本框的「内容已变」事件填进去（走与手打同一条限长路径）再点确定 / 取消。
            // 验的是真对话框这条路（弹出、限长、回写、取消）；验不了的只有人手在系统键盘上敲字本身
            // Test switch DM_TEST_KBD_TYPE="text" or ":cancel": the dialog pops up as usual, and 1.5 seconds later the text is filled in via the text field's "content changed" event (the same length-limit path as typing by hand), then OK / Cancel is tapped.
            // This verifies the real dialog path (popup, length limit, write-back, cancel); the only thing it cannot verify is a human hand typing on the system keyboard itself
            const char *tk = getenv("DM_TEST_KBD_TYPE"); if (!tk) return;
            NSString *typed = @(tk);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500000000), dispatch_get_main_queue(), ^{
                int cancel = [typed isEqualToString:@":cancel"];
                UITextField *tf = a.textFields.firstObject;
                if (!cancel) { tf.text = typed; [tf sendActionsForControlEvents:UIControlEventEditingChanged]; }
                LOG("前端：测试键盘输入（DM_TEST_KBD_TYPE）%s「%s」→ 文本框现为「%s」", cancel ? "取消" : "填入", cancel ? "" : typed.UTF8String, tf.text.UTF8String ?: "");
                if (!cancel && okAct && !okAct.enabled) { LOG("前端：测试键盘输入——「确定」是灰的（%s），不点；1.5 秒后改按取消", a.message.UTF8String ?: ""); cancel = 1; }
                [a dismissViewControllerAnimated:NO completion:^{ finish(!cancel); }];
            });
        }];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (!r) return 0;
    snprintf(out, out_cap, "%s", text.UTF8String ? text.UTF8String : "");
    return 1;
}

// ---- 生命周期 / 过热 / 音频会话 → host（hle.h：dm_focus_set / dm_set_opmode / dm_audio_suspend）----
// 焦点（场景生命周期）：sceneWillResignActive → 2；sceneDidEnterBackground → 3 + 音频暂停 + GPU 挂起；sceneWillEnterForeground → GPU 恢复 + 音频恢复 + 2；sceneDidBecomeActive → 1。
//   iOS 27 真机实测：无场景清单的旧式 App 启动后 applicationState 恒为 1（前台未激活），收不到激活 / 失活，故改走 UIScene。
//   游戏收到焦点 3 只发 0x2c 事件、不自停渲染⇒ 进后台由宿主挡 GPU：dm_gfx_suspend(1) 等在途命令完成才返回（后台提交 GPU 会被系统杀）。
// 过热：thermalState 变化只记日志。3D 渲染比例的降 / 升由实际掉帧驱动（hle_oe.c 动态比例，0.75 → 0.72 → 0.70；界面始终原生）。
// 之前是切掌机模式：那会连界面一起降，且运行中切模式的路径有宽度掩码缺陷（main+0xe10e48，r27 修）。
//   DM_THERMAL_NOFALLBACK=1：过热不切掌机（只供原生功耗测量，读数不混入别的分辨率）。
// 音频会话：Playback（开着静音开关也出声，只听音量键——手游的行业惯例；和原先的 SoloAmbient 一样会停掉其它 App 的音乐）。
// 这个类别下系统不再替我们按静音开关静音，锁屏 / 切后台 / 来电时停声全靠下面的生命周期与打断处理（进后台、被打断都 dm_audio_suspend）；不开后台音频能力。
// 打断开始暂停、结束时恢复。
// ---- Lifecycle / overheating / audio session → host (hle.h: dm_focus_set / dm_set_opmode / dm_audio_suspend) ----
// Focus (scene lifecycle): sceneWillResignActive → 2; sceneDidEnterBackground → 3 + audio pause + GPU suspend; sceneWillEnterForeground → GPU resume + audio resume + 2; sceneDidBecomeActive → 1.
//   Measured on a real device with iOS 27: for a legacy App without a scene manifest, applicationState stays 1 (foreground inactive) after launch and activation / deactivation never arrive, hence the switch to UIScene.
//   On focus 3 the game only sends event 0x2c and does not stop rendering by itself ⇒ on entering background the host blocks the GPU: dm_gfx_suspend(1) returns only after in-flight commands complete (submitting GPU work in background gets killed by the system).
// Overheating: thermalState changes are only logged. Lowering / raising the 3D render scale is driven by actual dropped frames (hle_oe.c dynamic scale, 0.75 → 0.72 → 0.70; the UI is always native).
// Previously it switched to handheld mode: that lowered the UI too, and the path that switches mode while running had a width mask bug (main+0xe10e48, fixed in r27).
//   DM_THERMAL_NOFALLBACK=1: do not switch to handheld on overheating (only for native power measurement, so readings are not mixed with other resolutions).
// Audio session: Playback (sound plays even with the mute switch on and only the volume keys are obeyed -- the industry convention for mobile games; like the earlier SoloAmbient it stops other Apps' music).
// Under this category the system no longer mutes for us by the mute switch, and stopping sound on lock screen / going to background / incoming call relies entirely on the lifecycle and interruption handling below (entering background and being interrupted both call dm_audio_suspend); the background audio capability is not enabled.
// Pause when an interruption begins, resume when it ends.
#import <AVFoundation/AVFoundation.h>

// 存档事务被拒后只通知玩家：游戏线程不等待 UIKit，不更改下个事务的恢复 / 提交流程。
// 强实现覆盖 hle_fs.c 的弱日志实现；每个被拒事务由平台层调用一次。
// After a save transaction is rejected, only notify the player: the game thread does not wait for UIKit and the recovery / commit flow of the next transaction is unchanged.
// The strong implementation overrides the weak logging implementation in hle_fs.c; the platform layer calls it once per rejected transaction.
void front_save_failed(int err) {
    LOG("前端：存档写失败 errno=%d，异步提示玩家", err);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:UIWindowScene.class]) for (UIWindow *w in ((UIWindowScene *)s).windows) if (w.isKeyWindow) vc = w.rootViewController;
        while (vc.presentedViewController) vc = vc.presentedViewController;
        if (!vc) { LOG("【红】前端：存档失败提示没有窗口 errno=%d", err); return; }
        UIAlertController *a = [UIAlertController alertControllerWithTitle:SWT("dmSaveNoSpace") message:nil preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:SWT("dmConfirm") style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:a animated:YES completion:^{ LOG("前端：存档失败提示已显示 errno=%d", err); }];
    });
}

#define THERMAL_RESTORE_SEC 60

static void thermal_apply(void) {
    static unsigned gen;                       // 每次状态变化作废之前排的「恢复主机模式」 | Every state change invalidates the previously scheduled "restore docked mode"
    NSProcessInfoThermalState s = NSProcessInfo.processInfo.thermalState;
    unsigned my = ++gen;
    if (s >= NSProcessInfoThermalStateSerious) {
        if (getenv("DM_THERMAL_NOFALLBACK")) { LOG("前端：过热 %ld，DM_THERMAL_NOFALLBACK 不降档（只供功耗测量）", (long)s); return; }
        LOG("前端：过热 %ld（3D 比例改由实际掉帧驱动，这里只记一行）", (long)s); return; }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)THERMAL_RESTORE_SEC * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (my == gen && NSProcessInfo.processInfo.thermalState < NSProcessInfoThermalStateSerious) { LOG("前端：温度回落持续 %d 秒", THERMAL_RESTORE_SEC); }
    });
}

// 音频暂停有两个互不相干的原因：在后台、会话被打断（来电 / 闹钟）。两个都解除才恢复出声——
// 原先各自直接调 dm_audio_suspend：通话中切回游戏、或打断结束时人还在别的 App，都会在不该出声 / 出不了声的时候去启动声卡
// Audio pause has two unrelated reasons: in background, and session interrupted (phone call / alarm). Sound resumes only when both are cleared --
// previously each called dm_audio_suspend directly: switching back to the game during a call, or the interruption ending while the player is still in another App, would both start the audio device when it should not / cannot produce sound
static int g_audio_bg, g_audio_intr;
void guest_clock_freeze_for(int reason, int on);
static void audio_apply(void) { dm_audio_suspend(g_audio_bg || g_audio_intr); }
// 进后台回调里（系统给这个回调 10 秒）：局内才等。等到「发后台焦点之后出现过存档提交、且之后 0.2 秒没有新的提交」（角色与共享箱是两个容器，可能分两次），最多 2.5 秒。
// 这段时间游戏主循环照常跑，出图直接丢弃。不在局内（主菜单 / 选角）不等：那里没有待存的进度
// In the enter-background callback (the system gives this callback 10 seconds): wait only in a run. Wait until "a save commit has appeared after sending the background focus, and no new commit for 0.2 seconds after that" (character and shared stash are two containers and may commit separately), at most 2.5 seconds.
// During this time the game main loop runs as usual and presented frames are simply dropped. Outside a run (main menu / character select) do not wait: there is no unsaved progress there
static void background_wait_save(void) {
    int dm_ingame_now(void); void mtl_present_discard(int on); int dm_save_wait(double max_s, uint64_t *commits, int *froze, double *ms);
    if (!dm_ingame_now()) return;
    uint64_t n = 0; int froze = 0; double ms = 0;
    mtl_present_discard(1);
    dm_save_wait(2.5, &n, &froze, &ms);   // 游戏停着（暂停菜单开着）时冻住世界、临时放开暂停存这一次，存完写回（diag.c） | If the game is stopped (pause menu open), freeze the world and temporarily release the pause for this one save, then write it back (diag.c)
    mtl_present_discard(0);
    LOG("前端：进后台等游戏存盘 %.0f ms，期间存档提交 %llu 次%s%s", ms, (unsigned long long)n, froze ? "（原先暂停着：冻结世界存盘、已恢复暂停）" : "", n ? "" : "【红】（2.5 秒内没等到）");
}
// 失活时也请游戏存一次盘（查 #22 发现的缺口）：上划进多任务界面时 App 多半只「失活」、不进后台，在那里直接划掉进程
// 走不到进后台的存盘，丢掉距上次定时存盘（游戏 5 分钟一次）以来的进度。只置游戏「下一拍就自动存盘」的计时（同 dm_request_save），不等提交、不挡主线程；
// 30 秒内只请一次（通知中心 / 控制中心来回拉不重复存）。恢复活跃什么都不做
// Also ask the game to save once on resign active (gap found while investigating #22): when swiping up into the app switcher the App mostly only "resigns active" without entering background, and swiping the process away there
// never reaches the enter-background save, losing the progress since the last timed save (the game saves every 5 minutes). Only sets the game's "auto-save on the next tick" timer (same as dm_request_save), does not wait for the commit and does not block the main thread;
// requested at most once per 30 seconds (pulling Notification Center / Control Center back and forth does not save repeatedly). Becoming active again does nothing
static void resign_save(void) {
    int dm_ingame_now(void); int dm_request_save(void); uint64_t now_ns(void);
    static uint64_t last; uint64_t now = now_ns();
    if (!dm_ingame_now() || (last && now - last < 30000000000ull)) return;
    if (dm_request_save()) { last = now; LOG("前端：失活，请游戏下一拍存一次盘（不等）"); }
}
// 场景生命周期（main.m 的 SceneDelegate 调）：0 失活 1 进后台 2 回前台 3 激活
// Scene lifecycle (called by SceneDelegate in main.m): 0 resign active, 1 enter background, 2 return to foreground, 3 become active
void front_ios_focus_event(int ev) {
    static const char *k[] = {"sceneWillResignActive", "sceneDidEnterBackground", "sceneWillEnterForeground", "sceneDidBecomeActive"};
    { uint64_t guest_ns(void); LOG("前端：%s（客体时钟 %.3f s，声卡回调累计 %llu）", k[ev], guest_ns() / 1e9, (unsigned long long)dm_audio_render_calls()); }   // 两个读数：核对后台那段没算进游戏时间、回来后声卡回调继续涨 | Two readings: to check that the background period was not counted into game time and that audio device callbacks keep increasing after return
    switch (ev) {
        case 0: [g_touch_view cancelControls]; { void dm_gamepad_reset(void); dm_gamepad_reset(); } dm_focus_set(2); resign_save(); break;   // 手柄的键和摇杆一并清零：按着键切走、在后台松开，回来不会补「松开」 | Controller buttons and sticks are zeroed as well: if a button is held while switching away and released in the background, no "release" is delivered on return
        // 进后台：客体时钟一并冻结（等效 Switch 挂起）——否则游戏线程挡在提交处、墙钟照走，回来时游戏内时间一下跳过整段后台时长（补拍 / 动画 / 冷却全跟着跳）
        // Entering background: the guest clock is frozen as well (equivalent to Switch suspend) -- otherwise the game thread is blocked at submission while the wall clock keeps running, and on return in-game time jumps over the whole background duration at once (catch-up ticks / animations / cooldowns all jump with it)
        case 1: dm_focus_set(3); dm_ui_lifecycle(1); g_audio_bg = 1; audio_apply(); { void dm_cloud_flush(int); dm_cloud_flush(1); }   // iCloud：局内攒下的存档切后台时传掉（cloud_ios.m） | iCloud: saves accumulated in the run are uploaded when going to background (cloud_ios.m)
            background_wait_save();   // 进后台先让游戏存一次盘并等提交完成，再挂起：手机上切出去后进程随时可能被系统回收，原版 5 分钟才自动存一次 | On entering background, first have the game save once and wait for the commit to finish, then suspend: on a phone the process may be reclaimed by the system at any time after switching away, and the original game only auto-saves every 5 minutes
            guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); break;   // 挂起返回前在途 GPU 已清空 | In-flight GPU work is already drained before the suspend returns
        case 2: dm_ui_lifecycle(0); { void dm_update_foreground(void); dm_update_foreground(); void dm_cloud_foreground(void); dm_cloud_foreground(); }   // 回前台时人若停在选角页，重查一次新版本（dm/host/dm_update.c；局内不查） | If the player is on the character select page when returning to foreground, check for a new version once more (dm/host/dm_update.c; not checked in a run)
            { void dm_3d_scale_resumed(void); dm_3d_scale_resumed(); } dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); g_audio_bg = g_audio_intr = 0; audio_apply(); dm_focus_set(2); break;   // 回前台一律当打断已过：进后台时系统自己会发一次「被打断」（真机实测），却不保证回来时补发「结束」；会话若真还被占，启动声卡失败不致命、激活时重试 | Returning to foreground always treats the interruption as over: when entering background the system itself sends one "interrupted" (measured on a real device) but does not guarantee an "ended" on return; if the session is really still occupied, failing to start the audio device is not fatal and is retried on activation
        case 3: dm_focus_set(1); audio_apply(); break;   // 激活时再对一次音频：上次恢复没成（会话还被占）的在这里重试 | Reconcile audio once more on activation: a previous resume that failed (session still occupied) is retried here
    }
}

// 整窗截图（含 Metal 游戏画面与操作层）写进 Documents/<name>，旁写 <name>.txt：窗口点尺寸、缩放、方向、安全区。
// 不对窗口做 drawViewHierarchy（两次合入冒烟卡死都紧跟在它之后：它要截挂着显示节拍的 Metal 图层，之后节拍停了回调；读转储）：
// 游戏画面由出图线程在下一次上屏时离屏拷一份（nvn_mtl.m mtl_grab_next_present，与上屏同一套编码、同尺寸）；拼图在主线程：黑底 → 游戏画面画在出图层的位置 →
// 出图层之上的各层（它自己的子层，和到窗口一路上每层里排在它后面的兄弟层）逐个 renderInContext。出图层底下只有黑底（留边容器 / 窗口），不另画。
// 1 秒内没等到上屏（节拍停了 / 进了后台）就报红不截，不等
// Whole-window screenshot (including the Metal game picture and the control layer) written into Documents/<name>, with <name>.txt alongside: window size in points, scale, orientation, safe area.
// No drawViewHierarchy on the window (both merge smoke test hangs came right after it: it has to capture the Metal layer that carries the display link, after which the display link stopped calling back; from reading the dump):
// the game picture is copied off-screen by the present thread at the next present (nvn_mtl.m mtl_grab_next_present, same encoding and same size as the present); compositing is on the main thread: black base → game picture drawn at the present layer's position →
// the layers above the present layer (its own sublayers, and at every level on the way up to the window the sibling layers ordered after it) renderInContext one by one. Below the present layer there is only the black base (margin container / window), nothing else is drawn.
// If no present arrives within 1 second (display link stopped / went to background), report a failure and do not capture, do not wait
void mtl_grab_next_present(void (^cb)(CGImageRef img));
static void window_shot(NSString *name) {
    UIWindowScene *ws = nil; UIWindow *win = nil;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes) if ([s isKindOfClass:UIWindowScene.class]) ws = (UIWindowScene *)s;
    for (UIWindow *w in ws.windows) if (w.isKeyWindow) win = w;
    CALayer *gl = (__bridge CALayer *)front_metal_layer();
    if (!win || !gl) { LOG("【红】DM_SCREEN_SHOT：没有窗口 / 出图层"); return; }
    static uint64_t seq; uint64_t my = ++seq; __block int got = 0;
    mtl_grab_next_present(^(CGImageRef game) {
        if (game) CGImageRetain(game);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (my != seq || got) { if (game) CGImageRelease(game); return; } got = 1;
            if (!game) { LOG("【红】DM_SCREEN_SHOT：%s 没拿到游戏画面", name.UTF8String); return; }
            NSMutableArray<CALayer *> *above = [NSMutableArray arrayWithArray:gl.sublayers ?: @[]];
            for (CALayer *ch = gl, *p = gl.superlayer; p; ch = p, p = p.superlayer) {
                NSUInteger i = [p.sublayers indexOfObject:ch]; if (i != NSNotFound) for (NSUInteger j = i + 1; j < p.sublayers.count; j++) [above addObject:p.sublayers[j]];
                if (p == win.layer) break; }
            UIGraphicsImageRendererFormat *fm = [UIGraphicsImageRendererFormat defaultFormat]; fm.scale = ws.screen.scale; fm.opaque = YES;
            UIImage *img = [[[UIGraphicsImageRenderer alloc] initWithSize:win.bounds.size format:fm] imageWithActions:^(UIGraphicsImageRendererContext *c) {
                CGContextRef cg = c.CGContext; [UIColor.blackColor setFill]; UIRectFill(win.bounds);
                [[UIImage imageWithCGImage:game] drawInRect:[gl convertRect:gl.bounds toLayer:win.layer]];
                for (CALayer *l in above) { if (l.hidden || l.opacity <= 0) continue; CGRect r = [l convertRect:l.bounds toLayer:win.layer];
                    CGContextSaveGState(cg); CGContextTranslateCTM(cg, r.origin.x, r.origin.y); [l renderInContext:cg]; CGContextRestoreGState(cg); } }];
            CGImageRelease(game);
            NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
            [UIImagePNGRepresentation(img) writeToFile:[doc stringByAppendingPathComponent:name] atomically:YES];
            UIEdgeInsets si = win.safeAreaInsets;   // 安全区（点）：灵动岛 / 圆角 / 底部条，界面元素须在其内 | Safe area (points): Dynamic Island / rounded corners / home indicator; UI elements must stay inside it
            NSString *meta = [NSString stringWithFormat:@"window_pt\t%.0f\t%.0f\nscale\t%.0f\norientation\t%ld\nsafe_pt\t%.1f\t%.1f\t%.1f\t%.1f\n", win.bounds.size.width, win.bounds.size.height,
                              ws.screen.scale, (long)ws.interfaceOrientation, si.top, si.left, si.bottom, si.right];
            [meta writeToFile:[doc stringByAppendingPathComponent:[name stringByAppendingString:@".txt"]] atomically:YES encoding:NSUTF8StringEncoding error:nil];
            LOG("DM_SCREEN_SHOT：已截 %s（%.0fx%.0f 点 ×%.0f，方向 %ld；游戏画面取自上屏帧）", name.UTF8String, win.bounds.size.width, win.bounds.size.height, ws.screen.scale, (long)ws.interfaceOrientation);
        });
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (my != seq || got) return; got = 1; mtl_grab_next_present(nil);
        LOG("【红】DM_SCREEN_SHOT：%s 1 秒内没有上屏帧（显示节拍停了 / 在后台？），没截", name.UTF8String); });
}

// ---------- 测试脚本触点（DM_TOUCH_SCRIPT=Documents 内文件名）----------
// 首次进入游戏态（非导航）后按时间执行：「秒 down|move 槽 位置」「秒 up 槽」「秒 shot|tshot 文件名」「秒 note 文字」；
// 位置 = "x,y"（点）或 "@目标[+dx,dy]"。触点走 DmMetalView 真实 touches* 方法，之后与手指同一路径。
// ---------- Test script touch points (DM_TOUCH_SCRIPT=file name inside Documents) ----------
// Executed by time after first entering game mode (non-navigation): "seconds down|move slot position" "seconds up slot" "seconds shot|tshot file name" "seconds note text";
// position = "x,y" (points) or "@target[+dx,dy]". Touch points go through DmMetalView's real touches* methods, and from there follow the same path as fingers.
static void touch_script_start(void) {
    static int started; const char *e=getenv("DM_TOUCH_SCRIPT"); if(started || !e) return; started=1;
    NSString *doc=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES)[0];
    NSError *rerr=nil; NSString *spath=[doc stringByAppendingPathComponent:@(e)];
    NSString *text=[NSString stringWithContentsOfFile:spath encoding:NSUTF8StringEncoding error:&rerr];
    if(!text) { NSDictionary *fa=[NSFileManager.defaultManager attributesOfFileSystemForPath:doc error:nil];
        LOG("[script] 【红】读不到脚本 %s：%s（路径 %s，存在 %d；主目录 %s；所在卷剩余 %.2f GB）",e,rerr.description.UTF8String ?: "无错误信息",spath.UTF8String,(int)[NSFileManager.defaultManager fileExistsAtPath:spath],NSHomeDirectory().UTF8String,[fa[NSFileSystemFreeSize] doubleValue]/1e9); return; }   // 真机连续多轮读不到刚拷上去的脚本（文件在、权限 0644），原来不带原因查不下去 | On a real device several consecutive rounds could not read a script that had just been copied over (file present, permission 0644); previously no reason was logged so it could not be investigated
    // 步骤按时间排好，由 120 Hz 主线程计时器逐条执行（dispatch_after 的长延时有秒级合并误差，点按会被压成同一刻）
    // Steps are sorted by time and executed one by one by a 120 Hz main-thread timer (long dispatch_after delays have second-level coalescing error, which would squash taps into the same instant)
    NSMutableArray<NSArray *> *steps=[NSMutableArray array];
    for(NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSString *line=[raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if(!line.length || [line hasPrefix:@"#"]) continue;
        [steps addObject:[line componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet]];
    }
    [steps sortWithOptions:NSSortStable usingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return [@([a[0] doubleValue]) compare:@([b[0] doubleValue])]; }];
    int n=(int)steps.count; __block NSUInteger next=0; __block double t0=CACurrentMediaTime();
    // walk_to <x> <y> [半径=6] [超时秒=15]：闭环走位（只测试用）——每 0.1 秒读一次玩家世界坐标，朝目标推摇杆，到半径内或超时即松手并记一行。
    // 世界坐标到屏幕方向按暗黑 2 的斜 45° 投影：屏幕右 = 世界 (+x, −y)，屏幕下 = 世界 (+x, +y)。占用脚本手指 15 号；脚本作者给后面的步骤留够超时时间。
    // walk_to <x> <y> [radius=6] [timeout seconds=15]: closed-loop walking (test only) -- read the player's world coordinates every 0.1 seconds and push the stick toward the target; release and log one line once within the radius or on timeout.
    // World coordinates map to screen directions by Diablo II's 45° isometric projection: screen right = world (+x, −y), screen down = world (+x, +y). Occupies script finger 15; the script author leaves enough timeout for the following steps.
    __block int wk_on=0, wk_tx=0, wk_ty=0, wk_r=6, wk_tick=0; __block double wk_dead=0;
    // tap_unit <类型> <类号> [超时秒=10] [x0 y0 x1 y1]：等这类单位出现在画面里（给了矩形就等它走进这块窗口点数矩形）→ 在它身体框中心落一下真实触点（手指 0）。
    // 位置取游戏自己的单位投影（dm_world_watch），只测试用：会走动的 NPC 用固定屏幕坐标点不中。
    // 末尾写成「+dx,dy」= 落点相对单位中心偏这么多窗口点（NPC 对话菜单盖在 NPC 身上、随 NPC 走，菜单行用它定位）。
    // tap_unit <type> <class id> [timeout seconds=10] [x0 y0 x1 y1]: wait for a unit of this class to appear on screen (if a rectangle is given, wait until it walks into this rectangle in window points) → land one real touch point (finger 0) at the center of its body box.
    // The position comes from the game's own unit projection (dm_world_watch); test only: a walking NPC cannot be hit with fixed screen coordinates.
    // A trailing "+dx,dy" = the landing point is offset from the unit center by this many window points (the NPC dialog menu sits on top of the NPC and moves with it; menu rows are located with it).
    __block int tu_on=0, tu_tick=0, tu_rect=0; __block double tu_dead=0, tu_up=0, tu_dx=0, tu_dy=0; static double tu_r[4];
    __block int ui_on=0, ui_tick=0; __block double ui_dead=0; __block NSString *ui_name=nil;
    // 跳过剩余等待（回归提速）：脚本的时刻是写死的，进局 / 走位 / 等测试开关把人带到位这类等待只能按最慢留够。
    // 「await pos [x y r]」= 玩家状态可读（给了坐标则还要走进这个半径）就把脚本时钟拨到下一步前 0.3 秒；「walk_to … skip」= 走到了同样拨。没等到就按原时刻走，行为与不写一样。
    // Skip the remaining wait (regression speed-up): script times are hard-coded, so waits like entering a run / walking / waiting for a test switch to bring the character into place have to be budgeted for the slowest case.
    // "await pos [x y r]" = once the player state is readable (and, if coordinates are given, the player has walked within this radius), set the script clock to 0.3 seconds before the next step; "walk_to … skip" = do the same on arrival. If the wait is not satisfied, proceed at the original time, behaving as if it were not written.
    __block int aw_on=0, aw_x=0, aw_y=0, aw_r=0, aw_tick=0, wk_skip=0;
    // 「await rw [超时秒=30]」= 等符文之语一键合成结束（宿主记完「成型读回」那一行、计数加一）就把脚本时钟拨到下一步前 0.3 秒；超时记红照原时刻走。
    // 判据按事件不按秒：读回由宿主在结束那一刻自己打，后面紧跟「note 结束」就是看到读回才收局，墙钟兜底 = 超时
    // "await rw [timeout seconds=30]" = once the runeword one-tap crafting finishes (the host has logged the 「成型读回」(formed readback) line and incremented the counter), set the script clock to 0.3 seconds before the next step; on timeout log a failure line and proceed at the original time.
    // The criterion is event-based, not second-based: the readback is printed by the host itself at the moment of finishing, so a 「note 结束」(note end) right after it means the run is closed only after seeing the readback; wall-clock backstop = timeout
    __block int awr_on=0, awr_base=0, awr_tick=0; __block double awr_dead=0;
    // 「await ui <控件名>[@<面板名>] [超时秒=30]」= 等这个界面控件可见（同 tap_ui 的观察：dm_ui_watch / dm_ui_watch_pos）就把脚本时钟拨到下一步前 0.3 秒，不点；
    // 超时记红照原时刻走。用在「测试开关按进程秒打开的面板」后面紧跟的点按（rwd：清单开得晚，点名字按秒等会落空）。与 tap_ui 共用一个观察槽：两者别同时挂
    // "await ui <widget name>[@<panel name>] [timeout seconds=30]" = once this UI widget is visible (same watch as tap_ui: dm_ui_watch / dm_ui_watch_pos), set the script clock to 0.3 seconds before the next step, without tapping;
    // on timeout log a failure line and proceed at the original time. Used for a tap that closely follows "a panel opened by a test switch at a process-second time" (rwd: the list opens late, so tapping the name after waiting by seconds would miss). Shares one watch slot with tap_ui: do not have both pending at once
    __block int aui_on=0, aui_tick=0; __block double aui_dead=0; __block NSString *aui_name=nil;
    void (^skip_wait)(const char *)=^(const char *why) { if(next>=steps.count) return; double T=[steps[next][0] doubleValue], nw=CACurrentMediaTime()-t0;
        if(T>nw+0.3) { t0-=T-nw-0.3; LOG("[script] %s：跳过剩余等待 %.1f 秒",why,T-nw-0.3); } };
    NSTimer *timer=[NSTimer timerWithTimeInterval:1.0/120 repeats:YES block:^(NSTimer *tm) {
        double now=CACurrentMediaTime()-t0;
        if(wk_on && (++wk_tick%12)==0) { dm_hud_info h; dm_hud_get(&h); double dx=wk_tx-h.x, dy=wk_ty-h.y, d=hypot(dx,dy);
            if(!h.valid || d<=wk_r || now>=wk_dead) { [g_touch_view scriptPhase:@"up" slot:15 at:nil]; wk_on=0;
                LOG("[script] walk_to (%d,%d) %s：现在 区域 %d (%d,%d)，差 %.0f",wk_tx,wk_ty,!h.valid?"玩家状态读不到":d<=wk_r?"到了":"超时",h.area,h.x,h.y,d); if(wk_skip && h.valid && d<=wk_r) skip_wait("walk_to 到了"); }
            else { double sx=dx-dy, sy=(dx+dy)/2, m=hypot(sx,sy); if(m<1e-6) m=1; [g_touch_view scriptPhase:@"move" slot:15 at:[NSString stringWithFormat:@"@joy+%d,%d",(int)lround(sx/m*40),(int)lround(sy/m*40)]]; } }
        if(aw_on && (++aw_tick%12)==0) { dm_hud_info h; dm_hud_get(&h);
            if(h.valid && h.area>0 && h.hp_max>0 && (!aw_r || hypot(aw_x-h.x,aw_y-h.y)<=aw_r)) { aw_on=0; LOG("[script] await pos：区域 %d (%d,%d)",h.area,h.x,h.y); skip_wait("await pos 等到了"); } }
        if(awr_on && (++awr_tick%12)==0) { int dm_runeword_done_count(void); int c=dm_runeword_done_count();
            if(c>awr_base) { awr_on=0; LOG("[script] await rw：一键合成结束（第 %d 次）",c); skip_wait("await rw 等到了"); }
            else if(now>=awr_dead) { awr_on=0; LOG("[script] 【红】await rw 超时：一键合成没结束"); } }
        if(aui_on && (++aui_tick%12)==0) { float nx=0, ny=0;
            if(dm_ui_watch_pos(&nx,&ny)) { aui_on=0; dm_ui_watch(NULL); LOG("[script] await ui %s：控件出现了",aui_name.UTF8String); skip_wait("await ui 等到了"); }
            else if(now>=aui_dead) { aui_on=0; dm_ui_watch(NULL); LOG("[script] 【红】await ui %s 超时：控件没出现",aui_name.UTF8String); } }
        if(tu_up && now>=tu_up) { [g_touch_view scriptPhase:@"up" slot:0 at:nil]; tu_up=0; }
        if(tu_on && (++tu_tick%12)==0) { float nx=0, ny=0; CGSize vs=g_touch_view.bounds.size; int have=dm_world_watch_pos(&nx,&ny); CGRect wr=world_rect(g_touch_view); double px=wr.origin.x+nx*wr.size.width+tu_dx, py=wr.origin.y+ny*wr.size.height+tu_dy; (void)vs;
            if(have && (!tu_rect || (px>=tu_r[0] && py>=tu_r[1] && px<=tu_r[2] && py<=tu_r[3]))) { tu_on=0; dm_world_watch(-1,0);
                LOG("[script] tap_unit：单位在窗口点 (%.0f,%.0f)，落触点",px,py); [g_touch_view scriptPhase:@"down" slot:0 at:[NSString stringWithFormat:@"=%.1f,%.1f",px,py]]; tu_up=now+0.12; }
            else if(now>=tu_dead) { tu_on=0; dm_world_watch(-1,0); if(have) LOG("[script] tap_unit 超时：单位在窗口点 (%.0f,%.0f)，没进指定矩形，没点",px,py); else LOG("[script] tap_unit 超时：画面里没有这类单位，没点"); } }
        // tap_ui <控件名>[@<面板名>] [超时秒=10]：等这个界面控件可见 → 在它中心落一下真实触点（手指 0）。位置由宿主用控件自己的命中函数求（dm_ui_watch），
        // 是游戏视图归一化坐标，直接乘本视图宽高（不走 world_rect：真全屏下世界按整窗算、界面按游戏视图算）。贴边控件用它，同一份脚本各种屏都点得中。
        // tap_ui <widget name>[@<panel name>] [timeout seconds=10]: wait for this UI widget to become visible → land one real touch point (finger 0) at its center. The position is computed by the host with the widget's own hit function (dm_ui_watch),
        // in game-view normalized coordinates, multiplied directly by this view's width and height (not via world_rect: in true fullscreen the world is computed on the whole window and the UI on the game view). Use it for edge-anchored widgets; the same script then hits them on every screen.
        if(ui_on && (++ui_tick%12)==0) { float nx=0, ny=0; CGSize vs=g_touch_view.bounds.size;
            if(dm_ui_watch_pos(&nx,&ny)) { ui_on=0; dm_ui_watch(NULL); double px=nx*vs.width, py=ny*vs.height;
                LOG("[script] tap_ui %s：控件在视图点 (%.0f,%.0f)，落触点",ui_name.UTF8String,px,py); [g_touch_view scriptPhase:@"down" slot:0 at:[NSString stringWithFormat:@"=%.1f,%.1f",px,py]]; tu_up=now+0.12; }
            else if(now>=ui_dead) { ui_on=0; dm_ui_watch(NULL); LOG("[script] 【红】tap_ui %s 超时：没有这个可见控件，没点",ui_name.UTF8String); } }
        // 落后了（游戏读条 / 长帧时计时器几百毫秒没跑）：整份时刻表往后顺延落后的量，只执行同一时刻的那几步，后面各步照原间隔走。
        // 原先一口气把到点的全部执行，按下 / 抬起 / 第二下按下挤在同一毫秒（菜单键按住 0.09 ms 游戏没看到、双击两下 0.46 ms 第一下没命中），量的是脚本而不是产品
        // Fell behind (the timer did not run for hundreds of ms during game loading / a long frame): shift the whole schedule later by the amount fallen behind, execute only the steps of the same instant, and let the later steps keep their original spacing.
        // Previously everything due was executed in one go, squeezing press / release / second press into the same millisecond (menu key held 0.09 ms and the game never saw it; double tap 0.46 ms apart and the first tap missed), measuring the script rather than the product
        if(next<steps.count && [steps[next][0] doubleValue]<=now) { double late=now-[steps[next][0] doubleValue];
            if(late>0.02) { t0+=late; now-=late; static int lg; if(late>0.1 && lg++<40) LOG("[script] 落后 %.2f 秒：后面各步顺延",late); } }
        while(next<steps.count && [steps[next][0] doubleValue]<=now) {
            NSArray *f=steps[next++]; NSString *op=f.count>1?f[1]:@""; aw_on=0;
            if([op isEqualToString:@"shot"]) window_shot(f[2]);
            else if([op isEqualToString:@"burst"] && f.count>=4) { void dm_gfx_burst(int, const char *); dm_gfx_burst([f[2] intValue], [doc stringByAppendingPathComponent:f[3]].UTF8String); LOG("[script] burst %d 帧 → %s", [f[2] intValue], [f[3] UTF8String]); }
            else if([op isEqualToString:@"tshot"]) [g_touch_view captureTouchLayer:[doc stringByAppendingPathComponent:f[2]]];   // 操作层 PNG + 控件/当前技能元数据 JSON | Control layer PNG + JSON metadata of widgets / current skills
            // type <文字>：往弹着的输入对话框里敲字——走 UITextField 的 insertText:（系统键盘上屏走的同一个入口，照样触发输入规则的拦截）；
            // alert ok|cancel：点对话框的「确定」/「取消」。两者都只在对话框弹着时有效，没弹着就记一行日志
            // type <text>: type into the input dialog currently shown -- goes through UITextField's insertText: (the same entry the system keyboard commits through, so input rule interception still fires);
            // alert ok|cancel: tap the dialog's 「确定」(OK) / 「取消」(Cancel). Both only work while a dialog is shown; otherwise one log line is written
            else if([op isEqualToString:@"walk_to"] && f.count>=4) { wk_tx=[f[2] intValue]; wk_ty=[f[3] intValue]; wk_r=f.count>4?[f[4] intValue]:6; wk_dead=now+(f.count>5?[f[5] doubleValue]:15); wk_tick=0; wk_skip=f.count>6 && [f[6] isEqualToString:@"skip"];
                if(wk_on) [g_touch_view scriptPhase:@"up" slot:15 at:nil];
                [g_touch_view scriptPhase:@"down" slot:15 at:@"@joy"]; wk_on=1; }
            else if([op isEqualToString:@"tap_unit"] && f.count>=4) { dm_world_watch([f[2] intValue],(uint32_t)[f[3] intValue]); tu_on=1; tu_tick=0; tu_dead=now+(f.count>4?[f[4] doubleValue]:10);
                NSString *last=f.lastObject; tu_dx=tu_dy=0; if([last hasPrefix:@"+"]) { NSArray *o=[[last substringFromIndex:1] componentsSeparatedByString:@","]; if(o.count==2) { tu_dx=[o[0] doubleValue]; tu_dy=[o[1] doubleValue]; } }
                tu_rect=f.count>=9 && ![f[5] hasPrefix:@"+"]; if(tu_rect) for(int i=0;i<4;i++) tu_r[i]=[f[5+i] doubleValue]; }
            // tap_ui / await ui 的控件名里「+」代表空格：脚本按空格拆参数，带空格的控件名（技能树的技能格 SkillButton3 2,0 = 第 3 页第 2 列第 0 行）要这样写，
            // 如 tap_ui SkillButton3+2,0@SkillsTreePanel。按名字点不受屏幕比例 / 真全屏贴顶上提影响（真机按 16:9 量的裸坐标点技能树偏了 134 单位）
            // In widget names for tap_ui / await ui, "+" stands for a space: scripts split arguments on spaces, so widget names with spaces (skill tree slot SkillButton3 2,0 = page 3, column 2, row 0) must be written this way,
            // e.g. tap_ui SkillButton3+2,0@SkillsTreePanel. Tapping by name is unaffected by screen aspect / the top shift in true fullscreen (raw coordinates measured at 16:9 on a real device missed the skill tree by 134 units)
            else if([op isEqualToString:@"tap_ui"] && f.count>=3) { ui_name=[f[2] stringByReplacingOccurrencesOfString:@"+" withString:@" "]; dm_ui_watch(ui_name.UTF8String); ui_on=1; ui_tick=0; ui_dead=now+(f.count>3?[f[3] doubleValue]:10); }
            else if([op isEqualToString:@"await"] && f.count>=4 && [f[2] isEqualToString:@"ui"]) { aui_name=[f[3] stringByReplacingOccurrencesOfString:@"+" withString:@" "]; dm_ui_watch(aui_name.UTF8String); aui_on=1; aui_tick=0; aui_dead=now+(f.count>4?[f[4] doubleValue]:30); }
            else if([op isEqualToString:@"await"] && f.count>=3 && [f[2] isEqualToString:@"rw"]) { int dm_runeword_done_count(void); awr_on=1; awr_tick=0; awr_base=dm_runeword_done_count(); awr_dead=now+(f.count>3?[f[3] doubleValue]:30); }
            else if([op isEqualToString:@"await"] && f.count>=3 && [f[2] isEqualToString:@"pos"]) { aw_on=1; aw_tick=0; aw_r=f.count>=6?[f[5] intValue]:0; aw_x=f.count>=6?[f[3] intValue]:0; aw_y=f.count>=6?[f[4] intValue]:0; }
            else if([op isEqualToString:@"pad"] && f.count>=3) { void dm_gamepad_inject(uint64_t,int32_t,int32_t,int32_t,int32_t); uint64_t m=strtoull([f[2] UTF8String],NULL,0);   // pad <按键位（同 dm_input.h）> [lx ly rx ry]：模拟手柄（没有实体手柄时测手柄路径） | pad <button bits (same as dm_input.h)> [lx ly rx ry]: simulated controller (tests the controller path without a physical controller)
                dm_gamepad_inject(m,f.count>3?[f[3] intValue]:0,f.count>4?[f[4] intValue]:0,f.count>5?[f[5] intValue]:0,f.count>6?[f[6] intValue]:0); LOG("[script] pad %#llx",(unsigned long long)m); }
            else if([op isEqualToString:@"pad2"] && f.count>=3) { void dm_gamepad_inject_n(int,uint64_t,int32_t,int32_t,int32_t,int32_t); uint64_t m=strtoull([f[2] UTF8String],NULL,0);   // pad2 …：第二个虚拟手柄，参数同 pad | pad2 ...: second virtual controller, same arguments as pad
                dm_gamepad_inject_n(2,m,f.count>3?[f[3] intValue]:0,f.count>4?[f[4] intValue]:0,f.count>5?[f[5] intValue]:0,f.count>6?[f[6] intValue]:0); LOG("[script] pad2 %#llx",(unsigned long long)m); }
            else if([op isEqualToString:@"padoff"] || [op isEqualToString:@"padoff2"]) { void dm_gamepad_drop_n(int); dm_gamepad_drop_n([op isEqualToString:@"padoff2"]?2:1); LOG("[script] %s",op.UTF8String); }   // padoff / padoff2：断开虚拟手柄 1 / 2 | padoff / padoff2: disconnect virtual controller 1 / 2
            else if([op isEqualToString:@"focus"] && f.count>=3) { front_ios_focus_event([f[2] intValue]); LOG("[script] focus %s",[f[2] UTF8String]); }   // focus <0 失活 / 3 回前台>：只发这两种，不真进后台 | focus <0 resign active / 3 return to foreground>: only sends these two events, without actually entering the background
            else if([op isEqualToString:@"padcur"] && f.count>=4) { void dm_padmode_test_cursor(float,float); dm_padmode_test_cursor([f[2] floatValue],[f[3] floatValue]); LOG("[script] padcur %s %s",[f[2] UTF8String],[f[3] UTF8String]); }   // padcur <nx> <ny>：把手柄光标放到视口归一化坐标（造现场；dm_padmode.c） | padcur <nx> <ny>: put the controller cursor at viewport-normalized coordinates (scenario setup; dm_padmode.c)
            else if([op isEqualToString:@"pos"]) { dm_hud_info h; dm_hud_get(&h); LOG("[script] pos 区域 %d (%d,%d) 生命 %d/%d",h.area,h.x,h.y,h.hp,h.hp_max); }
            else if([op isEqualToString:@"type"]) { UITextField *tf=g_text_alert.textFields.firstObject; NSString *tx=f.count>2?[[f subarrayWithRange:NSMakeRange(2,f.count-2)] componentsJoinedByString:@" "]:@"";
                if(tf) { for(NSUInteger i=0;i<tx.length;) { NSRange r=[tx rangeOfComposedCharacterSequenceAtIndex:i]; [tf insertText:[tx substringWithRange:r]]; i=NSMaxRange(r); } LOG("[script] type「%s」→ 框内「%s」",tx.UTF8String,tf.text.UTF8String); }
                else LOG("[script] type：没有弹着的输入对话框"); }
            else if([op isEqualToString:@"clear"]) { UITextField *tf=g_text_alert.textFields.firstObject; if(tf) { while(tf.text.length) [tf deleteBackward]; LOG("[script] clear → 框内空"); } else LOG("[script] clear：没有弹着的输入对话框"); }
            else if([op isEqualToString:@"alert"]) { int ok=f.count>2 && [f[2] isEqualToString:@"ok"]; UIAlertController *al=g_text_alert; void (^fin)(int)=g_text_alert_finish;
                UIAlertAction *okAct=nil; for(UIAlertAction *x in al.actions) if(x.style==UIAlertActionStyleDefault) okAct=x;
                if(al && fin && ok && okAct && !okAct.enabled) LOG("[script] alert 确定：按钮是灰的（框内「%s」），没点",al.textFields.firstObject.text.UTF8String);   // 真人点不动灰按钮，脚本也不许绕过 | A real person cannot tap a greyed-out button, and the script must not bypass it either
                else if(al && fin) { LOG("[script] alert %s（框内「%s」）",ok?"确定":"取消",al.textFields.firstObject.text.UTF8String); [al dismissViewControllerAnimated:NO completion:^{ fin(ok); }]; }
                else LOG("[script] alert：没有弹着的输入对话框"); }
            else if([op isEqualToString:@"note"]) LOG("[script] note %s",[[f subarrayWithRange:NSMakeRange(2,f.count-2)] componentsJoinedByString:@" "].UTF8String);
            else [g_touch_view scriptPhase:op slot:[f[2] intValue] at:f.count>3?f[3]:nil];
        }
        // 最后几步是 tap_ui / tap_unit / walk_to / await 这类「挂着等、之后的拍里才落」的动作时，要等它们落完或超时再停计时器：
        // 原先排完最后一步就停，最后一步是 tap_ui 时那一下永远不点，也没有超时红（暂停页 Exit、TouchStashRight 都静悄悄没点）
        // When the last few steps are actions like tap_ui / tap_unit / walk_to / await that "hang waiting and only land in a later tick", wait until they land or time out before stopping the timer:
        // previously the timer stopped as soon as the last step was scheduled, so when the last step was tap_ui that tap never happened, with no timeout failure either (pause page Exit and TouchStashRight were both silently not tapped)
        if(next>=steps.count && !ui_on && !tu_on && !wk_on && !aw_on && !awr_on && !aui_on && !tu_up) { LOG("[script] 全部 %d 步执行完（%.2fs）",(int)steps.count,now); [tm invalidate]; }
    }];
    timer.tolerance=0; [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
    LOG("[script] 已排 %d 步（%s）",n,e);
}

void front_ios_lifecycle_install(void) {
    front_game_res(1);   // 主线程先读屏幕尺寸并缓存（客体线程初始化时才取值，UIScreen 只该在主线程用） | Read and cache the screen size on the main thread first (the value is only fetched when the guest thread initializes, and UIScreen should only be used on the main thread)
    UIApplication.sharedApplication.idleTimerDisabled = YES;   // 游戏期间不自动锁屏（锁屏即进后台：浸泡 / 长局会被打断） | No auto-lock during the game (locking the screen means going to background: soak / long runs would be interrupted)
    { uint64_t mtl_present_count(void); void front_ios_heat_start(uint64_t (*)(void), int); front_ios_heat_start(mtl_present_count, 1); }   // DM_HEAT 发热归因 | DM_HEAT heat attribution
    // 亮度：每 2 s 读一次（宿主每 300 帧上屏行带出）；测试开关 DM_BRIGHTNESS=0–1 时偏差 > 0.01 即重设——启动早期只设一次会被忽略 / 被自动亮度改回（M1 读回 0.35）
    // Brightness: read once every 2 s (carried by the host's every-300-frames present line); with test switch DM_BRIGHTNESS=0–1, reset whenever the deviation > 0.01 -- setting it only once early in launch gets ignored / reverted by auto-brightness (M1 read back 0.35)
    { const char *br = getenv("DM_BRIGHTNESS"); double want = br ? atof(br) : -1;
      [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer *tm) { (void)tm; double cur = UIScreen.mainScreen.brightness;
          if (want >= 0 && fabs(cur - want) > 0.01) { UIScreen.mainScreen.brightness = want; LOG("亮度 %.2f → 重设 %.2f（DM_BRIGHTNESS）", cur, want); }
          g_dm_brightness = UIScreen.mainScreen.brightness; }]; }
    // 温控档位：启动记一次，之后每次变化记一行；宿主每 300 帧上屏行带出当前档位（M1 冷却判据：开测前连续 60 s 为 0）
    // Thermal state: logged once at launch, then one line on every change; the host's every-300-frames present line carries the current state (M1 cooling criterion: 0 for 60 s in a row before the test starts)
    { g_dm_thermal = (int)NSProcessInfo.processInfo.thermalState; LOG("温控档位 %d（启动）", g_dm_thermal); LOG("持续执行档：%d（1 = 系统已按稳态功率限制本进程）", (int)[NSProcessInfo.processInfo hasPerformanceProfile:NSProcessPerformanceProfileSustained]);
      [NSNotificationCenter.defaultCenter addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n;
          int t = (int)NSProcessInfo.processInfo.thermalState; if (t != g_dm_thermal) { LOG("温控档位 %d → %d", g_dm_thermal, t); g_dm_thermal = t; } }]; }
    // 浸泡（DM_SOAK，diag.c 每 N 秒写一行 TSV）：真机另每 60 秒记一行温控档位（0 正常 / 1 微热 / 2 严重 / 3 危急），看发热降频与帧时间的关系
    // Soak (DM_SOAK, diag.c writes one TSV line every N seconds): on a real device additionally log one thermal state line every 60 seconds (0 nominal / 1 fair / 2 serious / 3 critical), to see how thermal throttling relates to frame time
    if (getenv("DM_SOAK")) {
        uint64_t t0 = now_ns();
        [NSTimer scheduledTimerWithTimeInterval:60 repeats:YES block:^(NSTimer *tm) { (void)tm;
            LOG("[浸泡] 第 %.0f 秒 温控档位 %ld", (now_ns() - t0) / 1e9, (long)NSProcessInfo.processInfo.thermalState); }];
    }
    // 屏幕级截图（真机验收）：DM_SCREEN_SHOT=秒:文件名 → 到时把整个窗口（含方向、黑边）按屏幕像素截成 PNG 写进 Documents，
    // 旁写 <文件名>.txt：窗口尺寸（点）、屏幕缩放、界面方向（UIInterfaceOrientation：3/4 = 横屏）。dm/ios/tools/screen_check.py 核
    // Screen-level screenshot (real-device acceptance): DM_SCREEN_SHOT=seconds:file name → at that time capture the whole window (including orientation and black bars) at screen pixels as a PNG written into Documents,
    // with <file name>.txt written alongside: window size (points), screen scale, interface orientation (UIInterfaceOrientation: 3/4 = landscape). Checked by dm/ios/tools/screen_check.py
    const char *touchShot=getenv("DM_TOUCH_SHOT"); double touchAt; char touchName[256];
    if(touchShot && sscanf(touchShot,"%lf:%255s",&touchAt,touchName)==2) {
        NSString *file=@(touchName);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(touchAt*1e9)),dispatch_get_main_queue(),^{
            NSString *docs=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
            [g_touch_view captureTouchLayer:[docs stringByAppendingPathComponent:file]];
        });
    }
    const char *ss = getenv("DM_SCREEN_SHOT"); char sn[256]; double st;
    if (ss && sscanf(ss, "%lf:%255s", &st, sn) == 2) {
        NSString *name = @(sn);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(st * 1e9)), dispatch_get_main_queue(), ^{ window_shot(name); });
    }    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter; NSOperationQueue *mq = NSOperationQueue.mainQueue;
    [nc addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil queue:mq usingBlock:^(NSNotification *n) { (void)n; thermal_apply(); }];
    if (NSProcessInfo.processInfo.thermalState >= NSProcessInfoThermalStateSerious) thermal_apply();   // 启动时已经热 | Already hot at launch

    AVAudioSession *as = AVAudioSession.sharedInstance; NSError *e = nil;
    // 类别 Playback：静音开关开着也出声。带 MixWithOthers：Playback 缺省是独占的，激活会话会打断别的 App 的声音——
    // #77：游戏在前台时 ChatGPT 实时语音 / 屏幕共享、后台音乐都被打断，切走就恢复，游戏音量调到 0 也一样（打断发生在激活会话那一下，与音量无关）
    // Category Playback: sound plays with the silent switch on. With MixWithOthers: Playback is non-mixable by default, and activating the session interrupts other apps' audio —
    // #77: with the game in the foreground, ChatGPT live voice / screen sharing and background music were interrupted and came back when switching away, even with the in-game volume at 0 (the interruption happens when the session is activated, regardless of volume)
    if (![as setCategory:AVAudioSessionCategoryPlayback withOptions:AVAudioSessionCategoryOptionMixWithOthers error:&e] || ![as setActive:YES error:&e]) LOG("前端：音频会话设置失败 %s", e.localizedDescription.UTF8String);
    LOG("前端：音频会话 类别 %s 选项 %#lx（MixWithOthers %s）、别的 App 正在出声 %d", as.category.UTF8String, (unsigned long)as.categoryOptions, (as.categoryOptions & AVAudioSessionCategoryOptionMixWithOthers) ? "开" : "关", as.isOtherAudioPlaying);
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:as queue:mq usingBlock:^(NSNotification *n) {
        AVAudioSessionInterruptionType t = [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue];
        if (t == AVAudioSessionInterruptionTypeBegan) { LOG("前端：音频被打断"); g_audio_intr = 1; audio_apply(); return; }
        // 打断结束：不管系统给没给「应当恢复」都解除这个原因（没给的情形如闹钟被划掉，回到游戏仍该出声）；真正启动声卡由 audio_apply 按是否在后台决定
        // Interruption ended: clear this reason whether or not the system gave "should resume" (cases without it, e.g. an alarm swiped away, should still produce sound back in the game); actually starting the audio device is decided by audio_apply according to whether we are in background
        g_audio_intr = 0; [AVAudioSession.sharedInstance setActive:YES error:nil]; audio_apply(); LOG("前端：打断结束%s", g_audio_bg ? "（仍在后台，回前台再出声）" : "，恢复音频");
    }];
    // 系统音量 / 输出路由变化记一行（「按音量键黑屏、闪退」，日志里看不出按键时刻）：带上显示节拍上一次回调距今多久，好和停节拍对时刻
    // Log one line on system volume / output route changes ("pressing the volume keys causes a black screen or crash", and the log does not show when keys were pressed): include how long since the display link's last callback, to line up with display-link stalls
    static DmVolumeWatch *vw; vw = [DmVolumeWatch new]; [as addObserver:vw forKeyPath:@"outputVolume" options:NSKeyValueObservingOptionOld | NSKeyValueObservingOptionNew context:NULL];
    [nc addObserverForName:AVAudioSessionRouteChangeNotification object:as queue:mq usingBlock:^(NSNotification *n) {
        double mtl_link_tick_age_ms(void);
        LOG("前端：音频路由变化 原因 %lu（显示节拍上次回调 %.0f ms 前）", (unsigned long)[n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue], mtl_link_tick_age_ms()); }];
    LOG("前端：生命周期 / 过热 / 音频会话已接（类别 Playback，与别的 App 混音）");
    // 测试开关 DM_TEST_AUDIO_INTERRUPT=起始秒,时长秒：按时向本进程投递音频会话打断开始 / 结束（带 ShouldResume），模拟来电 / 闹钟；
    // 结束后 2 s 打出声卡回调增量（真出声 DM_AUDIO=1 时应 > 0）
    // 测试开关 DM_TEST_BG=起始秒,时长秒[;起始秒,时长秒…]：应用内模拟「进后台 → 回前台」（可排几组）——先暂停显示节拍（模仿系统停回调），再走与真实场景事件同一个处理函数。
    // 验的是我们自己的全部逻辑（丢弃排队帧、图形挂起 / 恢复、客体时钟冻结、音频、动态比例停判、回来后出图恢复）；验不了系统侧（表面回收、系统是否续上回调）
    // Test switch DM_TEST_AUDIO_INTERRUPT=start seconds,duration seconds: deliver audio session interruption began / ended (with ShouldResume) to this process on schedule, simulating a phone call / alarm;
    // 2 s after the end, print the audio device callback delta (should be > 0 with real sound output DM_AUDIO=1)
    // Test switch DM_TEST_BG=start seconds,duration seconds[;start seconds,duration seconds…]: simulate "enter background → return to foreground" inside the app (several groups can be queued) -- first pause the display link (mimicking the system stopping callbacks), then go through the same handler as the real scene events.
    // This verifies all of our own logic (dropping queued frames, graphics suspend / resume, guest clock freeze, audio, dynamic scale judgment pause, output resuming after return); it cannot verify the system side (surface reclamation, whether the system resumes callbacks)
    const char *tb = getenv("DM_TEST_BG"); double tb_at, tb_dur;
    for (const char *tp = tb; tp && sscanf(tp, "%lf,%lf", &tb_at, &tb_dur) == 2; tp = strchr(tp, ';') ? strchr(tp, ';') + 1 : NULL) {
        void mtl_link_pause(int on);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(tb_at * 1e9)), dispatch_get_main_queue(), ^{
            // 节拍先停 0.15 秒再发场景事件：真机上系统先停回调、之后才到 sceneDidEnterBackground，这段空档里游戏会把交换链画满、卡在取下一张贴图上
            // （玩家日志：进后台时排队 3 帧、等存盘 2.5 秒 0 次提交）。同一拍里先停后发只会排 1 帧，撞不到
            // Stop the display link 0.15 seconds before sending the scene event: on a real device the system stops the callbacks first and sceneDidEnterBackground arrives only later; in that gap the game fills the swap chain and blocks on acquiring the next texture
            // (player log: 3 frames queued when entering background, 2.5 seconds waiting for the save with 0 commits). Stopping and sending within the same tick queues only 1 frame and never hits it
            LOG("前端：测试进后台（DM_TEST_BG）"); mtl_link_pause(1);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150000000), dispatch_get_main_queue(), ^{ front_ios_focus_event(0); front_ios_focus_event(1); }); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((tb_at + tb_dur) * 1e9)), dispatch_get_main_queue(), ^{
            LOG("前端：测试回前台（DM_TEST_BG）"); mtl_link_pause(0); front_ios_focus_event(2); front_ios_focus_event(3);
            uint64_t c0 = dm_audio_render_calls();
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2000000000), dispatch_get_main_queue(), ^{
                int dm_game_paused(void); LOG("前端：测试回前台后 2 s 声卡回调 +%llu（客体时钟 %.3f s；原版暂停 %d）", (unsigned long long)(dm_audio_render_calls() - c0), guest_ns() / 1e9, dm_game_paused()); }); });
    }
    // 测试开关 DM_TEST_RESIGN=秒：到点只发「失活」（sceneWillResignActive），不进后台——模仿上划停在多任务界面；配合测试脚本随后 kill -9 = 在多任务界面里划掉
    // Test switch DM_TEST_RESIGN=seconds: at that time send only "resign active" (sceneWillResignActive) without going to background -- mimics swiping up and stopping in the app switcher; combined with the test script's subsequent kill -9 = swiping it away in the app switcher
    { const char *tr = getenv("DM_TEST_RESIGN"); double tr_at = tr ? atof(tr) : 0;
      if (tr_at > 0) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(tr_at * 1e9)), dispatch_get_main_queue(), ^{ LOG("前端：测试失活（DM_TEST_RESIGN）"); front_ios_focus_event(0); }); }
    const char *ai = getenv("DM_TEST_AUDIO_INTERRUPT"); double ai_at, ai_dur;
    if (ai && sscanf(ai, "%lf,%lf", &ai_at, &ai_dur) == 2) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(ai_at * 1e9)), dispatch_get_main_queue(), ^{
            LOG("前端：测试打断开始（DM_TEST_AUDIO_INTERRUPT）声卡回调累计 %llu", (unsigned long long)dm_audio_render_calls());
            [nc postNotificationName:AVAudioSessionInterruptionNotification object:as userInfo:@{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeBegan)}];
        });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((ai_at + ai_dur) * 1e9)), dispatch_get_main_queue(), ^{
            uint64_t c0 = dm_audio_render_calls();
            LOG("前端：测试打断结束（DM_TEST_AUDIO_INTERRUPT）声卡回调累计 %llu", (unsigned long long)c0);
            [nc postNotificationName:AVAudioSessionInterruptionNotification object:as userInfo:@{AVAudioSessionInterruptionTypeKey: @(AVAudioSessionInterruptionTypeEnded),
                                                                                                AVAudioSessionInterruptionOptionKey: @(AVAudioSessionInterruptionOptionShouldResume)}];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2000000000), dispatch_get_main_queue(), ^{
                LOG("前端：测试打断恢复后 2 s 声卡回调 +%llu", (unsigned long long)(dm_audio_render_calls() - c0));
            });
        });
    }
    // 激活诊断（真机只收到 willEnterForeground、等不到 didBecomeActive）：主线程定时器在启动后 1 / 3 / 10 s 打状态。
    // 定时器不来 = 主线程被堵；来了但场景停在 foregroundInactive = 有系统界面盖着 / 未获激活
    // Activation diagnostics (the real device only receives willEnterForeground and never gets didBecomeActive): a main-thread timer prints the state at 1 / 3 / 10 s after launch.
    // Timer never fires = the main thread is blocked; fires but the scene stays at foregroundInactive = a system UI is covering it / not activated
    static const double probe_at[] = {1, 3, 10};
    for (int i = 0; i < 3; i++) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(probe_at[i] * 1e9)), dispatch_get_main_queue(), ^{
        UIApplication *app = UIApplication.sharedApplication; NSMutableString *sc = [NSMutableString string];
        for (UIScene *x in app.connectedScenes) {
            UIWindow *kw = nil; if ([x isKindOfClass:UIWindowScene.class]) kw = ((UIWindowScene *)x).keyWindow;
            [sc appendFormat:@" 场景 activationState=%ld keyWindow=%d", (long)x.activationState, kw != nil];
        }
        LOG("前端：激活诊断 t=%.0fs applicationState=%ld（0 激活 / 1 未激活 / 2 后台）受保护数据=%d%s", probe_at[i], (long)app.applicationState,
            app.isProtectedDataAvailable, sc.UTF8String);
    });
}

// ---------- 发热归因（DM_HEAT=路径[,间隔秒（缺省 10）]，路径相对 Documents）----------
// 每行：秒、温控档位（0 正常 / 1 微热 / 2 严重 / 3 危急）、进程 CPU%（所有线程用户 + 系统时间差分 ÷ 墙钟，100 = 一个核）、
//       实际出图帧率、每帧 GPU 毫秒均值 / p95 / 样本数（命令缓冲 gpuEnd − gpuStart，图形后端取数）、屏幕亮度（0–1）、屏幕最高刷新率
// ---------- Heat attribution (DM_HEAT=path[,interval in seconds (default 10)], path relative to Documents) ----------
// Each line: seconds, thermal state (0 nominal / 1 fair / 2 serious / 3 critical), process CPU% (delta of user + system time of all threads ÷ wall clock, 100 = one core),
//       actual output frame rate, per-frame GPU ms mean / p95 / sample count (command buffer gpuEnd − gpuStart, read from the graphics backend), screen brightness (0–1), screen maximum refresh rate
#include <sys/resource.h>
void nvn_gpu_recent(double *avg, double *p95, unsigned *n);
// 没设 DM_HEAT（玩家正常启动）：每 10 秒往日志里记一行同样的读数，另带电量 / 充电 / 低电量模式——玩家报「卡 / 烫 / 掉电」时日志里有当时的背景。
// 一次取样是几次系统调用加一次最多 1024 个数的排序，耗时记在行尾（取样 … ms）；切过后台的那一拍间隔不准，不记。
// DM_HEAT not set (normal player launch): log one line with the same readings every 10 seconds, plus battery level / charging / low power mode -- when a player reports "lag / hot / battery drain", the log has the background at that time.
// One sample is a few system calls plus one sort of at most 1024 numbers; its cost is logged at the end of the line (「取样 … ms」(sample … ms)); the interval of a tick that went through background is inaccurate and is not logged.
void front_ios_heat_start(uint64_t (*presents)(void), int has_gpu) {
    const char *e = getenv("DM_HEAT");
    if (!e) {
        if (!has_gpu) return;   // 资源下载界面那次调用不起（进游戏后还会再调一次，免得起两个） | The call from the asset download UI does not start it (it is called again after entering the game, to avoid starting two)
        UIDevice.currentDevice.batteryMonitoringEnabled = YES;
        __block uint64_t t0 = now_ns(), tl = t0, pl = presents(); __block double cl = 0;
        { struct rusage ru; getrusage(RUSAGE_SELF, &ru); cl = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6; }
        [NSTimer scheduledTimerWithTimeInterval:10 repeats:YES block:^(NSTimer *tm) { (void)tm;
            uint64_t t = now_ns(), p = presents(); struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, dt = (t - tl) / 1e9;
            double ga = -1, gp = -1; unsigned gn = 0; if (has_gpu) nvn_gpu_recent(&ga, &gp, &gn);
            if (dt < 15) {
                float bl = UIDevice.currentDevice.batteryLevel; UIDeviceBatteryState bs = UIDevice.currentDevice.batteryState; char gpu[64] = "GPU 无样本", bat[32] = "电量 未知";
                if (gn) snprintf(gpu, sizeof gpu, "GPU %.2f / p95 %.2f ms（%u 帧）", ga, gp, gn);
                if (bl >= 0) snprintf(bat, sizeof bat, "电量 %.0f%%", bl * 100);
                LOG("[诊断] 第 %.0f 秒 CPU %.0f%% 帧率 %.1f %s 温控 %ld 亮度 %.2f %s %s 低电量模式 %s 取样 %.2f ms", (t - t0) / 1e9, (cpu - cl) / dt * 100, (p - pl) / dt, gpu,
                    (long)NSProcessInfo.processInfo.thermalState, UIScreen.mainScreen.brightness, bat,
                    bs == UIDeviceBatteryStateCharging ? "充电中" : bs == UIDeviceBatteryStateFull ? "已充满" : bs == UIDeviceBatteryStateUnplugged ? "未充电" : "充电状态未知",
                    NSProcessInfo.processInfo.lowPowerModeEnabled ? "开" : "关", (now_ns() - t) / 1e6);
            }
            tl = t; pl = p; cl = cpu; }];
        return;
    }
    char rel[512]; snprintf(rel, sizeof rel, "%.*s", (int)strcspn(e, ","), e); const char *c = strchr(e, ','); int iv = c ? atoi(c + 1) : 10;
    NSString *doc = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)[0];
    FILE *f = fopen([doc stringByAppendingPathComponent:@(rel)].fileSystemRepresentation, "w"); if (!f) return;
    fprintf(f, "秒\t温控\tCPU%%\t出图帧率\tGPU均值ms\tGPU_p95ms\tGPU样本\t亮度\t最高刷新\n"); fflush(f);
    __block uint64_t t0 = now_ns(), tl = t0, pl = presents(); __block double cl = 0;
    { struct rusage ru; getrusage(RUSAGE_SELF, &ru); cl = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6; }
    [NSTimer scheduledTimerWithTimeInterval:iv repeats:YES block:^(NSTimer *tm) { (void)tm;
        uint64_t t = now_ns(), p = presents(); struct rusage ru; getrusage(RUSAGE_SELF, &ru);
        double cpu = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, dt = (t - tl) / 1e9;
        double ga = -1, gp = -1; unsigned gn = 0; if (has_gpu) nvn_gpu_recent(&ga, &gp, &gn);
        fprintf(f, "%.0f\t%ld\t%.1f\t%.1f\t%.2f\t%.2f\t%u\t%.2f\t%ld\n", (t - t0) / 1e9, (long)NSProcessInfo.processInfo.thermalState, (cpu - cl) / dt * 100,
                (p - pl) / dt, ga, gp, gn, UIScreen.mainScreen.brightness, (long)UIScreen.mainScreen.maximumFramesPerSecond);
        fflush(f); tl = t; pl = p; cl = cpu; }];
}

// ---------- 发热负对照：最小 Metal 空白画面（-mode blank）----------
// 同一块屏、同样 60 帧出图，只清屏成中灰 RGB(50,50,50)（约等于游戏画面平均亮度，OLED 功耗随像素亮度变），看屏幕与显示链本身的发热
// ---------- Heat negative control: minimal blank Metal picture (-mode blank) ----------
// Same screen, same 60 fps output, only clears to mid grey RGB(50,50,50) (roughly the average brightness of the game picture; OLED power varies with pixel brightness), to see the heat of the screen and the display chain themselves
static uint64_t g_blank_n;
uint64_t front_blank_presents(void) { return g_blank_n; }
@interface DmBlankView : UIView
@end
@implementation DmBlankView { id<MTLCommandQueue> _q; CADisplayLink *_dl; }
+ (Class)layerClass { return CAMetalLayer.class; }
- (instancetype)initWithFrame:(CGRect)fr {
    if ((self = [super initWithFrame:fr])) {
        CAMetalLayer *l = (CAMetalLayer *)self.layer; l.device = MTLCreateSystemDefaultDevice(); l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.contentsScale = UIScreen.mainScreen.nativeScale;
        _q = [l.device newCommandQueue];
        _dl = [CADisplayLink displayLinkWithTarget:self selector:@selector(tick)]; _dl.preferredFrameRateRange = CAFrameRateRangeMake(60, 60, 60);
        [_dl addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    return self;
}
- (void)layoutSubviews { [super layoutSubviews]; CAMetalLayer *l = (CAMetalLayer *)self.layer; l.drawableSize = CGSizeMake(self.bounds.size.width * l.contentsScale, self.bounds.size.height * l.contentsScale); }
- (void)tick {
    @autoreleasepool {
        id<CAMetalDrawable> dr = [(CAMetalLayer *)self.layer nextDrawable]; if (!dr) return;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dr.texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(50 / 255.0, 50 / 255.0, 50 / 255.0, 1);
        id<MTLCommandBuffer> cb = [_q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb presentDrawable:dr]; [cb commit]; g_blank_n++;
    }
}
@end
UIView *front_ios_blank_view(CGRect fr) { return [[DmBlankView alloc] initWithFrame:fr]; }
