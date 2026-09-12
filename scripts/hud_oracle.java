// Ondes VOXEL — the HUD, asked of the real 1.20.1 client.
//
// Same method as scripts/chat_screen_oracle.java, whose mapping reader and
// input helpers this copies: the user's own vanilla 1.20.1 client joins a
// vanilla server as an operator and plays scripts/hud_scenes.txt — the very
// file our client plays with `ov_voxel --hud-script` against the same server.
// At each `shot` the running game is photographed twice through its own
// screenshot path, with the HUD and with it hidden (F1's `hideGui`), and asked
// for the numbers behind what it drew: health, absorption, food, air, frozen
// ticks, armour, the heart blink timers, the effects and their durations, the
// boss bars, the tab list, the titles, the F3 lines.
//
// Nothing here reads or translates Mojang code. The official mappings only
// *name* classes, fields and methods (CLAUDE.md § 1); every number printed is
// produced by the game's own bytecode at run time. What the HUD does with them
// — where a heart goes, when it blinks — is measured on the screenshots, as a
// black box.
//
// Driven by scripts/measure_hud.py.
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class HudOracle {

    // ── The mappings (same reader as the chat oracle) ────────────────────────

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
    static final String GUI = "net.minecraft.client.gui.Gui";
    static final String CHAT = "net.minecraft.client.gui.components.ChatComponent";
    static final String WINDOW = "com.mojang.blaze3d.platform.Window";
    static final String OPTIONS = "net.minecraft.client.Options";
    static final String ENTITY = "net.minecraft.world.entity.Entity";
    static final String LIVING = "net.minecraft.world.entity.LivingEntity";
    static final String PLAYER = "net.minecraft.world.entity.player.Player";
    static final String FOOD = "net.minecraft.world.food.FoodData";
    static final String EFFECT = "net.minecraft.world.effect.MobEffectInstance";
    static final String BOSS_OVERLAY = "net.minecraft.client.gui.components.BossHealthOverlay";
    static final String BOSS = "net.minecraft.world.BossEvent";
    static final String LERP_BOSS = "net.minecraft.client.gui.components.LerpingBossEvent";
    static final String TAB = "net.minecraft.client.gui.components.PlayerTabOverlay";
    static final String INFO = "net.minecraft.client.multiplayer.PlayerInfo";
    static final String LISTENER = "net.minecraft.client.multiplayer.ClientPacketListener";
    static final String DEBUG = "net.minecraft.client.gui.components.DebugScreenOverlay";
    static final String COMPONENT = "net.minecraft.network.chat.Component";
    static final String TEAM = "net.minecraft.world.scores.PlayerTeam";

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

    // ── Input, through the game's own handlers ──────────────────────────────

    static Method keyPress() throws Exception {
        return method("net.minecraft.client.KeyboardHandler", "keyPress",
                      "long", "int", "int", "int", "int");
    }

    static void keyAction(int glfwKey, int action) throws Exception {
        onRender(() -> {
            Object kb = get(mc, MC, "keyboardHandler");
            keyPress().invoke(kb, win, glfwKey, 0, action, 0);
            return null;
        });
    }

    static Object gui() throws Exception { return get(mc, MC, "gui"); }

    static int guiTicks() throws Exception {
        return onRender(() -> (Integer) get(gui(), GUI, "tickCount"));
    }

    static void waitTicks(int ticks) throws Exception {
        int target = guiTicks() + ticks;
        while (guiTicks() < target) pause(10);
    }

    static String quote(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    static String text(Object component) throws Exception {
        if (component == null) return "null";
        return quote((String) component.getClass().getMethod("getString").invoke(component));
    }

    static void grab(String name) throws Exception {
        onRender(() -> {
            Object target = call(mc, MC, "getMainRenderTarget", NONE);
            java.util.function.Consumer<Object> sink = c -> {};
            method("net.minecraft.client.Screenshot", "grab", "java.io.File", "java.lang.String",
                   "com.mojang.blaze3d.pipeline.RenderTarget", "java.util.function.Consumer")
                .invoke(null, gameDir, name, target, sink);
            return null;
        });
    }

    // ── What the game says ──────────────────────────────────────────────────

    static void line(String s) { out.println(s); }

    static void dumpStatics() {
        line("── statics");
        String[][] wanted = {
            {GUI, "NUM_HEARTS_PER_ROW"}, {GUI, "LINE_HEIGHT"}, {GUI, "HEART_SIZE"},
            {GUI, "HEART_SEPARATION"},
            {BOSS_OVERLAY, "BAR_WIDTH"}, {BOSS_OVERLAY, "BAR_HEIGHT"}, {BOSS_OVERLAY, "OVERLAY_OFFSET"},
            {LERP_BOSS, "LERP_MILLISECONDS"},
            {TAB, "MAX_ROWS_PER_COL"},
            {DEBUG, "COLOR_GREY"}, {DEBUG, "MARGIN_RIGHT"}, {DEBUG, "MARGIN_LEFT"}, {DEBUG, "MARGIN_TOP"},
            {EFFECT, "INFINITE_DURATION"},
        };
        for (String[] w : wanted) {
            try {
                line("  " + w[0].substring(w[0].lastIndexOf('.') + 1) + "." + w[1] + " = "
                     + get(null, w[0], w[1]));
            } catch (Throwable e) {
                line("  " + w[1] + " : " + e);
            }
        }
    }

    /// Everything the HUD reads, at the tick the last frame was drawn.
    static void dumpFacts(String label, int tickBefore, int tickAfter) throws Exception {
        onRender(() -> {
            Object g = gui();
            Object p = get(mc, MC, "player");
            line("── shot " + label + " (tick " + tickBefore + ".." + tickAfter + ", millis "
                 + call(null, "net.minecraft.Util", "getMillis", NONE) + ")");
            try {
                Object food = call(p, PLAYER, "getFoodData", NONE);
                line("  health " + call(p, LIVING, "getHealth", NONE)
                     + " max " + call(p, LIVING, "getMaxHealth", NONE)
                     + " absorption " + call(p, LIVING, "getAbsorptionAmount", NONE)
                     + " food " + call(food, FOOD, "getFoodLevel", NONE)
                     + " saturation " + call(food, FOOD, "getSaturationLevel", NONE)
                     + " air " + call(p, ENTITY, "getAirSupply", NONE)
                     + " frozen " + call(p, ENTITY, "getTicksFrozen", NONE)
                     + " armour " + call(p, LIVING, "getArmorValue", NONE));
                line("  xp level " + get(p, PLAYER, "experienceLevel")
                     + " progress " + get(p, PLAYER, "experienceProgress"));
                line("  gui tickCount " + get(g, GUI, "tickCount")
                     + " lastHealth " + get(g, GUI, "lastHealth")
                     + " displayHealth " + get(g, GUI, "displayHealth")
                     + " lastHealthTime " + get(g, GUI, "lastHealthTime")
                     + " healthBlinkTime " + get(g, GUI, "healthBlinkTime")
                     + " titleTime " + get(g, GUI, "titleTime")
                     + " times " + get(g, GUI, "titleFadeInTime") + " " + get(g, GUI, "titleStayTime")
                     + " " + get(g, GUI, "titleFadeOutTime")
                     + " overlayMessageTime " + get(g, GUI, "overlayMessageTime"));
                line("  title " + text(get(g, GUI, "title")) + " subtitle " + text(get(g, GUI, "subtitle"))
                     + " overlay " + text(get(g, GUI, "overlayMessageString")));
                Object vehicle = call(p, ENTITY, "getVehicle", NONE);
                if (vehicle != null) {
                    String v = "  vehicle " + vehicle.getClass().getSimpleName();
                    if (cls(LIVING).isInstance(vehicle)) {
                        v += " health " + call(vehicle, LIVING, "getHealth", NONE)
                           + " max " + call(vehicle, LIVING, "getMaxHealth", NONE);
                    }
                    line(v);
                }
            } catch (Throwable e) {
                line("  player : " + e);
            }
            try {
                Collection<?> effects = (Collection<?>) call(p, LIVING, "getActiveEffects", NONE);
                for (Object e : effects) {
                    line("  effect " + call(e, EFFECT, "getDescriptionId", NONE)
                         + " amplifier " + call(e, EFFECT, "getAmplifier", NONE)
                         + " duration " + call(e, EFFECT, "getDuration", NONE)
                         + " ambient " + call(e, EFFECT, "isAmbient", NONE)
                         + " visible " + call(e, EFFECT, "isVisible", NONE)
                         + " icon " + call(e, EFFECT, "showIcon", NONE)
                         + " beneficial " + isBeneficial(call(e, EFFECT, "getEffect", NONE)));
                }
            } catch (Throwable e) {
                line("  effects : " + e);
            }
            try {
                Map<?, ?> events = (Map<?, ?>) get(get(g, GUI, "bossOverlay"), BOSS_OVERLAY, "events");
                for (Object b : events.values()) {
                    line("  boss " + text(call(b, BOSS, "getName", NONE))
                         + " progress " + call(b, LERP_BOSS, "getProgress", NONE)
                         + " target " + get(b, LERP_BOSS, "targetPercent")
                         + " colour " + ((Enum<?>) call(b, BOSS, "getColor", NONE)).ordinal()
                         + " overlay " + ((Enum<?>) call(b, BOSS, "getOverlay", NONE)).ordinal());
                }
            } catch (Throwable e) {
                line("  bosses : " + e);
            }
            try {
                Object tab = get(g, GUI, "tabList");
                line("  tab visible " + get(tab, TAB, "visible") + " header " + text(get(tab, TAB, "header"))
                     + " footer " + text(get(tab, TAB, "footer")));
                Object conn = call(mc, MC, "getConnection", NONE);
                for (Object info : (Collection<?>) call(conn, LISTENER, "getOnlinePlayers", NONE)) {
                    Object profile = call(info, INFO, "getProfile", NONE);
                    Object team = call(info, INFO, "getTeam", NONE);
                    String t = team == null ? "none"
                        : ((Enum<?>) call(team, TEAM, "getColor", NONE)).name()
                          + " prefix " + text(call(team, TEAM, "getPlayerPrefix", NONE));
                    line("  player " + profile.getClass().getMethod("getName").invoke(profile)
                         + " latency " + call(info, INFO, "getLatency", NONE)
                         + " mode " + ((Enum<?>) call(info, INFO, "getGameMode", NONE)).ordinal()
                         + " display " + text(call(info, INFO, "getTabListDisplayName", NONE))
                         + " shown " + text(call(tab, TAB, "getNameForDisplay", new String[]{INFO}, info))
                         + " team " + t);
                }
            } catch (Throwable e) {
                line("  tab : " + e);
            }
            try {
                Object options = get(mc, MC, "options");
                boolean f3 = (Boolean) get(options, OPTIONS, "renderDebug");
                if (f3) {
                    Object debug = get(g, GUI, "debugScreen");
                    for (Object s : (List<?>) call(debug, DEBUG, "getGameInformation", NONE)) {
                        line("  f3 left " + quote(String.valueOf(s)));
                    }
                    for (Object s : (List<?>) call(debug, DEBUG, "getSystemInformation", NONE)) {
                        line("  f3 right " + quote(String.valueOf(s)));
                    }
                }
            } catch (Throwable e) {
                line("  f3 : " + e);
            }
            return null;
        });
    }

    static Object isBeneficial(Object effect) {
        try {
            return call(effect, "net.minecraft.world.effect.MobEffect", "isBeneficial", NONE);
        } catch (Throwable e) {
            return "?";
        }
    }

    // ── The scenes ──────────────────────────────────────────────────────────

    static void shot(String name) throws Exception {
        pause(120);  // at least one frame after the last change
        int before = guiTicks();
        grab(name + ".png");
        int after = guiTicks();
        dumpFacts(name, before, after);
        onRender(() -> { set(get(mc, MC, "options"), OPTIONS, "hideGui", true); return null; });
        pause(150);
        grab(name + "-nohud.png");
        onRender(() -> { set(get(mc, MC, "options"), OPTIONS, "hideGui", false); return null; });
        pause(100);
    }

    static void look(float yaw, float pitch) throws Exception {
        onRender(() -> {
            Object p = get(mc, MC, "player");
            call(p, ENTITY, "setYRot", new String[]{"float"}, yaw);
            call(p, ENTITY, "setXRot", new String[]{"float"}, pitch);
            return null;
        });
    }

    static void command(String command) throws Exception {
        onRender(() -> {
            Object conn = call(mc, MC, "getConnection", NONE);
            call(conn, LISTENER, "sendCommand", new String[]{"java.lang.String"}, command);
            return null;
        });
        line("cmd " + command);
        pause(60);
    }

    static Object parse(String json) throws Exception {
        return call(null, COMPONENT + "$Serializer", "fromJson", new String[]{"java.lang.String"}, json);
    }

    static void play(List<String> scenes) throws Exception {
        for (String raw : scenes) {
            String s = raw.strip();
            if (s.isEmpty() || s.startsWith("#")) continue;
            int sp = s.indexOf(' ');
            String verb = sp < 0 ? s : s.substring(0, sp);
            String rest = sp < 0 ? "" : s.substring(sp + 1).strip();
            switch (verb) {
                case "cmd" -> command(rest);
                case "wait" -> waitTicks(Integer.parseInt(rest));
                case "shot" -> shot(rest);
                case "look" -> {
                    String[] a = rest.split("\\s+");
                    look(Float.parseFloat(a[0]), Float.parseFloat(a[1]));
                }
                case "key" -> {
                    String[] a = rest.split("\\s+");
                    int k = switch (a[0]) {
                        case "tab" -> 258;
                        case "f3" -> 292;
                        case "e" -> 69;
                        case "esc" -> 256;
                        default -> -1;
                    };
                    if (a[1].equals("press") || a[1].equals("tap")) keyAction(k, 1);
                    if (a[1].equals("tap")) pause(60);
                    if (a[1].equals("release") || a[1].equals("tap")) keyAction(k, 0);
                    pause(60);
                }
                case "tabfoot" -> {
                    String[] a = rest.split("\\s*\\|\\s*", 2);
                    onRender(() -> {
                        Object tab = get(gui(), GUI, "tabList");
                        // Vanilla's own handler for Set Tab List Header And
                        // Footer turns an empty component into no header.
                        String h = a[0].equals("{\"text\":\"\"}") ? null : a[0];
                        String f = a[1].equals("{\"text\":\"\"}") ? null : a[1];
                        call(tab, TAB, "setHeader", new String[]{COMPONENT}, h == null ? null : parse(h));
                        call(tab, TAB, "setFooter", new String[]{COMPONENT}, f == null ? null : parse(f));
                        return null;
                    });
                    line("tabfoot " + rest);
                }
                case "use" -> {
                    // The right button, pressed and let go, through the game's
                    // own mouse handler: a right click on what is aimed at.
                    for (int action : new int[]{1, 0}) {
                        onRender(() -> {
                            Object mh = get(mc, MC, "mouseHandler");
                            call(mh, "net.minecraft.client.MouseHandler", "onPress",
                                 new String[]{"long", "int", "int", "int"}, win, 1, action, 0);
                            return null;
                        });
                        pause(60);
                    }
                    line("use");
                }
                default -> line("unknown scene line " + quote(s));
            }
        }
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
        pause(8000);
        gameDir = (File) get(mc, MC, "gameDirectory");
        win = onRender(() -> (Long) call(call(mc, MC, "getWindow", NONE), WINDOW, "getWindow", NONE));
        line("in world; window " + win);
        // The real keyboard and mouse are unplugged: only the scenes move it.
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
        onRender(() -> {
            Object chat = call(gui(), GUI, "getChat", NONE);
            call(chat, CHAT, "clearMessages", new String[]{"boolean"}, false);
            return null;
        });
        dumpStatics();
        play(Files.readAllLines(scenes, StandardCharsets.UTF_8));
        line("done");
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        File outDir = new File(args[1]);
        outDir.mkdirs();
        Path scenes = Paths.get(args[2]);
        out = new PrintStream(new FileOutputStream(new File(outDir, "facts.txt")), true, "UTF-8");
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
        }, "ov-hud-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
