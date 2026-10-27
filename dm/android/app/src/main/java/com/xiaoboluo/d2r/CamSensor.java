package com.xiaoboluo.d2r;

import android.app.Activity;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.view.Display;
import android.view.Surface;

// 越肩镜头的体感转向（实验，缺省关；DM_CAM=1 且 DM_CAM_SENSOR=1 / 2，宿主 dm/host/dm_cam.c，设计见 docs/over-the-shoulder-camera.zh-CN.md）。
// 1 = 转手机 / 转身看四周：陀螺仪的角速度投到真竖直方向（重力），镜头跟着转同样的角度。
// 2 = 像方向盘那样侧倾手机：重力在屏幕平面里偏开竖直多少，越过 8° 死区就按比例转（最快 2.5 弧度 / 秒）；手机放得太平（重力几乎垂直屏幕）不转。
// 只在游戏开着、在前台时听传感器（DmActivity 的 onResume / onPause），每次转动的弧度交给 nativeCamTurn（正 = 往画面右边转）。
// Sensor steering of the over-the-shoulder camera (experimental, off by default; DM_CAM=1 and DM_CAM_SENSOR=1 / 2, host dm/host/dm_cam.c, design in
// docs/over-the-shoulder-camera.md).
// 1 = turn the phone / your body to look around: the gyroscope's angular velocity projected on the real vertical (gravity); the camera turns by the same angle.
// 2 = tilt the phone like a steering wheel: how far gravity leans from vertical in the screen plane; past an 8 degree dead zone the camera turns in
// proportion (at most 2.5 radians / second); a phone held too flat (gravity almost normal to the screen) does not turn.
// Sensors are listened to only while the game runs in the foreground (DmActivity's onResume / onPause); each turn in radians goes to nativeCamTurn
// (positive = towards the right of the picture).
final class CamSensor implements SensorEventListener {
    private final Activity activity;
    private final SensorManager sensors;
    private int mode;
    private boolean listening;
    // 「上」在设备坐标里（重力传感器给的就是朝上的反作用力；只有加速度计时低通滤过）
    // "Up" in device coordinates (the gravity sensor reports the upward reaction force; with only an accelerometer it is low-pass filtered)
    private final float[] up = new float[3];
    private boolean haveUp;
    private long lastGyro, lastTilt;

    CamSensor(Activity activity) { this.activity = activity; sensors = activity.getSystemService(SensorManager.class); }

    void start(int m) {
        stop();
        mode = m;
        if (m <= 0 || sensors == null) return;
        Sensor g = sensors.getDefaultSensor(Sensor.TYPE_GRAVITY);
        if (g == null) g = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        if (g != null) listening |= sensors.registerListener(this, g, SensorManager.SENSOR_DELAY_GAME);
        if (m == 1) {
            Sensor gyro = sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
            if (gyro != null) listening |= sensors.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME);
        }
        android.util.Log.i("dm", "camera sensor: mode " + m + ", listening " + listening);
    }

    void stop() {
        if (listening && sensors != null) sensors.unregisterListener(this);
        listening = false;
        haveUp = false;
        lastGyro = lastTilt = 0;
    }

    @Override public void onAccuracyChanged(Sensor sensor, int accuracy) {}

    @Override public void onSensorChanged(SensorEvent e) {
        int type = e.sensor.getType();
        if (type == Sensor.TYPE_GRAVITY || type == Sensor.TYPE_ACCELEROMETER) {
            float k = type == Sensor.TYPE_GRAVITY || !haveUp ? 1f : 0.1f;
            for (int i = 0; i < 3; i++) up[i] += (e.values[i] - up[i]) * k;
            haveUp = true;
            if (mode == 2) tilt(e.timestamp);
        } else if (type == Sensor.TYPE_GYROSCOPE && mode == 1 && haveUp) {
            float dt = lastGyro == 0 ? 0 : Math.min(0.1f, (e.timestamp - lastGyro) * 1e-9f);
            lastGyro = e.timestamp;
            float n = (float) Math.sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
            if (n < 1 || dt <= 0) return;
            // 绕真竖直的角速度（弧度 / 秒），从上往下看逆时针为正 = 向左转；镜头往右转为正，所以取负。小于 0.05 的当手抖
            // Angular velocity around the real vertical (radians / second), counter-clockwise seen from above is positive = turning left; turning the camera
            // right is positive, hence the minus. Below 0.05 counts as hand shake
            float yaw = (e.values[0] * up[0] + e.values[1] * up[1] + e.values[2] * up[2]) / n;
            if (Math.abs(yaw) < 0.05f) return;
            DmActivity.nativeCamTurn(-yaw * dt);
        }
    }

    private void tilt(long timestamp) {
        float dt = lastTilt == 0 ? 0 : Math.min(0.1f, (timestamp - lastTilt) * 1e-9f);
        lastTilt = timestamp;
        if (dt <= 0) return;
        // 「上」换到屏幕坐标（同 Android 的 AccelerometerPlay 示例按屏幕方向换轴）
        // "Up" in screen coordinates (axes swapped by screen rotation, as in Android's AccelerometerPlay sample)
        Display display = activity.getDisplay();
        int rotation = display == null ? Surface.ROTATION_90 : display.getRotation();
        float sx, sy;
        switch (rotation) {
            case Surface.ROTATION_90: sx = -up[1]; sy = up[0]; break;
            case Surface.ROTATION_180: sx = -up[0]; sy = -up[1]; break;
            case Surface.ROTATION_270: sx = up[1]; sy = -up[0]; break;
            default: sx = up[0]; sy = up[1]; break;
        }
        if (Math.hypot(sx, sy) < 3) return;
        // 像方向盘往右（顺时针）转手机：屏幕里的「上」偏向左，roll 为正 = 往右转
        // Turning the phone right (clockwise) like a steering wheel: "up" on the screen leans left, roll is positive = turn right
        float roll = (float) Math.atan2(-sx, sy), over = Math.abs(roll) - 0.14f;
        if (over <= 0) return;
        DmActivity.nativeCamTurn(Math.min(2.5f, over * 2.5f) * Math.signum(roll) * dt);
    }
}
