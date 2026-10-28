package io.github.longcontroller.media;

import java.nio.ByteBuffer;
import java.util.Objects;

/** Stride-aware, bounded 8-bit YUV_420_888 to NV21 conversion. No Android dependency. */
public final class Yuv420 {
    public record Plane(ByteBuffer data, int rowStride, int pixelStride) {
        public Plane {
            Objects.requireNonNull(data);
            if (rowStride <= 0 || pixelStride <= 0) throw new IllegalArgumentException("Invalid stride");
            data = data.asReadOnlyBuffer();
        }
    }
    public static int byteCount(int width, int height) {
        if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0
                || width > 1920 || height > 1920 || (long) width * height > 1920L * 1080) {
            throw new IllegalArgumentException("Only even dimensions up to 1080p pixel area are supported");
        }
        return width * height * 3 / 2;
    }
    public static void toNv21(Plane y, Plane u, Plane v, int left, int top,
                             int width, int height, byte[] destination) {
        int bytes = byteCount(width, height);
        if (left < 0 || top < 0 || (left & 1) != 0 || (top & 1) != 0) {
            throw new IllegalArgumentException("Crop origin must be even and nonnegative");
        }
        if (destination == null || destination.length != bytes) throw new IllegalArgumentException("NV21 buffer size mismatch");
        validate(y, left, top, width, height);
        validate(u, left / 2, top / 2, width / 2, height / 2);
        validate(v, left / 2, top / 2, width / 2, height / 2);
        copy(y, left, top, width, height, destination, 0, 1);
        copy(v, left / 2, top / 2, width / 2, height / 2, destination, width * height, 2);
        copy(u, left / 2, top / 2, width / 2, height / 2, destination, width * height + 1, 2);
    }
    private static void validate(Plane plane, int left, int top, int width, int height) {
        long rowEnd = ((long) left + width - 1) * plane.pixelStride() + 1;
        long end = plane.data().position() + ((long) top + height - 1) * plane.rowStride() + rowEnd;
        if (rowEnd > plane.rowStride() || end > plane.data().limit()) {
            throw new IllegalArgumentException("Truncated or inconsistent YUV plane");
        }
    }
    private static void copy(Plane p, int left, int top, int width, int height,
                             byte[] output, int offset, int step) {
        int base = p.data().position();
        for (int row = 0; row < height; row++) {
            int start = base + (top + row) * p.rowStride() + left * p.pixelStride();
            for (int col = 0; col < width; col++) {
                output[offset] = p.data().get(start + col * p.pixelStride());
                offset += step;
            }
        }
    }
    private Yuv420() {}
}
