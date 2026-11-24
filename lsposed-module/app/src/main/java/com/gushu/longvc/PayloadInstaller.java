package com.gushu.longvc;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Build;
import android.util.Log;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

final class PayloadInstaller {
    static final String REMOTE_SCRIPT = "/data/local/tmp/longvc-control.sh";
    private static final String ASSET_ROOT = "payload/arm64-v8a/";
    private static final String REMOTE_ROOT = "/data/local/tmp/";
    private static final String REMOTE_MANIFEST = REMOTE_ROOT + "longvc-payload.sha256";
    private static final String[] FILES = {
            "cr_injector", "cr_feed_proc", "libcr_hooks.so", "longvc-control.sh"
    };

    private final AssetManager assets;
    private final RootShell shell;

    PayloadInstaller(Context context, RootShell shell) {
        this.assets = context.getApplicationContext().getAssets();
        this.shell = shell;
    }

    boolean isCurrent() throws IOException, InterruptedException {
        Map<String, Entry> expected = readManifest();
        StringBuilder command = new StringBuilder("set -e; ");
        appendInstalledChecks(command, expected);
        command.append("echo current");
        RootShell.Result result = shell.execute(command.toString(), 15L);
        return result.ok() && hasLine(result, "current");
    }

    private static void appendInstalledChecks(StringBuilder command,
                                               Map<String, Entry> expected)
            throws IOException {
        for (String name : FILES) {
            appendEntryCheck(command, REMOTE_ROOT + name, requireEntry(expected, name));
        }
        command.append("[ -x '").append(REMOTE_ROOT).append("cr_injector' ]; ")
                .append("[ -x '").append(REMOTE_ROOT).append("cr_feed_proc' ]; ")
                .append("[ -r '").append(REMOTE_ROOT).append("libcr_hooks.so' ]; ")
                .append("[ -x '").append(REMOTE_SCRIPT).append("' ]; ")
                .append("if [ \"$(getenforce 2>/dev/null)\" = Enforcing ]; then ")
                .append("ls -Z '").append(REMOTE_ROOT).append("libcr_hooks.so' 2>/dev/null | ")
                .append("grep -q 'u:object_r:system_file:s0'; fi; ");
    }

    private static void appendEntryCheck(StringBuilder command, String remote, Entry entry) {
        command.append("[ -f '").append(remote).append("' ]; ")
                .append("[ \"$(wc -c < '").append(remote).append("' | tr -d ' ')\" = '")
                .append(entry.size).append("' ]; ")
                .append("h=$(if command -v sha256sum >/dev/null 2>&1; then sha256sum '")
                .append(remote).append("'; else toybox sha256sum '").append(remote)
                .append("'; fi | awk '{print $1}'); ")
                .append("[ \"$h\" = '").append(entry.hash).append("' ]; ");
    }

    boolean isControlScriptCurrent() throws IOException, InterruptedException {
        Map<String, Entry> expected = readManifest();
        Entry script = requireEntry(expected, "longvc-control.sh");
        verifyPackagedAsset("longvc-control.sh", script);
        return isRemoteEntryCurrent(REMOTE_SCRIPT, script, true);
    }

    void ensureControlScript() throws IOException, InterruptedException {
        Map<String, Entry> expected = readManifest();
        Entry script = requireEntry(expected, "longvc-control.sh");
        verifyPackagedAsset("longvc-control.sh", script);
        if (isRemoteEntryCurrent(REMOTE_SCRIPT, script, true)) return;

        String suffix = Long.toUnsignedString(System.nanoTime());
        String staged = REMOTE_ROOT + ".longvc-control-" + suffix;
        try {
            streamAsset("longvc-control.sh", staged);
            if (!isRemoteEntryCurrent(staged, script, false)) {
                throw new IOException("recovery control script failed device-side verification");
            }
            StringBuilder command = new StringBuilder("set -e; backup='" + staged + ".backup'; "
                    + "had_old=0; if [ -e '" + REMOTE_SCRIPT + "' ]; then "
                    + "cp -p '" + REMOTE_SCRIPT + "' \"$backup\"; had_old=1; fi; "
                    + "rollback() { if [ \"$had_old\" = 1 ]; then cp -p \"$backup\" '"
                    + REMOTE_SCRIPT + "'; else rm -f '" + REMOTE_SCRIPT + "'; fi; }; "
                    + "trap rollback EXIT; trap 'exit 1' HUP INT TERM; "
                    + "mv -f '" + staged + "' '" + REMOTE_SCRIPT + "'; "
                    + "chown 0:0 '" + REMOTE_SCRIPT + "'; chmod 0755 '" + REMOTE_SCRIPT + "'; "
                    + "[ -x '" + REMOTE_SCRIPT + "' ]; ");
            appendEntryCheck(command, REMOTE_SCRIPT, script);
            command.append("trap - EXIT HUP INT TERM; echo control-ready");
            RootShell.Result result = shell.execute(command.toString(), 15L);
            requireOk("repair recovery control script", result);
            if (!hasLine(result, "control-ready")) {
                throw new IOException("recovery control script failed post-commit verification");
            }
            discardStage("rm -f '" + staged + ".backup'");
        } finally {
            // The backup could be the only intact old controller after a failed rollback.
            discardStage("rm -f '" + staged + "'");
        }
    }

