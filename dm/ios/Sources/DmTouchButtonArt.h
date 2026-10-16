// 触屏技能钮的铸铁嵌槽：内接原圆形触区；UIKit 与离屏预览共用同一份绘制。
// Cast-iron touch skill sockets: contained in the original circular hit area; UIKit and offscreen previews share the same drawing.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <math.h>

static CGRect dm_button_rect(CGFloat x, CGFloat y, CGFloat radius) {
    return CGRectMake(x-radius*.91,y-radius*.91,radius*1.82,radius*1.82);
}
static void dm_button_gradient(CGContextRef c, CGRect r, const CGFloat *colors, size_t count) {
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGGradientRef g=CGGradientCreateWithColorComponents(space,colors,NULL,count);
    CGContextDrawLinearGradient(c,g,CGPointMake(CGRectGetMidX(r),CGRectGetMinY(r)),CGPointMake(CGRectGetMidX(r),CGRectGetMaxY(r)),0);
    CGGradientRelease(g);CGColorSpaceRelease(space);
}
static void dm_button_draw(CGContextRef c, CGRect r, int held, int empty, int primary) {
    CGFloat side=r.size.width,edge=side*(primary?.052:.045),cx=CGRectGetMidX(r),cy=CGRectGetMidY(r);
    CGRect outer=CGRectInset(r,side*.012,side*.012),face=CGRectInset(r,edge*1.7,edge*1.7);
    CGFloat a=held?1:(empty?.43:.88);
    CGContextSaveGState(c);
    // 外沿是冷铁，细内沿才用旧铜；亮暗沿形成厚度，避免整圈金边抢画面。
    // Cold iron outside, aged brass only on the fine inner lip; lit and dark edges add depth without a dominant gold outline.
    const CGFloat metal[]={.35,.36,.34,a, .17,.19,.19,a, .055,.068,.072,a, .24,.25,.23,a};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,outer);CGContextClip(c);
    dm_button_gradient(c,outer,metal,4);CGContextRestoreGState(c);
    CGContextSetRGBStrokeColor(c,.012,.016,.017,.85);CGContextSetLineWidth(c,side*.025);CGContextStrokeEllipseInRect(c,outer);
    CGRect lip=CGRectInset(r,edge,edge);
    CGContextSetRGBStrokeColor(c,.52,.45,.31,held?.95:(empty?.25:.58));
    CGContextSetLineWidth(c,side*.018);CGContextStrokeEllipseInRect(c,lip);
    const CGFloat well[]={.016,.023,.027,empty?.26:.92, .049,.060,.061,empty?.34:.88};
    const CGFloat pressed[]={.085,.068,.038,.96, .032,.038,.039,.92};
    CGContextSaveGState(c);CGContextAddEllipseInRect(c,face);CGContextClip(c);dm_button_gradient(c,face,held?pressed:well,2);CGContextRestoreGState(c);
    CGContextSetRGBStrokeColor(c,0,0,0,held?.95:.73);CGContextSetLineWidth(c,side*.024);CGContextStrokeEllipseInRect(c,face);
    // 主钮的四颗小铆点与细切口；细节只落在边框，不盖住技能图案。
    // Four small rivets and fine cuts on the primary button; detail stays on the rim and leaves the skill image clear.
    if(primary) for(int i=0;i<4;i++) {
        CGFloat angle=(45+90*i)*M_PI/180,rad=side*.474,x=cx+cos(angle)*rad,y=cy+sin(angle)*rad,d=side*.013;
        CGContextSetRGBFillColor(c,.53,.49,.38,held?.78:.48);CGContextFillEllipseInRect(c,CGRectMake(x-d,y-d,d*2,d*2));
    }
    for(int i=0;i<4;i++) {
        CGFloat angle=i*M_PI/2,rad=side*.469;
        CGContextSetRGBStrokeColor(c,.015,.022,.024,empty?.3:.8);CGContextSetLineWidth(c,side*.012);
        CGContextMoveToPoint(c,cx+cos(angle)*(rad-side*.018),cy+sin(angle)*(rad-side*.018));
        CGContextAddLineToPoint(c,cx+cos(angle)*(rad+side*.018),cy+sin(angle)*(rad+side*.018));CGContextStrokePath(c);
    }
    if(held) {
        CGContextSetRGBStrokeColor(c,.83,.69,.39,.9);CGContextSetLineWidth(c,side*.016);
        CGContextAddArc(c,cx,cy,side*.447,3.7,5.7,0);CGContextStrokePath(c);
    }
    if(empty) {
        CGFloat arm=side*.057;
        CGContextSetRGBStrokeColor(c,.62,.59,.49,held?.85:.33);CGContextSetLineWidth(c,side*.018);CGContextSetLineCap(c,kCGLineCapRound);
        CGContextMoveToPoint(c,cx-arm,cy);CGContextAddLineToPoint(c,cx+arm,cy);
        CGContextMoveToPoint(c,cx,cy-arm);CGContextAddLineToPoint(c,cx,cy+arm);CGContextStrokePath(c);
    }
    CGContextRestoreGState(c);
}
