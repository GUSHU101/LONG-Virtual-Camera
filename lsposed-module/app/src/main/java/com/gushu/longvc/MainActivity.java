package com.gushu.longvc;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Collections;
import java.util.Locale;

import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.HookedTarget;

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
    private EditText obsUrlInput;
    private Switch obsAudioSwitch;
    private Button rootButton;
    private Button installButton;
    private Button stopButton;
    private Button scopeButton;
    private Button refreshButton;
    private Button selectVideoButton;
    private Button selectImageButton;
    private Button selectAudioButton;
    private Button configureObsButton;
    private boolean rendering;
    private boolean busy;
    private boolean emergencyBusy;
    private boolean documentPickerOpen;
    private boolean canRequestSystemScope;
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
        obsUrlInput = findViewById(R.id.obs_connection_input);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            obsUrlInput.setImportantForContentCapture(View.IMPORTANT_FOR_CONTENT_CAPTURE_NO);
        }
        obsAudioSwitch = findViewById(R.id.obs_audio_switch);
        rootButton = findViewById(R.id.root_button);
        installButton = findViewById(R.id.install_button);
        stopButton = findViewById(R.id.stop_button);
        scopeButton = findViewById(R.id.scope_button);
        refreshButton = findViewById(R.id.refresh_button);
        selectVideoButton = findViewById(R.id.select_video_button);
        selectImageButton = findViewById(R.id.select_image_button);
        selectAudioButton = findViewById(R.id.select_audio_button);
        configureObsButton = findViewById(R.id.configure_obs_button);

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
        refreshButton.setOnClickListener(view -> {
            renderFramework();
            runOperation("正在读取运行状态…", rootController::requestRootAndReadStatus);
        });
        scopeButton.setOnClickListener(view -> requestSystemScope());
        selectVideoButton.setOnClickListener(view -> selectDocument("video/*", PICK_VIDEO));
        selectImageButton.setOnClickListener(view -> selectDocument("image/*", PICK_IMAGE));
        selectAudioButton.setOnClickListener(view -> selectDocument("audio/*", PICK_AUDIO));
        configureObsButton.setOnClickListener(view -> {
            String url = obsUrlInput.getText().toString();
            boolean useObsAudio = obsAudioSwitch.isChecked();
            runOperation("正在保存电脑拉流地址…",
                    () -> rootController.configureObs(url, useObsAudio),
                    () -> obsUrlInput.getText().clear());
        });

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
        runOperation(pending, operation, () -> {});
    }

    private void runOperation(String pending, StateOperation operation, Runnable onSuccess) {
        if (busy || emergencyBusy) {
            Toast.makeText(this, "已有操作正在执行", Toast.LENGTH_SHORT).show();
            renderState(lastState);
            return;
        }
        final long generation = operationGeneration + 1;
        boolean accepted = LongVcApplication.executeRootOperation(() -> {
            try {
                RuntimeState state = operation.run();
                runOnUiThreadIfAlive(() -> {
                    if (generation != operationGeneration) return;
                    busy = false;
                    onSuccess.run();
                    RuntimeState display = state;
                    lastState = display;
                    renderState(display);
                    operationLog.setText(buildSummary(display));
                });
            } catch (Exception error) {
                showOperationFailure(generation, false, error);
            }
        });
        if (!accepted) {
            operationLog.setText(R.string.root_operation_in_progress);
            renderState(lastState);
            return;
        }
        operationGeneration = generation;
        busy = true;
        operationLog.setText(pending);
        renderControls();
    }

    private void runEmergencyStop() {
        if (emergencyBusy) return;
        final long generation = operationGeneration + 1;
        boolean accepted = LongVcApplication.executeEmergencyOperation(() -> {
            try {
                RuntimeState state = rootController.run(RootController.Action.STOP_ALL);
                runOnUiThreadIfAlive(() -> {
                    if (generation != operationGeneration) return;
                    emergencyBusy = false;
                    RuntimeState display = state;
                    lastState = display;
                    renderState(display);
                    operationLog.setText(getString(R.string.recovery_verified,
                            buildSummary(display)));
                });
            } catch (Exception error) {
                showOperationFailure(generation, true, error);
            }
        });
        if (!accepted) {
            operationLog.setText(R.string.recovery_in_progress);
            return;
        }
        operationGeneration = generation;
        busy = false;
        emergencyBusy = true;
        operationLog.setText(R.string.recovery_in_progress);
        renderControls();
    }

    private void showOperationFailure(long generation, boolean recovery, Exception error) {
        RuntimeState observed = null;
        String statusError = "";
        if (error instanceof InterruptedException) {
            Thread.currentThread().interrupt();
        } else if (!Thread.currentThread().isInterrupted()) {
            try {
                observed = rootController.requestRootAndReadStatus();
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            } catch (Exception readFailure) {
                statusError = safeMessage(readFailure);
            }
        }
        final RuntimeState current = observed;
        final String readError = statusError;
        runOnUiThreadIfAlive(() -> {
            if (generation != operationGeneration) return;
            busy = false;
            emergencyBusy = false;
            lastState = current;
            renderState(lastState);
            String status = current == null
                    ? getString(R.string.runtime_state_unknown, readError)
                    : buildSummary(lastState);
            operationLog.setText(getString(recovery ? R.string.recovery_unverified
                    : R.string.operation_failed_with_status, safeMessage(error), status));
        });
    }

    private void runOnUiThreadIfAlive(Runnable action) {
        runOnUiThread(() -> {
            if (!destroyed && !isDestroyed()) action.run();
        });
    }

    private void requestSystemScope() {
        XposedService service = xposedService;
        if (service == null || !canRequestSystemScope) return;
        try {
            service.requestScope(Collections.singletonList("system"),
                new XposedService.OnScopeEventListener() {
                    @Override
                    public void onScopeRequestApproved(java.util.List<String> approved) {
                        runOnUiThreadIfAlive(() -> {
                            operationLog.setText(approved.contains("system")
                                    ? getString(R.string.scope_approved, getPackageName())
                                    : getString(R.string.scope_failed, approved));
                            renderFramework();
                        });
                    }

                    @Override
                    public void onScopeRequestFailed(String message) {
                        runOnUiThreadIfAlive(() -> operationLog.setText(
                                getString(R.string.scope_failed, message)));
                    }
                });
        } catch (RuntimeException error) {
            operationLog.setText(getString(R.string.scope_failed, safeMessage(error)));
            renderFramework();
        }
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
                    state.installed ? "已校验" : "未通过当前 APK 校验",
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
        stopButton.setEnabled(!emergencyBusy);
        refreshButton.setEnabled(!blocked);
        scopeButton.setEnabled(!blocked && canRequestSystemScope);
        selectVideoButton.setEnabled(!blocked && rootReady);
        selectImageButton.setEnabled(!blocked && rootReady);
        selectAudioButton.setEnabled(!blocked && rootReady);
        configureObsButton.setEnabled(!blocked && rootReady);
        obsUrlInput.setEnabled(!blocked && rootReady);
        obsAudioSwitch.setEnabled(!blocked && rootReady);
    }

    private void renderSource(RuntimeState state) {
        if (state == null || !state.root) {
            sourceStatus.setText(R.string.source_not_configured);
            obsConfiguration.setText(R.string.obs_not_configured);
            return;
        }
        if (!state.controllerVerified) {
            sourceStatus.setText(R.string.source_controller_mismatch);
            obsConfiguration.setText(R.string.obs_not_configured);
            return;
        }
        String visual = state.visualUsesObs()
                ? state.videoChannelReady ? "电脑 OBS（已收到视频）" : "电脑 OBS"
                : !state.visualSourceReady ? "本地（未选择）"
                : "image".equals(state.visualKind) ? "本地图片" : "本地视频";
        String audio = state.audioUsesObs()
                ? !state.micEnabled ? "OBS AAC（虚拟麦克风关闭）"
                    : state.audioChannelReady ? "OBS AAC（已收到音频）" : "OBS AAC（等待音频）"
                : state.audioSourceReady ? "本地音频" : "手机原生麦克风（替换关闭）";
        sourceStatus.setText(getString(R.string.source_independent_status, visual, audio));
        obsUrlInput.setHint(state.obsConfigured ? R.string.obs_url_saved_hint : R.string.obs_url_hint);
        obsAudioSwitch.setChecked(state.audioUsesObs());
        obsConfiguration.setText(state.obsConfigured
                ? formatObsConfiguration(state) : getString(R.string.obs_not_configured));
    }

    private void renderFramework() {
        XposedService service = xposedService;
        canRequestSystemScope = false;
        if (service == null) {
            frameworkStatus.setText(getString(R.string.framework_unbound, getPackageName()));
            renderControls();
            return;
        }
        try {
            int api = service.getApiVersion();
            boolean systemCap = (service.getFrameworkProperties() & XposedService.PROP_CAP_SYSTEM) != 0L;
            boolean scoped = service.getScope().contains("system");
            canRequestSystemScope = systemCap && !scoped;
            int status = R.string.scope_no_capability;
            HookedTarget systemTarget = null;
            if (systemCap && !scoped) {
                status = R.string.scope_not_granted;
            } else if (systemCap) {
                status = R.string.scope_target_unknown;
                if (service.getApiVersion() >= 102) {
                    status = R.string.scope_waiting_system;
                    for (HookedTarget target : service.getRunningTargets()) {
                        String process = target.getProcessName();
                        if (target.getUid() != android.os.Process.SYSTEM_UID
                                || target.getPid() <= 0) continue;
                        if ("system_server".equals(process)) {
                            systemTarget = target;
                            break;
                        }
                        // Some frameworks expose the Android system package
                        // or scope alias as the process name in this snapshot.
                        if ("android".equals(process) || "system".equals(process)) {
                            systemTarget = target;
                        }
                    }
                    if (systemTarget != null) {
                        status = switch (systemTarget.getState()) {
                            case UP_TO_DATE -> R.string.scope_running;
                            case STALE -> R.string.scope_stale;
                            case RELOADING -> R.string.scope_reloading;
                            case FAILED -> R.string.scope_reload_failed;
                        };
                        frameworkStatus.setText(getString(R.string.framework_target_detail,
                                getPackageName(), service.getFrameworkName(), api,
                                getString(status), systemTarget.getProcessName(),
                                systemTarget.getPid()));
                        renderControls();
                        return;
                    }
                }
            }
            frameworkStatus.setText(getString(R.string.framework_detail,
                    getPackageName(), service.getFrameworkName(), api, getString(status)));
        } catch (RuntimeException error) {
            canRequestSystemScope = false;
            frameworkStatus.setText(getString(R.string.framework_query_failed,
                    getPackageName(), safeMessage(error)));
        }
        renderControls();
    }

    private static String buildSummary(RuntimeState state) {
        if (!state.root) return "Root 无法确认，运行状态未知。\n" + state.raw;
        if (!state.controllerVerified) return "当前 APK 的控制器未校验，运行状态未知。";
        return "输入源：" + sourceLabel(state)
                + "\n全局相机：" + cameraState(state, state.cameraEnabled)
                + "\n照片替换：" + photoState(state)
                + "\n虚拟麦克风：" + microphoneState(state)
                + (!state.cameraEnabled && !state.photoEnabled && !state.cameraRuntimeStock
                    ? "\n原生相机服务恢复尚未确认。" : "")
                + (!state.cameraEnabled && !state.photoEnabled && !state.micEnabled && !state.feedersIdle
                    ? "\n馈送进程退出尚未确认。" : "")
                + (!state.cameraEnabled && !state.photoEnabled && !state.micEnabled && !state.modeMarkersClear
                    ? "\n检测到残留开关记录，等待清理。" : "")
                + cameraOutputSummary(state);
    }

    private static String cameraState(RuntimeState state, boolean enabled) {
        if (!enabled) return "关闭";
        if (!state.videoFeederReady) return state.visualUsesObs()
                ? "等待启动电脑拉流" : "等待本地媒体解码器";
        if (!state.cameraHookReady) return "等待 cameraserver hook";
        if (!state.videoChannelReady) return state.visualUsesObs()
                ? "等待新的 OBS H.264 帧，当前使用原生画面" : "等待本地视频帧";
        return "载荷已接入且有输入帧";
    }

    private static String photoState(RuntimeState state) {
        if (!state.photoEnabled && state.cameraEnabled) {
            return "关闭，预览可替换，照片仍使用原生相机";
        }
        if (!state.photoEnabled || !state.cameraHookReady || !state.videoChannelReady) {
            return cameraState(state, state.photoEnabled);
        }
        if (state.cameraQueuedJpeg > 0) return "已向相机提交替换 JPEG";
        if (state.cameraQueuedRawTen > 0) return "已向相机提交替换 RAW10 帧";
        return "等待相机请求兼容的照片输出";
    }

    private static String cameraOutputSummary(RuntimeState state) {
        if ((!state.cameraEnabled && !state.photoEnabled) || !state.cameraHookReady)
            return "";
        boolean direct = state.hasCameraOutputStats();
        boolean preview = state.hasCameraPreviewStats();
        if (!direct && !preview)
            return "\n\n尚无相机输出记录。打开相机后返回刷新状态。";
        StringBuilder summary = new StringBuilder("\n");
        if (direct) {
            summary.append("\n直接输出写入：YUV ").append(state.cameraWrittenYuv)
                    .append(" 帧，JPEG ").append(state.cameraWrittenJpeg).append(" 张。");
            if (state.hasCameraQueueStats()) {
                summary.append("\n系统队列接受：YUV ").append(state.cameraQueuedYuv)
                        .append(" 帧，JPEG ").append(state.cameraQueuedJpeg)
                        .append(" 张。提交失败 ").append(state.cameraQueueErrors).append(" 次。");
            }
            if (state.cameraRawTen > 0) {
                summary.append("\nRAW10 传感器帧直通 ").append(state.cameraRawTen)
                        .append(" 次。");
            }
            long otherFormats = state.cameraUnsupported - state.cameraRawTen;
            if (otherFormats > 0) {
                summary.append("\n其他不支持格式直通 ").append(otherFormats).append(" 次。");
            }
        }
        if (preview) {
            summary.append("\n取景缓冲接收 ").append(state.cameraPreviewStaged)
                    .append(" 帧（共 ").append(state.cameraPreviewSeen).append(" 次回调，提交失败 ")
                    .append(state.cameraPreviewErrors).append(" 次）。");
        }
        if (state.cameraRecordingQueued > 0) {
            summary.append("\n录像编码队列累计接收 ").append(state.cameraRecordingQueued)
                    .append(" 帧替换视频。");
        }
        if (state.cameraWrittenRawTen >= 0) {
            summary.append("\nRAW10 写入 ").append(state.cameraWrittenRawTen)
                    .append(" 帧，系统队列接受 ").append(state.cameraQueuedRawTen)
                    .append(" 帧，缓冲锁定失败 ").append(state.cameraRawTenLockErrors).append(" 次。");
        }
        return summary.toString();
    }

    private static String microphoneState(RuntimeState state) {
        if (!state.micEnabled) return state.audioRuntimeStock ? "关闭（手机原生麦克风）"
                : "关闭（原生音频服务恢复尚未确认）";
        if (!state.audioHookReady) return "等待 audioserver hook";
        if (!state.audioFeederReady) return state.audioUsesObs()
                ? "等待启动电脑拉流，输出静音" : "等待本地音频解码器，输出静音";
        if (!state.audioChannelReady) return state.audioUsesObs()
                ? "等待 OBS AAC 音频帧，输出静音" : "等待本地 PCM，输出静音";
        return "READY";
    }

    private String formatObsConfiguration(RuntimeState state) {
        boolean useVideo = state.visualUsesObs() && (state.cameraEnabled || state.photoEnabled);
        boolean useAudio = state.audioUsesObs() && state.micEnabled;
        String input;
        if (!useVideo && !useAudio) {
            input = "开启使用此来源的输出后，手机开始拉流。";
        } else if (!state.obsPullRunning) {
            input = "拉流进程未启动，请刷新状态。";
        } else {
            input = "H.264 " + (useVideo ? state.videoChannelReady ? "已收到" : "等待视频" : "未启用")
                    + " · AAC " + (useAudio ? state.audioChannelReady ? "已收到" : "等待音频" : "未启用");
        }
        return getString(R.string.obs_address_configured) + "\n" + input;
    }

    private static String sourceLabel(RuntimeState state) {
        if (state.visualUsesObs()) return "电脑 OBS 流";
        if (state.audioUsesObs()) return "本地画面 / OBS 音频";
        if (!state.visualSourceReady && !state.audioSourceReady) return "手机本地（未配置）";
        return "手机本地媒体";
    }

    private static String label(RootController.Action action) {
        return switch (action) {
            case ARM_CAMERA, DISARM_CAMERA -> "全局相机";
            case ARM_PHOTO, DISARM_PHOTO -> "照片替换";
            case ARM_MIC, DISARM_MIC -> "虚拟麦克风";
            case STOP_ALL -> "全部替换";
        };
    }

    private String safeMessage(Throwable error) {
        if (error instanceof RootController.CameraActionException cameraError) {
            return getString(cameraError.messageResource);
        }
        String message = error.getMessage();
        if (message == null || message.isBlank()) message = error.getClass().getSimpleName();
        message = RootShell.redactDiagnostic(message);
        return message.length() > 1600 ? message.substring(0, 1600) + "…" : message;
    }

    @FunctionalInterface
    private interface StateOperation {
        RuntimeState run() throws Exception;
    }
}
