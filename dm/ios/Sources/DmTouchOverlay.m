#import "DmTouchOverlay.h"
#include "DmTouchControlArt.h"
#include <math.h>
#include <TargetConditionals.h>
#include "dm_touch_native_art.h"
#include "hle.h"
#include "dm_input.h"
#include "../../host/dm_i18n.h"

static UIColor *bronze(CGFloat a) { return [UIColor colorWithRed:.66 green:.53 blue:.32 alpha:a]; }
// 外观按落指立即反馈，不等待游戏按键判定；取消瞄准的手指不再点亮槽框。
// Visual feedback starts on contact without waiting for game input; a cancelled aim finger no longer lights the frame.
static BOOL button_held(const dm_tc_state *s, int target) {
    uint64_t button=target>=0 && target<DM_TC_COUNT?s->layout.target[target].button:0;
    if(button && (s->buttons&button)==button) return YES;
    for(int i=0;i<DM_TC_MAX_TOUCH;i++)
        if(s->finger[i].active && !s->finger[i].aim_cancelled && s->finger[i].target==target) return YES;
    return NO;
}
static void caption(NSString *s, CGRect rect, CGFloat size, CGFloat alpha) {
    NSMutableParagraphStyle *p = [NSMutableParagraphStyle new]; p.alignment = NSTextAlignmentCenter;
    UIFont *f = [UIFont fontWithName:@"Palatino" size:size];
    [s drawInRect:rect withAttributes:@{NSFontAttributeName:f ?: [UIFont systemFontOfSize:size],
        NSForegroundColorAttributeName:[UIColor colorWithRed:.85 green:.79 blue:.65 alpha:alpha],NSParagraphStyleAttributeName:p}];
}
// 技能钮上画哪个图标：读数入口与输入层相同（dm_ta_current_set），只多一条「旧了照旧画」。
// 测试注入 DM_TEST_OVERLAY_STALE_MS=<毫秒>：绘制时把「现在」往后拨这么多，等于技能快照已经这么旧（造「游戏停拍」时覆盖层的样子，不影响输入判定）
// Which icon to draw on a skill button: same reading entry as the input layer (dm_ta_current_set), with just one extra rule: "if stale, draw as before".
// Test injection DM_TEST_OVERLAY_STALE_MS=<ms>: when drawing, push "now" forward by this much, as if the skill snapshot were already this old (reproduces how the overlay looks when game ticks stall; does not affect input decisions)
static dm_ta_glyph shown_glyph(const dm_tg_snapshot *n, uint64_t button, uint64_t now, int set) {
    static int64_t stale = -1; if (stale < 0) { const char *e = getenv("DM_TEST_OVERLAY_STALE_MS"); stale = e ? atoll(e) * 1000000ll : 0; }
    now += (uint64_t)stale;
    dm_ta_glyph g = dm_ta_current_set(n, button, now, set);
    // 快照只是旧了（超过 250 毫秒没更新：游戏停拍、切后台那一瞬、长卡顿）→ 照它被取到那一刻的内容画，不退回 F 标签 / 十字——
    // 绑定没变，只是暂时没人刷新；换场景时桥会把快照清空（dm_touch_game_bridge.c），那时取到时刻为 0，仍画占位。输入层的同一读数（front_ios.m）不走这里，照旧只认新鲜的
    // The snapshot is merely stale (not updated for over 250 ms: game ticks stalled, the instant of going to background, a long hitch) → draw the content it had at the moment it was captured, do not fall back to the F label / cross --
    // the binding has not changed, nobody is refreshing it for the moment; on scene change the bridge clears the snapshot (dm_touch_game_bridge.c), then the capture time is 0 and the placeholder is still drawn. The same reading in the input layer (front_ios.m) does not go through here and still accepts only fresh ones
    if (g.status == DM_TA_UNKNOWN && n && n->captured_ns && now > n->captured_ns && now - n->captured_ns > DM_TG_MAX_AGE_NS) g = dm_ta_current_set(n, button, n->captured_ns, set);
    return g;
}
@implementation DmTouchOverlay {
    UIImage *_circle, *_socket, *_map, *_menu, *_nativeAtlases[DM_TA_ATLASES], *_silAtlases[DM_TA_ATLASES];   // _sil：同帧位的银色剪影图集（gen_hud_v4.py） | _sil: silver silhouette atlas at the same frame positions (gen_hud_v4.py)
    NSMutableDictionary<NSString *,UIImage *> *_glyphFrames, *_buttonFrames;
    dm_ta_glyph _nativeVisualGlyphs[5];
}
- (instancetype)initWithFrame:(CGRect)f {
    if (!(self = [super initWithFrame:f])) return nil;
    self.backgroundColor = UIColor.clearColor; self.opaque = NO; self.userInteractionEnabled = NO;
    _circle=[UIImage imageNamed:@"dm_native_circle"];
    // 通用按钮材质为新生成的美术，技能图案仍来自玩家自己的游戏文件。
    // The generic socket texture is newly generated artwork; skill glyphs still come from the player's own game files.
    _socket=[UIImage imageNamed:@"dm_mobile_socket"];
    _map=[UIImage imageNamed:@"dm_native_map"]; _menu=[UIImage imageNamed:@"dm_native_menu"];
    _glyphFrames=[NSMutableDictionary dictionary]; _buttonFrames=[NSMutableDictionary dictionary];
    for(int i=0;i<DM_TA_ATLASES;i++) _nativeAtlases[i]=[UIImage imageNamed:[NSString stringWithUTF8String:dm_ta_atlases[i].asset]];
    for(int i=0;i<DM_TA_ATLASES;i++) _silAtlases[i]=[UIImage imageWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:
        [@"hud_v4_ui/sil_" stringByAppendingString:[NSString stringWithUTF8String:dm_ta_atlases[i].asset]]]];
    return self;
}
- (void)setNativeMapping:(dm_tg_snapshot)n {
    _nativeMapping=n;
    uint64_t buttons[5]={DM_BTN_A,DM_BTN_B,DM_BTN_X,DM_BTN_Y,DM_BTN_R};
    uint64_t hostNow=now_ns(); BOOL dirty=NO;
    for(int i=0;i<5;i++) {
        dm_ta_glyph next=shown_glyph(&n,buttons[i],hostNow,self.controls?self.controls->skill_set:0),previous=_nativeVisualGlyphs[i];
        if(next.status!=previous.status || next.raw_skill_id!=previous.raw_skill_id || next.atlas!=previous.atlas ||
           next.normal_frame!=previous.normal_frame || next.slot!=previous.slot || next.effective_slot!=previous.effective_slot)dirty=YES;
        _nativeVisualGlyphs[i]=next;
    }
    // 复用现有front CADisplayLink赋快照：只在图或新鲜性改变时重绘，不另开timer。
    // Reuse the existing front CADisplayLink to assign the snapshot: redraw only when the image or freshness changes, no extra timer.
    if(dirty)[self setNeedsDisplay];
}
- (UIImage *)frameImage:(UIImage *)atlas key:(NSString *)key rect:(CGRect)rect {
    if(!atlas) return nil;
    UIImage *image=_glyphFrames[key]; if(image) return image;
    CGImageRef crop=CGImageCreateWithImageInRect(atlas.CGImage,rect); if(!crop) return nil;
    image=[UIImage imageWithCGImage:crop]; CGImageRelease(crop); _glyphFrames[key]=image; return image;
}
- (UIImage *)skillImage:(dm_ta_glyph)g held:(BOOL)held {
    if(g.status!=DM_TA_GLYPH)return nil;
    int frame=held?g.pressed_frame:g.normal_frame;dm_ta_frame f;
    if(!dm_ta_frame_at(g.atlas,frame,&f))return nil;
    UIImage *sil=_silAtlases[g.atlas],*atlas=sil?:_nativeAtlases[g.atlas];
    CGFloat trim=sil?0:round(f.width*.08);
    UIImage *glyph=[self frameImage:atlas key:[NSString stringWithFormat:@"socket:%d:%d",g.atlas,frame] rect:CGRectInset(CGRectMake(f.x,f.y,f.width,f.height),trim,trim)];
    if(!sil || !glyph) return glyph;
    // 保留原版技能的明暗纹理，统一为银灰，不再做职业染色。
    // Preserve the original skill shading in silver gray, without class tinting.
    NSString *key=[NSString stringWithFormat:@"silver:%d:%d",g.atlas,frame];UIImage *image=_glyphFrames[key];
    if(!image) {
        UIGraphicsImageRendererFormat *format=[UIGraphicsImageRendererFormat preferredFormat];format.opaque=NO;format.scale=glyph.scale;
        image=[[[UIGraphicsImageRenderer alloc] initWithSize:glyph.size format:format] imageWithActions:^(UIGraphicsImageRendererContext *ctx){
            CGRect r=(CGRect){CGPointZero,glyph.size};[glyph drawInRect:r];dm_skill_silver(ctx.CGContext,r);[glyph drawInRect:r blendMode:kCGBlendModeDestinationIn alpha:1];
        }];_glyphFrames[key]=image;
    }
    return image;
}
// 缓存控件的静态底图；移动时只换摇杆头坐标，不在每帧创建渐变。
// Cache static control art; movement only changes the thumb position, without creating gradients every frame.
- (UIImage *)controlArt:(NSString *)key size:(CGSize)size draw:(void (^)(CGContextRef))draw {
    key=[NSString stringWithFormat:@"%@:%.3f:%.1f",key,size.width,self.contentScaleFactor];
    UIImage *image=_buttonFrames[key];
    if(!image) {
        UIGraphicsImageRendererFormat *format=[UIGraphicsImageRendererFormat preferredFormat];format.opaque=NO;format.scale=self.contentScaleFactor;
        image=[[[UIGraphicsImageRenderer alloc] initWithSize:size format:format] imageWithActions:^(UIGraphicsImageRendererContext *ctx){draw(ctx.CGContext);}];
        // 限制旋转 / 窗口缩放产生的缓存数量。
        // Bound the cache count produced by rotation / window resizing.
        if(_buttonFrames.count>=64) [_buttonFrames removeAllObjects];_buttonFrames[key]=image;
    }
    return image;
}
- (void)drawButton:(CGRect)rect held:(BOOL)held empty:(BOOL)empty primary:(BOOL)primary {
    NSString *key=[NSString stringWithFormat:@"skill:%d:%d:%d",held,empty,primary];
    UIImage *image=[self controlArt:key size:rect.size draw:^(CGContextRef c){
        CGRect r=(CGRect){CGPointZero,rect.size};
        if(self->_socket && !empty) {
            [self->_socket drawInRect:r];dm_socket_neutralize(c,r);
            if(held) dm_button_highlight(c,r);
        } else dm_button_draw(c,r,held,empty,primary);
    }];
    [image drawInRect:rect];
}
- (void)drawJoystickPart:(CGRect)rect base:(BOOL)base active:(BOOL)active {
    NSString *key=[NSString stringWithFormat:@"stick:%d:%d",base,active];
    UIImage *image=[self controlArt:key size:rect.size draw:^(CGContextRef c){
        if(base) dm_joystick_base_draw(c,(CGRect){CGPointZero,rect.size},active);
        else dm_joystick_thumb_draw(c,(CGRect){CGPointZero,rect.size},active);
    }];
    [image drawInRect:rect];
}
- (void)setNavigationMode:(BOOL)n { _navigationMode = n; [self setNeedsDisplay]; }
// ---- 底部 UI v4：UIKit 部分（经验条、「Lv · %」、角标、难度数字与 1–8 选择条），素材与几何由 tools/gen_hud_v4.py 生成（App 内 hud_v4_ui/）----
// 几何单位 = 2868×1320 效果图像素：点 = (像素 − 1434) × 屏高/1320 + 屏宽/2（x），像素 × 屏高/1320（y）——与原生 HUD 同样按屏高缩放、以屏幕中线居中
// ---- Bottom UI v4: UIKit part (experience bar, "Lv · %", badges, difficulty number and the 1–8 selector strip); assets and geometry are generated by tools/gen_hud_v4.py (hud_v4_ui/ inside the App) ----
// Geometry unit = pixels of the 2868×1320 mockup: point = (pixel − 1434) × screen height/1320 + screen width/2 (x), pixel × screen height/1320 (y) -- scaled by screen height and centered on the screen's center line, same as the native HUD
static NSDictionary *v4ui(void) {
    static NSDictionary *ui; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSString *p=[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"hud_v4_ui/ui.json"];
        NSData *d=[NSData dataWithContentsOfFile:p]; ui=d?[NSJSONSerialization JSONObjectWithData:d options:0 error:nil]:nil;
        if(!ui) NSLog(@"【红】底部 UI v4：App 内缺 hud_v4_ui/ui.json"); });
    return ui;
}
static UIImage *v4img(NSString *name) {
    static NSMutableDictionary *cache; if(!cache) cache=[NSMutableDictionary dictionary];
    UIImage *im=cache[name]; if(im) return im;
    im=[UIImage imageWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:[@"hud_v4_ui" stringByAppendingPathComponent:name]]];
    if(im) cache[name]=im; return im;
}
// 逐字贴预渲染字形（字效与效果图同源）；align：0 居中、1 右对齐；(x, baseline) 为效果图像素
// Blit pre-rendered glyphs character by character (text effect from the same source as the mockup); align: 0 centered, 1 right-aligned; (x, baseline) are in mockup pixels
static void v4text(NSString *str, NSDictionary *glyphs, CGFloat x, CGFloat baseline, int align, CGFloat W, CGFloat u) {
    CGFloat adv=0; for(NSUInteger i=0;i<str.length;i++) adv+=[glyphs[[str substringWithRange:NSMakeRange(i,1)]][@"adv"] doubleValue];
    CGFloat pen=align==1?x-adv:x-adv/2;
    for(NSUInteger i=0;i<str.length;i++) {
        NSDictionary *gm=glyphs[[str substringWithRange:NSMakeRange(i,1)]]; if(!gm) continue;
        UIImage *im=[gm[@"file"] isKindOfClass:NSString.class]?v4img(gm[@"file"]):nil;
        if(im) [im drawInRect:CGRectMake((pen+[gm[@"dx"] doubleValue]-1434)*u+W/2,(baseline+[gm[@"dy"] doubleValue])*u,im.size.width*u,im.size.height*u)];
        pen+=[gm[@"adv"] doubleValue];
    }
}
- (void)drawV4:(CGContextRef)c {
    NSDictionary *ui=v4ui(); const dm_hud_info h=self.hud; const dm_tc_state *s=self.controls;
    if(!ui || !dm_hud_bar_visible(&h,self.navigationMode && !self.dialogOnly,self.hudBlocked)) return;   // 只在局内常态显示：面板 / 读取画面 / 过场（导航态）与死亡时让开，不挡游戏自己的画面（dm_hud.h） | Shown only in the normal in-run state: yields during panels / loading screen / cutscenes (navigation mode) and on death, so it does not cover the game's own picture (dm_hud.h)
    CGFloat W=self.bounds.size.width, u=self.bounds.size.height/1320.0;
    CGFloat (^X)(CGFloat)=^(CGFloat px){ return (px-1434)*u+W/2; };
    // 底栏静态美术：效果图原生分辨率直接贴（不经游戏 1568×720 渲染再放大）；腰带格内挖空透出游戏画的药水、球心挖空透出球液
    // Bottom bar static art: blit the mockup directly at native resolution (not rendered by the game at 1568×720 and then upscaled); the belt cells are cut out to show the potions drawn by the game, and the orb centers are cut out to show the orb liquid
    UIImage *full=v4img(@"base_full.png"); NSDictionary *bo=ui[@"base"];
    if(full) [full drawInRect:CGRectMake(X([bo[@"x"] doubleValue]),[bo[@"y"] doubleValue]*u,full.size.width*u,full.size.height*u)];
    // 第 5 格（现为六格布局；原第 6 格）= 原版十字按钮整格替换（含边框）；按住第 1 帧。两态：有未分配点数时红色 + 外圈金光（原版「有点可加」提示），否则默认色（暗底金十字）
    // Cell 5 (now a six-cell layout; formerly cell 6) = the original game's cross button replacing the whole cell (border included); frame 1 while held. Two states: with unspent points it is red + an outer gold glow (the original game's "points available" hint), otherwise the default color (gold cross on dark base)
    { NSArray *s6=ui[@"slots"][5]; BOOL held=s && (s->buttons & DM_BTN_PLUS), alert=h.points>0 || h.quest_unread;
      UIImage *btn=v4img(alert?(held?@"lvl_1.png":@"lvl_0.png"):(held?@"lvl_d1.png":@"lvl_d0.png")); CGRect r=CGRectMake(X([s6[0] doubleValue]),[s6[1] doubleValue]*u,([s6[2] doubleValue]-[s6[0] doubleValue])*u,([s6[3] doubleValue]-[s6[1] doubleValue])*u);
      if(h.points>0) { CGContextSaveGState(c); CGContextSetShadowWithColor(c,CGSizeZero,10*u,[UIColor colorWithRed:1 green:.82 blue:.35 alpha:.95].CGColor); [btn drawInRect:r]; CGContextRestoreGState(c); }
      if(btn) [btn drawInRect:r]; }
    // 经验条：底槽 + 描边 + 填充（本级‰）+ 20 等分刻度，同效果图
    // Experience bar: base groove + outline + fill (‰ of the current level) + 20 equal tick marks, same as the mockup
    NSDictionary *e=ui[@"exp"]; CGFloat x0=[e[@"x0"] doubleValue], x1=[e[@"x1"] doubleValue], y=[e[@"y"] doubleValue], eh=[e[@"h"] doubleValue];
    UIColor *(^rgb)(NSArray *)=^(NSArray *a){ return [UIColor colorWithRed:[a[0] doubleValue]/255 green:[a[1] doubleValue]/255 blue:[a[2] doubleValue]/255 alpha:1]; };
    CGContextSetFillColorWithColor(c,rgb(e[@"track"]).CGColor); CGContextFillRect(c,CGRectMake(X(x0),y*u,(x1-x0)*u,eh*u));
    CGContextSetStrokeColorWithColor(c,rgb(e[@"edge"]).CGColor); CGContextSetLineWidth(c,u); CGContextStrokeRect(c,CGRectMake(X(x0)+u/2,y*u+u/2,(x1-x0-1)*u,(eh-1)*u));
    CGContextSetFillColorWithColor(c,rgb(e[@"fill"]).CGColor); CGContextFillRect(c,CGRectMake(X(x0+1),(y+1)*u,(x1-x0-2)*u*h.exp_permille/1000.0,(eh-2)*u));
    int ticks=[e[@"ticks"] intValue]; CGContextSetFillColorWithColor(c,rgb(e[@"track"]).CGColor);
    for(int t=1;t<ticks;t++) CGContextFillRect(c,CGRectMake(X(x0+round((x1-x0)*t/(double)ticks)),(y+1)*u,u,(eh-2)*u));
    NSDictionary *vt=ui[@"vital"];   // 生命 / 法力：球顶上方居中，效果图字形原生分辨率（原生数值文字已挪出屏幕） | Life / mana: centered above the top of the orb, mockup glyphs at native resolution (the native numeric text has been moved off screen)
    v4text([NSString stringWithFormat:@(dm_ui("dmHudLife")),h.hp,h.hp_max],vt[@"glyphs"],[vt[@"left_x"] doubleValue],[vt[@"baseline"] doubleValue],0,W,u);
    v4text([NSString stringWithFormat:@(dm_ui("dmHudMana")),h.mp,h.mp_max],vt[@"glyphs"],[vt[@"right_x"] doubleValue],[vt[@"baseline"] doubleValue],0,W,u);
    NSDictionary *lv=ui[@"lv"];
    v4text([NSString stringWithFormat:@"Lv %d · %d%%",h.level,h.exp_permille/10],lv[@"glyphs"],[lv[@"x"] doubleValue],[lv[@"baseline"] doubleValue],0,W,u);
    // 角标：腰带四列件数（第 1–4 格），右下内角右对齐（回城不要书 / 卷轴、不显示次数，第 5 格是十字）
    // Badges: item counts of the four belt columns (cells 1–4), right-aligned at the inner bottom-right corner (town portal needs no tome / scroll and shows no count; cell 5 is the cross)
    NSArray *slots=ui[@"slots"], *in=ui[@"inner"]; NSDictionary *bd=ui[@"badge"];
    int counts[5]={0,h.belt[0],h.belt[1],h.belt[2],h.belt[3]};
    for(int i=1;i<=4;i++) { if(!counts[i]) continue;
        v4text([NSString stringWithFormat:@"%d",counts[i]],bd[@"glyphs"],[slots[i][0] doubleValue]+[in[2] doubleValue]+[bd[@"dx"] doubleValue],[slots[i][1] doubleValue]+[in[3] doubleValue]+[bd[@"dy"] doubleValue],1,W,u); }
    // 难度：第 0 格当前档数字；选择条展开时画 8 格（当前档不透明、其余略淡）
    // Difficulty: cell 0 shows the current level number; when the selector strip is expanded, draw 8 cells (current level opaque, the rest slightly faded)
    void (^digit)(int,CGFloat,CGFloat,CGFloat)=^(int n,CGFloat cx,CGFloat cy,CGFloat alpha){ UIImage *d=v4img([NSString stringWithFormat:@"diff_%d.png",n]);
        if(d) [d drawInRect:CGRectMake(X(cx)-d.size.width*u/2,cy*u-d.size.height*u/2,d.size.width*u,d.size.height*u) blendMode:kCGBlendModeNormal alpha:alpha]; };
    CGFloat icx=([in[0] doubleValue]+[in[2] doubleValue]+1)/2, icy=([in[1] doubleValue]+[in[3] doubleValue]+1)/2;
    if(h.players>=1 && h.players<=8) digit(h.players,[slots[0][0] doubleValue]+icx,[slots[0][1] doubleValue]+icy,1);
    if(s && s->diff_open) {
        NSDictionary *pk=ui[@"pick"]; UIImage *tile=v4img(@"slot.png");
        for(int i=0;i<8;i++) {
            CGFloat cx=[pk[@"center_x"] doubleValue]+(i-3.5)*[pk[@"pitch"] doubleValue], cy=[pk[@"y"] doubleValue];
            CGFloat tw=tile.size.width, th=tile.size.height, a=(i+1==h.players)?1:.82;
            if(tile) [tile drawInRect:CGRectMake(X(cx-tw/2),(cy-th/2)*u,tw*u,th*u) blendMode:kCGBlendModeNormal alpha:a];
            digit(i+1,cx,cy,a);
        }
    }
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    const dm_tc_state *s = self.controls; if (!s) return;
    const dm_tc_layout *l = &s->layout;
    CGFloat k = l->joy_radius / 60;
    if (k <= 0) return;
    CGContextRef c = UIGraphicsGetCurrentContext();
    uint64_t hostNow=now_ns(); dm_tg_snapshot native=self.nativeMapping;
    // 圆钮上可能写出来的字一律走中英表（dm/i18n/ui_text.tsv），跟游戏语言：技能钮画图标 / 键位图、不写字；腰带 / 十字由底栏画（下面整个跳过），这几格不会写字
    // Any text that could be written on round buttons always goes through the bilingual table (dm/i18n/ui_text.tsv) and follows the game language: skill buttons draw icons / key glyphs and no text; the belt / cross are drawn by the bottom bar (skipped entirely below), so these cells never get text
    NSString *labels[DM_TC_COUNT] = {@"",@"",@"",@"",@"",@"",@"",@"",@"",@"",@"",@(dm_ui("dmTcMap"))};
    if (self.navigationMode) { labels[0]=@(dm_ui("dmTcNavConfirm")); labels[1]=@(dm_ui("dmTcNavBack")); labels[2]=@(dm_ui("dmTcNavAction")); labels[3]=@(dm_ui("dmTcNavBind")); labels[4]=@(dm_ui("dmTcNavNext")); }   // 底栏十字位置的「关闭」已去：关闭入口只留外框右上角那一个（dm_tc_nav_mask） | The 「关闭」(Close) at the bottom bar cross position has been removed: the only close entry left is the one at the top-right corner of the outer frame (dm_tc_nav_mask)
    for (int i=0;i<DM_TC_COUNT;i++) {
        if (!(s->enabled_targets & (1u<<i))) continue;
        if(i>DM_TC_PORTAL && i<=DM_TC_MENU) continue; // 底部 UI v4：腰带 / 功能入口（十字）由底栏画（drawV4 / 原生 HUD），这里只保命中区，面板态也一样—— | Bottom UI v4: the belt / function entry (cross) is drawn by the bottom bar (drawV4 / native HUD); only the hit area is kept here, in panel state too —
                                                      // 原先只在游戏态跳过：只开着 NPC 对话菜单时（导航态 3，十字可点）这一格被画成圆底 + 写死的中文「功能」，英文界面也写中文。回城（技能弧上方圆钮）照技能钮画
                                                      // Previously skipped only in the gameplay state: with only the NPC dialog menu open (navigation state 3, cross tappable) this cell was drawn as a round base plus the hardcoded Chinese "功能" (Functions), even in the English UI. Town Portal (the round button above the skill arc) is drawn like a skill button
        const dm_tc_target *t = &l->target[i]; CGRect b = CGRectMake(t->x-t->r,t->y-t->r,t->r*2,t->r*2);
        BOOL held=button_held(s,i);
        CGFloat opacity = held ? .6 : .28;
        if(!self.navigationMode && i==DM_TC_MAP && s->map_cancel_active) {
            CGContextSetFillColorWithColor(c,[UIColor colorWithRed:.55 green:.05 blue:.04 alpha:.36].CGColor); CGContextFillRect(c,b); labels[i]=@(dm_ui("dmTcCancel"));
        }
        BOOL skillKey=!self.navigationMode && (i<=DM_TC_SKILL4 || i==DM_TC_PORTAL);   // 回城钮（圆形、和技能图标一样） | Town portal button (round, same as the skill icons)
        if (skillKey) {
            b=dm_button_rect(t->x,t->y,t->r);
        } else if (_circle) [_circle drawInRect:b blendMode:kCGBlendModeNormal alpha:opacity];
        else {
            CGContextSetFillColorWithColor(c,[UIColor colorWithWhite:.02 alpha:.16].CGColor); CGContextFillEllipseInRect(c,b);
            CGContextSetStrokeColorWithColor(c,bronze(.28).CGColor); CGContextSetLineWidth(c,k); CGContextStrokeEllipseInRect(c,b);
        }
        UIImage *icon=nil; BOOL showCaption=YES; int ssKind=0, ssCharges=0;
        if(!self.navigationMode && i<=DM_TC_SKILL4) {
            dm_ta_glyph g=shown_glyph(&native,t->button,hostNow,s->skill_set);
            if(g.status!=DM_TA_EMPTY && g.status!=DM_TA_UNKNOWN) { int dm_swapskill_slot(int,int*); ssKind=dm_swapskill_slot(g.slot,&ssCharges); }
            icon=[self skillImage:g held:held];
            showCaption=g.status!=DM_TA_EMPTY && !icon;
        } else if(!self.navigationMode && i==DM_TC_PORTAL) {
            icon=[self skillImage:dm_ta_skill(359) held:held]; // 原版回城技能图（技能描述表 Town Portal：通用技能页第 18 格，219 / 220 / 411 同一张），不显示次数 | the original game's town portal skill image (skill description table Town Portal: cell 18 of the generic skill page, 219 / 220 / 411 share one image), count not shown
        } else if(!self.navigationMode && (i==DM_TC_MENU || i==DM_TC_MAP)) {
            icon=[self frameImage:i==DM_TC_MAP?_map:_menu key:[NSString stringWithFormat:@"fixed:%d:%d",i,held] rect:CGRectMake(held?36:0,0,36,36)];
        }
        if(!self.navigationMode && i==DM_TC_MAP && s->map_cancel_active) {icon=nil;showCaption=YES;}
        BOOL emptySkill=skillKey && !icon && (!showCaption || i==DM_TC_MAIN);
        if(skillKey) [self drawButton:b held:held empty:emptySkill primary:i==DM_TC_MAIN];
        if(emptySkill) continue;   // 空槽保留绑定入口，细边框和刻印由背景绘制 | Empty slots keep their binding entry; the background draws their thin rim and engraving
        if(skillKey) {   // 技能美术居中；无图的未知槽保留原标签回退 | Center skill artwork; unknown slots with no image keep the existing label fallback
            BOOL bare=icon!=nil;   // 有图标的技能钮不写字（最大的钮是「主技能 / 交互」、可绑任意技能，钮上只画所绑技能的图标，不写「攻击」）：图标放大并居中。取不到图标时主钮仍落回「攻击」字样 | skill buttons that have an icon carry no text (the largest button is "main skill / interact" and can be bound to any skill, so the button draws only the bound skill's icon and does not say 「攻击」(Attack)): the icon is enlarged and centered. When no icon can be obtained the main button still falls back to the 「攻击」(Attack) text | skill buttons that have an icon carry no text (the largest button is "main skill / interact" and can be bound to any skill, so it draws only the bound skill's icon and no Attack label): the icon is enlarged and centered. When no icon can be obtained the main button still falls back to the Attack label
            BOOL spent=(ssKind&2) && ssCharges<=0;   // 充能用完：钮变灰（灰底上按亮度贴图标 = 去色），按下也不换武器（dm_actions.c） | charges used up: the button turns gray (the icon applied by luminance on a gray base = desaturated), and pressing does not swap weapons either (dm_actions.c)
            if(icon) { CGFloat size=b.size.width*(bare?.76:.68); CGSize in_=icon.size; CGFloat fit=size/MAX(in_.width,in_.height);
                CGRect ir=CGRectMake(t->x-in_.width*fit/2,t->y-(bare?0:t->r*.12)-in_.height*fit/2,in_.width*fit,in_.height*fit);
                CGContextSaveGState(c); CGContextAddEllipseInRect(c,CGRectInset(b,b.size.width*.09,b.size.height*.09)); CGContextClip(c);
                if(spent) { CGContextSetFillColorWithColor(c,[UIColor colorWithWhite:.30 alpha:1].CGColor); CGContextFillEllipseInRect(c,CGRectInset(b,b.size.width*.08,b.size.height*.08));
                    [icon drawInRect:ir blendMode:kCGBlendModeLuminosity alpha:.55];
                    CGContextSetFillColorWithColor(c,[UIColor colorWithWhite:0 alpha:.35].CGColor); CGContextFillEllipseInRect(c,CGRectInset(b,b.size.width*.06,b.size.height*.06)); }
                else { CGContextSetShadowWithColor(c,CGSizeZero,held?t->r*.10:t->r*.055,[UIColor colorWithWhite:.65 alpha:.32].CGColor); [icon drawInRect:ir blendMode:kCGBlendModeNormal alpha:held?1:.94]; } CGContextRestoreGState(c); }   // 图标保持清晰，只有空槽框淡出 | Keep icons clear; only empty slot frames fade
            // 副手组技能角标：右上「Ⅱ」= 技能在另一组武器上，按下先换武器；右下剩余次数 = 充能技能，字用腰带药水件数那套（ui.json badge）
            // Secondary weapon set skill badges: top right "Ⅱ" = the skill is on the other weapon set, pressing swaps weapons first; bottom right remaining count = charged skill, the text uses the same style as the belt potion count (ui.json badge)
            if(ssKind&1) { CGFloat br=t->r*.30, bx=t->x+t->r*.74, by=t->y-t->r*.74; CGRect bb=CGRectMake(bx-br,by-br,br*2,br*2), lw2=CGRectInset(bb,MAX(1,br*.08),MAX(1,br*.08));
                CGContextSetFillColorWithColor(c,[UIColor colorWithRed:.11 green:.094 blue:.078 alpha:.95].CGColor); CGContextFillEllipseInRect(c,bb);
                CGContextSetLineWidth(c,MAX(1,br*.14)); CGContextSetStrokeColorWithColor(c,[UIColor colorWithRed:.63 green:.50 blue:.27 alpha:1].CGColor); CGContextStrokeEllipseInRect(c,lw2);
                CGFloat ts=br*1.15; caption(@"Ⅱ",CGRectMake(bx-br,by-ts*.62,br*2,ts*1.4),ts,1); }
            if(ssKind&2) { NSDictionary *ui=v4ui(); CGFloat W=self.bounds.size.width, u=self.bounds.size.height/1320.0; NSDictionary *bd=ui[@"badge"];
                if(bd && u>0) v4text([NSString stringWithFormat:@"%d",ssCharges<0?0:ssCharges],bd[@"glyphs"],(t->x+t->r*.66-W/2)/u+1434,(t->y+t->r*.42)/u,1,W,u); }
            if(bare) continue;
            NSString *ln=i==DM_TC_MAIN?@"lbl_main.png":[NSString stringWithFormat:@"lbl_f%d.png",i-DM_TC_SKILL1+1+(s->skill_set?4:0)];
            UIImage *lbl=[UIImage imageWithContentsOfFile:[NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:[@"hud_v4_ui" stringByAppendingPathComponent:ln]]];
            if(lbl) { CGFloat lh=t->r*(i==DM_TC_MAIN?.30:.36), lw_=lbl.size.width*lh/lbl.size.height;
                [lbl drawInRect:CGRectMake(t->x-lw_/2,t->y+t->r*.50-lh/2,lw_,lh)]; }
            continue;
        }
        if(icon) {
            CGFloat size=b.size.width*(i==DM_TC_MAIN?.55:.62);
            CGSize intrinsic=icon.size;CGFloat fit=size/MAX(intrinsic.width,intrinsic.height);
            CGRect glyph=CGRectMake(t->x-intrinsic.width*fit/2,t->y-intrinsic.height*fit/2,intrinsic.width*fit,intrinsic.height*fit);
            [icon drawInRect:glyph blendMode:kCGBlendModeNormal alpha:held?1:.82];showCaption=NO;
        }
        CGFloat textSize = (i==DM_TC_MAIN ? 13 : 10)*k;
        if(showCaption) caption(labels[i],CGRectMake(b.origin.x,t->y-textSize*.6,b.size.width,textSize*1.6),textSize,held ? 1 : .84);
        if (i>=DM_TC_BELT1 && i<=DM_TC_BELT4) caption([NSString stringWithFormat:@"%d",i-DM_TC_BELT1+1],CGRectMake(t->x-10*k,t->y+8*k,20*k,12*k),8*k,.65);
    }
    if (!self.navigationMode && l->swap.r > 0) {   // 技能组切换键：显示当前第几套 | skill set switch button: shows which set is current
        CGRect b=dm_button_rect(l->swap.x,l->swap.y,l->swap.r);
        BOOL sheld=button_held(s,DM_TC_SWAP);
        [self drawButton:b held:sheld empty:NO primary:NO];
        CGFloat ts=12*k; caption(s->skill_set ? @"Ⅱ" : @"Ⅰ",CGRectMake(b.origin.x,l->swap.y-ts*.65,b.size.width,ts*1.6),ts,sheld?1:.86);
    }
    [self drawV4:c];   // 底栏：它自己按面板栈 / 死亡判显隐（dm_hud_bar_visible）。原先放在摇杆之后，被下面「面板开着就不画摇杆」的提前返回一起跳过—— | Bottom bar: it decides its own visibility from the panel stack / death (dm_hud_bar_visible). It used to come after the joystick and was skipped along with the early return below for "don't draw the joystick while a panel is open" —
                       // 只开着 NPC 对话菜单 / 说话框时（设计上底栏照常显示）UIKit 底栏不画，露出游戏层那份：没有数值和经验条、难度格空、十字位写着「功能」
                       // When only the NPC dialog menu / speech box is open (by design the bottom bar stays visible), the UIKit bottom bar is not drawn and the game-layer copy shows through: no values or experience bar, the difficulty cell empty, and "功能" (Functions) written in the cross slot
    if (self.hud.valid && self.hud.dead) return;   // 死亡画面：右侧圆钮已由掩码收起，左侧摇杆圈也不画（截图：死后摇杆圈还在） | death screen: the round buttons on the right are already hidden by the mask, and the stick ring on the left is not drawn either (screenshot: the stick ring was still there after death)
    if (s->navigation_mode) return;   // 面板开着（与起摇杆同一个判定：dm_touch_controls.c 起摇杆的条件带 !navigation_mode）：摇杆起不来，静止淡影只会压在面板上（iPad 上双面板占满整宽）——不画 | a panel is open (the same check as starting the stick: the stick-start condition in dm_touch_controls.c includes !navigation_mode): the stick cannot start, and the idle faint shadow would only sit on top of the panel (on iPad the dual panels fill the whole width) - not drawn
    BOOL moving = s->joy_slot >= 0;
    CGPoint o = moving ? CGPointMake(s->joy_origin_x,s->joy_origin_y) : CGPointMake(l->joy_x,l->joy_y);
    CGFloat radius=l->joy_radius;
    CGRect base=CGRectMake(o.x-radius,o.y-radius,radius*2,radius*2);
    [self drawJoystickPart:base base:YES active:moving];
    CGFloat dx=s->lx/32767.0*radius,dy=-s->ly/32767.0*radius;
    CGRect thumb=CGRectMake(o.x+dx-23*k,o.y+dy-23*k,46*k,46*k);
    [self drawJoystickPart:thumb base:NO active:moving];

}
- (NSDictionary *)controlMetadata {
    const dm_tc_state *s=self.controls; if (!s) return @{};
    NSMutableArray *a=[NSMutableArray array];
    dm_tg_snapshot visualNative=self.nativeMapping; uint64_t hostNow=now_ns();
    NSString *names[DM_TC_COUNT]={@"main",@"skill1",@"skill2",@"skill3",@"skill4",@"portal",@"belt1",@"belt2",@"belt3",@"belt4",@"menu",@"map"};
    for(int i=0;i<DM_TC_COUNT;i++) {
        const dm_tc_target *t=&s->layout.target[i];
        NSMutableDictionary *entry=[@{@"id":names[i],@"rect_pt":@[@(t->x-t->r),@(t->y-t->r),@(t->r*2),@(t->r*2)],@"npad_button":@(t->button),@"visible":@((s->enabled_targets&(1u<<i))!=0)} mutableCopy];
        if(!self.navigationMode && i>=DM_TC_PORTAL && i<=DM_TC_MENU) {
            entry[@"visual_owner"]=i==DM_TC_PORTAL?@"overlay_portal_skill":(i==DM_TC_MENU?@"native_levelup_button":@"native_game_belt"); entry[@"host_drawn"]=@NO;
            entry[@"touch_enabled"]=@((s->enabled_targets&(1u<<i))!=0);
        }
        if(i<=DM_TC_SKILL4) {
            dm_ta_glyph g=shown_glyph(&visualNative,t->button,hostNow,s->skill_set);
            BOOL held=button_held(s,i);
            BOOL drawn=!self.navigationMode && [self skillImage:g held:held]!=nil;
            NSString *status=self.navigationMode?@"navigation_label":(g.status==DM_TA_EMPTY?@"empty_slot":(drawn?@"current_skill":@"empty_unknown"));
            entry[@"currentSkill"]=@{@"schema":@"dm-current-skill-visual-1",@"status":status,@"reason":[NSString stringWithUTF8String:g.reason],@"raw_skill_id":@(g.raw_skill_id),@"slot":@(g.slot),@"effective_slot":@(g.effective_slot),@"atlas":@(g.atlas),@"normal_frame":@(g.normal_frame),@"pressed_frame":@(g.pressed_frame),@"display_frame":@(held?g.pressed_frame:g.normal_frame),@"glyph_drawn":@(drawn),@"icon_diameter_ratio":@(.76),@"visual_rect_pt":@[@(dm_button_rect(t->x,t->y,t->r).origin.x),@(dm_button_rect(t->x,t->y,t->r).origin.y),@(t->r*2),@(t->r*2)]};
        }
        [a addObject:entry];
    }
    [a addObject:@{@"id":@"swap",@"rect_pt":@[@(s->layout.swap.x-s->layout.swap.r),@(s->layout.swap.y-s->layout.swap.r),@(s->layout.swap.r*2),@(s->layout.swap.r*2)],@"skill_set":@(s->skill_set),@"visible":@(!self.navigationMode)}];
    BOOL moving=s->joy_slot>=0; CGFloat k=s->layout.joy_radius/60;
    CGFloat jx=moving?s->joy_origin_x:s->layout.joy_x,jy=moving?s->joy_origin_y:s->layout.joy_y,r=s->layout.joy_radius;
    CGFloat tx=jx+s->lx/32767.0*r,ty=jy-s->ly/32767.0*r;
    NSDictionary *joystick=@{@"id":@"joystick",@"active":@(moving),@"base_rect_pt":@[@(jx-r),@(jy-r),@(2*r),@(2*r)],@"thumb_rect_pt":@[@(tx-23*k),@(ty-23*k),@(46*k),@(46*k)]};
    dm_tg_snapshot native=self.nativeMapping;
    NSMutableArray *mapping=[NSMutableArray array],*ids=[NSMutableArray array];
    for(int i=0;i<DM_TG_FACETS;i++) [mapping addObject:@{@"logical":@(native.mapping[i].logical),@"normal":@(native.mapping[i].normal),@"alt":@(native.mapping[i].alt)}];
    for(int i=0;i<DM_TG_SLOTS;i++) [ids addObject:@(native.raw_skill_id[i])];
    NSDictionary *nativeInfo=@{@"schema":@"dm-native-binding-4",@"source_id":@(native.source),@"source":native.source==DM_TG_SOURCE_NATIVE_GETTER?@"native_getter":@"none",
        @"supported":@(native.supported),@"valid":@(native.valid),@"ready":@(native.ready),@"secondary_group":@(native.secondary_group),@"group_seen":@(native.group_seen),@"group_started_ns":@(native.group_started_ns),@"binding_identity":@(native.binding_identity),@"scene_epoch":@(native.scene_epoch),@"world_epoch":@(native.world_epoch),@"identity_kind":native.identity_scope==DM_TG_IDENTITY_NATIVE?@"fixed_native_hud_updater_cohort":(native.identity_scope==DM_TG_IDENTITY_SYNTHETIC?@"synthetic_stub":@"unconfirmed"),@"caller_identity":@(native.caller_identity),@"hud_identity":@(native.hud_identity),@"identity_scope":@(native.identity_scope),@"captured_ns":@(native.captured_ns),@"selected_bank":@(native.selected_bank),@"getter_input_slot":@(native.getter_input_slot),@"valid_scope":@"table_parse_only",@"observed_index":@(native.observed_index),@"mapping":mapping,@"raw_skill_ids":ids,
        @"portal":@{@"valid":@(native.portal.valid),@"slot":@(native.portal.slot),@"raw_skill_id":@(native.portal.raw_skill_id),@"buttons":@(native.portal.buttons)}};
    // capture版本与布局版本分开；期望state由测试输入提供，不用visible反推。
    // The capture version and the layout version are separate; the expected state is supplied by the test input, not inferred back from visible.
    return @{@"capture_schema":@"dm-touch-capture-2",@"profile_id":@"native_art_v1",@"binding_profile_id":@"touch_native_binding_v5",@"control_layout":@"thumb_fan_v5_lower_portal_arc",@"tablet_layout":@(s->layout.tablet),@"swap_above":@(s->layout.swap_above),@"input_schema":@"dm-touch-input-5",@"native_mapping":nativeInfo,
        @"layout_sha256":[NSString stringWithUTF8String:dm_ta_geometry_sha],@"geometry_sha256":[NSString stringWithUTF8String:dm_ta_geometry_sha],@"binding_protocol_sha256":[NSString stringWithUTF8String:dm_ta_binding_sha],
        @"art_provenance":@{@"schema":@"dm-native-art-provenance-1",@"manifest":@"native_art_manifest.json",@"manifest_sha256":[NSString stringWithUTF8String:dm_ta_manifest_sha],@"shell_idle_alpha":@.28,@"shell_held_alpha":@.6,@"button_style":@"immortal_reference_v5_neutral",@"fixed_functions":@[@"portal",@"map",@"menu"],@"unsupported_state":@"no ammo/cooldown/mana tint"},
        @"source":TARGET_OS_SIMULATOR?@"uikit_simulator_overlay":@"uikit_device_overlay",
        @"right_aim":@{@"rx":@(s->rx),@"ry":@(s->ry),@"owner_slot":@(s->aim_slot),@"map_cancel_active":@(s->map_cancel_active),@"navigation_suppressed":@(s->navigation_mode),@"mapping":@"UIKit dx/r, -dy/r; independent clamped SDK axes"},
        @"state":self.navigationMode?@"navigation":@"game",@"joystick":joystick,@"coordinate_space":@"UIKit points",@"view_size_pt":@[@(s->layout.width),@(s->layout.height)],@"navigation":@(self.navigationMode),@"controls":a,@"portal_request":@{@"gesture_epoch":@(s->gesture_epoch),@"binding_identity":@(s->portal_binding_identity),@"scene_epoch":@(s->portal_scene_epoch),@"phase":@(s->portal_phase),@"pending":@(s->portal_pending)},@"portal_binding_verified":@NO,@"game_hud_relayout_verified":@NO};
}
@end
