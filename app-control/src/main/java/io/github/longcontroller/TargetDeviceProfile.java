package io.github.longcontroller;

import java.util.ArrayList;
import java.util.List;

/** Declared target, not attestation or proof of camera compatibility. */
public final class TargetDeviceProfile {
    public static final String ID = "redfin-UP1A.231105.001.B2";
    public static final String DEVICE = "redfin";
    public static final String BUILD_ID = "UP1A.231105.001.B2";
    public static final int API = 34;
    public static final String SECURITY_PATCH = "2023-11-05";
    public static final String AOSP_TAG = "android-14.0.0_r28";
    public static final String IMAGE_URL = "https://dl.google.com/dl/android/aosp/redfin-up1a.231105.001.b2-factory-4e5a2679.zip?hl=zh-cn";

    // Source-level release gate. Not a user preference or a Gradle opt-in switch.
    // Re-enable only after the backend contracts and physical-device recovery tests pass.
    public static final boolean CROSS_APP_BACKEND_READY = false;

    public record Observed(String manufacturer, String model, String device, int api,
                           String buildId, String securityPatch, String type, String tags,
                           List<String> abis) {}
    public record Match(boolean matches, List<String> mismatches) {}

    public static Match match(Observed value) {
        List<String> errors = new ArrayList<>();
        if (value == null) return new Match(false, List.of("设备信息缺失"));
        check(errors, "manufacturer", "Google", value.manufacturer());
        check(errors, "model", "Pixel 5", value.model());
        check(errors, "device", DEVICE, value.device());
        if (value.api() != API) errors.add("API 应为 " + API + "，实际 " + value.api());
        check(errors, "buildId", BUILD_ID, value.buildId());
        check(errors, "securityPatch", SECURITY_PATCH, value.securityPatch());
        check(errors, "type", "user", value.type());
        check(errors, "tags", "release-keys", value.tags());
        if (value.abis() == null || !value.abis().contains("arm64-v8a")) {
            errors.add("缺少 arm64-v8a ABI");
        }
        return new Match(errors.isEmpty(), List.copyOf(errors));
    }

    private static void check(List<String> errors, String field, String expected, String actual) {
        if (!expected.equals(actual)) errors.add(field + " 应为 " + expected + "，实际 " + actual);
    }

    private TargetDeviceProfile() {}
}
