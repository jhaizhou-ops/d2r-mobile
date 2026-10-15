package org.d2r.binaryprobe;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityEvent;
import java.util.function.BooleanSupplier;

/** Android rendering/events only; hit testing and gestures belong to upstream C. */
public final class TouchControlsView extends View {
    private static native void nativeResize(float width,float height,float left,float right);
    private static native void nativeEnable(boolean active);
    private static native void nativeCancel();
    private static native int nativeEvent(int[] ids,float[] xs,float[] ys,boolean cancelled);
    private static native float[] nativeFrame();
    private static native void nativeActivate(int target);
    // Canonical Android touch-layer tokens, mirrored in DESIGN.md.
    static final int INK=0x8030241b, RIM=0xbba08359, TEXT=0xffe9d9ba, PRESSED=0xcc80623c;
    private static final String[] LABELS={"主技能","技能1","技能2","技能3","技能4","回城","药水1","药水2","药水3","药水4","菜单","地图","切换技能组"};
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final BooleanSupplier ready;
    private float[] frame;
    private boolean active;
    private int accessibilityFocus=View.NO_ID;
    private final float density;
    public TouchControlsView(Context context,BooleanSupplier ready){super(context);this.ready=ready;density=getResources().getDisplayMetrics().density;
        setContentDescription("触摸操控：左侧摇杆移动，右侧技能按钮拖动瞄准，底部药水");setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);setVisibility(GONE);}
    public void setActive(boolean on){active=on;nativeEnable(on);setVisibility(on?VISIBLE:GONE);if(on){resize();postInvalidateOnAnimation();}}
    public void cancel(){nativeCancel();}
    private void resize(){if(getWidth()==0||getHeight()==0)return;float left=0,right=0;WindowInsets insets=getRootWindowInsets();
        if(insets!=null&&insets.getDisplayCutout()!=null){left=insets.getDisplayCutout().getSafeInsetLeft()/density;right=insets.getDisplayCutout().getSafeInsetRight()/density;}
        nativeResize(getWidth()/density,getHeight()/density,left,right);frame=nativeFrame();}
    @Override protected void onSizeChanged(int w,int h,int ow,int oh){super.onSizeChanged(w,h,ow,oh);resize();}
    private void circle(Canvas canvas,float x,float y,float radius,String label,boolean pressed){if(radius<=0)return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(pressed?PRESSED:INK);canvas.drawCircle(x,y,radius,paint);
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(1.4f);paint.setColor(RIM);canvas.drawCircle(x,y,radius,paint);
        if(label!=null){paint.setStyle(Paint.Style.FILL);paint.setColor(TEXT);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(Math.min(14,radius*.5f));canvas.drawText(label,x,y-(paint.ascent()+paint.descent())/2,paint);}}
    @Override protected void onDraw(Canvas canvas){super.onDraw(canvas);if(!active)return;frame=nativeFrame();if(frame==null||frame.length!=58)return;
        canvas.save();canvas.scale(density,density);
        for(int i=0;i<12;i++)circle(canvas,frame[i*3],frame[i*3+1],frame[i*3+2],LABELS[i],frame[44+i]>0);
        circle(canvas,frame[36],frame[37],frame[38],null,frame[57]>0);circle(canvas,frame[42],frame[43],frame[38]*.38f,null,frame[57]>0);
        circle(canvas,frame[39],frame[40],frame[41],frame[56]>0?"Ⅱ":"Ⅰ",false);
        if(accessibilityFocus!=View.NO_ID){float x,y,r;if(accessibilityFocus<12){x=frame[accessibilityFocus*3];y=frame[accessibilityFocus*3+1];r=frame[accessibilityFocus*3+2];}
            else if(accessibilityFocus==12){x=frame[39];y=frame[40];r=frame[41];}else{x=frame[36];y=frame[37];r=frame[38];}
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(3);paint.setColor(TEXT);canvas.drawCircle(x,y,r+3,paint);}
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
            else if(id==12){x=frame[39];y=frame[40];r=frame[41];label=LABELS[12];}
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
