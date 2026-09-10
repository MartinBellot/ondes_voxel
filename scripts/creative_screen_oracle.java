// Ondes VOXEL — the creative screen, asked of the real 1.20.1 client.
//
// The geometry of the creative inventory lives in the client, and the client
// can be *run*. This class starts the user's own vanilla 1.20.1 client (the
// PrismLauncher instance), lets it join a vanilla server in creative, and then
// drives it with synthetic GLFW events injected into the game's own keyboard
// and mouse handlers: E opens the inventory, a click on a tab button selects
// it, a wheel notch scrolls, a character goes into the search box. After each
// gesture it asks the running game for the numbers — where the panel is,
// where every tab button is, where every slot is — and takes a screenshot
// through the game's own screenshot path.
//
// Nothing here reads or translates Mojang code. The official mappings are used
// only to *name* classes, fields and methods (CLAUDE.md § 1), exactly as in
// scripts/creative_tabs_oracle.java, and every number printed is produced by
// the game's own bytecode at run time. The input path is the game's too: a
// synthetic key press goes through KeyboardHandler.keyPress, the same method
// GLFW's callback calls, so a click that selects a tab here selects it for the
// reason a real click would.
//
// What this cannot do, named: `Screen.hasShiftDown()` reads the physical key
// state from GLFW, not the event stream, so no shift-gesture can be injected.
//
// Driven by scripts/measure_creative_screen.py.
//
//   java -XstartOnFirstThread -cp <out>:<client classpath> ov.CreativeScreenOracle \
//        <client mappings> <out dir> <Minecraft arguments...>
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public final class CreativeScreenOracle {

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
                    Class<?> p = m.getParameterTypes()[i];
                    ok = p.getName().equals(obfOf(argTypes[i]));
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

    // ── The game, and a way to run on its render thread ─────────────────────

    static final String MC = "net.minecraft.client.Minecraft";
    static final String SCREEN = "net.minecraft.client.gui.screens.Screen";
    static final String ACS = "net.minecraft.client.gui.screens.inventory.AbstractContainerScreen";
    static final String CIS = "net.minecraft.client.gui.screens.inventory.CreativeModeInventoryScreen";
    static final String TAB = "net.minecraft.world.item.CreativeModeTab";
    static final String TABS = "net.minecraft.world.item.CreativeModeTabs";
    static final String SLOT = "net.minecraft.world.inventory.Slot";
    static final String MENU = "net.minecraft.world.inventory.AbstractContainerMenu";
    static final String WINDOW = "com.mojang.blaze3d.platform.Window";
    static final String STACK = "net.minecraft.world.item.ItemStack";

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

    static void key(int glfwKey) throws Exception {
        onRender(() -> {
            Object kb = get(mc, MC, "keyboardHandler");
            Method press = method("net.minecraft.client.KeyboardHandler", "keyPress",
                                  "long", "int", "int", "int", "int");
            press.invoke(kb, win, glfwKey, 0, 1, 0);
            press.invoke(kb, win, glfwKey, 0, 0, 0);
            return null;
        });
        pause(400);
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
        pause(400);
    }

    /// Screen coordinates per GUI pixel: the inverse of what MouseHandler does
    /// to a cursor position.
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

    static void click(int button) throws Exception {
        onRender(() -> {
            Object mh = get(mc, MC, "mouseHandler");
            Method press = method("net.minecraft.client.MouseHandler", "onPress",
                                  "long", "int", "int", "int");
            press.invoke(mh, win, button, 1, 0);
            press.invoke(mh, win, button, 0, 0);
            return null;
        });
        pause(400);
    }

    static void scroll(double dy) throws Exception {
        onRender(() -> {
            Object mh = get(mc, MC, "mouseHandler");
            call(mh, "net.minecraft.client.MouseHandler", "onScroll",
                 new String[]{"long", "double", "double"}, win, 0.0, dy);
            return null;
        });
        pause(400);
    }

    static void shot(String name) throws Exception {
        pause(700);
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

    static Object screen() throws Exception { return onRender(() -> get(mc, MC, "screen")); }

    // Never nest onRender: a task that waits on the render thread from the
    // render thread waits forever. That is how the first run hung.
    static int leftPos() throws Exception {
        return onRender(() -> (Integer) get(get(mc, MC, "screen"), ACS, "leftPos"));
    }

    static int topPos() throws Exception {
        return onRender(() -> (Integer) get(get(mc, MC, "screen"), ACS, "topPos"));
    }

    static String describe(Object stack) throws Exception {
        if (stack == null) return "null";
        int count = (Integer) call(stack, STACK, "getCount", NONE);
        if (count == 0) return "empty";
        Object item = call(stack, STACK, "getItem", NONE);
        Object name = call(stack, STACK, "getHoverName", NONE);
        String text = (String) name.getClass().getMethod("getString").invoke(name);
        return item + " x" + count + " \"" + text + "\"";
    }

    static List<?> tabs() throws Exception {
        return (List<?>) call(null, TABS, "tabs", NONE);
    }

    static String tabId(Object tab) throws Exception {
        Object registry = get(null, "net.minecraft.core.registries.BuiltInRegistries",
                              "CREATIVE_MODE_TAB");
        return String.valueOf(call(registry, "net.minecraft.core.Registry", "getKey",
                                   new String[]{"java.lang.Object"}, tab));
    }

    static Object selectedTab() throws Exception {
        return onRender(() -> get(null, CIS, "selectedTab"));
    }

    static void dumpLayout(String label) throws Exception {
        onRender(() -> {
            Object s = get(mc, MC, "screen");
            out.println("── layout " + label);
            out.println("screen " + (s == null ? "none" : s.getClass().getName())
                        + (s != null && s.getClass() == cls(CIS) ? " (CreativeModeInventoryScreen)" : ""));
            if (s == null || !cls(ACS).isInstance(s)) return null;
            Object w = call(mc, MC, "getWindow", NONE);
            out.println("gui " + get(s, SCREEN, "width") + "x" + get(s, SCREEN, "height")
                        + " scale " + call(w, WINDOW, "getGuiScale", NONE));
            out.println("panel left " + get(s, ACS, "leftPos") + " top " + get(s, ACS, "topPos")
                        + " size " + get(s, ACS, "imageWidth") + "x" + get(s, ACS, "imageHeight")
                        + " title at " + get(s, ACS, "titleLabelX") + "," + get(s, ACS, "titleLabelY"));
            if (s.getClass() != cls(CIS)) return null;
            out.println("selected " + tabId(get(null, CIS, "selectedTab"))
                        + " scrollOffs " + get(s, CIS, "scrollOffs"));
            Method tx = method(CIS, "getTabX", TAB);
            Method ty = method(CIS, "getTabY", TAB);
            for (Object tab : tabs()) {
                out.println("tab " + tabId(tab) + " row " + call(tab, TAB, "row", NONE)
                            + " column " + call(tab, TAB, "column", NONE)
                            + " right " + call(tab, TAB, "isAlignedRight", NONE)
                            + " type " + call(tab, TAB, "getType", NONE)
                            + " x " + tx.invoke(s, tab) + " y " + ty.invoke(s, tab));
            }
            Object box = get(s, CIS, "searchBox");
            String aw = "net.minecraft.client.gui.components.AbstractWidget";
            out.println("searchBox " + call(box, aw, "getX", NONE) + "," + call(box, aw, "getY", NONE)
                        + " " + call(box, aw, "getWidth", NONE) + "x" + call(box, aw, "getHeight", NONE)
                        + " visible " + get(box, aw, "visible"));
            Object destroy = get(s, CIS, "destroyItemSlot");
            if (destroy != null) {
                out.println("destroySlot " + get(destroy, SLOT, "x") + "," + get(destroy, SLOT, "y"));
            }
            Object menu = get(s, ACS, "menu");
            List<?> slots = (List<?>) get(menu, MENU, "slots");
            out.println("slots " + slots.size());
            for (Object slot : slots) {
                String extra = "";
                try {
                    Object target = get(slot, CIS + "$SlotWrapper", "target");
                    extra = " wraps container slot " + call(target, SLOT, "getContainerSlot", NONE)
                          + " of " + get(target, SLOT, "container").getClass().getName();
                } catch (Throwable ignored) {
                    extra = " container slot " + call(slot, SLOT, "getContainerSlot", NONE)
                          + " of " + get(slot, SLOT, "container").getClass().getName();
                }
                out.println("  slot " + get(slot, SLOT, "index") + " at " + get(slot, SLOT, "x")
                            + "," + get(slot, SLOT, "y") + extra);
            }
            return null;
        });
    }

    static void dumpHands(String label) throws Exception {
        onRender(() -> {
            Object player = get(mc, MC, "player");
            Object inv = call(player, "net.minecraft.world.entity.player.Player", "getInventory", NONE);
            Object menu = get(player, "net.minecraft.world.entity.player.Player", "inventoryMenu");
            out.println("── hands " + label);
            // Inline, not through itemEntities(): this already runs on the
            // render thread, and a nested onRender would wait on itself.
            // Entities *and* the items they carry: a count of entities says
            // that something was thrown, the sum says how much.
            int entities = 0;
            int thrown = 0;
            Object level = get(mc, MC, "level");
            for (Object e : (Iterable<?>) call(level, "net.minecraft.client.multiplayer.ClientLevel",
                                               "entitiesForRendering", NONE)) {
                if (cls("net.minecraft.world.entity.item.ItemEntity").isInstance(e)) {
                    entities++;
                    Object item = call(e, "net.minecraft.world.entity.item.ItemEntity", "getItem", NONE);
                    thrown += (Integer) call(item, STACK, "getCount", NONE);
                }
            }
            out.println("item entities " + entities + " holding " + thrown);
            out.println("carried " + describe(call(menu, MENU, "getCarried", NONE)));
            Object s = get(mc, MC, "screen");
            if (s != null && cls(ACS).isInstance(s)) {
                out.println("screen carried " + describe(call(get(s, ACS, "menu"), MENU, "getCarried", NONE)));
            }
            for (int i = 0; i < 41; i++) {
                Object st = call(inv, "net.minecraft.world.entity.player.Inventory", "getItem",
                                 new String[]{"int"}, i);
                String d = describe(st);
                if (!d.equals("empty")) out.println("  inventory " + i + " " + d);
            }
            return null;
        });
    }

    // ── The run ─────────────────────────────────────────────────────────────

    static void clickTab(String id) throws Exception {
        int left = leftPos();
        int top = topPos();
        for (Object tab : tabs()) {
            if (!tabId(tab).equals(id)) continue;
            Object s = screen();
            int x = (Integer) onRender(() -> method(CIS, "getTabX", TAB).invoke(s, tab));
            int y = (Integer) onRender(() -> method(CIS, "getTabY", TAB).invoke(s, tab));
            move(left + x + 13, top + y + 16);
            click(0);
            out.println("clicked tab " + id + " -> selected " + tabId(selectedTab()));
            return;
        }
        throw new IllegalStateException("no tab " + id);
    }

    /// A click of `type` on the index-th slot of the open menu, through the
    /// screen's own slotClicked.
    static void slotClick(int index, int button, Object type) throws Exception {
        onRender(() -> {
            Object s = get(mc, MC, "screen");
            Object slot = ((List<?>) get(get(s, ACS, "menu"), MENU, "slots")).get(index);
            method(CIS, "slotClicked", SLOT, "int", "int", "net.minecraft.world.inventory.ClickType")
                .invoke(s, slot, get(slot, SLOT, "index"), button, type);
            return null;
        });
        pause(500);
    }

    static void cell(int index) throws Exception {
        move(leftPos() + 9 + 18 * (index % 9) + 8, topPos() + 18 + 18 * (index / 9) + 8);
    }

    static void drive() throws Exception {
        // The game: wait for it, then for the player, then for the chunks.
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
        // Detach the real devices. The window sits on the user's desktop, and
        // the first run's "hover" capture showed a tooltip under the *real*
        // pointer: GLFW's cursor callback writes the same field the synthetic
        // move does. With the callbacks gone, only this driver speaks.
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
        dumpHands("join");

        key(69);  // E
        pause(800);
        move(2, 2);
        dumpLayout("opened");
        shot("00-open.png");

        List<String> ids = new ArrayList<>();
        for (Object tab : tabs()) ids.add(tabId(tab));
        int n = 1;
        for (String id : ids) {
            clickTab(id);
            move(2, 2);
            if (id.endsWith(":inventory") || id.endsWith(":search") || id.endsWith(":hotbar")) {
                dumpLayout(id);
            }
            shot(String.format("%02d-%s.png", n++, id.substring(id.indexOf(':') + 1)));
        }

        // Tooltips: a cell, a tab button.
        clickTab("minecraft:building_blocks");
        cell(0);
        shot("hover-cell.png");
        clickTab("minecraft:combat");
        cell(0);
        shot("hover-cell-combat.png");
        move(2, 2);
        {
            // Hover (not click) the Combat button.
            int left = leftPos();
            int top = topPos();
            Object s = screen();
            for (Object tab : tabs()) {
                if (!tabId(tab).equals("minecraft:food_and_drinks")) continue;
                int x = (Integer) onRender(() -> method(CIS, "getTabX", TAB).invoke(s, tab));
                int y = (Integer) onRender(() -> method(CIS, "getTabY", TAB).invoke(s, tab));
                move(left + x + 13, top + y + 16);
            }
        }
        shot("hover-tab.png");

        // Scrolling.
        clickTab("minecraft:building_blocks");
        move(2, 2);
        scroll(-1);
        dumpLayout("scrolled once");
        shot("scroll-1.png");
        scroll(-100);
        dumpLayout("scrolled to the end");
        shot("scroll-end.png");
        scroll(100);

        // The search.
        clickTab("minecraft:search");
        type("diamond");
        move(2, 2);
        dumpLayout("search diamond");
        shot("search-diamond.png");
        cell(0);
        shot("search-hover.png");
        // E and Q while the search page is up: typed, or acted on?
        move(2, 2);
        key(69);
        out.println("E on the search page -> screen "
                    + (screen() == null ? "closed" : screen().getClass().getName())
                    + " box " + quote(String.valueOf(onRender(() -> call(get(get(mc, MC, "screen"), CIS,
                          "searchBox"), "net.minecraft.client.gui.components.EditBox", "getValue", NONE)))));
        if (screen() == null) key(69);
        type("q");
        cell(0);
        key(81);
        dumpHands("Q over search cell 0 after typing q");
        out.println("box now " + quote(String.valueOf(onRender(() -> call(get(get(mc, MC, "screen"), CIS,
                          "searchBox"), "net.minecraft.client.gui.components.EditBox", "getValue", NONE)))));
        for (int i = 0; i < 12; i++) key(259);  // backspace

        // Gestures on the catalogue, and what they leave in hand.
        clickTab("minecraft:building_blocks");
        cell(0);
        click(0);
        dumpHands("left click on cell 0");
        shot("carry.png");
        move(leftPos() + 9 + 8, topPos() + 112 + 8);
        click(0);
        dumpHands("then left click on hotbar 0");
        cell(1);
        click(1);
        dumpHands("right click on cell 1");
        move(leftPos() + 9 + 18 + 8, topPos() + 112 + 8);
        click(0);
        dumpHands("then left click on hotbar 1");
        cell(2);
        click(2);
        dumpHands("middle click on cell 2");
        move(leftPos() + 9 + 36 + 8, topPos() + 112 + 8);
        click(0);
        dumpHands("then left click on hotbar 2");
        cell(3);
        key(52);  // '4'
        dumpHands("key 4 over cell 3");
        cell(4);
        key(81);  // Q
        dumpHands("Q over cell 4");
        cell(5);
        click(0);
        move(leftPos() - 20, topPos() + 20);
        click(0);
        dumpHands("left click on cell 5, then outside the panel");
        // A stack in hand dropped on a hotbar slot that already holds one.
        cell(6);
        click(0);
        move(leftPos() + 9 + 8, topPos() + 112 + 8);
        click(0);
        dumpHands("cell 6 onto the occupied hotbar 0");
        move(2, 2);
        click(0);
        dumpHands("and the swapped stack clicked outside");

        // What a click on the catalogue does to what is already in hand.
        cell(0);
        click(0);
        click(0);
        dumpHands("left click on cell 0 twice");
        click(1);
        dumpHands("then right click on the same cell");
        cell(1);
        click(0);
        dumpHands("then left click on a different cell");
        cell(1);
        click(2);
        dumpHands("then middle click on that cell");
        move(leftPos() + 9 + 18 * 5 + 8, topPos() + 112 + 8);
        click(1);
        dumpHands("then right click on the empty hotbar 5");
        move(2, 2);
        click(1);
        dumpHands("then right click outside");
        click(0);
        dumpHands("then left click outside");
        // Q over a hotbar slot of the category page, and over a catalogue cell
        // with something in hand.
        move(leftPos() + 9 + 36 + 8, topPos() + 112 + 8);
        key(81);
        dumpHands("Q over hotbar 2");
        cell(7);
        click(0);
        cell(8);
        key(81);
        dumpHands("Q over cell 8 with cell 7 in hand");
        move(2, 2);
        click(0);

        // Shift and Ctrl, through the screen's own slotClicked. mouseClicked
        // turns a click into QUICK_MOVE when hasShiftDown() is true, and that
        // reads the physical key from GLFW, which no event can fake; calling
        // slotClicked with QUICK_MOVE is the same path from there on.
        Object quick = get(null, "net.minecraft.world.inventory.ClickType", "QUICK_MOVE");
        Object toss = get(null, "net.minecraft.world.inventory.ClickType", "THROW");
        move(2, 2);
        click(0);
        slotClick(10, 0, quick);
        dumpHands("shift-click on cell 10");
        slotClick(10, 0, quick);
        dumpHands("shift-click on cell 10 again");
        slotClick(45, 0, quick);
        dumpHands("shift-click on hotbar 0 of the category page");
        slotClick(11, 1, toss);
        dumpHands("ctrl+Q on cell 11");
        slotClick(12, 0, toss);
        dumpHands("Q on cell 12, empty hand");
        cell(13);
        click(0);
        slotClick(14, 0, toss);
        dumpHands("Q on cell 14 with cell 13 in hand");
        move(2, 2);
        click(0);

        // The survival page.
        clickTab("minecraft:inventory");
        move(leftPos() + 173 + 8, topPos() + 112 + 8);
        shot("inventory-hover-destroy.png");
        dumpHands("survival page");
        // Pick the hotbar 1 stack up on the survival page and destroy it.
        {
            int left = leftPos();
            int top = topPos();
            Object s = screen();
            Object menu = onRender(() -> get(s, ACS, "menu"));
            List<?> slots = (List<?>) onRender(() -> get(menu, MENU, "slots"));
            for (Object slot : slots) {
                int container = (Integer) onRender(() -> {
                    try {
                        Object target = get(slot, CIS + "$SlotWrapper", "target");
                        // The player's Inventory, not the crafting grid, whose
                        // slot 1 is parked at (-2000, -2000) and was found first.
                        if (get(target, SLOT, "container").getClass()
                            != cls("net.minecraft.world.entity.player.Inventory")) return -1;
                        return (Integer) call(target, SLOT, "getContainerSlot", NONE);
                    } catch (Throwable e) {
                        return -1;
                    }
                });
                // Hotbar 0: the one slot every earlier gesture left full.
                if (container == 0) {
                    int x = (Integer) onRender(() -> get(slot, SLOT, "x"));
                    int y = (Integer) onRender(() -> get(slot, SLOT, "y"));
                    move(left + x + 8, top + y + 8);
                    shot("inventory-hover-item.png");
                    click(0);
                    dumpHands("survival page: picked hotbar 1");
                    move(left + 173 + 8, top + 112 + 8);
                    click(0);
                    dumpHands("survival page: dropped on the destroy slot");
                    onRender(() -> {
                        Object d = get(s, CIS, "destroyItemSlot");
                        method(CIS, "slotClicked", SLOT, "int", "int",
                               "net.minecraft.world.inventory.ClickType")
                            .invoke(s, d, get(d, SLOT, "index"), 0, quick);
                        return null;
                    });
                    pause(500);
                    dumpHands("survival page: shift-click on the destroy slot");
                    break;
                }
            }
        }

        // T on a category page: does it jump to the search tab?
        clickTab("minecraft:building_blocks");
        move(2, 2);
        key(84);
        out.println("T on building blocks -> selected " + tabId(selectedTab()));

        dumpCatalogue(new File(gameDir, "oracle/creative_items.json"));

        key(256);  // Escape
        dumpHands("closed");
        out.println("done");
    }

    // ── The data the client computes and the server jar cannot ──────────────

    static String quote(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char c : s.toCharArray()) {
            if (c == '"' || c == '\\') b.append('\\').append(c);
            else if (c < 0x20) b.append(String.format("\\u%04x", (int) c));
            else b.append(c);
        }
        return b.append('"').toString();
    }

    static String componentsJson(List<?> lines) throws Exception {
        Method toJson = method("net.minecraft.network.chat.Component$Serializer", "toJson",
                               "net.minecraft.network.chat.Component");
        StringBuilder b = new StringBuilder("[");
        for (int i = 0; i < lines.size(); i++) {
            if (i > 0) b.append(',');
            b.append((String) toJson.invoke(null, lines.get(i)));
        }
        return b.append(']').toString();
    }

    static List<?> pickerItems(Object s) throws Exception {
        return (List<?>) get(get(s, ACS, "menu"), CIS + "$ItemPickerMenu", "items");
    }

    static String stackKey(Object stack) throws Exception {
        Object item = call(stack, STACK, "getItem", NONE);
        Object tag = call(stack, STACK, "getTag", NONE);
        return "{\"item\":" + quote("minecraft:" + item) + ",\"snbt\":"
             + (tag == null ? "null" : quote(tag.toString())) + "}";
    }

    /// Every stack of the search tab, with what the running client says about
    /// it: the tooltip as components (on a category page, and on the search
    /// page where the tab names are added), the tint of each layer, and the
    /// durability. Then the search results for a set of queries, in order.
    static void dumpCatalogue(File file) throws Exception {
        file.getParentFile().mkdirs();
        clickTab("minecraft:building_blocks");
        Object s = screen();
        Method tooltip = method(CIS, "getTooltipFromContainerItem", STACK);
        Object search = call(null, TABS, "searchTab", NONE);
        Collection<?> all = (Collection<?>) call(search, TAB, "getDisplayItems", NONE);
        List<Object> stacks = new ArrayList<>(all);

        List<String> base = onRender(() -> {
            List<String> r = new ArrayList<>();
            for (Object st : stacks) r.add(componentsJson((List<?>) tooltip.invoke(s, st)));
            return r;
        });
        clickTab("minecraft:search");
        Object s2 = screen();
        List<String> onSearch = onRender(() -> {
            List<String> r = new ArrayList<>();
            for (Object st : stacks) r.add(componentsJson((List<?>) tooltip.invoke(s2, st)));
            return r;
        });
        List<String> extra = onRender(() -> {
            List<String> r = new ArrayList<>();
            // A field, not a getter: 1.20.1's Minecraft has no getItemColors().
            Object colours = get(mc, MC, "itemColors");
            Method colour = method("net.minecraft.client.color.item.ItemColors", "getColor", STACK, "int");
            for (Object st : stacks) {
                StringBuilder b = new StringBuilder("\"tints\":[");
                for (int layer = 0; layer < 3; layer++) {
                    if (layer > 0) b.append(',');
                    b.append((Integer) colour.invoke(colours, st, layer));
                }
                b.append("],\"max_damage\":").append(call(st, STACK, "getMaxDamage", NONE))
                 .append(",\"rarity\":").append(quote(String.valueOf(call(st, STACK, "getRarity", NONE))));
                // The armour slot an item may go in: the survival page's
                // four armour slots refuse anything else, in creative too.
                String equipment = "?";
                try {
                    // LivingEntity, not Mob: 1.20.1 has it there (the first
                    // run asked Mob and got NoSuchMethod 1557 times).
                    Object slot = call(null, "net.minecraft.world.entity.LivingEntity",
                                       "getEquipmentSlotForItem", new String[]{STACK}, st);
                    equipment = (String) call(slot, "net.minecraft.world.entity.EquipmentSlot", "getName", NONE);
                } catch (Throwable e) {
                    equipment = "error: " + e;
                }
                b.append(",\"equipment\":").append(quote(equipment));
                r.add(b.toString());
            }
            return r;
        });

        // The queries: typed the way a player types them, through setValue and
        // the screen's own refresh, and read back as the picker's list.
        String[] queries = {"diamond", "stone", "oak_l", "sharp", "night vision", "potion",
                            "minecraft:", "c418", "applies", "red", "ench", "#minecraft:logs",
                            "sword", "e", "zzz", "Stone", "  stone", "glass pane", "spawn"};
        StringBuilder q = new StringBuilder();
        for (String query : queries) {
            List<String> keys = onRender(() -> {
                Object box = get(s2, CIS, "searchBox");
                call(box, "net.minecraft.client.gui.components.EditBox", "setValue",
                     new String[]{"java.lang.String"}, query);
                call(s2, CIS, "refreshSearchResults", NONE);
                List<String> r = new ArrayList<>();
                for (Object st : pickerItems(s2)) r.add(stackKey(st));
                return r;
            });
            if (q.length() > 0) q.append(',');
            q.append("{\"query\":").append(quote(query)).append(",\"results\":[")
             .append(String.join(",", keys)).append("]}");
            out.println("query " + quote(query) + " -> " + keys.size());
        }
        onRender(() -> {
            Object box = get(s2, CIS, "searchBox");
            call(box, "net.minecraft.client.gui.components.EditBox", "setValue",
                 new String[]{"java.lang.String"}, "");
            call(s2, CIS, "refreshSearchResults", NONE);
            return null;
        });

        // The saved hotbars page: what is in its cells, and what they say.
        clickTab("minecraft:hotbar");
        Object s3 = screen();
        String hotbars = onRender(() -> {
            StringBuilder b = new StringBuilder("[");
            List<?> items = pickerItems(s3);
            for (int i = 0; i < items.size(); i++) {
                if (i > 0) b.append(',');
                Object st = items.get(i);
                boolean empty = (Integer) call(st, STACK, "getCount", NONE) == 0;
                b.append("{\"index\":").append(i).append(",\"stack\":")
                 .append(empty ? "null" : stackKey(st)).append(",\"tooltip\":")
                 .append(empty ? "[]" : componentsJson((List<?>) tooltip.invoke(s3, st))).append('}');
            }
            return b.append(']').toString();
        });

        try (Writer w = new OutputStreamWriter(new FileOutputStream(file), StandardCharsets.UTF_8)) {
            w.write("{\"version\":\"1.20.1\",\"stacks\":[");
            for (int i = 0; i < stacks.size(); i++) {
                if (i > 0) w.write(",\n");
                String key = stackKey(stacks.get(i));
                w.write(key.substring(0, key.length() - 1) + ",\"tooltip\":" + base.get(i)
                        + ",\"search_tooltip\":" + onSearch.get(i) + "," + extra.get(i) + "}");
            }
            w.write("],\n\"queries\":[" + q + "],\n\"hotbars\":" + hotbars + "}\n");
        }
        out.println("catalogue " + stacks.size() + " stacks -> " + file);
    }

    static int itemEntities() throws Exception {
        return onRender(() -> {
            Object level = get(mc, MC, "level");
            int n = 0;
            for (Object e : (Iterable<?>) call(level, "net.minecraft.client.multiplayer.ClientLevel",
                                               "entitiesForRendering", NONE)) {
                if (cls("net.minecraft.world.entity.item.ItemEntity").isInstance(e)) n++;
            }
            return n;
        });
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
        }, "ov-creative-oracle");
        driver.setDaemon(true);
        driver.start();
        Class.forName("net.minecraft.client.main.Main").getMethod("main", String[].class)
             .invoke(null, (Object) mcArgs);
    }
}
