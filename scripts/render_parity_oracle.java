// Ondes VOXEL — the world's rendering, asked of the real 1.20.1 client.
//
// The terrain, the sky, the fog and the lightmap are drawn by the client, and
// the client can be *run*. This class starts the user's own vanilla 1.20.1
// client (the PrismLauncher instance), lets it join a server — ours, so that
// both clients are fed the very same bytes — and walks it through a list of
// scenes: a time of day, a position, a view. At each one it waits for every
// section in range to be compiled, takes a screenshot through the game's own
// screenshot path, and prints what the running game says about the frame: the
// sky, fog, sunrise and cloud colours, the fog distances, the star brightness,
// and all 256 texels of the lightmap with the torch flicker they were built
// with.
//
// Then a sweep: the lightmap at a range of times of day and at the three
// brightness settings, which is what the two undocumented constants of the
// lightmap are fitted against (docs/provenance/rendu-parite.md).
//
// Nothing here reads or translates Mojang code. The official mappings are used
// only to *name* classes, fields and methods (CLAUDE.md § 1), exactly as in
// scripts/creative_screen_oracle.java, and every number printed is produced by
// the game's own bytecode at run time.
//
// Driven by scripts/measure_render_parity.py.
//
//   java -XstartOnFirstThread -cp <out>:<client classpath> ov.RenderParityOracle \
//        <client mappings> <out dir> <scenes file> <Minecraft arguments...>
//
// The scenes file, one directive a line:
//   cmd <command without slash>            sent as the player, in order
//   scene <name> <x> <y> <z> <yaw> <pitch> <time>
//   sweep                                   the lightmap sweep
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class RenderParityOracle {

    // ── The mappings, as three lookup tables (same reader as the tabs oracle) ─

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
            // Declared methods up the hierarchy: getShade lives on ClientLevel,
            // getDayTime on Level.
            for (Class<?> c = cls(official); c != null; c = c.getSuperclass()) {
                for (Method m : c.getDeclaredMethods()) {
                    if (!m.getName().equals(obf) || m.getParameterCount() != argTypes.length) continue;
                    boolean ok = true;
                    for (int i = 0; i < argTypes.length && ok; i++) {
                        // getTypeName, not getName: an array's name is "[F",
                        // its type name "float[]" as the mappings write it.
                        // The first run lost AmbientOcclusionFace.calculate
                        // (a float[] parameter) to exactly that.
                        ok = m.getParameterTypes()[i].getTypeName().equals(obfOf(argTypes[i]));
                    }
                    if (ok) {
                        m.setAccessible(true);
                        return m;
                    }
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

    // ── The game, and a way to run on its render thread ─────────────────────

    static final String MC = "net.minecraft.client.Minecraft";
    static final String LEVEL = "net.minecraft.client.multiplayer.ClientLevel";
    static final String WORLD = "net.minecraft.world.level.Level";
    static final String GAME_RENDERER = "net.minecraft.client.renderer.GameRenderer";
    static final String LEVEL_RENDERER = "net.minecraft.client.renderer.LevelRenderer";
    static final String LIGHT = "net.minecraft.client.renderer.LightTexture";
    static final String FOG = "net.minecraft.client.renderer.FogRenderer";
    static final String EFFECTS = "net.minecraft.client.renderer.DimensionSpecialEffects";
    static final String OPTIONS = "net.minecraft.client.Options";
    static final String OPTION = "net.minecraft.client.OptionInstance";
    static final String CAMERA = "net.minecraft.client.Camera";
    static final String VEC3 = "net.minecraft.world.phys.Vec3";
    static final String NATIVE = "com.mojang.blaze3d.platform.NativeImage";
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

    static void send(String text) throws Exception {
        onRender(() -> {
            Object connection = call(mc, MC, "getConnection", NONE);
            call(connection, "net.minecraft.client.multiplayer.ClientPacketListener", "sendCommand",
                 new String[]{"java.lang.String"}, text);
            return null;
        });
        out.println("cmd " + text);
        pause(150);
    }

    static void shot(String name) throws Exception {
        onRender(() -> {
            Object target = call(mc, MC, "getMainRenderTarget", NONE);
            java.util.function.Consumer<Object> sink = c -> {};
            method("net.minecraft.client.Screenshot", "grab", "java.io.File", "java.lang.String",
                   "com.mojang.blaze3d.pipeline.RenderTarget", "java.util.function.Consumer")
                .invoke(null, gameDir, name, target, sink);
            return null;
        });
        out.println("shot " + name);
        pause(500);
    }

    static String vec(Object v) throws Exception {
        return String.format(Locale.ROOT, "%.9f %.9f %.9f", get(v, VEC3, "x"), get(v, VEC3, "y"),
                             get(v, VEC3, "z"));
    }

    /// Everything about the frame the game will draw next, read on the render
    /// thread between two frames.
    static void dumpFrame(String label) throws Exception {
        onRender(() -> {
            Object level = get(mc, MC, "level");
            Object gr = get(mc, MC, "gameRenderer");
            Object camera = call(gr, GAME_RENDERER, "getMainCamera", NONE);
            Object pos = call(camera, CAMERA, "getPosition", NONE);
            out.println("── frame " + label);
            Object w = call(mc, MC, "getWindow", NONE);
            out.println("window " + call(w, WINDOW, "getWidth", NONE) + "x" + call(w, WINDOW, "getHeight", NONE));
            out.println("camera " + vec(pos) + " yaw " + call(camera, CAMERA, "getYRot", NONE)
                        + " pitch " + call(camera, CAMERA, "getXRot", NONE));
            long day = (Long) call(level, WORLD, "getDayTime", NONE);
            out.println("daytime " + day);
            out.println("skyDarken " + call(level, LEVEL, "getSkyDarken", new String[]{"float"}, 1.0F));
            float sunAngle = (Float) call(level, WORLD, "getSunAngle", new String[]{"float"}, 1.0F);
            out.println("sunAngle " + sunAngle);
            Object effects = call(level, LEVEL, "effects", NONE);
            float tod = (float) (sunAngle / (2.0 * Math.PI));
            float[] sunrise = (float[]) call(effects, EFFECTS, "getSunriseColor",
                                             new String[]{"float", "float"}, tod, 1.0F);
            out.println("sunrise " + (sunrise == null ? "none" : Arrays.toString(sunrise)));
            out.println("sky " + vec(call(level, LEVEL, "getSkyColor",
                                          new String[]{VEC3, "float"}, pos, 1.0F)));
            out.println("cloud " + vec(call(level, LEVEL, "getCloudColor", new String[]{"float"}, 1.0F)));
            out.println("stars " + call(level, LEVEL, "getStarBrightness", new String[]{"float"}, 1.0F));
            out.println("fogColour " + get(null, FOG, "fogRed") + " " + get(null, FOG, "fogGreen") + " "
                        + get(null, FOG, "fogBlue"));
            Class<?> rs = Class.forName("com.mojang.blaze3d.systems.RenderSystem");
            out.println("shaderFog " + rs.getMethod("getShaderFogStart").invoke(null) + " "
                        + rs.getMethod("getShaderFogEnd").invoke(null) + " "
                        + rs.getMethod("getShaderFogShape").invoke(null) + " "
                        + Arrays.toString((float[]) rs.getMethod("getShaderFogColor").invoke(null)));
            out.println("renderDistance " + call(gr, GAME_RENDERER, "getRenderDistance", NONE));
            // The two fogs of a frame, asked of the game one after the other:
            // the sky's, then the terrain's (which is what a frame ends on).
            Object distance = call(gr, GAME_RENDERER, "getRenderDistance", NONE);
            for (String mode : new String[]{"FOG_SKY", "FOG_TERRAIN"}) {
                Object fogMode = get(null, FOG + "$FogMode", mode);
                call(null, FOG, "setupFog", new String[]{CAMERA, FOG + "$FogMode", "float", "boolean", "float"},
                     camera, fogMode, distance, false, 1.0F);
                out.println("fog " + mode + " " + rs.getMethod("getShaderFogStart").invoke(null) + " "
                            + rs.getMethod("getShaderFogEnd").invoke(null) + " "
                            + rs.getMethod("getShaderFogShape").invoke(null));
            }
            out.println("cloudHeight " + call(effects, EFFECTS, "getCloudHeight", NONE)
                        + " moonPhase " + call(call(level, WORLD, "dimensionType", NONE),
                                               "net.minecraft.world.level.dimension.DimensionType",
                                               "moonPhase", new String[]{"long"}, day));
            StringBuilder shade = new StringBuilder("shade");
            for (Object d : cls("net.minecraft.core.Direction").getEnumConstants()) {
                shade.append(' ').append(d).append('=')
                     .append(call(level, LEVEL, "getShade",
                                  new String[]{"net.minecraft.core.Direction", "boolean"}, d, true));
            }
            out.println(shade);
            out.println("rendered " + call(get(mc, MC, "levelRenderer"), LEVEL_RENDERER,
                                           "countRenderedChunks", NONE)
                        + " all " + call(get(mc, MC, "levelRenderer"), LEVEL_RENDERER,
                                         "hasRenderedAllChunks", NONE)
                        + " fps " + call(mc, MC, "getFps", NONE));
            dumpLightmapInline(gr);
            return null;
        });
    }

    /// The 16x16 lightmap as the game last built it: block light across, sky
    /// light down, with the flicker it was built from. NativeImage stores
    /// ABGR, so the channels are unpacked here and printed as R G B.
    static void dumpLightmapInline(Object gr) throws Exception {
        Object lt = call(gr, GAME_RENDERER, "lightTexture", NONE);
        Object pixels = get(lt, LIGHT, "lightPixels");
        Method at = method(NATIVE, "getPixelRGBA", "int", "int");
        out.println("flicker " + get(lt, LIGHT, "blockLightRedFlicker"));
        for (int sky = 0; sky < 16; sky++) {
            StringBuilder b = new StringBuilder("lightmap " + sky);
            for (int block = 0; block < 16; block++) {
                int abgr = (Integer) at.invoke(pixels, block, sky);
                b.append(' ').append(abgr & 0xFF).append(',').append((abgr >> 8) & 0xFF).append(',')
                 .append((abgr >> 16) & 0xFF);
            }
            out.println(b);
        }
    }

    static void setOption(String name, Object value) throws Exception {
        onRender(() -> {
            Object options = get(mc, MC, "options");
            Object instance = get(options, OPTIONS, name);
            call(instance, OPTION, "set", new String[]{"java.lang.Object"}, value);
            return null;
        });
    }

    static void hover() throws Exception {
        // No gravity between the teleport and the capture: a falling camera
        // is a moving camera.
        onRender(() -> {
            Object player = get(mc, MC, "player");
            Object abilities = call(player, "net.minecraft.world.entity.player.Player", "getAbilities", NONE);
            field("net.minecraft.world.entity.player.Abilities", "flying").set(abilities, true);
            call(player, "net.minecraft.world.entity.Entity", "setDeltaMovement",
                 new String[]{"double", "double", "double"}, 0.0, 0.0, 0.0);
            return null;
        });
    }

    /// Every chunk the server streams (a 17 x 17 square: SETTLE_CHUNKS in the
    /// driver) has arrived, the count has not moved for three seconds, and
    /// every section in range is compiled. The same rule as our client's
    /// --settle-shot, so neither capture is taken on a half-loaded scene.
    static int settleChunks = 289;

    static void waitCompiled(long timeoutMs) throws Exception {
        long start = System.currentTimeMillis();
        int stable = 0;
        int last = -1;
        while (System.currentTimeMillis() - start < timeoutMs) {
            int[] state = onRender(() -> {
                Object level = get(mc, MC, "level");
                Object source = call(level, LEVEL, "getChunkSource", NONE);
                int loaded = (Integer) call(source, "net.minecraft.client.multiplayer.ClientChunkCache",
                                            "getLoadedChunksCount", NONE);
                boolean all = (Boolean) call(get(mc, MC, "levelRenderer"), LEVEL_RENDERER,
                                             "hasRenderedAllChunks", NONE);
                return new int[]{loaded, all ? 1 : 0};
            });
            boolean ok = state[1] == 1 && state[0] == last && state[0] >= settleChunks;
            stable = ok ? stable + 1 : 0;
            last = state[0];
            if (stable >= 6) {
                out.println("settled: " + state[0] + " chunks");
                return;
            }
            pause(500);
        }
        out.println("warning: not settled after " + timeoutMs + " ms (" + last + " chunks)");
    }

    /// Smooth lighting as the game computes it, for one face of one block:
    /// the four corner brightnesses and packed lightmap coordinates of the
    /// game's own AmbientOcclusionFace, next to the quad's vertex positions so
    /// that corner i can be matched to a vertex.
    static void ao(String[] p) throws Exception {
        int x = Integer.parseInt(p[1]), y = Integer.parseInt(p[2]), z = Integer.parseInt(p[3]);
        String face = p[4].toUpperCase(Locale.ROOT);
        onRender(() -> {
            Object level = get(mc, MC, "level");
            Object pos = cls("net.minecraft.core.BlockPos").getConstructor(int.class, int.class, int.class)
                                                           .newInstance(x, y, z);
            Object state = call(level, "net.minecraft.world.level.BlockGetter", "getBlockState",
                                new String[]{"net.minecraft.core.BlockPos"}, pos);
            Object dir = null;
            for (Object d : cls("net.minecraft.core.Direction").getEnumConstants()) {
                if (((Enum<?>) d).name().equals(face)) dir = d;
            }
            Object dispatcher = call(mc, MC, "getBlockRenderer", NONE);
            Object model = call(dispatcher, "net.minecraft.client.renderer.block.BlockRenderDispatcher",
                                "getBlockModel", new String[]{"net.minecraft.world.level.block.state.BlockState"},
                                state);
            Object random = call(null, "net.minecraft.util.RandomSource", "create", new String[]{"long"}, 42L);
            List<?> quads = (List<?>) call(model, "net.minecraft.client.resources.model.BakedModel", "getQuads",
                                           new String[]{"net.minecraft.world.level.block.state.BlockState",
                                                        "net.minecraft.core.Direction",
                                                        "net.minecraft.util.RandomSource"},
                                           state, dir, random);
            String aof = "net.minecraft.client.renderer.block.ModelBlockRenderer$AmbientOcclusionFace";
            Constructor<?> ctor = cls(aof).getDeclaredConstructor();
            ctor.setAccessible(true);
            Object face0 = ctor.newInstance();
            float[] shape = new float[24];
            // Full-cube faces: the shape flags say "on the block boundary".
            java.util.BitSet flags = new java.util.BitSet(3);
            flags.set(0);
            call(face0, aof, "calculate", new String[]{"net.minecraft.world.level.BlockAndTintGetter",
                     "net.minecraft.world.level.block.state.BlockState", "net.minecraft.core.BlockPos",
                     "net.minecraft.core.Direction", "float[]", "java.util.BitSet", "boolean"},
                 level, state, pos, dir, shape, flags, true);
            float[] brightness = (float[]) get(face0, aof, "brightness");
            int[] light = (int[]) get(face0, aof, "lightmap");
            StringBuilder b = new StringBuilder("ao " + x + " " + y + " " + z + " " + face + " " + state);
            for (int i = 0; i < 4; i++) {
                b.append(String.format(Locale.ROOT, " | %.6f block %d sky %d", brightness[i],
                                       light[i] & 0xFFFF, (light[i] >>> 16) & 0xFFFF));
            }
            if (!quads.isEmpty()) {
                int[] v = (int[]) call(quads.get(0), "net.minecraft.client.renderer.block.model.BakedQuad",
                                       "getVertices", NONE);
                int stride = v.length / 4;
                for (int i = 0; i < 4; i++) {
                    b.append(String.format(Locale.ROOT, " | v%d %.4f %.4f %.4f", i,
                                           Float.intBitsToFloat(v[i * stride]),
                                           Float.intBitsToFloat(v[i * stride + 1]),
                                           Float.intBitsToFloat(v[i * stride + 2])));
                }
            }
            out.println(b);
            return null;
        });
    }

    static void scene(String[] p) throws Exception {
        String name = p[1];
        double x = Double.parseDouble(p[2]), y = Double.parseDouble(p[3]), z = Double.parseDouble(p[4]);
        float yaw = Float.parseFloat(p[5]), pitch = Float.parseFloat(p[6]);
        long time = Long.parseLong(p[7]);
        send("time set " + time);
        send(String.format(Locale.ROOT, "tp @s %.4f %.4f %.4f %.4f %.4f", x, y, z, yaw, pitch));
        for (int i = 0; i < 8; i++) {
            hover();
            pause(250);
        }
        waitCompiled(90000);
        // Light updates and neighbour recompiles land after the first compile.
        for (int i = 0; i < 8; i++) {
            hover();
            pause(250);
        }
        waitCompiled(30000);
        // The time again, right before the capture: the server's clock may
        // have been set while chunks were still arriving.
        send("time set " + time);
        pause(1500);
        dumpFrame(name);
        shot(name + ".png");
    }

    /// Which of a block's weighted variants the game draws at each position:
    /// the state's own position seed, and the texture coordinates of the
    /// first quad of `face` from the model the game resolves with it — the
    /// same random the chunk compiler seeds. Stone, dirt, grass, sand, tall
    /// grass... all pick a rotation or a mirror per position, and a renderer
    /// that picks another shows the right colours on the wrong texels.
    ///   variants <x0> <y> <z0> <x1> <z1> <face>
    static void variants(String[] p) throws Exception {
        int x0 = Integer.parseInt(p[1]), y = Integer.parseInt(p[2]), z0 = Integer.parseInt(p[3]);
        int x1 = Integer.parseInt(p[4]), z1 = Integer.parseInt(p[5]);
        // `none` asks for the quads with no cull face: a plant's cross.
        String face = p[6].toUpperCase(Locale.ROOT);
        onRender(() -> {
            Object level = get(mc, MC, "level");
            Object dir = null;
            for (Object d : cls("net.minecraft.core.Direction").getEnumConstants()) {
                if (((Enum<?>) d).name().equals(face)) dir = d;
            }
            if (dir == null && !face.equals("NONE")) throw new IllegalArgumentException("face " + face);
            Object dispatcher = call(mc, MC, "getBlockRenderer", NONE);
            Object random = call(null, "net.minecraft.util.RandomSource", "create", new String[]{"long"}, 0L);
            Constructor<?> posCtor = cls("net.minecraft.core.BlockPos").getConstructor(int.class, int.class, int.class);
            String base = "net.minecraft.world.level.block.state.BlockBehaviour$BlockStateBase";
            for (int x = x0; x <= x1; x++) {
                for (int z = z0; z <= z1; z++) {
                    Object pos = posCtor.newInstance(x, y, z);
                    Object state = call(level, "net.minecraft.world.level.BlockGetter", "getBlockState",
                                        new String[]{"net.minecraft.core.BlockPos"}, pos);
                    long seed = (Long) call(state, base, "getSeed", new String[]{"net.minecraft.core.BlockPos"}, pos);
                    Object model = call(dispatcher, "net.minecraft.client.renderer.block.BlockRenderDispatcher",
                                        "getBlockModel",
                                        new String[]{"net.minecraft.world.level.block.state.BlockState"}, state);
                    call(random, "net.minecraft.util.RandomSource", "setSeed", new String[]{"long"}, seed);
                    List<?> quads = (List<?>) call(model, "net.minecraft.client.resources.model.BakedModel",
                                                   "getQuads",
                                                   new String[]{"net.minecraft.world.level.block.state.BlockState",
                                                                "net.minecraft.core.Direction",
                                                                "net.minecraft.util.RandomSource"},
                                                   state, dir, random);
                    Object offset = call(state, base, "getOffset",
                                         new String[]{"net.minecraft.world.level.BlockGetter",
                                                      "net.minecraft.core.BlockPos"}, level, pos);
                    StringBuilder b = new StringBuilder(String.format(Locale.ROOT,
                        "variant %d %d %d %s seed %d offset %s quads %d", x, y, z, state, seed, vec(offset),
                        quads.size()));
                    if (!quads.isEmpty()) {
                        int[] v = (int[]) call(quads.get(0), "net.minecraft.client.renderer.block.model.BakedQuad",
                                               "getVertices", NONE);
                        int stride = v.length / 4;
                        for (int i = 0; i < 4; i++) {
                            b.append(String.format(Locale.ROOT, " | %.4f %.4f %.4f uv %.6f %.6f",
                                                   Float.intBitsToFloat(v[i * stride]),
                                                   Float.intBitsToFloat(v[i * stride + 1]),
                                                   Float.intBitsToFloat(v[i * stride + 2]),
                                                   Float.intBitsToFloat(v[i * stride + 4]),
                                                   Float.intBitsToFloat(v[i * stride + 5])));
                        }
                    }
                    out.println(b);
                }
            }
            return null;
        });
    }

    /// Every block the game nudges off its centre, and how far: the default
    /// state of each registered block that has an offset function, asked for
    /// its offset at 512 positions, printed as the range on each axis. That is
    /// the table ov_render's block_models.cpp keeps — nothing in the assets or
    /// the data generator carries it.
    static void offsets() throws Exception {
        onRender(() -> {
            Object level = get(mc, MC, "level");
            Object registry = get(null, "net.minecraft.core.registries.BuiltInRegistries", "BLOCK");
            String base = "net.minecraft.world.level.block.state.BlockBehaviour$BlockStateBase";
            Constructor<?> posCtor = cls("net.minecraft.core.BlockPos").getConstructor(int.class, int.class, int.class);
            int count = 0;
            for (Object block : (Iterable<?>) registry) {
                Object state = call(block, "net.minecraft.world.level.block.Block", "defaultBlockState", NONE);
                if (!(Boolean) call(state, base, "hasOffsetFunction", NONE)) continue;
                double[] lo = {9, 9, 9};
                double[] hi = {-9, -9, -9};
                for (int k = 0; k < 512; k++) {
                    Object pos = posCtor.newInstance(k * 7919 - 2000000, 64, k * 104729 - 3000000);
                    Object o = call(state, base, "getOffset",
                                    new String[]{"net.minecraft.world.level.BlockGetter",
                                                 "net.minecraft.core.BlockPos"}, level, pos);
                    double[] v = {(Double) get(o, VEC3, "x"), (Double) get(o, VEC3, "y"), (Double) get(o, VEC3, "z")};
                    for (int a = 0; a < 3; a++) {
                        lo[a] = Math.min(lo[a], v[a]);
                        hi[a] = Math.max(hi[a], v[a]);
                    }
                }
                Object key = call(registry, "net.minecraft.core.Registry", "getKey",
                                  new String[]{"java.lang.Object"}, block);
                out.println(String.format(Locale.ROOT, "offsets %s x %.9f %.9f y %.9f %.9f z %.9f %.9f",
                                          key, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]));
                count++;
            }
            out.println("offsets: " + count + " blocks with an offset function");
            return null;
        });
    }

    static void sweep() throws Exception {
        long[] times = {6000, 11000, 12000, 12500, 12800, 13000, 13200, 13500, 14000, 18000, 23000, 23500};
        double[] gammas = {0.0, 0.5, 1.0};
        for (double g : gammas) {
            setOption("gamma", g);
            for (long t : times) {
                send("time set " + t);
                pause(700);
                onRender(() -> {
                    Object level = get(mc, MC, "level");
                    out.println("── sweep gamma " + g + " time " + t + " daytime "
                                + call(level, WORLD, "getDayTime", NONE) + " skyDarken "
                                + call(level, LEVEL, "getSkyDarken", new String[]{"float"}, 1.0F));
                    dumpLightmapInline(get(mc, MC, "gameRenderer"));
                    return null;
                });
            }
        }
        setOption("gamma", 0.5);
    }

    static void drive(Path scenes) throws Exception {
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
        pause(6000);
        gameDir = (File) get(mc, MC, "gameDirectory");
        win = onRender(() -> (Long) call(call(mc, MC, "getWindow", NONE), WINDOW, "getWindow", NONE));
        out.println("in world; window " + win);
        // Detach the real devices: the window sits on the user's desktop.
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
            // F1: no hand, no hotbar, no crosshair.
            field(OPTIONS, "hideGui").set(get(mc, MC, "options"), true);
            return null;
        });

        for (String line : Files.readAllLines(scenes, StandardCharsets.UTF_8)) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            // One directive failing is logged and skipped, never fatal: the
            // first run died on its first `ao` line and lost eleven scenes and
            // the sweep with it, and its place in the lock queue.
            try {
                switch (p[0]) {
                    case "cmd" -> send(line.substring(4).trim());
                    case "wait" -> pause(Long.parseLong(p[1]));
                    case "scene" -> scene(p);
                    case "sweep" -> sweep();
                    case "ao" -> ao(p);
                    case "variants" -> variants(p);
                    case "offsets" -> offsets();
                    default -> out.println("unknown directive: " + line);
                }
            } catch (Throwable e) {
                out.println("failed: " + line + " -> " + e);
                Throwable cause = e.getCause();
                if (cause != null) out.println("   cause: " + cause);
            }
        }
        out.println("done");
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        File outDir = new File(args[1]);
        outDir.mkdirs();
        out = new PrintStream(new FileOutputStream(new File(outDir, "facts.txt")), true, "UTF-8");
        Path scenes = Paths.get(args[2]);
        String[] mcArgs = Arrays.copyOfRange(args, 3, args.length);
        Thread driver = new Thread(() -> {
            int code = 0;
            try {
                drive(scenes);
            } catch (Throwable e) {
                e.printStackTrace(out);
                code = 1;
            }
            out.flush();
            Runtime.getRuntime().halt(code);
        }, "ov-render-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