    void install() throws IOException, InterruptedException {
        requireArm64();
        Map<String, Entry> expected = readManifest();
        verifyPackagedAssets(expected);

        String suffix = Long.toUnsignedString(System.nanoTime());
        String stage = REMOTE_ROOT + ".longvc-stage-" + suffix;
        RootShell.Result prepare = shell.execute(
                "umask 077; mkdir '" + stage + "' && chmod 0700 '" + stage + "'", 10L);
        requireOk("prepare payload stage", prepare);

        boolean commitStarted = false;
        boolean commitCompleted = false;
        try {
            for (String name : FILES) {
                streamAsset(name, stage + "/" + name);
            }
            streamAsset("payload.sha256", stage + "/payload.sha256");
            verifyRemoteStage(stage, expected);
            quiesceRuntime(stage);
            commitStarted = true;
            commit(stage, expected);
            commitCompleted = true;
        } finally {
            // A failed commit may have left its only recoverable backup under
            // the stage. Keep it for manual repair; pre-commit failures and a
            // fully verified commit are safe to clean automatically.
            if (!commitStarted || commitCompleted) {
                discardStage("rm -rf '" + stage + "'");
            }
        }
    }

    private void streamAsset(String name, String remote)
            throws IOException, InterruptedException {
        try (InputStream input = assets.open(ASSET_ROOT + name, AssetManager.ACCESS_STREAMING)) {
            RootShell.Result result = shell.stream(
                    "umask 077; dd of='" + remote + "' bs=65536 2>/dev/null", input, 30L);
            requireOk("stage " + name, result);
        }
    }

    private void verifyRemoteStage(String stage, Map<String, Entry> expected)
            throws IOException, InterruptedException {
        StringBuilder command = new StringBuilder("set -e; ");
        for (String name : FILES) {
            appendEntryCheck(command, stage + "/" + name, requireEntry(expected, name));
        }
        command.append("echo verified");
        RootShell.Result result = shell.execute(command.toString(), 30L);
        requireOk("verify staged payload", result);
        if (!result.output.contains("verified")) throw new IOException("payload verification did not finish");
    }

