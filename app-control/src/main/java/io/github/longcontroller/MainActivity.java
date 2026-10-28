package io.github.longcontroller;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.text.TextUtils;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.VideoView;

import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;
import io.github.longcontroller.media.VideoFrameProbe;

public final class MainActivity extends Activity {
    private static final int REQUEST_VIDEO = 1001;
    private final ExecutorService ioExecutor = Executors.newSingleThreadExecutor();

    private ConfigStore store;
    private TextView statusView;
    private TextView sourceView;
    private TextView targetsView;
    private TextView diagnosticsView;
    private VideoView preview;
    private Switch enabledSwitch;
    private boolean bindingSwitch;
    private boolean foreground;
    private boolean busy;
    private AtomicBoolean decodeCancellation;
    private String previewSource = "";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        store = new ConfigStore(this);
        setContentView(buildUi());
        refreshUi();
        runDeviceProbe();
    }

    @Override
    protected void onResume() {
        super.onResume();
        foreground = true;
        if (store != null) refreshUi();
    }

    @Override
    protected void onStop() {
        foreground = false;
        if (decodeCancellation != null) decodeCancellation.set(true);
        previewSource = "";
        if (preview != null) preview.stopPlayback();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        if (preview != null) preview.stopPlayback();
        ioExecutor.shutdownNow();
        super.onDestroy();
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(Color.rgb(245, 247, 250));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(20), dp(18), dp(20), dp(32));
        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView title = text("LONG 虚拟摄像头", 28, Color.rgb(13, 71, 161));
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        root.addView(title);

        TextView subtitle = text(
                "Pixel 5 / redfin · Android 14 · UP1A.231105.001.B2\n首阶段：无需 Root 的媒体预览和设备基线诊断。",
                15, Color.DKGRAY);
        subtitle.setPadding(0, dp(4), 0, dp(16));
        root.addView(subtitle);

        statusView = cardText(root);

        addSection(root, "1. 选择虚拟视频");
        sourceView = bodyText(root);
        Button chooseVideo = button("从文件选择视频");
        chooseVideo.setOnClickListener(v -> chooseVideo());
        root.addView(chooseVideo);

        preview = new VideoView(this);
        LinearLayout.LayoutParams previewParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(210));
        previewParams.setMargins(0, dp(10), 0, dp(6));
        root.addView(preview, previewParams);
        preview.setContentDescription("用户选择的视频本地预览，不是目标应用相机画面");
        preview.setOnPreparedListener(player -> {
            player.setLooping(true);
            player.setVolume(0f, 0f);
            if (foreground) preview.start();
        });
        preview.setOnErrorListener((player, what, extra) -> {
            previewSource = "";
            Toast.makeText(this, "预览失败；请重新选择受支持的视频", Toast.LENGTH_LONG).show();
            return true;
        });
        Button decode = button("验证真实视频帧 → NV21（最多 90 帧）");
        decode.setOnClickListener(v -> runVideoFrameProbe());
        root.addView(decode);
        Button clear = button("删除已导入媒体");
        clear.setOnClickListener(v -> {
            if (busy || !store.hasSource()) return;
            new AlertDialog.Builder(this).setTitle("删除应用内的媒体副本？")
                    .setMessage("不会删除系统文件选择器中的原始视频。")
                    .setPositiveButton("删除副本", (dialog, which) -> {
                        preview.stopPlayback(); previewSource = "";
                        setBusy(true, "正在删除私有副本…");
                        ioExecutor.execute(() -> {
                            String message;
                            try { store.clearSource(); message = "应用内副本已移除；原视频未改动"; }
                            catch (Exception error) { message = "删除失败：" + safeMessage(error); }
                            final String result = message;
                            runOnUiThread(() -> {
                                if (isDestroyed()) return;
                                setBusy(false, null); refreshUi();
                                Toast.makeText(this, result, Toast.LENGTH_LONG).show();
                            });
                        });
                    }).setNegativeButton("取消", null).show();
        });
        root.addView(clear);

        addSection(root, "2. 规划后续测试应用");
        targetsView = bodyText(root);
        Button chooseTargets = button("编辑测试清单（不开放外部访问）");
        chooseTargets.setOnClickListener(v -> chooseTargetApps());
        root.addView(chooseTargets);

        addSection(root, "3. 显式开启实验");
        enabledSwitch = new Switch(this);
        enabledSwitch.setText("允许白名单应用使用虚拟输入");
        enabledSwitch.setTextSize(16);
        enabledSwitch.setPadding(0, dp(8), 0, dp(8));
        enabledSwitch.setOnCheckedChangeListener(this::onEnabledChanged);
        root.addView(enabledSwitch);

        TextView warning = text(
                "当前构建已禁用跨应用替换入口，未 Root 的设备也可使用本地预览与诊断。\n" +
                        "五条采集路径均为 NOT_RUN；属性匹配和 LSPosed 注入不代表兼容性通过。",
                14, Color.rgb(183, 28, 28));
        warning.setPadding(0, dp(4), 0, dp(10));
        root.addView(warning);

        addSection(root, "设备与原生相机预检");
        diagnosticsView = bodyText(root);
        Button refresh = button("重新探测（只读）");
        refresh.setOnClickListener(v -> runDeviceProbe());
        root.addView(refresh);
        Button nativeProbe = button("打开原生相机帧测试（需相机权限）");
        nativeProbe.setOnClickListener(v -> startActivity(new Intent(this, NativeCameraProbeActivity.class)));
        root.addView(nativeProbe);
        Button share = button("分享设备诊断文本（不含视频）");
        share.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setTitle("分享前确认")
                .setMessage("文本包含系统构建指纹和相机能力，不包含视频或图像。将由你选择接收应用。")
                .setPositiveButton("选择接收应用", (dialog, which) -> startActivity(Intent.createChooser(
                        new Intent(Intent.ACTION_SEND).setType("text/plain")
                                .putExtra(Intent.EXTRA_TEXT, diagnosticsView.getText().toString()), "分享设备诊断")))
                .setNegativeButton("取消", null).show());
        root.addView(share);

        TextView footer = text(
                "紧急恢复：关闭上方开关 → 在 LSPosed 停用 LONG 模块 → 强制停止目标应用 → 用系统相机确认原生摄像头恢复。",
                13, Color.DKGRAY);
        footer.setPadding(0, dp(22), 0, 0);
        root.addView(footer);
        return scroll;
    }

    private void chooseVideo() {
        if (busy) return;
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("video/*");
        startActivityForResult(intent, REQUEST_VIDEO);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_VIDEO || resultCode != RESULT_OK || data == null) return;
        Uri uri = data.getData();
        if (uri == null) return;
        String displayName = displayName(uri);
        setBusy(true, "正在复制并校验媒体…");
        preview.stopPlayback(); previewSource = "";
        ioExecutor.execute(() -> {
            try {
                ConfigStore.ImportResult result = store.importVideo(getContentResolver(), uri, displayName);
                runOnUiThread(() -> {
                    if (isDestroyed() || isFinishing()) return;
                    setBusy(false, null);
                    Toast.makeText(this,
                            "已导入 " + humanBytes(result.bytes()) + "；启用状态已安全关闭",
                            Toast.LENGTH_LONG).show();
                    refreshUi();
                });
            } catch (Exception error) {
                runOnUiThread(() -> {
                    if (isDestroyed() || isFinishing()) return;
                    setBusy(false, null);
                    Toast.makeText(this, "导入失败：" + safeMessage(error), Toast.LENGTH_LONG).show();
                    refreshUi();
                });
            }
        });
    }

    private void chooseTargetApps() {
        if (busy) return;
        PackageManager pm = getPackageManager();
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<ResolveInfo> resolved = pm.queryIntentActivities(launcher, PackageManager.MATCH_ALL);
        Map<String, AppChoice> unique = new LinkedHashMap<>();
        for (ResolveInfo item : resolved) {
            if (item.activityInfo == null || getPackageName().equals(item.activityInfo.packageName)) continue;
            ApplicationInfo info = item.activityInfo.applicationInfo;
            String packageName = item.activityInfo.packageName;
            String label = String.valueOf(info.loadLabel(pm));
            unique.put(packageName, new AppChoice(label, packageName));
        }
        List<AppChoice> choices = new ArrayList<>(unique.values());
        choices.sort(Comparator.comparing(AppChoice::label, String.CASE_INSENSITIVE_ORDER));
        if (choices.isEmpty()) {
            Toast.makeText(this, "没有找到可见的启动器应用", Toast.LENGTH_LONG).show();
            return;
        }

        Set<String> selected = new HashSet<>(store.allowedPackages());
        String[] labels = new String[choices.size()];
        boolean[] checked = new boolean[choices.size()];
        for (int i = 0; i < choices.size(); i++) {
            AppChoice choice = choices.get(i);
            labels[i] = choice.label() + "\n" + choice.packageName();
            checked[i] = selected.contains(choice.packageName());
        }

        new AlertDialog.Builder(this)
                .setTitle("仅选择你要测试的应用")
                .setMultiChoiceItems(labels, checked, (dialog, which, isChecked) -> {
                    String packageName = choices.get(which).packageName();
                    if (isChecked) selected.add(packageName);
                    else selected.remove(packageName);
                })
                .setPositiveButton("保存（并关闭当前实验）", (dialog, which) -> {
                    setBusy(true, "正在保存测试清单…");
                    ioExecutor.execute(() -> {
                        String errorText;
                        try { store.setAllowedPackages(selected); errorText = null; }
                        catch (Exception error) { errorText = safeMessage(error); }
                        final String result = errorText;
                        runOnUiThread(() -> {
                            if (isDestroyed() || isFinishing()) return;
                            setBusy(false, null); refreshUi();
                            if (result != null) Toast.makeText(this, result, Toast.LENGTH_LONG).show();
                        });
                    });
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void onEnabledChanged(CompoundButton button, boolean checked) {
        if (bindingSwitch) return;
        if (!checked) {
            store.setEnabled(false);
            refreshUi();
            return;
        }
        new AlertDialog.Builder(this)
                .setTitle("确认兼容性实验")
                .setMessage("只在你拥有或获准测试的普通媒体应用中使用。此开关不证明目标应用兼容，也不适用于要求真实物理采集来源的场景。发生异常时立即停用并验证原生相机恢复。")
                .setPositiveButton("我已了解，开启", (dialog, which) -> {
                    try {
                        store.setEnabled(true);
                    } catch (IllegalStateException error) {
                        Toast.makeText(this, error.getMessage(), Toast.LENGTH_LONG).show();
                    }
                    refreshUi();
                })
                .setNegativeButton("保持原生", (dialog, which) -> {
                    store.setEnabled(false);
                    refreshUi();
                })
                .setOnCancelListener(dialog -> refreshUi())
                .show();
    }

    private void refreshUi() {
        bindingSwitch = true;
        enabledSwitch.setChecked(store.isEnabled());
        enabledSwitch.setEnabled(!busy && TargetDeviceProfile.CROSS_APP_BACKEND_READY
                && ModuleStatus.isActive() && DeviceProbe.targetMatch().matches());
        bindingSwitch = false;

        String module = ModuleStatus.isActive() ? "已检测到 LSPosed 注入" : "未检测到 LSPosed 注入";
        String state = store.isEnabled() ? "兼容性实验已授权" : "保持原生摄像头";
        statusView.setText(state + "\n" + module + "\n证据等级：NOT_RUN（需实机消费者验证）");
        statusView.setTextColor(store.isEnabled() ? Color.rgb(230, 81, 0) : Color.rgb(27, 94, 32));

        if (store.hasSource()) {
            String hash = store.sourceSha256();
            sourceView.setText(store.sourceName() + "\nSHA-256: " + abbreviateHash(hash));
            preview.setVisibility(View.VISIBLE);
            if (!busy && !store.sourceSha256().equals(previewSource)) startPreview();
        } else {
            sourceView.setText("未导入视频。选择媒体后会复制到应用私有目录；不会把原始 URI 暴露给目标应用。");
            preview.setVisibility(View.GONE);
        }

        Set<String> targets = store.allowedPackages();
        targetsView.setText(targets.isEmpty()
                ? "未选择目标应用；外部实验不能开启。"
                : "计划测试 " + targets.size() + " 个包（尚未开放媒体访问）：\n" + TextUtils.join("\n", targets));
    }

    private void startPreview() {
        if (busy || !foreground || !store.hasSource() || preview == null) return;
        preview.stopPlayback();
        previewSource = store.sourceSha256();
        preview.setVideoURI(MediaConfigProvider.mediaUri(this));
        preview.start();
    }

    private void runDeviceProbe() {
        if (busy || ioExecutor.isShutdown()) return;
        diagnosticsView.setText("正在读取本机公开能力…");
        ioExecutor.execute(() -> {
            String report = DeviceProbe.collect(this);
            runOnUiThread(() -> {
                if (!isDestroyed() && !isFinishing()) diagnosticsView.setText(report);
            });
        });
    }

    private void setBusy(boolean busy, String message) {
        this.busy = busy;
        enabledSwitch.setEnabled(!busy && TargetDeviceProfile.CROSS_APP_BACKEND_READY);
        if (message != null) statusView.setText(message);
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(
                uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (index >= 0) return cursor.getString(index);
            }
        } catch (Exception ignored) {
            // A generated, non-sensitive label is used below.
        }
        return "virtual-source-video";
    }

    private static String safeMessage(Exception error) {
        return error.getClass().getSimpleName() + "（检查格式、容量、可用空间及文件访问授权）";
    }

    private void runVideoFrameProbe() {
        if (busy || !foreground || !store.hasSource()) {
            Toast.makeText(this, "请先导入视频并等待当前操作完成", Toast.LENGTH_SHORT).show();
            return;
        }
        preview.stopPlayback(); previewSource = "";
        setBusy(true, "正在解码并验证真实视频 YUV/NV21 帧…");
        AtomicBoolean cancelled = new AtomicBoolean();
        decodeCancellation = cancelled;
        ioExecutor.execute(() -> {
            String message;
            try {
                VideoFrameProbe.Result result = VideoFrameProbe.run(store.sourceFile(), cancelled::get);
                message = "LOCAL_VIDEO_FRAMES_OBSERVED（不是跨应用 PASS）\nCodec=" + result.codec()
                        + "\nNV21=" + result.width() + "×" + result.height() + " / 帧数=" + result.frames()
                        + "\n非递增 PTS=" + result.nonIncreasingPts() + " / 耗时=" + result.elapsedMs() + " ms"
                        + "\n末帧 CRC32=" + Long.toHexString(result.lastFrameCrc32())
                        + "\n只验证解码和布局；未进行方向矫正、实时节奏或目标应用注入。";
            } catch (Exception error) { message = "视频帧验证未通过：" + safeMessage(error); }
            final String result = message;
            runOnUiThread(() -> {
                if (isDestroyed() || isFinishing()) return;
                decodeCancellation = null;
                setBusy(false, null); refreshUi();
                diagnosticsView.setText(cancelled.get() ? "视频帧验证已取消；未判定通过" : result);
            });
        });
    }

    private static String abbreviateHash(String hash) {
        if (hash == null || hash.length() < 16) return "UNKNOWN";
        return hash.substring(0, 12) + "…" + hash.substring(hash.length() - 8);
    }

    private static String humanBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        double value = bytes;
        String[] units = {"B", "KiB", "MiB", "GiB"};
        int unit = 0;
        while (value >= 1024 && unit < units.length - 1) {
            value /= 1024;
            unit++;
        }
        return new DecimalFormat("0.0").format(value) + " " + units[unit];
    }

    private void addSection(LinearLayout root, String label) {
        TextView section = text(label, 19, Color.rgb(38, 50, 56));
        section.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        section.setPadding(0, dp(22), 0, dp(6));
        root.addView(section);
    }

    private TextView cardText(LinearLayout root) {
        TextView view = text("", 16, Color.DKGRAY);
        view.setBackgroundColor(Color.WHITE);
        view.setPadding(dp(16), dp(14), dp(16), dp(14));
        root.addView(view, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return view;
    }

    private TextView bodyText(LinearLayout root) {
        TextView view = text("", 14, Color.DKGRAY);
        view.setPadding(0, 0, 0, dp(8));
        root.addView(view);
        return view;
    }

    private Button button(String label) {
        Button button = new Button(this);
        button.setText(label);
        button.setAllCaps(false);
        return button;
    }

    private TextView text(String value, int sp, int color) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(sp);
        view.setTextColor(color);
        view.setLineSpacing(0, 1.15f);
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private record AppChoice(String label, String packageName) {}
}
