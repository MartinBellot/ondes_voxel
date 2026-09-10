// Ondes VOXEL — the game's menus, asked of the real 1.20.1 client.
//
// Same method as scripts/chat_screen_oracle.java and
// scripts/creative_screen_oracle.java, whose mapping reader and input helpers
// this copies: the user's own vanilla 1.20.1 client is started on its title
// screen and driven with synthetic GLFW events injected into the game's own
// keyboard and mouse handlers. Each screen it reaches is walked widget by
// widget — class, position, size, message, and the *translation key* of the
// message with its arguments — and captured through the game's own
// screenshot path.
//
// The walk: title → options (video, sounds, controls, key binds, mouse,
// language) → singleplayer → create world (three tabs, a name, cheats on, a
// seed) → in the world: F3, the pause menu (and whether the integrated server
// really stops), the in-game options, a death by /kill, respawn → options.txt
// written with known values → save and quit → the world list → multiplayer →
// direct connection.
//
// Nothing here reads or translates Mojang code. The official mappings only
// *name* classes, fields and methods (CLAUDE.md § 1); every number printed is
// produced by the game's own bytecode at run time.
//
// Driven by scripts/measure_screens.py.
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.function.Predicate;

public final class ScreensOracle {

    // ── The mappings (same reader as the chat oracle) ────────────────────────

    static final Map<String, String> CLASSES = new HashMap<>();
    static final Map<String, String> OFFICIAL = new HashMap<>();
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
                    OFFICIAL.put(obf, current);
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

