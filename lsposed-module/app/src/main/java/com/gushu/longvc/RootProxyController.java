package com.gushu.longvc;

import android.content.Context;

import java.io.ByteArrayInputStream;
import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.UUID;

final class RootProxyController {
    private static final String DIR = "/data/cr/proxy";
    private static final String ASSETS = "proxy/arm64-v8a/";
    private static final String SCRIPT = "longvc-proxy.sh";
    private static final String BINARY = "sing-box";
    private static final String TRUSTED = RootShell.TRUSTED_STORAGE + "trusted_dir /data/cr && trusted_dir " + DIR;
    private final Context context;
    private final RootShell shell = new RootShell();

    RootProxyController(Context context) {
        this.context = context.getApplicationContext();
    }

    String start(ProxyProfile profile) throws Exception {
        byte[] config = profile.config(true).toString().getBytes(StandardCharsets.UTF_8);
        install();
        return invoke("start", config);
    }

    String stop() throws Exception {
        return inspect("stop");
    }

    String status() throws Exception {
        return inspect("status");
    }

    String test(ProxyProfile profile, String url) throws Exception {
        byte[] payload = (url + "\n" + profile.config(false)).getBytes(StandardCharsets.UTF_8);
        install();
        return invoke("test", payload);
    }

    private String inspect(String action) throws Exception {
        RootShell.Result state = shell.execute("if [ ! -e " + DIR + " ] && [ ! -L " + DIR
                + " ]; then echo proxy=stopped; else " + TRUSTED + "; fi", 12);
        if (!state.ok()) return "unknown";
        if (state.hasLine("proxy=stopped")) return "stopped";
        stage(SCRIPT, scriptDigest());
        return invoke(action, null);
    }

    private String invoke(String action, byte[] input) throws Exception {
        String sha = scriptDigest();
        String path = DIR + "/" + SCRIPT;
        String command = TRUSTED + " || exit 1; "
                + verify(path, sha) + " || exit 1; exec sh " + path + " " + action;
        RootShell.Result result = input == null ? shell.execute(command, 16)
                : shell.stream(command, new ByteArrayInputStream(input), 24);
        if (result.timedOut) return "timeout";
        if (result.outputTruncated || result.streamError != null) return "unknown";
        // The helper prints only structured operation codes. Do not display su or core logs.
        for (String line : result.output.split("\\R")) {
            if (line.matches("proxy=[a-z_]+")) return line.substring("proxy=".length());
        }
        return "unknown";
    }

    private void install() throws Exception {
        String prepare = RootShell.TRUSTED_STORAGE
                + "ensure_trusted_dir /data/cr 711 && ensure_trusted_dir " + DIR + " 700";
        if (!shell.execute(prepare, 12).ok()) throw new IOException("proxy_storage");
        String coreDigest;
        try (BufferedReader input = new BufferedReader(new InputStreamReader(
                context.getAssets().open(ASSETS + BINARY + ".sha256"), StandardCharsets.US_ASCII))) {
            coreDigest = input.readLine();
        }
        if (coreDigest == null || !coreDigest.matches("[a-f0-9]{64}")) throw new IOException("proxy_asset");
        stage(BINARY, coreDigest);
        stage(SCRIPT, scriptDigest());
    }

    private void stage(String name, String expected) throws Exception {
        String target = DIR + "/" + name;
        if (shell.execute(TRUSTED + " && " + verify(target, expected), 16).ok()) return;
        String temp = DIR + "/.upload-" + UUID.randomUUID();
        String runningGuard = BINARY.equals(name) ? "[ ! -e " + DIR + "/core.pid ] || exit 1; " : "";
        String write = "set -eu; " + TRUSTED + " || exit 1; umask 077; "
                + "trap 'rm -f -- " + temp + "' EXIT; cat > " + temp + "; " + verify(temp, expected) + " || exit 1"
                + "; chmod 700 " + temp + "; exec 9>" + DIR + "/session.lock; timeout 6 flock -x 8 8>&9; "
                + "if " + verify(target, expected) + "; then exit 0; fi; "
                + runningGuard + "mv -f -- " + temp + " " + target;
        try (InputStream input = context.getAssets().open(ASSETS + name)) {
            if (!shell.stream(write, input, 35).ok()) throw new IOException("proxy_asset");
        }
    }

    private String scriptDigest() throws IOException, NoSuchAlgorithmException {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = context.getAssets().open(ASSETS + SCRIPT)) {
            byte[] buffer = new byte[4096];
            int count;
            while ((count = input.read(buffer)) != -1) digest.update(buffer, 0, count);
        }
        StringBuilder text = new StringBuilder(64);
        for (byte value : digest.digest()) {
            text.append(Character.forDigit((value >> 4) & 15, 16));
            text.append(Character.forDigit(value & 15, 16));
        }
        return text.toString();
    }

    private static String verify(String path, String digest) {
        return "[ -f " + path + " ] && [ ! -L " + path + " ] && "
                + "[ \"$(sha256sum " + path + " | cut -d ' ' -f 1)\" = " + digest + " ]";
    }
}
