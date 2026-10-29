// 秘境爬塔左侧任务栏：放在左边并缩小，布局参考暗黑 3 右侧任务栏，倒计时条和整体风格按暗黑 2。
// 尺寸数字照设计稿：设计稿按 Mac 窗口 2 倍像素画（游戏区 1334×614），
// 这里以 u = 游戏区高 / 614 为单位换算。数据由 dm/host/dm_rift.c 出（dm_rift_hud），两端前端刷新时调用。
// 字：原版 BlizzardGlobal——运行时从资源包读出来注册（不进仓库、不进 App 包）；读不到就用系统粗体。
//   资源包读法同 dm/ios/tools/touch_layout.py read_layout：data/build_manifest.psv 按路径后缀找行 → 第 7 列编码键（第 10、11 字节 = 数据档号，12–16 = 偏移）、
//   第 8 列原始长度、第 9 列编码（n 原样 / z 整块 zlib / 其余 BLTE 分块：每块首字节 N 原样、Z = zlib）。
// Left-side quest bar for the rift tower climb: placed on the left and shrunk; the layout follows Diablo 3's right-side quest bar, the countdown bar and overall style follow Diablo 2.
// Size numbers follow the design mock: the mock is drawn at 2x pixels of the Mac window (game area 1334×614),
// and here everything is converted with u = game area height / 614 as the unit. Data comes from dm/host/dm_rift.c (dm_rift_hud); both Apple front ends call it while refreshing their overlays.
// Font: the original game's BlizzardGlobal -- read from the asset pack at runtime and registered (not in the repository, not in the App bundle); if it cannot be read, use the system bold font.
//   The asset pack is read the same way as dm/ios/tools/touch_layout.py read_layout: find the row in data/build_manifest.psv by path suffix → column 7 is the encoding key (bytes 10 and 11 = data archive number, 12–16 = offset),
//   column 8 the raw length, column 9 the encoding (n as-is / z whole-block zlib / otherwise BLTE chunks: first byte of each chunk N as-is, Z = zlib).
#import "dm_rift_hud.h"
#import <QuartzCore/QuartzCore.h>
#if TARGET_OS_OSX
#define DmRiftView NSView
#define DmRiftFont NSFont
#define DmRiftColor NSColor
#else
#define DmRiftView UIView
#define DmRiftFont UIFont
#define DmRiftColor UIColor
#endif
#include "dm_i18n.h"
#import <CoreText/CoreText.h>
#include "../../host/dm_rift.h"

