package com.gushu.longvc;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Build;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.VideoView;

import java.io.IOException;
import java.io.InputStream;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

final class StudioPreview {
    private static final int PREVIEW_LIMIT = 960;
    private static final String URI = "visual_uri";
    private static final String KIND = "visual_kind";
    private static final String SOURCE_ID = "visual_source_id";
    private final Activity activity;
    private final Runnable refresh;
    private final SharedPreferences preferences;
    private final ExecutorService images = Executors.newSingleThreadExecutor();
    private final View[] panels;
    private RuntimeState state;
    private Uri recentUri;
    private String recentKind;
    private String recentSourceId;
    private View playingPanel;
    private View player;
    private Future<?> imageTask;
    private boolean busy;
    private long request;

    StudioPreview(Activity activity, Runnable refresh) {
        this.activity = activity;
        this.refresh = refresh;
        preferences = activity.getSharedPreferences("studio_preview", Activity.MODE_PRIVATE);
        String saved = preferences.getString(URI, null);
        recentUri = saved == null ? null : Uri.parse(saved);
        recentKind = preferences.getString(KIND, "");
        recentSourceId = preferences.getString(SOURCE_ID, "");
        panels = new View[]{activity.findViewById(R.id.workbench_preview),
                activity.findViewById(R.id.source_preview)};
        for (View panel : panels) {
            panel.findViewById(R.id.play_preview).setOnClickListener(view -> toggle(panel));
        }
    }

    void remember(Uri uri, RuntimeState imported) {
        if (imported.visualSourceId.isEmpty()) return;
        stop();
        Uri previous = recentUri;
        recentUri = uri;
        recentKind = imported.visualKind;
        recentSourceId = imported.visualSourceId;
        try {
            activity.getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            preferences.edit().putString(URI, uri.toString()).putString(KIND, recentKind)
                    .putString(SOURCE_ID, recentSourceId).apply();
        } catch (SecurityException unavailable) {
            preferences.edit().remove(URI).remove(KIND).remove(SOURCE_ID).apply();
        }
        if (previous != null && !previous.equals(uri)) {
            try {
                activity.getContentResolver().releasePersistableUriPermission(previous, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            } catch (SecurityException ignored) {
                // Some providers only grant access for the active task.
            }
        }
    }

    void render(RuntimeState current) {
        state = current;
        if (!localAvailable() && playingPanel != null) stop();
        else renderPanels();
    }

    void setBusy(boolean busy) {
        this.busy = busy;
        renderPanels();
    }

    private boolean localAvailable() {
        return recentUri != null && state != null && state.matchesLocalVisual(recentKind, recentSourceId);
    }

    private boolean usesObs() {
        return state != null && state.controllerVerified && state.visualUsesObs();
    }

    private void renderPanels() {
        boolean obs = usesObs();
        boolean local = localAvailable();
        int title = obs ? R.string.preview_obs : local ? R.string.preview_source_only : R.string.preview_empty;
        int status = obs ? state.videoChannelReady ? R.string.preview_obs_ready : R.string.preview_obs_waiting
                : local ? R.string.preview_source_only : R.string.preview_empty;
        for (View panel : panels) {
            TextView previewTitle = panel.findViewById(R.id.preview_title);
            TextView previewState = panel.findViewById(R.id.preview_state);
            TextView note = panel.findViewById(R.id.preview_note);
            Button button = panel.findViewById(R.id.play_preview);
            previewTitle.setText(title);
            if (panel != playingPanel) {
                previewState.setVisibility(View.VISIBLE);
                previewState.setText(status);
            }
            if (local) {
                note.setText(activity.getString(R.string.preview_local_ready,
                        activity.getString("image".equals(recentKind) ? R.string.local_image : R.string.local_video)));
            } else {
                note.setText(obs ? R.string.preview_obs_note : R.string.preview_local_unavailable);
            }
            button.setText(panel == playingPanel ? R.string.preview_stop : obs ? R.string.refresh : R.string.preview_play);
            button.setEnabled(local || (obs && !busy));
        }
    }

    private void toggle(View panel) {
        if (usesObs()) {
            refresh.run();
            return;
        }
        if (playingPanel == panel) {
            stop();
            return;
        }
        if (!localAvailable()) return;
        stop();
        playingPanel = panel;
        renderPanels();
        TextView status = panel.findViewById(R.id.preview_state);
        status.setText(R.string.preview_loading);
        long generation = request;
        Uri uri = recentUri;
        if ("image".equals(recentKind)) {
            imageTask = images.submit(() -> {
                try {
                    Bitmap bitmap = loadImage(activity.getContentResolver(), uri);
                    activity.runOnUiThread(() -> {
                        if (generation != request) return;
                        ImageView image = new ImageView(activity);
                        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
                        image.setImageBitmap(bitmap);
                        mount(panel, image);
                        status.setVisibility(View.GONE);
                    });
                } catch (IOException | RuntimeException error) {
                    activity.runOnUiThread(() -> fail(panel, generation));
                }
            });
        } else {
            VideoView video = new VideoView(activity);
            video.setAudioFocusRequest(AudioManager.AUDIOFOCUS_NONE);
            video.setOnPreparedListener(media -> {
                if (generation != request) return;
                media.setVolume(0, 0);
                status.setVisibility(View.GONE);
                video.start();
            });
            video.setOnErrorListener((media, what, extra) -> {
                fail(panel, generation);
                return true;
            });
            video.setOnCompletionListener(media -> {
                if (generation == request) stop();
            });
            mount(panel, video);
            try {
                video.setVideoURI(uri);
            } catch (RuntimeException error) {
                fail(panel, generation);
            }
        }
    }

    private void mount(View panel, View view) {
        FrameLayout frame = panel.findViewById(R.id.preview_canvas);
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER);
        frame.addView(view, 0, params);
        player = view;
    }

    private void fail(View panel, long generation) {
        if (generation != request) return;
        stop();
        TextView note = panel.findViewById(R.id.preview_note);
        note.setText(R.string.preview_failed);
    }

    void stop() {
        ++request;
        if (imageTask != null) imageTask.cancel(true);
        imageTask = null;
        if (player instanceof VideoView video) video.stopPlayback();
        if (player != null) ((FrameLayout) player.getParent()).removeView(player);
        player = null;
        playingPanel = null;
        renderPanels();
    }

    void close() {
        stop();
        images.shutdownNow();
    }

    private static Bitmap loadImage(ContentResolver resolver, Uri uri) throws IOException {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            return ImageDecoder.decodeBitmap(ImageDecoder.createSource(resolver, uri), (decoder, info, source) -> {
                int width = info.getSize().getWidth();
                int height = info.getSize().getHeight();
                float scale = Math.min(1f, (float) PREVIEW_LIMIT / Math.max(width, height));
                decoder.setTargetSize(Math.max(1, Math.round(width * scale)), Math.max(1, Math.round(height * scale)));
            });
        }
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inJustDecodeBounds = true;
        try (InputStream input = resolver.openInputStream(uri)) {
            BitmapFactory.decodeStream(input, null, options);
        }
        if (options.outWidth <= 0 || options.outHeight <= 0) throw new IOException("invalid image");
        options.inSampleSize = 1;
        while (Math.max(options.outWidth, options.outHeight) / options.inSampleSize > PREVIEW_LIMIT) {
            options.inSampleSize *= 2;
        }
        options.inJustDecodeBounds = false;
        try (InputStream input = resolver.openInputStream(uri)) {
            Bitmap bitmap = BitmapFactory.decodeStream(input, null, options);
            if (bitmap == null) throw new IOException("invalid image");
            return bitmap;
        }
    }
}
