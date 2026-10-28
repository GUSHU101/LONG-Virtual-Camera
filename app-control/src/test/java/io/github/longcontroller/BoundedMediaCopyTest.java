package io.github.longcontroller;

import io.github.longcontroller.media.BoundedMediaCopy;
import org.junit.Test;
import java.io.*;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicBoolean;
import static org.junit.Assert.*;

public class BoundedMediaCopyTest {
    private MessageDigest digest() throws Exception { return MessageDigest.getInstance("SHA-256"); }
    @Test public void copiesExactlyLimitAndHashes() throws Exception {
        byte[] input = new byte[]{1,2,3,4};
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        MessageDigest hash = digest();
        assertEquals(4, BoundedMediaCopy.copy(new ByteArrayInputStream(input), output, hash,4,() -> false));
        assertArrayEquals(input,output.toByteArray());
        assertArrayEquals(digest().digest(input),hash.digest());
    }
    @Test public void rejectsOversizeBeforeWritingChunk() throws Exception {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        assertThrows(IOException.class, () -> BoundedMediaCopy.copy(new ByteArrayInputStream(new byte[5]),output,digest(),4,() -> false));
        assertEquals(0,output.size());
    }
    @Test public void rejectsEmpty() {
        assertThrows(IOException.class, () -> BoundedMediaCopy.copy(new ByteArrayInputStream(new byte[0]),new ByteArrayOutputStream(),digest(),4,() -> false));
    }
    @Test public void cancellationBeforeReading() {
        InputStream input = new InputStream() { @Override public int read() { fail("Must not read cancelled import"); return -1; } };
        assertThrows(IOException.class, () -> BoundedMediaCopy.copy(input,new ByteArrayOutputStream(),digest(),4,() -> true));
    }
    @Test public void cancellationDuringReadDoesNotCommitBytes() throws Exception {
        AtomicBoolean cancelled = new AtomicBoolean();
        InputStream input = new ByteArrayInputStream(new byte[]{1}) {
            @Override public int read(byte[] buffer, int offset, int length) {
                cancelled.set(true); return super.read(buffer,offset,length);
            }
        };
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        assertThrows(IOException.class, () -> BoundedMediaCopy.copy(input,output,digest(),4,cancelled::get));
        assertEquals(0,output.size());
    }
    @Test public void makesProgressWhenBulkReadReturnsZero() throws Exception {
        InputStream input = new ByteArrayInputStream(new byte[]{1,2}) {
            @Override public int read(byte[] b,int offset,int length) { return available() > 0 ? 0 : -1; }
        };
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        assertEquals(2,BoundedMediaCopy.copy(input,output,digest(),2,() -> false));
        assertArrayEquals(new byte[]{1,2},output.toByteArray());
    }
}
