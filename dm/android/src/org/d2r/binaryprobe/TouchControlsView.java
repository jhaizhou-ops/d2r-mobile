package org.d2r.binaryprobe;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
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
    // Skill / main / set-switch buttons: dark base + bronze double ring, faded while idle and solid while held (iOS "少挡画面" idem).
    private void actionButton(Canvas c,float x,float y,float r,boolean pressed){float f=pressed?1:IDLE,lw=Math.max(1.5f,r*.075f);
        disc(c,x,y,r,INK,(int)(255*(pressed?.92f:.80f*f)));
        ring(c,x,y,r-lw/2,lw,RIM,(int)(255*(pressed?1:.90f*f)));
        ring(c,x,y,r-lw*1.3f,lw/2,RIM_DARK,(int)(255*(pressed?.90f:.90f*f)));}
    // Belt / menu / map cells: the game's own HUD already carries their artwork, so draw a fainter ring only.
    /** 右上角「Ⅱ」标记：技能在另一组武器上，按下先换武器（iOS 同一处） */
    private void badgeMark(Canvas c,float x,float y,String mark){
        float br=Math.max(6,getResources().getDisplayMetrics().density*0);br=14;
        disc(c,x,y,br,0xff1e1814,242);ring(c,x,y,br*.84f,Math.max(1,br*.14f),RIM,255);
        caption(c,x,y,br*1.15f,mark,255);
    }
    /** 右下角剩余次数：字形走底栏 v4 的角标表（ui.json badge） */
    private void badgeCount(Canvas c,float x,float y,int n){
        JSONObject ui=v4ui();if(ui==null||n<0)return;
        try{JSONObject bd=ui.getJSONObject("badge");float u=(getHeight()/density)/1320f;if(u<=0)return;
            v4text(c,Integer.toString(n),bd.getJSONObject("glyphs"),(x-getWidth()/density)/u+1434,y/u,1,getWidth()/density,u);
        }catch(Exception ignored){}
    }
    private void utilityButton(Canvas c,float x,float y,float r,boolean pressed){float lw=Math.max(1.2f,r*.06f);
        disc(c,x,y,r,INK,(int)(255*(pressed?.62f:.30f)));
        ring(c,x,y,r-lw/2,lw,RIM,(int)(255*(pressed?.68f:.34f)));}
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
        drawV4(canvas);   // 底栏 v4：垫在圆钮之下
        icons=nativeSkillIcons();
        for(int i=0;i<12;i++){if(i==11)continue;   // 地图块：只留命中区（右上角小地图那一块 = 开/关地图），不画圆钮与字
            if(v4on&&i>=6&&i<=10)continue;   // 腰带1-4 + 十字：底栏 v4 画，这几格只留命中区
            float x=frame[i*3],y=frame[i*3+1],r=frame[i*3+2];if(r<=0)continue;boolean pressed=frame[44+i]>0;
            if(i<=5){
                actionButton(canvas,x,y,r,pressed);
                boolean[] spent=new boolean[1];
                Bitmap icon=i==5?skillIcon(5,pressed,spent):skillIcon(i,pressed,spent);   // 5 = 回城钮（固定回城技能）
                if(icon!=null){
                    float size=r*2*.64f,iw=icon.getWidth(),ih=icon.getHeight(),fit=size/Math.max(iw,ih);
                    float gx=x-iw*fit/2,gy=y-(r*.12f)-ih*fit/2;
                    if(spent[0]){disc(canvas,x,y,r*.86f,0xff4d4d4d,255);
                        v4paint.setAlpha(140);v4dst.set(gx,gy,gx+iw*fit,gy+ih*fit);canvas.drawBitmap(icon,null,v4dst,v4paint);v4paint.setAlpha(255);
                        disc(canvas,x,y,r,0x00000000,90);}
                    else{v4paint.setAlpha((int)(255*(pressed?1:.92f)));v4dst.set(gx,gy,gx+iw*fit,gy+ih*fit);canvas.drawBitmap(icon,null,v4dst,v4paint);v4paint.setAlpha(255);}
                    int kind=i<5&&icons!=null&&icons.length>=70?(int)icons[i*14+12]:0,chg=i<5&&icons!=null&&icons.length>=70?(int)icons[i*14+13]:0;
                    if((kind&1)!=0)badgeMark(canvas,x+r*.74f,y-r*.74f,"Ⅱ");               // 技能在另一组武器上
                    if((kind&2)!=0&&!spent[0])badgeCount(canvas,x+r*.66f,y+r*.42f,chg);  // 剩余充能（角标字形与腰带件数同一套）
                    continue;   // 有图标不写字
                }
                caption(canvas,x,y,Math.max(11,r*(i==0?.30f:.40f)),labels()[i],(int)(255*(pressed?1:.84f)));}
            else if(i>=6&&i<=9){utilityButton(canvas,x,y,r,pressed);caption(canvas,x,y+r*.42f,Math.max(8,r*.40f),Integer.toString(i-5),(int)(255*(pressed?.95f:.68f)));}
            else{utilityButton(canvas,x,y,r,pressed);caption(canvas,x,y,Math.max(8,r*.38f),labels()[i],(int)(255*(pressed?.95f:.68f)));}}
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