static NSData *zinflate(const uint8_t *p, size_t n, size_t cap) {   // zlib（带 2 字节头）→ 原文；Foundation 的 Zlib 算法是裸 deflate，跳过头 | zlib (with 2-byte header) → plaintext; Foundation's Zlib algorithm is raw deflate, so skip the header
    (void)cap; if (n < 2) return nil; return [[NSData dataWithBytes:p + 2 length:n - 2] decompressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib error:nil];
}
static NSData *romfs_file(NSString *suffix) {
    const char *root = getenv("DM_ROMFS"); if (!root) return nil;
    NSString *data = [@(root) stringByAppendingPathComponent:@"data"], *psv = [NSString stringWithContentsOfFile:[data stringByAppendingPathComponent:@"build_manifest.psv"] encoding:NSUTF8StringEncoding error:nil];
    for (NSString *line in [psv componentsSeparatedByString:@"\n"]) { NSArray<NSString *> *f = [line componentsSeparatedByString:@"|"];
        if (f.count < 11 || [line hasPrefix:@"#"] || ![f[9].lowercaseString hasSuffix:suffix]) continue;
        NSString *ek = f[6]; if (ek.length < 32) return nil; uint8_t k[16]; for (int i = 0; i < 16; i++) k[i] = (uint8_t)strtol([[ek substringWithRange:NSMakeRange(i * 2, 2)] UTF8String], NULL, 16);
        uint64_t off = 0; for (int i = 11; i < 16; i++) off = off << 8 | k[i]; long rawlen = f[7].longLongValue;
        NSFileHandle *h = [NSFileHandle fileHandleForReadingAtPath:[data stringByAppendingPathComponent:[NSString stringWithFormat:@"%02d-%08d.data", k[9], k[10]]]]; if (!h) return nil;
        [h seekToFileOffset:off]; NSData *raw = [h readDataOfLength:rawlen]; [h closeFile]; const uint8_t *r = raw.bytes; if ((long)raw.length != rawlen) return nil;
        if ([f[8] isEqualToString:@"n"]) return raw;
        if ([f[8] isEqualToString:@"z"]) return zinflate(r, raw.length, 64 << 20);
        uint32_t hs = (uint32_t)r[4] << 24 | r[5] << 16 | r[6] << 8 | r[7], nch = (uint32_t)r[9] << 16 | r[10] << 8 | r[11], pos = hs; NSMutableData *o = [NSMutableData data];
        for (uint32_t i = 0; i < nch; i++) { const uint8_t *e = r + 12 + i * 24; uint32_t es = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3], ds = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
            if (pos + es > raw.length) return nil; const uint8_t *b = r + pos;
            if (b[0] == 'N') [o appendBytes:b + 1 length:es - 1]; else { NSData *d = zinflate(b + 1, es - 1, ds); if (!d) return nil; [o appendData:d]; } pos += es; }
        return o; }
    return nil;
}
static DmRiftFont *d2font(CGFloat size) {   // 原版 BlizzardGlobal（中文、数字都是它） | The original game's BlizzardGlobal (used for both Chinese text and digits)
    static NSString *name; static int tried;
    if (!tried) { tried = 1; NSData *d = romfs_file(@"/hd/ui/fonts/blizzardglobal-v5_81.ttf");
        CGDataProviderRef p = d ? CGDataProviderCreateWithCFData((__bridge CFDataRef)d) : NULL; CGFontRef f = p ? CGFontCreateWithDataProvider(p) : NULL;
        if (f && CTFontManagerRegisterGraphicsFont(f, NULL)) name = (__bridge_transfer NSString *)CGFontCopyPostScriptName(f);
        if (f) CGFontRelease(f); if (p) CGDataProviderRelease(p);
        NSLog(@"[秘境] 任务栏字体：%@", name ?: @"读不到原版 BlizzardGlobal，用系统粗体"); }
    return (name ? [DmRiftFont fontWithName:name size:size] : nil) ?: [DmRiftFont boldSystemFontOfSize:size];
}
static DmRiftColor *rgb(int r, int g, int b, CGFloat a) { return [DmRiftColor colorWithRed:r / 255.0 green:g / 255.0 blue:b / 255.0 alpha:a]; }
static CATextLayer *text_layer(void) { CATextLayer *t = [CATextLayer layer];
#if TARGET_OS_OSX
    t.contentsScale = NSScreen.mainScreen.backingScaleFactor ?: 1;
#else
    t.contentsScale = UIScreen.mainScreen.scale;
#endif
     t.shadowOpacity = 0.86; t.shadowOffset = CGSizeMake(1, 1); t.shadowRadius = 0; t.shadowColor = DmRiftColor.blackColor.CGColor; return t; }
static void set_text(CATextLayer *t, NSString *s, DmRiftFont *f, DmRiftColor *c, CGRect fr, NSString *align) {
    t.frame = fr; t.alignmentMode = align; t.foregroundColor = c.CGColor; t.font = (__bridge CFTypeRef)f; t.fontSize = f.pointSize;
    if (![t.string isEqual:s]) t.string = s;
}

