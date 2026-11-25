package com.gushu.longvc;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.graphics.Matrix;
import android.net.Uri;
import android.os.Build;
import android.provider.OpenableColumns;
import android.util.Size;

import androidx.exifinterface.media.ExifInterface;

import java.io.ByteArrayInputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.MessageDigest;
import java.security.DigestInputStream;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

final class MediaSourceManager {
    private static final String MEDIA_DIR = "/data/local/tmp/longvc-media";
    private static final String CONTROL_DIR = "/data/cr/control";
    private static final String SOURCE_CONFIG = CONTROL_DIR + "/source.conf";
    private static final long MAX_MEDIA_BYTES = 2L * 1024L * 1024L * 1024L;
    private static final int MAX_IMAGE_WIDTH = 1920;
    private static final int MAX_IMAGE_HEIGHT = 1920;
    private static final long MAX_IMAGE_PIXELS = 1920L * 1080L;
    private static final int LNV1_MAGIC = 0x31564e4c;

    private final Context context;
    private final ContentResolver resolver;
    private final RootShell shell;

    MediaSourceManager(Context context, RootShell shell) {
        this.context = context.getApplicationContext();
        this.resolver = this.context.getContentResolver();
        this.shell = shell;
    }

    PreparedMedia prepareVideo(Uri uri) throws IOException, InterruptedException {
        requireMime(uri, "video/");
        long size = querySize(uri);
        return prepareMedia(requireInput(uri), size, "visual", "video");
    }

    PreparedMedia prepareImage(Uri uri) throws IOException, InterruptedException {
        requireMime(uri, "image/");
        byte[] encoded = encodeImageAsNv21(uri);
        return prepareMedia(new ByteArrayInputStream(encoded), encoded.length, "visual", "image");
    }

    PreparedMedia prepareAudio(Uri uri) throws IOException, InterruptedException {
        requireMime(uri, "audio/");
        long size = querySize(uri);
        return prepareMedia(requireInput(uri), size, "audio", "");
    }

    String configureObs(boolean allowLan) throws IOException, InterruptedException {
        ensureStorage();
        SourceSnapshot current = readConfig();
        byte[] random = new byte[32];
        new SecureRandom().nextBytes(random);
        String token = hex(random);
        commitConfig(current, current.config.withObs(token, allowLan ? "lan" : "loopback"));
        return token;
    }

    private PreparedMedia prepareMedia(InputStream source, long declaredSize,
                                       String type, String kind)
            throws IOException, InterruptedException {
        String staged = MEDIA_DIR + "/." + type + "-" + UUID.randomUUID();
        Fingerprint fingerprint;
        try (InputStream input = source) {
            ensureStorage();
            fingerprint = streamAndVerify(input, declaredSize, staged);
        } catch (IOException | InterruptedException | RuntimeException failure) {
            cleanup(staged);
            throw failure;
        }
        return new PreparedMedia(staged, type, kind, fingerprint);
    }

    void commitMedia(PreparedMedia media) throws IOException, InterruptedException {
        SourceSnapshot current = readConfig();
        String published = publishImmutable(media.staged, media.type, media.fingerprint);
        SourceConfig next = "audio".equals(media.type)
                ? current.config.withAudio(published)
                : current.config.withVisual(media.kind, published);
        commitConfig(current, next);
    }

    private void ensureStorage() throws IOException, InterruptedException {
        String command = "set -e; umask 077; mkdir -p '" + MEDIA_DIR + "' '"
                + CONTROL_DIR + "'; chown 0:0 '" + MEDIA_DIR + "' '" + CONTROL_DIR
                + "'; chmod 0700 '" + MEDIA_DIR + "' '" + CONTROL_DIR + "'; "
                + "if command -v chcon >/dev/null 2>&1; then "
                + "chcon u:object_r:system_data_file:s0 '/data/cr' '" + CONTROL_DIR
                + "' 2>/dev/null || [ \"$(getenforce 2>/dev/null)\" != Enforcing ]; "
                + "else [ \"$(getenforce 2>/dev/null)\" != Enforcing ]; fi";
        requireOk("prepare private media storage", shell.execute(command, 15L));
    }

