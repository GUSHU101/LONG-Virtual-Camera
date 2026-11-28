package com.gushu.longvc;

import android.content.Context;
import android.net.Uri;

import java.io.IOException;

final class RootController {
    enum Action {
        ARM_CAMERA("arm-camera", true),
        DISARM_CAMERA("disarm-camera", false),
        ARM_PHOTO("arm-photo", true),
        DISARM_PHOTO("disarm-photo", false),
        ARM_MIC("arm-mic", true),
        DISARM_MIC("disarm-mic", false),
        STOP_ALL("stop-all", false);

        final String command;
        final boolean requiresNativePayload;

        Action(String command, boolean requiresNativePayload) {
            this.command = command;
            this.requiresNativePayload = requiresNativePayload;
        }
    }

    static final class CameraActionException extends IOException {
        final int messageResource;

        CameraActionException(String message, int messageResource) {
            super(message);
            this.messageResource = messageResource;
        }
    }

    private final RootShell shell = new RootShell();
    private final PayloadInstaller installer;
    private final MediaSourceManager mediaSources;

    RootController(Context context) {
        installer = new PayloadInstaller(context, shell);
        mediaSources = new MediaSourceManager(context, shell);
    }

    RuntimeState requestRootAndReadStatus() throws IOException, InterruptedException {
        RootShell.Result root = shell.execute("id -u", 15L);
        if (!root.ok() || !"0".equals(root.output.trim())) {
            return RuntimeState.noRoot(root.describe());
        }
        return status();
    }

    RuntimeState installAndReadStatus() throws IOException, InterruptedException {
        installer.install();
        return status();
    }

    RuntimeState run(Action action) throws IOException, InterruptedException {
        if (action.requiresNativePayload) {
            if (!installer.isCurrent()) installer.install();
        } else {
            // An emergency disable/restore must not depend on healthy native
            // binaries. Repair just the audited control script if necessary.
            installer.ensureControlScript();
        }
        RootShell.Result result = shell.execute(
                PayloadInstaller.REMOTE_SCRIPT + " " + action.command, 45L);
        if (!result.ok() || !result.hasLine("result=ok")
                || !result.hasLine("action=" + action.command)) {
            String cameraError = switch (action) {
                case ARM_CAMERA -> "camera_arm_failed";
                case ARM_PHOTO -> "photo_arm_failed";
                default -> null;
            };
            if (cameraError != null) {
                if (result.reportsFailure(cameraError, "camera_recovery=unverified")) {
                    throw new CameraActionException("Camera recovery could not be verified",
                            R.string.camera_recovery_failed);
                }
                if (result.reportsFailure(cameraError, "cr_injector: FAIL rc=17")) {
                    throw new CameraActionException("Camera module initialization failed (rc=17)",
                            R.string.camera_initialization_failed);
                }
            }
            throw new IOException(action.command + " failed: " + result.describe());
        }
        RuntimeState state = status();
        if (action == Action.STOP_ALL && !state.isStopped()) {
            throw new IOException("stop-all returned but the runtime is still active or unverified");
        }
        return state;
    }

    RuntimeState stageVideo(Uri uri) throws IOException, InterruptedException {
        return replaceMedia(mediaSources.prepareVideo(uri));
    }

    RuntimeState stageImage(Uri uri) throws IOException, InterruptedException {
        return replaceMedia(mediaSources.prepareImage(uri));
    }

    RuntimeState stageAudio(Uri uri) throws IOException, InterruptedException {
        return replaceMedia(mediaSources.prepareAudio(uri));
    }

    private RuntimeState replaceMedia(MediaSourceManager.PreparedMedia media)
            throws IOException, InterruptedException {
        try (media) {
            return replaceSource(() -> mediaSources.commitMedia(media));
        }
    }

    RuntimeState configureObs(String url, boolean obsAudio)
            throws IOException, InterruptedException {
        if (mediaSources.obsSettingsMatch(url, obsAudio)) return status();
        return replaceSource(() -> mediaSources.configureObs(url, obsAudio));
    }

