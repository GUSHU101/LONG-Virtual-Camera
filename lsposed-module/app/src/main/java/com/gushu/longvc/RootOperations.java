package com.gushu.longvc;

import java.util.concurrent.Executor;

/** Coordinates ordinary root commands and recovery across Activity instances. */
final class RootOperations {
    private final Executor ordinaryExecutor;
    private final Executor recoveryExecutor;
    private Task ordinary;
    private boolean recovering;

    private static final class Task {
        boolean cancelled;
        Thread runner;
    }

    RootOperations(Executor ordinaryExecutor, Executor recoveryExecutor) {
        this.ordinaryExecutor = ordinaryExecutor;
        this.recoveryExecutor = recoveryExecutor;
    }

    synchronized boolean submit(Runnable action) {
        if (ordinary != null || recovering) return false;
        Task task = new Task();
        ordinary = task;
        ordinaryExecutor.execute(() -> executeOrdinary(task, action));
        return true;
    }

    private void executeOrdinary(Task task, Runnable action) {
        synchronized (this) {
            if (task.cancelled) {
                ordinary = null;
                return;
            }
            task.runner = Thread.currentThread();
        }
        try {
            action.run();
        } finally {
            synchronized (this) {
                task.runner = null;
                ordinary = null;
            }
        }
    }

    synchronized boolean recover(Runnable action) {
        if (recovering) return false;
        recovering = true;
        if (ordinary != null) {
            ordinary.cancelled = true;
            if (ordinary.runner != null) ordinary.runner.interrupt();
        }
        recoveryExecutor.execute(() -> {
            try {
                action.run();
            } finally {
                synchronized (this) {
                    recovering = false;
                }
            }
        });
        return true;
    }
}
