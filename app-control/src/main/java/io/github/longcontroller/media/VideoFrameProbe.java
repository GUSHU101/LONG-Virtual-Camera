package io.github.longcontroller.media;

import android.graphics.ImageFormat;
import android.graphics.Rect;
import android.media.Image;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.os.SystemClock;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.concurrent.CancellationException;
import java.util.function.BooleanSupplier;
import java.util.zip.CRC32;

/** Single-thread, bounded diagnostic decoder; no background service or cross-app injection. */
public final class VideoFrameProbe {
    public record Result(String codec, int frames, int width, int height,
                         int nonIncreasingPts, long elapsedMs, long lastFrameCrc32, boolean eos) {}

    public static Result run(File file, BooleanSupplier cancelled) throws IOException {
        MediaExtractor extractor = new MediaExtractor();
        MediaCodec codec = null;
        long start = SystemClock.elapsedRealtime();
        int frames = 0, width = 0, height = 0, badPts = 0;
        long previousPts = Long.MIN_VALUE, checksum = 0;
        boolean inputEnded = false, outputEnded = false;
        byte[] nv21 = null;
        try {
            extractor.setDataSource(file.getAbsolutePath());
            MediaFormat format = null;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat candidate = extractor.getTrackFormat(i);
                if ("video/avc".equals(candidate.getString(MediaFormat.KEY_MIME))) {
                    format = candidate;
                    extractor.selectTrack(i);
                    break;
                }
            }
            if (format == null) throw new IOException("No H.264 video track");
            Yuv420.byteCount(format.getInteger(MediaFormat.KEY_WIDTH), format.getInteger(MediaFormat.KEY_HEIGHT));
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
            codec = MediaCodec.createDecoderByType("video/avc");
            String name = codec.getName();
            codec.configure(format, null, null, 0);
            codec.start();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            while (!outputEnded && frames < 90) {
                if (cancelled.getAsBoolean() || Thread.currentThread().isInterrupted()) throw new CancellationException();
                if (SystemClock.elapsedRealtime() - start > 10000) throw new IOException("Decode timeout");
                if (!inputEnded) {
                    int slot = codec.dequeueInputBuffer(10000);
                    if (slot >= 0) {
                        ByteBuffer input = codec.getInputBuffer(slot);
                        if (input == null) throw new IOException("No codec input buffer");
                        input.clear();
                        int size = extractor.readSampleData(input, 0);
                        if (size < 0) {
                            codec.queueInputBuffer(slot, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inputEnded = true;
                        } else {
                            if ((extractor.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_ENCRYPTED) != 0) {
                                throw new IOException("Encrypted samples are not supported");
                            }
                            codec.queueInputBuffer(slot, 0, size, extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }
                int slot = codec.dequeueOutputBuffer(info, 10000);
                if (slot >= 0) {
                    try {
                        if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                            try (Image image = codec.getOutputImage(slot)) {
                                if (image == null || image.getFormat() != ImageFormat.YUV_420_888) {
                                    throw new IOException("Codec does not expose YUV_420_888 images");
                                }
                                Rect crop = image.getCropRect();
                                width = crop.width(); height = crop.height();
                                int count = Yuv420.byteCount(width, height);
                                if (nv21 == null || nv21.length != count) nv21 = new byte[count];
                                Image.Plane[] planes = image.getPlanes();
                                if (planes.length != 3) throw new IOException("Expected three YUV planes");
                                Yuv420.toNv21(plane(planes[0]), plane(planes[1]), plane(planes[2]),
                                        crop.left, crop.top, width, height, nv21);
                                CRC32 crc = new CRC32(); crc.update(nv21); checksum = crc.getValue();
                                if (info.presentationTimeUs <= previousPts) badPts++;
                                previousPts = info.presentationTimeUs;
                                frames++;
                            }
                        }
                        outputEnded = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    } finally { codec.releaseOutputBuffer(slot, false); }
                }
            }
            if (frames == 0) throw new IOException("No decoded frame observed");
            return new Result(name, frames, width, height, badPts,
                    SystemClock.elapsedRealtime() - start, checksum, outputEnded);
        } finally {
            // Codec is used and released only on the worker that created it.
            try { if (codec != null) codec.release(); } finally { extractor.release(); }
        }
    }
    private static Yuv420.Plane plane(Image.Plane plane) {
        return new Yuv420.Plane(plane.getBuffer(), plane.getRowStride(), plane.getPixelStride());
    }
    private VideoFrameProbe() {}
}
