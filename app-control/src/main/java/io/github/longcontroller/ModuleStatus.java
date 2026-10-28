package io.github.longcontroller;

/** LSPosed replaces the return value in the controller process when the module is active. */
public final class ModuleStatus {
    private ModuleStatus() {}

    public static boolean isActive() {
        return false;
    }
}
