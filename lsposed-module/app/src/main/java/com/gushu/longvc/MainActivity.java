package com.gushu.longvc;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.text.method.ScrollingMovementMethod;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Collections;
import java.util.Enumeration;
import java.util.Locale;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;

import io.github.libxposed.service.XposedService;

public final class MainActivity extends Activity
        implements LongVcApplication.ServiceStateListener {
    private static final int PICK_VIDEO = 1001;
    private static final int PICK_IMAGE = 1002;
    private static final int PICK_AUDIO = 1003;
    private static final String PICKER_OPEN = "document_picker_open";

    private RootController rootController;
    private XposedService xposedService;
    private TextView frameworkStatus;
    private TextView rootStatus;
    private TextView sourceStatus;
    private TextView obsConfiguration;
    private TextView operationLog;
    private Switch cameraSwitch;
    private Switch photoSwitch;
    private Switch micSwitch;
    private Switch obsLanSwitch;
    private Button rootButton;
    private Button installButton;
    private Button stopButton;
    private Button scopeButton;
    private Button refreshButton;
    private Button selectVideoButton;
    private Button selectImageButton;
    private Button selectAudioButton;
    private Button configureObsButton;
    private Button copyObsButton;
    private boolean rendering;
    private boolean busy;
    private boolean emergencyBusy;
    private boolean documentPickerOpen;
    private boolean destroyed;
    private long operationGeneration;
    private RuntimeState lastState;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        documentPickerOpen = savedInstanceState != null
                && savedInstanceState.getBoolean(PICKER_OPEN);
        setContentView(R.layout.activity_main);
        rootController = new RootController(this);

        frameworkStatus = findViewById(R.id.framework_status);
        rootStatus = findViewById(R.id.root_status);
        sourceStatus = findViewById(R.id.source_status);
        obsConfiguration = findViewById(R.id.obs_configuration);
        operationLog = findViewById(R.id.operation_log);
        operationLog.setMovementMethod(new ScrollingMovementMethod());
        cameraSwitch = findViewById(R.id.camera_switch);
        photoSwitch = findViewById(R.id.photo_switch);
        micSwitch = findViewById(R.id.mic_switch);
        obsLanSwitch = findViewById(R.id.obs_lan_switch);
        rootButton = findViewById(R.id.root_button);
        installButton = findViewById(R.id.install_button);
        stopButton = findViewById(R.id.stop_button);
        scopeButton = findViewById(R.id.scope_button);
        refreshButton = findViewById(R.id.refresh_button);
        selectVideoButton = findViewById(R.id.select_video_button);
        selectImageButton = findViewById(R.id.select_image_button);
        selectAudioButton = findViewById(R.id.select_audio_button);
        configureObsButton = findViewById(R.id.configure_obs_button);
        copyObsButton = findViewById(R.id.copy_obs_button);

        // Privacy invariant: UI construction never arms a media path. In
        // particular, the microphone begins OFF and only this listener can arm it.
        rendering = true;
        cameraSwitch.setChecked(false);
        photoSwitch.setChecked(false);
        micSwitch.setChecked(false);
        rendering = false;

        cameraSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!rendering) runToggle(button, checked,
                    RootController.Action.ARM_CAMERA, RootController.Action.DISARM_CAMERA);
        });
        photoSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!rendering) runToggle(button, checked,
                    RootController.Action.ARM_PHOTO, RootController.Action.DISARM_PHOTO);
        });
        micSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!rendering) runToggle(button, checked,
                    RootController.Action.ARM_MIC, RootController.Action.DISARM_MIC);
        });

        rootButton.setOnClickListener(view -> runOperation("正在请求 root…",
                rootController::requestRootAndReadStatus));
        installButton.setOnClickListener(view -> runOperation("正在校验并安装 ARM64 载荷…",
                rootController::installAndReadStatus));
        stopButton.setOnClickListener(view -> runEmergencyStop());
        refreshButton.setOnClickListener(view -> runOperation("正在读取运行状态…",
                rootController::requestRootAndReadStatus));
        scopeButton.setOnClickListener(view -> requestSystemScope());
        selectVideoButton.setOnClickListener(view -> selectDocument("video/*", PICK_VIDEO));
        selectImageButton.setOnClickListener(view -> selectDocument("image/*", PICK_IMAGE));
        selectAudioButton.setOnClickListener(view -> selectDocument("audio/*", PICK_AUDIO));
        configureObsButton.setOnClickListener(view -> {
            boolean allowLan = obsLanSwitch.isChecked();
            runOperation("正在生成 OBS 推流密钥并切换输入源…",
                    () -> rootController.configureObs(allowLan));
        });
        copyObsButton.setOnClickListener(view -> copyObsConfiguration());

        renderControls();
    }

    @Override
    protected void onStart() {
        super.onStart();
        LongVcApplication.addServiceStateListener(this, true);
        // The picker result may arrive after onStart. A background refresh
        // cannot take the operation slot from the pending user import.
        if (!documentPickerOpen && !busy && !emergencyBusy) refreshOnEntry();
    }

    private void refreshOnEntry() {
        runOperation("正在检查 LSPosed 与 root 状态…", rootController::requestRootAndReadStatus);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putBoolean(PICKER_OPEN, documentPickerOpen);
        super.onSaveInstanceState(outState);
    }

    @Override
    protected void onStop() {
        LongVcApplication.removeServiceStateListener(this);
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        super.onDestroy();
    }

    @SuppressWarnings("deprecation")
    private void selectDocument(String mime, int requestCode) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType(mime);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        documentPickerOpen = true;
        try {
            startActivityForResult(intent, requestCode);
        } catch (android.content.ActivityNotFoundException error) {
            documentPickerOpen = false;
            operationLog.setText(getString(R.string.operation_failed, safeMessage(error)));
        }
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_VIDEO && requestCode != PICK_IMAGE &&
                requestCode != PICK_AUDIO) return;
        documentPickerOpen = false;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            if (lastState == null && !busy && !emergencyBusy) refreshOnEntry();
            return;
        }
        Uri uri = data.getData();
        switch (requestCode) {
            case PICK_VIDEO -> runOperation("正在校验并导入本地视频…",
                    () -> rootController.stageVideo(uri));
            case PICK_IMAGE -> runOperation("正在解码并导入本地图片…",
                    () -> rootController.stageImage(uri));
            case PICK_AUDIO -> runOperation("正在校验并导入本地音频…",
                    () -> rootController.stageAudio(uri));
            default -> { }
        }
    }

    @Override
    public void onServiceStateChanged(XposedService service) {
        xposedService = service;
        runOnUiThreadIfAlive(this::renderFramework);
    }

    private void runToggle(CompoundButton button, boolean checked,
                           RootController.Action on, RootController.Action off) {
        button.setEnabled(false);
        RootController.Action action = checked ? on : off;
        runOperation((checked ? "正在启用 " : "正在停用 ") + label(action) + "…",
                () -> rootController.run(action));
    }

    private void runOperation(String pending, StateOperation operation) {
        if (busy || emergencyBusy) {
            Toast.makeText(this, "已有操作正在执行", Toast.LENGTH_SHORT).show();
            renderState(lastState);
            return;
        }
        final long generation = ++operationGeneration;
        busy = true;
        operationLog.setText(pending);
        renderControls();
        LongVcApplication.executeRootOperation(() -> {
            try {
                RuntimeState state = operation.run();
                runOnUiThreadIfAlive(() -> {
                    if (generation != operationGeneration) return;
                    busy = false;
                    RuntimeState display = preserveEphemeralObsKey(state);
                    lastState = display;
                    renderState(display);
                    operationLog.setText(buildSummary(display));
                });
            } catch (Throwable error) {
                runOnUiThreadIfAlive(() -> {
                    if (generation != operationGeneration) return;
                    busy = false;
                    operationLog.setText(getString(R.string.operation_failed, safeMessage(error)));
                    Toast.makeText(this, "操作失败，请查看日志", Toast.LENGTH_LONG).show();
                    renderState(lastState);
                });
            }
        });
    }

    private void runEmergencyStop() {
        if (emergencyBusy) return;
        final long generation = ++operationGeneration;
        busy = false;
        emergencyBusy = true;
        operationLog.setText("正在中断普通任务、停止全部替换并验证系统服务恢复…");
        renderControls();
        LongVcApplication.executeEmergencyOperation(() -> {
            try {
                RuntimeState state = rootController.run(RootController.Action.STOP_ALL);
                runOnUiThreadIfAlive(() -> {
                    if (generation != operationGeneration) return;
                    emergencyBusy = false;
                    RuntimeState display = preserveEphemeralObsKey(state);
                    lastState = display;
                    renderState(display);
                    operationLog.setText(getString(R.string.recovery_verified,
                            buildSummary(display)));
                });
            } catch (Throwable error) {
                runOnUiThreadIfAlive(() -> {
                    if (generation != operationGeneration) return;
                    emergencyBusy = false;
                    operationLog.setText(getString(R.string.operation_failed,
                            safeMessage(error)));
                    Toast.makeText(this, "恢复未通过验证，请立即重启手机",
                            Toast.LENGTH_LONG).show();
                    renderState(lastState);
                });
            }
        });
    }

    private void runOnUiThreadIfAlive(Runnable action) {
        runOnUiThread(() -> {
            if (!destroyed && !isDestroyed()) action.run();
        });
    }

    private void requestSystemScope() {
        XposedService service = xposedService;
        if (service == null) {
            Toast.makeText(this, "尚未连接现代 LSPosed 服务", Toast.LENGTH_LONG).show();
            return;
        }
        service.requestScope(Collections.singletonList("system"),
                new XposedService.OnScopeEventListener() {
                    @Override
                    public void onScopeRequestApproved(java.util.List<String> approved) {
                        runOnUiThreadIfAlive(() -> {
                            operationLog.setText(getString(R.string.scope_approved, approved));
                            renderFramework();
                        });
                    }

                    @Override
                    public void onScopeRequestFailed(String message) {
                        runOnUiThreadIfAlive(() -> operationLog.setText(
                                getString(R.string.scope_failed, message)));
                    }
                });
    }

    private void renderState(RuntimeState state) {
        rendering = true;
        if (state == null) {
            cameraSwitch.setChecked(false);
            photoSwitch.setChecked(false);
            micSwitch.setChecked(false);
            rootStatus.setText(R.string.root_not_checked);
        } else {
            cameraSwitch.setChecked(state.cameraEnabled);
            photoSwitch.setChecked(state.photoEnabled);
            micSwitch.setChecked(state.micEnabled);
            String status = String.format(Locale.ROOT,
                    "Root：%s  载荷：%s  ABI：%s  SELinux：%s",
                    state.root ? "可用" : "不可用",
                    state.installed ? "已校验" : "未安装",
                    state.abi, state.selinux);
            rootStatus.setText(status);
        }
        renderSource(state);
        rendering = false;
        renderControls();
    }

    private void renderControls() {
        boolean rootReady = lastState != null && lastState.root;
        boolean visualReady = lastState != null && lastState.visualSourceReady;
        boolean audioReady = lastState != null && lastState.audioSourceReady;
        boolean blocked = busy || emergencyBusy;
        cameraSwitch.setEnabled(!blocked && rootReady &&
                (visualReady || lastState.cameraEnabled));
        photoSwitch.setEnabled(!blocked && rootReady &&
                (visualReady || lastState.photoEnabled));
        micSwitch.setEnabled(!blocked && rootReady &&
                (audioReady || lastState.micEnabled));
        rootButton.setEnabled(!blocked);
        installButton.setEnabled(!blocked && rootReady);
        // Emergency recovery remains actionable during a long media import.
        stopButton.setEnabled(!emergencyBusy && rootReady);
        refreshButton.setEnabled(!blocked);
        scopeButton.setEnabled(!blocked && xposedService != null);
        selectVideoButton.setEnabled(!blocked && rootReady);
        selectImageButton.setEnabled(!blocked && rootReady);
        selectAudioButton.setEnabled(!blocked && rootReady);
        configureObsButton.setEnabled(!blocked && rootReady);
        obsLanSwitch.setEnabled(!blocked && rootReady);
        copyObsButton.setEnabled(!blocked && lastState != null
                && lastState.obsConfigured && !lastState.obsStreamKey.isBlank());
    }

    private void renderSource(RuntimeState state) {
        if (state == null || !state.root) {
            sourceStatus.setText(R.string.source_not_configured);
            obsConfiguration.setText(R.string.obs_not_configured);
            return;
        }
        String visual = state.visualUsesObs()
                ? "OBS" + (state.obsListenerReady ? "（监听中）" : "（待启用）")
                : !state.visualSourceReady ? "本地（未选择）"
                : "image".equals(state.visualKind) ? "本地图片" : "本地视频";
        String audio = state.audioUsesObs()
                ? "OBS AAC" + (state.obsListenerReady ? "（监听中）" : "（待启用）")
                : state.audioSourceReady ? "本地音频" : "手机原生麦克风（替换关闭）";
        sourceStatus.setText(getString(R.string.source_independent_status, visual, audio));
        if (state.obsConfigured) obsLanSwitch.setChecked("lan".equals(state.obsBind));
        obsConfiguration.setText(state.obsConfigured
                ? formatObsConfiguration(state) : getString(R.string.obs_not_configured));
    }

    private void renderFramework() {
        XposedService service = xposedService;
        if (service == null) {
            frameworkStatus.setText(R.string.framework_unbound);
            renderControls();
            return;
        }
        boolean systemScope = service.getScope().contains("system");
        boolean running = false;
        if (service.getApiVersion() >= 102) {
            for (var target : service.getRunningTargets()) {
                String process = target.getProcessName();
                if ("system_server".equals(process) || "system".equals(process)
                        || "android".equals(process)) {
                    running = true;
                    break;
                }
            }
        }
        boolean systemCap = (service.getFrameworkProperties() & XposedService.PROP_CAP_SYSTEM) != 0L;
        frameworkStatus.setText(String.format(Locale.ROOT,
                "LSPosed：%s API %d · system 能力：%s · system 作用域：%s · 系统层进程：%s",
                service.getFrameworkName(), service.getApiVersion(), yesNo(systemCap),
                yesNo(systemScope), yesNo(running)));
        renderControls();
    }

    private static String buildSummary(RuntimeState state) {
        return "输入源：" + sourceLabel(state)
                + "\n全局相机：" + cameraState(state, state.cameraEnabled)
                + "\n照片替换：" + cameraState(state, state.photoEnabled)
                + "\n虚拟麦克风：" + microphoneState(state)
                + (state.micEnabled ? "" : "（手机原生麦克风）")
                + "\n\n本地媒体与 OBS 共用同一条手机端解码和系统服务替换管线。";
    }

    private static String cameraState(RuntimeState state, boolean enabled) {
        if (!enabled) return "关闭";
        if (!state.videoFeederReady) return state.visualUsesObs()
                ? "等待手机 RTMP 监听器" : "等待本地媒体解码器";
        if (!state.cameraHookReady) return "等待 cameraserver hook";
        if (!state.videoChannelReady) return state.visualUsesObs()
                ? "等待 OBS H.264 视频帧" : "等待本地视频帧";
        return "READY";
    }

    private static String microphoneState(RuntimeState state) {
        if (!state.micEnabled) return "关闭";
        if (!state.audioFeederReady) return state.audioUsesObs()
                ? "等待手机 RTMP 监听器" : "等待本地音频解码器";
        if (!state.audioHookReady) return "等待 audioserver hook";
        if (!state.audioChannelReady) return state.audioUsesObs()
                ? "等待 OBS AAC 音频帧" : "等待本地 PCM";
        return "READY";
    }

    private void copyObsConfiguration() {
        RuntimeState state = lastState;
        if (state == null || !state.obsConfigured || state.obsStreamKey.isBlank()) return;
        String configuration = formatObsConfiguration(state);
        ClipboardManager clipboard = getSystemService(ClipboardManager.class);
        if (clipboard == null) {
            Toast.makeText(this, "无法访问剪贴板", Toast.LENGTH_LONG).show();
            return;
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("LONG OBS configuration",
                configuration));
        Toast.makeText(this, "OBS 配置已复制", Toast.LENGTH_SHORT).show();
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            ClipData current = clipboard.getPrimaryClip();
            if (current == null || current.getItemCount() == 0) return;
            CharSequence text = current.getItemAt(0).coerceToText(this);
            if (!configuration.contentEquals(text)) return;
            if (Build.VERSION.SDK_INT >= 28) clipboard.clearPrimaryClip();
            else clipboard.setPrimaryClip(ClipData.newPlainText("", ""));
        }, 60_000L);
    }

    private String formatObsConfiguration(RuntimeState state) {
        String address = localIpv4Address();
        String host = address == null ? "<手机局域网IP>" : address;
        String key = state.obsStreamKey.isBlank()
                ? "<仅在生成后的本次 App 会话显示，请重新生成>"
                : state.obsStreamKey;
        String lan = "lan".equals(state.obsBind)
                ? "局域网服务器：rtmp://" + host + ":1935/live\n"
                : "局域网监听：关闭（推荐）\n";
        return lan + "推流密钥：" + key
                + "\nUSB 服务器：rtmp://127.0.0.1:1935/live"
                + "\nUSB 转发：adb forward tcp:1935 tcp:1935";
    }

    private static String localIpv4Address() {
        try {
            Enumeration<NetworkInterface> interfaces = NetworkInterface.getNetworkInterfaces();
            while (interfaces != null && interfaces.hasMoreElements()) {
                NetworkInterface network = interfaces.nextElement();
                if (!network.isUp() || network.isLoopback()) continue;
                Enumeration<InetAddress> addresses = network.getInetAddresses();
                while (addresses.hasMoreElements()) {
                    InetAddress address = addresses.nextElement();
                    if (address instanceof Inet4Address && !address.isLoopbackAddress()
                            && !address.isLinkLocalAddress()) return address.getHostAddress();
                }
            }
        } catch (Exception ignored) {
            // UI falls back to a descriptive placeholder.
        }
        return null;
    }

    private static String sourceLabel(RuntimeState state) {
        if (state.visualUsesObs() && state.audioUsesObs()) {
            return state.obsListenerReady ? "OBS（监听中）" : "OBS（已配置）";
        }
        if (state.usesObs()) return "混合来源";
        if (!state.visualSourceReady && !state.audioSourceReady) return "手机本地（未配置）";
        return "手机本地媒体";
    }

    private RuntimeState preserveEphemeralObsKey(RuntimeState state) {
        if (state == null || !state.obsConfigured || !state.obsStreamKey.isBlank()
                || lastState == null || lastState.obsStreamKey.isBlank()) return state;
        return state.withObsStreamKey(lastState.obsStreamKey);
    }

    private static String label(RootController.Action action) {
        return switch (action) {
            case ARM_CAMERA, DISARM_CAMERA -> "全局相机";
            case ARM_PHOTO, DISARM_PHOTO -> "照片替换";
            case ARM_MIC, DISARM_MIC -> "虚拟麦克风";
            case STOP_ALL -> "全部替换";
        };
    }

    private static String safeMessage(Throwable error) {
        String message = error.getMessage();
        if (message == null || message.isBlank()) message = error.getClass().getSimpleName();
        return message.length() > 1600 ? message.substring(0, 1600) + "…" : message;
    }

    private static String yesNo(boolean value) {
        return value ? "是" : "否";
    }

    @FunctionalInterface
    private interface StateOperation {
        RuntimeState run() throws Exception;
    }
}
