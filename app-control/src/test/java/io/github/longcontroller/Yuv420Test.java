package io.github.longcontroller;

import io.github.longcontroller.media.Yuv420;
import org.junit.Test;
import java.nio.ByteBuffer;
import static org.junit.Assert.*;

public class Yuv420Test {
    private Yuv420.Plane plane(int row, int pixel, byte... bytes) {
        return new Yuv420.Plane(ByteBuffer.wrap(bytes), row, pixel);
    }
    @Test public void planarToNv21() {
        byte[] out = new byte[6];
        Yuv420.toNv21(plane(2,1,(byte)1,(byte)2,(byte)3,(byte)4), plane(1,1,(byte)10),
                plane(1,1,(byte)20),0,0,2,2,out);
        assertArrayEquals(new byte[]{1,2,3,4,20,10},out);
    }
    @Test public void supportsInterleavedChromaWithoutTrailingPadding() {
        byte[] out = new byte[12];
        Yuv420.toNv21(plane(6,1,(byte)1,(byte)2,(byte)3,(byte)4,(byte)99,(byte)99,
                        (byte)5,(byte)6,(byte)7,(byte)8),
                plane(4,2,(byte)10,(byte)99,(byte)11),plane(4,2,(byte)20,(byte)99,(byte)21),0,0,4,2,out);
        assertArrayEquals(new byte[]{1,2,3,4,5,6,7,8,20,10,21,11},out);
    }
    @Test public void supportsCropAndBufferPositionWithoutMutatingSource() {
        ByteBuffer y = ByteBuffer.wrap(new byte[]{99,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16});
        y.position(1);
        byte[] out = new byte[6];
        Yuv420.toNv21(new Yuv420.Plane(y,4,1),plane(2,1,(byte)20,(byte)21,(byte)22,(byte)23),
                plane(2,1,(byte)30,(byte)31,(byte)32,(byte)33),2,2,2,2,out);
        assertArrayEquals(new byte[]{11,12,15,16,33,23},out);
        assertEquals(1,y.position());
    }
    @Test public void rejectsOddLargeAndNegativeDimensions() {
        for (int[] size : new int[][]{{1,2},{2,1},{-2,2},{0,2},{Integer.MAX_VALUE,2},{1920,1920}}) {
            assertThrows(IllegalArgumentException.class, () -> Yuv420.byteCount(size[0],size[1]));
        }
    }
    @Test public void validatesAllPlanesBeforeWriting() {
        byte[] out = new byte[]{99,99,99,99,99,99};
        assertThrows(IllegalArgumentException.class, () -> Yuv420.toNv21(
                plane(2,1,(byte)1,(byte)2,(byte)3,(byte)4),plane(1,1,(byte)10),plane(1,1),0,0,2,2,out));
        assertArrayEquals(new byte[]{99,99,99,99,99,99},out);
    }
    @Test public void rejectsOddCropAndWrongDestination() {
        Yuv420.Plane p = plane(2,1,(byte)1,(byte)2,(byte)3,(byte)4);
        assertThrows(IllegalArgumentException.class, () -> Yuv420.toNv21(p,p,p,1,0,2,2,new byte[6]));
        assertThrows(IllegalArgumentException.class, () -> Yuv420.toNv21(p,p,p,0,0,2,2,new byte[5]));
    }
}
