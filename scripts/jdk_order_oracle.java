// The iteration orders the dedicated server's files owe to the JDK.
//
// No Minecraft code: plain java.util, run with the JDK 1.20.1 ships with
// (17). It prints the orders src/ov_server/src/admin/java_compat.cpp
// re-specifies, so the unit tests pin JDK-produced values rather than our
// own reading of the documentation:
//
//   properties-<case>  the keys of a Properties filled the way the server
//                      fills it (a file loaded, then each setting's default
//                      put), copied into a fresh Properties by putAll — the
//                      copy's iteration is the order Properties.store writes;
//   hashmap-<case>     a HashMap<String, …> filled in a given order, keyed by
//                      offline uuids or IP addresses.
//
// Usage: java -Xmx32m scripts/jdk_order_oracle.java
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.UUID;

public class jdk_order_oracle {
    // Keep in the order of kDefaults in server_properties.cpp.
    static final String[] DEFAULTS = {
        "online-mode", "prevent-proxy-connections", "server-ip", "spawn-animals", "spawn-npcs",
        "pvp", "allow-flight", "motd", "force-gamemode", "enforce-whitelist", "difficulty",
        "gamemode", "level-name", "server-port", "enable-query", "query.port", "enable-rcon",
        "rcon.port", "rcon.password", "hardcore", "allow-nether", "spawn-monsters",
        "use-native-transport", "enable-command-block", "spawn-protection", "op-permission-level",
        "function-permission-level", "max-tick-time", "max-chained-neighbor-updates", "rate-limit",
        "view-distance", "simulation-distance", "max-players", "network-compression-threshold",
        "broadcast-rcon-to-ops", "broadcast-console-to-ops", "max-world-size", "sync-chunk-writes",
        "enable-jmx-monitoring", "enable-status", "hide-online-players",
        "entity-broadcast-range-percentage", "text-filtering-config", "player-idle-timeout",
        "white-list", "enforce-secure-profile", "level-seed", "generator-settings",
        "level-type", "generate-structures", "initial-enabled-packs", "initial-disabled-packs",
        // The server resource pack is read last, after the world's settings.
        "resource-pack", "require-resource-pack", "resource-pack-prompt", "resource-pack-sha1",
    };

    /// The same Properties, iterated as it is — no copy. Stored this way,
    /// the table is the one the puts grew (to 128 buckets for 57 keys), not
    /// the one putAll presizes (256).
    static void direct(String name, String[] fromFile) {
        Properties loaded = new Properties();
        for (String key : fromFile) {
            loaded.put(key, "x");
        }
        for (String key : DEFAULTS) {
            loaded.put(key, "x");
        }
        List<String> keys = new ArrayList<>();
        for (Map.Entry<Object, Object> e : loaded.entrySet()) {
            keys.add((String) e.getKey());
        }
        System.out.println("direct-" + name + " " + String.join(",", keys));
    }

    static void properties(String name, String[] fromFile) {
        Properties loaded = new Properties();
        for (String key : fromFile) {
            loaded.put(key, "x");
        }
        for (String key : DEFAULTS) {
            loaded.put(key, "x");  // get(): an existing key keeps its place
        }
        Properties copy = new Properties();
        copy.putAll(loaded);
        List<String> keys = new ArrayList<>();
        for (Map.Entry<Object, Object> e : copy.entrySet()) {
            keys.add((String) e.getKey());
        }
        System.out.println("properties-" + name + " " + String.join(",", keys));
    }

    /// What a first start writes: the table iterated as it is.
    static String[] freshOrder() {
        Properties loaded = new Properties();
        for (String key : DEFAULTS) {
            loaded.put(key, "x");
        }
        List<String> keys = new ArrayList<>();
        for (Map.Entry<Object, Object> e : loaded.entrySet()) {
            keys.add((String) e.getKey());
        }
        return keys.toArray(new String[0]);
    }

    static void hashMap(String name, List<String> keys) {
        Map<String, Integer> map = new HashMap<>();
        for (String key : keys) {
            map.put(key, 0);
        }
        System.out.println("hashmap-" + name + " " + String.join(",", map.keySet()));
    }

    static String offline(String player) {
        return UUID.nameUUIDFromBytes(("OfflinePlayer:" + player).getBytes(StandardCharsets.UTF_8))
            .toString();
    }

    public static void main(String[] args) {
        properties("fresh", new String[] {});
        properties("capture", new String[] {"server-port", "ov-unknown-key", "motd"});
        // A second start reads back the file the first one wrote, in its order.
        properties("restart", freshOrder());
        direct("fresh", new String[] {});
        direct("capture", new String[] {"server-port", "ov-unknown-key", "motd"});
        // A second start of the jar: its own first file read back, in order.
        direct("restart", freshOrder());
        List<String> uuids = new ArrayList<>();
        for (String player : new String[] {"Ovq_tempban", "Ovq_oldban", "Ovq_alice", "Ovq_bobby",
                                           "ovprobe", "Alice", "Bob", "Carol", "Dave", "Erin",
                                           "Frank", "Grace", "Heidi", "Ivan"}) {
            uuids.add(offline(player));
        }
        hashMap("uuids", uuids);
        hashMap("ips", List.of("10.9.9.9", "10.0.0.1", "10.0.0.2", "127.0.0.1", "10.0.0.3"));
    }
}
