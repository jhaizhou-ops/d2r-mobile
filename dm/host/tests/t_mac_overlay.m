// 合成目录验证 Mac 覆盖层选择：开关、缺包、完整组合；不读游戏文件。
// Verifies Mac overlay selection with synthetic directories: switches, missing package, complete combinations; reads no game files.
#import "../app_mac_overlay.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int notes, extra;
static int setting(const char *key) { return !strcmp(key, "item_notes") ? notes : !strcmp(key, "extra") ? extra : 0; }
static void variant(NSString *root, NSString *name) {
    NSString *dir = [[root stringByAppendingPathComponent:name] stringByAppendingPathComponent:@"data"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL]);
    assert([@"synthetic" writeToFile:[dir stringByAppendingPathComponent:@".build.config"] atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
}
int main(void) { @autoreleasepool {
    NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSString *base = @"/synthetic/base"; BOOL missing = YES;
    assert([dm_mac_overlay(base, nil, setting, &missing) isEqual:base] && !missing);
    notes = 1;
    assert([dm_mac_overlay(base, nil, setting, &missing) isEqual:base] && missing);
    variant(root, @"item_notes");
    assert([dm_mac_overlay(base, root, setting, &missing) isEqual:[root stringByAppendingPathComponent:@"item_notes"]] && !missing);
    notes = 0;
    assert([dm_mac_overlay(base, root, setting, &missing) isEqual:base] && !missing);
    variant(root, @"extra"); notes = extra = 1;
    assert([dm_mac_overlay(base, root, setting, &missing) isEqual:base] && missing);
    variant(root, @"extra+item_notes");
    assert([dm_mac_overlay(base, root, setting, &missing) isEqual:[root stringByAppendingPathComponent:@"extra+item_notes"]] && !missing);
    [NSFileManager.defaultManager removeItemAtPath:[root stringByAppendingPathComponent:@"extra+item_notes/data/.build.config"] error:NULL];
    assert([dm_mac_overlay(base, root, setting, &missing) isEqual:base] && missing);
    [NSFileManager.defaultManager removeItemAtPath:root error:NULL];
    puts("MAC_OVERLAY OK: off, on, absent, incomplete and exact combinations");
} }
