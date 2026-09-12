// The entity models of the real 1.20.1 client, as the game itself bakes them.
//
// An entity model is not in the resource pack: Java builds it at run time
// (LayerDefinitions.createRoots) and bakes it into ModelPart trees. This
// program runs the user's own client, waits until its resources have loaded,
// and prints every baked layer — every part's pivot, rotation and scale, every
// cube's polygons with the texture coordinates the game will draw them with —
// as JSON. Nothing is read from decompiled code: the official mappings are
// used only to *name* the classes and fields reflected on, exactly as the
// render parity and creative screen oracles do, and every number printed is
// produced by the game's own bytecode while it runs.
//
// The output is Mojang data, like the data generator's: it is written under
// data/vanilla/1.20.1/ (gitignored) and never committed.
// scripts/measure_entity_render.py drives it; docs/provenance/rendu-entites.md
// says what it established.
//
//   java -XstartOnFirstThread -cp <out>:<client classpath> ov.EntityModelOracle \
//        <client mappings> <out json> <Minecraft arguments...>
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class EntityModelOracle {

    // ── The mappings, as lookup tables (same reader as the other oracles) ───

    static final Map<String, String> CLASSES = new HashMap<>();
    static final Map<String, String> OFFICIAL_OF = new HashMap<>();
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
                    String obf = line.substring(arrow + 4, line.length() - 1);
                    CLASSES.put(current, obf);
                    OFFICIAL_OF.put(obf, current);
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

    /// The official name of a runtime class, walking up to the first mapped one
    /// (a lambda or an anonymous class has no name of its own).
    static String officialName(Class<?> c) {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            String name = OFFICIAL_OF.get(k.getName());
            if (name != null) return k == c ? name : name + "+";
        }
        return c.getName();
    }

    static Field field(String official, String name) throws Exception {
        String obf = FIELDS.getOrDefault(official, Map.of()).get(name);
        if (obf == null) throw new NoSuchFieldException(official + "." + name);
        for (Class<?> c = cls(official); c != null; c = c.getSuperclass()) {
            try {
                Field f = c.getDeclaredField(obf);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException ignored) {
            }
        }
        throw new NoSuchFieldException(official + "." + name + " (" + obf + ")");
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
            for (Class<?> c = cls(official); c != null; c = c.getSuperclass()) {
                for (Method m : c.getDeclaredMethods()) {
                    if (!m.getName().equals(obf) || m.getParameterCount() != argTypes.length) continue;
                    boolean ok = true;
                    for (int i = 0; i < argTypes.length && ok; i++) {
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
    static final String MODEL_SET = "net.minecraft.client.model.geom.EntityModelSet";
    static final String LAYER_LOCATION = "net.minecraft.client.model.geom.ModelLayerLocation";
    static final String LAYER_DEFINITION = "net.minecraft.client.model.geom.builders.LayerDefinition";
    static final String MATERIAL = "net.minecraft.client.model.geom.builders.MaterialDefinition";
    static final String PART = "net.minecraft.client.model.geom.ModelPart";
    static final String CUBE = PART + "$Cube";
    static final String POLYGON = PART + "$Polygon";
    static final String VERTEX = PART + "$Vertex";
    static final String DISPATCHER = "net.minecraft.client.renderer.entity.EntityRenderDispatcher";
    static final String RENDERER = "net.minecraft.client.renderer.entity.EntityRenderer";
    static final String LIVING_RENDERER = "net.minecraft.client.renderer.entity.LivingEntityRenderer";
    static final String ENTITY_TYPE = "net.minecraft.world.entity.EntityType";
    static final String REGISTRIES = "net.minecraft.core.registries.BuiltInRegistries";
    static final String REGISTRY = "net.minecraft.core.Registry";
    static final String DYE = "net.minecraft.world.item.DyeColor";
    static final String SHEEP = "net.minecraft.world.entity.animal.Sheep";
    static final String GAME_RENDERER = "net.minecraft.client.renderer.GameRenderer";
    static final String OVERLAY = "net.minecraft.client.renderer.texture.OverlayTexture";
    static final String DYNAMIC = "net.minecraft.client.renderer.texture.DynamicTexture";
    static final String NATIVE = "com.mojang.blaze3d.platform.NativeImage";

    static Object mc;

    static <T> T onRender(Callable<T> c) throws Exception {
        CompletableFuture<T> f = new CompletableFuture<>();
        ((Executor) mc).execute(() -> {
            try {
                f.complete(c.call());
            } catch (Throwable e) {
                f.completeExceptionally(e);
            }
        });
        return f.get(60, TimeUnit.SECONDS);
    }

    // ── JSON, written by hand: the output is flat and the classpath is the game's

    static String num(float v) {
        if (v == (long) v && Math.abs(v) < 1e9) return Long.toString((long) v);
        return Float.toString(v);
    }

    static String str(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\');
            b.append(c);
        }
        return b.append('"').toString();
    }

    static float f(Object target, String official, String name) throws Exception {
        return ((Number) get(target, official, name)).floatValue();
    }

    /// One part, its cubes as the polygons the game draws, and its children.
    static void part(StringBuilder b, String name, Object part, String indent) throws Exception {
        b.append(indent).append("{\"name\": ").append(str(name));
        b.append(", \"pivot\": [").append(num(f(part, PART, "x"))).append(", ")
         .append(num(f(part, PART, "y"))).append(", ").append(num(f(part, PART, "z"))).append(']');
        // Radians, as the game stores them. The converter turns them into
        // degrees and into this project's axes; printing them raw keeps this
        // program a reader and nothing else.
        b.append(", \"rotation\": [").append(num(f(part, PART, "xRot"))).append(", ")
         .append(num(f(part, PART, "yRot"))).append(", ").append(num(f(part, PART, "zRot"))).append(']');
        b.append(", \"scale\": [").append(num(f(part, PART, "xScale"))).append(", ")
         .append(num(f(part, PART, "yScale"))).append(", ").append(num(f(part, PART, "zScale"))).append(']');
        b.append(", \"visible\": ").append(get(part, PART, "visible"));
        b.append(", \"skip_draw\": ").append(get(part, PART, "skipDraw"));
        b.append(",\n").append(indent).append(" \"cubes\": [");
        List<?> cubes = (List<?>) get(part, PART, "cubes");
        for (int ci = 0; ci < cubes.size(); ci++) {
            Object cube = cubes.get(ci);
            b.append(ci == 0 ? "\n" : ",\n").append(indent).append("  {\"min\": [")
             .append(num(f(cube, CUBE, "minX"))).append(", ").append(num(f(cube, CUBE, "minY"))).append(", ")
             .append(num(f(cube, CUBE, "minZ"))).append("], \"max\": [")
             .append(num(f(cube, CUBE, "maxX"))).append(", ").append(num(f(cube, CUBE, "maxY"))).append(", ")
             .append(num(f(cube, CUBE, "maxZ"))).append("], \"polygons\": [");
            Object[] polygons = (Object[]) get(cube, CUBE, "polygons");
            for (int pi = 0; pi < polygons.length; pi++) {
                Object polygon = polygons[pi];
                org.joml.Vector3f normal = (org.joml.Vector3f) get(polygon, POLYGON, "normal");
                b.append(pi == 0 ? "" : ", ").append("{\"normal\": [").append(num(normal.x)).append(", ")
                 .append(num(normal.y)).append(", ").append(num(normal.z)).append("], \"vertices\": [");
                Object[] vertices = (Object[]) get(polygon, POLYGON, "vertices");
                for (int vi = 0; vi < vertices.length; vi++) {
                    org.joml.Vector3f pos = (org.joml.Vector3f) get(vertices[vi], VERTEX, "pos");
                    b.append(vi == 0 ? "" : ", ").append('[').append(num(pos.x)).append(", ")
                     .append(num(pos.y)).append(", ").append(num(pos.z)).append(", ")
                     .append(num(f(vertices[vi], VERTEX, "u"))).append(", ")
                     .append(num(f(vertices[vi], VERTEX, "v"))).append(']');
                }
                b.append("]}");
            }
            b.append("]}");
        }
        b.append(cubes.isEmpty() ? "]" : "\n" + indent + " ]");
        b.append(",\n").append(indent).append(" \"children\": [");
        Map<?, ?> children = (Map<?, ?>) get(part, PART, "children");
        boolean first = true;
        for (Map.Entry<?, ?> child : children.entrySet()) {
            b.append(first ? "\n" : ",\n");
            first = false;
            part(b, (String) child.getKey(), child.getValue(), indent + "  ");
        }
        b.append(first ? "]}" : "\n" + indent + " ]}");
    }

    static void dump(Path outFile) throws Exception {
        StringBuilder b = new StringBuilder("{\n\"format\": \"ov-java-entity-models-1\",\n");
        b.append("\"game\": \"1.20.1\",\n");

        // ── Every baked layer ───────────────────────────────────────────────
        Object models = onRender(() -> call(mc, MC, "getEntityModels", NONE));
        Map<?, ?> roots = (Map<?, ?>) get(models, MODEL_SET, "roots");
        TreeMap<String, Object> sorted = new TreeMap<>();
        for (Map.Entry<?, ?> e : roots.entrySet()) {
            Object location = e.getKey();
            String key = get(location, LAYER_LOCATION, "model") + "#" + get(location, LAYER_LOCATION, "layer");
            sorted.put(key, e.getValue());
        }
        b.append("\"layers\": {");
        boolean firstLayer = true;
        for (Map.Entry<String, Object> e : sorted.entrySet()) {
            Object definition = e.getValue();
            Object material = get(definition, LAYER_DEFINITION, "material");
            Object root = call(definition, LAYER_DEFINITION, "bakeRoot", NONE);
            b.append(firstLayer ? "\n" : ",\n");
            firstLayer = false;
            b.append(str(e.getKey())).append(": {\"texture\": [").append(get(material, MATERIAL, "xTexSize"))
             .append(", ").append(get(material, MATERIAL, "yTexSize")).append("], \"root\":\n");
            part(b, "root", root, "  ");
            b.append('}');
        }
        b.append("\n},\n");
        System.err.println("[oracle] layers: " + sorted.size());

        // ── Which renderer, model and layers each entity type is drawn by ──
        b.append("\"renderers\": {");
        Object dispatcher = onRender(() -> call(mc, MC, "getEntityRenderDispatcher", NONE));
        Map<?, ?> renderers = (Map<?, ?>) get(dispatcher, DISPATCHER, "renderers");
        Object typeRegistry = get(null, REGISTRIES, "ENTITY_TYPE");
        TreeMap<String, String> rows = new TreeMap<>();
        for (Map.Entry<?, ?> e : renderers.entrySet()) {
            Object key = call(typeRegistry, REGISTRY, "getKey", new String[]{"java.lang.Object"}, e.getKey());
            Object renderer = e.getValue();
            StringBuilder row = new StringBuilder("{\"renderer\": " + str(officialName(renderer.getClass())));
            row.append(", \"shadow_radius\": ").append(num(f(renderer, RENDERER, "shadowRadius")));
            if (cls(LIVING_RENDERER).isInstance(renderer)) {
                Object model = get(renderer, LIVING_RENDERER, "model");
                row.append(", \"model\": ").append(str(officialName(model.getClass())));
                row.append(", \"layers\": [");
                List<?> layers = (List<?>) get(renderer, LIVING_RENDERER, "layers");
                for (int i = 0; i < layers.size(); i++) {
                    row.append(i == 0 ? "" : ", ").append(str(officialName(layers.get(i).getClass())));
                }
                row.append(']');
            }
            rows.put(String.valueOf(key), row.append('}').toString());
        }
        boolean firstRow = true;
        for (Map.Entry<String, String> e : rows.entrySet()) {
            b.append(firstRow ? "\n" : ",\n").append(str(e.getKey())).append(": ").append(e.getValue());
            firstRow = false;
        }
        b.append("\n},\n");

        // ── Colours the game computes rather than reads ─────────────────────
        b.append("\"dye\": {");
        Object[] dyes = cls(DYE).getEnumConstants();
        for (int i = 0; i < dyes.length; i++) {
            float[] diffuse = (float[]) call(dyes[i], DYE, "getTextureDiffuseColors", NONE);
            float[] fleece = (float[]) call(null, SHEEP, "getColorArray", new String[]{DYE}, dyes[i]);
            b.append(i == 0 ? "\n" : ",\n").append(str(String.valueOf(call(dyes[i], DYE, "getName", NONE))))
             .append(": {\"id\": ").append(call(dyes[i], DYE, "getId", NONE))
             .append(", \"diffuse\": [").append(num(diffuse[0])).append(", ").append(num(diffuse[1]))
             .append(", ").append(num(diffuse[2])).append("], \"sheep\": [").append(num(fleece[0]))
             .append(", ").append(num(fleece[1])).append(", ").append(num(fleece[2])).append("]}");
        }
        b.append("\n},\n");

        // The overlay texture: the red of a hurt entity and the white of a
        // creeper about to go off are both read from this 16x16 image.
        b.append("\"overlay\": [");
        String overlayRows = onRender(() -> {
            Object gr = get(mc, MC, "gameRenderer");
            Object overlay = call(gr, GAME_RENDERER, "overlayTexture", NONE);
            Object texture = get(overlay, OVERLAY, "texture");
            Object pixels = call(texture, DYNAMIC, "getPixels", NONE);
            Method at = method(NATIVE, "getPixelRGBA", "int", "int");
            StringBuilder o = new StringBuilder();
            for (int y = 0; y < 16; y++) {
                o.append(y == 0 ? "\n[" : ",\n[");
                for (int x = 0; x < 16; x++) {
                    int abgr = (Integer) at.invoke(pixels, x, y);
                    // NativeImage stores ABGR; printed as R, G, B, A.
                    o.append(x == 0 ? "" : ", ").append('[').append(abgr & 0xFF).append(", ")
                     .append((abgr >> 8) & 0xFF).append(", ").append((abgr >> 16) & 0xFF).append(", ")
                     .append((abgr >>> 24) & 0xFF).append(']');
                }
                o.append(']');
            }
            return o.toString();
        });
        b.append(overlayRows).append("\n]\n}\n");

        Files.writeString(outFile, b.toString(), StandardCharsets.UTF_8);
        System.err.println("[oracle] wrote " + outFile + " (" + b.length() + " chars)");
    }

    static void drive(Path outFile) throws Exception {
        // The game exists once its singleton does; its models exist once the
        // first resource reload has run, which is when the loading overlay
        // goes away.
        for (int i = 0; i < 1200; i++) {
            Method getInstance = null;
            try {
                getInstance = method(MC, "getInstance");
            } catch (Throwable ignored) {
            }
            if (getInstance != null) {
                mc = getInstance.invoke(null);
                if (mc != null) {
                    boolean ready = onRender(() -> {
                        if (call(mc, MC, "getOverlay", NONE) != null) return false;
                        Object models = call(mc, MC, "getEntityModels", NONE);
                        return !((Map<?, ?>) get(models, MODEL_SET, "roots")).isEmpty();
                    });
                    if (ready) break;
                }
            }
            Thread.sleep(500);
        }
        Thread.sleep(2000);
        dump(outFile);
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        Path outFile = Paths.get(args[1]);
        String[] mcArgs = Arrays.copyOfRange(args, 2, args.length);
        Thread driver = new Thread(() -> {
            int code = 0;
            try {
                drive(outFile);
            } catch (Throwable e) {
                e.printStackTrace();
                code = 1;
            }
            Runtime.getRuntime().halt(code);
        }, "ov-entity-model-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