    private Fingerprint streamAndVerify(InputStream source, long declaredSize,
                                        String remote)
            throws IOException, InterruptedException {
        if (declaredSize == 0L || declaredSize > MAX_MEDIA_BYTES) {
            throw new IOException("media file is empty or exceeds 2 GiB");
        }
        MessageDigest digest = sha256();
        long limit = declaredSize > 0L ? Math.min(MAX_MEDIA_BYTES, declaredSize)
                : MAX_MEDIA_BYTES;
        CountingInputStream counted = new CountingInputStream(
                new DigestInputStream(source, digest), limit);
        long timeout = declaredSize > 0L
                ? Math.max(60L, Math.min(900L, 60L + declaredSize / (2L * 1024L * 1024L)))
                : 900L;
        RootShell.Result result = shell.stream(
                "umask 077; dd of='" + remote + "' bs=65536 2>/dev/null",
                counted, timeout);
        requireOk("copy selected media", result);
        if (counted.exceeded) {
            throw new IOException("media file exceeds 2 GiB");
        }
        long size = counted.count;
        if (size <= 0L || (declaredSize > 0L && size != declaredSize)) {
            throw new IOException("media copy ended at an unexpected size");
        }
        Fingerprint fingerprint = new Fingerprint(hex(digest.digest()), size);
        if (!remoteMatches(remote, fingerprint)) {
            throw new IOException("device-side media checksum verification failed");
        }
        return fingerprint;
    }

    private void streamSmall(String value, String remote)
            throws IOException, InterruptedException {
        try (InputStream input = new ByteArrayInputStream(
                value.getBytes(java.nio.charset.StandardCharsets.UTF_8))) {
            RootShell.Result result = shell.stream(
                    "umask 077; dd of='" + remote + "' bs=4096 2>/dev/null",
                    input, 15L);
            requireOk("write source configuration", result);
        }
    }

    private String publishImmutable(String staged, String type,
                                    Fingerprint fingerprint)
            throws IOException, InterruptedException {
        if (!("visual".equals(type) || "audio".equals(type))) {
            throw new IllegalArgumentException("invalid media type");
        }
        String published = MEDIA_DIR + "/" + type + "-" + fingerprint.sha256 + ".media";
        String command = "set -e; if [ -e '" + published + "' ]; then "
                + verifyCommand(published, fingerprint) + "rm -f '" + staged + "'; "
                + "else mv '" + staged + "' '" + published + "'; fi; "
                + "chown 0:0 '" + published + "'; chmod 0600 '" + published + "'; "
                + verifyCommand(published, fingerprint) + "echo media-published";
        RootShell.Result result = shell.execute(command, 45L);
        requireOk("publish immutable media", result);
        if (!result.output.contains("media-published")) {
            throw new IOException("media publication did not finish");
        }
        return published;
    }

    private SourceSnapshot readConfig() throws IOException, InterruptedException {
        RootShell.Result result = shell.execute(
                "if [ -e '" + SOURCE_CONFIG + "' ]; then cat '" + SOURCE_CONFIG
                        + "'; else printf 'schema=2\\n'; fi", 15L);
        if (!result.ok()) throw new IOException("could not read source configuration");
        byte[] contents = result.output.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        return new SourceSnapshot(SourceConfig.parse(result.output), hex(sha256().digest(contents)));
    }

    private void commitConfig(SourceSnapshot current, SourceConfig config)
            throws IOException, InterruptedException {
        String serialized = config.serialize();
        byte[] bytes = serialized.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        String stageDigest = hex(sha256().digest(bytes));
        String stageName = ".source-config-" + UUID.randomUUID();
        String staged = CONTROL_DIR + "/" + stageName;
        try {
            streamSmall(serialized, staged);
            String command = PayloadInstaller.REMOTE_SCRIPT + " commit-source "
                    + stageName + " " + current.digest + " " + stageDigest;
            RootShell.Result result = shell.execute(command, 30L);
            if (result.hasLine("message=source_config_conflict")
                    || result.hasLine("message=source_runtime_active")) {
                throw new SourceConflictException();
            }
            requireOk("commit source configuration", result);
            if (!result.hasLine("result=ok") || !result.hasLine("action=commit-source")) {
                throw new IOException("source configuration commit did not finish");
            }
        } finally {
            cleanup(staged);
        }
    }

    private boolean remoteMatches(String path, Fingerprint fingerprint)
            throws IOException, InterruptedException {
        RootShell.Result result = shell.execute(
                "set -e; " + verifyCommand(path, fingerprint) + "echo verified",
                30L);
        return result.ok() && result.output.contains("verified");
    }

    private static String verifyCommand(String path, Fingerprint fingerprint) {
        return "[ \"$(wc -c < '" + path + "' | tr -d ' ')\" = '"
                + fingerprint.size + "' ]; "
                + "h=$(if command -v sha256sum >/dev/null 2>&1; then sha256sum '"
                + path + "'; else toybox sha256sum '" + path
                + "'; fi | awk '{print $1}'); [ \"$h\" = '"
                + fingerprint.sha256 + "' ]; ";
    }