    private void commit(String stage, Map<String, Entry> expected)
            throws IOException, InterruptedException {
        StringBuilder command = new StringBuilder("set -e; "
                + "backup='" + stage + "/backup'; mkdir \"$backup\"; "
                + "for n in cr_injector cr_feed_proc libcr_hooks.so longvc-control.sh longvc-payload.sha256; do "
                + "  [ ! -e '" + REMOTE_ROOT + "'$n ] || cp -p '" + REMOTE_ROOT + "'$n \"$backup/$n\"; "
                + "done; "
                + "rollback() { for n in cr_injector cr_feed_proc libcr_hooks.so longvc-control.sh longvc-payload.sha256; do "
                + "  if [ -e \"$backup/$n\" ]; then cp -p \"$backup/$n\" '" + REMOTE_ROOT + "'$n || true; "
                + "  else rm -f '" + REMOTE_ROOT + "'$n || true; fi; done; "
                + "  chown 0:0 '" + REMOTE_ROOT + "cr_injector' '" + REMOTE_ROOT + "cr_feed_proc' "
                + "    '" + REMOTE_ROOT + "libcr_hooks.so' '" + REMOTE_SCRIPT + "' '" + REMOTE_MANIFEST + "' 2>/dev/null || true; "
                + "  chmod 0755 '" + REMOTE_ROOT + "cr_injector' '" + REMOTE_ROOT + "cr_feed_proc' "
                + "    '" + REMOTE_ROOT + "libcr_hooks.so' '" + REMOTE_SCRIPT + "' 2>/dev/null || true; "
                + "  chmod 0644 '" + REMOTE_MANIFEST + "' 2>/dev/null || true; "
                + "  command -v chcon >/dev/null 2>&1 && chcon u:object_r:system_file:s0 '"
                + REMOTE_ROOT + "libcr_hooks.so' 2>/dev/null || true; }; "
                + "trap rollback EXIT; trap 'exit 1' HUP INT TERM; "
                + "mv -f '" + stage + "/cr_injector' '" + REMOTE_ROOT + "cr_injector'; "
                + "mv -f '" + stage + "/cr_feed_proc' '" + REMOTE_ROOT + "cr_feed_proc'; "
                + "mv -f '" + stage + "/libcr_hooks.so' '" + REMOTE_ROOT + "libcr_hooks.so'; "
                + "mv -f '" + stage + "/longvc-control.sh' '" + REMOTE_SCRIPT + "'; "
                + "mv -f '" + stage + "/payload.sha256' '" + REMOTE_MANIFEST + "'; "
                + "chown 0:0 '" + REMOTE_ROOT + "cr_injector' '" + REMOTE_ROOT + "cr_feed_proc' "
                + "  '" + REMOTE_ROOT + "libcr_hooks.so' '" + REMOTE_SCRIPT + "' '" + REMOTE_MANIFEST + "'; "
                + "chmod 0755 '" + REMOTE_ROOT + "cr_injector' '" + REMOTE_ROOT + "cr_feed_proc' "
                + "  '" + REMOTE_ROOT + "libcr_hooks.so' '" + REMOTE_SCRIPT + "'; "
                + "chmod 0644 '" + REMOTE_MANIFEST + "'; "
                + "if command -v chcon >/dev/null 2>&1; then "
                + "  chcon u:object_r:system_file:s0 '" + REMOTE_ROOT + "libcr_hooks.so' || "
                + "    [ \"$(getenforce 2>/dev/null)\" != Enforcing ]; "
                + "else [ \"$(getenforce 2>/dev/null)\" != Enforcing ]; fi; ");
        // Keep the rollback trap armed through the same checks used by isCurrent().
        // The caller removes the stage and backup only after the shell confirms.
        appendInstalledChecks(command, expected);
        command.append("trap - EXIT HUP INT TERM; echo committed");
        RootShell.Result result = shell.execute(command.toString(), 30L);
        requireOk("commit payload; recovery stage=" + stage, result);
        if (!hasLine(result, "committed")) throw new IOException("payload commit did not finish");
    }

    private void discardStage(String command) {
        if (Thread.currentThread().isInterrupted()) return;
        try {
            RootShell.Result result = shell.execute(command, 10L);
            if (!result.ok()) Log.w("LongVcInstaller", "Payload cleanup did not complete");
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        } catch (IOException cleanupFailure) {
            Log.w("LongVcInstaller", "Payload cleanup could not start", cleanupFailure);
        }
    }

    private static boolean hasLine(RootShell.Result result, String expected) {
        for (String line : result.output.split("\\R")) {
            if (expected.equals(line.trim())) return true;
        }
        return false;
    }

    private void quiesceRuntime(String stage) throws IOException, InterruptedException {
        RootShell.Result stop = shell.execute(
                "/system/bin/sh '" + stage + "/longvc-control.sh' stop-all", 45L);
        requireOk("stop active runtime before payload update", stop);
        if (!stop.output.contains("result=ok")) {
            throw new IOException("runtime stop did not report success before payload update");
        }

        String verify = "set -e; "
                + "for p in $(pidof cr_feed_proc 2>/dev/null); do "
                + "  exe=$(readlink /proc/$p/exe 2>/dev/null); "
                + "  case \"$exe\" in */cr_feed_proc*) exit 1 ;; esac; "
                + "done; "
                + "for proc in cameraserver audioserver; do for p in $(pidof $proc 2>/dev/null); do "
                + "  if grep -q '/libcr_hooks\\.so' /proc/$p/maps 2>/dev/null; then exit 1; fi; "
                + "done; done; echo quiescent";
        RootShell.Result state = shell.execute(verify, 15L);
        requireOk("verify quiescent runtime before payload update", state);
        if (!state.output.contains("quiescent")) {
            throw new IOException("runtime remained active before payload update");
        }
    }

