// The real 1.20.1 client drawing entities: screenshots, and the vertices the
// game itself emits for each one.
//
// Same technique as scripts/render_parity_oracle.java and
// scripts/entity_model_oracle.java: the user's own client runs, the official
// mappings are used only to *name* what is reflected on, and every number is
// produced by the game's bytecode while it runs. Nothing here is decompiled
// code.
//
// Two measurements per scene:
//   • a screenshot through the game's own capture path, before and after the
//     entity is summoned, so scripts/compare_entity_render.py can cut the
//     entity's silhouette out by difference;
//   • the vertices: each entity near the scene is rendered once more by the
//     game's own EntityRenderDispatcher into a MultiBufferSource that records
//     instead of drawing — a java.lang.reflect.Proxy over the game's
//     VertexConsumer interface. What it records is exactly what the game
//     would have sent to the GPU: positions relative to the entity, texture
//     coordinates, colours, overlay and light, per render type.
//
// Driven by scripts/measure_entity_render.py.
//
//   java -XstartOnFirstThread -cp <out>:<client classpath> ov.EntityRenderOracle \
//        <client mappings> <out dir> <directives file> <Minecraft arguments...>
//
// The directives, one a line:
//   cmd <command without slash>
//   wait <milliseconds>
//   shot <name> <x> <y> <z> <yaw> <pitch>      teleport, settle, capture
//   dump <name> <x> <y> <z> <radius>           the vertices of the entities there
//   facts <label>                              light directions; riding heights
//   flap <ticks>                               a dragon's wing beat, tick by tick
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class EntityRenderOracle {

    // ── The mappings (same reader as the other oracles) ─────────────────────

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

    // ── The game ────────────────────────────────────────────────────────────

    static final String MC = "net.minecraft.client.Minecraft";
    static final String OPTIONS = "net.minecraft.client.Options";
    static final String WINDOW = "com.mojang.blaze3d.platform.Window";
    static final String ENTITY = "net.minecraft.world.entity.Entity";
    static final String LIVING = "net.minecraft.world.entity.LivingEntity";
    static final String LEVEL = "net.minecraft.client.multiplayer.ClientLevel";
    static final String LEVEL_RENDERER = "net.minecraft.client.renderer.LevelRenderer";
    static final String DISPATCHER = "net.minecraft.client.renderer.entity.EntityRenderDispatcher";
    static final String POSE_STACK = "com.mojang.blaze3d.vertex.PoseStack";
    static final String BUFFERS = "net.minecraft.client.renderer.MultiBufferSource";
    static final String CONSUMER = "com.mojang.blaze3d.vertex.VertexConsumer";
    static final String RENDER_TYPE = "net.minecraft.client.renderer.RenderType";
    static final String REGISTRIES = "net.minecraft.core.registries.BuiltInRegistries";
    static final String REGISTRY = "net.minecraft.core.Registry";
    static final String CAMERA = "net.minecraft.client.Camera";
    static final String GAME_RENDERER = "net.minecraft.client.renderer.GameRenderer";
    static final String VEC3 = "net.minecraft.world.phys.Vec3";

    static Object mc;
    static long win;
    static PrintStream out;
    static File gameDir;
    static Path outDir;

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

    static void pause(long ms) throws InterruptedException { Thread.sleep(ms); }

    static void send(String text) throws Exception {
        onRender(() -> {
            Object connection = call(mc, MC, "getConnection", NONE);
            call(connection, "net.minecraft.client.multiplayer.ClientPacketListener", "sendCommand",
                 new String[]{"java.lang.String"}, text);
            return null;
        });
        out.println("cmd " + text);
        pause(120);
    }

    static double d(Object target, String official, String name, String[] types, Object... args)
            throws Exception {
        return ((Number) call(target, official, name, types, args)).doubleValue();
    }

    // ── A MultiBufferSource that records ────────────────────────────────────

    /// One render type's vertices, as the game emitted them.
    static final class Recorded {
        final String renderType;
        final List<float[]> vertices = new ArrayList<>();
        float[] pending = new float[16];

        Recorded(String renderType) { this.renderType = renderType; }
    }

    static Object recordingBuffers(Map<String, Recorded> sink) throws Exception {
        Class<?> consumerClass = cls(CONSUMER);
        Class<?> buffersClass = cls(BUFFERS);
        // The game's VertexConsumer methods this recorder answers, by their
        // obfuscated name and arity. Every other method, default ones included,
        // falls through to its own default implementation.
        Method vertex3 = method(CONSUMER, "vertex", "double", "double", "double");
        Method colour4 = method(CONSUMER, "color", "int", "int", "int", "int");
        Method uv = method(CONSUMER, "uv", "float", "float");
        Method overlay = method(CONSUMER, "overlayCoords", "int", "int");
        Method light = method(CONSUMER, "uv2", "int", "int");
        Method normal = method(CONSUMER, "normal", "float", "float", "float");
        Method end = method(CONSUMER, "endVertex");
        Method bulk = method(CONSUMER, "vertex", "float", "float", "float", "float", "float", "float",
                             "float", "float", "float", "int", "int", "float", "float", "float");

        return Proxy.newProxyInstance(buffersClass.getClassLoader(), new Class<?>[]{buffersClass},
            (bufferProxy, bufferMethod, bufferArgs) -> {
                if (bufferArgs == null || bufferArgs.length != 1) {
                    return bufferMethod.isDefault()
                        ? InvocationHandler.invokeDefault(bufferProxy, bufferMethod, bufferArgs) : null;
                }
                String key = String.valueOf(bufferArgs[0]);
                Recorded recorded = sink.computeIfAbsent(key, Recorded::new);
                return Proxy.newProxyInstance(consumerClass.getClassLoader(), new Class<?>[]{consumerClass},
                    (proxy, m, a) -> {
                        String name = m.getName();
                        int n = m.getParameterCount();
                        if (sameAs(m, bulk)) {
                            float[] v = new float[16];
                            for (int i = 0; i < 9; i++) v[i] = (Float) a[i];
                            int ov = (Integer) a[9];
                            int li = (Integer) a[10];
                            v[9] = ov & 0xFFFF; v[10] = ov >>> 16;
                            v[11] = li & 0xFFFF; v[12] = li >>> 16;
                            v[13] = (Float) a[11]; v[14] = (Float) a[12]; v[15] = (Float) a[13];
                            recorded.vertices.add(v);
                            return null;
                        }
                        if (sameAs(m, vertex3)) {
                            recorded.pending = new float[16];
                            recorded.pending[3] = recorded.pending[4] = recorded.pending[5] = recorded.pending[6] = 1.0F;
                            recorded.pending[0] = ((Double) a[0]).floatValue();
                            recorded.pending[1] = ((Double) a[1]).floatValue();
                            recorded.pending[2] = ((Double) a[2]).floatValue();
                            return proxy;
                        }
                        if (sameAs(m, colour4)) {
                            for (int i = 0; i < 4; i++) recorded.pending[3 + i] = ((Integer) a[i]) / 255.0F;
                            return proxy;
                        }
                        if (sameAs(m, uv)) {
                            recorded.pending[7] = (Float) a[0];
                            recorded.pending[8] = (Float) a[1];
                            return proxy;
                        }
                        if (sameAs(m, overlay)) {
                            recorded.pending[9] = (Integer) a[0];
                            recorded.pending[10] = (Integer) a[1];
                            return proxy;
                        }
                        if (sameAs(m, light)) {
                            recorded.pending[11] = (Integer) a[0];
                            recorded.pending[12] = (Integer) a[1];
                            return proxy;
                        }
                        if (sameAs(m, normal)) {
                            recorded.pending[13] = (Float) a[0];
                            recorded.pending[14] = (Float) a[1];
                            recorded.pending[15] = (Float) a[2];
                            return proxy;
                        }
                        if (sameAs(m, end)) {
                            recorded.vertices.add(recorded.pending);
                            recorded.pending = new float[16];
                            return null;
                        }
                        if (m.isDefault()) {
                            return InvocationHandler.invokeDefault(proxy, m, a);
                        }
                        // defaultColor, unsetDefaultColor: nothing to record.
                        return m.getReturnType() == void.class ? null : proxy;
                    });
            });
    }

    static boolean sameAs(Method invoked, Method mapped) {
        return invoked.getName().equals(mapped.getName())
            && Arrays.equals(invoked.getParameterTypes(), mapped.getParameterTypes());
    }

    // ── Directives ──────────────────────────────────────────────────────────

    static void hover() throws Exception {
        onRender(() -> {
            Object player = get(mc, MC, "player");
            Object abilities = call(player, "net.minecraft.world.entity.player.Player", "getAbilities", NONE);
            field("net.minecraft.world.entity.player.Abilities", "flying").set(abilities, true);
            call(player, ENTITY, "setDeltaMovement", new String[]{"double", "double", "double"}, 0.0, 0.0, 0.0);
            return null;
        });
    }

    static void shot(String[] p) throws Exception {
        String name = p[1];
        // Standing on the ground, not hovering: a vanilla server kicks a
        // survival player whose client says it flies.
        send(String.format(Locale.ROOT, "tp @s %s %s %s %s %s", p[2], p[3], p[4], p[5], p[6]));
        // Settle: every chunk in range compiled, then a second and a half more
        // for the entity to arrive and the frame to be stable.
        for (int i = 0; i < 100; i++) {
            boolean all = onRender(() -> (Boolean) call(get(mc, MC, "levelRenderer"), LEVEL_RENDERER,
                                                       "hasRenderedAllChunks", NONE));
            if (all && i > 4) break;
            pause(200);
        }
        pause(1500);
        onRender(() -> {
            Object target = call(mc, MC, "getMainRenderTarget", NONE);
            java.util.function.Consumer<Object> sinkMessage = c -> {};
            method("net.minecraft.client.Screenshot", "grab", "java.io.File", "java.lang.String",
                   "com.mojang.blaze3d.pipeline.RenderTarget", "java.util.function.Consumer")
                .invoke(null, gameDir, name + ".png", target, sinkMessage);
            return null;
        });
        out.println("shot " + name);
        pause(400);
    }

    static void dump(String[] p) throws Exception {
        String name = p[1];
        double cx = Double.parseDouble(p[2]), cy = Double.parseDouble(p[3]), cz = Double.parseDouble(p[4]);
        double radius = Double.parseDouble(p[5]);
        StringBuilder states = new StringBuilder();
        String json = onRender(() -> {
            Object level = get(mc, MC, "level");
            Object dispatcher = call(mc, MC, "getEntityRenderDispatcher", NONE);
            Object typeRegistry = get(null, REGISTRIES, "ENTITY_TYPE");
            // The camera the sprites, the names and the fire were turned to.
            Object camera = call(get(mc, MC, "gameRenderer"), GAME_RENDERER, "getMainCamera", NONE);
            states.append(String.format(Locale.ROOT, "camera %.4f %.4f\n",
                (Float) call(camera, CAMERA, "getYRot", NONE), (Float) call(camera, CAMERA, "getXRot", NONE)));
            StringBuilder b = new StringBuilder("[");
            boolean first = true;
            for (Object entity : (Iterable<?>) call(level, LEVEL, "entitiesForRendering", NONE)) {
                if (entity == get(mc, MC, "player")) continue;
                double x = d(entity, ENTITY, "getX", NONE);
                double y = d(entity, ENTITY, "getY", NONE);
                double z = d(entity, ENTITY, "getZ", NONE);
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz) > radius * radius) continue;
                Object type = call(entity, ENTITY, "getType", NONE);
                Object key = call(typeRegistry, REGISTRY, "getKey", new String[]{"java.lang.Object"}, type);
                float yaw = (Float) call(entity, ENTITY, "getYRot", NONE);
                float bodyYaw = cls(LIVING).isInstance(entity)
                    ? (Float) get(entity, LIVING, "yBodyRot") : yaw;
                float headYaw = (Float) call(entity, ENTITY, "getYHeadRot", NONE);
                float pitch = (Float) call(entity, ENTITY, "getXRot", NONE);
                Map<String, Recorded> sink = new LinkedHashMap<>();
                Object buffers = recordingBuffers(sink);
                Object stack = cls(POSE_STACK).getConstructor().newInstance();
                // At the entity's own position, a partial tick of one: the
                // vertices come out relative to the entity, in world axes.
                call(dispatcher, DISPATCHER, "render",
                     new String[]{ENTITY, "double", "double", "double", "float", "float", POSE_STACK, BUFFERS, "int"},
                     entity, 0.0, 0.0, 0.0, yaw, 1.0F, stack, buffers, 0xF000F0);
                b.append(first ? "\n" : ",\n");
                first = false;
                b.append(String.format(Locale.ROOT,
                    "{\"type\": \"%s\", \"id\": %d, \"position\": [%.5f, %.5f, %.5f], \"yaw\": %.3f, "
                        + "\"body_yaw\": %.3f, \"head_yaw\": %.3f, \"pitch\": %.3f, \"layers\": [",
                    key, (Integer) call(entity, ENTITY, "getId", NONE), x, y, z, yaw, bodyYaw, headYaw, pitch));
                // The entity's synced data, as the network would have carried
                // it (the non-default values), one line a entity: what the
                // offline check feeds our client (`ov_voxel --entity-check`).
                states.append(String.format(Locale.ROOT, "entity %s %d %.5f %.5f %.5f %.3f %.3f %.3f %.3f",
                    key, (Integer) call(entity, ENTITY, "getId", NONE), x, y, z, yaw, bodyYaw, headYaw, pitch));
                // Its age in ticks: what every time-driven animation reads (a
                // blaze's rods, a ghast's tentacles, a crystal's spin).
                states.append(" t=").append(get(entity, ENTITY, "tickCount"));
                // A crystal spins and bobs on a clock of its own, which starts
                // at a random value: that clock, not its age.
                String crystal = "net.minecraft.world.entity.boss.enderdragon.EndCrystal";
                if (cls(crystal).isInstance(entity)) {
                    states.append(" ct=").append(get(entity, crystal, "time"));
                }
                // What it wears and holds: not synced data but its own packet
                // (Set Equipment), so read from the slots, one token each.
                if (cls(LIVING).isInstance(entity)) {
                    Object[] slots = cls("net.minecraft.world.entity.EquipmentSlot").getEnumConstants();
                    Object itemRegistry = get(null, REGISTRIES, "ITEM");
                    for (int s = 0; s < slots.length; s++) {
                        Object worn = call(entity, LIVING, "getItemBySlot",
                                           new String[]{"net.minecraft.world.entity.EquipmentSlot"}, slots[s]);
                        if ((Boolean) call(worn, "net.minecraft.world.item.ItemStack", "isEmpty", NONE)) continue;
                        Object item = call(worn, "net.minecraft.world.item.ItemStack", "getItem", NONE);
                        states.append(" e").append(s).append('=')
                              .append(call(itemRegistry, REGISTRY, "getKey", new String[]{"java.lang.Object"}, item));
                    }
                }
                Object data = call(entity, ENTITY, "getEntityData", NONE);
                Object values = call(data, "net.minecraft.network.syncher.SynchedEntityData", "getNonDefaultValues", NONE);
                if (values != null) {
                    for (Object value : (List<?>) values) {
                        int index = (Integer) call(value, "net.minecraft.network.syncher.SynchedEntityData$DataValue", "id", NONE);
                        Object v = call(value, "net.minecraft.network.syncher.SynchedEntityData$DataValue", "value", NONE);
                        states.append(" m").append(index).append('=').append(encodeValue(v));
                    }
                }
                states.append('\n');
                boolean firstLayer = true;
                for (Recorded r : sink.values()) {
                    b.append(firstLayer ? "" : ", ");
                    firstLayer = false;
                    b.append("{\"render_type\": \"").append(r.renderType.replace("\"", "'")).append("\", \"vertices\": [");
                    for (int i = 0; i < r.vertices.size(); i++) {
                        float[] v = r.vertices.get(i);
                        b.append(i == 0 ? "" : ", ").append('[');
                        for (int k = 0; k < v.length; k++) {
                            b.append(k == 0 ? "" : ",").append(String.format(Locale.ROOT, "%.5f", v[k]));
                        }
                        b.append(']');
                    }
                    b.append("]}");
                }
                b.append("]}");
            }
            return b.append("\n]\n").toString();
        });
        Files.writeString(outDir.resolve(name + ".json"), json, StandardCharsets.UTF_8);
        Files.writeString(outDir.resolve(name + ".state"), states.toString(), StandardCharsets.UTF_8);
        out.println("dump " + name);
    }

    /// One data value, in a form a line parser reads: a number, `v:type,profession,level`
    /// for villager data, `b:id` for a block state, `r:x,y,z` for rotations,
    /// `t:text` for a name (spaces as underscores), `1`/`0` for an optional's
    /// presence, `?` for what the offline check does not read.
    static String encodeValue(Object v) throws Exception {
        if (v instanceof Boolean flag) return flag ? "1" : "0";
        if (v instanceof Number number) return String.valueOf(number);
        if (v instanceof Optional<?> optional) {
            if (optional.isEmpty()) return "0";
            Object inner = optional.get();
            if (cls("net.minecraft.network.chat.Component").isInstance(inner)) {
                String text = (String) call(inner, "net.minecraft.network.chat.Component", "getString", NONE);
                return "t:" + text.replace(' ', '_');
            }
            if (cls("net.minecraft.world.level.block.state.BlockState").isInstance(inner)) {
                return "b:" + call(null, "net.minecraft.world.level.block.Block", "getId",
                                   new String[]{"net.minecraft.world.level.block.state.BlockState"}, inner);
            }
            if (cls("net.minecraft.core.BlockPos").isInstance(inner)) {
                // A crystal's beam target: the packed position, as the wire has it.
                return "p:" + call(inner, "net.minecraft.core.BlockPos", "asLong", NONE);
            }
            return "1";
        }
        if (cls("net.minecraft.network.chat.Component").isInstance(v)) {
            return "t:" + ((String) call(v, "net.minecraft.network.chat.Component", "getString", NONE)).replace(' ', '_');
        }
        if (cls("net.minecraft.world.level.block.state.BlockState").isInstance(v)) {
            return "b:" + call(null, "net.minecraft.world.level.block.Block", "getId",
                               new String[]{"net.minecraft.world.level.block.state.BlockState"}, v);
        }
        String villager = "net.minecraft.world.entity.npc.VillagerData";
        if (cls(villager).isInstance(v)) {
            Object type = call(v, villager, "getType", NONE);
            Object profession = call(v, villager, "getProfession", NONE);
            int level = (Integer) call(v, villager, "getLevel", NONE);
            int t = (Integer) call(get(null, REGISTRIES, "VILLAGER_TYPE"), REGISTRY, "getId", new String[]{"java.lang.Object"}, type);
            int p = (Integer) call(get(null, REGISTRIES, "VILLAGER_PROFESSION"), REGISTRY, "getId", new String[]{"java.lang.Object"}, profession);
            return "v:" + t + "," + p + "," + level;
        }
        String rotations = "net.minecraft.core.Rotations";
        if (cls(rotations).isInstance(v)) {
            return String.format(Locale.ROOT, "r:%.3f,%.3f,%.3f", (Float) call(v, rotations, "getX", NONE),
                                 (Float) call(v, rotations, "getY", NONE), (Float) call(v, rotations, "getZ", NONE));
        }
        String cat = "net.minecraft.world.entity.animal.CatVariant";
        if (cls(cat).isInstance(v)) {
            return String.valueOf(call(get(null, REGISTRIES, "CAT_VARIANT"), REGISTRY, "getId", new String[]{"java.lang.Object"}, v));
        }
        return "?";
    }

    static void facts(String label) throws Exception {
        onRender(() -> {
            out.println("── facts " + label);
            Class<?> rs = Class.forName("com.mojang.blaze3d.systems.RenderSystem");
            for (Field f : rs.getDeclaredFields()) {
                if (f.getType().isArray() && f.getType().getComponentType().getName().equals("org.joml.Vector3f")) {
                    f.setAccessible(true);
                    Object[] lights = (Object[]) f.get(null);
                    out.println("light directions (" + f.getName() + ") " + Arrays.toString(lights));
                }
            }
            Object player = get(mc, MC, "player");
            Object vehicle = call(player, ENTITY, "getVehicle", NONE);
            double py = d(player, ENTITY, "getY", NONE);
            Object gr = get(mc, MC, "gameRenderer");
            Object camera = call(gr, GAME_RENDERER, "getMainCamera", NONE);
            Object pos = call(camera, CAMERA, "getPosition", NONE);
            double camY = ((Number) get(pos, VEC3, "y")).doubleValue();
            out.println(String.format(Locale.ROOT, "player y %.6f camera y %.6f eye %.6f", py, camY,
                        d(player, ENTITY, "getEyeHeight", NONE)));
            // The light directions above are the game's world directions
            // turned into view space by this camera: its angles undo that.
            out.println(String.format(Locale.ROOT, "camera yaw %.4f pitch %.4f",
                        (Float) call(camera, CAMERA, "getYRot", NONE), (Float) call(camera, CAMERA, "getXRot", NONE)));
            if (vehicle != null) {
                Object type = call(vehicle, ENTITY, "getType", NONE);
                double vy = d(vehicle, ENTITY, "getY", NONE);
                out.println(String.format(Locale.ROOT, "riding %s vehicle y %.6f player-vehicle %.6f camera-vehicle %.6f",
                            call(get(null, REGISTRIES, "ENTITY_TYPE"), REGISTRY, "getKey", new String[]{"java.lang.Object"}, type),
                            vy, py - vy, camY - vy));
            }
            return null;
        });
    }

    static void flap(int ticks) throws Exception {
        for (int t = 0; t < ticks; t++) {
            String row = onRender(() -> {
                Object level = get(mc, MC, "level");
                for (Object entity : (Iterable<?>) call(level, LEVEL, "entitiesForRendering", NONE)) {
                    Class<?> dragon = cls("net.minecraft.world.entity.boss.enderdragon.EnderDragon");
                    if (!dragon.isInstance(entity)) continue;
                    return String.format(Locale.ROOT, "flap %.6f oflap %.6f y %.5f yrot %.4f",
                        (Float) get(entity, "net.minecraft.world.entity.boss.enderdragon.EnderDragon", "flapTime"),
                        (Float) get(entity, "net.minecraft.world.entity.boss.enderdragon.EnderDragon", "oFlapTime"),
                        d(entity, ENTITY, "getY", NONE), (Float) call(entity, ENTITY, "getYRot", NONE));
                }
                return "flap none";
            });
            out.println(row);
            pause(50);
        }
    }

    static void drive(Path directives) throws Exception {
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
            field(OPTIONS, "hideGui").set(get(mc, MC, "options"), true);
            return null;
        });

        for (String line : Files.readAllLines(directives, StandardCharsets.UTF_8)) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            try {
                switch (p[0]) {
                    case "cmd" -> send(line.substring(4).trim());
                    case "wait" -> pause(Long.parseLong(p[1]));
                    case "shot" -> shot(p);
                    case "dump" -> dump(p);
                    case "facts" -> facts(p.length > 1 ? p[1] : "");
                    case "flap" -> flap(Integer.parseInt(p[1]));
                    default -> out.println("unknown directive: " + line);
                }
            } catch (Throwable e) {
                out.println("failed: " + line + " -> " + e);
                Throwable cause = e.getCause();
                if (cause != null) out.println("   cause: " + cause);
                e.printStackTrace(out);
            }
        }
        out.println("done");
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        outDir = Paths.get(args[1]);
        Files.createDirectories(outDir);
        out = new PrintStream(new FileOutputStream(outDir.resolve("facts.txt").toFile()), true, "UTF-8");
        Path directives = Paths.get(args[2]);
        String[] mcArgs = Arrays.copyOfRange(args, 3, args.length);
        Thread driver = new Thread(() -> {
            int code = 0;
            try {
                drive(directives);
            } catch (Throwable e) {
                e.printStackTrace(out);
                code = 1;
            }
            out.flush();
            Runtime.getRuntime().halt(code);
        }, "ov-entity-render-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