    private byte[] encodeImageAsNv21(Uri uri) throws IOException {
        Bitmap bitmap = decodeBoundedBitmap(uri);
        try {
            int width = bitmap.getWidth() & ~1;
            int height = bitmap.getHeight() & ~1;
            if (width < 16 || height < 16) {
                throw new IOException("image dimensions are too small");
            }
            if (width != bitmap.getWidth() || height != bitmap.getHeight()) {
                Bitmap cropped = Bitmap.createBitmap(bitmap, 0, 0, width, height);
                if (cropped != bitmap) {
                    bitmap.recycle();
                    bitmap = cropped;
                }
            }
            int[] pixels = new int[width * height];
            bitmap.getPixels(pixels, 0, width, 0, 0, width, height);
            byte[] nv21 = new byte[width * height * 3 / 2];
            int yIndex = 0;
            int vuIndex = width * height;
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int color = pixels[y * width + x];
                    int r = (color >> 16) & 0xff;
                    int g = (color >> 8) & 0xff;
                    int b = color & 0xff;
                    int yy = clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                    nv21[yIndex++] = (byte) yy;
                    if ((y & 1) == 0 && (x & 1) == 0) {
                        int v = clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                        int u = clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                        nv21[vuIndex++] = (byte) v;
                        nv21[vuIndex++] = (byte) u;
                    }
                }
            }
            ByteBuffer output = ByteBuffer.allocate(12 + nv21.length)
                    .order(ByteOrder.LITTLE_ENDIAN);
            output.putInt(LNV1_MAGIC);
            output.putInt(width);
            output.putInt(height);
            output.put(nv21);
            return output.array();
        } finally {
            if (!bitmap.isRecycled()) bitmap.recycle();
        }
    }

    private Bitmap decodeBoundedBitmap(Uri uri) throws IOException {
        if (Build.VERSION.SDK_INT >= 28) {
            ImageDecoder.Source source = ImageDecoder.createSource(resolver, uri);
            try {
                return ImageDecoder.decodeBitmap(source, (decoder, info, ignored) -> {
                    int width = info.getSize().getWidth();
                    int height = info.getSize().getHeight();
                    if (width < 2 || height < 2) {
                        throw new IllegalArgumentException("invalid image dimensions");
                    }
                    Size bounded = boundedSizeValid(width, height);
                    decoder.setAllocator(ImageDecoder.ALLOCATOR_SOFTWARE);
                    decoder.setMemorySizePolicy(ImageDecoder.MEMORY_POLICY_LOW_RAM);
                    decoder.setTargetSize(bounded.getWidth(), bounded.getHeight());
                });
            } catch (IllegalArgumentException invalid) {
                throw new IOException("invalid image dimensions", invalid);
            }
        }

        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        try (InputStream input = requireInput(uri)) {
            BitmapFactory.decodeStream(input, null, bounds);
        }
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) {
            throw new IOException("cannot decode selected image");
        }
        Size target = boundedSize(bounds.outWidth, bounds.outHeight);
        int sample = 1;
        while (bounds.outWidth / (sample * 2) >= target.getWidth()
                && bounds.outHeight / (sample * 2) >= target.getHeight()) {
            sample *= 2;
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = sample;
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap decoded;
        try (InputStream input = requireInput(uri)) {
            decoded = BitmapFactory.decodeStream(input, null, options);
        }
        if (decoded == null) throw new IOException("cannot decode selected image");
        decoded = applyExifOrientation(uri, decoded);
        Size orientedTarget = boundedSize(decoded.getWidth(), decoded.getHeight());
        if (decoded.getWidth() == orientedTarget.getWidth()
                && decoded.getHeight() == orientedTarget.getHeight()) return decoded;
        Bitmap scaled = Bitmap.createScaledBitmap(decoded, orientedTarget.getWidth(),
                orientedTarget.getHeight(), true);
        if (scaled != decoded) decoded.recycle();
        return scaled;
    }

    private Bitmap applyExifOrientation(Uri uri, Bitmap bitmap) {
        int orientation;
        try (InputStream input = requireInput(uri)) {
            orientation = new ExifInterface(input).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        } catch (IOException | RuntimeException ignored) {
            return bitmap;
        }

        Matrix transform = new Matrix();
        switch (orientation) {
            case ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> transform.setScale(-1f, 1f);
            case ExifInterface.ORIENTATION_ROTATE_180 -> transform.setRotate(180f);
            case ExifInterface.ORIENTATION_FLIP_VERTICAL -> transform.setScale(1f, -1f);
            case ExifInterface.ORIENTATION_TRANSPOSE -> {
                transform.setRotate(90f);
                transform.postScale(-1f, 1f);
            }
            case ExifInterface.ORIENTATION_ROTATE_90 -> transform.setRotate(90f);
            case ExifInterface.ORIENTATION_TRANSVERSE -> {
                transform.setRotate(270f);
                transform.postScale(-1f, 1f);
            }
            case ExifInterface.ORIENTATION_ROTATE_270 -> transform.setRotate(270f);
            default -> {
                return bitmap;
            }
        }
        Bitmap oriented = Bitmap.createBitmap(bitmap, 0, 0, bitmap.getWidth(),
                bitmap.getHeight(), transform, true);
        if (oriented != bitmap) bitmap.recycle();
        return oriented;
    }

    private static Size boundedSize(int width, int height) throws IOException {
        if (width < 2 || height < 2) throw new IOException("invalid image dimensions");
        return boundedSizeValid(width, height);
    }

    private static Size boundedSizeValid(int width, int height) {
        double scale = 1.0;
        if (width > MAX_IMAGE_WIDTH) {
            scale = Math.min(scale, (double) MAX_IMAGE_WIDTH / width);
        }
        if (height > MAX_IMAGE_HEIGHT) {
            scale = Math.min(scale, (double) MAX_IMAGE_HEIGHT / height);
        }
        long pixels = (long) width * height;
        if (pixels > MAX_IMAGE_PIXELS) {
            scale = Math.min(scale, Math.sqrt((double) MAX_IMAGE_PIXELS / pixels));
        }
        int outWidth = Math.max(16, ((int) Math.floor(width * scale)) & ~1);
        int outHeight = Math.max(16, ((int) Math.floor(height * scale)) & ~1);
        return new Size(outWidth, outHeight);
    }

    private long querySize(Uri uri) {
        try (Cursor cursor = resolver.query(uri,
                new String[]{OpenableColumns.SIZE},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) {
                    return cursor.getLong(sizeIndex);
                }
            }
        } catch (RuntimeException ignored) {
            // The stream itself remains authoritative.
        }
        return -1L;
    }

    private void requireMime(Uri uri, String prefix) throws IOException {
        String mime = resolver.getType(uri);
        if (mime != null && !mime.toLowerCase(Locale.ROOT).startsWith(prefix)) {
            throw new IOException("selected document has unsupported type: " + mime);
        }
    }

    private InputStream requireInput(Uri uri) throws IOException {
        InputStream input = resolver.openInputStream(uri);
        if (input == null) throw new IOException("cannot open selected document");
        return input;
    }

    private void cleanup(String... paths) {
        if (paths.length == 0) return;
        StringBuilder command = new StringBuilder("rm -f");
        for (String path : paths) command.append(" '").append(path).append("'");
        command.append(" 2>/dev/null; true");
        try {
            shell.execute(command.toString(), 15L);
        } catch (IOException ignored) {
            // Best-effort cleanup must not mask the transaction result.
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private static MessageDigest sha256() throws IOException {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("SHA-256 unavailable", impossible);
        }
    }

    private static String hex(byte[] value) {
        StringBuilder result = new StringBuilder(value.length * 2);
        for (byte item : value) {
            result.append(String.format(Locale.ROOT, "%02x", item & 0xff));
        }
        return result.toString();
    }

    private static int clamp(int value) {
        return Math.max(0, Math.min(255, value));
    }

    private static void requireOk(String step, RootShell.Result result)
            throws IOException {
        if (!result.ok()) throw new IOException(step + " failed: " + result.describe());
    }

    private static final class SourceConfig {
        final String visualMode;
        final String visualKind;
        final String visualPath;
        final String audioMode;
        final String audioPath;
        final String obsBind;
        final String obsToken;
        final String generation;

        SourceConfig(String visualMode, String visualKind, String visualPath,
                     String audioMode, String audioPath, String obsBind,
                     String obsToken) {
            this.visualMode = visualMode;
            this.visualKind = visualKind;
            this.visualPath = visualPath;
            this.audioMode = audioMode;
            this.audioPath = audioPath;
            this.obsBind = obsBind;
            this.obsToken = obsToken;
            this.generation = Long.toUnsignedString(System.nanoTime());
        }

        static SourceConfig parse(String raw) throws IOException {
            Map<String, String> values = new LinkedHashMap<>();
            for (String line : raw.split("\\R")) {
                int delimiter = line.indexOf('=');
                if (delimiter <= 0) continue;
                String key = line.substring(0, delimiter).trim();
                String value = line.substring(delimiter + 1).trim();
                if (key.matches("[a-z_]+") && value.length() <= 512) {
                    if (values.putIfAbsent(key, value) != null) {
                        throw new IOException("duplicate source configuration key");
                    }
                }
            }
            if (values.size() == 1 && "2".equals(values.get("schema"))) {
                return new SourceConfig("local", "none", "", "local", "",
                        "loopback", "");
            }
            if (!"2".equals(values.get("schema"))) {
                throw new IOException("unsupported source configuration schema");
            }
            String visualMode = values.getOrDefault("visual_mode", "local");
            String visualKind = values.getOrDefault("visual_kind", "none");
            String visualPath = values.getOrDefault("visual_path", "");
            String audioMode = values.getOrDefault("audio_mode", "local");
            String audioPath = values.getOrDefault("audio_path", "");
            String obsBind = values.getOrDefault("obs_bind", "loopback");
            String obsToken = values.getOrDefault("obs_token", "");
            if (!modeValid(visualMode) || !modeValid(audioMode)
                    || !("none".equals(visualKind) || "video".equals(visualKind)
                    || "image".equals(visualKind))
                    || !("loopback".equals(obsBind) || "lan".equals(obsBind))
                    || (!visualPath.isEmpty() && !validMediaPath(visualPath, "visual"))
                    || (!audioPath.isEmpty() && !validMediaPath(audioPath, "audio"))
                    || (!obsToken.isEmpty() && !obsToken.matches("[0-9a-f]{64}"))) {
                throw new IOException("invalid source configuration");
            }
            if (("obs".equals(visualMode) || "obs".equals(audioMode))
                    && obsToken.isEmpty()) {
                throw new IOException("OBS source is missing its authentication token");
            }
            return new SourceConfig(visualMode, visualKind, visualPath, audioMode,
                    audioPath, obsBind, obsToken);
        }

        SourceConfig withVisual(String kind, String path) {
            return new SourceConfig("local", kind, path, audioMode, audioPath,
                    obsBind, obsToken);
        }

        SourceConfig withAudio(String path) {
            return new SourceConfig(visualMode, visualKind, visualPath, "local",
                    path, obsBind, obsToken);
        }

        SourceConfig withObs(String token, String bind) {
            return new SourceConfig("obs", visualKind, visualPath, "obs",
                    audioPath, bind, token);
        }

        String serialize() {
            return "schema=2\n"
                    + "generation=" + generation + "\n"
                    + "visual_mode=" + visualMode + "\n"
                    + "visual_kind=" + visualKind + "\n"
                    + "visual_path=" + visualPath + "\n"
                    + "audio_mode=" + audioMode + "\n"
                    + "audio_path=" + audioPath + "\n"
                    + "obs_bind=" + obsBind + "\n"
                    + "obs_token=" + obsToken + "\n";
        }

        static boolean validMediaPath(String path, String type) {
            return path.matches("^/data/local/tmp/longvc-media/" + type
                    + "-[0-9a-f]{64}\\.media$");
        }

        private static boolean modeValid(String mode) {
            return "local".equals(mode) || "obs".equals(mode);
        }
    }

    private record SourceSnapshot(SourceConfig config, String digest) {}

    static final class SourceConflictException extends IOException {
        SourceConflictException() {
            super("来源或运行状态已由另一个进程更改，请刷新后重试");
        }
    }

    final class PreparedMedia implements AutoCloseable {
        private final String staged;
        private final String type;
        private final String kind;
        private final Fingerprint fingerprint;

        private PreparedMedia(String staged, String type, String kind, Fingerprint fingerprint) {
            this.staged = staged;
            this.type = type;
            this.kind = kind;
            this.fingerprint = fingerprint;
        }

        @Override
        public void close() {
            cleanup(staged);
        }
    }

    private static final class Fingerprint {
        final String sha256;
        final long size;

        Fingerprint(String sha256, long size) {
            this.sha256 = sha256;
            this.size = size;
        }
    }

    private static final class CountingInputStream extends FilterInputStream {
        final long limit;
        long count;
        boolean exceeded;

        CountingInputStream(InputStream input, long limit) {
            super(input);
            this.limit = limit;
        }

        @Override
        public int read() throws IOException {
            int value = super.read();
            if (value >= 0) increment(1);
            return value;
        }

        @Override
        public int read(byte[] buffer, int offset, int length) throws IOException {
            int allowed = (int) Math.min(length, Math.max(1L, limit + 1L - count));
            int read = super.read(buffer, offset, allowed);
            if (read > 0) increment(read);
            return read;
        }

        private void increment(int amount) throws IOException {
            count += amount;
            if (count > limit) {
                exceeded = true;
                throw new IOException("media file exceeds size limit");
            }
        }
    }
}
