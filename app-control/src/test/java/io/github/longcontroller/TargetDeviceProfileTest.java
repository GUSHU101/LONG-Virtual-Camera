package io.github.longcontroller;

import org.junit.Test;
import java.util.List;
import static org.junit.Assert.*;

public class TargetDeviceProfileTest {
    private TargetDeviceProfile.Observed device(String model, String code, int api, String build, String patch) {
        return new TargetDeviceProfile.Observed("Google", model, code, api, build, patch,
                "user", "release-keys", List.of("arm64-v8a", "armeabi-v7a"));
    }
    @Test public void matchesDeclaredTarget() {
        assertTrue(TargetDeviceProfile.match(device("Pixel 5", "redfin", 34,
                "UP1A.231105.001.B2", "2023-11-05")).matches());
    }
    @Test public void rejectsOriginalNovemberBuildWithoutB2() {
        assertFalse(TargetDeviceProfile.match(device("Pixel 5", "redfin", 34,
                "UP1A.231105.001", "2023-11-05")).matches());
    }
    @Test public void rejectsBrambleWithSameBuild() {
        assertFalse(TargetDeviceProfile.match(device("Pixel 4a (5G)", "bramble", 34,
                "UP1A.231105.001.B2", "2023-11-05")).matches());
    }
    @Test public void rejectsApiOrSecurityPatchDrift() {
        assertFalse(TargetDeviceProfile.match(device("Pixel 5", "redfin", 35,
                "UP1A.231105.001.B2", "2023-11-05")).matches());
        assertFalse(TargetDeviceProfile.match(device("Pixel 5", "redfin", 34,
                "UP1A.231105.001.B2", "2024-02-05")).matches());
    }
    @Test public void rejectsUnknownAndCustomBuild() {
        assertFalse(TargetDeviceProfile.match(null).matches());
        assertFalse(TargetDeviceProfile.match(new TargetDeviceProfile.Observed("Google", "Pixel 5",
                "redfin", 34, "UP1A.231105.001.B2", "2023-11-05", "userdebug", "test-keys", null)).matches());
    }
    @Test public void crossAppBackendCannotBeEnabledInBaseline() {
        assertFalse(TargetDeviceProfile.CROSS_APP_BACKEND_READY);
    }
}
