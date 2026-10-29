// 用宿主状态替身验证真实 AppKit 绘制器的可见性、坐标、进度和结算；不读取存档。
// Verify the real AppKit renderer's visibility, coordinates, progress and completion with stub host state; no saves are read.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import "../../ios/Sources/dm_rift_hud.h"
#include "../dm_rift.h"
#include <assert.h>
#include <string.h>
static dm_rift_hud_t state;
static int active;
int dm_rift_hud(dm_rift_hud_t *out) { *out = state; return active; }
const char *dm_ui(const char *key) {
    if (!strcmp(key, "dmRiftHudTitle")) return "Rift";
    if (!strcmp(key, "dmRiftTierN")) return "Tier %d";
    if (!strcmp(key, "dmRiftHudGoal")) return "Progress %d%%";
    if (!strcmp(key, "dmRiftHudGuard")) return "Guardian";
    if (!strcmp(key, "dmRiftHudKillN")) return "Defeated %d/%d";
    if (!strcmp(key, "dmRiftHudDone")) return "Tier %d cleared";
    if (!strcmp(key, "dmRiftHudNext")) return "Next tier %d";
    return key;
}
static void near(CGFloat a, CGFloat b) { assert(fabs(a - b) < 0.01); }
int main(int argc, char **argv) { @autoreleasepool {
    NSView *host = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1334, 614)]; host.wantsLayer = YES;
    CGRect wr = CGRectMake(0, 0, 1334, 614);
    dm_rift_hud_draw(host, wr, NO); assert(host.layer.sublayers.count == 0);
    active = 1; state = (dm_rift_hud_t){.tier=10, .progress=.42f, .secs_left=125};
    dm_rift_hud_draw(host, wr, NO); assert(host.layer.sublayers.count == 1);
    CALayer *box = host.layer.sublayers[0]; assert(!box.hidden && box.geometryFlipped);
    near(box.frame.origin.x, 28); near(box.frame.origin.y, 386); near(box.frame.size.height, 78);
    NSArray *children = box.sublayers;
    CATextLayer *sub = children[4], *timer = children[5], *goal = children[6];
    CALayer *track = children[7], *fill = track.sublayers[0];
    assert([sub.string isEqual:@"Tier 10"] && [timer.string isEqual:@"2:05"] && [goal.string isEqual:@"Progress 42%"]);
    near(fill.frame.size.width, 226 * .42);
    dm_rift_hud_draw(host, wr, YES); assert(box.hidden);
    dm_rift_hud_draw(host, wr, NO); assert(!box.hidden && host.layer.sublayers.count == 1);
    active = 0; dm_rift_hud_draw(host, wr, NO); assert(box.hidden); active = 1;
    wr = CGRectMake(80, 20, 667, 307); dm_rift_hud_draw(host, wr, NO);
    near(box.frame.origin.x, 94); near(box.frame.origin.y, 213); near(box.frame.size.width, 125);
    state.guardian=1; state.goal=2; state.gdead=1; state.gtotal=2; state.secs_left=-1;
    dm_rift_hud_draw(host, wr, NO); assert([sub.string isEqual:@"Guardian"] && [timer.string isEqual:@"0:00"] && [goal.string isEqual:@"Defeated 1/2"]);
    state.done=1; state.inc=2; state.next=12; dm_rift_hud_draw(host, wr, NO);
    assert([sub.string isEqual:@"Tier 10 cleared"] && [goal.string isEqual:@"Next tier 12"]); near(fill.frame.size.width, 112);
    state = (dm_rift_hud_t){.tier=10, .progress=.42f, .secs_left=125};
    dm_rift_hud_draw(host, CGRectMake(0, 0, 1334, 614), NO);
    if (argc > 1) {
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:1334 pixelsHigh:614 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        NSGraphicsContext *gc = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [host.layer renderInContext:gc.CGContext];
        assert([[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(argv[1]) atomically:YES]);
    }
    puts("rift HUD: visibility, viewport, progress, timer, guardian and completion OK");
} return 0; }
