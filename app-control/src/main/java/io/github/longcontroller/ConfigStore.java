package io.github.longcontroller;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.graphics.Bitmap;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;

public final class ConfigStore {
    private static final String PREFS = "long_vcam_config_v1";
    private static final String KEY_ENABLED = "enabled";
    private static final String KEY_SOURCE_NAME = "source_name";
    private static final String KEY_SOURCE_MIME = "source_mime";
    private static final String KEY_SOURCE_SHA256 = "source_sha256";
    private static final String KEY_ALLOWED_PACKAGES = "allowed_packages";
    private static final String KEY_GENERATION = "generation";
    private static final String SOURCE_FILE = "virtual-source.media";
    private static final String KEY_SOURCE_FILE = "source_file";
    private static final Object IMPORT_LOCK = new Object();
    public static final long MAX_IMPORT_BYTES = 256L * 1024L * 1024L;

    private final Context context;
    private final SharedPreferences preferences;

    public ConfigStore(Context context) {
        this.context = context.getApplicationContext();
        this.preferences = this.context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public File sourceFile() {
        String name = preferences.getString(KEY_SOURCE_FILE, SOURCE_FILE);
        if (name == null || !(name.equals(SOURCE_FILE) || name.matches("source-[a-f0-9-]{36}\\.media"))) name = SOURCE_FILE;
        return new File(context.getFilesDir(), name);
    }

    public boolean hasSource() {
        File source = sourceFile();
        return source.isFile() && source.length() > 0;
    }

    public String sourceName() {
        return preferences.getString(KEY_SOURCE_NAME, "未选择");
    }

    public String sourceMime() {
        return preferences.getString(KEY_SOURCE_MIME, "application/octet-stream");
    }

    public String sourceSha256() {
        return preferences.getString(KEY_SOURCE_SHA256, "");
    }

    public boolean isEnabled() {
        return TargetDeviceProfile.CROSS_APP_BACKEND_READY
                && DeviceProbe.targetMatch().matches() && preferences.getBoolean(KEY_ENABLED, false);
    }

    public long generation() {
        return preferences.getLong(KEY_GENERATION, 0L);
    }

    public Set<String> allowedPackages() {
        return Collections.unmodifiableSet(new HashSet<>(
                preferences.getStringSet(KEY_ALLOWED_PACKAGES, Collections.emptySet())));
    }

    public void setAllowedPackages(Set<String> packages) {
        Set<String> clean = new HashSet<>();
        for (String packageName : packages) {
            if (packageName != null && packageName.matches("[A-Za-z0-9_.]+")) {
                clean.add(packageName);
            }
        }
        SharedPreferences.Editor editor = preferences.edit()
                .putStringSet(KEY_ALLOWED_PACKAGES, clean)
                .putLong(KEY_GENERATION, generation() + 1L);
        editor.putBoolean(KEY_ENABLED, false);
        editor.commit();
        notifyChanged();
    }

    public void setEnabled(boolean enabled) {
        if (enabled && (!TargetDeviceProfile.CROSS_APP_BACKEND_READY
                || !DeviceProbe.targetMatch().matches() || !ModuleStatus.isActive())) {
            throw new IllegalStateException("当前构建未开放跨应用替换；请先完成 Pixel 5 原生基线验收");
        }
        if (enabled && (!hasSource() || allowedPackages().isEmpty())) {
            throw new IllegalStateException("请先选择视频和至少一个目标应用");
        }
        preferences.edit()
                .putBoolean(KEY_ENABLED, enabled)
                .putLong(KEY_GENERATION, generation() + 1L)
                .commit();
        notifyChanged();
    }

    public ImportResult importVideo(ContentResolver resolver, Uri uri, String displayName)
            throws IOException {
        synchronized (IMPORT_LOCK) {
            return importVideoLocked(resolver, uri, displayName);
        }
    }

    private ImportResult importVideoLocked(ContentResolver resolver, Uri uri, String displayName)
            throws IOException {
        if (Thread.currentThread().isInterrupted()) throw new IOException("导入已取消");
        setEnabled(false);
        File previous = sourceFile();
        File destination = new File(context.getFilesDir(), "source-" + UUID.randomUUID() + ".media");
        File temporary = new File(context.getFilesDir(), destination.getName() + ".tmp");
        MessageDigest digest;
        try {
            digest = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException impossible) {
            throw new IOException("SHA-256 unavailable", impossible);
        }

        long bytes = 0L;
        try (InputStream input = resolver.openInputStream(uri);
             FileOutputStream output = new FileOutputStream(temporary, false)) {
            if (input == null) {
                throw new IOException("无法读取所选媒体");
            }
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) >= 0) {
                if (Thread.currentThread().isInterrupted()) throw new IOException("导入已取消");
                if (count == 0) continue;
                if (bytes + count > MAX_IMPORT_BYTES) throw new IOException("视频超过 256 MiB 导入上限");
                output.write(buffer, 0, count);
                digest.update(buffer, 0, count);
                bytes += count;
            }
            output.getFD().sync();
        } catch (IOException | RuntimeException error) {
            //noinspection ResultOfMethodCallIgnored
            temporary.delete();
            throw error;
        }
        if (bytes == 0L) {
            //noinspection ResultOfMethodCallIgnored
            temporary.delete();
            throw new IOException("所选媒体为空");
        }
        String mime;
        try {
            mime = validateVideo(temporary);
            if (Thread.currentThread().isInterrupted()) throw new IOException("导入已取消");
        } catch (IOException | RuntimeException error) {
            temporary.delete();
            throw new IOException("媒体验证失败：首阶段支持 H.264、最大 1920×1080 像素面积、可解码视频", error);
        }
        if (!temporary.renameTo(destination)) {
            temporary.delete();
            throw new IOException("无法原子提交媒体；旧媒体保持不变");
        }

