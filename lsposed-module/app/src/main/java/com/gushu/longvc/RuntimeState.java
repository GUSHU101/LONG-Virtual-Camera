package com.gushu.longvc;

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
    final boolean obsListenerReady;
    final String obsBind;
    final String obsStreamKey;
    final boolean cameraEnabled;
    final boolean photoEnabled;
    final boolean micEnabled;
    final boolean videoFeederReady;
    final boolean audioFeederReady;
    final boolean cameraHookReady;
    final boolean audioHookReady;
    final boolean videoChannelReady;
    final boolean audioChannelReady;
    final String abi;
    final String selinux;
    final String raw;
    private final Map<String, String> values;

    private RuntimeState(Map<String, String> values, String raw, String ephemeralObsKey) {
        this.values = new LinkedHashMap<>(values);
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
        this.obsListenerReady = flag(values, "obs_listener_ready");
        this.obsBind = values.getOrDefault("obs_bind", "loopback");
        this.obsStreamKey = ephemeralObsKey == null ? "" : ephemeralObsKey;
        this.cameraEnabled = flag(values, "camera_enabled");
        this.photoEnabled = flag(values, "photo_enabled");
        this.micEnabled = flag(values, "mic_enabled");
        this.videoFeederReady = flag(values, "video_feeder_ready");
        this.audioFeederReady = flag(values, "audio_feeder_ready");
        this.cameraHookReady = flag(values, "camera_hook_ready");
        this.audioHookReady = flag(values, "audio_hook_ready");
        this.videoChannelReady = flag(values, "video_channel_ready");
        this.audioChannelReady = flag(values, "audio_channel_ready");
        this.abi = values.getOrDefault("abi", "unknown");
        this.selinux = values.getOrDefault("selinux", "unknown");
        this.raw = raw;
    }

    static RuntimeState parse(String raw) {
        Map<String, String> values = new LinkedHashMap<>();
        for (String line : raw.split("\\R")) {
            int delimiter = line.indexOf('=');
            if (delimiter <= 0) continue;
            String key = line.substring(0, delimiter).trim();
            String value = line.substring(delimiter + 1).trim();
            if (key.matches("[a-z_]+")) values.put(key, value);
        }
        return new RuntimeState(values, raw, "");
    }

    static RuntimeState noRoot(String raw) {
        Map<String, String> values = new LinkedHashMap<>();
        values.put("root", "0");
        return new RuntimeState(values, raw, "");
    }

    boolean cameraHealthy() {
        return cameraEnabled && videoFeederReady && cameraHookReady && videoChannelReady;
    }

    boolean photoHealthy() {
        return photoEnabled && videoFeederReady && cameraHookReady && videoChannelReady;
    }

    boolean micHealthy() {
        return micEnabled && audioFeederReady && audioHookReady && audioChannelReady;
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

    RuntimeState withObsStreamKey(String key) {
        if (key == null || !key.matches("[0-9a-f]{64}")) return this;
        return new RuntimeState(values, raw, key);
    }

    private static boolean flag(Map<String, String> values, String key) {
        return "1".equals(values.get(key));
    }
}
