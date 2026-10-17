// 更新响应判定：沿用版本文件四字段契约，只返回有效元数据与故障类别；不联网、不更新UI。
// Update response policy: retain the four-field manifest contract and return validated metadata/failure kind; no networking or UI changes.
#pragma once
#import <Foundation/Foundation.h>
// 版本文件 → 要不要提示。返回 nil = 提示（out 填好），否则是不提示的原因（进日志）
// Version file → whether to show a notice. Returns nil = show it (out is filled in); otherwise the reason for not showing it (goes to the log)
static NSString *upd_parse(NSData *d, int *build, NSString **note, int *urgent, NSString **install) {
    NSDictionary *j = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
    if (![j isKindOfClass:NSDictionary.class]) return @"格式不对：不是 JSON 对象";
    NSNumber *b = j[@"build"], *u = j[@"urgent"]; NSString *n = j[@"note"], *i = j[@"install_url"];
    if (![b isKindOfClass:NSNumber.class] || ![u isKindOfClass:NSNumber.class] || ![n isKindOfClass:NSString.class] || ![i isKindOfClass:NSString.class]) return @"格式不对：四个字段缺了或类型不对";
    if (!(b.doubleValue >= 1 && b.doubleValue < 2e9) || b.doubleValue != b.intValue) return @"格式不对：build 不是正整数";
    NSString *sch = [NSURL URLWithString:i].scheme.lowercaseString;
    if (![sch isEqualToString:@"https"] && ![sch isEqualToString:@"itms-services"]) return @"格式不对：install_url 不是 https / itms-services 链接";
    n = [[n componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" "];
    if (n.length > 24) n = [n substringWithRange:[n rangeOfComposedCharacterSequencesForRange:NSMakeRange(0, 24)]];   // 说明只有一行的位置：超过 24 个字截掉 | The note has room for only one line: truncate beyond 24 characters
    *build = b.intValue; *note = n; *urgent = u.boolValue; *install = i; return nil;
}
static NSString *dm_update_response(NSData *d, NSURLResponse *r, NSError *e, int *build, NSString **note, int *urgent, NSString **install, int *safe, int *kind) {
    *safe = 0; *kind = 2;
    if (!e && [r isKindOfClass:NSHTTPURLResponse.class]) {
        NSInteger status = [(NSHTTPURLResponse *)r statusCode]; if (status >= 500 && status <= 599) *kind = 3;
    }
    NSString *why = nil;
    if (e) why = [NSString stringWithFormat:@"取不到：%@", e.localizedDescription];
    else if ([r isKindOfClass:NSHTTPURLResponse.class] && [(NSHTTPURLResponse *)r statusCode] != 200) why = [NSString stringWithFormat:@"取不到：HTTP %ld", (long)[(NSHTTPURLResponse *)r statusCode]];
    else if (d.length > 4096) why = @"格式不对：超过 4096 字节";
    else why = upd_parse(d, build, note, urgent, install);
    // 只有成功且完整有效的版本清单才有权改变绘制；错误页即使像JSON也不能下发开关。
    // Only a successful, fully valid manifest may change rendering; even a JSON-shaped error page cannot supply the switch.
    if (!why) {
        NSDictionary *j = [NSJSONSerialization JSONObjectWithData:d options:0 error:nil];
        NSNumber *rs = [j isKindOfClass:NSDictionary.class] ? j[@"render_safe"] : nil;
        *safe = [rs isKindOfClass:NSNumber.class] && rs.boolValue;
    }
    return why;
}
