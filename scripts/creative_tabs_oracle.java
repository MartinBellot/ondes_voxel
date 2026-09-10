// Ondes VOXEL — the creative tabs, asked of the game itself.
//
// In 1.20.1 the creative inventory's tabs and their contents are built in
// `CreativeModeTabs`, which ships in the *server* jar as well as the client's.
// There is therefore an oracle, and this is it: the class is loaded, the game
// is bootstrapped, `tryRebuildTabContents` is called, and the resulting stacks
// are printed in the order the game put them in.
//
// Nothing here reads or translates Mojang code. The official mappings are used
// only to *name* classes, fields and methods — exactly the use CLAUDE.md § 1
// permits — and every fact printed below is produced by the game's own
// bytecode at run time, the same way `tools/ov_datagen` gets the registries.
//
// Driven by scripts/measure_creative_tabs.py, which supplies the classpath and
// the mappings file. Output is JSON on stdout.
//
//   javac -d <out> scripts/creative_tabs_oracle.java
//   java -cp <out>:<server jar>:<libs> ov.CreativeTabsOracle <mappings> [op]
//
// The class lives in a package on purpose: the server jar is signed, and an
// unsigned class in the same (default) package makes the JVM refuse to load
// any of it with "signer information does not match".
package ov;

import java.io.*;
import java.lang.reflect.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

public final class CreativeTabsOracle {

    // ── The mappings, as three lookup tables ────────────────────────────────
    //
    // Proguard format: a class line "official -> obf:", then indented member
    // lines "[from:to:]type name[(args)] -> obf".

    static final Map<String, String> CLASSES = new HashMap<>();
    static final Map<String, Map<String, String>> FIELDS = new HashMap<>();
    /// "name/argc" -> "obf officialArgType,officialArgType". The argument types
    /// are kept because obfuscation collapses overloads onto one name: five
    /// different methods on CreativeModeTabs are all called `a`.
    static final Map<String, Map<String, String>> METHODS = new HashMap<>();

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
                               .put(head.substring(sp + 1) + "/" + n, obf + " " + args);
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

    static Object staticField(String official, String name) throws Exception {
        String obf = FIELDS.getOrDefault(official, Map.of()).get(name);
        if (obf == null) throw new NoSuchFieldException(official + "." + name);
        Field f = cls(official).getDeclaredField(obf);
        f.setAccessible(true);
        return f.get(null);
    }

    /// The run-time name a mapped parameter type must have.
    static String obfOf(String officialType) {
        int dim = 0;
        while (officialType.endsWith("[]")) {
            officialType = officialType.substring(0, officialType.length() - 2);
            dim++;
        }
        return CLASSES.getOrDefault(officialType, officialType) + "[]".repeat(dim);
    }

    static Method method(String official, String name, int argc) throws Exception {
        String entry = METHODS.getOrDefault(official, Map.of()).get(name + "/" + argc);
        if (entry == null) throw new NoSuchMethodException(official + "." + name + "/" + argc);
        int sp = entry.indexOf(' ');
        String obf = sp < 0 ? entry : entry.substring(0, sp);
        String[] argTypes = (sp < 0 || entry.length() == sp + 1)
                          ? new String[0] : entry.substring(sp + 1).split(",");
        List<Method> hits = new ArrayList<>();
        for (Method m : cls(official).getDeclaredMethods()) {
            if (!m.getName().equals(obf) || m.getParameterCount() != argc) continue;
            boolean ok = true;
            for (int i = 0; i < argc && ok; i++) {
                Class<?> p = m.getParameterTypes()[i];
                String actual = p.isArray() ? p.getCanonicalName() : p.getName();
                ok = actual != null && actual.equals(obfOf(argTypes[i].trim()));
            }
            if (ok) hits.add(m);
        }
        if (hits.size() != 1)
            throw new NoSuchMethodException(official + "." + name + "/" + argc
                                            + " -> obf " + obf + ": " + hits.size() + " matches");
        hits.get(0).setAccessible(true);
        return hits.get(0);
    }

    static Object construct(String official, Object... args) throws Exception {
        for (Constructor<?> c : cls(official).getDeclaredConstructors()) {
            if (c.getParameterCount() != args.length) continue;
            c.setAccessible(true);
            return c.newInstance(args);
        }
        throw new NoSuchMethodException(official + " has no constructor of " + args.length);
    }

