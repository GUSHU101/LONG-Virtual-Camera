package com.gushu.longvc;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.regex.Pattern;

final class RootShell {
    private static final int MAX_CAPTURE_BYTES = 128 * 1024;
    private static final Pattern DIAGNOSTIC_ADDRESS =
            Pattern.compile("(?i)\\b(?:rtmps?|https?|content|file)://\\S+");
    static final String TRUSTED_STORAGE = "trusted_path() { "
            + "[ ! -L \"$1\" ] || return 1; "
            + "case \"$(stat -c '%u:%a' \"$1\" 2>/dev/null)\" in "
            + "0:[0-7][0145][0145]) return 0 ;; *) return 1 ;; esac; }; "
            + "trusted_dir() { [ -d \"$1\" ] && trusted_path \"$1\"; }; "
            + "ensure_trusted_dir() { "
            + "if [ ! -e \"$1\" ] && [ ! -L \"$1\" ]; then "
            + "mkdir \"$1\" 2>/dev/null || trusted_dir \"$1\" || return 1; fi; "
            + "trusted_dir \"$1\" && chmod \"$2\" \"$1\"; }; ";

    static final class Result {
        final int exitCode;
        final String output;
        final boolean timedOut;
        final boolean outputTruncated;
        final IOException streamError;

        Result(int exitCode, String output, boolean timedOut,
               boolean outputTruncated, IOException streamError) {
            this.exitCode = exitCode;
            this.output = output;
            this.timedOut = timedOut;
            this.outputTruncated = outputTruncated;
            this.streamError = streamError;
        }

        boolean ok() {
            return exitCode == 0 && !timedOut && !outputTruncated
                    && streamError == null;
        }

        boolean hasLine(String expected) {
            for (String line : output.split("\\R")) {
                if (expected.equals(line.trim())) return true;
            }
            return false;
        }

        String describe() {
            if (timedOut) return "root command timed out";
            if (streamError != null) return "root stream failed: " + redactDiagnostic(streamError.getMessage());
            if (outputTruncated) return "root output exceeded the safety limit";
            String trimmed = redactDiagnostic(output).trim();
            if (trimmed.length() > 1200) trimmed = trimmed.substring(0, 1200) + "…";
            return "exit=" + exitCode + (trimmed.isEmpty() ? "" : ", " + trimmed);
        }
    }

    static String redactDiagnostic(String message) {
        return message == null ? "" : DIAGNOSTIC_ADDRESS.matcher(message).replaceAll("[地址已隐藏]");
    }

    Result execute(String command, long timeoutSeconds) throws IOException, InterruptedException {
        return run(command, null, timeoutSeconds);
    }

    // Only for removing this operation's private staging files. Preserve the
    // cancellation before returning so ordinary root actions still abort.
    Result executeStageCleanup(String command, long timeoutSeconds)
            throws IOException, InterruptedException {
        boolean cancelled = Thread.interrupted();
        try {
            return execute(command, timeoutSeconds);
        } finally {
            if (cancelled) Thread.currentThread().interrupt();
        }
    }

    Result stream(String command, InputStream input, long timeoutSeconds)
            throws IOException, InterruptedException {
        if (input == null) throw new IllegalArgumentException("input == null");
        return run(command, input, timeoutSeconds);
    }

    private Result run(String command, InputStream input, long timeoutSeconds)
            throws IOException, InterruptedException {
        if (command == null || command.isEmpty()) {
            throw new IllegalArgumentException("empty root command");
        }
        if (timeoutSeconds < 1L || timeoutSeconds > 1_800L) {
            throw new IllegalArgumentException("invalid root timeout");
        }
        checkCancellation();
        Process process = new ProcessBuilder("su", "-c", command)
                .redirectErrorStream(true)
                .start();

        Capture capture = new Capture(process.getInputStream());
        Thread reader = new Thread(capture, "longvc-root-reader");
        reader.setDaemon(true);
        reader.start();

        AtomicReference<IOException> writerError = new AtomicReference<>();
        Thread writer = new Thread(() -> {
            try (OutputStream out = process.getOutputStream()) {
                if (input != null) {
                    byte[] buffer = new byte[64 * 1024];
                    int read;
                    while ((read = input.read(buffer)) >= 0) {
                        if (Thread.currentThread().isInterrupted()) {
                            throw new IOException("root input copy interrupted");
                        }
                        if (read > 0) out.write(buffer, 0, read);
                    }
                    out.flush();
                }
            } catch (IOException e) {
                writerError.set(e);
            }
        }, "longvc-root-writer");
        writer.setDaemon(true);
        writer.start();

        boolean finished;
        try {
            finished = process.waitFor(timeoutSeconds, TimeUnit.SECONDS);
            if (!finished) terminate(process, input, reader, writer);
            writer.join(3_000L);
            reader.join(3_000L);
            if (finished && (writer.isAlive() || reader.isAlive())) {
                terminate(process, input, reader, writer);
                writer.join(1_000L);
                reader.join(1_000L);
            }
        } catch (InterruptedException e) {
            terminate(process, input, reader, writer);
            Thread.currentThread().interrupt();
            throw e;
        }

        int exit = finished && !process.isAlive() ? process.exitValue() : -1;
        IOException error = writerError.get();
        if (error == null) error = capture.error.get();
        if (error == null && (writer.isAlive() || reader.isAlive())) {
            error = new IOException("root I/O workers did not terminate");
        }
        return new Result(exit, capture.text(), !finished, capture.truncated, error);
    }

    static void checkCancellation() throws InterruptedException {
        if (Thread.currentThread().isInterrupted()) {
            throw new InterruptedException("root operation cancelled");
        }
    }

    private static void terminate(Process process, InputStream source,
                                  Thread reader, Thread writer) {
        writer.interrupt();
        reader.interrupt();
        process.destroy();
        try {
            if (!process.waitFor(500L, TimeUnit.MILLISECONDS)) {
                process.destroyForcibly();
                process.waitFor(2L, TimeUnit.SECONDS);
            }
        } catch (InterruptedException interrupted) {
            process.destroyForcibly();
            Thread.currentThread().interrupt();
        }
        // A document provider can block in close(). Terminate the process
        // first, then release the provider stream on a background thread.
        if (source != null) {
            Thread closer = new Thread(() -> closeQuietly(source), "longvc-root-input-close");
            closer.setDaemon(true);
            closer.start();
        }
    }

    private static void closeQuietly(java.io.Closeable closeable) {
        if (closeable == null) return;
        try {
            closeable.close();
        } catch (IOException ignored) {
            // Termination is already a failure path.
        }
    }

    private static final class Capture implements Runnable {
        private final InputStream input;
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        private final AtomicReference<IOException> error = new AtomicReference<>();
        private volatile boolean truncated;

        Capture(InputStream input) {
            this.input = input;
        }

        @Override
        public void run() {
            try (InputStream in = input) {
                byte[] buffer = new byte[8 * 1024];
                int read;
                while ((read = in.read(buffer)) >= 0) {
                    if (read == 0) continue;
                    int remaining = MAX_CAPTURE_BYTES - bytes.size();
                    if (remaining > 0) bytes.write(buffer, 0, Math.min(read, remaining));
                    if (read > remaining) truncated = true;
                }
            } catch (IOException e) {
                error.set(e);
            }
        }

        String text() {
            String value = new String(bytes.toByteArray(), StandardCharsets.UTF_8);
            return truncated ? value + "\n[output truncated]" : value;
        }
    }
}
