package com.gushu.longvc;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.format.DateFormat;
import android.view.View;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Collections;
import java.util.Date;
import java.util.Locale;
import java.util.function.Consumer;

import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.HookedTarget;

public final class MainActivity extends Activity
        implements LongVcApplication.ServiceStateListener {
    private static final int PICK_VIDEO = 1001;
    private static final int PICK_IMAGE = 1002;
    private static final int PICK_AUDIO = 1003;
    private static final String PICKER_OPEN = "document_picker_open";
    private static final String OBS_AUDIO_DRAFT = "obs_audio_draft";
    private static final String VISUAL_CHOICE = "visual_choice";
    private static final String OBS_AUDIO_ONLY = "obs_audio_only";
    private static final String APPEARANCE = "studio_appearance";
    private static final String LIGHT = "light";
    private static final long STATUS_MAX_AGE_MS = 15_000L;

    private enum VisualChoice {
        VIDEO(R.id.choose_video), IMAGE(R.id.choose_image), OBS(R.id.choose_obs);

        private final int viewId;
        VisualChoice(int viewId) { this.viewId = viewId; }
    }

    private RootController rootController;
    private StudioOperations operations;
    private final Consumer<StudioOperations.Snapshot> operationListener = this::renderOperation;
    private DashboardView dashboard;
    private StudioPreview studioPreview;
    private StudioEvents studioEvents;
    private XposedService xposedService;
    private TextView frameworkStatus;
    private TextView rootStatus;
    private TextView visualConfiguration;
    private TextView mediaAudioConfig;
    private TextView obsConfiguration;
    private TextView obsAddressGuidance;
    private TextView overviewState;
    private TextView overviewSource;
    private TextView overviewAudio;
    private TextView overviewBadge;
    private TextView overviewDescription;
    private TextView workbenchDevice;
    private TextView workbenchServices;
    private TextView monitorHealth;
    private TextView cameraStatus;
    private TextView photoStatus;
    private TextView micStatus;
    private TextView runtimeDetails;
    private TextView statusUpdated;
    private Button overviewAction;
    private Switch cameraSwitch;
    private Switch photoSwitch;
    private Switch micSwitch;
    private EditText obsUrlInput;
    private Switch obsAudioSwitch;
    private Button rootButton;
    private Button installButton;
    private Button scopeButton;
    private Button refreshButton;
    private Button selectVideoButton;
    private Button selectImageButton;
    private Button selectAudioButton;
    private Button configureObsButton;
    private Button phoneMicButton;
    private Button selectObsAudioButton;
    private View[] recoveryButtons;
    private VisualChoice visualChoice;
    private boolean sourcePanelChosen;
    private boolean rendering;
    private boolean busy;
    private boolean emergencyBusy;
    private boolean documentPickerOpen;
    private boolean obsAudioEdited;
    private boolean obsAudioOnly;
    private boolean canRequestSystemScope;
    private boolean destroyed;
    private RuntimeState lastState;

    @Override
    protected void attachBaseContext(Context base) {
        Configuration configuration = new Configuration(base.getResources().getConfiguration());
        boolean light = base.getSharedPreferences(APPEARANCE, MODE_PRIVATE).getBoolean(LIGHT, false);
        configuration.uiMode = (configuration.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
                | (light ? Configuration.UI_MODE_NIGHT_NO : Configuration.UI_MODE_NIGHT_YES);
        super.attachBaseContext(base.createConfigurationContext(configuration));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        documentPickerOpen = savedInstanceState != null
                && savedInstanceState.getBoolean(PICKER_OPEN);
        setContentView(R.layout.activity_main);
        studioEvents = new StudioEvents(this, savedInstanceState);
        studioPreview = new StudioPreview(this, this::refreshOnEntry);
        dashboard = new DashboardView(this, savedInstanceState, studioPreview::stop);
        rootController = new RootController(getApplicationContext());
        operations = ((LongVcApplication) getApplication()).studioOperations();

        frameworkStatus = findViewById(R.id.framework_status);
        rootStatus = findViewById(R.id.root_status);
        visualConfiguration = findViewById(R.id.visual_configuration);
        mediaAudioConfig = findViewById(R.id.media_audio_config);
        obsConfiguration = findViewById(R.id.obs_configuration);
        obsAddressGuidance = findViewById(R.id.obs_address_guidance);
        overviewState = findViewById(R.id.overview_state);
        overviewSource = findViewById(R.id.overview_source);
        overviewAudio = findViewById(R.id.overview_audio);
        overviewBadge = findViewById(R.id.overview_badge);
        overviewDescription = findViewById(R.id.overview_description);
        workbenchDevice = findViewById(R.id.workbench_device);
        workbenchServices = findViewById(R.id.workbench_services);
        monitorHealth = findViewById(R.id.monitor_health);
        overviewAction = findViewById(R.id.overview_action);
        cameraStatus = findViewById(R.id.camera_status);
        photoStatus = findViewById(R.id.photo_status);
        micStatus = findViewById(R.id.mic_status);
        runtimeDetails = findViewById(R.id.runtime_details);
        statusUpdated = findViewById(R.id.status_updated);
        cameraSwitch = findViewById(R.id.camera_switch);
        photoSwitch = findViewById(R.id.photo_switch);
        micSwitch = findViewById(R.id.mic_switch);
        obsUrlInput = findViewById(R.id.obs_connection_input);
        if (getLastNonConfigurationInstance() instanceof String addressDraft) {
            obsUrlInput.setText(addressDraft);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            obsUrlInput.setImportantForContentCapture(View.IMPORTANT_FOR_CONTENT_CAPTURE_NO);
        }
        obsAudioSwitch = findViewById(R.id.obs_audio_switch);
        if (savedInstanceState != null && savedInstanceState.containsKey(OBS_AUDIO_DRAFT)) {
            obsAudioEdited = true;
            obsAudioSwitch.setChecked(savedInstanceState.getBoolean(OBS_AUDIO_DRAFT));
        }
        rootButton = findViewById(R.id.root_button);
        installButton = findViewById(R.id.install_button);
        recoveryButtons = new View[]{findViewById(R.id.stop_button), findViewById(R.id.header_stop_button),
                findViewById(R.id.monitor_stop_button), findViewById(R.id.settings_stop_button)};
        scopeButton = findViewById(R.id.scope_button);
        refreshButton = findViewById(R.id.refresh_button);
        selectVideoButton = findViewById(R.id.select_video_button);
        selectImageButton = findViewById(R.id.select_image_button);
        selectAudioButton = findViewById(R.id.select_audio_button);
        configureObsButton = findViewById(R.id.configure_obs_button);
        phoneMicButton = findViewById(R.id.phone_mic_button);
        selectObsAudioButton = findViewById(R.id.select_obs_audio);

        for (VisualChoice choice : VisualChoice.values()) {
            findViewById(choice.viewId).setOnClickListener(view -> {
                sourcePanelChosen = true;
                obsAudioOnly = false;
                showVisualChoice(choice);
            });
        }
        VisualChoice restoredChoice = VisualChoice.VIDEO;
        if (savedInstanceState != null && savedInstanceState.containsKey(VISUAL_CHOICE)) {
            sourcePanelChosen = true;
            obsAudioOnly = savedInstanceState.getBoolean(OBS_AUDIO_ONLY);
            int savedChoice = savedInstanceState.getInt(VISUAL_CHOICE, 0);
            if (savedChoice >= 0 && savedChoice < VisualChoice.values().length) {
                restoredChoice = VisualChoice.values()[savedChoice];
            }
        }
        showVisualChoice(restoredChoice);

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
        obsAudioSwitch.setOnCheckedChangeListener((button, checked) -> {
            if (!rendering) obsAudioEdited = true;
        });

        rootButton.setOnClickListener(view -> runOperation("正在请求 root…",
                RootController::requestRootAndReadStatus));
        installButton.setOnClickListener(view -> runOperation("正在校验并安装 ARM64 载荷…",
                RootController::installAndReadStatus));
        for (View button : recoveryButtons) button.setOnClickListener(view -> confirmStop());
        refreshButton.setOnClickListener(view -> {
            renderFramework();
            runOperation(getString(R.string.runtime_read_pending), RootController::requestRootAndReadStatus);
        });
        scopeButton.setOnClickListener(view -> requestSystemScope());
        selectVideoButton.setOnClickListener(view -> selectDocument("video/*", PICK_VIDEO));
        selectImageButton.setOnClickListener(view -> selectDocument("image/*", PICK_IMAGE));
        selectAudioButton.setOnClickListener(view -> selectDocument("audio/*", PICK_AUDIO));
        phoneMicButton.setOnClickListener(view -> runToggle(
                micSwitch, false, RootController.Action.ARM_MIC, RootController.Action.DISARM_MIC));
        selectObsAudioButton.setOnClickListener(view -> {
            if (lastState == null || !lastState.obsConfigured) {
                sourcePanelChosen = true;
                obsAudioOnly = true;
                showVisualChoice(VisualChoice.OBS);
                scrollToObsSettings();
                return;
            }
            runOperation(new StudioOperations.Request(getString(R.string.select_obs_audio_pending),
                    StudioOperations.Effect.OBS_AUDIO_SELECTED, null),
                    controller -> controller.configureObsAudio(""));
        });
        configureObsButton.setOnClickListener(view -> {
            String url = obsUrlInput.getText().toString().trim();
            boolean missingSavedUrl = lastState != null && lastState.controllerVerified
                    && !lastState.obsConfigured;
            if (url.isEmpty() ? missingSavedUrl : !MediaSourceManager.validObsUrl(url)) {
                obsUrlInput.setError(getString(R.string.obs_address_invalid));
                obsUrlInput.requestFocus();
                return;
            }
            obsUrlInput.setError(null);
            boolean useObsAudio = obsAudioSwitch.isChecked();
            boolean audioOnly = obsAudioOnly;
            runOperation(new StudioOperations.Request(getString(R.string.save_obs_pending),
                    StudioOperations.Effect.OBS_SAVED, null),
                    controller -> audioOnly ? controller.configureObsAudio(url)
                            : controller.configureObs(url, useObsAudio));
        });
        overviewAction.setOnClickListener(view -> dashboard.show(DashboardView.Page.SOURCES));
        findViewById(R.id.device_details).setOnClickListener(view -> dashboard.openDiagnostics());
        bindSettings();

        renderState(null);
    }

    @Override
    protected void onStart() {
        super.onStart();
        LongVcApplication.addServiceStateListener(this, true);
        StudioOperations.Snapshot latest = operations.latest();
        boolean hasUnseenOperation = operations.hasUnclaimedCompletion();
        operations.attach(operationListener);
        // The picker result may arrive after onStart. A background refresh
        // cannot take the operation slot from the pending user import.
        if (!documentPickerOpen && !busy && !emergencyBusy && !hasUnseenOperation
                && needsForegroundRefresh(latest)) refreshOnEntry();
    }

    private static boolean needsForegroundRefresh(StudioOperations.Snapshot latest) {
        if (latest == null) return true;
        if (latest.pending()) return false;
        long age = System.currentTimeMillis() - latest.outcome().readAt();
        return age < 0 || age >= STATUS_MAX_AGE_MS;
    }

    private void refreshOnEntry() {
        runOperation("正在检查 LSPosed 与 root 状态…", RootController::requestRootAndReadStatus);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        outState.putBoolean(PICKER_OPEN, documentPickerOpen);
        if (obsAudioEdited) outState.putBoolean(OBS_AUDIO_DRAFT, obsAudioSwitch.isChecked());
        if (sourcePanelChosen) {
            outState.putInt(VISUAL_CHOICE, visualChoice.ordinal());
            outState.putBoolean(OBS_AUDIO_ONLY, obsAudioOnly);
        }
        dashboard.save(outState);
        studioEvents.save(outState);
        super.onSaveInstanceState(outState);
    }

    @Override
    @SuppressWarnings("deprecation")
    public Object onRetainNonConfigurationInstance() {
        // Keep an unsubmitted address through theme and rotation changes without saving it to disk.
        return obsUrlInput.getText().toString();
    }

    @Override
    protected void onStop() {
        LongVcApplication.removeServiceStateListener(this);
        operations.detach(operationListener);
        studioPreview.stop();
        super.onStop();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        dashboard.close();
        studioPreview.close();
        super.onDestroy();
    }

    @Override
    // DashboardView registers the predictive-back callback from API 33.
    @SuppressLint("GestureBackNavigation")
    @SuppressWarnings("deprecation")
    public void onBackPressed() {
        if (!dashboard.returnToControl()) super.onBackPressed();
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
            showNotice(DashboardView.Notice.ERROR, R.string.operation_attention,
                    getString(R.string.operation_failed, safeMessage(error)));
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
            case PICK_VIDEO -> runOperation(new StudioOperations.Request("正在校验并导入本地视频…",
                            StudioOperations.Effect.VIDEO_IMPORTED, uri.toString()),
                    controller -> controller.stageVideo(uri));
            case PICK_IMAGE -> runOperation(new StudioOperations.Request("正在解码并导入本地图片…",
                            StudioOperations.Effect.IMAGE_IMPORTED, uri.toString()),
                    controller -> controller.stageImage(uri));
            case PICK_AUDIO -> runOperation(new StudioOperations.Request("正在校验并导入本地音频…",
                            StudioOperations.Effect.AUDIO_IMPORTED, null),
                    controller -> controller.stageAudio(uri));
            default -> { }
        }
    }

    private void announceMediaSaved() {
        Toast.makeText(this, R.string.media_saved, Toast.LENGTH_SHORT).show();
    }

    @Override
    public void onServiceStateChanged(XposedService service) {
        xposedService = service;
        runOnUiThreadIfAlive(this::renderFramework);
    }

    private void runToggle(CompoundButton button, boolean checked,
                           RootController.Action on, RootController.Action off) {
        // Reflect the last observed device state until the operation completes.
        rendering = true;
        button.setChecked(!checked);
        rendering = false;
        RootController.Action action = checked ? on : off;
        runOperation((checked ? "正在启用 " : "正在停用 ") + label(action) + "…",
                controller -> controller.run(action));
    }

    private void confirmStop() {
        if (emergencyBusy) return;
        new AlertDialog.Builder(this)
                .setTitle(R.string.stop_confirmation_title)
                .setMessage(R.string.stop_confirmation_message)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.confirm_stop, (dialog, which) -> {
                    studioPreview.stop();
                    runEmergencyStop();
                })
                .show();
    }

    private void bindSettings() {
        SharedPreferences appearance = getSharedPreferences(APPEARANCE, MODE_PRIVATE);
        boolean light = appearance.getBoolean(LIGHT, false);
        TextView themeValue = findViewById(R.id.theme_value);
        themeValue.setText(light ? R.string.theme_light : R.string.theme_dark);
        View.OnClickListener changeTheme = view -> {
            if (busy || emergencyBusy) return;
            appearance.edit().putBoolean(LIGHT, !light).apply();
            recreate();
        };
        findViewById(R.id.change_theme).setOnClickListener(changeTheme);
        findViewById(R.id.theme_action).setOnClickListener(changeTheme);
        findViewById(R.id.settings_authorization).setOnClickListener(
                view -> dashboard.openDiagnosticsAt(R.id.authorization_card));
        findViewById(R.id.settings_components).setOnClickListener(
                view -> dashboard.openDiagnosticsAt(R.id.runtime_card));
        findViewById(R.id.settings_events).setOnClickListener(
                view -> dashboard.openDiagnosticsAt(R.id.events_card));
        findViewById(R.id.settings_proxy).setOnClickListener(
                view -> startActivity(new Intent(this, ProxyActivity.class)));
        TextView version = findViewById(R.id.studio_version);
        try {
            version.setText(getString(R.string.studio_version,
                    getPackageManager().getPackageInfo(getPackageName(), 0).versionName));
        } catch (PackageManager.NameNotFoundException unavailable) {
            version.setVisibility(View.GONE);
        }
    }

    private void showVisualChoice(VisualChoice choice) {
        visualChoice = choice;
        for (VisualChoice candidate : VisualChoice.values()) {
            findViewById(candidate.viewId).setSelected(candidate == choice);
        }
        findViewById(R.id.local_source_panel).setVisibility(choice == VisualChoice.OBS ? View.GONE : View.VISIBLE);
        findViewById(R.id.obs_source_panel).setVisibility(choice == VisualChoice.OBS ? View.VISIBLE : View.GONE);
        selectVideoButton.setVisibility(choice == VisualChoice.VIDEO ? View.VISIBLE : View.GONE);
        selectImageButton.setVisibility(choice == VisualChoice.IMAGE ? View.VISIBLE : View.GONE);
        obsAudioSwitch.setVisibility(obsAudioOnly ? View.GONE : View.VISIBLE);
        configureObsButton.setText(obsAudioOnly ? R.string.save_obs_audio : R.string.configure_obs);
        renderVisualConfiguration(lastState);
    }

    private void scrollToObsSettings() {
        ScrollView page = findViewById(R.id.source_page);
        View target = findViewById(R.id.obs_source_panel);
        page.post(() -> page.smoothScrollTo(0, target.getTop()));
    }

    private void runOperation(String pending, ControllerOperation operation) {
        runOperation(new StudioOperations.Request(pending), operation);
    }

    private void runOperation(StudioOperations.Request request, ControllerOperation operation) {
        if (busy || emergencyBusy) {
            Toast.makeText(this, "已有操作正在执行", Toast.LENGTH_SHORT).show();
            renderState(lastState);
            return;
        }
        RootController controller = rootController;
        boolean accepted = operations.start(request, false, () -> operation.run(controller),
                controller::requestRootAndReadStatus);
        if (!accepted) {
            showNotice(DashboardView.Notice.INFO, R.string.root_operation_in_progress, "");
            renderState(lastState);
        }
    }

    private void runEmergencyStop() {
        if (emergencyBusy) return;
        RootController controller = rootController;
        boolean accepted = operations.start(
                new StudioOperations.Request(getString(R.string.recovery_in_progress)), true,
                () -> controller.run(RootController.Action.STOP_ALL), controller::requestRootAndReadStatus);
        if (!accepted) {
            showNotice(DashboardView.Notice.INFO, R.string.recovery_in_progress, "");
        }
    }

    private void renderOperation(StudioOperations.Snapshot snapshot) {
        busy = snapshot.pending() && !snapshot.recovery();
        emergencyBusy = snapshot.pending() && snapshot.recovery();
        if (snapshot.pending()) {
            dashboard.notice(DashboardView.Notice.BUSY, snapshot.request().label(), "");
            renderControls();
            return;
        }

        StudioOperations.Outcome outcome = snapshot.outcome();
        boolean once = operations.claimCompletion(snapshot);
        if (outcome.failure() == null && once) applyOperationSuccess(snapshot.request(), outcome.state());
        publishState(outcome.state(), outcome.readAt());
        if (outcome.failure() != null) {
            renderOperationFailure(snapshot.recovery(), outcome, once);
        } else if (snapshot.recovery()) {
            String details = getString(R.string.recovery_verified);
            dashboard.notice(DashboardView.Notice.INFO, getString(R.string.operation_restored), details);
            if (once) studioEvents.add(StudioEvents.Level.INFO, details);
        } else {
            dashboard.notice(DashboardView.Notice.NONE, "", "");
            if (once) recordOperation(outcome.state(), snapshot.request().label());
        }
    }

    private void applyOperationSuccess(StudioOperations.Request request, RuntimeState state) {
        switch (request.effect()) {
            case VIDEO_IMPORTED, IMAGE_IMPORTED -> {
                studioPreview.remember(Uri.parse(request.document()), state);
                announceMediaSaved();
            }
            case AUDIO_IMPORTED -> announceMediaSaved();
            case OBS_SAVED -> {
                obsAudioEdited = false;
                obsUrlInput.getText().clear();
            }
            case OBS_AUDIO_SELECTED -> obsAudioEdited = false;
            case NONE -> { }
        }
    }

    private void renderOperationFailure(boolean recovery, StudioOperations.Outcome outcome, boolean once) {
        String message = safeMessage(outcome.failure());
        String details = getString(recovery ? R.string.recovery_unverified : R.string.operation_failed, message);
        if (outcome.state() == null) {
            statusUpdated.setText(R.string.runtime_read_failed);
            rootStatus.setText(R.string.root_unconfirmed);
            String readError = outcome.statusFailure() == null ? "" : safeMessage(outcome.statusFailure());
            details += "\n\n" + getString(R.string.runtime_state_unknown, readError.equals(message) ? "" : readError);
        }
        dashboard.notice(DashboardView.Notice.ERROR, getString(R.string.operation_attention), details);
        if (once) studioEvents.add(StudioEvents.Level.ERROR, details);
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
                            if (approved.contains("system")) {
                                showNotice(DashboardView.Notice.INFO, R.string.scope_approved_short,
                                        getString(R.string.scope_approved, getPackageName()));
                            } else {
                                showScopeFailure(approved.toString());
                            }
                            renderFramework();
                        });
                    }

                    @Override
                    public void onScopeRequestFailed(String message) {
                        runOnUiThreadIfAlive(() -> showScopeFailure(message));
                    }
                });
        } catch (RuntimeException error) {
            showScopeFailure(safeMessage(error));
            renderFramework();
        }
    }

    private void showScopeFailure(String message) {
        showNotice(DashboardView.Notice.ERROR, R.string.operation_attention,
                getString(R.string.scope_failed, RootShell.redactDiagnostic(message)));
    }

    private void showNotice(DashboardView.Notice notice, int message, CharSequence details) {
        dashboard.notice(notice, getString(message), details);
        if (notice == DashboardView.Notice.ERROR) {
            studioEvents.add(StudioEvents.Level.ERROR, details);
        } else if (notice == DashboardView.Notice.INFO) {
            studioEvents.add(details.length() == 0 ? StudioEvents.Level.WARNING : StudioEvents.Level.INFO,
                    details.length() == 0 ? getString(message) : details);
        }
    }

    private void recordOperation(RuntimeState state, String operation) {
        if (!state.root) {
            studioEvents.add(StudioEvents.Level.WARNING, getString(R.string.overview_root_required));
        } else if (!state.controllerVerified) {
            studioEvents.add(StudioEvents.Level.WARNING, getString(R.string.overview_verify_payload));
        } else {
            studioEvents.add(StudioEvents.Level.INFO, getString(R.string.event_operation_completed,
                    operation.replaceFirst("^正在", "").replaceFirst("…$", "")));
        }
    }

    private void publishState(RuntimeState state, long readAt) {
        lastState = state;
        if (state == null) {
            statusUpdated.setText(R.string.status_not_read);
        } else {
            statusUpdated.setText(getString(R.string.status_updated,
                    DateFormat.getTimeFormat(this).format(new Date(readAt))));
        }
        renderState(state);
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
        renderOverview(state);
        studioPreview.render(state);
        if (state == null) runtimeDetails.setText(R.string.overview_unknown);
        else runtimeDetails.setText(buildSummary(state));
        rendering = false;
        renderControls();
    }

    private void renderControls() {
        boolean rootReady = lastState != null && lastState.root;
        boolean visualReady = lastState != null && lastState.visualSourceReady;
        boolean audioReady = lastState != null && lastState.audioSourceReady;
        boolean blocked = busy || emergencyBusy;
        boolean canStartCamera = visualReady && !RootController.isCameraStartPaused();
        cameraSwitch.setEnabled(!blocked && rootReady &&
                (canStartCamera || lastState.cameraEnabled));
        photoSwitch.setEnabled(!blocked && rootReady &&
                (canStartCamera || lastState.photoEnabled));
        micSwitch.setEnabled(!blocked && rootReady &&
                (audioReady || lastState.micEnabled));
        rootButton.setEnabled(!blocked);
        installButton.setEnabled(!blocked && rootReady);
        // Emergency recovery remains actionable during a long media import.
        for (View button : recoveryButtons) button.setEnabled(!emergencyBusy);
        refreshButton.setEnabled(!blocked);
        scopeButton.setEnabled(!blocked && canRequestSystemScope);
        selectVideoButton.setEnabled(!blocked && rootReady);
        selectImageButton.setEnabled(!blocked && rootReady);
        selectAudioButton.setEnabled(!blocked && rootReady);
        configureObsButton.setEnabled(!blocked && rootReady);
        obsUrlInput.setEnabled(!blocked && rootReady);
        obsAudioSwitch.setEnabled(!blocked && rootReady);
        phoneMicButton.setEnabled(!blocked && rootReady && lastState.micEnabled);
        phoneMicButton.setText(rootReady && !lastState.micEnabled && lastState.audioRuntimeStock
                ? R.string.studio_in_use : R.string.studio_use_native);
        selectObsAudioButton.setEnabled(!blocked && rootReady
                && (!lastState.audioUsesObs() || !lastState.obsConfigured));
        selectObsAudioButton.setText(rootReady && lastState.audioUsesObs() && lastState.obsConfigured
                ? R.string.studio_selected : R.string.studio_select);
        studioPreview.setBusy(blocked);
        findViewById(R.id.theme_action).setEnabled(!blocked);
        findViewById(R.id.change_theme).setEnabled(!blocked);
    }

    private void renderSource(RuntimeState state) {
        obsAddressGuidance.setText(state != null && state.obsConfigured
                ? R.string.obs_url_saved_hint : R.string.obs_url_hint);
        renderVisualConfiguration(state);
        if (state == null || !state.root || !state.controllerVerified) {
            mediaAudioConfig.setText(state == null ? R.string.overview_unknown
                    : state.root ? R.string.source_controller_mismatch : R.string.source_needs_root);
            obsConfiguration.setText(R.string.obs_status_unknown);
            return;
        }
        if (!sourcePanelChosen) {
            showVisualChoice(state.visualUsesObs() ? VisualChoice.OBS
                    : "image".equals(state.visualKind) ? VisualChoice.IMAGE : VisualChoice.VIDEO);
        }
        String audio = configuredAudioLabel(state);
        mediaAudioConfig.setText(!state.audioSourceReady ? getString(R.string.microphone_replace_hint)
                : getString(state.micEnabled ? R.string.audio_selection : R.string.audio_selection_inactive, audio));
        if (!obsAudioEdited) obsAudioSwitch.setChecked(state.audioUsesObs());
        obsConfiguration.setText(state.obsConfigured
                ? formatObsConfiguration(state) : getString(R.string.obs_not_configured));
    }

    private void renderVisualConfiguration(RuntimeState state) {
        if (state == null || !state.root || !state.controllerVerified) {
            visualConfiguration.setText(state == null ? R.string.overview_unknown
                    : state.root ? R.string.source_controller_mismatch : R.string.source_needs_root);
            return;
        }
        boolean localSelected = state.visualSourceReady && !state.visualUsesObs()
                && ("image".equals(state.visualKind) == (visualChoice == VisualChoice.IMAGE));
        visualConfiguration.setText(localSelected
                ? getString(R.string.visual_file_selected, visualSourceLabel(state))
                : getString(R.string.studio_waiting_source));
    }

    private String configuredAudioLabel(RuntimeState state) {
        return getString(state.audioUsesObs() ? R.string.obs_audio
                : state.audioSourceReady ? R.string.local_audio : R.string.studio_native_mic);
    }

    private void renderOverview(RuntimeState state) {
        boolean verified = state != null && state.root && state.controllerVerified;
        int heading = R.string.overview_unknown;
        if (state != null && !state.root) heading = R.string.overview_root_required;
        else if (state != null && !verified) heading = R.string.overview_verify_payload;
        else if (verified) {
            if (RootController.isCameraStartPaused()) heading = R.string.overview_paused;
            else if (!state.visualSourceReady) heading = R.string.overview_select_source;
            else if (!state.cameraEnabled && !state.photoEnabled) heading = R.string.overview_standby;
            else if (!state.cameraHookReady) heading = R.string.overview_waiting_hook;
            else if (!state.videoFeederReady || !state.videoChannelReady) heading = R.string.overview_waiting_frames;
            else heading = R.string.overview_ready;
        }
        overviewState.setText(heading);
        boolean awaitingOutput = heading == R.string.overview_waiting_hook
                || heading == R.string.overview_waiting_frames;
        boolean ready = heading == R.string.overview_ready;
        int badge = !verified ? R.string.studio_status_unknown
                : ready ? R.string.studio_status_input
                : awaitingOutput || heading == R.string.overview_paused || !state.visualSourceReady
                    ? R.string.studio_status_waiting : R.string.studio_status_configured;
        overviewBadge.setText(badge);
        overviewBadge.setTextColor(getColor(ready ? R.color.accent_ink
                : badge == R.string.studio_status_waiting ? R.color.warning : R.color.ink_muted));
        overviewDescription.setText(!verified ? R.string.studio_state_description
                : ready ? R.string.studio_ready_hint
                : awaitingOutput ? R.string.studio_source_only : R.string.studio_outputs_idle);
        overviewSource.setText(verified ? visualSourceLabel(state) : getString(R.string.studio_status_unknown));
        overviewAudio.setText(!verified ? R.string.studio_status_unknown
                : state.micEnabled ? state.audioUsesObs() ? R.string.obs_audio : R.string.local_audio
                : R.string.studio_native_mic);
        cameraStatus.setVisibility(verified ? View.VISIBLE : View.GONE);
        photoStatus.setVisibility(verified ? View.VISIBLE : View.GONE);
        int device = verified && state.installed ? R.string.studio_auth_ready
                : state == null ? R.string.overview_unknown : state.root
                    ? R.string.overview_verify_payload : R.string.overview_root_required;
        monitorHealth.setText(device);
        renderDeviceAuthorization();
        int services = R.string.overview_unknown;
        if (verified) {
            if (state.cameraEnabled || state.photoEnabled || state.micEnabled) {
                services = R.string.studio_active_services;
            } else if (state.cameraRuntimeStock && state.audioRuntimeStock && state.feedersIdle
                    && state.modeMarkersClear) {
                services = R.string.studio_stock_services;
            }
        }
        workbenchServices.setText(services);
        if (verified) {
            cameraStatus.setText(state.cameraEnabled ? cameraState(state, true) : "使用手机相机");
            photoStatus.setText(state.photoEnabled ? photoState(state) : "使用手机相机");
            micStatus.setText(microphoneState(state));
        } else {
            micStatus.setText(R.string.microphone_replace_hint);
        }
        overviewAction.setText(verified && state.visualSourceReady
                ? R.string.change_source : R.string.open_source_settings);
    }

    private static String visualSourceLabel(RuntimeState state) {
        if (state.visualUsesObs()) return "电脑 OBS";
        if (!state.visualSourceReady) return "尚未选择";
        return "image".equals(state.visualKind) ? "本地图片" : "本地视频";
    }

    private void renderDeviceAuthorization() {
        RuntimeState state = lastState;
        int root = state == null ? R.string.overview_unknown
                : !state.root ? R.string.overview_root_required
                : !state.controllerVerified || !state.installed ? R.string.overview_verify_payload
                : R.string.studio_auth_ready;
        workbenchDevice.setText(getString(R.string.studio_auth_summary,
                getString(xposedService == null ? R.string.studio_framework_unknown
                        : R.string.studio_framework_connected), getString(root)));
    }

    private void renderFramework() {
        renderDeviceAuthorization();
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
                + (RootController.isCameraStartPaused()
                    ? "\n本次 App 运行的相机和照片替换已暂停。" : "")
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
        return "已收到音频";
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
    private interface ControllerOperation {
        RuntimeState run(RootController controller) throws Exception;
    }
}
