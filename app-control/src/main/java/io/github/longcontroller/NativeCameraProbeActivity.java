package io.github.longcontroller;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Size;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

/** Foreground-only native capture probe. Never persists pixels or grants compatibility. */
public final class NativeCameraProbeActivity extends Activity {
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Handler deadlines = new Handler(Looper.getMainLooper());
    private TextView report;
    private CameraDevice camera;
    private CameraCaptureSession session;
    private ImageReader reader;
    private int generation;
    private int frames;
    private int nonIncreasingTimestamps;
    private long lastTimestamp;
    private boolean foreground;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(20 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        report = new TextView(this);
        report.setTextSize(16);
        report.setText("原生 Camera2 基线测试\n需用户授权相机；仅统计 YUV 帧和时间戳，不保存图像、不录音。\n"
                + "请分别测试前后摄，并在关闭后重复打开。结果不是虚拟相机兼容证据。");
        layout.addView(report);
        Button start = new Button(this);
        start.setText("选择 Camera ID 并采集 3 秒");
        start.setOnClickListener(view -> chooseCamera());
        layout.addView(start);
        Button stop = new Button(this);
        stop.setText("立即关闭相机");
        stop.setOnClickListener(view -> finishProbe("用户停止；已请求释放相机"));
        layout.addView(stop);
        setContentView(layout);
    }

    @Override protected void onStart() { super.onStart(); foreground = true; }
    @Override protected void onStop() {
        foreground = false;
        finishProbe("测试中断：离开前台；已请求释放相机，请重新开始");
        super.onStop();
    }

    private void chooseCamera() {
        if (!foreground) return;
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.CAMERA}, 1);
            return;
        }
        closeResources();
        try {
            CameraManager manager = getSystemService(CameraManager.class);
            String[] ids = manager.getCameraIdList();
            String[] labels = new String[ids.length];
            for (int i = 0; i < ids.length; i++) {
                Integer facing = manager.getCameraCharacteristics(ids[i]).get(CameraCharacteristics.LENS_FACING);
                labels[i] = "ID " + ids[i] + " / " + (Integer.valueOf(CameraCharacteristics.LENS_FACING_FRONT).equals(facing)
                        ? "前摄" : Integer.valueOf(CameraCharacteristics.LENS_FACING_BACK).equals(facing) ? "后摄" : "其他");
            }
            if (ids.length == 0) { report.setText("未发现 Camera ID"); return; }
            new AlertDialog.Builder(this).setTitle("选择本机实际枚举的相机")
                    .setItems(labels, (dialog, which) -> startProbe(manager, ids[which]))
                    .setNegativeButton("取消", null).show();
        } catch (Exception error) {
            finishProbe("枚举失败：" + error.getClass().getSimpleName());
        }
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        if (code == 1 && results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) chooseCamera();
        else report.setText("相机权限未授权；未执行采集");
    }

    @SuppressWarnings("deprecation")
    private void startProbe(CameraManager manager, String id) {
        if (!foreground || checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) return;
        closeResources();
        final int token = generation;
        frames = 0;
        lastTimestamp = 0;
        nonIncreasingTimestamps = 0;
        try {
            StreamConfigurationMap map = manager.getCameraCharacteristics(id)
                    .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
            Size[] sizes = map == null ? null : map.getOutputSizes(ImageFormat.YUV_420_888);
            if (sizes == null) throw new IllegalStateException("No YUV output");
            Size size = Arrays.stream(sizes)
                    .filter(s -> s.getWidth() > 0 && s.getHeight() > 0
                            && (long) s.getWidth() * s.getHeight() <= 1920L * 1080)
                    .min(Comparator.comparingLong(s -> Math.abs((long) s.getWidth() * s.getHeight() - 640L * 480)))
                    .orElseThrow(() -> new IllegalStateException("No bounded YUV output"));
            reader = ImageReader.newInstance(size.getWidth(), size.getHeight(), ImageFormat.YUV_420_888, 3);
            reader.setOnImageAvailableListener(source -> {
                if (token != generation) return;
                try (Image image = source.acquireLatestImage()) {
                    if (image == null) return;
                    long timestamp = image.getTimestamp();
                    if (timestamp <= lastTimestamp) nonIncreasingTimestamps++;
                    lastTimestamp = timestamp;
                    frames++;
                } catch (RuntimeException error) {
                    finishProbe("帧读取失败：" + error.getClass().getSimpleName());
                }
            }, handler);
            report.setText("原生 Camera ID=" + id + " / " + size + " / YUV_420_888\n正在打开…");
            deadlines.postDelayed(() -> { if (token == generation) finishProbe("超时；未确认原生采集正常"); }, 10000);
            manager.openCamera(id, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice device) {
                    if (token != generation || !foreground) { device.close(); return; }
                    camera = device;
                    try {
                        device.createCaptureSession(List.of(reader.getSurface()), new CameraCaptureSession.StateCallback() {
                            @Override public void onConfigured(CameraCaptureSession configured) {
                                if (token != generation || !foreground) { configured.close(); return; }
                                session = configured;
                                try {
                                    CaptureRequest.Builder request = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                                    request.addTarget(reader.getSurface());
                                    configured.setRepeatingRequest(request.build(), null, handler);
                                    deadlines.postDelayed(() -> {
                                        if (token != generation) return;
                                        finishProbe("Camera ID=" + id + " / " + size + "\n"
                                                + (frames > 0 ? "FRAMES_OBSERVED（仅原生）" : "NO_FRAMES")
                                                + "\n已取得帧数=" + frames + "，非递增时间戳=" + nonIncreasingTimestamps
                                                + "\n已请求释放；请再次测试，并用系统相机检查可重新打开。\n跨应用替换仍为 NOT_RUN。");
                                    }, 3000);
                                } catch (Exception error) { finishProbe("采集启动失败：" + error.getClass().getSimpleName()); }
                            }
                            @Override public void onConfigureFailed(CameraCaptureSession failed) {
                                failed.close();
                                if (token == generation) finishProbe("原生会话配置失败");
                            }
                        }, handler);
                    } catch (Exception error) { finishProbe("会话创建失败：" + error.getClass().getSimpleName()); }
                }
                @Override public void onDisconnected(CameraDevice device) {
                    device.close();
                    if (token == generation) finishProbe("相机断开");
                }
                @Override public void onError(CameraDevice device, int error) {
                    device.close();
                    if (token == generation) finishProbe("相机错误 code=" + error);
                }
            }, handler);
        } catch (Exception error) { finishProbe("打开失败：" + error.getClass().getSimpleName()); }
    }

    private void finishProbe(String text) {
        closeResources();
        if (report != null) report.setText(text);
    }

    private void closeResources() {
        generation++;
        // Never remove CameraDevice callbacks: a late onOpened must close its device.
        deadlines.removeCallbacksAndMessages(null);
        if (session != null) { session.close(); session = null; }
        if (camera != null) { camera.close(); camera = null; }
        if (reader != null) { reader.close(); reader = null; }
    }
}
