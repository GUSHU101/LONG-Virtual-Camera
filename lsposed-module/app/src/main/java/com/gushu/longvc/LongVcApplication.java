package com.gushu.longvc;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;

import java.util.Set;
import java.util.concurrent.CopyOnWriteArraySet;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import io.github.libxposed.service.XposedService;
import io.github.libxposed.service.XposedServiceHelper;

public final class LongVcApplication extends Application
        implements XposedServiceHelper.OnServiceListener {
    interface ServiceStateListener {
        void onServiceStateChanged(XposedService service);
    }

    private static final Set<ServiceStateListener> LISTENERS = new CopyOnWriteArraySet<>();
    // One process-wide queue prevents a configuration change from creating a
    // second Activity executor that races an in-flight root transaction.
    private static final ExecutorService ROOT_OPERATIONS =
            Executors.newSingleThreadExecutor(runnable -> {
                Thread thread = new Thread(runnable, "longvc-root-operation");
                thread.setDaemon(true);
                return thread;
            });
    // Recovery must never sit behind a multi-gigabyte media import. It gets a
    // dedicated worker and first interrupts the currently running ordinary
    // operation. The device-side controller lock still serializes mutations.
    private static final ExecutorService EMERGENCY_OPERATIONS =
            Executors.newSingleThreadExecutor(runnable -> {
                Thread thread = new Thread(runnable, "longvc-emergency-stop");
                thread.setDaemon(true);
                return thread;
            });
    private final StudioOperations studioOperations = new StudioOperations(
            new RootOperations(ROOT_OPERATIONS, EMERGENCY_OPERATIONS),
            new Handler(Looper.getMainLooper())::post);
    private static volatile XposedService service;

    static void addServiceStateListener(ServiceStateListener listener, boolean notifyImmediately) {
        LISTENERS.add(listener);
        if (notifyImmediately) listener.onServiceStateChanged(service);
    }

    static void removeServiceStateListener(ServiceStateListener listener) {
        LISTENERS.remove(listener);
    }

    StudioOperations studioOperations() {
        return studioOperations;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        XposedServiceHelper.registerListener(this);
    }

    @Override
    public void onServiceBind(XposedService boundService) {
        service = boundService;
        notifyListeners(boundService);
    }

    @Override
    public void onServiceDied(XposedService deadService) {
        if (service == deadService) service = null;
        notifyListeners(service);
    }

    private static void notifyListeners(XposedService current) {
        for (ServiceStateListener listener : LISTENERS) {
            listener.onServiceStateChanged(current);
        }
    }
}
