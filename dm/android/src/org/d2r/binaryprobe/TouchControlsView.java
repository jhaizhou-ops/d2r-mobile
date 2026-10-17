package org.d2r.binaryprobe;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityEvent;
import java.util.function.BooleanSupplier;

/** Android rendering/events only; hit testing and gestures belong to upstream C.
 *  The visual language mirrors the iOS overlay (dm/ios/Sources/DmTouchOverlay.m):
 *  dark disc + bronze double ring, action buttons fade while idle, and the
 *  joystick is a faint ring with a bronze sparkle knob and a ±30° aim arc. */
public final class TouchControlsView extends View {
    private static native void nativeResize(float width,float height,float left,float right);
    private static native void nativeEnable(boolean active);
    private static native void nativeCancel();
    private static native int nativeEvent(int[] ids,float[] xs,float[] ys,boolean cancelled);
    private static native float[] nativeFrame();
    private static native void nativeActivate(int target);
    static native int nativeFrontend();   // 1 = 局外（前端页面：主菜单/选角/加载）——覆盖层隐藏 | 1 = front-end page: overlay hidden
    // Canonical Android touch-layer tokens, mirrored in DESIGN.md and in the iOS overlay palette.
    static final int INK=0xff16110c, RIM=0xff997845, RIM_DARK=0xff1f170d, TEXT=0xffd9c9a6;
    private static final int KNOB_RING=0xffb08e4e, STAR=0xffc8a460, STAR_CORE=0xffecd696;
    private static final float IDLE=0.6f;   // iOS DM_SKILL_IDLE_ALPHA: idle fade of action-button base and rings
    private static final String[] LABELS={"主技能","技能1","技能2","技能3","技能4","回城","药水1","药水2","药水3","药水4","菜单","地图"};
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path star=new Path();
    private final BooleanSupplier ready;
    private float[] frame;
    private boolean active;
    private int accessibilityFocus=View.NO_ID;
    private final float density;
    public TouchControlsView(Context context,BooleanSupplier ready){super(context);this.ready=ready;density=getResources().getDisplayMetrics().density;
        paint.setTypeface(Typeface.SERIF);
        setContentDescription("触摸操控：左侧摇杆移动，右侧技能按钮拖动瞄准，底部药水");setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);setVisibility(GONE);}
    public void setActive(boolean on){active=on;nativeEnable(on);setVisibility(on?VISIBLE:GONE);if(on){resize();postInvalidateOnAnimation();}}
    public void cancel(){nativeCancel();}
    private void resize(){if(getWidth()==0||getHeight()==0)return;float left=0,right=0;WindowInsets insets=getRootWindowInsets();
        if(insets!=null&&insets.getDisplayCutout()!=null){left=insets.getDisplayCutout().getSafeInsetLeft()/density;right=insets.getDisplayCutout().getSafeInsetRight()/density;}
        nativeResize(getWidth()/density,getHeight()/density,left,right);frame=nativeFrame();}
    @Override protected void onSizeChanged(int w,int h,int ow,int oh){super.onSizeChanged(w,h,ow,oh);resize();}
    private void disc(Canvas c,float x,float y,float r,int color,int alpha){if(r<=0)return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setAlpha(alpha);c.drawCircle(x,y,r,paint);}
    private void ring(Canvas c,float x,float y,float radius,float width,int color,int alpha){if(radius<=0)return;
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(width);paint.setColor(color);paint.setAlpha(alpha);c.drawCircle(x,y,radius,paint);}
    private void caption(Canvas c,float x,float y,float size,String text,int alpha){if(text==null||text.isEmpty()||size<=0)return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(TEXT);paint.setAlpha(alpha);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(size);
        c.drawText(text,x,y-(paint.ascent()+paint.descent())/2,paint);}
    // Skill / main / set-switch buttons: dark base + bronze double ring, faded while idle and solid while held (iOS "少挡画面" idem).
    private void actionButton(Canvas c,float x,float y,float r,boolean pressed){float f=pressed?1:IDLE,lw=Math.max(1.5f,r*.075f);
        disc(c,x,y,r,INK,(int)(255*(pressed?.92f:.80f*f)));
        ring(c,x,y,r-lw/2,lw,RIM,(int)(255*(pressed?1:.90f*f)));
        ring(c,x,y,r-lw*1.3f,lw/2,RIM_DARK,(int)(255*(pressed?.90f:.90f*f)));}
    // Belt / menu / map cells: the game's own HUD already carries their artwork, so draw a fainter ring only.
    private void utilityButton(Canvas c,float x,float y,float r,boolean pressed){float lw=Math.max(1.2f,r*.06f);
        disc(c,x,y,r,INK,(int)(255*(pressed?.62f:.30f)));
        ring(c,x,y,r-lw/2,lw,RIM,(int)(255*(pressed?.68f:.34f)));}
    @Override protected void onDraw(Canvas canvas){super.onDraw(canvas);if(!active)return;frame=nativeFrame();if(frame==null||frame.length!=58)return;
        canvas.save();canvas.scale(density,density);
        for(int i=0;i<12;i++){float x=frame[i*3],y=frame[i*3+1],r=frame[i*3+2];if(r<=0)continue;boolean pressed=frame[44+i]>0;
            if(i<=5){actionButton(canvas,x,y,r,pressed);caption(canvas,x,y,Math.max(11,r*(i==0?.30f:.40f)),LABELS[i],(int)(255*(pressed?1:.84f)));}
            else if(i>=6&&i<=9){utilityButton(canvas,x,y,r,pressed);caption(canvas,x,y+r*.42f,Math.max(8,r*.40f),Integer.toString(i-5),(int)(255*(pressed?.95f:.68f)));}
            else{utilityButton(canvas,x,y,r,pressed);caption(canvas,x,y,Math.max(8,r*.38f),LABELS[i],(int)(255*(pressed?.95f:.68f)));}}
        float sx=frame[39],sy=frame[40],sr=frame[41];
        if(sr>0){actionButton(canvas,sx,sy,sr,false);caption(canvas,sx,sy,Math.max(11,sr*.62f),frame[56]>0?"Ⅱ":"Ⅰ",255);}
        float jx=frame[36],jy=frame[37],jr=frame[38];
        if(jr>0){boolean moving=frame[57]>0;ring(canvas,jx,jy,jr,Math.max(1.1f,jr*.017f),RIM,(int)(255*(moving?.32f:.12f)));
            float kx=frame[42],ky=frame[43],kr=jr*.383f,w=Math.max(1.2f,kr*.075f),e=kr*.45f;
            int ka=(int)(255*(moving?.82f:.20f));
            disc(canvas,kx,ky,kr,INK,ka);ring(canvas,kx,ky,kr-w/2,w,KNOB_RING,ka);
            star.reset();star.moveTo(0,-e);star.quadTo(0,0,e,0);star.quadTo(0,0,0,e);star.quadTo(0,0,-e,0);star.quadTo(0,0,0,-e);star.close();
            canvas.save();canvas.translate(kx,ky);paint.setStyle(Paint.Style.FILL);paint.setColor(STAR);paint.setAlpha(ka);
            canvas.drawPath(star,paint);disc(canvas,0,0,kr*.12f,STAR_CORE,ka);canvas.restore();
            float dx=kx-jx,dy=ky-jy;
            if(moving&&dx*dx+dy*dy>1){float ar=jr+3;RectF oval=new RectF(jx-ar,jy-ar,jx+ar,jy+ar);
                paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(Math.max(1.6f,jr*.033f));paint.setColor(RIM);paint.setAlpha((int)(255*.70f));
                canvas.drawArc(oval,(float)Math.toDegrees(Math.atan2(dy,dx))-30f,60f,false,paint);}}
        if(accessibilityFocus!=View.NO_ID){float x,y,r;if(accessibilityFocus<12){x=frame[accessibilityFocus*3];y=frame[accessibilityFocus*3+1];r=frame[accessibilityFocus*3+2];}
            else if(accessibilityFocus==12){x=frame[39];y=frame[40];r=frame[41];}else{x=frame[36];y=frame[37];r=frame[38];}
            ring(canvas,x,y,r+3,3,TEXT,255);}
        canvas.restore();
        if(ready.getAsBoolean())postInvalidateOnAnimation();}
    @Override public boolean onTouchEvent(MotionEvent event){if(!active||!ready.getAsBoolean())return true;
        int action=event.getActionMasked(),skip=action==MotionEvent.ACTION_POINTER_UP?event.getActionIndex():-1;
        int count=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)?0:event.getPointerCount()-(skip>=0?1:0);
        if(count>16){nativeCancel();return true;}
        int[] ids=new int[count];float[] xs=new float[count],ys=new float[count];int k=0;
        for(int i=0;i<event.getPointerCount()&&k<count;i++)if(i!=skip){ids[k]=event.getPointerId(i);xs[k]=Math.max(0,Math.min(.999999f,event.getX(i)/getWidth()));ys[k]=Math.max(0,Math.min(.999999f,event.getY(i)/getHeight()));k++;}
        nativeEvent(ids,xs,ys,action==MotionEvent.ACTION_CANCEL);postInvalidateOnAnimation();return true;}
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider(){return new AccessibilityNodeProvider(){
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id){AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();node.setPackageName(getContext().getPackageName());node.setSource(TouchControlsView.this,id);
            if(id==View.NO_ID){onInitializeAccessibilityNodeInfo(node);if(frame!=null){for(int i=0;i<12;i++)if(frame[i*3+2]>0)node.addChild(TouchControlsView.this,i);node.addChild(TouchControlsView.this,12);for(int i=20;i<24;i++)node.addChild(TouchControlsView.this,i);}return node;}
            if(frame==null)return null;float x,y,r;String label;
            if(id>=0&&id<12){x=frame[id*3];y=frame[id*3+1];r=frame[id*3+2];label=LABELS[id];}
            else if(id==12){x=frame[39];y=frame[40];r=frame[41];label="切换技能组";}
            else if(id>=20&&id<24){r=frame[38]/3;x=frame[36]+(id==20?-r*2:id==21?r*2:0);y=frame[37]+(id==22?-r*2:id==23?r*2:0);label=new String[]{"向左移动","向右移动","向上移动","向下移动"}[id-20];}
            else return null;
            node.setClassName("android.widget.Button");node.setContentDescription(label);node.setClickable(true);node.setFocusable(true);node.setAccessibilityFocused(accessibilityFocus==id);node.setEnabled(ready.getAsBoolean());node.setVisibleToUser(active);node.setParent(TouchControlsView.this);
            Rect bounds=new Rect((int)((x-r)*density),(int)((y-r)*density),(int)((x+r)*density),(int)((y+r)*density));node.setBoundsInParent(bounds);int[] location=new int[2];getLocationOnScreen(location);bounds.offset(location[0],location[1]);node.setBoundsInScreen(bounds);node.addAction(AccessibilityNodeInfo.ACTION_CLICK);node.addAction(accessibilityFocus==id?AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS:AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS);return node;}
        @Override public boolean performAction(int id,int action,Bundle args){if(!((id>=0&&id<=12)||(id>=20&&id<=23)))return false;
            if(action==AccessibilityNodeInfo.ACTION_CLICK&&active&&ready.getAsBoolean()){nativeActivate(id);postInvalidateOnAnimation();return true;}
            if(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS||action==AccessibilityNodeInfo.ACTION_CLEAR_ACCESSIBILITY_FOCUS){accessibilityFocus=action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?id:View.NO_ID;
                AccessibilityEvent event=AccessibilityEvent.obtain(action==AccessibilityNodeInfo.ACTION_ACCESSIBILITY_FOCUS?AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUSED:AccessibilityEvent.TYPE_VIEW_ACCESSIBILITY_FOCUS_CLEARED);event.setSource(TouchControlsView.this,id);event.setPackageName(getContext().getPackageName());event.setClassName("android.widget.Button");if(getParent()!=null)getParent().requestSendAccessibilityEvent(TouchControlsView.this,event);invalidate();return true;}
            return false;}
    };}
}
