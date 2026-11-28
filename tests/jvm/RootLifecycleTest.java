package com.gushu.longvc;

import java.io.IOException;
import java.util.ArrayDeque;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class RootLifecycleTest {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void cancelsQueuedWork() {
        ArrayDeque<Runnable> ordinary = new ArrayDeque<>();
        ArrayDeque<Runnable> recovery = new ArrayDeque<>();
        RootOperations tasks = new RootOperations(ordinary::add, recovery::add);
        AtomicInteger ran = new AtomicInteger();
        check(tasks.submit(() -> ran.addAndGet(10)), "first activity accepted");
        check(!tasks.submit(() -> ran.addAndGet(100)), "second activity must not replace pending work");
        check(tasks.recover(ran::incrementAndGet), "recovery accepted");
        check(!tasks.recover(ran::incrementAndGet), "duplicate recovery rejected");
        recovery.remove().run();
        check(!tasks.submit(ran::incrementAndGet), "cancelled task still owns its slot");
        ordinary.remove().run();
        check(ran.get() == 1, "obsolete queued work ran after recovery");
        check(tasks.submit(ran::incrementAndGet), "new action accepted after recovery");
        ordinary.remove().run();
        check(ran.get() == 2, "new action ran");
    }

    private static void interruptsRunningWork() throws Exception {
        ExecutorService ordinary = Executors.newSingleThreadExecutor();
        ArrayDeque<Runnable> recovery = new ArrayDeque<>();
        RootOperations tasks = new RootOperations(ordinary, recovery::add);
        CountDownLatch started = new CountDownLatch(1);
        CountDownLatch interrupted = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        try {
            check(tasks.submit(() -> {
                started.countDown();
                try {
                    release.await();
                } catch (InterruptedException expected) {
                    interrupted.countDown();
                    try {
                        release.await();
                    } catch (InterruptedException repeated) {
                        Thread.currentThread().interrupt();
                    }
                }
            }), "ordinary action accepted");
            check(started.await(2, TimeUnit.SECONDS), "ordinary action started");
            check(tasks.recover(() -> {}), "recovery can be scheduled during a task");
            check(interrupted.await(2, TimeUnit.SECONDS), "correct worker interrupted");
            recovery.remove().run();
            check(!tasks.submit(() -> {}), "must wait for cancelled task cleanup");
            release.countDown();
            ordinary.submit(() -> {}).get(2, TimeUnit.SECONDS);
            check(tasks.submit(() -> {}), "command accepted after cleanup");
        } finally {
            release.countDown();
            ordinary.shutdownNow();
        }
    }

    private static String snapshot(String result) {
        return "schema=1\nroot=1\nresult=" + result + "\n"
                + "camera_enabled=0\nphoto_enabled=0\nmic_enabled=0\n"
                + "camera_hook_ready=0\ncamera_runtime=stock\naudio_hook_ready=0\nvideo_feeder_ready=0\n"
                + "audio_runtime=stock\nfeeder_runtime=idle\nmode_markers=clear\n"
                + "audio_feeder_ready=0\nvideo_channel_ready=0\naudio_channel_ready=0\n"
                + "visual_source_ready=1\naudio_source_ready=0\nobs_pull_running=0\n";
    }

    private static void verifiesRecoveryResult() throws Exception {
        String stopped = snapshot("ok");
        check(RuntimeState.parseController(stopped, true, true).isStopped(), "verified stop");
        check(!RuntimeState.parseController(snapshot("degraded"), false, false).isStopped(),
                "unknown controller cannot prove stop");
        check(!RuntimeState.parseController(stopped.replace("camera_hook_ready=0",
                "camera_hook_ready=1"), true, true).isStopped(), "live hook cannot be ignored");
        for (String ambiguous : new String[]{stopped.replace("camera_runtime=stock\n", ""),
                stopped.replace("camera_runtime=stock", "camera_runtime=hook_mapped"),
                stopped.replace("camera_runtime=stock", "camera_runtime=unknown"),
                stopped.replace("audio_runtime=stock", "audio_runtime=hook_mapped"),
                stopped.replace("audio_runtime=stock\n", ""),
                stopped.replace("feeder_runtime=idle", "feeder_runtime=active"),
                stopped.replace("feeder_runtime=idle", "feeder_runtime=unknown"),
                stopped.replace("feeder_runtime=idle\n", ""),
                stopped.replace("mode_markers=clear", "mode_markers=present"),
                stopped.replace("mode_markers=clear\n", "")}) {
            check(!RuntimeState.parseController(ambiguous, true, true).isStopped(),
                    "missing or ambiguous runtime state must prevent recovery confirmation");
        }
        for (String damaged : new String[]{stopped.replace("camera_enabled=0\n", ""),
                stopped + "camera_enabled=1\n", stopped.replace("result=ok", "result=error")}) {
            try {
                RuntimeState.parseController(damaged, true, true);
                throw new AssertionError("accepted an incomplete or ambiguous status");
            } catch (IOException expected) {
                // Failures must surface instead of becoming default OFF values.
            }
        }
        RootShell.Result action = new RootShell.Result(0,
                "warning: result=ok\naction=stop-all\n", false, false, null);
        check(!action.hasLine("result=ok"), "diagnostic text cannot acknowledge success");
        RootShell.Result failure = new RootShell.Result(1,
                "playback rtmp://relay.local/live/path-key?token=query-secret\n"
                        + "read content://provider/my-private-video",
                false, false, null);
        String diagnostic = failure.describe();
        check(diagnostic.contains("playback") && diagnostic.contains("read"),
                "retain diagnostic context");
        check(!diagnostic.contains("relay.local") && !diagnostic.contains("path-key")
                        && !diagnostic.contains("query-secret") && !diagnostic.contains("my-private-video"),
                "diagnostics must omit stream and content addresses");
        String cameraFailure = "NormalPerm(policy log)\ncr_injector: FAIL rc=17\n"
                + "result=error\nmessage=camera_arm_failed\n";
        RootShell.Result failedCamera = new RootShell.Result(1, cameraFailure, false, false, null);
        check(failedCamera.reportsFailure("camera_arm_failed", "cr_injector: FAIL rc=17"),
                "recognize the exact camera initialization failure despite policy log noise");
        check(!failedCamera.reportsFailure("photo_arm_failed", "cr_injector: FAIL rc=17"),
                "do not attribute camera failures to the photo operation");
        check(!new RootShell.Result(1, cameraFailure, false, true, null)
                        .reportsFailure("camera_arm_failed", "cr_injector: FAIL rc=17"),
                "truncated command output cannot diagnose a structured failure");
        Thread.currentThread().interrupt();
        try {
            RootShell.checkCancellation();
            throw new AssertionError("cancelled task can still launch a command");
        } catch (InterruptedException expected) {
            Thread.interrupted();
        }
    }

    private static void checksCameraOutputStats() throws Exception {
        String raw = snapshot("ok").replace("camera_hook_ready=0", "camera_hook_ready=1");
        check(!RuntimeState.parseController(raw, true, true).hasCameraOutputStats(),
                "a mapped hook alone cannot verify writes");
        String counters = "camera_written_yuv=59\ncamera_written_jpeg=0\n"
                + "camera_raw_ten=60\ncamera_unsupported=60\n";
        RuntimeState state = RuntimeState.parseController(raw + counters, true, true);
        check(state.hasCameraOutputStats() && state.cameraWrittenYuv == 59
                && state.cameraWrittenJpeg == 0 && state.cameraRawTen == 60,
                "mixed YUV and RAW streams must remain distinguishable");
        check(!state.hasCameraQueueStats(), "old write counters do not confirm queue submission");
        RuntimeState queued = RuntimeState.parseController(raw + counters
                + "camera_queued_yuv=57\ncamera_queued_jpeg=0\ncamera_queue_errors=2\n",
                true, true);
        check(queued.hasCameraQueueStats() && queued.cameraQueuedYuv == 57
                && queued.cameraQueueErrors == 2, "queue failures must remain distinct from writes");
        RuntimeState preview = RuntimeState.parseController(raw
                + "camera_preview_seen=90\ncamera_preview_staged=88\ncamera_preview_errors=1\n",
                true, true);
        check(preview.hasCameraPreviewStats() && preview.cameraPreviewSeen == 90
                && preview.cameraPreviewStaged == 88 && preview.cameraPreviewErrors == 1
                && !preview.hasCameraQueueStats(),
                "deferred preview staging has its own acknowledgement");
        check(!RuntimeState.parseController(raw + counters.replace("=60\n", "=-1\n"),
                true, true).hasCameraOutputStats(), "invalid counters must be absent");
    }

    public static void main(String[] args) throws Exception {
        cancelsQueuedWork();
        interruptsRunningWork();
        verifiesRecoveryResult();
        checksCameraOutputStats();
        System.out.println("Root lifecycle checks passed");
    }
}
