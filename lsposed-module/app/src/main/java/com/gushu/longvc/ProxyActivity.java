package com.gushu.longvc;

import static android.view.ViewGroup.LayoutParams.MATCH_PARENT;
import static android.view.ViewGroup.LayoutParams.WRAP_CONTENT;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.text.InputType;
import android.view.View;
import android.view.WindowInsets;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class ProxyActivity extends Activity {
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor();
    private RootProxyController controller;
    private LinearLayout fields;
    private TextView result;
    private Spinner protocols;
    private EditText host;
    private EditText port;
    private EditText user;
    private EditText password;
    private EditText sni;
    private EditText url;
    private TextView userLabel;
    private TextView passwordLabel;
    private TextView sniLabel;
    private Button start;
    private Button stop;
    private Button refresh;
    private Button test;

    @Override
    protected void attachBaseContext(Context base) {
        Configuration configuration = new Configuration(base.getResources().getConfiguration());
        boolean light = base.getSharedPreferences("studio_appearance", MODE_PRIVATE).getBoolean("light", false);
        configuration.uiMode = (configuration.uiMode & ~Configuration.UI_MODE_NIGHT_MASK)
                | (light ? Configuration.UI_MODE_NIGHT_NO : Configuration.UI_MODE_NIGHT_YES);
        super.attachBaseContext(base.createConfigurationContext(configuration));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        controller = new RootProxyController(this);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        ScrollView page = new ScrollView(this);
        layout.addView(page, new LinearLayout.LayoutParams(MATCH_PARENT, 0, 1));
        fields = new LinearLayout(this);
        fields.setOrientation(LinearLayout.VERTICAL);
        int inset = dp(16);
        fields.setPadding(inset, inset, inset, inset);
        page.addView(fields);
        LinearLayout actions = new LinearLayout(this);
        actions.setPadding(inset, dp(4), inset, dp(4));
        actions.setBackgroundColor(getColor(R.color.surface_card));
        layout.addView(actions);
        setContentView(layout);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            layout.setOnApplyWindowInsetsListener((view, insets) -> {
                Insets visible = insets.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                view.setPadding(visible.left, visible.top, visible.right, visible.bottom);
                actions.setVisibility(insets.isVisible(WindowInsets.Type.ime()) ? View.GONE : View.VISIBLE);
                return WindowInsets.CONSUMED;
            });
            layout.requestApplyInsets();
        }
        fields.addView(button(R.string.proxy_back, this::finish));
        TextView heading = label(R.string.proxy_title);
        heading.setTextAppearance(R.style.PageTitle);
        label(R.string.proxy_intro);
        protocols = new Spinner(this);
        protocols.setId(R.id.proxy_protocol_field);
        protocols.setMinimumHeight(dp(48));
        TextView protocolLabel = label(R.string.proxy_protocol);
        protocolLabel.setLabelFor(protocols.getId());
        ArrayAdapter<CharSequence> options = ArrayAdapter.createFromResource(
                this, R.array.proxy_protocols, android.R.layout.simple_spinner_item);
        options.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        protocols.setAdapter(options);
        fields.addView(protocols);
        host = field(R.string.proxy_host, R.id.proxy_host_field, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        port = field(R.string.proxy_port, R.id.proxy_port_field, InputType.TYPE_CLASS_NUMBER);
        userLabel = label(R.string.proxy_username);
        user = input(userLabel, R.id.proxy_user_field, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        passwordLabel = label(R.string.proxy_password);
        password = input(passwordLabel, R.id.proxy_password_field, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        sniLabel = label(R.string.proxy_sni);
        sni = input(sniLabel, R.id.proxy_sni_field, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        url = field(R.string.proxy_url, R.id.proxy_url_field, InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        user.setSaveEnabled(false);
        password.setSaveEnabled(false);
        url.setSaveEnabled(false);
        protocols.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                ProxyProfile.Protocol protocol = ProxyProfile.Protocol.values()[position];
                boolean uuid = protocol == ProxyProfile.Protocol.VLESS || protocol == ProxyProfile.Protocol.VMESS;
                userLabel.setText(uuid ? R.string.proxy_uuid : R.string.proxy_username);
                show(userLabel, user, protocol != ProxyProfile.Protocol.HYSTERIA2);
                show(passwordLabel, password, !uuid);
                show(sniLabel, sni, protocol.tls);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        result = label(R.string.proxy_idle);
        result.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        test = button(R.string.proxy_test, this::test);
        start = button(R.string.proxy_start, this::confirmStart);
        fields.addView(test);
        fields.addView(start);
        refresh = button(R.string.refresh, () -> run(controller::status));
        stop = button(R.string.proxy_stop, () -> run(controller::stop));
        actions.addView(refresh, new LinearLayout.LayoutParams(0, WRAP_CONTENT, 1));
        actions.addView(stop, new LinearLayout.LayoutParams(0, WRAP_CONTENT, 2));
        run(controller::status);
    }

    private void confirmStart() {
        ProxyProfile profile = profile();
        if (profile == null) return;
        String address = value(url).trim().isEmpty() ? "" : testAddress();
        if (address == null) return;
        new AlertDialog.Builder(this)
                .setTitle(R.string.proxy_confirmation)
                .setMessage(address.isEmpty() ? R.string.proxy_confirmation_detail : R.string.proxy_confirmation_test)
                .setNegativeButton(android.R.string.cancel, null)
                .setPositiveButton(R.string.proxy_start, (dialog, which) -> run(() -> {
                    if (!address.isEmpty()) {
                        String preflight = controller.test(profile, address);
                        if (!"response".equals(preflight)) return preflight;
                    }
                    return controller.start(profile);
                }))
                .show();
    }

    private void test() {
        ProxyProfile profile = profile();
        if (profile == null) return;
        String address = testAddress();
        if (address != null) run(() -> controller.test(profile, address));
    }

    private String testAddress() {
        try {
            String address = ProxyProfile.testUrl(value(url));
            url.setError(null);
            return address;
        } catch (IllegalArgumentException invalid) {
            url.setError(getString(R.string.proxy_url_invalid));
            url.requestFocus();
            return null;
        }
    }

    private ProxyProfile profile() {
        try {
            ProxyProfile.Protocol protocol = ProxyProfile.Protocol.values()[protocols.getSelectedItemPosition()];
            return new ProxyProfile(protocol, value(host), value(port),
                    user.getVisibility() == View.VISIBLE ? value(user) : "",
                    password.getVisibility() == View.VISIBLE ? value(password) : "",
                    sni.getVisibility() == View.VISIBLE ? value(sni) : "");
        } catch (IllegalArgumentException invalid) {
            result.setText(R.string.proxy_invalid);
            return null;
        }
    }

    private void run(ProxyTask task) {
        setButtons(false);
        result.setText(R.string.proxy_working);
        WORKER.execute(() -> {
            String code = "unknown";
            long begin = SystemClock.elapsedRealtime();
            try {
                code = task.run();
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            } catch (Exception unavailable) {
                // Root and core output may contain credentials. Use localized codes.
            }
            long elapsed = SystemClock.elapsedRealtime() - begin;
            String status = code;
            runOnUiThread(() -> {
                if (isDestroyed() || isFinishing()) return;
                setButtons(true);
                if ("response".equals(status)) result.setText(getString(R.string.proxy_response, elapsed));
                else result.setText(switch (status) {
                    case "active" -> R.string.proxy_active;
                    case "stopped" -> R.string.proxy_stopped;
                    case "already_running" -> R.string.proxy_already_running;
                    case "unsupported", "route_unavailable" -> R.string.proxy_device_unsupported;
                    case "busy" -> R.string.proxy_busy;
                    case "residue", "route_busy" -> R.string.proxy_residue;
                    case "timeout" -> R.string.proxy_timeout;
                    case "connection_failed" -> R.string.proxy_connection_failed;
                    case "invalid_configuration" -> R.string.proxy_invalid;
                    case "start_failed" -> R.string.proxy_start_failed;
                    case "recovery_unverified", "unverified" -> R.string.proxy_failed;
                    default -> R.string.proxy_unknown;
                });
            });
        });
    }

    private void setButtons(boolean enabled) {
        for (Button button : new Button[]{start, stop, refresh, test}) button.setEnabled(enabled);
    }

    private EditText field(int text, int id, int type) {
        return input(label(text), id, type);
    }

    private EditText input(TextView label, int id, int type) {
        EditText view = new EditText(this);
        view.setId(id);
        view.setMinHeight(dp(48));
        view.setSingleLine(true);
        view.setInputType(type);
        view.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        label.setLabelFor(id);
        fields.addView(view);
        return view;
    }

    private TextView label(int text) {
        TextView view = new TextView(this);
        view.setTextAppearance(R.style.BodyText);
        view.setText(text);
        view.setPadding(0, dp(8), 0, dp(4));
        fields.addView(view);
        return view;
    }

    private Button button(int text, Runnable onClick) {
        Button view = new Button(this);
        view.setText(text);
        view.setAllCaps(false);
        view.setMinHeight(dp(48));
        view.setOnClickListener(v -> onClick.run());
        return view;
    }

    private int dp(int value) {
        return Math.round(getResources().getDisplayMetrics().density * value);
    }

    private static void show(TextView label, EditText view, boolean visible) {
        int visibility = visible ? View.VISIBLE : View.GONE;
        label.setVisibility(visibility);
        view.setVisibility(visibility);
    }

    private static String value(EditText input) {
        return input.getText().toString();
    }

    @FunctionalInterface
    private interface ProxyTask {
        String run() throws Exception;
    }
}