        String hash = hex(digest.digest());
        boolean committed = preferences.edit()
                .putString(KEY_SOURCE_FILE, destination.getName())
                .putString(KEY_SOURCE_NAME, safeDisplayName(displayName))
                .putString(KEY_SOURCE_MIME, mime == null ? "video/*" : mime)
                .putString(KEY_SOURCE_SHA256, hash)
                .putBoolean(KEY_ENABLED, false)
                .putLong(KEY_GENERATION, generation() + 1L)
                .commit();
        if (!committed) {
            // Keep both versions: SharedPreferences may already have updated its in-memory view.
            throw new IOException("配置写盘未确认；已保留新旧媒体，请重启后核验");
        }
        if (!previous.equals(destination)) previous.delete();
        notifyChanged();
        return new ImportResult(bytes, hash);
    }

    public void clearSource() throws IOException {
        synchronized (IMPORT_LOCK) {
            if (!preferences.edit().remove(KEY_SOURCE_FILE).remove(KEY_SOURCE_NAME)
                    .remove(KEY_SOURCE_SHA256).remove(KEY_SOURCE_MIME).putBoolean(KEY_ENABLED, false)
                    .putLong(KEY_GENERATION, generation() + 1).commit()) throw new IOException("配置写盘失败");
            File[] files = context.getFilesDir().listFiles();
            boolean failed = false;
            if (files != null) for (File file : files) {
                if (file.getName().equals(SOURCE_FILE) || file.getName().matches("source-[a-f0-9-]{36}\\.media(\\.tmp)?")) {
                    if (file.isFile() && !file.delete()) failed = true;
                }
            }
            notifyChanged();
            if (failed) throw new IOException("部分媒体副本无法删除");
        }
    }

    private static String safeDisplayName(String name) {
        if (name == null) return "virtual-source";
        String clean = name.replaceAll("[\\p{Cc}\\p{Cf}]", "");
        return clean.substring(0, Math.min(clean.length(), 160));
    }

    private static String validateVideo(File file) throws IOException {
        MediaExtractor extractor = new MediaExtractor();
        try {
            extractor.setDataSource(file.getAbsolutePath());
            boolean found = false;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat format = extractor.getTrackFormat(i);
                String mime = format.getString(MediaFormat.KEY_MIME);
                if (mime == null || !mime.startsWith("video/")) continue;
                int width = format.getInteger(MediaFormat.KEY_WIDTH);
                int height = format.getInteger(MediaFormat.KEY_HEIGHT);
                if (!"video/avc".equals(mime) || width < 1 || height < 1
                        || width > 1920 || height > 1920 || (long) width * height > 1920L * 1080L) {
                    throw new IOException("Unsupported video track");
                }
                found = true;
            }
            if (!found) throw new IOException("No video track");
        } finally {
            extractor.release();
        }
        try (MediaMetadataRetriever retriever = new MediaMetadataRetriever()) {
            retriever.setDataSource(file.getAbsolutePath());
            Bitmap frame = retriever.getFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC);
            if (frame == null) throw new IOException("No decodable frame");
            frame.recycle();
            String mime = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_MIMETYPE);
            return mime != null && mime.startsWith("video/") ? mime : "application/octet-stream";
        }
    }

    private void notifyChanged() {
        context.getContentResolver().notifyChange(MediaConfigProvider.configUri(context), null);
    }

    private static String hex(byte[] bytes) {
        StringBuilder value = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) {
            value.append(String.format(Locale.ROOT, "%02x", item & 0xff));
        }
        return value.toString();
    }

    public record ImportResult(long bytes, String sha256) {}
}
