package com.gushu.longvc;

import android.util.Log;

import io.github.libxposed.api.XposedModule;
import io.github.libxposed.api.XposedModuleInterface.ModuleLoadedParam;
import io.github.libxposed.api.XposedModuleInterface.SystemServerStartingParam;

public final class ModuleMain extends XposedModule {
    private static final String TAG = "LONGVirtualCamera";

    @Override
    public void onModuleLoaded(ModuleLoadedParam param) {
        log(Log.INFO, TAG, "module loaded in " + param.getProcessName());
        log(Log.INFO, TAG, "framework=" + getFrameworkName()
                + " version=" + getFrameworkVersionCode() + " api=" + getApiVersion());
        boolean systemCapable = (getFrameworkProperties() & PROP_CAP_SYSTEM) != 0L;
        log(systemCapable ? Log.INFO : Log.ERROR, TAG,
                "system scope capability=" + systemCapable);
    }

    @Override
    public void onSystemServerStarting(SystemServerStartingParam param) {
        // Deliberately no per-app Java camera hooks. The global media path is the
        // one native cameraserver hook managed by the root control plane.
        log(Log.INFO, TAG, "system_server active; native global control plane ready, classloader="
                + param.getClassLoader());
    }
}
