package io.github.longcontroller;

import android.content.Context;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraManager;
import android.os.Build;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;

public final class DeviceProbe {
    private DeviceProbe() {}

    public static TargetDeviceProfile.Match targetMatch() {
        return TargetDeviceProfile.match(new TargetDeviceProfile.Observed(
                Build.MANUFACTURER, Build.MODEL, Build.DEVICE, Build.VERSION.SDK_INT,
                Build.ID, Build.VERSION.SECURITY_PATCH, Build.TYPE, Build.TAGS,
                Arrays.asList(Build.SUPPORTED_ABIS)));
    }

    public static String collect(Context context) {
        StringBuilder report = new StringBuilder();
        report.append("Evidence: CAPABILITY_ONLY (不是跨应用 PASS)\n");
        TargetDeviceProfile.Match match = targetMatch();
        report.append("首要目标: ").append(TargetDeviceProfile.ID).append('\n');
        report.append("目标匹配: ").append(match.matches() ? "MATCH（属性匹配，非真实性认证）" : "MISMATCH")
                .append('\n');
        for (String mismatch : match.mismatches()) report.append("  • ").append(mismatch).append('\n');
        report.append("跨应用替换: BUILD_DISABLED / NOT_RUN\n");
        report.append("Android: ").append(Build.VERSION.RELEASE)
                .append(" / API ").append(Build.VERSION.SDK_INT).append('\n');
        report.append("Build: ").append(Build.FINGERPRINT).append('\n');
        report.append("Security patch: ").append(Build.VERSION.SECURITY_PATCH).append('\n');
        report.append("Root binary hint: ").append(hasRootBinaryHint() ? "FOUND" : "NOT_FOUND")
                .append(" (不代表已授权)\n");
        report.append("LSPosed module active in controller: ")
                .append(ModuleStatus.isActive() ? "YES" : "NO/UNKNOWN").append('\n');

        try {
            CameraManager cameraManager = context.getSystemService(CameraManager.class);
            String[] ids = cameraManager == null ? new String[0] : cameraManager.getCameraIdList();
            report.append("Camera IDs: ").append(ids.length).append('\n');
            for (String id : ids) {
                CameraCharacteristics c = cameraManager.getCameraCharacteristics(id);
                Integer facing = c.get(CameraCharacteristics.LENS_FACING);
                Integer level = c.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL);
                report.append("  • ").append(id)
                        .append(" facing=").append(facingName(facing))
                        .append(" level=").append(levelName(level)).append('\n');
            }
        } catch (Exception error) {
            report.append("Camera probe error: ").append(error.getClass().getSimpleName())
                    .append(" (保持原生)\n");
        }
        return report.toString();
    }

    private static boolean hasRootBinaryHint() {
        List<String> candidates = new ArrayList<>();
        candidates.add("/system/bin/su");
        candidates.add("/system/xbin/su");
        candidates.add("/sbin/su");
        candidates.add("/data/adb/magisk");
        for (String candidate : candidates) {
            if (new File(candidate).exists()) return true;
        }
        return false;
    }

    private static String facingName(Integer facing) {
        if (facing == null) return "UNKNOWN";
        if (facing == CameraCharacteristics.LENS_FACING_FRONT) return "FRONT";
        if (facing == CameraCharacteristics.LENS_FACING_BACK) return "BACK";
        if (facing == CameraCharacteristics.LENS_FACING_EXTERNAL) return "EXTERNAL";
        return String.format(Locale.ROOT, "%d", facing);
    }

    private static String levelName(Integer level) {
        if (level == null) return "UNKNOWN";
        if (level == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LEGACY) return "LEGACY";
        if (level == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED) return "LIMITED";
        if (level == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_FULL) return "FULL";
        if (level == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_3) return "LEVEL_3";
        if (level == CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL_EXTERNAL) return "EXTERNAL";
        return Integer.toString(level);
    }
}
