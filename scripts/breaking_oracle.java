// Ondes VOXEL — breaking a block, asked of the real 1.20.1 client.
//
// Three things about breaking live only in the client, and the client can be
// run. This class starts the user's own vanilla 1.20.1 client (the
// PrismLauncher instance), lets it join a vanilla server through a recording
// proxy (scripts/measure_breaking.py), and then:
//
//   1. **holds the attack button** — a synthetic GLFW press injected into the
//      game's own MouseHandler, released after a set time — on columns of
//      blocks posed in front of the player, tool by tool. The proxy records
//      every Player Action and Swing Arm the client sends, and the tick each
//      went on is what the dig schedule is measured by;
//   2. **poses the cracks**: ClientLevel.destroyBlockProgress, the very call
//      the game makes when Set Block Destroy Stage arrives, at stages 0, 4 and
//      9 on stone, planks, glass and a slab, each captured through the game's
//      own screenshot path with the GUI hidden;
//   3. **reads the particles**: ClientLevel.addDestroyBlockEffect and
//      ParticleEngine.crack, the two calls a break and a hit make, and then the
//      fields of every particle they made — at birth, and every 50 ms after.
//
// Nothing here reads or translates Mojang code. The official mappings are used
// only to *name* classes, fields and methods (CLAUDE.md § 1), as in
// scripts/creative_screen_oracle.java, and every number printed is produced by
// the game's own bytecode at run time.
//
//   java -XstartOnFirstThread -cp <out>:<client classpath> ov.BreakingOracle \
//        <client mappings> <out dir> <Minecraft arguments...>
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class BreakingOracle {

    // ── The mappings, as three lookup tables (the creative oracle's reader) ──

    static final Map<String, String> CLASSES = new HashMap<>();
    static final Map<String, Map<String, String>> FIELDS = new HashMap<>();
    static final Map<String, Map<String, List<String>>> METHODS = new HashMap<>();

    static void loadMappings(Path file) throws IOException {
        String current = null;
        try (BufferedReader r = Files.newBufferedReader(file, StandardCharsets.UTF_8)) {
            String line;
            while ((line = r.readLine()) != null) {
                if (line.startsWith("#")) continue;
                if (!line.startsWith("    ")) {
                    int arrow = line.indexOf(" -> ");
                    if (arrow < 0) { current = null; continue; }
                    current = line.substring(0, arrow);
                    CLASSES.put(current, line.substring(arrow + 4, line.length() - 1));
                } else if (current != null) {
                    String body = line.trim();
                    int arrow = body.lastIndexOf(" -> ");
                    if (arrow < 0) continue;
                    String obf = body.substring(arrow + 4);
                    String decl = body.substring(0, arrow);
                    int colon = decl.indexOf(':');
                    if (colon >= 0) {
                        int second = decl.indexOf(':', colon + 1);
                        if (second >= 0) decl = decl.substring(second + 1);
                    }
                    int paren = decl.indexOf('(');
                    if (paren < 0) {
                        int sp = decl.lastIndexOf(' ');
                        if (sp < 0) continue;
                        FIELDS.computeIfAbsent(current, k -> new HashMap<>())
                              .put(decl.substring(sp + 1), obf);
                    } else {
                        String head = decl.substring(0, paren);
                        int sp = head.lastIndexOf(' ');
                        if (sp < 0) continue;
                        String args = decl.substring(paren + 1, decl.lastIndexOf(')'));
                        int n = args.isEmpty() ? 0 : args.split(",").length;
                        METHODS.computeIfAbsent(current, k -> new HashMap<>())
                               .computeIfAbsent(head.substring(sp + 1) + "/" + n,
                                                k -> new ArrayList<>())
                               .add(obf + " " + args);
                    }
                }
            }
        }
    }

    static Class<?> cls(String official) throws ClassNotFoundException {
        String obf = CLASSES.get(official);
        if (obf == null) throw new ClassNotFoundException("no mapping for " + official);
        return Class.forName(obf);
    }

    static String obfOf(String officialType) {
        int dim = 0;
        while (officialType.endsWith("[]")) {
            officialType = officialType.substring(0, officialType.length() - 2);
            dim++;
        }
        return CLASSES.getOrDefault(officialType, officialType) + "[]".repeat(dim);
    }

    /// A field by its official name, looked for on the class and its parents.
    static Field field(String official, String name) throws Exception {
        String obf = FIELDS.getOrDefault(official, Map.of()).get(name);
        if (obf == null) throw new NoSuchFieldException(official + "." + name);
        Field f = cls(official).getDeclaredField(obf);
        f.setAccessible(true);
        return f;
    }

    static Object get(Object target, String official, String name) throws Exception {
        return field(official, name).get(target);
    }

    static void set(Object target, String official, String name, Object value) throws Exception {
        field(official, name).set(target, value);
    }

    static Method method(String official, String name, String... argTypes) throws Exception {
        List<String> entries = METHODS.getOrDefault(official, Map.of())
                                      .get(name + "/" + argTypes.length);
        if (entries == null) throw new NoSuchMethodException(official + "." + name);
        for (String entry : entries) {
            int sp = entry.indexOf(' ');
            String obf = entry.substring(0, sp);
            String[] declared = entry.length() == sp + 1 ? new String[0]
                                                         : entry.substring(sp + 1).split(",");
            if (!Arrays.equals(declared, argTypes)) continue;
            for (Method m : cls(official).getDeclaredMethods()) {
                if (!m.getName().equals(obf) || m.getParameterCount() != argTypes.length) continue;
                boolean ok = true;
                for (int i = 0; i < argTypes.length && ok; i++) {
                    ok = m.getParameterTypes()[i].getName().equals(obfOf(argTypes[i]));
                }
                if (ok) {
                    m.setAccessible(true);
                    return m;
                }
            }
        }
        throw new NoSuchMethodException(official + "." + name + Arrays.toString(argTypes));
    }

    static Object call(Object target, String official, String name, String[] types,
                       Object... args) throws Exception {
        return method(official, name, types).invoke(target, args);
    }

    static final String[] NONE = new String[0];

    // ── Names ───────────────────────────────────────────────────────────────

    static final String MC = "net.minecraft.client.Minecraft";
    static final String CLIENT_LEVEL = "net.minecraft.client.multiplayer.ClientLevel";
    static final String LISTENER = "net.minecraft.client.multiplayer.ClientPacketListener";
    static final String ENGINE = "net.minecraft.client.particle.ParticleEngine";
    static final String PARTICLE = "net.minecraft.client.particle.Particle";
    static final String QUAD = "net.minecraft.client.particle.SingleQuadParticle";
    static final String TERRAIN = "net.minecraft.client.particle.TerrainParticle";
    static final String BLOCK_POS = "net.minecraft.core.BlockPos";
    static final String DIRECTION = "net.minecraft.core.Direction";
    static final String BLOCKS = "net.minecraft.world.level.block.Blocks";
    static final String BLOCK = "net.minecraft.world.level.block.Block";
    static final String STATE = "net.minecraft.world.level.block.state.BlockState";
    static final String OPTIONS = "net.minecraft.client.Options";
    static final String WINDOW = "com.mojang.blaze3d.platform.Window";

    static Object mc;
    static long win;
    static PrintStream out;
    static File gameDir;

    static <T> T onRender(Callable<T> c) throws Exception {
        CompletableFuture<T> f = new CompletableFuture<>();
        ((Executor) mc).execute(() -> {
            try {
                f.complete(c.call());
            } catch (Throwable e) {
                f.completeExceptionally(e);
            }
        });
        return f.get(30, TimeUnit.SECONDS);
    }

    static void pause(long ms) throws InterruptedException { Thread.sleep(ms); }

    // ── Gestures ────────────────────────────────────────────────────────────

    static void command(String text) throws Exception {
        onRender(() -> {
            Object connection = call(mc, MC, "getConnection", NONE);
            call(connection, LISTENER, "sendCommand", new String[]{"java.lang.String"}, text);
            return null;
        });
        pause(200);
    }

    /// A label the proxy sees go by, so each gesture's packets can be told apart.
    static void mark(String label) throws Exception {
        command("say ovmark " + label);
    }

    static void key(int glfwKey) throws Exception {
        onRender(() -> {
            Object kb = get(mc, MC, "keyboardHandler");
            Method press = method("net.minecraft.client.KeyboardHandler", "keyPress",
                                  "long", "int", "int", "int", "int");
            press.invoke(kb, win, glfwKey, 0, 1, 0);
            press.invoke(kb, win, glfwKey, 0, 0, 0);
            return null;
        });
        pause(300);
    }

    /// Left mouse down (1) or up (0), through the game's own handler.
    static void mouse(int action) throws Exception {
        onRender(() -> {
            Object mh = get(mc, MC, "mouseHandler");
            method("net.minecraft.client.MouseHandler", "onPress", "long", "int", "int", "int")
                .invoke(mh, win, 0, action, 0);
            return null;
        });
    }

    static void shot(String name) throws Exception {
        pause(500);
        onRender(() -> {
            Object target = call(mc, MC, "getMainRenderTarget", NONE);
            java.util.function.Consumer<Object> sink = c -> {};
            method("net.minecraft.client.Screenshot", "grab", "java.io.File", "java.lang.String",
                   "com.mojang.blaze3d.pipeline.RenderTarget", "java.util.function.Consumer")
                .invoke(null, gameDir, name, target, sink);
            return null;
        });
        out.println("shot " + name);
        pause(300);
    }

    static Object blockPos(int x, int y, int z) throws Exception {
        return cls(BLOCK_POS).getConstructor(int.class, int.class, int.class).newInstance(x, y, z);
    }

    static Object defaultState(String blockField) throws Exception {
        Object block = get(null, BLOCKS, blockField);
        return call(block, BLOCK, "defaultBlockState", NONE);
    }

    // ── 1. The dig schedule ─────────────────────────────────────────────────

    /// A column of `count` blocks at eye height straight south of the player,
    /// the attack held for `holdMs`.
    static void scenario(String label, String block, String tool, String effect,
                         boolean creative, int count, long holdMs) throws Exception {
        command("gamemode " + (creative ? "creative" : "survival"));
        command("effect clear @s");
        if (effect != null) command("effect give @s " + effect + " true");
        command("clear @s");
        if (tool != null) command("item replace entity @s hotbar.0 with " + tool);
        key(49);  // "1": the first hotbar slot
        command("fill 0 -59 1 0 -59 9 minecraft:air");
        for (int i = 0; i < count; i++) command("setblock 0 -59 " + (2 + i) + " " + block);
        command("tp @s 0.5 -60 0.5 0 0");
        pause(1500);
        mark(label + ".start");
        pause(300);
        mouse(1);
        pause(holdMs);
        mouse(0);
        pause(600);
        mark(label + ".end");
        out.println("scenario " + label + " " + block + " tool " + tool + " effect " + effect
                    + " creative " + creative + " held " + holdMs + " ms");
    }

    // ── 2. The cracks ───────────────────────────────────────────────────────

    static void cracks() throws Exception {
        command("gamemode creative");
        command("effect clear @s");
        command("clear @s");
        command("fill -3 -60 -3 3 -57 8 minecraft:air");
        Object options = get(mc, MC, "options");
        onRender(() -> { set(options, OPTIONS, "hideGui", true); return null; });
        String[][] blocks = {{"stone", "minecraft:stone"}, {"planks", "minecraft:oak_planks"},
                             {"glass", "minecraft:glass"},
                             {"slab", "minecraft:stone_slab[type=bottom]"}};
        Object level = get(mc, MC, "level");
        Object pos = blockPos(0, -60, 2);
        Method progress = method(CLIENT_LEVEL, "destroyBlockProgress", "int", BLOCK_POS, "int");
        for (String[] b : blocks) {
            command("setblock 0 -60 2 " + b[1]);
            command("tp @s 0.5 -60 0.5 0 30");
            pause(2500);
            for (int stage : new int[]{-1, 0, 4, 9}) {
                onRender(() -> { progress.invoke(level, 7777, pos, stage); return null; });
                shot("crack-" + b[0] + "-" + (stage < 0 ? "none" : String.valueOf(stage)) + ".png");
            }
            onRender(() -> { progress.invoke(level, 7777, pos, -1); return null; });
        }
        onRender(() -> { set(options, OPTIONS, "hideGui", false); return null; });
    }

    // ── 3. The particles ────────────────────────────────────────────────────

    static String describe(Object p) throws Exception {
        StringBuilder b = new StringBuilder();
        for (String f : new String[]{"x", "y", "z", "xd", "yd", "zd"}) {
            b.append(',').append(get(p, PARTICLE, f));
        }
        for (String f : new String[]{"lifetime", "age", "gravity", "friction", "bbWidth",
                                     "rCol", "gCol", "bCol", "hasPhysics", "onGround"}) {
            b.append(',').append(get(p, PARTICLE, f));
        }
        b.append(',').append(get(p, QUAD, "quadSize"));
        return b.toString();
    }

    /// Every terrain particle the engine holds, those not yet ticked included.
    static List<Object> terrainParticles(Object engine) throws Exception {
        List<Object> r = new ArrayList<>();
        Class<?> terrain = cls(TERRAIN);
        for (Object queue : ((Map<?, ?>) get(engine, ENGINE, "particles")).values()) {
            for (Object p : (Collection<?>) queue) if (terrain.isInstance(p)) r.add(p);
        }
        for (Object p : (Collection<?>) get(engine, ENGINE, "particlesToAdd")) {
            if (terrain.isInstance(p)) r.add(p);
        }
        return r;
    }

    static void particles() throws Exception {
        command("gamemode creative");
        command("fill -3 -60 -3 3 -57 8 minecraft:air");
        command("setblock 0 -60 6 minecraft:stone");
        command("tp @s 0.5 -60 0.5 0 20");
        pause(2500);
        Object level = get(mc, MC, "level");
        Object engine = get(mc, MC, "particleEngine");
        Object stone = defaultState("STONE");
        Object air = blockPos(0, -59, 3);
        Method burst = method(CLIENT_LEVEL, "addDestroyBlockEffect", BLOCK_POS, STATE);
        out.println("particle-fields x,y,z,xd,yd,zd,lifetime,age,gravity,friction,bbWidth,"
                    + "rCol,gCol,bCol,hasPhysics,onGround,quadSize");
        for (int trial = 0; trial < 6; trial++) {
            final int t = trial;
            pause(3000);  // the last trial's are gone: the longest lives 40 ticks
            List<Object> born = onRender(() -> {
                burst.invoke(level, air, stone);
                return terrainParticles(engine);
            });
            for (Object p : born) out.println("P,destroy," + t + describe(p));
            // Follow them: how many are left, 50 ms apart.
            Set<Integer> ids = new HashSet<>();
            for (Object p : born) ids.add(System.identityHashCode(p));
            for (int ms = 0; ms <= 2500; ms += 50) {
                final int stamp = ms;
                onRender(() -> {
                    int alive = 0;
                    StringBuilder ages = new StringBuilder();
                    for (Object p : terrainParticles(engine)) {
                        if (!ids.contains(System.identityHashCode(p))) continue;
                        alive++;
                        if (ages.length() < 4000) ages.append(' ').append(get(p, PARTICLE, "age"))
                                                      .append('/').append(get(p, PARTICLE, "y"));
                    }
                    out.println("S," + t + "," + stamp + "," + alive + "," + ages);
                    return null;
                });
                pause(50);
            }
        }
        // Hits: one crack particle a call, on the up face of the stone.
        Object up = get(null, DIRECTION, "UP");
        Object north = get(null, DIRECTION, "NORTH");
        Object stonePos = blockPos(0, -60, 6);
        Method crack = method(ENGINE, "crack", BLOCK_POS, DIRECTION);
        pause(3000);
        for (Object face : new Object[]{up, north}) {
            List<Object> made = onRender(() -> {
                for (int i = 0; i < 64; i++) crack.invoke(engine, stonePos, face);
                return terrainParticles(engine);
            });
            for (Object p : made) out.println("P,crack-" + face + ",0" + describe(p));
            pause(3000);
        }
    }

    // ── The run ─────────────────────────────────────────────────────────────

    static void drive(String only) throws Exception {
        while (true) {
            Method getInstance = null;
            try {
                getInstance = method(MC, "getInstance");
            } catch (Throwable ignored) {
            }
            if (getInstance != null) {
                mc = getInstance.invoke(null);
                if (mc != null && get(mc, MC, "player") != null) break;
            }
            pause(500);
        }
        pause(10000);  // the chunks, and the operator status the driver grants
        gameDir = (File) get(mc, MC, "gameDirectory");
        win = onRender(() -> (Long) call(call(mc, MC, "getWindow", NONE), WINDOW, "getWindow", NONE));
        out.println("in world; window " + win);
        // Detach the real devices, as the creative oracle does: only this
        // driver speaks, and a stray real click cannot start a dig.
        onRender(() -> {
            Class<?> glfw = Class.forName("org.lwjgl.glfw.GLFW");
            for (String setter : new String[]{"glfwSetCursorPosCallback", "glfwSetMouseButtonCallback",
                                              "glfwSetScrollCallback", "glfwSetKeyCallback",
                                              "glfwSetCharModsCallback", "glfwSetCharCallback"}) {
                for (Method m : glfw.getMethods()) {
                    if (m.getName().equals(setter) && m.getParameterCount() == 2
                        && m.getParameterTypes()[0] == long.class
                        && m.getParameterTypes()[1].isInterface()) {
                        m.invoke(null, win, null);
                        break;
                    }
                }
            }
            return null;
        });
        command("gamerule doDaylightCycle false");
        command("gamerule doWeatherCycle false");
        command("gamerule doMobSpawning false");
        command("time set noon");
        command("weather clear");

        if (only.isEmpty() || only.contains("dig")) {
            // Each column is three blocks: the first measures a dig from the
            // press, the second and third the gap between blocks while held.
            scenario("stone.hand", "minecraft:stone", null, null, false, 3, 17000);
            scenario("dirt.hand", "minecraft:dirt", null, null, false, 3, 3500);
            scenario("stone.wooden_pickaxe", "minecraft:stone", "minecraft:wooden_pickaxe", null,
                     false, 3, 4500);
            scenario("planks.wooden_axe", "minecraft:oak_planks", "minecraft:wooden_axe", null,
                     false, 3, 5500);
            scenario("stone.wooden_pickaxe.haste2", "minecraft:stone", "minecraft:wooden_pickaxe",
                     "minecraft:haste 1000 1", false, 3, 3500);
            scenario("stone.creative", "minecraft:stone", null, null, true, 4, 2500);
            scenario("slime.hand", "minecraft:slime_block", null, null, false, 3, 1500);
        }
        if (only.isEmpty() || only.contains("crack")) cracks();
        if (only.isEmpty() || only.contains("particle")) particles();
        out.println("done");
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        File outDir = new File(args[1]);
        outDir.mkdirs();
        out = new PrintStream(new FileOutputStream(new File(outDir, "facts.txt")), true, "UTF-8");
        String only = System.getProperty("ov.only", "");
        String[] mcArgs = Arrays.copyOfRange(args, 2, args.length);
        Thread driver = new Thread(() -> {
            int code = 0;
            try {
                drive(only);
            } catch (Throwable e) {
                e.printStackTrace(out);
                code = 1;
            }
            out.flush();
            Runtime.getRuntime().halt(code);
        }, "ov-breaking-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
