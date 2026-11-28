package com.gushu.longvc;

import java.io.IOException;
import java.util.LinkedHashMap;
import java.util.Map;

final class RuntimeState {
    final boolean root;
    final boolean installed;
    final boolean controllerVerified;
    final String sourceMode;
    final String visualMode;
    final String audioMode;
    final String visualKind;
    final boolean visualSourceReady;
    final boolean audioSourceReady;
    final boolean obsConfigured;
    final boolean obsPullRunning;
    final boolean cameraEnabled;
    final boolean photoEnabled;
    final boolean micEnabled;
    final boolean videoFeederReady;
    final boolean audioFeederReady;
    final boolean cameraHookReady;
    final boolean cameraRuntimeStock;
    final boolean audioHookReady;
    final boolean videoChannelReady;
    final boolean audioChannelReady;
    final long cameraWrittenYuv;
    final long cameraWrittenJpeg;
    final long cameraQueuedYuv;
    final long cameraQueuedJpeg;
    final long cameraRecordingQueued;
    final long cameraQueueErrors;
    final long cameraPreviewSeen;
    final long cameraPreviewStaged;
    final long cameraPreviewErrors;
    final long cameraRawTen;
    final long cameraWrittenRawTen;
    final long cameraQueuedRawTen;
    final long cameraRawTenLockErrors;
    final long cameraUnsupported;
    final String abi;
    final String selinux;
    final String raw;

    private RuntimeState(Map<String, String> values, String raw) {
        this.root = flag(values, "root");
        this.installed = flag(values, "installed");
        this.controllerVerified = flag(values, "controller_verified");
        this.sourceMode = values.getOrDefault("source_mode", "unknown");
        this.visualMode = values.getOrDefault("visual_mode", "local");
        this.audioMode = values.getOrDefault("audio_mode", "local");
        this.visualKind = values.getOrDefault("visual_kind", "none");
        this.visualSourceReady = flag(values, "visual_source_ready");
        this.audioSourceReady = flag(values, "audio_source_ready");
        this.obsConfigured = flag(values, "obs_configured");
        this.obsPullRunning = flag(values, "obs_pull_running");
        this.cameraEnabled = flag(values, "camera_enabled");
        this.photoEnabled = flag(values, "photo_enabled");
        this.micEnabled = flag(values, "mic_enabled");
        this.videoFeederReady = flag(values, "video_feeder_ready");
        this.audioFeederReady = flag(values, "audio_feeder_ready");
        this.cameraHookReady = flag(values, "camera_hook_ready");
        this.cameraRuntimeStock = "stock".equals(values.get("camera_runtime"));
        this.audioHookReady = flag(values, "audio_hook_ready");
        this.videoChannelReady = flag(values, "video_channel_ready");
        this.audioChannelReady = flag(values, "audio_channel_ready");
        this.cameraWrittenYuv = counter(values, "camera_written_yuv");
        this.cameraWrittenJpeg = counter(values, "camera_written_jpeg");
        this.cameraQueuedYuv = counter(values, "camera_queued_yuv");
        this.cameraQueuedJpeg = counter(values, "camera_queued_jpeg");
        this.cameraRecordingQueued = counter(values, "camera_recording_queued");
        this.cameraQueueErrors = counter(values, "camera_queue_errors");
        this.cameraPreviewSeen = counter(values, "camera_preview_seen");
        this.cameraPreviewStaged = counter(values, "camera_preview_staged");
        this.cameraPreviewErrors = counter(values, "camera_preview_errors");
        this.cameraRawTen = counter(values, "camera_raw_ten");
        this.cameraWrittenRawTen = counter(values, "camera_written_raw_ten");
        this.cameraQueuedRawTen = counter(values, "camera_queued_raw_ten");
        this.cameraRawTenLockErrors = counter(values, "camera_raw_ten_lock_errors");
        this.cameraUnsupported = counter(values, "camera_unsupported");
        this.abi = values.getOrDefault("abi", "unknown");
        this.selinux = values.getOrDefault("selinux", "unknown");
        this.raw = raw;
    }

    static RuntimeState parseController(String raw, boolean controllerVerified,
                                        boolean hashesCurrent) throws IOException {
        Map<String, String> values = new LinkedHashMap<>();
        for (String line : raw.split("\\R")) {
            int delimiter = line.indexOf('=');
            if (delimiter <= 0) continue;
            String key = line.substring(0, delimiter).trim();
            String value = line.substring(delimiter + 1).trim();
            if (key.matches("[a-z_]+") && values.putIfAbsent(key, value) != null) {
                throw new IOException("duplicate controller status key: " + key);
            }
        }
        String expectedResult = controllerVerified ? "ok" : "degraded";
        if (!"1".equals(values.get("schema")) || !"1".equals(values.get("root"))
                || !expectedResult.equals(values.get("result"))) {
            throw new IOException("missing or unexpected controller status acknowledgement");
        }
        for (String key : new String[]{"camera_enabled", "photo_enabled", "mic_enabled",
                "camera_hook_ready", "audio_hook_ready", "video_feeder_ready",
                "audio_feeder_ready", "video_channel_ready", "audio_channel_ready",
                "visual_source_ready", "audio_source_ready", "obs_pull_running"}) {
            String value = values.get(key);
            if (!"0".equals(value) && !"1".equals(value)) {
                throw new IOException("missing or invalid controller status: " + key);
            }
        }
        values.put("installed", hashesCurrent ? "1" : "0");
        values.put("controller_verified", controllerVerified ? "1" : "0");
        return new RuntimeState(values, raw);
    }

    static RuntimeState noRoot(String raw) {
        Map<String, String> values = new LinkedHashMap<>();
        values.put("root", "0");
        return new RuntimeState(values, raw);
    }

    boolean cameraHealthy() {
        return cameraEnabled && videoFeederReady && cameraHookReady && videoChannelReady;
    }

    boolean photoHealthy() {
        return photoEnabled && videoFeederReady && cameraHookReady && videoChannelReady;
    }

    boolean hasCameraOutputStats() {
        return cameraHookReady && cameraWrittenYuv >= 0 && cameraWrittenJpeg >= 0
                && cameraRawTen >= 0 && cameraUnsupported >= cameraRawTen;
    }

    boolean hasCameraQueueStats() {
        return hasCameraOutputStats() && cameraQueuedYuv >= 0 && cameraQueuedJpeg >= 0
                && cameraQueueErrors >= 0;
    }

    boolean hasCameraPreviewStats() {
        return cameraHookReady && cameraPreviewSeen >= 0 && cameraPreviewStaged >= 0
                && cameraPreviewErrors >= 0;
    }

    boolean micHealthy() {
        return micEnabled && audioFeederReady && audioHookReady && audioChannelReady;
    }

    boolean isStopped() {
        return root && controllerVerified && !cameraEnabled && !photoEnabled
                && !micEnabled && cameraRuntimeStock && !cameraHookReady && !audioHookReady
                && !videoFeederReady && !audioFeederReady && !obsPullRunning
                && !videoChannelReady && !audioChannelReady;
    }

    boolean usesObs() {
        return visualUsesObs() || audioUsesObs();
    }

    boolean visualUsesObs() {
        return "obs".equals(visualMode);
    }

    boolean audioUsesObs() {
        return "obs".equals(audioMode);
    }

    private static boolean flag(Map<String, String> values, String key) {
        return "1".equals(values.get(key));
    }

    private static long counter(Map<String, String> values, String key) {
        String value = values.get(key);
        if (value == null || !value.matches("[0-9]{1,10}")) return -1L;
        return Long.parseLong(value);
    }
}
