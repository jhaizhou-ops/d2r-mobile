package org.d2r.binaryprobe;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import org.json.JSONArray;
import org.json.JSONObject;

public final class GameActivity extends Activity implements SurfaceHolder.Callback {
    static { System.loadLibrary("d2r_mobile_android"); }
    private static native int nativeConfigure(String files, String locale,boolean touchRoute);
    private static native int nativeTouch(int[] ids,float[] xs,float[] ys,boolean cancelled);
    private static native int nativeSurface(Surface surface,int width,int height,float density,float hz,int focus);
    private static native int nativeStart();
    private static native String nativeStatus();
    private static native String nativeKeyboardRequest();
    private static native void nativeKeyboardResult(long id,boolean accepted,String text);
    private static native void nativeKeyboardReady(boolean ready);
    private static native int nativeActions(long buttons,int lx,int ly,int rx,int ry);   // 释放所有按住的键（开文本框/退到后台时清手）| release every held key (when the text keyboard opens / the app goes to background)
    private android.app.AlertDialog keyboardDialog;
    private long keyboardId;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final HandlerThread surfaceThread=new HandlerThread("game-surface");
    private Handler surfaceWorker;
    private boolean nativeLaunched, destroyed;
    private volatile boolean inputReady;
    private volatile int surfaceGeneration;
    private SurfaceView game;
    private TextView status;
    private TouchControlsView touchControls;
    private boolean prepared, configured, surfaceReady, launched, resumed, failed;
    private boolean overlayOn;   // 覆盖层当前是否在屏：局内一直在，局外/前端页面自动隐藏 = iOS 模型（不再有「菜单模式」这层） | overlay on screen: always in a run, auto-hidden on front-end pages = the iOS model (no separate "menu mode" any more)
    private int width,height;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        surfaceThread.start();surfaceWorker=new Handler(surfaceThread.getLooper());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN|View.SYSTEM_UI_FLAG_HIDE_NAVIGATION|
            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY|View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN|View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        FrameLayout root=new FrameLayout(this);root.setBackgroundColor(0xff0d0d0f);
        game=new SurfaceView(this);game.setContentDescription("游戏画面");game.getHolder().addCallback(this);
        game.setOnTouchListener((view,event) -> {
            if(!configured||!surfaceReady||!inputReady||width<=0||height<=0)return true;
            int action=event.getActionMasked(),skip=action==MotionEvent.ACTION_POINTER_UP?event.getActionIndex():-1;
            int count=(action==MotionEvent.ACTION_UP||action==MotionEvent.ACTION_CANCEL)?0:event.getPointerCount()-(skip>=0?1:0);
            if(count>16){nativeTouch(new int[0],new float[0],new float[0],true);return true;}
            int[] ids=new int[count];float[] xs=new float[count],ys=new float[count];int k=0;
            for(int i=0;i<event.getPointerCount()&&k<count;i++)if(i!=skip){
                ids[k]=event.getPointerId(i);xs[k]=Math.max(0,Math.min(0.999999f,event.getX(i)/width));
                ys[k]=Math.max(0,Math.min(0.999999f,event.getY(i)/height));k++;
            }
            int rc=nativeTouch(ids,xs,ys,action==MotionEvent.ACTION_CANCEL);
            if(rc!=0)fail("触摸输入不可用",new Exception("错误 "+rc));return true;
        });
        root.addView(game,new FrameLayout.LayoutParams(-1,-1));
        touchControls=new TouchControlsView(this,() -> inputReady&&resumed&&!failed);
        root.addView(touchControls,new FrameLayout.LayoutParams(-1,-1));
        status=new TextView(this);status.setTextColor(0xffe9d9ba);status.setTextSize(18);
        status.setGravity(Gravity.CENTER);status.setBackgroundColor(0xe60d0d0f);
        status.setPadding(dp(28),dp(18),dp(28),dp(18));status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        status.setText("正在准备游戏文件");root.addView(status,new FrameLayout.LayoutParams(-1,-1));setContentView(root);
        new Thread(() -> {
            try { prepareModules();ui.post(() -> {prepared=true;configureAndStart();}); }
            catch(Exception e){ui.post(() -> fail("游戏文件准备失败",e));}
        },"game-files").start();
    }
    private int dp(int value){return Math.round(value*getResources().getDisplayMetrics().density);}
    /* 覆盖层显隐：已就绪 && 局内（nativeFrontend()==0）——局外/前端页面自动让开，对齐 iOS（iOS 无模式切换，屏上也没有那组方向/确认/返回钮）。
     * Overlay visibility: input ready && in a run (nativeFrontend()==0); front-end pages hide it, like iOS (iOS has no mode switch and none of those arrow / confirm / back buttons). */
    private void syncOverlay(){
        if(touchControls==null)return;
        boolean want=inputReady&&!failed&&TouchControlsView.nativeFrontend()==0;
        if(want!=overlayOn){overlayOn=want;touchControls.setActive(want);}
    }
    private static String digest(File file)throws Exception {
        MessageDigest md=MessageDigest.getInstance("SHA-256");byte[] bytes=new byte[65536];
        try(InputStream in=new FileInputStream(file)){int n;while((n=in.read(bytes))!=-1)md.update(bytes,0,n);}
        StringBuilder text=new StringBuilder();for(byte b:md.digest())text.append(String.format("%02x",b&255));return text.toString();
    }
    /* TEST PATCH (not in dodola's tree): read from the app's EXTERNAL files dir, because this
       device's SELinux policy blocks run-as, so game data cannot be placed into internal storage.
       Renderer/host code paths are untouched; only the storage location changes. */
    private java.io.File filesBase(){
        java.io.File external=getExternalFilesDir(null);
        return external!=null?external:getFilesDir();
    }
    private void prepareModules()throws Exception {
        String json;
        try(InputStream in=getAssets().open("module-manifest.json")){
            java.io.ByteArrayOutputStream out=new java.io.ByteArrayOutputStream();byte[] b=new byte[4096];int n;
            while((n=in.read(b))!=-1)out.write(b,0,n);json=new String(out.toByteArray(),StandardCharsets.UTF_8);
        }
        JSONArray list=new JSONObject(json).getJSONArray("files");File directory=new File(filesBase(),"d2r-mobile/modules");
        if(!directory.isDirectory()&&!directory.mkdirs())throw new java.io.IOException("不能创建游戏目录");
        for(int i=0;i<list.length();i++){
            JSONObject entry=list.getJSONObject(i);String name=entry.getString("name"),hash=entry.getString("sha256");
            if(!name.matches("(main|sdk|subsdk0)\\.(patched\\.bin|fixups\\.tsv|gates\\.tsv)"))throw new java.io.IOException("模块清单无效");
            File target=new File(directory,name);
            if(target.isFile()&&target.length()==entry.getLong("bytes")&&digest(target).equals(hash))continue;
            File temporary=new File(directory,name+".new");
            try(InputStream in=getAssets().open("modules/"+name);FileOutputStream out=new FileOutputStream(temporary)){
                byte[] b=new byte[65536];int n;while((n=in.read(b))!=-1)out.write(b,0,n);out.getFD().sync();
            }
            if(temporary.length()!=entry.getLong("bytes")||!digest(temporary).equals(hash))throw new java.io.IOException("游戏模块校验失败");
            if(!temporary.renameTo(target))throw new java.io.IOException("不能保存游戏模块");
        }
        if(!new File(filesBase(),"game.romfs").isFile())throw new java.io.IOException("缺少原游戏资源");
        if(!new File(filesBase(),"d2r-mobile/shaders/shader_index.bin").isFile())throw new java.io.IOException("缺少游戏图形缓存");
    }
    private void configureAndStart(){
        if(!prepared||failed||destroyed)return;
        if(!configured){int rc=nativeConfigure(filesBase().getAbsolutePath(),getResources().getConfiguration().getLocales().get(0).toLanguageTag(),
            getPackageManager().hasSystemFeature(android.content.pm.PackageManager.FEATURE_TOUCHSCREEN));
            if(rc!=0){fail("游戏初始化失败",new Exception("错误 "+rc));return;}configured=true;}
        if(surfaceReady)publishSurface();
    }
    private void publishSurface(){
        if(!configured)return;
        final Surface surface=surfaceReady?game.getHolder().getSurface():null;
        final int w=width,h=height,focus=resumed?1:3;
        final float density=getResources().getDisplayMetrics().density,hz=getWindowManager().getDefaultDisplay().getRefreshRate();
        final boolean start=surfaceReady&&resumed&&!destroyed;
        final int generation=++surfaceGeneration;
        inputReady=false;
        // Draining an in-flight GPU frame can take seconds. Serialize Surface
        // replacement and startup off the Android lifecycle/UI thread.
        surfaceWorker.post(() -> {
            if(generation!=surfaceGeneration)return;
            int rc=nativeSurface(surface,w,h,density,hz,focus);
            if(generation!=surfaceGeneration)return;
            if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed&&generation==surfaceGeneration)fail("游戏窗口不可用",new Exception("错误 "+error));});return;}
            inputReady=start;
            ui.post(() -> {if(!destroyed){touchControls.postInvalidateOnAnimation();syncOverlay();}});
            if(start&&!nativeLaunched){rc=nativeStart();
                if(rc!=0){final int error=rc;ui.post(() -> {if(!destroyed)fail("游戏启动失败",new Exception("错误 "+error));});return;}
                nativeLaunched=true;ui.post(() -> {if(!destroyed&&!launched){launched=true;ui.post(poll);}});
            }
        });
    }
    private void fail(String title,Exception error){nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}failed=true;status.setVisibility(View.VISIBLE);status.setText(title+"\n"+error.getMessage());}
    private final Runnable poll=new Runnable(){public void run(){
        if(!launched||failed)return;
        try{String raw=nativeStatus();JSONObject s=new JSONObject(raw);
            if(s.getLong("presented_frames")>0)status.setVisibility(View.GONE);else status.setText(s.getString("phase"));
            String request=nativeKeyboardRequest();if(request!=null&&resumed&&!destroyed){JSONObject k=new JSONObject(request);long id=k.getLong("id");
                if(keyboardDialog==null||keyboardId!=id){if(keyboardDialog!=null)keyboardDialog.dismiss();keyboardId=id;nativeActions(0,0,0,0,0);touchControls.cancel();
                    keyboardDialog=ShellTextInputDialog.show(GameActivity.this,k.getString("title"),k.getString("subtitle"),k.getString("hint"),k.getString("initial"),k.getInt("min"),k.getInt("max"),(ok,text)->{nativeKeyboardResult(id,ok,text);keyboardDialog=null;});}}
        }catch(Exception e){fail("无法读取游戏状态",e);}syncOverlay();ui.postDelayed(this,250);
    }};
    @Override public void onResume(){super.onResume();resumed=true;nativeKeyboardReady(true);configureAndStart();}
    @Override public void onPause(){nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}resumed=false;inputReady=false;if(touchControls!=null)touchControls.cancel();if(configured)nativeActions(0,0,0,0,0);if(configured&&surfaceReady)publishSurface();syncOverlay();super.onPause();}
    @Override public void surfaceCreated(SurfaceHolder holder){if(!destroyed)surfaceReady=true;}
    @Override public void surfaceChanged(SurfaceHolder holder,int format,int w,int h){if(destroyed)return;width=w;height=h;surfaceReady=true;configureAndStart();}
    @Override public void surfaceDestroyed(SurfaceHolder holder){if(destroyed)return;surfaceReady=false;if(configured)publishSurface();}
    @Override public void onDestroy(){nativeKeyboardReady(false);if(keyboardDialog!=null){keyboardDialog.dismiss();keyboardDialog=null;}destroyed=true;inputReady=false;surfaceReady=false;if(configured)publishSurface();ui.removeCallbacks(poll);surfaceThread.quitSafely();super.onDestroy();}
}
