package com.gushu.longvc;

import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;

/** Keeps the current operation and its result independently of any screen instance. */
final class StudioOperations {
    enum Effect { NONE, VIDEO_IMPORTED, IMAGE_IMPORTED, AUDIO_IMPORTED, OBS_SAVED, OBS_AUDIO_SELECTED }

    record Request(String label, Effect effect, String document) {
        Request(String label) {
            this(label, Effect.NONE, null);
        }
    }

    record Outcome(RuntimeState state, Exception failure, Exception statusFailure, long readAt) {}

    record Snapshot(long id, Request request, boolean recovery, Outcome outcome) {
        boolean pending() { return outcome == null; }
    }

    @FunctionalInterface
    interface Task {
        RuntimeState run() throws Exception;
    }

    private final RootOperations workers;
    private final Executor ui;
    private final Set<Consumer<Snapshot>> listeners = new LinkedHashSet<>();
    private Snapshot current;
    private long handled;

    StudioOperations(RootOperations workers, Executor ui) {
        this.workers = workers;
        this.ui = ui;
    }

    // Entry points and observer delivery run on the UI executor.
    Snapshot latest() { return current; }

    void attach(Consumer<Snapshot> listener) {
        listeners.add(listener);
        if (current != null) listener.accept(current);
    }

    void detach(Consumer<Snapshot> listener) {
        listeners.remove(listener);
    }

    boolean claimCompletion(Snapshot snapshot) {
        if (snapshot != current || snapshot.pending() || snapshot.id() <= handled) return false;
        handled = snapshot.id();
        return true;
    }

    boolean start(Request request, boolean recovery, Task task, Task readStatus) {
        long id = current == null ? 1 : current.id() + 1;
        AtomicReference<Outcome> result = new AtomicReference<>();
        Runnable work = () -> result.set(execute(task, readStatus));
        Runnable finished = () -> ui.execute(() -> {
            if (current == null || current.id() != id) return;
            current = new Snapshot(id, request, recovery, result.get());
            notifyListeners();
        });
        boolean accepted = recovery ? workers.recover(work, finished) : workers.submit(work, finished);
        if (accepted) {
            current = new Snapshot(id, request, recovery, null);
            notifyListeners();
        }
        return accepted;
    }

    private void notifyListeners() {
        Snapshot snapshot = current;
        for (Consumer<Snapshot> listener : List.copyOf(listeners)) listener.accept(snapshot);
    }

    private static Outcome execute(Task task, Task readStatus) {
        try {
            return new Outcome(task.run(), null, null, System.currentTimeMillis());
        } catch (Exception failure) {
            if (failure instanceof InterruptedException) Thread.currentThread().interrupt();
            RuntimeState observed = null;
            Exception statusFailure = null;
            if (!Thread.currentThread().isInterrupted()) {
                try {
                    observed = readStatus.run();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                } catch (Exception error) {
                    statusFailure = error;
                }
            }
            return new Outcome(observed, failure, statusFailure, System.currentTimeMillis());
        }
    }
}