    static String officialName(Class<?> c) {
        String name = OFFICIAL.get(c.getName());
        if (name == null) return c.getName();
        return name.substring(name.lastIndexOf('.') + 1);
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
    static final String SCREEN = "net.minecraft.client.gui.screens.Screen";
    static final String WIDGET = "net.minecraft.client.gui.components.AbstractWidget";
    static final String CONTAINER = "net.minecraft.client.gui.components.events.ContainerEventHandler";
    static final String LIST = "net.minecraft.client.gui.components.AbstractSelectionList";
    static final String EDIT = "net.minecraft.client.gui.components.EditBox";
    static final String COMPONENT = "net.minecraft.network.chat.Component";
    static final String TRANSLATABLE = "net.minecraft.network.chat.contents.TranslatableContents";
    static final String LITERAL = "net.minecraft.network.chat.contents.LiteralContents";
    static final String WINDOW = "com.mojang.blaze3d.platform.Window";
    static final String OPTIONS = "net.minecraft.client.Options";
    static final String OPTION = "net.minecraft.client.OptionInstance";

    static Object mc;
    static long win;
    static PrintStream out;
    static File gameDir;
    static File outDir;

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

    // ── Input, through the game's own handlers ──────────────────────────────

    static void key(int glfwKey) throws Exception {
        onRender(() -> {
            Object kb = get(mc, MC, "keyboardHandler");
            Method press = method("net.minecraft.client.KeyboardHandler", "keyPress",
                                  "long", "int", "int", "int", "int");
            press.invoke(kb, win, glfwKey, 0, 1, 0);
            press.invoke(kb, win, glfwKey, 0, 0, 0);
            return null;
        });
        pause(600);
    }

    static double ratio() throws Exception {
        return onRender(() -> {
            Object w = call(mc, MC, "getWindow", NONE);
            int screenW = (Integer) call(w, WINDOW, "getScreenWidth", NONE);
            int guiW = (Integer) call(w, WINDOW, "getGuiScaledWidth", NONE);
            return (double) screenW / guiW;
        });
    }

    static void move(double gx, double gy) throws Exception {
        double r = ratio();
        onRender(() -> {
            Object mh = get(mc, MC, "mouseHandler");
            call(mh, "net.minecraft.client.MouseHandler", "onMove",
                 new String[]{"long", "double", "double"}, win, gx * r, gy * r);
            return null;
        });
        pause(150);
    }

    static void click(double gx, double gy) throws Exception {
        move(gx, gy);
        onRender(() -> {
            Object mh = get(mc, MC, "mouseHandler");
            Method press = method("net.minecraft.client.MouseHandler", "onPress",
                                  "long", "int", "int", "int");
            press.invoke(mh, win, 0, 1, 0);
            press.invoke(mh, win, 0, 0, 0);
            return null;
        });
        pause(900);
    }

    static void shot(String name) throws Exception {
        pause(400);
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

    // ── What the game says ──────────────────────────────────────────────────

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
        return (String) component.getClass().getMethod("getString").invoke(component);
    }

    /// A component as its translation key and arguments, recursively:
    /// `options.generic_value(options.fov, options.fov.min)`. The keys are what
    /// our client looks up in the same language file.
    static String keyOf(Object component) throws Exception {
        if (component == null) return "null";
        if (!cls(COMPONENT).isInstance(component)) return quote(String.valueOf(component));
        Object contents = call(component, COMPONENT, "getContents", NONE);
        StringBuilder b = new StringBuilder();
        if (cls(TRANSLATABLE).isInstance(contents)) {
            b.append(call(contents, TRANSLATABLE, "getKey", NONE));
            Object[] args = (Object[]) call(contents, TRANSLATABLE, "getArgs", NONE);
            if (args.length > 0) {
                b.append('(');
                for (int i = 0; i < args.length; i++) {
                    if (i > 0) b.append(", ");
                    b.append(keyOf(args[i]));
                }
                b.append(')');
            }
        } else if (cls(LITERAL).isInstance(contents)) {
            b.append(quote(text(component)));
        } else {
            b.append(officialName(contents.getClass())).append(':').append(quote(text(component)));
        }
        List<?> siblings = (List<?>) call(component, COMPONENT, "getSiblings", NONE);
        for (Object s : siblings) b.append(" + ").append(keyOf(s));
        return b.toString();
    }

    static Object screen() throws Exception { return get(mc, MC, "screen"); }

    static String screenName() throws Exception {
        return onRender(() -> {
            Object s = screen();
            return s == null ? "none" : officialName(s.getClass());
        });
    }

    static List<Object> widgets() throws Exception {
        return onRender(() -> {
            List<Object> all = new ArrayList<>();
            Object s = screen();
            if (s != null) collect(s, all, 0);
            return all;
        });
    }

    static void collect(Object container, List<Object> all, int depth) throws Exception {
        if (depth > 6) return;
        List<?> children = (List<?>) call(container, CONTAINER, "children", NONE);
        for (Object child : children) {
            if (cls(WIDGET).isInstance(child)) all.add(child);
            if (cls(CONTAINER).isInstance(child)) collect(child, all, depth + 1);
        }
    }

    static String describeWidget(Object w) throws Exception {
        return officialName(w.getClass()) + " " + call(w, WIDGET, "getX", NONE) + ","
               + call(w, WIDGET, "getY", NONE) + " " + call(w, WIDGET, "getWidth", NONE) + "x"
               + call(w, WIDGET, "getHeight", NONE) + " active " + get(w, WIDGET, "active")
               + " visible " + get(w, WIDGET, "visible")
               + " text " + quote(text(call(w, WIDGET, "getMessage", NONE)))
               + " key " + keyOf(call(w, WIDGET, "getMessage", NONE))
               + (cls(EDIT).isInstance(w) ? " value " + quote(String.valueOf(get(w, EDIT, "value")))
                                            + " maxLength " + get(w, EDIT, "maxLength") : "");
    }

    static void dump(String label) throws Exception {
        onRender(() -> {
            Object s = screen();
            Object w = call(mc, MC, "getWindow", NONE);
            out.println("── screen " + label + ": " + (s == null ? "none" : officialName(s.getClass()))
                        + " gui " + call(w, WINDOW, "getGuiScaledWidth", NONE) + "x"
                        + call(w, WINDOW, "getGuiScaledHeight", NONE) + " scale "
                        + call(w, WINDOW, "getGuiScale", NONE) + " paused " + call(mc, MC, "isPaused", NONE));
            if (s == null) return null;
            out.println("title " + quote(text(get(s, SCREEN, "title"))) + " key " + keyOf(get(s, SCREEN, "title")));
            List<Object> all = new ArrayList<>();
            collect(s, all, 0);
            for (Object wd : all) out.println("  " + describeWidget(wd));
            dumpLists(s, 0);
            return null;
        });
    }

    /// Every selection list on the screen: its box and its rows. Rows are not
    /// widgets, so a world list's geometry is only visible this way.
    static void dumpLists(Object container, int depth) throws Exception {
        if (depth > 4) return;
        for (Object child : (List<?>) call(container, CONTAINER, "children", NONE)) {
            if (cls(LIST).isInstance(child)) {
                List<?> rows = (List<?>) call(child, CONTAINER, "children", NONE);
                out.println("  list " + officialName(child.getClass()) + " x0 " + get(child, LIST, "x0")
                            + " y0 " + get(child, LIST, "y0") + " x1 " + get(child, LIST, "x1")
                            + " y1 " + get(child, LIST, "y1") + " itemHeight " + get(child, LIST, "itemHeight")
                            + " rowWidth " + call(child, LIST, "getRowWidth", NONE)
                            + " rowLeft " + call(child, LIST, "getRowLeft", NONE)
                            + " scrollbarX " + call(child, LIST, "getScrollbarPosition", NONE)
                            + " rows " + rows.size());
                Method top = method(LIST, "getRowTop", "int");
                for (int i = 0; i < Math.min(rows.size(), 40); i++) {
                    Object row = rows.get(i);
                    String extra = "";
                    try {
                        extra = " narration " + quote(text(row.getClass().getMethod("getNarration").invoke(row)));
                    } catch (Throwable ignored) {
                    }
                    out.println("    row " + i + " " + officialName(row.getClass()) + " top " + top.invoke(child, i) + extra);
                }
            }
            if (cls(CONTAINER).isInstance(child)) dumpLists(child, depth + 1);
        }
    }

    static Object findWidget(Predicate<String> match) throws Exception {
        for (Object w : widgets()) {
            String t = onRender(() -> text(call(w, WIDGET, "getMessage", NONE)));
            boolean usable = onRender(() -> (Boolean) get(w, WIDGET, "visible") && (Boolean) get(w, WIDGET, "active"));
            if (usable && match.test(t)) return w;
        }
        return null;
    }

    static void clickWidget(Object w) throws Exception {
        int[] box = onRender(() -> new int[]{(Integer) call(w, WIDGET, "getX", NONE),
                                             (Integer) call(w, WIDGET, "getY", NONE),
                                             (Integer) call(w, WIDGET, "getWidth", NONE),
                                             (Integer) call(w, WIDGET, "getHeight", NONE)});
        click(box[0] + box[2] / 2.0, box[1] + box[3] / 2.0);
    }

    static boolean clickLabel(String label) throws Exception {
        Object w = findWidget(t -> t.equals(label));
        if (w == null) {
            out.println("!! no widget labelled " + quote(label) + " on " + screenName());
            return false;
        }
        clickWidget(w);
        out.println("clicked " + quote(label) + " -> " + screenName());
        return true;
    }

    static boolean clickPrefix(String prefix) throws Exception {
        Object w = findWidget(t -> t.startsWith(prefix));
        if (w == null) {
            out.println("!! no widget starting " + quote(prefix) + " on " + screenName());
            return false;
        }
        clickWidget(w);
        out.println("clicked " + quote(prefix) + "… -> " + screenName());
        return true;
    }

    static void waitScreen(String name, int seconds) throws Exception {
        for (int i = 0; i < seconds * 10; i++) {
            if (screenName().equals(name)) return;
            pause(100);
        }
        out.println("!! waited " + seconds + " s for " + name + ", on " + screenName());
    }

    static void setEdit(int index, String value) throws Exception {
        int seen = 0;
        for (Object w : widgets()) {
            if (onRender(() -> cls(EDIT).isInstance(w))) {
                if (seen++ == index) {
                    onRender(() -> { call(w, EDIT, "setValue", new String[]{"java.lang.String"}, value); return null; });
                    out.println("edit box " + index + " set to " + quote(value));
                    return;
                }
            }
        }
        out.println("!! no edit box " + index + " on " + screenName());
    }

    static void dumpDebug(String label) throws Exception {
        onRender(() -> {
            Object gui = get(mc, MC, "gui");
            Object overlay = get(gui, "net.minecraft.client.gui.Gui", "debugOverlay");
            String o = "net.minecraft.client.gui.components.DebugScreenOverlay";
            out.println("── debug " + label + " renderDebug " + get(get(mc, MC, "options"), OPTIONS, "renderDebug"));
            for (Object l : (List<?>) call(overlay, o, "getGameInformation", NONE)) out.println("  left " + quote(String.valueOf(l)));
            for (Object l : (List<?>) call(overlay, o, "getSystemInformation", NONE)) out.println("  right " + quote(String.valueOf(l)));
            return null;
        });
    }

    static long gameTime() throws Exception {
        return onRender(() -> {
            Object level = get(mc, MC, "level");
            return (Long) call(level, "net.minecraft.world.level.Level", "getGameTime", NONE);
        });
    }

    static long serverTime() throws Exception {
        return onRender(() -> {
            Object server = call(mc, MC, "getSingleplayerServer", NONE);
            if (server == null) return -1L;
            return ((Number) call(server, "net.minecraft.server.MinecraftServer", "getTickCount", NONE)).longValue();
        });
    }

    static Object option(String getter) throws Exception {
        return call(get(mc, MC, "options"), OPTIONS, getter, NONE);
    }

    static void setOption(String getter, Object value) throws Exception {
        onRender(() -> {
            call(option(getter), OPTION, "set", new String[]{"java.lang.Object"}, value);
            return null;
        });
        out.println("option " + getter + " set to " + value);
    }

    static void saveOptions(String copyName) throws Exception {
        onRender(() -> { call(get(mc, MC, "options"), OPTIONS, "save", NONE); return null; });
        pause(300);
        Files.copy(new File(gameDir, "options.txt").toPath(), new File(outDir, copyName).toPath(),
                   StandardCopyOption.REPLACE_EXISTING);
        out.println("options.txt saved as " + copyName);
    }

    // ── The run ─────────────────────────────────────────────────────────────

    static void drive() throws Exception {
        while (true) {
            Method getInstance = null;
            try {
                getInstance = method(MC, "getInstance");
            } catch (Throwable ignored) {
            }
            if (getInstance != null) {
                mc = getInstance.invoke(null);
                if (mc != null && get(mc, MC, "screen") != null
                    && officialName(get(mc, MC, "screen").getClass()).equals("TitleScreen")) break;
            }
            pause(500);
        }
        pause(4000);  // the title screen fades in over its first second
        gameDir = (File) get(mc, MC, "gameDirectory");
        win = onRender(() -> (Long) call(call(mc, MC, "getWindow", NONE), WINDOW, "getWindow", NONE));
        out.println("title screen up; window " + win);
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
        saveOptions("options-initial.txt");

        // ── The title screen and the options, from the title ────────────────
        move(1, 1);  // the pointer off every button, so no capture shows a hover
        dump("title");
        shot("01-title.png");
        clickLabel("Options...");
        dump("options (title)");
        shot("02-options.png");
        clickLabel("Video Settings...");
        dump("video");
        move(1, 1);
        shot("03-video.png");
        key(256);
        clickLabel("Music & Sounds...");
        dump("sounds");
        move(1, 1);
        shot("04-sounds.png");
        key(256);
        clickLabel("Controls...");
        dump("controls");
        move(1, 1);
        shot("05-controls.png");
        clickLabel("Key Binds...");
        dump("key binds");
        move(1, 1);
        shot("06-keybinds.png");
        key(256);
        clickLabel("Mouse Settings...");
        dump("mouse");
        move(1, 1);
        shot("07-mouse.png");
        key(256);
        key(256);
        clickLabel("Language...");
        dump("language");
        move(1, 1);
        shot("08-language.png");
        key(256);
        key(256);
        waitScreen("TitleScreen", 5);

        // ── A new world, with a seed ────────────────────────────────────────
        clickLabel("Singleplayer");
        pause(1000);
        dump("singleplayer, no world yet");
        move(1, 1);
        shot("09-create-game.png");
        setEdit(0, "Oracle World");
        clickPrefix("Allow Cheats");
        dump("create, cheats on");
        clickLabel("World");
        dump("create, world tab");
        move(1, 1);
        shot("10-create-world.png");
        setEdit(0, "1234567890");
        dump("create, seed typed");
        clickLabel("More");
        dump("create, more tab");
        move(1, 1);
        shot("11-create-more.png");
        clickLabel("Game");
        clickLabel("Create New World");
        for (int i = 0; i < 1800; i++) {
            String name = screenName();
            if (i % 20 == 0) out.println("loading: " + name);
            if (i == 20) shot("12-loading.png");
            if (name.equals("none") && onRender(() -> get(mc, MC, "player")) != null) break;
            pause(100);
        }
        pause(6000);
        out.println("in world");

        // ── F3 ──────────────────────────────────────────────────────────────
        key(292);
        pause(1500);
        dumpDebug("f3");
        shot("13-f3.png");
        key(292);

        // ── The pause menu, and whether the game pauses ─────────────────────
        long before = serverTime();
        pause(2000);
        long running = serverTime();
        out.println("server ticks in 2 s, no screen: " + (running - before));
        key(256);
        pause(500);
        dump("pause");
        move(1, 1);
        shot("14-pause.png");
        long paused0 = serverTime();
        long level0 = gameTime();
        pause(2000);
        out.println("server ticks in 2 s, pause menu up: " + (serverTime() - paused0)
                    + "; level game time " + level0 + " -> " + gameTime());
        clickLabel("Options...");
        dump("options (in game)");
        move(1, 1);
        shot("15-options-ingame.png");
        key(256);
        clickLabel("Back to Game");
        pause(800);
        out.println("after Back to Game: " + screenName() + " paused " + onRender(() -> call(mc, MC, "isPaused", NONE)));

        // ── Death ───────────────────────────────────────────────────────────
        onRender(() -> {
            Object connection = call(mc, MC, "getConnection", NONE);
            call(connection, "net.minecraft.client.multiplayer.ClientPacketListener", "sendCommand",
                 new String[]{"java.lang.String"}, "kill");
            return null;
        });
        pause(300);
        dump("death, at once");
        shot("16-death-early.png");
        pause(2500);
        dump("death, after the delay");
        onRender(() -> {
            Object s = screen();
            if (s != null && officialName(s.getClass()).equals("DeathScreen")) {
                String d = "net.minecraft.client.gui.screens.DeathScreen";
                for (String f : new String[]{"causeOfDeath", "delayTicker", "hardcore", "deathScore"}) {
                    try {
                        Object v = get(s, d, f);
                        out.println("death " + f + " " + (cls(COMPONENT).isInstance(v) ? keyOf(v) + " " + quote(text(v)) : v));
                    } catch (Throwable e) {
                        out.println("death " + f + " unreadable: " + e);
                    }
                }
            }
            return null;
        });
        move(1, 1);
        shot("17-death.png");
        clickLabel("Respawn");
        pause(3000);
        out.println("after Respawn: " + screenName());

        // ── options.txt, written by vanilla with known values ───────────────
        setOption("fov", 90);
        setOption("renderDistance", 8);
        setOption("sensitivity", 0.75);
        setOption("enableVsync", Boolean.FALSE);
        setOption("framerateLimit", 60);
        onRender(() -> {
            Object options = get(mc, MC, "options");
            Class<?> source = cls("net.minecraft.sounds.SoundSource");
            Object music = null, block = null;
            for (Object c : source.getEnumConstants()) {
                String n = ((Enum<?>) c).name();
                if (n.equals("MUSIC")) music = c;
                if (n.equals("BLOCKS")) block = c;
            }
            Method sound = method(OPTIONS, "getSoundSourceOptionInstance", "net.minecraft.sounds.SoundSource");
            call(sound.invoke(options, music), OPTION, "set", new String[]{"java.lang.Object"}, 0.25);
            call(sound.invoke(options, block), OPTION, "set", new String[]{"java.lang.Object"}, 0.5);
            return null;
        });
        saveOptions("options-changed.txt");

        // ── Back to the title, and the world list ───────────────────────────
        key(256);
        pause(500);
        clickLabel("Save and Quit to Title");
        waitScreen("TitleScreen", 60);
        pause(3000);
        clickLabel("Singleplayer");
        pause(1500);
        dump("select world");
        move(1, 1);
        shot("18-select-world.png");
        key(256);
        waitScreen("TitleScreen", 5);
        clickLabel("Multiplayer");
        pause(1500);
        dump("multiplayer");
        move(1, 1);
        shot("19-multiplayer.png");
        clickLabel("Direct Connection");
        dump("direct connection");
        move(1, 1);
        shot("20-direct.png");
        key(256);
        key(256);
        dump("back on the title");
        out.println("done");
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        outDir = new File(args[1]);
        outDir.mkdirs();
        out = new PrintStream(new FileOutputStream(new File(outDir, "facts.txt")), true, "UTF-8");
        String[] mcArgs = Arrays.copyOfRange(args, 2, args.length);
        Thread driver = new Thread(() -> {
            int code = 0;
            try {
                drive();
            } catch (Throwable e) {
                e.printStackTrace(out);
                code = 1;
            }
            out.flush();
            Runtime.getRuntime().halt(code);
        }, "ov-screens-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
