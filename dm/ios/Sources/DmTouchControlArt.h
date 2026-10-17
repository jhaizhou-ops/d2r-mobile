// 暗黑手游风格的触屏外观：技能薄边嵌槽与浮动摇杆，UIKit / 离屏预览共用；不改输入几何。
// Dark-fantasy mobile control art: thin skill sockets and a floating stick, shared by UIKit / offscreen previews without changing input geometry.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>

static CGRect dm_button_rect(CGFloat x, CGFloat y, CGFloat radius) {
    return CGRectMake(x-radius*.94,y-radius*.94,radius*1.88,radius*1.88);
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
    CGContextSetRGBStrokeColor(c,held?.52:.26,held?.77:.29,held?.73:.28,held?.9:(empty?.18:.55));
    CGContextSetLineWidth(c,side*(held?.018:.009));CGContextStrokeEllipseInRect(c,face);
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
    CGFloat side=r.size.width,cx=CGRectGetMidX(r),cy=CGRectGetMidY(r),radius=side*.42;
    CGContextSaveGState(c);
    CGRect disc=CGRectInset(r,side*.08,side*.08);
    const CGFloat shade[]={.018,.024,.028,active?.12:.025, .045,.057,.060,active?.16:.04};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,disc);CGContextClip(c);dm_button_gradient(c,disc,shade,2);CGContextRestoreGState(c);
    CGContextSetRGBStrokeColor(c,.66,.70,.69,active?.22:.09);CGContextSetLineWidth(c,side*.006);CGContextStrokeEllipseInRect(c,disc);
    CGContextSetRGBStrokeColor(c,.012,.018,.022,active?.18:.06);CGContextSetLineWidth(c,side*.012);CGContextStrokeEllipseInRect(c,CGRectInset(disc,side*.012,side*.012));
    // 四个轻量方向刻度固定在底盘上，只有手柄头随真实轴值移动。
    // Four faint direction marks stay on the base; only the thumb follows the actual stick axes.
    CGContextSetRGBFillColor(c,.66,.71,.70,active?.28:.10);
    for(int i=0;i<4;i++) {
        CGFloat a=i*M_PI/2,dx=cos(a),dy=sin(a),x=cx+dx*radius*.87,y=cy+dy*radius*.87,k=side*.024;
        CGContextMoveToPoint(c,x+dx*k,y+dy*k);CGContextAddLineToPoint(c,x-dx*k-dy*k*.7,y-dy*k+dx*k*.7);
        CGContextAddLineToPoint(c,x-dx*k+dy*k*.7,y-dy*k-dx*k*.7);CGContextClosePath(c);CGContextFillPath(c);
    }
    CGContextRestoreGState(c);
}
static void dm_joystick_thumb_draw(CGContextRef c, CGRect r, int active) {
    CGFloat side=r.size.width;CGRect disc=CGRectInset(r,side*.055,side*.055);
    CGContextSaveGState(c);
    const CGFloat fill[]={.20,.24,.25,active?.58:.16, .065,.085,.095,active?.62:.20};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,disc);CGContextClip(c);dm_button_gradient(c,disc,fill,2);CGContextRestoreGState(c);
    CGContextSetRGBStrokeColor(c,.72,.76,.74,active?.48:.20);CGContextSetLineWidth(c,side*.018);CGContextStrokeEllipseInRect(c,disc);
    CGContextSetRGBStrokeColor(c,.018,.026,.032,active?.62:.22);CGContextSetLineWidth(c,side*.025);CGContextStrokeEllipseInRect(c,CGRectInset(disc,side*.035,side*.035));
    CGContextRestoreGState(c);
}
