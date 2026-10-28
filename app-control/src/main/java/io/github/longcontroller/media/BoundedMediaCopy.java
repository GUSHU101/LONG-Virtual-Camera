package io.github.longcontroller.media;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.util.function.BooleanSupplier;

/** Does not own either stream. Caller must discard partial output on failure. */
public final class BoundedMediaCopy {
    public static long copy(InputStream input, OutputStream output, MessageDigest digest,
                            long maximum, BooleanSupplier cancelled) throws IOException {
        if (maximum < 1) throw new IllegalArgumentException("Positive limit required");
        byte[] buffer = new byte[64 * 1024];
        long total = 0;
        while (true) {
            if (cancelled.getAsBoolean()) throw new IOException("Copy cancelled");
            int count = input.read(buffer);
            if (count < 0) break;
            if (count == 0) {
                int single = input.read();
                if (single < 0) break;
                buffer[0] = (byte) single;
                count = 1;
            }
            if (cancelled.getAsBoolean()) throw new IOException("Copy cancelled");
            if (count > maximum - total) throw new IOException("Media exceeds import limit");
            output.write(buffer, 0, count);
            digest.update(buffer, 0, count);
            total += count;
        }
        if (cancelled.getAsBoolean()) throw new IOException("Copy cancelled");
        if (total == 0) throw new IOException("Empty media");
        return total;
    }
    private BoundedMediaCopy() {}
}