    /// Bind the datapack's tags into the built-in registries.
    ///
    /// This is not optional decoration. Three of the tab generators ask the
    /// registries for a *tag* — `#minecraft:placeable` for paintings,
    /// `#minecraft:goat_horns` for the horns — and a registry whose tags were
    /// never bound answers "empty" rather than failing. The first run of this
    /// oracle produced a creative inventory with **one** painting instead of
    /// twenty-six and **no** goat horn at all, and said nothing. Loading the
    /// vanilla data pack the way the server does is what makes the answer
    /// whole, and the count is the check.
    static int bindVanillaTags() throws Exception {
        // The registries reached here are the same objects BuiltInRegistries
        // holds, so binding into them is what VanillaRegistries.createLookup()
        // will see afterwards.
        Object registryOfRegistries = staticField("net.minecraft.core.registries.BuiltInRegistries",
                                                  "REGISTRY");
        Object access = method("net.minecraft.core.RegistryAccess", "fromRegistryOfRegistries", 1)
                            .invoke(null, registryOfRegistries);
        Object vanillaPack = method("net.minecraft.server.packs.repository.ServerPacksSource",
                                    "createVanillaPackSource", 0).invoke(null);
        Object packType = staticField("net.minecraft.server.packs.PackType", "SERVER_DATA");
        Object resources = construct("net.minecraft.server.packs.resources.MultiPackResourceManager",
                                     packType, List.of(vanillaPack));

        // The whole ReloadableServerResources would also build Commands, which
        // needs the dynamic worldgen registries this access does not carry.
        // TagManager alone needs none of that.
        Object manager = construct("net.minecraft.tags.TagManager", access);
        Class<?> barrierType =
            cls("net.minecraft.server.packs.resources.PreparableReloadListener$PreparationBarrier");
        Object barrier = Proxy.newProxyInstance(
            CreativeTabsOracle.class.getClassLoader(), new Class<?>[]{barrierType},
            (proxy, m, a) -> java.util.concurrent.CompletableFuture.completedFuture(a[0]));
        Object profiler = staticField("net.minecraft.util.profiling.InactiveProfiler", "INSTANCE");
        java.util.concurrent.Executor direct = Runnable::run;
        Object future = method("net.minecraft.tags.TagManager", "reload", 6)
                            .invoke(manager, barrier, resources, profiler, profiler, direct, direct);
        ((java.util.concurrent.CompletableFuture<?>) future).get();

        Method bind = method("net.minecraft.server.ReloadableServerResources",
                             "updateRegistryTags", 2);
        List<?> results = (List<?>) method("net.minecraft.tags.TagManager", "getResult", 0)
                                        .invoke(manager);
        int bound = 0;
        for (Object result : results) {
            bind.invoke(null, access, result);
            bound += ((Map<?, ?>) method("net.minecraft.tags.TagManager$LoadResult", "tags", 0)
                                      .invoke(result)).size();
        }
        return bound;
    }

    // ── Output ──────────────────────────────────────────────────────────────

    static String json(String s) {
        StringBuilder b = new StringBuilder("\"");
        for (char ch : s.toCharArray()) {
            if (ch == '"' || ch == '\\') b.append('\\').append(ch);
            else if (ch < 0x20) b.append(String.format("\\u%04x", (int) ch));
            else b.append(ch);
        }
        return b.append('"').toString();
    }

