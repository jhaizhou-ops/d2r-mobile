package com.xiaoboluo.d2r;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.Choreographer;
import android.view.MotionEvent;
import android.view.View;

// 越肩镜头的左右转按钮（实验，DM_CAM=1 时才加进来；宿主 dm/host/dm_cam.c）：摇杆左上方两个圆钮，按住就按 1.8 弧度 / 秒（约 100° / 秒）往左 / 右转，
// 和体感转向走同一条路（nativeCamTurn：跟随让开，摇杆的「前方」跟着转）。只在局里、镜头在出图时画出来、收触摸（nativeCamActive，每帧问一次）；
// 按钮以外的触摸一概不收，落到下面的触屏层，所以可以一手按住转、一手推摇杆。图形用画布直接画，不用游戏的图，也不动覆盖层
// Turn-left / turn-right buttons of the over-the-shoulder camera (experimental, added only with DM_CAM=1; host dm/host/dm_cam.c): two round buttons above
// and left of the stick; holding one turns at 1.8 radians / second (about 100 degrees / second) to the left / right, the same way as sensor steering
// (nativeCamTurn: the follow stands aside, the stick's "ahead" turns too). They are drawn and take touches only in a game while the camera is drawing
// (nativeCamActive, asked every frame); touches outside the buttons are never taken and fall through to the touch layer below, so one finger can hold a
// turn while another pushes the stick. Drawn directly on the canvas, without game art and without touching the overlay
final class CamButtons extends View implements Choreographer.FrameCallback {
    private static final float RATE = 1.8f;
    // 圆钮中心与半径（画面宽 / 高的比例）：摇杆中心约在 (0.14, 0.72)，这两个在它左上方的空处
    // Button centres and radius (fractions of the picture's width / height): the stick's centre is about (0.14, 0.72), these sit in the free space above
    // and left of it
    private static final float LEFT_X = 0.07f, RIGHT_X = 0.18f, Y = 0.42f, R = 0.06f;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), ring = new Paint(Paint.ANTI_ALIAS_FLAG), glyph = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int leftId = -1, rightId = -1;
    private boolean shown, running;
    private long lastNs;

    CamButtons(Context context) {
        super(context);
        fill.setStyle(Paint.Style.FILL);
        ring.setStyle(Paint.Style.STROKE);
        ring.setColor(0xB0FFFFFF);
        glyph.setColor(0xF0FFFFFF);
        glyph.setTextAlign(Paint.Align.CENTER);
    }

    void start() { if (!running) { running = true; lastNs = 0; Choreographer.getInstance().postFrameCallback(this); } }
    void stop() { running = false; leftId = rightId = -1; invalidate(); }

    @Override public void doFrame(long ns) {
        if (!running) return;
        boolean active = DmActivity.nativeCamActive() != 0;
        if (active != shown) { shown = active; if (!active) leftId = rightId = -1; invalidate(); }
        int dir = (rightId >= 0 ? 1 : 0) - (leftId >= 0 ? 1 : 0);
        if (dir != 0 && lastNs != 0) DmActivity.nativeCamTurn(dir * RATE * Math.min(0.1f, (ns - lastNs) * 1e-9f));
        lastNs = ns;
        Choreographer.getInstance().postFrameCallback(this);
    }

    private float cx(int i) { return getWidth() * (i == 0 ? LEFT_X : RIGHT_X); }
    private float cy() { return getHeight() * Y; }
    private float r() { return getHeight() * R; }
    // 命中半径放宽到 1.3 倍，手指不必按得很准
    // The hit radius is widened to 1.3 times, so the finger need not be precise
    private int hit(float x, float y) {
        for (int i = 0; i < 2; i++) { float dx = x - cx(i), dy = y - cy(); if (dx * dx + dy * dy <= r() * r() * 1.69f) return i; }
        return -1;
    }

    @Override protected void onDraw(Canvas canvas) {
        if (!shown) return;
        float r = r();
        ring.setStrokeWidth(r * 0.08f);
        glyph.setTextSize(r * 1.2f);
        for (int i = 0; i < 2; i++) {
            boolean held = i == 0 ? leftId >= 0 : rightId >= 0;
            fill.setColor(held ? 0xA0FFFFFF : 0x60000000);
            canvas.drawCircle(cx(i), cy(), r, fill);
            canvas.drawCircle(cx(i), cy(), r, ring);
            canvas.drawText(i == 0 ? "⟲" : "⟳", cx(i), cy() + r * 0.42f, glyph);
        }
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        if (!shown) return false;
        int action = e.getActionMasked(), index = e.getActionIndex(), id = e.getPointerId(index);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int b = hit(e.getX(index), e.getY(index));
                if (b < 0) return action != MotionEvent.ACTION_DOWN;
                if (b == 0) leftId = id; else rightId = id;
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (id == leftId) leftId = -1;
                if (id == rightId) rightId = -1;
                invalidate();
                return true;
            case MotionEvent.ACTION_CANCEL:
                leftId = rightId = -1;
                invalidate();
                return true;
            default:
                return true;
        }
    }
}
