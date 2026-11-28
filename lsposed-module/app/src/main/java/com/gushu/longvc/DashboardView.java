package com.gushu.longvc;

import android.app.Activity;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.ScrollView;
import android.widget.TextView;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

final class DashboardView {
    private static final String SELECTED_PAGE = "dashboard_page";

    enum Page {
        CONTROL(R.id.control_page, R.id.nav_control),
        SOURCES(R.id.source_page, R.id.nav_sources),
        DIAGNOSTICS(R.id.diagnostics_page, R.id.nav_diagnostics),
        SETTINGS(R.id.settings_page, R.id.nav_settings);

        final int contentId;
        final int tabId;

        Page(int contentId, int tabId) {
            this.contentId = contentId;
            this.tabId = tabId;
        }
    }

    enum Notice { NONE, BUSY, INFO, ERROR }

    private final Activity activity;
    private final View banner;
    private final TextView feedback;
    private final View progress;
    private final View detailsButton;
    private final View operationCard;
    private final TextView operationLog;
    private final Runnable onPageLeaving;
    private final OnBackInvokedCallback backToControl;
    private Page current;
    private Notice notice = Notice.NONE;

    DashboardView(Activity activity, Bundle savedState, Runnable onPageLeaving) {
        this.activity = activity;
        this.onPageLeaving = onPageLeaving;
        backToControl = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                ? () -> show(Page.CONTROL) : null;
        banner = activity.findViewById(R.id.feedback_banner);
        feedback = activity.findViewById(R.id.feedback_text);
        progress = activity.findViewById(R.id.feedback_progress);
        detailsButton = activity.findViewById(R.id.feedback_details);
        operationCard = activity.findViewById(R.id.operation_card);
        operationLog = activity.findViewById(R.id.operation_log);
        detailsButton.setOnClickListener(view -> openDiagnostics());
        for (Page page : Page.values()) {
            activity.findViewById(page.tabId).setOnClickListener(view -> show(page));
        }
        Page[] pages = Page.values();
        int index = savedState == null ? 0 : savedState.getInt(SELECTED_PAGE);
        show(index >= 0 && index < pages.length ? pages[index] : Page.CONTROL);
        applyInsets(activity.findViewById(R.id.dashboard_root));
    }

    void save(Bundle state) {
        state.putInt(SELECTED_PAGE, current.ordinal());
    }

    void show(Page next) {
        if (current == next) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            OnBackInvokedDispatcher back = activity.getOnBackInvokedDispatcher();
            if ((current == null || current == Page.CONTROL) && next != Page.CONTROL) {
                back.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, backToControl);
            } else if (current != null && next == Page.CONTROL) {
                back.unregisterOnBackInvokedCallback(backToControl);
            }
        }
        if (current != null) onPageLeaving.run();
        if (current == Page.SOURCES) {
            View focused = activity.getCurrentFocus();
            if (focused != null) {
                activity.getSystemService(InputMethodManager.class)
                        .hideSoftInputFromWindow(focused.getWindowToken(), 0);
                focused.clearFocus();
            }
        }
        current = next;
        for (Page page : Page.values()) {
            boolean selected = page == next;
            activity.findViewById(page.contentId).setVisibility(selected ? View.VISIBLE : View.GONE);
            activity.findViewById(page.tabId).setSelected(selected);
        }
        updateDetailsButton();
    }

    boolean returnToControl() {
        if (current == Page.CONTROL) return false;
        show(Page.CONTROL);
        return true;
    }

    void close() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU && current != Page.CONTROL) {
            activity.getOnBackInvokedDispatcher().unregisterOnBackInvokedCallback(backToControl);
        }
    }

    void openDiagnostics() {
        openDiagnosticsAt(0);
    }

    void openDiagnosticsAt(int anchor) {
        show(Page.DIAGNOSTICS);
        ScrollView page = activity.findViewById(R.id.diagnostics_page);
        page.post(() -> {
            View target = anchor == 0 ? null : page.findViewById(anchor);
            page.scrollTo(0, target == null ? 0 : target.getTop());
        });
    }

    void notice(Notice next, CharSequence message, CharSequence details) {
        notice = next;
        banner.setVisibility(next == Notice.NONE ? View.GONE : View.VISIBLE);
        banner.setBackgroundColor(activity.getColor(
                next == Notice.ERROR ? R.color.warning_fill : R.color.surface_subtle));
        feedback.setTextColor(activity.getColor(
                next == Notice.ERROR ? R.color.error : R.color.ink));
        feedback.setText(message);
        progress.setVisibility(next == Notice.BUSY ? View.VISIBLE : View.GONE);
        operationLog.setText(details);
        operationCard.setVisibility(details.length() == 0 ? View.GONE : View.VISIBLE);
        updateDetailsButton();
    }

    private void updateDetailsButton() {
        detailsButton.setVisibility(notice == Notice.ERROR && current != Page.DIAGNOSTICS
                ? View.VISIBLE : View.GONE);
    }

    private void applyInsets(View root) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            activity.getWindow().setDecorFitsSystemWindows(false);
            root.setOnApplyWindowInsetsListener((view, insets) -> {
                Insets visible = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                view.setPadding(visible.left, visible.top, visible.right, visible.bottom);
                View navigation = view.findViewById(R.id.studio_navigation);
                navigation.setVisibility(insets.isVisible(WindowInsets.Type.ime()) ? View.GONE : View.VISIBLE);
                return WindowInsets.CONSUMED;
            });
            root.requestApplyInsets();
        }
    }
}
