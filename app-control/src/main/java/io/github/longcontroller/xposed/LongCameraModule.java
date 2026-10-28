package io.github.longcontroller.xposed;

import io.github.longcontroller.BuildConfig;
import io.github.longcontroller.ModuleStatus;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.XC_MethodHook;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.XposedHelpers;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public final class LongCameraModule implements IXposedHookLoadPackage {
    private static final String TAG = "LONG-VCAM: ";

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam loadPackageParam) {
        if (loadPackageParam.packageName == null) return;

        if (BuildConfig.APPLICATION_ID.equals(loadPackageParam.packageName)) {
            hookControllerStatus(loadPackageParam.classLoader);
            return;
        }
        // No target-process hooks are installed by the baseline build.
    }

    private static void hookControllerStatus(ClassLoader classLoader) {
        try {
            XposedHelpers.findAndHookMethod(
                    ModuleStatus.class.getName(),
                    classLoader,
                    "isActive",
                    new XC_MethodHook() {
                        @Override
                        protected void afterHookedMethod(MethodHookParam param) {
                            param.setResult(true);
                        }
                    });
        } catch (Throwable error) {
            log(error);
        }
    }

    static void log(String message) {
        XposedBridge.log(TAG + message);
    }

    static void log(Throwable error) {
        XposedBridge.log(TAG + error.getClass().getSimpleName());
    }
}
