package com.xiaoboluo.d2r;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;
import java.io.File;

// 越肩镜头的画面外怪物指示（实验；总是加上，镜头关着时什么都不画；宿主 dm/host/dm_cam.c dm_cam_threats）：透视画面只看得见前方，怪从画面外很快贴上脸（维护者试玩），
// 这里在屏幕边上给每只画面外的怪画一个朝它的箭头——前方更远处在上沿、左右在两侧、身后在下面两角。颜色按档位（普通琥珀、精英蓝、Boss 红），
// 越近越大越亮，很近时闪。另在地图（角上小地图或全屏地图）的玩家点上画一支白箭头，指镜头朝向（dm_cam_map_heading）：画面转过之后，地图不转，靠它对上方向。
// 每帧问一次（nativeCamThreats / nativeCamHeading / nativeCamAim），只画，不收触摸；箭头用画布直接画，拖技能键瞄准时地上的环用游戏自己的金环图
// Off-picture monster indicators of the over-the-shoulder camera (experimental; always added, nothing drawn while the camera is off; host dm/host/dm_cam.c dm_cam_threats): the
// perspective picture only shows what is ahead, and monsters reach the character's face from outside it fast (the maintainer's play test), so each monster
// off the picture gets an arrow on the screen edge pointing at it -- further ahead on the top edge, left and right on the sides, behind in the two bottom
// corners. Colour by tier, after the original name colours (normal silver, champion blue, unique / super unique gold, act boss red; hle_target.c dm_threat_tier), larger and brighter when closer, blinking when very close. Also a white arrow on the player
// point of the map (the corner minimap or the full-screen map) pointing where the camera looks (dm_cam_map_heading): once the picture has turned the map
// does not, and this lines the two up. Asked once per frame (nativeCamThreats / nativeCamHeading / nativeCamAim); it only draws and never takes touches; the arrows are
// drawn directly on the canvas, and the ring on the ground while a skill button is dragged to aim uses the game's own gold ring art
final class CamThreats extends View implements Choreographer.FrameCallback {
    private static final int[] TIER_COLOUR = { 0xFFE1E1E1, 0xFF6E6EFF, 0xFFC7B377, 0xFFFF5040 };
    private final float[] data = new float[16 * 5], head = new float[4], drawn = new float[4], aim = new float[50], mesh = new float[50];
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path arrow = new Path();
    private int count;
    private boolean running, headOn, aimOn, ringTried;
    // 瞄准环用游戏自己的金环（导入包 touchart/dm_native_circle.png，触屏圆钮同一张）；包里没有就画一圈线
    // The aim ring uses the game's own gold ring (import pack touchart/dm_native_circle.png, the same image as the touch round buttons); without it, a line ring
    private Bitmap ring;
    private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);

    CamThreats(Context context) {
        super(context);
        fill.setStyle(Paint.Style.FILL);
        edge.setStyle(Paint.Style.STROKE);
        edge.setColor(0xC0000000);
    }

    void start() { if (!running) { running = true; Choreographer.getInstance().postFrameCallback(this); } }
    void stop() { running = false; count = 0; headOn = false; aimOn = false; invalidate(); }

    @Override public void doFrame(long ns) {
        if (!running) return;
        int n = DmActivity.nativeCamThreats(data);
        boolean h = DmActivity.nativeCamHeading(head) != 0;
        // 朝向箭头只在变了（转过 0.5° 以上、小地图挪了、开关了）时重画，免得没有怪时也每帧重画这一层
        // Redraw for the heading arrow only when it changed (turned more than 0.5°, the minimap moved, shown or hidden), so this layer is not redrawn every
        // frame when there are no monsters
        boolean headMoved = h != headOn || h && (Math.abs(head[2] - drawn[2]) > 0.009f || Math.abs(head[0] - drawn[0]) > 0.002f
            || Math.abs(head[1] - drawn[1]) > 0.002f || Math.abs(head[3] - drawn[3]) > 0.002f);
        // 瞄准环（拖技能键时，宿主 dm_cam_aim 投好的 5 × 5 网格）：瞄着就每帧重画
        // Aim ring (while a skill button is dragged; the 5 x 5 grid projected by the host's dm_cam_aim): redrawn every frame while aiming
        boolean a = DmActivity.nativeCamAim(aim) == 25;
        if (n != count || n > 0 || headMoved || a || a != aimOn) { count = n; headOn = h; aimOn = a; System.arraycopy(head, 0, drawn, 0, 4); invalidate(); }
        Choreographer.getInstance().postFrameCallback(this);
    }

    @Override public boolean onTouchEvent(MotionEvent e) { return false; }

    @Override protected void onDraw(Canvas canvas) {
        float w = getWidth(), h = getHeight(), blink = (System.nanoTime() / 1_000_000L % 400) < 200 ? 1f : 0.55f;
        if (aimOn) {
            // 瞄准环：游戏的金环平躺在地上（网格把图按透视贴上去），在怪的箭头下面
            // Aim ring: the game's gold ring lying flat on the ground (the grid maps the image in perspective), under the monster arrows
            if (!ringTried) {
                ringTried = true;
                File f = new File(getContext().getFilesDir(), "dm/overlay/touchart/dm_native_circle.png");
                if (f.isFile() && f.length() <= 4 * 1024 * 1024) ring = BitmapFactory.decodeFile(f.getAbsolutePath());
            }
            for (int i = 0; i < 25; i++) { mesh[2 * i] = aim[2 * i] * w; mesh[2 * i + 1] = aim[2 * i + 1] * h; }
            if (ring != null) {
                ringPaint.setAlpha(230);
                canvas.drawBitmapMesh(ring, 4, 4, mesh, 0, null, 0, ringPaint);
            } else {
                arrow.reset();
                final int[] rim = { 0, 1, 2, 3, 4, 9, 14, 19, 24, 23, 22, 21, 20, 15, 10, 5 };
                arrow.moveTo(mesh[0], mesh[1]);
                for (int k = 1; k < rim.length; k++) arrow.lineTo(mesh[2 * rim[k]], mesh[2 * rim[k] + 1]);
                arrow.close();
                edge.setColor(0xE0D4B060);
                edge.setStrokeWidth(Math.max(1.5f, h * 0.004f));
                canvas.drawPath(arrow, edge);
                edge.setColor(0xC0000000);
            }
        }
        for (int i = 0; i < count; i++) {
            float x = data[i * 5] * w, y = data[i * 5 + 1] * h, dir = data[i * 5 + 2], urgency = data[i * 5 + 3];
            int tier = Math.max(0, Math.min(3, (int) data[i * 5 + 4]));
            // 箭头：尖朝怪那一边，长度随紧迫度 0.035–0.065 屏高
            // Arrow: tip towards the monster, length from 0.035 to 0.065 of the screen height with urgency
            float len = h * (0.035f + 0.03f * urgency), half = len * 0.6f;
            float alpha = (0.45f + 0.55f * urgency) * (urgency > 0.75f ? blink : 1f);
            fill.setColor(TIER_COLOUR[tier]);
            fill.setAlpha((int) (255 * alpha));
            edge.setStrokeWidth(Math.max(1f, len * 0.08f));
            edge.setAlpha((int) (200 * alpha));
            canvas.save();
            canvas.translate(x, y);
            canvas.rotate((float) Math.toDegrees(dir));
            arrow.reset();
            arrow.moveTo(0, -len);
            arrow.lineTo(half, len * 0.35f);
            arrow.lineTo(0, len * 0.05f);
            arrow.lineTo(-half, len * 0.35f);
            arrow.close();
            canvas.drawPath(arrow, fill);
            canvas.drawPath(arrow, edge);
            canvas.restore();
        }
        if (headOn) {
            // 朝向箭头：白色，离开玩家点一小段（不盖住游戏自己的玩家标记），尖在 0.115 个尺度处（小地图 = 框高，全屏地图更大）
            // Heading arrow: white, starting a little away from the player point (so the game's own player marker stays visible), tip at 0.115 of the scale
            // (minimap = the frame height, larger on the full-screen map)
            float s = drawn[3] * h, tip = s * 0.115f, base = s * 0.045f, half = s * 0.042f;
            fill.setColor(0xFFFFFFFF);
            fill.setAlpha(235);
            edge.setStrokeWidth(Math.max(1f, s * 0.006f));
            edge.setAlpha(210);
            canvas.save();
            canvas.translate(drawn[0] * w, drawn[1] * h);
            canvas.rotate((float) Math.toDegrees(drawn[2]));
            arrow.reset();
            arrow.moveTo(0, -tip);
            arrow.lineTo(half, -base);
            arrow.lineTo(0, -base - (tip - base) * 0.3f);
            arrow.lineTo(-half, -base);
            arrow.close();
            canvas.drawPath(arrow, fill);
            canvas.drawPath(arrow, edge);
            canvas.restore();
        }
    }
}