    public static void main(String[] argv) throws Exception {
        loadMappings(Paths.get(argv[0]));
        boolean opPermissions = Boolean.parseBoolean(argv.length > 1 ? argv[1] : "true");

        method("net.minecraft.SharedConstants", "tryDetectVersion", 0).invoke(null);
        method("net.minecraft.server.Bootstrap", "bootStrap", 0).invoke(null);

        // The flags a vanilla world has, not every flag the registry knows.
        // allFlags() was used first, and it switched on the experimental
        // "bundle" feature: the catalogue then carried a Bundle that the real
        // 1.20.1 client does not show — measured by
        // scripts/measure_creative_screen.py, whose search tab lacks it.
        Object flags = staticField("net.minecraft.world.flag.FeatureFlags", "DEFAULT_FLAGS");

        // Tags first: createLookup() wraps the built-in registries themselves,
        // so binding into them is what the lookup will see.
        int boundTags = bindVanillaTags();
        Object lookup = method("net.minecraft.data.registries.VanillaRegistries",
                               "createLookup", 0).invoke(null);

        // hasPermissions: an operator sees the operator tab's contents. The
        // lab bench runs its player as an operator, so true is the case to
        // reproduce; false is offered for the other one.
        method("net.minecraft.world.item.CreativeModeTabs", "tryRebuildTabContents", 3)
            .invoke(null, flags, opPermissions, lookup);

        Object tabRegistry = staticField("net.minecraft.core.registries.BuiltInRegistries",
                                         "CREATIVE_MODE_TAB");
        Object itemRegistry = staticField("net.minecraft.core.registries.BuiltInRegistries", "ITEM");
        Method getKey = method("net.minecraft.core.Registry", "getKey", 1);

        Method displayItems = method("net.minecraft.world.item.CreativeModeTab", "getDisplayItems", 0);
        Method searchItems = method("net.minecraft.world.item.CreativeModeTab",
                                    "getSearchTabDisplayItems", 0);
        Method iconItem = method("net.minecraft.world.item.CreativeModeTab", "getIconItem", 0);
        Method tabRow = method("net.minecraft.world.item.CreativeModeTab", "row", 0);
        Method tabColumn = method("net.minecraft.world.item.CreativeModeTab", "column", 0);
        Method tabType = method("net.minecraft.world.item.CreativeModeTab", "getType", 0);
        Method alignedRight = method("net.minecraft.world.item.CreativeModeTab", "isAlignedRight", 0);
        Method displayName = method("net.minecraft.world.item.CreativeModeTab", "getDisplayName", 0);
        Method stackItem = method("net.minecraft.world.item.ItemStack", "getItem", 0);
        Method stackCount = method("net.minecraft.world.item.ItemStack", "getCount", 0);
        Method stackTag = method("net.minecraft.world.item.ItemStack", "getTag", 0);
        Method contentsOf = method("net.minecraft.network.chat.Component", "getContents", 0);
        Method translationKey = method("net.minecraft.network.chat.contents.TranslatableContents",
                                       "getKey", 0);
        Method nbtWrite = method("net.minecraft.nbt.NbtIo", "write", 2);
        Class<?> translatable = cls("net.minecraft.network.chat.contents.TranslatableContents");

        StringBuilder out = new StringBuilder();
        out.append("{\n  \"version\": \"1.20.1\",\n  \"op_permissions\": ")
           .append(opPermissions).append(",\n  \"bound_tags\": ").append(boundTags)
           .append(",\n  \"tabs\": [\n");

        @SuppressWarnings("unchecked")
        Iterable<Object> tabs = (Iterable<Object>) tabRegistry;
        boolean firstTab = true;
        for (Object tab : tabs) {
            if (!firstTab) out.append(",\n");
            firstTab = false;

            Object contents = contentsOf.invoke(displayName.invoke(tab));
            String tkey = translatable.isInstance(contents)
                        ? String.valueOf(translationKey.invoke(contents)) : "";

            out.append("    {\n")
               .append("      \"id\": ").append(json(String.valueOf(getKey.invoke(tabRegistry, tab)))).append(",\n")
               .append("      \"translation_key\": ").append(json(tkey)).append(",\n")
               .append("      \"row\": ").append(json(String.valueOf(tabRow.invoke(tab)))).append(",\n")
               .append("      \"column\": ").append(tabColumn.invoke(tab)).append(",\n")
               .append("      \"type\": ").append(json(String.valueOf(tabType.invoke(tab)))).append(",\n")
               .append("      \"aligned_right\": ").append(alignedRight.invoke(tab)).append(",\n")
               .append("      \"icon\": ")
               .append(json(String.valueOf(getKey.invoke(itemRegistry, stackItem.invoke(iconItem.invoke(tab))))))
               .append(",\n");

            for (String which : new String[]{"display", "search"}) {
                @SuppressWarnings("unchecked")
                Collection<Object> stacks = (Collection<Object>)
                    (which.equals("display") ? displayItems.invoke(tab) : searchItems.invoke(tab));
                out.append("      \"").append(which).append("\": [");
                boolean first = true;
                for (Object st : stacks) {
                    if (!first) out.append(",");
                    first = false;
                    String item = String.valueOf(getKey.invoke(itemRegistry, stackItem.invoke(st)));
                    int count = (Integer) stackCount.invoke(st);
                    Object tag = stackTag.invoke(st);
                    if (tag == null && count == 1) {
                        out.append(json(item));
                    } else {
                        out.append("{\"item\":").append(json(item));
                        if (count != 1) out.append(",\"count\":").append(count);
                        if (tag != null) {
                            // Exactly the bytes a Slot carries after item and
                            // count: TAG_Compound, an empty name, the payload.
                            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                            nbtWrite.invoke(null, tag, new DataOutputStream(bytes));
                            out.append(",\"nbt\":")
                               .append(json(Base64.getEncoder().encodeToString(bytes.toByteArray())))
                               .append(",\"snbt\":").append(json(String.valueOf(tag)));
                        }
                        out.append('}');
                    }
                }
                out.append(which.equals("display") ? "],\n" : "]\n");
            }
            out.append("    }");
        }
        out.append("\n  ],\n  \"items\": [");

        @SuppressWarnings("unchecked")
        Iterable<Object> items = (Iterable<Object>) itemRegistry;
        boolean firstItem = true;
        for (Object it : items) {
            if (!firstItem) out.append(",");
            firstItem = false;
            out.append(json(String.valueOf(getKey.invoke(itemRegistry, it))));
        }
        out.append("]\n}\n");
        System.out.print(out);
    }
}
