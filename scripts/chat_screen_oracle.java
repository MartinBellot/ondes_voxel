// Ondes VOXEL — the chat screen, asked of the real 1.20.1 client.
//
// Same method as scripts/creative_screen_oracle.java, whose mapping reader and
// input helpers this copies: the user's own vanilla 1.20.1 client joins a
// vanilla server as an operator, and is driven with synthetic GLFW events
// injected into the game's own keyboard and mouse handlers. After each gesture
// the running game is asked for its numbers — the input box, the chat's size,
// the lines it holds and when they were added, the suggestion list, the title
// timers — and a screenshot is taken through the game's own screenshot path.
//
// Nothing here reads or translates Mojang code. The official mappings only
// *name* classes, fields and methods (CLAUDE.md § 1); every number printed is
// produced by the game's own bytecode at run time, and functions such as the
// chat's fade are measured as black boxes, by sampling them.
//
// What this cannot do, named: `Screen.hasControlDown()` and `hasShiftDown()`
// read the physical key state from GLFW, not the event stream, so Ctrl/Cmd and
// Shift chords (select all, word jumps, copy/paste) cannot be injected.
//
// Driven by scripts/measure_chat_screen.py.
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class ChatScreenOracle {

    // ── The mappings (same reader as the creative oracle) ────────────────────

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

    static Object get(Object target, String official, String name) throws Exception {
        String obf = FIELDS.getOrDefault(official, Map.of()).get(name);
        if (obf == null) throw new NoSuchFieldException(official + "." + name);
        Field f = cls(official).getDeclaredField(obf);
        f.setAccessible(true);
        return f.get(target);
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
    static final String LINE = "net.minecraft.client.GuiMessage$Line";
    static final String CHAT_SCREEN = "net.minecraft.client.gui.screens.ChatScreen";
    static final String EDIT = "net.minecraft.client.gui.components.EditBox";
    static final String WIDGET = "net.minecraft.client.gui.components.AbstractWidget";
    static final String SUGG = "net.minecraft.client.gui.components.CommandSuggestions";
    static final String SUGG_LIST = SUGG + "$SuggestionsList";
    static final String RECT = "net.minecraft.client.renderer.Rect2i";
    static final String WINDOW = "com.mojang.blaze3d.platform.Window";
    static final String COMPONENT = "net.minecraft.network.chat.Component";

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

    static void key(int glfwKey) throws Exception {
        onRender(() -> {
            Object kb = get(mc, MC, "keyboardHandler");
            keyPress().invoke(kb, win, glfwKey, 0, 1, 0);
            keyPress().invoke(kb, win, glfwKey, 0, 0, 0);
            return null;
        });
        pause(350);
    }

    /// A key and the character a real keyboard produces with it, in the order
    /// GLFW delivers them: press, character, release.
    static void keyWithChar(int glfwKey, int codepoint) throws Exception {
        onRender(() -> {
            Object kb = get(mc, MC, "keyboardHandler");
            keyPress().invoke(kb, win, glfwKey, 0, 1, 0);
            call(kb, "net.minecraft.client.KeyboardHandler", "charTyped",
                 new String[]{"long", "int", "int"}, win, codepoint, 0);
            keyPress().invoke(kb, win, glfwKey, 0, 0, 0);
            return null;
        });
        pause(350);
    }

    static void type(String text) throws Exception {
        for (int cp : text.codePoints().toArray()) {
            onRender(() -> {
                Object kb = get(mc, MC, "keyboardHandler");
                call(kb, "net.minecraft.client.KeyboardHandler", "charTyped",
                     new String[]{"long", "int", "int"}, win, cp, 0);
                return null;
            });
        }
        pause(350);
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
        pause(250);
    }

    static void scroll(double dy) throws Exception {
        onRender(() -> {
            Object mh = get(mc, MC, "mouseHandler");
            call(mh, "net.minecraft.client.MouseHandler", "onScroll",
                 new String[]{"long", "double", "double"}, win, 0.0, dy);
            return null;
        });
        pause(350);
    }

    static void shot(String name) throws Exception {
        pause(300);
        onRender(() -> {
            Object target = call(mc, MC, "getMainRenderTarget", NONE);
            java.util.function.Consumer<Object> sink = c -> {};
            method("net.minecraft.client.Screenshot", "grab", "java.io.File", "java.lang.String",
                   "com.mojang.blaze3d.pipeline.RenderTarget", "java.util.function.Consumer")
                .invoke(null, gameDir, name, target, sink);
            return null;
        });
        out.println("shot " + name + " at tick " + guiTicks());
        pause(200);
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

    static Object gui() throws Exception { return get(mc, MC, "gui"); }

    static Object chat() throws Exception { return call(gui(), GUI, "getChat", NONE); }

    static int guiTicks() throws Exception {
        return onRender(() -> (Integer) call(gui(), GUI, "getGuiTicks", NONE));
    }

    /// A FormattedCharSequence as a plain string, through a proxy sink.
    static String sequence(Object seq) throws Exception {
        StringBuilder b = new StringBuilder();
        Class<?> sinkType = cls("net.minecraft.util.FormattedCharSink");
        Object sink = Proxy.newProxyInstance(sinkType.getClassLoader(), new Class<?>[]{sinkType},
            (proxy, m, a) -> {
                if (a != null && a.length == 3 && a[2] instanceof Integer) {
                    b.appendCodePoint((Integer) a[2]);
                    return Boolean.TRUE;
                }
                if (m.getName().equals("hashCode")) return 0;
                if (m.getName().equals("equals")) return proxy == a[0];
                if (m.getName().equals("toString")) return "sink";
                return Boolean.TRUE;
            });
        for (Method m : seq.getClass().getMethods()) {
            if (m.getParameterCount() == 1 && m.getParameterTypes()[0] == sinkType
                && m.getReturnType() == boolean.class) {
                m.setAccessible(true);
                m.invoke(seq, sink);
                break;
            }
        }
        return b.toString();
    }

    static void dumpStatics() throws Exception {
        onRender(() -> {
            out.println("── statics");
            for (String f : new String[]{"MAX_CHAT_HISTORY", "MESSAGE_INDENT", "MESSAGE_TAG_MARGIN_LEFT",
                                         "BOTTOM_MARGIN", "TIME_BEFORE_MESSAGE_DELETION"}) {
                out.println("ChatComponent." + f + " = " + get(null, CHAT, f));
            }
            out.println("ChatScreen.MOUSE_SCROLL_SPEED = " + get(null, CHAT_SCREEN, "MOUSE_SCROLL_SPEED"));
            for (String f : new String[]{"CURSOR_INSERT_WIDTH", "CURSOR_INSERT_COLOR",
                                         "CURSOR_APPEND_CHARACTER", "DEFAULT_TEXT_COLOR",
                                         "BORDER_COLOR_FOCUSED", "BORDER_COLOR", "BACKGROUND_COLOR"}) {
                out.println("EditBox." + f + " = " + get(null, EDIT, f));
            }
            Object w = call(mc, MC, "getWindow", NONE);
            out.println("gui " + call(w, WINDOW, "getGuiScaledWidth", NONE) + "x"
                        + call(w, WINDOW, "getGuiScaledHeight", NONE)
                        + " scale " + call(w, WINDOW, "getGuiScale", NONE));
            Object c = chat();
            Method factor = method(CHAT, "getTimeFactor", "int");
            StringBuilder b = new StringBuilder("timeFactor");
            for (int age = 0; age <= 220; age += 5) {
                b.append(' ').append(age).append('=').append(factor.invoke(c, age));
            }
            for (int age : new int[]{179, 181, 185, 190, 195, 199, 200, 201}) {
                b.append(' ').append(age).append('=').append(factor.invoke(c, age));
            }
            out.println(b);
            Object g = gui();
            out.println("title times " + get(g, GUI, "titleFadeInTime") + " "
                        + get(g, GUI, "titleStayTime") + " " + get(g, GUI, "titleFadeOutTime"));
            return null;
        });
    }

    static void dumpChat(String label) throws Exception {
        onRender(() -> {
            Object c = chat();
            out.println("── chat " + label + " (tick " + call(gui(), GUI, "getGuiTicks", NONE) + ")");
            out.println("width " + call(c, CHAT, "getWidth", NONE) + " height "
                        + call(c, CHAT, "getHeight", NONE) + " linesPerPage "
                        + call(c, CHAT, "getLinesPerPage", NONE) + " lineHeight "
                        + call(c, CHAT, "getLineHeight", NONE) + " scale "
                        + call(c, CHAT, "getScale", NONE) + " focused "
                        + call(c, CHAT, "isChatFocused", NONE));
            List<?> all = (List<?>) get(c, CHAT, "allMessages");
            List<?> lines = (List<?>) get(c, CHAT, "trimmedMessages");
            List<?> recent = (List<?>) call(c, CHAT, "getRecentChat", NONE);
            out.println("allMessages " + all.size() + " trimmedMessages " + lines.size()
                        + " scrollbarPos " + get(c, CHAT, "chatScrollbarPos")
                        + " recentChat " + recent.size() + " " + recent);
            for (int i = 0; i < Math.min(14, lines.size()); i++) {
                Object l = lines.get(i);
                out.println("  line " + i + " added " + get(l, LINE, "addedTime") + " end "
                            + get(l, LINE, "endOfEntry") + " " + quote(sequence(get(l, LINE, "content"))));
            }
            return null;
        });
    }

    static void dumpScreen(String label) throws Exception {
        onRender(() -> {
            Object s = get(mc, MC, "screen");
            out.println("── screen " + label + ": " + (s == null ? "none" : s.getClass().getName()
                        + (s.getClass() == cls(CHAT_SCREEN) ? " (ChatScreen)" : "")));
            if (s == null || s.getClass() != cls(CHAT_SCREEN)) return null;
            Object box = get(s, CHAT_SCREEN, "input");
            out.println("input " + call(box, WIDGET, "getX", NONE) + "," + call(box, WIDGET, "getY", NONE)
                        + " " + call(box, WIDGET, "getWidth", NONE) + "x" + call(box, WIDGET, "getHeight", NONE)
                        + " value " + quote(String.valueOf(get(box, EDIT, "value")))
                        + " cursor " + get(box, EDIT, "cursorPos") + " highlight " + get(box, EDIT, "highlightPos")
                        + " displayPos " + get(box, EDIT, "displayPos") + " maxLength " + get(box, EDIT, "maxLength")
                        + " textColor " + get(box, EDIT, "textColor") + " bordered " + get(box, EDIT, "bordered")
                        + " suggestion " + quote(String.valueOf(get(box, EDIT, "suggestion"))));
            out.println("historyPos " + get(s, CHAT_SCREEN, "historyPos"));
            Object cs = get(s, CHAT_SCREEN, "commandSuggestions");
            Object list = get(cs, SUGG, "suggestions");
            out.println("usage lines " + ((List<?>) get(cs, SUGG, "commandUsage")).size()
                        + " at " + get(cs, SUGG, "commandUsagePosition") + " width "
                        + get(cs, SUGG, "commandUsageWidth"));
            for (Object u : (List<?>) get(cs, SUGG, "commandUsage")) {
                out.println("  usage " + quote(sequence(u)));
            }
            if (list == null) {
                out.println("suggestions none");
                return null;
            }
            Object rect = get(list, SUGG_LIST, "rect");
            List<?> entries = (List<?>) get(list, SUGG_LIST, "suggestionList");
            out.println("suggestions rect " + call(rect, RECT, "getX", NONE) + "," + call(rect, RECT, "getY", NONE)
                        + " " + call(rect, RECT, "getWidth", NONE) + "x" + call(rect, RECT, "getHeight", NONE)
                        + " count " + entries.size() + " offset " + get(list, SUGG_LIST, "offset")
                        + " current " + get(list, SUGG_LIST, "current"));
            for (Object e : entries) {
                Object range = e.getClass().getMethod("getRange").invoke(e);
                out.println("  suggestion " + quote((String) e.getClass().getMethod("getText").invoke(e))
                            + " range " + range);
            }
            return null;
        });
    }

    static void dumpGui(String label) throws Exception {
        onRender(() -> {
            Object g = gui();
            Object title = get(g, GUI, "title");
            Object sub = get(g, GUI, "subtitle");
            Object overlay = get(g, GUI, "overlayMessageString");
            out.println("── gui " + label + " (tick " + call(g, GUI, "getGuiTicks", NONE) + ")");
            out.println("title " + (title == null ? "null" : quote((String) title.getClass().getMethod("getString").invoke(title)))
                        + " subtitle " + (sub == null ? "null" : quote((String) sub.getClass().getMethod("getString").invoke(sub)))
                        + " titleTime " + get(g, GUI, "titleTime") + " times " + get(g, GUI, "titleFadeInTime")
                        + " " + get(g, GUI, "titleStayTime") + " " + get(g, GUI, "titleFadeOutTime"));
            out.println("overlay " + (overlay == null ? "null" : quote((String) overlay.getClass().getMethod("getString").invoke(overlay)))
                        + " overlayMessageTime " + get(g, GUI, "overlayMessageTime")
                        + " animate " + get(g, GUI, "animateOverlayMessageColor"));
            return null;
        });
    }

    /// Put a line in the open chat box and press Enter, through the screen's
    /// own key handler — the path a player's Enter takes.
    static void submit(String line) throws Exception {
        if (onRender(() -> get(mc, MC, "screen")) == null) {
            key(84);
        }
        onRender(() -> {
            Object s = get(mc, MC, "screen");
            call(get(s, CHAT_SCREEN, "input"), EDIT, "setValue", new String[]{"java.lang.String"}, line);
            return null;
        });
        pause(300);
        key(257);
        out.println("submitted " + quote(line));
    }

    static void waitForTick(int tick) throws Exception {
        while (guiTicks() < tick) pause(50);
    }

    static void addLocal(String text) throws Exception {
        onRender(() -> {
            Object literal = call(null, COMPONENT, "literal", new String[]{"java.lang.String"}, text);
            call(chat(), CHAT, "addMessage", new String[]{COMPONENT}, literal);
            return null;
        });
    }

    static void splitLines(String text) throws Exception {
        onRender(() -> {
            Object font = get(mc, MC, "font");
            Object splitter = call(font, "net.minecraft.client.gui.Font", "getSplitter", NONE);
            Object literal = call(null, COMPONENT, "literal", new String[]{"java.lang.String"}, text);
            Object empty = get(null, "net.minecraft.network.chat.Style", "EMPTY");
            int width = (Integer) call(chat(), CHAT, "getWidth", NONE);
            List<?> parts = (List<?>) call(splitter, "net.minecraft.client.StringSplitter", "splitLines",
                new String[]{"net.minecraft.network.chat.FormattedText", "int", "net.minecraft.network.chat.Style"},
                literal, width, empty);
            out.println("── split at " + width + ": " + quote(text));
            for (Object p : parts) {
                String s = (String) p.getClass().getMethod("getString").invoke(p);
                out.println("  piece " + quote(s) + " width "
                            + call(font, "net.minecraft.client.gui.Font", "width", new String[]{"java.lang.String"}, s));
            }
            return null;
        });
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
                if (mc != null && get(mc, MC, "player") != null) break;
            }
            pause(500);
        }
        pause(8000);
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
            return null;
        });
        // Look straight up: a uniform sky behind every capture, so that
        // scripts/measure_chat_geometry.py --sky can take any pixel that is not
        // sky colour as drawn by the interface, and read alphas off it.
        onRender(() -> {
            Object p = get(mc, MC, "player");
            call(p, "net.minecraft.world.entity.Entity", "setXRot", new String[]{"float"}, -90.0f);
            call(p, "net.minecraft.world.entity.Entity", "setYRot", new String[]{"float"}, 0.0f);
            return null;
        });
        pause(1500);
        // The join messages (and anything the server said) off the screen, so
        // every capture below shows only what this run put there.
        onRender(() -> { call(chat(), CHAT, "clearMessages", new String[]{"boolean"}, false); return null; });
        dumpStatics();
        dumpGui("join");
        dumpChat("join");
        shot("00-world.png");

        // ── Opening ─────────────────────────────────────────────────────────
        keyWithChar(84, 't');
        dumpScreen("T with its character");
        dumpChat("focused, empty");
        shot("01-open-empty.png");
        key(256);
        dumpScreen("escape");
        key(257);
        dumpScreen("Enter from the world");
        // No Escape here: with no screen open it would bring up the pause menu.
        keyWithChar(47, '/');
        dumpScreen("slash with its character");
        shot("01b-slash.png");
        key(258);
        pause(800);
        dumpScreen("/ then Tab");
        shot("01c-slash-tab.png");
        key(258);
        pause(300);
        dumpScreen("/ then Tab twice");
        key(264);
        dumpScreen("/ Tab Tab then Down");
        key(256);
        dumpScreen("escape with a suggestion list");
        if (onRender(() -> get(mc, MC, "screen")) != null) key(256);
        dumpScreen("escape again");

        // ── Editing ─────────────────────────────────────────────────────────
        key(84);
        type("hello world");
        dumpScreen("typed hello world");
        key(263); key(263); key(263);
        dumpScreen("left x3");
        key(268);
        dumpScreen("home");
        key(269);
        dumpScreen("end");
        key(259);
        dumpScreen("backspace");
        type("d");
        type("é§x");
        dumpScreen("typed e-acute, section sign, x");
        key(259); key(259);
        {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < 300; i++) b.append((char) ('a' + i % 26));
            onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                                  new String[]{"java.lang.String"}, ""); return null; });
            type(b.toString());
            dumpScreen("typed 300 letters");
            shot("02-long-input.png");
            onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                                  new String[]{"java.lang.String"}, "hello world"); return null; });
        }
        key(257);
        dumpScreen("enter");
        pause(600);
        dumpChat("after hello world");
        int added = onRender(() -> {
            List<?> lines = (List<?>) get(chat(), CHAT, "trimmedMessages");
            return lines.isEmpty() ? -1 : (Integer) get(lines.get(0), LINE, "addedTime");
        });
        out.println("hello world added at tick " + added);
        if (added >= 0) {
            waitForTick(added + 20);   shot("03-fade-01s.png");
            waitForTick(added + 170);  shot("04-fade-8.5s.png");
            waitForTick(added + 186);  shot("05-fade-9.3s.png");
            waitForTick(added + 194);  shot("06-fade-9.7s.png");
            waitForTick(added + 204);  shot("07-fade-10.2s.png");
        }

        // ── Commands and suggestions ────────────────────────────────────────
        keyWithChar(47, '/');
        type("ti");
        pause(800);
        dumpScreen("/ti");
        shot("08-suggest-ti.png");
        key(258);
        pause(500);
        dumpScreen("/ti then Tab");
        shot("09-suggest-tab.png");
        onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                              new String[]{"java.lang.String"}, "/time set "); return null; });
        pause(1200);
        dumpScreen("/time set ");
        shot("10-suggest-argument.png");
        onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                              new String[]{"java.lang.String"}, "/"); return null; });
        pause(1200);
        dumpScreen("/ alone");
        shot("11-suggest-slash.png");
        onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                              new String[]{"java.lang.String"}, "/gamemode "); return null; });
        pause(1200);
        dumpScreen("/gamemode ");
        // How a typed command is coloured: literals, arguments, and what does
        // not parse.
        onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                              new String[]{"java.lang.String"}, "/tp @s 0 64 0 zz"); return null; });
        pause(1200);
        dumpScreen("/tp @s 0 64 0 zz");
        shot("11b-typed-arguments.png");
        onRender(() -> { call(get(get(mc, MC, "screen"), CHAT_SCREEN, "input"), EDIT, "setValue",
                              new String[]{"java.lang.String"}, "/time set day"); return null; });
        pause(1200);
        dumpScreen("/time set day");
        shot("11c-typed-literals.png");
        key(257);
        pause(1500);
        dumpChat("after /time set day");
        shot("12-time-set.png");

        // ── History ─────────────────────────────────────────────────────────
        key(84);
        key(265);
        dumpScreen("up once");
        key(265);
        dumpScreen("up twice");
        key(265);
        dumpScreen("up thrice");
        key(264);
        dumpScreen("down");
        key(256);
        submit("  spaced    out   message  ");
        pause(800);
        dumpChat("whitespace");

        // ── Wrapping ────────────────────────────────────────────────────────
        String prose = "The quick brown fox jumps over the lazy dog, and then it keeps running "
                     + "because the lazy dog finally woke up and is now very, very angry.";
        String word = "Supercalifragilisticexpialidocious".repeat(4);
        splitLines(prose);
        splitLines(word);
        splitLines("first line\nsecond line");
        splitLines("  leading spaces and a trailing one ");
        onRender(() -> { call(chat(), CHAT, "clearMessages", new String[]{"boolean"}, false); return null; });
        addLocal(prose);
        addLocal(word);
        dumpChat("wrapped");
        shot("13-wrapped.png");

        // ── Scrolling ───────────────────────────────────────────────────────
        onRender(() -> { call(chat(), CHAT, "clearMessages", new String[]{"boolean"}, false); return null; });
        for (int i = 1; i <= 40; i++) addLocal("line " + i);
        key(84);
        move(40, 100);
        dumpChat("40 lines, open");
        shot("14-open-40.png");
        scroll(1);
        dumpChat("scrolled up one notch");
        shot("15-scrolled.png");
        scroll(-1);
        dumpChat("scrolled back");
        key(266);
        dumpChat("page up");
        key(267);
        dumpChat("page down");
        key(256);
        shot("16-closed-40.png");

        // ── Styles, the action bar, titles ──────────────────────────────────
        onRender(() -> { call(chat(), CHAT, "clearMessages", new String[]{"boolean"}, false); return null; });
        // Two commands, each under the 256 characters the box holds (the first
        // run's single tellraw was cut by the box and answered with an error).
        submit("/tellraw @s [{\"text\":\"bold \",\"bold\":true},{\"text\":\"ital \",\"italic\":true},"
               + "{\"text\":\"hex \",\"color\":\"#3080ff\"},{\"text\":\"gold\",\"color\":\"gold\",\"underlined\":true},"
               + "{\"text\":\" x\",\"strikethrough\":true}]");
        submit("/tellraw @s {\"translate\":\"commands.time.set\",\"with\":[\"42\"],\"color\":\"green\"}");
        pause(1200);
        dumpChat("tellraw");
        shot("17-styles.png");
        submit("/title @s actionbar {\"text\":\"Action bar\"}");
        pause(600);
        dumpGui("actionbar");
        shot("18-actionbar.png");
        submit("/title @s subtitle {\"text\":\"Subtitle\",\"color\":\"yellow\"}");
        submit("/title @s title {\"text\":\"Title\"}");
        pause(1000);
        dumpGui("title");
        shot("19-title.png");
        pause(4000);
        dumpGui("title later");
        shot("20-title-fading.png");
        dumpChat("end");
        out.println("done");
    }

    public static void main(String[] args) throws Exception {
        loadMappings(Paths.get(args[0]));
        File outDir = new File(args[1]);
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
        }, "ov-chat-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
