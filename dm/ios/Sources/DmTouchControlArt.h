// 暗黑手游风格的触屏外观：技能薄边嵌槽与浮动摇杆，UIKit / 离屏预览共用；不改输入几何。
// Dark-fantasy mobile control art: thin skill sockets and a floating stick, shared by UIKit / offscreen previews without changing input geometry.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>

static CGRect dm_button_rect(CGFloat x, CGFloat y, CGFloat radius) {
    return CGRectMake(x-radius,y-radius,radius*2,radius*2);
}
static void dm_button_gradient(CGContextRef c, CGRect r, const CGFloat *colors, size_t count) {
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGGradientRef g=CGGradientCreateWithColorComponents(space,colors,NULL,count);
    CGContextDrawLinearGradient(c,g,CGPointMake(CGRectGetMidX(r),CGRectGetMinY(r)),CGPointMake(CGRectGetMidX(r),CGRectGetMaxY(r)),0);
    CGGradientRelease(g);CGColorSpaceRelease(space);
}
// 只中和内槽的色相；裁切区域避开已验收的金属外圈。
// Neutralize only the socket interior; the clip excludes the accepted metal rim.
static void dm_socket_neutralize(CGContextRef c, CGRect r) {
    CGContextSaveGState(c);CGFloat inset=r.size.width*.08;
    CGContextAddEllipseInRect(c,CGRectInset(r,inset,inset));CGContextClip(c);
    CGContextSetBlendMode(c,kCGBlendModeSaturation);CGContextSetGrayFillColor(c,.5,1);CGContextFillRect(c,r);
    CGContextRestoreGState(c);
}
// 银灰处理保留原图明暗；调用方随后用原图的 alpha 恢复透明边缘。
// Silver treatment preserves source shading; callers then restore transparent edges with the original alpha.
static void dm_skill_silver(CGContextRef c, CGRect r) {
    CGContextSaveGState(c);CGContextSetBlendMode(c,kCGBlendModeSaturation);
    CGContextSetGrayFillColor(c,.5,1);CGContextFillRect(c,r);CGContextRestoreGState(c);
}
// 按下态只提亮为银灰，与黑灰内槽和铁边一致。
// Held feedback uses a silver-gray highlight consistent with the charcoal face and iron rim.
static void dm_button_highlight(CGContextRef c, CGRect r) {
    CGContextSaveGState(c);CGContextSetGrayStrokeColor(c,.64,.72);
    CGContextSetLineWidth(c,r.size.width*.015);
    CGContextStrokeEllipseInRect(c,CGRectInset(r,r.size.width*.052,r.size.width*.052));
    CGContextRestoreGState(c);
}
static void dm_button_draw(CGContextRef c, CGRect r, int held, int empty, int primary) {
    CGFloat side=r.size.width,cx=CGRectGetMidX(r),cy=CGRectGetMidY(r);
    CGRect rim=CGRectInset(r,side*.018,side*.018),face=CGRectInset(r,side*.055,side*.055);
    CGContextSaveGState(c);
    // 窄铁边带极淡的暗红底色，图案占主体；空槽不画厚重底板或装饰。
    // Narrow iron edges with a faint dark-red undertone, dominated by the skill image; empty slots have no heavy backing or ornaments.
    const CGFloat metal[]={.26,.28,.28,empty?.22:.9, .055,.065,.07,empty?.22:.95, .16,.11,.10,empty?.16:.7};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,rim);CGContextClip(c);dm_button_gradient(c,rim,metal,3);CGContextRestoreGState(c);
    CGContextSetRGBStrokeColor(c,.008,.012,.015,empty?.38:.9);CGContextSetLineWidth(c,side*.025);CGContextStrokeEllipseInRect(c,rim);
    const CGFloat well[]={.021,.021,.021,empty?.08:.94, .050,.050,.050,empty?.16:.94};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,face);CGContextClip(c);dm_button_gradient(c,face,well,2);CGContextRestoreGState(c);
    // 空槽用稍亮的银灰细边，在明亮地面上保留绑定入口的轮廓。
    // A brighter silver-gray hairline keeps empty binding slots visible on bright ground.
    if(empty) CGContextSetGrayStrokeColor(c,held?.72:.62,held?.80:.60);
    else CGContextSetRGBStrokeColor(c,held?.64:.26,held?.64:.29,held?.64:.28,held?.72:.55);
    CGContextSetLineWidth(c,side*(held?.018:(empty?.012:.009)));CGContextStrokeEllipseInRect(c,face);
    if(primary && !empty) {
        CGContextSetRGBStrokeColor(c,.31,.23,.22,.52);CGContextSetLineWidth(c,side*.012);
        CGContextStrokeEllipseInRect(c,CGRectInset(r,side*.035,side*.035));
    }
    if(empty) {
        CGFloat arm=side*.055;CGContextSetRGBStrokeColor(c,.60,.65,.64,held?.75:.25);
        CGContextSetLineWidth(c,side*.018);CGContextSetLineCap(c,kCGLineCapRound);
        CGContextMoveToPoint(c,cx-arm,cy);CGContextAddLineToPoint(c,cx+arm,cy);
        CGContextMoveToPoint(c,cx,cy-arm);CGContextAddLineToPoint(c,cx,cy+arm);CGContextStrokePath(c);
    }
    CGContextRestoreGState(c);
}
static void dm_joystick_base_draw(CGContextRef c, CGRect r, int active) {
    CGFloat side=r.size.width,cx=CGRectGetMidX(r),cy=CGRectGetMidY(r);
    CGPoint center=CGPointMake(cx,cy);
    CGContextSaveGState(c);
    // 底盘向边缘渐隐，避免硬圆框压住地面；细刻纹保留方向感。
    // Feather the base into the ground instead of a hard circular frame; fine etchings retain orientation.
    const CGFloat haze[]={.025,.025,.025,active?.10:.045,
        .025,.025,.025,active?.22:.12, .025,.025,.025,0};
    const CGFloat stops[]={0,.78,1};
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGGradientRef gradient=CGGradientCreateWithColorComponents(space,haze,stops,3);
    CGContextDrawRadialGradient(c,gradient,center,0,center,side*.48,0);
    CGGradientRelease(gradient);CGColorSpaceRelease(space);
    CGContextSetLineWidth(c,side*.004);CGContextSetLineCap(c,kCGLineCapRound);
    // 留出四个缺口，让方向刻纹融入底盘，而不是悬浮的实心箭头。
    // Four gaps integrate the direction etchings into the base instead of floating solid arrows.
    for(int i=0;i<4;i++) {
        CGFloat angle=i*M_PI/2;
        CGContextSetGrayStrokeColor(c,.63,active?.12:.055);
        CGContextAddArc(c,cx,cy,side*.416,angle+.20,angle+M_PI/2-.20,0);CGContextStrokePath(c);
        CGContextSaveGState(c);CGContextTranslateCTM(c,cx,cy);CGContextRotateCTM(c,angle);
        CGFloat x=side*.414,k=side*.028;
        CGContextSetGrayStrokeColor(c,.68,active?.25:.13);
        CGContextMoveToPoint(c,x+k,0);CGContextAddLineToPoint(c,x-k,-k*.65);
        CGContextAddLineToPoint(c,x-k*.42,0);CGContextAddLineToPoint(c,x-k,k*.65);
        CGContextClosePath(c);CGContextStrokePath(c);
        CGContextMoveToPoint(c,x-k*.42,0);CGContextAddLineToPoint(c,x-k*2.1,0);CGContextStrokePath(c);
        CGContextRestoreGState(c);
    }
    CGContextRestoreGState(c);
}
static void dm_joystick_thumb_draw(CGContextRef c, CGRect r, int active) {
    CGFloat side=r.size.width;CGRect disc=CGRectInset(r,side*.055,side*.055);
    CGContextSaveGState(c);
    // 低反光的黑灰摇杆头：弧形高光与底缘阴影形成轻微厚度。
    // A matte charcoal thumb with a curved highlight and shaded lower edge for subtle depth.
    const CGFloat fill[]={.23,.23,.23,active?.78:.46,
        .14,.14,.14,active?.82:.49, .065,.065,.065,active?.84:.52};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,disc);CGContextClip(c);
    dm_button_gradient(c,disc,fill,3);CGContextRestoreGState(c);
    CGContextSetGrayStrokeColor(c,.025,active?.65:.32);CGContextSetLineWidth(c,side*.035);
    CGContextStrokeEllipseInRect(c,disc);
    CGContextSetGrayStrokeColor(c,.64,active?.40:.22);CGContextSetLineWidth(c,side*.014);
    CGContextAddArc(c,CGRectGetMidX(r),CGRectGetMidY(r),side*.437,M_PI*1.07,M_PI*1.91,0);CGContextStrokePath(c);
    CGContextSetGrayStrokeColor(c,.42,active?.20:.10);CGContextSetLineWidth(c,side*.012);
    CGContextAddArc(c,CGRectGetMidX(r),CGRectGetMidY(r),side*.410,M_PI*1.12,M_PI*1.88,0);CGContextStrokePath(c);
    CGContextRestoreGState(c);
}