void dm_rift_hud_draw(DmRiftView *host, CGRect wr, BOOL hide) {
    static CALayer *box, *lineT, *lineB, *lineL, *track, *ticks[9]; static CAGradientLayer *fill; static CAShapeLayer *pin[2]; static CATextLayer *title, *sub, *timer, *goal;
    dm_rift_hud_t h; int on = !hide && dm_rift_hud(&h);
    if (!box) { if (!on) return;
        box = [CALayer layer];
#if TARGET_OS_OSX
        box.geometryFlipped = YES; // AppKit 宿主左下原点，子层保留 iOS 左上布局 | Children keep the iOS top-left layout in AppKit's bottom-left host.
#endif
        box.backgroundColor = rgb(0, 0, 0, 170 / 255.0).CGColor;
        lineT = [CALayer layer]; lineB = [CALayer layer]; lineL = [CALayer layer]; track = [CALayer layer]; fill = [CAGradientLayer layer];
        title = text_layer(); sub = text_layer(); timer = text_layer(); goal = text_layer();
        for (CALayer *l in @[lineT, lineB, lineL, title, sub, timer, goal, track]) [box addSublayer:l];
        [track addSublayer:fill]; for (int i = 0; i < 9; i++) { ticks[i] = [CALayer layer]; ticks[i].backgroundColor = rgb(0, 0, 0, 110 / 255.0).CGColor; [track addSublayer:ticks[i]]; }
        for (int i = 0; i < 2; i++) { pin[i] = [CAShapeLayer layer]; pin[i].fillColor = rgb(160, 135, 80, 1).CGColor; pin[i].strokeColor = rgb(60, 48, 30, 1).CGColor; [box addSublayer:pin[i]]; }
        [host.layer addSublayer:box]; }
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    box.hidden = !on;
    if (on) {
        CGFloat u = wr.size.height / 614, W = 250 * u, H = 78 * u;

#if TARGET_OS_OSX
        box.frame = CGRectMake(wr.origin.x + 28 * u, CGRectGetMaxY(wr) - 150 * u - H, W, H);
        CGFloat scale = host.window.backingScaleFactor ?: NSScreen.mainScreen.backingScaleFactor ?: 1;
        for (CATextLayer *text in @[title, sub, timer, goal]) text.contentsScale = scale;
#else
        box.frame = CGRectMake(wr.origin.x + 28 * u, wr.origin.y + 150 * u, W, H);
#endif
        lineT.frame = CGRectMake(0, 0, W, MAX(1, u)); lineB.frame = CGRectMake(0, H - MAX(1, u), W, MAX(1, u)); lineL.frame = CGRectMake(0, 0, 2 * u, H);
        lineT.backgroundColor = lineB.backgroundColor = rgb(120, 100, 60, 0.9).CGColor; lineL.backgroundColor = rgb(150, 125, 75, 1).CGColor;
        int g = h.guardian, s = h.secs_left < 0 ? 0 : h.secs_left; DmRiftColor *gold = rgb(199, 179, 119, 1), *red = rgb(200, 50, 40, 1), *white = rgb(215, 210, 200, 1), *grey = rgb(150, 145, 135, 1);
        set_text(title, @(dm_ui("dmRiftHudTitle")), d2font(17 * u), g ? red : gold, CGRectMake(10 * u, 6 * u, 40 * u, 22 * u), kCAAlignmentLeft);
        NSString *st = h.done ? (h.inc ? [NSString stringWithFormat:@(dm_ui("dmRiftHudDone")), h.tier] : [NSString stringWithFormat:@(dm_ui("dmRiftHudLate")), h.tier]) : g ? @(dm_ui("dmRiftHudGuard")) : [NSString stringWithFormat:@(dm_ui("dmRiftTierN")), h.tier];
        set_text(sub, st, d2font(14 * u), white, CGRectMake(48 * u, 9 * u, 130 * u, 19 * u), kCAAlignmentLeft);
        set_text(timer, [NSString stringWithFormat:@"%d:%02d", s / 60, s % 60], d2font(17 * u), (s <= 60 && !h.done) ? red : gold, CGRectMake(W - 90 * u, 6 * u, 80 * u, 22 * u), kCAAlignmentRight);
        NSString *gl = h.done ? (!h.inc ? @(dm_ui("dmRiftHudSame")) : h.next > h.tier ? [NSString stringWithFormat:@(dm_ui("dmRiftHudNext")), h.next] : @(dm_ui("dmRiftHudTop"))) : h.goal == 5 ? [NSString stringWithFormat:@(dm_ui("dmRiftHudOverKillN")), h.gdead, h.gtotal] : h.goal == 4 ? @(dm_ui("dmRiftHudOverKill")) : h.goal == 3 ? @(dm_ui("dmRiftHudOver"))
            : (h.goal == 1 || h.goal == 2) && h.nwave > 1 ? [NSString stringWithFormat:@(dm_ui("dmRiftHudKillWave")), h.wave, h.nwave, h.gdead, h.gtotal] : h.goal == 2 ? [NSString stringWithFormat:@(dm_ui("dmRiftHudKillN")), h.gdead, h.gtotal] : h.goal == 1 ? @(dm_ui("dmRiftHudKill")) : [NSString stringWithFormat:@(dm_ui("dmRiftHudGoal")), (int)(h.progress * 100)];   // 已超时：第二行换成这一句，优先于「击败守护者」/ 进度目标（第一行照旧写第几层 / 守护者现身） | overtime: the second line becomes this sentence, taking priority over "Slay the Rift Guardian" / the progress goal (the first line still says which level / that the Guardian has appeared)
        set_text(goal, gl, d2font(12 * u), grey, CGRectMake(10 * u, 32 * u, W - 20 * u, 17 * u), kCAAlignmentLeft);
        CGRect tr = CGRectMake(11 * u, 55 * u, W - 22 * u, 10 * u);   // 含 1 单位外框（条 8 + 外框各 1）   // 进度条：外框暗边 + 深底，填充金褐（守护者转暗红），每 10% 一道刻痕，两端菱形钉 | Includes the 1-unit outer frame (bar 8 + frame 1 on each side)   // Progress bar: dark-edged outer frame + deep background, golden-brown fill (turns dark red for the guardian), a notch every 10%, diamond studs at both ends
        track.frame = tr; track.backgroundColor = rgb(25, 20, 14, 1).CGColor; track.borderColor = rgb(95, 78, 48, 1).CGColor; track.borderWidth = MAX(1, u);
        CGFloat bw = MAX(1, u); fill.frame = CGRectMake(bw, bw, (tr.size.width - 2 * bw) * (h.done ? 1 : h.progress), tr.size.height - 2 * bw);
        fill.colors = g ? @[(id)rgb(190, 50, 40, 1).CGColor, (id)rgb(110, 20, 15, 1).CGColor] : @[(id)rgb(205, 160, 80, 1).CGColor, (id)rgb(140, 100, 40, 1).CGColor];
        for (int i = 0; i < 9; i++) ticks[i].frame = CGRectMake(bw + (tr.size.width - 2 * bw) * (i + 1) / 10, bw, MAX(1, u), tr.size.height - 2 * bw);
        for (int i = 0; i < 2; i++) { CGFloat cx = i ? tr.origin.x + tr.size.width + 4 * u : tr.origin.x - 4 * u, cy = CGRectGetMidY(tr), r = 4 * u; CGMutablePathRef p = CGPathCreateMutable();
            CGPathMoveToPoint(p, NULL, cx, cy - r); CGPathAddLineToPoint(p, NULL, cx + r, cy); CGPathAddLineToPoint(p, NULL, cx, cy + r); CGPathAddLineToPoint(p, NULL, cx - r, cy); CGPathCloseSubpath(p); pin[i].path = p; CGPathRelease(p); pin[i].lineWidth = MAX(1, u * 0.8); }
    }
    [CATransaction commit];
}
