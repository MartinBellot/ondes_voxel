import java.util.random.RandomGenerator;
import java.util.random.RandomGeneratorFactory;

// Reference vectors for Xoroshiro128++ from the JDK's own implementation.
//
// The JDK ships this generator as a standard algorithm, which makes it an
// oracle for the transition function and for the Stafford-13 seed mix — the two
// pieces Minecraft's XoroshiroRandomSource is built from. Minecraft differs
// only in which constant it XORs the seed with before mixing, so validating
// against the JDK pins everything except that one documented constant.
public class gen_xoroshiro_vectors {
    public static void main(String[] args) {
        RandomGeneratorFactory<RandomGenerator> f =
            RandomGeneratorFactory.of("Xoroshiro128PlusPlus");
        long[] seeds = {0L, 1L, -1L, 42L, 1234567890123L, -998877665544332211L};
        for (long seed : seeds) {
            RandomGenerator r = f.create(seed);
            System.out.printf("    {%dLL, {", seed);
            for (int i = 0; i < 8; i++) System.out.printf("%dLL,", r.nextLong());
            System.out.println("}},");
        }
    }
}