    RuntimeState status() throws IOException, InterruptedException {
        // Never execute an unknown/corrupt file as root merely to read status.
        // A degraded but rooted fallback keeps repair and emergency-stop UI
        // available instead of misreporting the device as non-rooted.
        boolean scriptCurrent = installer.isControlScriptCurrent();
        String command;
        if (scriptCurrent) {
            command = PayloadInstaller.REMOTE_SCRIPT + " status";
        } else {
            command = "echo schema=1; echo root=1; echo installed=0; "
                    + "echo source_mode=unknown; echo visual_source_ready=0; "
                    + "echo visual_mode=local; echo audio_mode=local; "
                    + "echo visual_kind=none; "
                    + "echo audio_source_ready=0; echo obs_configured=0; "
                    + "echo obs_pull_running=0; "
                    + "echo camera_enabled=0; echo photo_enabled=0; echo mic_enabled=0; "
                    + "echo video_feeder_ready=0; echo audio_feeder_ready=0; "
                    + "echo camera_hook_ready=0; echo audio_hook_ready=0; "
                    + "echo video_channel_ready=0; echo audio_channel_ready=0; "
                    + "echo abi=$(getprop ro.product.cpu.abi 2>/dev/null); "
                    + "echo selinux=$(getenforce 2>/dev/null); echo result=degraded";
        }
        RootShell.Result result = shell.execute(command, 15L);
        if (!result.ok()) {
            throw new IOException("controller status unavailable: " + result.describe());
        }
        boolean hashesCurrent = installer.isCurrent();
        return RuntimeState.parseController(result.output, scriptCurrent, hashesCurrent);
    }

    private RuntimeState replaceSource(SourceMutation mutation)
            throws IOException, InterruptedException {
        // Source staging only needs root, but status and rollback must still be
        // driven by the exact audited controller script bundled in this APK.
        installer.ensureControlScript();
        RuntimeState before = status();
        boolean active = before.cameraEnabled || before.photoEnabled || before.micEnabled;
        if (active) run(Action.STOP_ALL);
        try {
            mutation.apply();
        } catch (InterruptedException cancelled) {
            // Emergency stop deliberately interrupts media staging. Restoring
            // the old leases here would race recovery and re-arm replacement.
            Thread.currentThread().interrupt();
            throw cancelled;
        } catch (MediaSourceManager.SourceConflictException conflict) {
            // A concurrent controller owns the new state. Do not re-arm from
            // the snapshot that failed the compare-and-replace.
            throw conflict;
        } catch (IOException failure) {
            if (active && !Thread.currentThread().isInterrupted()) restoreBestEffort(before, failure);
            throw failure;
        } catch (RuntimeException failure) {
            if (active && !Thread.currentThread().isInterrupted()) restoreBestEffort(before, failure);
            throw failure;
        }

        try {
            restore(before, status());
            return status();
        } catch (InterruptedException cancelled) {
            Thread.currentThread().interrupt();
            throw cancelled;
        } catch (IOException failure) {
            try {
                run(Action.STOP_ALL);
            } catch (IOException | InterruptedException | RuntimeException cleanup) {
                if (cleanup instanceof InterruptedException) Thread.currentThread().interrupt();
                failure.addSuppressed(cleanup);
            }
            throw failure;
        }
    }

    private void restore(RuntimeState state, RuntimeState source)
            throws IOException, InterruptedException {
        RootShell.checkCancellation();
        if (state.cameraEnabled && source.visualSourceReady) run(Action.ARM_CAMERA);
        if (state.photoEnabled && source.visualSourceReady) run(Action.ARM_PHOTO);
        if (state.micEnabled && source.audioSourceReady) run(Action.ARM_MIC);
    }

    private void restoreBestEffort(RuntimeState state, Throwable failure) {
        try {
            restore(state, state);
        } catch (IOException | InterruptedException | RuntimeException restoreFailure) {
            if (restoreFailure instanceof InterruptedException) Thread.currentThread().interrupt();
            failure.addSuppressed(restoreFailure);
        }
    }

    @FunctionalInterface
    private interface SourceMutation {
        void apply() throws IOException, InterruptedException;
    }
}
