package com.gushu.longvc;

import android.app.Activity;
import android.os.Bundle;
import android.text.format.DateFormat;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.util.ArrayDeque;
import java.util.Date;
import java.util.Deque;

final class StudioEvents {
    private static final int CAPACITY = 30;
    private static final String FILTER = "studio_event_filter";
    private static final Deque<Entry> entries = new ArrayDeque<>();

    enum Level {
        INFO(R.id.events_info, R.string.event_info, R.color.accent_ink),
        WARNING(R.id.events_warn, R.string.event_warn, R.color.warning),
        ERROR(R.id.events_error, R.string.event_error, R.color.error);

        private final int filterId;
        private final int label;
        private final int color;

        Level(int filterId, int label, int color) {
            this.filterId = filterId;
            this.label = label;
            this.color = color;
        }
    }

    private record Entry(Level level, long time, String text) {}

    private final Activity activity;
    private final LinearLayout rows;
    private Level filter;

    StudioEvents(Activity activity, Bundle state) {
        this.activity = activity;
        rows = activity.findViewById(R.id.event_rows);
        activity.findViewById(R.id.events_all).setOnClickListener(view -> filter(null));
        for (Level level : Level.values()) {
            activity.findViewById(level.filterId).setOnClickListener(view -> filter(level));
        }
        activity.findViewById(R.id.clear_events).setOnClickListener(view -> {
            entries.clear();
            render();
        });
        int selection = state == null ? -1 : state.getInt(FILTER, -1);
        Level[] levels = Level.values();
        filter(selection >= 0 && selection < levels.length ? levels[selection] : null);
    }

    void save(Bundle state) {
        state.putInt(FILTER, filter == null ? -1 : filter.ordinal());
    }

    void add(Level level, CharSequence message) {
        if (entries.size() >= CAPACITY) entries.removeLast();
        entries.addFirst(new Entry(level, System.currentTimeMillis(),
                RootShell.redactDiagnostic(message.toString())));
        render();
    }

    private void filter(Level value) {
        filter = value;
        activity.findViewById(R.id.events_all).setSelected(value == null);
        for (Level level : Level.values()) {
            activity.findViewById(level.filterId).setSelected(value == level);
        }
        render();
    }

    private void render() {
        rows.removeAllViews();
        for (Entry entry : entries) {
            if (filter != null && entry.level != filter) continue;
            String time = DateFormat.getTimeFormat(activity).format(new Date(entry.time));
            TextView label = new TextView(activity);
            label.setTextAppearance(R.style.CaptionText);
            label.setText(activity.getString(entry.level.label, time));
            label.setTextColor(activity.getColor(entry.level.color));
            if (rows.getChildCount() > 0) {
                label.setPadding(0, activity.getResources().getDimensionPixelSize(R.dimen.space_small), 0, 0);
            }
            rows.addView(label);
            TextView body = new TextView(activity);
            body.setTextAppearance(R.style.BodyText);
            body.setText(entry.text);
            body.setTextColor(activity.getColor(R.color.ink));
            rows.addView(body);
        }
        if (rows.getChildCount() == 0) {
            TextView empty = new TextView(activity);
            empty.setText(R.string.event_empty);
            empty.setTextColor(activity.getColor(R.color.ink_muted));
            rows.addView(empty);
        }
    }
}