    private Map<String, Entry> readManifest() throws IOException {
        Map<String, Entry> result = new LinkedHashMap<>();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                assets.open(ASSET_ROOT + "payload.sha256"), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] parts = line.trim().split("\\s+", 3);
                if (parts.length != 3 || !parts[0].matches("[0-9a-f]{64}")
                        || !parts[1].matches("[0-9]+") || !parts[2].matches("[A-Za-z0-9._-]+")) {
                    throw new IOException("invalid packaged payload manifest line");
                }
                final long size;
                try {
                    size = Long.parseLong(parts[1]);
                } catch (NumberFormatException invalidSize) {
                    throw new IOException("invalid packaged payload size", invalidSize);
                }
                if (result.put(parts[2], new Entry(parts[0], size)) != null) {
                    throw new IOException("duplicate packaged payload manifest entry");
                }
            }
        }
        for (String name : FILES) requireEntry(result, name);
        if (result.size() != FILES.length) {
            throw new IOException("unexpected packaged payload manifest entry");
        }
        return result;
    }

    private void verifyPackagedAssets(Map<String, Entry> expected) throws IOException {
        for (String name : FILES) {
            Entry entry = requireEntry(expected, name);
            verifyPackagedAsset(name, entry);
        }
    }

    private void verifyPackagedAsset(String name, Entry entry) throws IOException {
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("SHA-256 unavailable", impossible);
        }
        long size = 0L;
        try (InputStream input = assets.open(ASSET_ROOT + name, AssetManager.ACCESS_STREAMING)) {
            byte[] buffer = new byte[64 * 1024];
            int read;
            while ((read = input.read(buffer)) >= 0) {
                if (read == 0) continue;
                digest.update(buffer, 0, read);
                size += read;
            }
        }
        String hash = hex(digest.digest());
        if (size != entry.size || !hash.equals(entry.hash)) {
            throw new IOException("packaged asset checksum mismatch: " + name);
        }
    }

    private boolean isRemoteEntryCurrent(String remote, Entry entry, boolean requireExecutable)
            throws IOException, InterruptedException {
        String command = "set -e; [ -f '" + remote + "' ]; "
                + (requireExecutable ? "[ -x '" + remote + "' ]; " : "")
                + "[ \"$(wc -c < '" + remote + "' | tr -d ' ')\" = '" + entry.size + "' ]; "
                + "h=$(if command -v sha256sum >/dev/null 2>&1; then sha256sum '" + remote
                + "'; else toybox sha256sum '" + remote + "'; fi | awk '{print $1}'); "
                + "[ \"$h\" = '" + entry.hash + "' ]; echo current";
        RootShell.Result result = shell.execute(command, 15L);
        return result.ok() && result.output.contains("current");
    }

    private static Entry requireEntry(Map<String, Entry> entries, String name) throws IOException {
        Entry entry = entries.get(name);
        if (entry == null || entry.size <= 0L) throw new IOException("missing payload manifest entry: " + name);
        return entry;
    }

    private static void requireArm64() throws IOException {
        for (String abi : Build.SUPPORTED_64_BIT_ABIS) {
            if ("arm64-v8a".equals(abi)) return;
        }
        throw new IOException("this build contains only the ARM64 payload; device ABI is unsupported");
    }

    private static void requireOk(String step, RootShell.Result result) throws IOException {
        if (!result.ok()) throw new IOException(step + " failed: " + result.describe());
    }

    private static String hex(byte[] bytes) {
        StringBuilder value = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) value.append(String.format(Locale.ROOT, "%02x", item & 0xff));
        return value.toString();
    }

    private static final class Entry {
        final String hash;
        final long size;

        Entry(String hash, long size) {
            this.hash = hash;
            this.size = size;
        }
    }
}
