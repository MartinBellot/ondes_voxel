import java.util.Random;

// Reference vectors from the JDK's own java.util.Random, which is what
// Minecraft's LegacyRandomSource is. Printed as C++ so there is no chance of a
// transcription error between the oracle and the test.
public class Vectors {
    public static void main(String[] args) {
        long[] seeds = {0L, 1L, -1L, 42L, 1234567890123L, 0x5DEECE66DL, -998877665544332211L};
        for (long seed : seeds) {
            Random r = new Random(seed);
            System.out.printf("    {%dLL, {", seed);
            for (int i = 0; i < 8; i++) System.out.printf("%d,", r.nextInt());
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("%dLL,", r.nextLong());
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("%d,", r.nextInt(100));
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("%d,", r.nextInt(16));  // power of two
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("0x%08x,", Float.floatToRawIntBits(r.nextFloat()));
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("0x%016xULL,", Double.doubleToRawLongBits(r.nextDouble()));
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("%s,", r.nextBoolean());
            System.out.print("}, {");
            r = new Random(seed);
            for (int i = 0; i < 8; i++) System.out.printf("0x%016xULL,", Double.doubleToRawLongBits(r.nextGaussian()));
            System.out.println("}},");
        }
    }
}
