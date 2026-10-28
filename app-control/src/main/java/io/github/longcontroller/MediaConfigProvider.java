package io.github.longcontroller;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.UriMatcher;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Binder;
import android.os.ParcelFileDescriptor;
import android.os.Process;

import java.io.File;
import java.io.FileNotFoundException;
import java.util.Arrays;
import java.util.Collections;

public final class MediaConfigProvider extends ContentProvider {
    private static final int CONFIG = 1;
    private static final int MEDIA = 2;
    private UriMatcher matcher;
    private ConfigStore store;

    public static String authority(Context context) {
        return context.getPackageName() + ".media";
    }

    public static Uri configUri(Context context) {
        return Uri.parse("content://" + authority(context) + "/config");
    }

    public static Uri mediaUri(Context context) {
        return Uri.parse("content://" + authority(context) + "/media");
    }

    @Override
    public boolean onCreate() {
        Context context = requireContext();
        store = new ConfigStore(context);
        matcher = new UriMatcher(UriMatcher.NO_MATCH);
        matcher.addURI(authority(context), "config", CONFIG);
        matcher.addURI(authority(context), "media", MEDIA);
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        enforceAuthorizedCaller();
        if (matcher.match(uri) != CONFIG) {
            throw new IllegalArgumentException("Unsupported query URI");
        }
        MatrixCursor cursor = new MatrixCursor(new String[]{
                "enabled", "generation", "source_name", "source_mime", "source_sha256",
                "deployment_mode", "evidence"
        }, 1);
        cursor.addRow(new Object[]{
                store.isEnabled() ? 1 : 0,
                store.generation(),
                store.sourceName(),
                store.sourceMime(),
                store.sourceSha256(),
                "LSPOSED_EMBEDDED",
                "NOT_RUN"
        });
        cursor.setNotificationUri(requireContext().getContentResolver(), uri);
        return cursor;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        enforceAuthorizedCaller();
        if (matcher.match(uri) != MEDIA || !"r".equals(mode)) {
            throw new FileNotFoundException("Read-only media endpoint");
        }
        if (Binder.getCallingUid() != Process.myUid() && !store.isEnabled()) {
            throw new FileNotFoundException("Virtual media is disabled");
        }
        File source = store.sourceFile();
        if (!source.isFile() || source.length() == 0L) {
            throw new FileNotFoundException("No imported media");
        }
        return ParcelFileDescriptor.open(source, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public String getType(Uri uri) {
        enforceAuthorizedCaller();
        if (matcher.match(uri) == CONFIG) return "vnd.android.cursor.item/vnd.long.vcam.config";
        if (matcher.match(uri) == MEDIA) return store.sourceMime();
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException("Provider is read-only");
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Provider is read-only");
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException("Provider is read-only");
    }

    private void enforceAuthorizedCaller() {
        int uid = Binder.getCallingUid();
        if (uid == Process.myUid()) return;
        if (!TargetDeviceProfile.CROSS_APP_BACKEND_READY) {
            throw new SecurityException("Cross-app backend is disabled in this build");
        }
        String[] packages = requireContext().getPackageManager().getPackagesForUid(uid);
        if (packages == null) packages = new String[0];
        if (packages.length != 1 || !store.allowedPackages().contains(packages[0])) {
            throw new SecurityException("Caller UID is not in the user-approved target list");
        }
    }

}
