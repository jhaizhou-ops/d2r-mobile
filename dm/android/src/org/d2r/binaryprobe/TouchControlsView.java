package org.d2r.binaryprobe;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PorterDuff;
import android.graphics.PorterDuffXfermode;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;
import android.view.accessibility.AccessibilityEvent;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.HashMap;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;

/** Android rendering/events only; hit testing and gestures belong to upstream C.
 *  The visual language mirrors the iOS overlay (dm/ios/Sources/DmTouchOverlay.m):
 *  dark disc + bronze double ring, action buttons fade while idle, and the
 *  joystick is a faint ring with a bronze sparkle knob and a ±30° aim arc. */
public final class TouchControlsView extends View {
    private static native void nativeResize(float width,float height,float left,float right,float bottom);   // bottom = 手势条安全边距：技能弧整体上抬这么多（不然主技能钮压在手势条下不好按）
    private static native String nativeUiText(String key);   // 串表（dm/i18n/ui_text.tsv）：界面文案跟游戏语言（英文版进英文）
    private static native void nativeEnable(boolean active);
    private static native void nativeCancel();
    private static native int nativeEvent(int[] ids,float[] xs,float[] ys,boolean cancelled,float liftNx,float liftNy);   // 抬起点：抬起那根手指的位置（拖放落点按松手这一刻问，前端面板页同 iOS）
    private static native float[] nativeFrame();
    private static native void nativeActivate(int target);
    static native int nativeFrontend();   // 1 = 局外（前端页面：主菜单/选角/加载）——覆盖层隐藏 | 1 = front-end page: overlay hidden
    private static native float[] nativeHud();
    private static native float[] nativeSkillIcons();   // P7 技能钮图标：每钮 14 个数 {status,atlas,帧常态,帧按住,常态x,y,w,h,按住x,y,w,h,换武器组位,充能}   // P3 底栏 v4 数据：{valid,hp,hp_max,mp,mp_max,level,exp‰,belt0..3,players,quest,points,dead,portal,panel_mode,frontend}
    // Canonical Android touch-layer tokens, mirrored in DESIGN.md and in the iOS overlay palette.
    static final int INK=0xff16110c, RIM=0xff997845, RIM_DARK=0xff1f170d, TEXT=0xffd9c9a6;
    private static final int KNOB_RING=0xffb08e4e, STAR=0xffc8a460, STAR_CORE=0xffecd696;
    private static final float IDLE=0.6f;   // iOS DM_SKILL_IDLE_ALPHA: idle fade of action-button base and rings
    private String[] labels(){String[] l=new String[12];
        l[0]=nativeUiText("dmTcMain");for(int i=1;i<=4;i++)l[i]=String.format(nativeUiText("dmTcSkill"),i);
        for(int i=6;i<=9;i++)l[i]=Integer.toString(i-5);l[11]=nativeUiText("dmTcMap");for(int i=5;i<12;i++)if(l[i]==null)l[i]="";
        return l;}
    private final Paint paint=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path star=new Path();
    private final BooleanSupplier ready;
    private float[] frame;
    private boolean active;
    private int accessibilityFocus=View.NO_ID;
    private final float density;
    // ===== P3 底栏 v4（一比一照 iOS DmTouchOverlay.m drawV4：底图原生分辨率直贴、腰带格挖空透游戏药水、球心挖空透球液）=====
    private final Paint v4paint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final RectF v4dst=new RectF();
    private final HashMap<String,Bitmap> v4imgs=new HashMap<>();
    private JSONObject v4cache;private boolean v4tried;private boolean v4on;
    // ===== P7 技能钮图标：原版图集（剪影优先，iOS skillImage 同一口径）=====
    private final Bitmap[] skillAtlas=new Bitmap[9],skillSil=new Bitmap[9];
    private final HashMap<String,Bitmap> cropCache=new HashMap<>();
    private float[] icons;
    private Bitmap atlas(int i,boolean sil){
        Bitmap[] arr=sil?skillSil:skillAtlas;
        if(arr[i]!=null||cropCache.containsKey((sil?"s":"n")+i))return arr[i];
        try(InputStream in=getContext().getAssets().open((sil?"hud_v4_ui/sil_":"")+"dm_native_skill_"+i+".png")){arr[i]=BitmapFactory.decodeStream(in);}catch(Exception e){arr[i]=null;}
        return arr[i];
    }
    private Bitmap crop(int atlasIdx,int frame,float x,float y,float w,float h,boolean silver){
        String key=(silver?"s":"n")+atlasIdx+"_"+frame;
        Bitmap c=cropCache.get(key);if(c!=null)return c;
        Bitmap src=atlas(atlasIdx,silver);if(src==null)src=atlas(atlasIdx,!silver);
        if(src==null||w<=0||h<=0)return null;
        if(x+w>src.getWidth()||y+h>src.getHeight())return null;
        c=Bitmap.createBitmap(src,(int)x,(int)y,(int)w,(int)h);cropCache.put(key,c);return c;
    }
    /** 技能钮图标：先剪影后原图；充能用完（kind&2 且余量<=0）画成灰色（iOS 用 luminosity 混合，安卓用灰底+半透明近似）。 */
    private Bitmap skillIcon(int idx,boolean held,boolean[]spentOut){
        if(icons==null||icons.length<84)return null;
        int base=idx*14,status=(int)icons[base];if(status!=2)return null;
        int a=(int)icons[base+1];if(a<0||a>8)return null;
        float[] r=held?new float[]{icons[base+8],icons[base+9],icons[base+10],icons[base+11]}
                     :new float[]{icons[base+4],icons[base+5],icons[base+6],icons[base+7]};
        int kind=(int)icons[base+12],charges=(int)icons[base+13];
        if(spentOut!=null)spentOut[0]=((kind&2)!=0)&&charges<=0;
        return crop(a,held?(int)icons[base+3]:(int)icons[base+2],r[0],r[1],r[2],r[3],true);
    }   // 底栏 v4 是否在屏（腰带四格 + 十字由底栏画，这几格只留命中区 = iOS 口径）
    public TouchControlsView(Context context,BooleanSupplier ready){super(context);this.ready=ready;density=getResources().getDisplayMetrics().density;
        paint.setTypeface(Typeface.SERIF);
        setContentDescription("触摸操控：左侧摇杆移动，右侧技能按钮拖动瞄准，底部药水");setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);setVisibility(GONE);}
    public void setActive(boolean on){active=on;nativeEnable(on);setVisibility(on?VISIBLE:GONE);if(on){resize();postInvalidateOnAnimation();}}
    public void cancel(){nativeCancel();}
    private void resize(){if(getWidth()==0||getHeight()==0)return;float left=0,right=0;WindowInsets insets=getRootWindowInsets();
        if(insets!=null&&insets.getDisplayCutout()!=null){left=insets.getDisplayCutout().getSafeInsetLeft()/density;right=insets.getDisplayCutout().getSafeInsetRight()/density;}
        float bottom=0;
        if(insets!=null&&insets.getStableInsetBottom()>0)bottom=insets.getStableInsetBottom()/density;
        nativeResize(getWidth()/density,getHeight()/density,left,right,bottom);frame=nativeFrame();}
    @Override protected void onSizeChanged(int w,int h,int ow,int oh){super.onSizeChanged(w,h,ow,oh);resize();}
    private void disc(Canvas c,float x,float y,float r,int color,int alpha){if(r<=0)return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(color);paint.setAlpha(alpha);c.drawCircle(x,y,r,paint);}
    private void ring(Canvas c,float x,float y,float radius,float width,int color,int alpha){if(radius<=0)return;
        paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(width);paint.setColor(color);paint.setAlpha(alpha);c.drawCircle(x,y,radius,paint);}
    private void caption(Canvas c,float x,float y,float size,String text,int alpha){if(text==null||text.isEmpty()||size<=0)return;
        paint.setStyle(Paint.Style.FILL);paint.setColor(TEXT);paint.setAlpha(alpha);paint.setTextAlign(Paint.Align.CENTER);paint.setTextSize(size);
        c.drawText(text,x,y-(paint.ascent()+paint.descent())/2,paint);}
    // ===== 新触摸外观（一比一照 iOS DmTouchControlArt.h：薄边嵌槽 + 浮动摇杆；drawButton / drawJoystickPart 的取图与缓存同口径）=====
    // ===== New touch look: 1:1 port of iOS DmTouchControlArt.h (thin sockets + floating stick), same art caching as drawButton / drawJoystickPart =====
    private interface ArtPainter{void draw(Canvas c,float size);}
    private final HashMap<String,Bitmap> artCache=new HashMap<>();        // 静态底图（iOS _buttonFrames）
    private final HashMap<String,Bitmap> iconShadow=new HashMap<>();      // 带阴影的技能图标（iOS 在绘制时加 shadow）
    private final HashMap<String,Bitmap> lblCache=new HashMap<>();
    private final Paint artPaint=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
    private final Paint artFill=new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF artRect=new RectF();
    private Bitmap socketSrc,circleSrc,mapSrc,menuSrc;   // dm_mobile_socket / dm_native_circle / dm_native_map / dm_native_menu
    private boolean artTried;
    private static final ColorMatrixColorFilter DESAT=new ColorMatrixColorFilter(new ColorMatrix(new float[]{
        .33f,.33f,.33f,0,0, .33f,.33f,.33f,0,0, .33f,.33f,.33f,0,0, 0,0,0,1,0}));   // 充能用完的灰图标（iOS luminosity 混合的近似） | gray icon for spent charges (approximation of the iOS luminosity blend)
    private static int cl(double v){return (int)Math.round(Math.max(0,Math.min(1,v))*255);}
    private static int argb(double r,double g,double b,double a){return (cl(a)<<24)|(cl(r)<<16)|(cl(g)<<8)|cl(b);}
    private static int gray(double v,double a){int V=cl(v);return (cl(a)<<24)|(V<<16)|(V<<8)|V;}
    private Bitmap assetPng(String name){try(InputStream in=getContext().getAssets().open(name)){return BitmapFactory.decodeStream(in);}catch(Exception e){return null;}}
    private void loadArt(){if(artTried)return;artTried=true;
        socketSrc=assetPng("dm_mobile_socket.png");circleSrc=assetPng("dm_native_circle.png");
        mapSrc=assetPng("dm_native_map.png");menuSrc=assetPng("dm_native_menu.png");}
    private Bitmap lblArt(String name){Bitmap b=lblCache.get(name);if(b!=null)return b;
        b=assetPng("hud_v4_ui/"+name);if(b!=null)lblCache.put(name,b);return b;}
    /** 缓存静态底图（iOS controlArt 同口径）：按 dp 尺寸 × density 渲一张，之后每帧只贴图。 | Cache static control art like iOS controlArt: render once at size × density, then only blit. */
    private Bitmap controlArt(String key,float size,ArtPainter p){
        String k=key+":"+Math.round(size*density);
        Bitmap b=artCache.get(k);if(b!=null)return b;
        int px=Math.max(1,Math.round(size*density));
        try{b=Bitmap.createBitmap(px,px,Bitmap.Config.ARGB_8888);}catch(Throwable e){return null;}
        Canvas c=new Canvas(b);c.scale(px/size,px/size);p.draw(c,size);
        if(artCache.size()>=64)artCache.clear();   // 限制旋转 / 窗口缩放产生的缓存数量（同 iOS 64） | bound the cache count (64, same as iOS)
        artCache.put(k,b);return b;
    }
    /** 只中和内槽的色相；裁切区域避开金属外圈（dm_socket_neutralize）。Android 没有 SATURATION 混合模式，改成「去色副本 + 椭圆遮罩」再贴回来。 | Neutralize only the socket interior; the clip excludes the metal rim (dm_socket_neutralize). Android has no SATURATION blend mode, so: a desaturated copy masked to the inner ellipse, composited back. */
    private Bitmap socketNeutral(float s){
        if(socketSrc==null)return null;
        String k="sn:"+Math.round(s*density);
        Bitmap b=artCache.get(k);if(b!=null)return b;
        int px=Math.max(1,Math.round(s*density));
        try{b=Bitmap.createBitmap(px,px,Bitmap.Config.ARGB_8888);}catch(Throwable e){return null;}
        Canvas c=new Canvas(b);float sc=px/s;c.scale(sc,sc);
        Paint p=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);p.setColorFilter(DESAT);
        artRect.set(0,0,s,s);c.drawBitmap(socketSrc,null,artRect,p);
        float in=s*.08f;Path clip=new Path();clip.addOval(in,in,s-in,s-in,Path.Direction.CW);
        Paint m=new Paint(Paint.ANTI_ALIAS_FLAG);m.setStyle(Paint.Style.FILL);m.setColor(0xff000000);
        m.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.DST_IN));c.drawPath(clip,m);
        if(artCache.size()>=64)artCache.clear();artCache.put(k,b);return b;
    }
    private void socketDraw(Canvas c,float s,boolean held){
        artRect.set(0,0,s,s);artPaint.setStyle(Paint.Style.FILL);artPaint.setAlpha(255);c.drawBitmap(socketSrc,null,artRect,artPaint);
        Bitmap n=socketNeutral(s);if(n!=null)c.drawBitmap(n,null,artRect,artPaint);
        if(held){artPaint.setStyle(Paint.Style.STROKE);artPaint.setColor(gray(.64,.72));artPaint.setStrokeWidth(s*.015f);
            c.drawOval(s*.052f,s*.052f,s-s*.052f,s-s*.052f,artPaint);}   // 按下态只提亮为银灰（dm_button_highlight） | held is a silver-gray highlight (dm_button_highlight)
    }
    /** dm_button_draw：窄铁边 / 黑灰内槽 / 空槽细银灰边 + 十字。 | dm_button_draw: narrow iron rim, charcoal well, empty slots get a silver hairline + cross. */
    private void buttonDraw(Canvas c,float s,boolean held,boolean empty,boolean primary){
        float cx=s/2,cy=s/2,rim=s*.018f,face=s*.055f;
        artFill.setStyle(Paint.Style.FILL);artPaint.setStyle(Paint.Style.STROKE);
        artFill.setShader(new LinearGradient(0,rim,0,s-rim,
            new int[]{argb(.26,.28,.28,empty?.22:.9),argb(.055,.065,.07,empty?.22:.95),argb(.16,.11,.10,empty?.16:.7)},
            new float[]{0f,.5f,1f},Shader.TileMode.CLAMP));
        c.drawOval(rim,rim,s-rim,s-rim,artFill);
        artFill.setShader(null);
        artPaint.setColor(argb(.008,.012,.015,empty?.38:.9));artPaint.setStrokeWidth(s*.025f);c.drawOval(rim,rim,s-rim,s-rim,artPaint);
        artFill.setShader(new LinearGradient(0,face,0,s-face,
            new int[]{argb(.021,.021,.021,empty?.08:.94),argb(.050,.050,.050,empty?.16:.94)},new float[]{0f,1f},Shader.TileMode.CLAMP));
        c.drawOval(face,face,s-face,s-face,artFill);artFill.setShader(null);
        if(empty)artPaint.setColor(gray(held?.72:.62,held?.80:.60));
        else artPaint.setColor(argb(held?.64:.26,held?.64:.29,held?.64:.28,held?.72:.55));
        artPaint.setStrokeWidth(s*(held?.018f:(empty?.012f:.009f)));c.drawOval(face,face,s-face,s-face,artPaint);
        if(primary&&!empty){artPaint.setColor(argb(.31,.23,.22,.52));artPaint.setStrokeWidth(s*.012f);
            c.drawOval(s*.035f,s*.035f,s-s*.035f,s-s*.035f,artPaint);}   // 主技能一圈极淡暗红（让最大的钮与四个技能区分开） | faint dark-red ring on the main skill so the biggest button reads apart from the four
        if(empty){float arm=s*.055f;artPaint.setColor(argb(.60,.65,.64,held?.75:.25));artPaint.setStrokeWidth(s*.018f);artPaint.setStrokeCap(Paint.Cap.ROUND);
            c.drawLine(cx-arm,cy,cx+arm,cy,artPaint);c.drawLine(cx,cy-arm,cx,cy+arm,artPaint);artPaint.setStrokeCap(Paint.Cap.BUTT);}
    }
    private void drawButton(Canvas canvas,float x,float y,float r,boolean held,boolean empty,boolean primary){
        float side=r*2;
        Bitmap b=controlArt("btn:"+(held?1:0)+(empty?1:0)+(primary?1:0),side,(c,s)->{
            if(socketSrc!=null&&!empty)socketDraw(c,s,held);else buttonDraw(c,s,held,empty,primary);});
        if(b==null)return;
        artRect.set(x-r,y-r,x+r,y+r);artPaint.setStyle(Paint.Style.FILL);artPaint.setAlpha(255);canvas.drawBitmap(b,null,artRect,artPaint);
    }
    /* 摇杆可见性（Android 实测：iOS 那套 α 值在手机上几乎看不见，玩家反馈「非常透明、看不清」）。
       这里只抬高不透明度，几何与画法仍与 iOS DmTouchControlArt.h 逐项一致；闲时也保留一圈能看清的轮廓。
       Joystick visibility (measured on Android: the iOS alpha values are almost invisible on a phone, players reported "very transparent, cannot see it").
       Only opacity is raised; geometry and drawing still follow iOS DmTouchControlArt.h item by item, and even the idle state keeps a readable outline. */
    private void joystickBaseDraw(Canvas c,float s,boolean active){
        float cx=s/2,cy=s/2;
        artFill.setStyle(Paint.Style.FILL);
        artFill.setShader(new RadialGradient(cx,cy,s*.48f,
            new int[]{gray(.025,active?.20:.11),gray(.025,active?.34:.24),gray(.025,0)},new float[]{0f,.78f,1f},Shader.TileMode.CLAMP));
        c.drawCircle(cx,cy,s*.48f,artFill);artFill.setShader(null);
        /* 外圈一条实线：坐标「底盘在哪」一眼可辨（iOS 只有渐隐与刻纹，手机上太淡） | One solid outer ring so "where the base is" reads at a glance (iOS only has the fade and etchings, too faint on a phone) */
        artPaint.setStyle(Paint.Style.STROKE);artPaint.setStrokeWidth(s*.012f);
        artPaint.setColor(gray(.70,active?.40:.26));c.drawCircle(cx,cy,s*.455f,artPaint);
        artPaint.setStrokeWidth(s*.004f);artPaint.setStrokeCap(Paint.Cap.ROUND);
        RectF ring=new RectF(cx-s*.416f,cy-s*.416f,cx+s*.416f,cy+s*.416f);
        for(int i=0;i<4;i++){
            float a=(float)(i*Math.PI/2);
            artPaint.setColor(gray(.72,active?.30:.16));
            c.drawArc(ring,(float)Math.toDegrees(a+.20),(float)Math.toDegrees(Math.PI/2-.40),false,artPaint);
            c.save();c.translate(cx,cy);c.rotate((float)Math.toDegrees(a));
            float xx=s*.414f,kk=s*.028f;Path p=new Path();
            p.moveTo(xx+kk,0);p.lineTo(xx-kk,-kk*.65f);p.lineTo(xx-kk*.42f,0);p.lineTo(xx-kk,kk*.65f);p.close();
            artPaint.setColor(gray(.76,active?.45:.30));c.drawPath(p,artPaint);
            c.drawLine(xx-kk*.42f,0,xx-kk*2.1f,0,artPaint);
            c.restore();
        }
        artPaint.setStrokeCap(Paint.Cap.BUTT);
    }
    /** 低反光黑灰摇杆头（dm_joystick_thumb_draw）。 | Matte charcoal thumb (dm_joystick_thumb_draw). */
    private void joystickThumbDraw(Canvas c,float s,boolean active){
        float in=s*.055f,mid=s/2;
        artFill.setStyle(Paint.Style.FILL);
        artFill.setShader(new LinearGradient(0,in,0,s-in,
            new int[]{gray(.23,active?.94:.72),gray(.14,active?.96:.76),gray(.065,active?.97:.80)},new float[]{0f,.5f,1f},Shader.TileMode.CLAMP));
        c.drawOval(in,in,s-in,s-in,artFill);artFill.setShader(null);
        artPaint.setStyle(Paint.Style.STROKE);
        artPaint.setColor(gray(.02,active?.90:.62));artPaint.setStrokeWidth(s*.035f);c.drawOval(in,in,s-in,s-in,artPaint);
        /* 亮边压在暗盘上：草地上/火光旁都能看出摇杆头推到哪 | A bright rim over the dark disc so the thumb position reads on grass or next to firelight */
        artPaint.setColor(gray(.86,active?.85:.55));artPaint.setStrokeWidth(s*.016f);
        c.drawArc(new RectF(in,in,s-in,s-in),(float)Math.toDegrees(Math.PI*1.02),(float)Math.toDegrees(Math.PI*.96),false,artPaint);
        RectF r1=new RectF(mid-s*.437f,mid-s*.437f,mid+s*.437f,mid+s*.437f);
        artPaint.setColor(gray(.70,active?.55:.34));artPaint.setStrokeWidth(s*.014f);
        c.drawArc(r1,(float)Math.toDegrees(Math.PI*1.07),(float)Math.toDegrees(Math.PI*.84),false,artPaint);
        RectF r2=new RectF(mid-s*.410f,mid-s*.410f,mid+s*.410f,mid+s*.410f);
        artPaint.setColor(gray(.48,active?.30:.16));artPaint.setStrokeWidth(s*.012f);
        c.drawArc(r2,(float)Math.toDegrees(Math.PI*1.12),(float)Math.toDegrees(Math.PI*.76),false,artPaint);
    }
    /** 静态底图缓存键要带「按下态」：iOS 的 key 是 stick:base:active，这里原先漏了 active，按下时一直拿闲时那张（最淡）| The static-art cache key must include the pressed state: the iOS key is stick:base:active, this one used to drop active and always reused the faint idle image while pushed. */
    private void drawStickPart(Canvas canvas,float x,float y,float side,boolean base,boolean active){
        final boolean act=active;
        Bitmap b=controlArt((base?"stick-base":"stick-thumb")+(act?":down":":idle"),side,(c,s)->{if(base)joystickBaseDraw(c,s,act);else joystickThumbDraw(c,s,act);});
        if(b==null)return;
        artRect.set(x-side/2,y-side/2,x+side/2,y+side/2);artPaint.setStyle(Paint.Style.FILL);artPaint.setAlpha(255);canvas.drawBitmap(b,null,artRect,artPaint);
    }
    /** 技能图标加柔和外发光（iOS CGContextSetShadowWithColor 同口径），渲进离屏位图再贴。 | Skill icon with a soft glow, same as the iOS shadow; rendered offscreen and then blitted. */
    private Bitmap shadowedIcon(Bitmap icon,float radiusDp,boolean held){
        if(icon==null)return null;
        String key=System.identityHashCode(icon)+":"+Math.round(radiusDp*density);
        Bitmap b=iconShadow.get(key);if(b!=null)return b;
        float sh=radiusDp*density,pad=sh*2f;
        int w=icon.getWidth(),h=icon.getHeight();
        try{b=Bitmap.createBitmap(w+(int)(pad*2),h+(int)(pad*2),Bitmap.Config.ARGB_8888);}catch(Throwable e){return icon;}
        Canvas c=new Canvas(b);Paint p=new Paint(Paint.ANTI_ALIAS_FLAG|Paint.FILTER_BITMAP_FLAG);
        p.setShadowLayer(sh,0,0,argb(.65,.65,.65,.32));c.drawBitmap(icon,pad,pad,p);
        if(iconShadow.size()>=64)iconShadow.clear();iconShadow.put(key,b);return b;
    }
    /** 右上角「Ⅱ」标记：技能在另一组武器上，按下先换武器（iOS 同一处、同一套数值） | Top-right "Ⅱ" badge: the skill is on the other weapon set, pressing swaps first (same place and numbers as iOS) */
    private void badgeMark(Canvas c,float x,float y,float br,String mark){
        float lw=Math.max(1,br*.14f);
        disc(c,x,y,br,argb(.11,.094,.078,.95),255);
        ring(c,x,y,br-lw/2,lw,RIM,255);
        caption(c,x,y,br*1.15f,mark,255);
    }
    /** 右下角剩余次数：字形走底栏 v4 的角标表（ui.json badge） */
    private void badgeCount(Canvas c,float x,float y,int n){
        JSONObject ui=v4ui();if(ui==null||n<0)return;
        try{JSONObject bd=ui.getJSONObject("badge");float u=(getHeight()/density)/1320f;if(u<=0)return;
            v4text(c,Integer.toString(n),bd.getJSONObject("glyphs"),(x-getWidth()/density)/u+1434,y/u,1,getWidth()/density,u);
        }catch(Exception ignored){}
    }
    /** 非技能格（菜单 / 地图）的圆底：有素材贴 dm_native_circle，没有就退回落款（iOS _circle 同口径）。 | Round base for non-skill cells (menu / map): blit dm_native_circle when present, otherwise fall back (same as iOS _circle). */
    private void circleCell(Canvas c,float x,float y,float r,boolean pressed){
        if(circleSrc!=null){artRect.set(x-r,y-r,x+r,y+r);artPaint.setStyle(Paint.Style.FILL);
            artPaint.setAlpha(cl(pressed?.6:.28));c.drawBitmap(circleSrc,null,artRect,artPaint);artPaint.setAlpha(255);return;}
        artFill.setStyle(Paint.Style.FILL);artFill.setShader(null);artFill.setColor(argb(0,0,0,.16));c.drawCircle(x,y,r,artFill);
        artPaint.setStyle(Paint.Style.STROKE);artPaint.setColor(argb(.28,.23,.14,1));artPaint.setStrokeWidth(1);c.drawCircle(x,y,r,artPaint);
    }
    private void fixedIcon(Canvas c,Bitmap atlas,float x,float y,float box,boolean held,int alpha){
        if(atlas==null||box<=0)return;
        String key=System.identityHashCode(atlas)+":"+(held?1:0);
        Bitmap frame=iconShadow.get("fx"+key);
        if(frame==null){
            int fx=held?36:0;if(fx+36>atlas.getWidth()||36>atlas.getHeight())return;
            try{frame=Bitmap.createBitmap(atlas,fx,0,36,36);}catch(Throwable e){return;}
            if(iconShadow.size()>=96)iconShadow.clear();
            iconShadow.put("fx"+key,frame);
        }
        artRect.set(x-box/2,y-box/2,x+box/2,y+box/2);artPaint.setStyle(Paint.Style.FILL);artPaint.setAlpha(alpha);
        c.drawBitmap(frame,null,artRect,artPaint);artPaint.setAlpha(255);
    }
    private float X(float px,float W,float u){return (px-1434)*u+W/2;}
    private JSONObject v4ui(){if(v4tried)return v4cache;v4tried=true;
        try(InputStream in=getContext().getAssets().open("hud_v4_ui/ui.json")){
            ByteArrayOutputStream bo=new ByteArrayOutputStream();byte[] b=new byte[8192];int n;
            while((n=in.read(b))>0)bo.write(b,0,n);
            v4cache=new JSONObject(new String(bo.toByteArray(),"UTF-8"));}
        catch(Exception e){v4cache=null;android.util.Log.w("dm_v4","ui.json 读不到",e);}   // 没有素材就整块不画（不崩、不挡画面）
        return v4cache;}
    private Bitmap v4img(String name){if(v4imgs.containsKey(name))return v4imgs.get(name);
        Bitmap b=null;
        try(InputStream in=getContext().getAssets().open("hud_v4_ui/"+name)){b=BitmapFactory.decodeStream(in);}catch(Exception e){b=null;}
        v4imgs.put(name,b);return b;}
    private static int rgb(JSONArray a)throws org.json.JSONException{return 0xff000000|((int)a.getDouble(0)<<16)|((int)a.getDouble(1)<<8)|(int)a.getDouble(2);}
    // 预渲染字形拼字（iOS v4text 同口径：adv 累加排布、缺字跳过不占宽、align 0=居中 1=右对齐）
    private void v4text(Canvas c,String s,JSONObject glyphs,float x,float baseline,int align,float W,float u){
        float adv=0;for(int i=0;i<s.length();i++){JSONObject g=glyphs.optJSONObject(s.substring(i,i+1));if(g!=null)adv+=(float)g.optDouble("adv",0);}
        float pen=align==1?x-adv:x-adv/2;
        for(int i=0;i<s.length();i++){JSONObject g=glyphs.optJSONObject(s.substring(i,i+1));if(g==null)continue;
            Bitmap b=v4img(g.optString("file"));
            if(b!=null){float gx=(pen+(float)g.optDouble("dx",0)-1434)*u+W/2,gy=(baseline+(float)g.optDouble("dy",0))*u;
                v4dst.set(gx,gy,gx+b.getWidth()*u,gy+b.getHeight()*u);v4paint.setAlpha(255);c.drawBitmap(b,null,v4dst,v4paint);}
            pen+=(float)g.optDouble("adv",0);}}
    private void drawV4(Canvas c){
        JSONObject ui=v4ui();if(ui==null)return;
        float[] hud=nativeHud();if(hud==null||hud.length<18)return;
        boolean valid=hud[0]>0,dead=hud[14]>0,panel=hud[16]>0,front=hud[17]>0;
        if(!valid||dead||panel||front){v4on=false;return;}   // 只在局内常态显示（面板/死亡/前端让开）
        v4on=true;
        float W=getWidth()/density;if(W<=0)return;
        float u=(getHeight()/density)/1320f;if(u<=0)return;
        try{
            JSONObject bo=ui.getJSONObject("base");Bitmap full=v4img("base_full.png");
            if(full!=null){float bx=X((float)bo.getDouble("x"),W,u),by=(float)bo.getDouble("y")*u;
                v4dst.set(bx,by,bx+full.getWidth()*u,by+full.getHeight()*u);v4paint.setAlpha(255);c.drawBitmap(full,null,v4dst,v4paint);}
            JSONArray slots=ui.getJSONArray("slots"),s6=slots.getJSONArray(5);
            boolean held=frame!=null&&frame[44+10]>0;
            boolean alert=hud[13]>0||hud[12]>0;
            Bitmap btn=v4img(alert?(held?"lvl_1.png":"lvl_0.png"):(held?"lvl_d1.png":"lvl_d0.png"));
            float x5=(float)s6.getDouble(0),y5=(float)s6.getDouble(1);
            float w5=((float)s6.getDouble(2)-x5)*u,h5=((float)s6.getDouble(3)-y5)*u;
            float cx5=X(x5,W,u),cy5=y5*u;
            JSONArray in5=ui.getJSONArray("inner");   // 十字格内区先压不透明底（lvl_*.png 自身半透明，会让游戏层那行标签透上来；iOS 那边是靠藏控件）
            paint.setStyle(Paint.Style.FILL);paint.setColor(0xff2d261a);paint.setAlpha(255);
            c.drawRect(cx5+(float)in5.getDouble(0)*u,cy5+(float)in5.getDouble(1)*u,
                cx5+(float)in5.getDouble(2)*u,cy5+(float)in5.getDouble(3)*u,paint);
            if(btn!=null){if(hud[13]>0){paint.setStyle(Paint.Style.STROKE);paint.setColor(0xffffd15a);   // 有点可加：金色外闪光（iOS shadow 10u 的近似）
                    for(int k=3;k>=1;k--){paint.setStrokeWidth(2.6f*u*k);paint.setAlpha(16+10*(3-k));c.drawRoundRect(cx5-2*u,cy5-2*u,cx5+w5+2*u,cy5+h5+2*u,8*u,8*u,paint);}}
                v4dst.set(cx5,cy5,cx5+w5,cy5+h5);v4paint.setAlpha(255);c.drawBitmap(btn,null,v4dst,v4paint);}
            JSONObject e=ui.getJSONObject("exp");
            float x0=(float)e.getDouble("x0"),x1=(float)e.getDouble("x1"),ey0=(float)e.getDouble("y"),eh=(float)e.getDouble("h");
            int track=rgb(e.getJSONArray("track")),edge=rgb(e.getJSONArray("edge")),fill=rgb(e.getJSONArray("fill"));
            paint.setStyle(Paint.Style.FILL);paint.setColor(track);paint.setAlpha(255);
            c.drawRect(X(x0,W,u),ey0*u,X(x1,W,u),(ey0+eh)*u,paint);
            paint.setStyle(Paint.Style.STROKE);paint.setStrokeWidth(u);paint.setColor(edge);
            c.drawRect(X(x0,W,u)+u/2,ey0*u+u/2,X(x1,W,u)-u/2,(ey0+eh)*u-u/2,paint);
            paint.setStyle(Paint.Style.FILL);paint.setColor(fill);
            float perm=Math.max(0,Math.min(1000,hud[6]));
            c.drawRect(X(x0+1,W,u),(ey0+1)*u,X(x0+1,W,u)+(x1-x0-2)*u*perm/1000f,(ey0+eh-1)*u,paint);
            int ticks=(int)e.optDouble("ticks",20);paint.setColor(track);
            for(int t=1;t<ticks;t++){float tx=X(x0+Math.round((x1-x0)*t/(double)ticks),W,u);c.drawRect(tx,(ey0+1)*u,tx+u,(ey0+eh-1)*u,paint);}
            JSONObject vt=ui.getJSONObject("vital"),vg=vt.getJSONObject("glyphs");
            v4text(c,String.format(java.util.Locale.ROOT,nativeUiText("dmHudLife"),(int)hud[1],(int)hud[2]),vg,(float)vt.getDouble("left_x"),(float)vt.getDouble("baseline"),0,W,u);
            v4text(c,String.format(java.util.Locale.ROOT,nativeUiText("dmHudMana"),(int)hud[3],(int)hud[4]),vg,(float)vt.getDouble("right_x"),(float)vt.getDouble("baseline"),0,W,u);
            JSONObject lv=ui.getJSONObject("lv");
            v4text(c,String.format(java.util.Locale.ROOT,"Lv %d · %d%%",(int)hud[5],(int)(hud[6]/10)),lv.getJSONObject("glyphs"),(float)lv.getDouble("x"),(float)lv.getDouble("baseline"),0,W,u);
            JSONObject bd=ui.getJSONObject("badge");JSONArray in=ui.getJSONArray("inner");
            float inx=(float)in.getDouble(2),iny=(float)in.getDouble(3);
            for(int i=1;i<=4;i++){int n=(int)hud[6+i];if(n<=0)continue;
                float bx=(float)slots.getJSONArray(i).getDouble(0)+inx+(float)bd.getDouble("dx");
                float by=(float)slots.getJSONArray(i).getDouble(1)+iny+(float)bd.getDouble("dy");
                v4text(c,Integer.toString(n),bd.getJSONObject("glyphs"),bx,by,1,W,u);}
            int players=(int)hud[11];
            if(players>=1&&players<=8){Bitmap d=v4img("diff_"+players+".png");
                float icx=((float)in.getDouble(0)+inx+1)/2,icy=((float)in.getDouble(1)+iny+1)/2;
                float dcx=(float)slots.getJSONArray(0).getDouble(0)+icx,dcy=(float)slots.getJSONArray(0).getDouble(1)+icy;
                if(d!=null){float dx=X(dcx,W,u),dy=dcy*u;v4dst.set(dx-d.getWidth()*u/2,dy-d.getHeight()*u/2,dx+d.getWidth()*u/2,dy+d.getHeight()*u/2);v4paint.setAlpha(255);c.drawBitmap(d,null,v4dst,v4paint);}}
        }catch(Exception ignored){}
    }
    @Override protected void onDraw(Canvas canvas){super.onDraw(canvas);if(!active)return;frame=nativeFrame();if(frame==null||frame.length!=58)return;
        canvas.save();canvas.scale(density,density);
        loadArt();
        drawV4(canvas);   // 底栏 v4：垫在圆钮之下
        icons=nativeSkillIcons();
        int set=(int)frame[56];
        for(int i=0;i<12;i++){
            if(i>5&&i<=10)continue;   /* 腰带 1-4 + 十字：由底栏 v4 画，这几格只留命中区（iOS 同样无条件跳过） | belt 1-4 + cross: drawn by the v4 bar, hit area only (iOS skips them unconditionally too) */
            float x=frame[i*3],y=frame[i*3+1],r=frame[i*3+2];if(r<=0)continue;boolean held=frame[44+i]>0;
            boolean skillKey=i<=5;Bitmap icon=null;boolean showCaption=true,spent=false;int ssKind=0,ssCharges=0;
            if(skillKey){
                if(i==5)icon=skillIcon(5,held,null);   // 回城钮：固定回城技能图（iOS dm_ta_skill(359)）
                else{boolean[] sp=new boolean[1];icon=skillIcon(i,held,sp);spent=sp[0];
                    if(icons!=null&&icons.length>=84){ssKind=(int)icons[i*14+12];ssCharges=(int)icons[i*14+13];}}
                /* iOS：showCaption = 状态不是「空位」且没有图；空位（状态 1）在任何一键上都画空槽，未知（状态 0）只有主键画空槽、其余退回 F 标签 | iOS: showCaption = status is not EMPTY and there is no icon; an EMPTY slot (status 1) draws the empty art on every key, an UNKNOWN one (status 0) only on the main key, the rest fall back to the F label */
                int st=(icons!=null&&icons.length>=84)?(int)icons[i*14]:0;
                showCaption=(st!=1)&&icon==null;
            }
            boolean emptySkill=skillKey&&icon==null&&(!showCaption||i==0);   // 空槽保留绑定入口（iOS emptySkill 同判定） | empty slot keeps the binding entry (same test as iOS emptySkill)
            if(skillKey)drawButton(canvas,x,y,r,held,emptySkill,i==0);
            else circleCell(canvas,x,y,r,held);
            if(emptySkill)continue;   /* 细边框与十字由底图绘制，这里不再画东西 | the thin rim and cross come from the base art; draw nothing more here */
            if(skillKey){
                if(icon!=null){
                    float box=r*2*.76f,iw=icon.getWidth(),ih=icon.getHeight(),fit=box/Math.max(iw,ih);
                    float gy=y;   // 有图标就不写字，图标居中（iOS bare） | with an icon there is no text and the icon is centered (iOS bare)
                    if(spent){   // 充能用完：灰底 + 去色图标（iOS .30 底 + luminosity .55 的近似） | charges used up: gray base + desaturated icon (approximation of iOS .30 base + luminosity .55)
                        disc(canvas,x,y,r*.84f,0xff4d4d4d,255);
                        artRect.set(x-iw*fit/2,gy-ih*fit/2,x+iw*fit/2,gy+ih*fit/2);
                        artPaint.setAlpha(140);artPaint.setColorFilter(DESAT);canvas.drawBitmap(icon,null,artRect,artPaint);
                        artPaint.setColorFilter(null);artPaint.setAlpha(255);
                        disc(canvas,x,y,r*.88f,0x00000000,90);
                    }else{
                        float sh=held?r*.10f:r*.055f,pad=sh*2f;   // iOS CGContextSetShadowWithColor 半径 | iOS shadow radius
                        Bitmap sh2=shadowedIcon(icon,sh,held);
                        artRect.set(x-iw*fit/2-pad,gy-ih*fit/2-pad,x+iw*fit/2+pad,gy+ih*fit/2+pad);
                        artPaint.setAlpha((int)(255*(held?1:.94f)));canvas.drawBitmap(sh2,null,artRect,artPaint);artPaint.setAlpha(255);
                    }
                    if((ssKind&1)!=0)badgeMark(canvas,x+r*.74f,y-r*.74f,r*.30f,"Ⅱ");            // 技能在另一组武器上 | skill lives on the other weapon set
                    if((ssKind&2)!=0&&!spent)badgeCount(canvas,x+r*.66f,y+r*.42f,ssCharges);   // 剩余充能（底栏角标同一套字形） | remaining charges (same glyphs as the v4 badge)
                    continue;
                }
                Bitmap lbl=lblArt(i==0?"lbl_main.png":String.format(java.util.Locale.ROOT,"lbl_f%d.png",i+(set>0?4:0)));
                if(lbl!=null){float lh=r*(i==0?.30f:.36f),lw_=lbl.getWidth()*lh/lbl.getHeight();
                    artRect.set(x-lw_/2,y+r*.50f-lh/2,x+lw_/2,y+r*.50f+lh/2);artPaint.setAlpha(255);
                    canvas.drawBitmap(lbl,null,artRect,artPaint);}
                continue;
            }
            /* 非技能格（地图键）：圆底 + 固定图标（36×36 两帧：常态 / 按下）。它在游戏态被掩码禁用、r=0，这里不会画 | non-skill cell (map key): round base + fixed icon (36x36, normal / held). It is masked out in game state (r=0), so this does not draw */
            Bitmap atlas=(i==11)?mapSrc:menuSrc;
            if(atlas!=null){fixedIcon(canvas,atlas,x,y,r*2*.62f,held,cl(held?1:.82));showCaption=false;}
            if(showCaption)caption(canvas,x,y,Math.max(8,r*.38f),labels()[i],(int)(255*(held?.95f:.68f)));
        }
        float sx=frame[39],sy=frame[40],sr=frame[41];
        if(sr>0){boolean sheld=frame[44+10]>0;drawButton(canvas,sx,sy,sr,sheld,false,false);
            float ts=sr*(12f/22f);caption(canvas,sx,sy+ts*.15f,ts,set>0?"Ⅱ":"Ⅰ",(int)(255*(sheld?1:.86f)));}   // iOS ts=12k、k=joy_radius/60、换组键半径 22k | iOS ts=12k with k=joy_radius/60 and a 22k swap radius
        float jx=frame[36],jy=frame[37],jr=frame[38];
        if(jr>0){boolean moving=frame[57]>0;
            drawStickPart(canvas,jx,jy,jr*2f,true,moving);                       // 浮动底盘：按着时跟手指起点（iOS 同口径） | floating base: follows the touch origin while held (same as iOS)
            drawStickPart(canvas,frame[42],frame[43],jr*46f/60f,false,moving);   // 摇杆头：iOS 46k = 摇杆半径 × 46/60 | thumb: iOS 46k = stick radius × 46/60
        }
        if(accessibilityFocus!=View.NO_ID){float x,y,r;if(accessibilityFocus<12){x=frame[accessibilityFocus*3];y=frame[accessibilityFocus*3+1];r=frame[accessibilityFocus*3+2];}
            else if(accessibilityFocus==12){x=frame[39];y=frame[40];r=frame[41];}else{x=frame[36];y=frame[37];r=frame[38];}
            ring(canvas,x,y,r+3,3,TEXT,255);}
        canvas.restore();
        if(ready.getAsBoolean())postInvalidateOnAnimation();}
    @Override public boolean onTouchEvent(MotionEvent event){if(!active||!ready.getAsBoolean())return true;
        int action=event.getActionMasked(),skip=action==MotionEvent.ACTION_POINTER_UP?event.getActionIndex():-1;
        int liftIndex=action==MotionEvent.ACTION_UP?event.getPointerCount()-1:skip;
        float liftNx=liftIndex>=0?event.getX(liftIndex)/getWidth():-1f, liftNy=liftIndex>=0?event.getY(liftIndex)/getHeight():-1f;
        int count=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)?0:event.getPointerCount()-(skip>=0?1:0);
        if(count>16){nativeCancel();return true;}
        int[] ids=new int[count];float[] xs=new float[count],ys=new float[count];int k=0;
        for(int i=0;i<event.getPointerCount()&&k<count;i++)if(i!=skip){ids[k]=event.getPointerId(i);xs[k]=Math.max(0,Math.min(.999999f,event.getX(i)/getWidth()));ys[k]=Math.max(0,Math.min(.999999f,event.getY(i)/getHeight()));k++;}
        nativeEvent(ids,xs,ys,action==MotionEvent.ACTION_CANCEL,liftNx,liftNy);postInvalidateOnAnimation();return true;}
    @Override public AccessibilityNodeProvider getAccessibilityNodeProvider(){return new AccessibilityNodeProvider(){
        @Override public AccessibilityNodeInfo createAccessibilityNodeInfo(int id){AccessibilityNodeInfo node=AccessibilityNodeInfo.obtain();node.setPackageName(getContext().getPackageName());node.setSource(TouchControlsView.this,id);
            if(id==View.NO_ID){onInitializeAccessibilityNodeInfo(node);if(frame!=null){for(int i=0;i<12;i++)if(frame[i*3+2]>0)node.addChild(TouchControlsView.this,i);node.addChild(TouchControlsView.this,12);for(int i=20;i<24;i++)node.addChild(TouchControlsView.this,i);}return node;}
            if(frame==null)return null;float x,y,r;String label;
            if(id>=0&&id<12){x=frame[id*3];y=frame[id*3+1];r=frame[id*3+2];label=labels()[id];}
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
