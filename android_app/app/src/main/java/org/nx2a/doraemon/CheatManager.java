package org.nx2a.doraemon;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Manages built-in v1.1.2 cheats and imported Atmosphere .txt cheat files.
 * Shares exact file format with native cheat_engine.c:
 *   - filesDir/game_data/cheats/*.txt
 *   - filesDir/game_data/cheats_state.tsv   (master=0|1, on=<cheat_id>)
 *
 * ALL cheats and Master switch default to OFF (false).
 */
public final class CheatManager {
    public static final String EXPECTED_BUILD_ID_16 = "306DFCFC9EBA4A1A";

    public static final class Entry {
        public final String id;
        public final String name;
        public final String source;
        public final boolean builtin;
        public final int group; // mutual exclusion group: 1=Friendship, 2=Affection, 3=Gold
        public boolean enabled;

        public Entry(String id, String name, String source, boolean builtin, int group, boolean enabled) {
            this.id = id;
            this.name = name;
            this.source = source;
            this.builtin = builtin;
            this.group = group;
            this.enabled = enabled;
        }
    }

    public static final class State {
        public boolean master = false; // ALWAYS OFF by default!
        public final List<Entry> entries = new ArrayList<>();
    }

    public static final class ImportResult {
        public final String fileName;
        public final int cheatCount;
        public final boolean buildIdMismatch;

        public ImportResult(String fileName, int cheatCount, boolean buildIdMismatch) {
            this.fileName = fileName;
            this.cheatCount = cheatCount;
            this.buildIdMismatch = buildIdMismatch;
        }
    }

    private CheatManager() {}

    public static File getCheatsDir(Context ctx) {
        return new File(CoreAssetSync.getGameDataDir(ctx), "cheats");
    }

    public static File getStateFile(Context ctx) {
        return new File(CoreAssetSync.getGameDataDir(ctx), "cheats_state.tsv");
    }

    public static State load(Context ctx) {
        State st = new State();
        Set<String> enabledIds = new LinkedHashSet<>();

        File sf = getStateFile(ctx);
        if (sf.isFile()) {
            try {
                for (String raw : CoreAssetSync.readFileUtf8(sf).split("\n")) {
                    String line = raw.trim();
                    if (line.startsWith("master=")) {
                        st.master = !"0".equals(line.substring(7).trim());
                    } else if (line.startsWith("on=")) {
                        enabledIds.add(line.substring(3).trim());
                    }
                }
            } catch (IOException ignored) {
            }
        }

        // 1. Built-in verified v1.1.2 cheats (exact match with cheat_engine.c register_builtins())
        addBuiltin(st, enabledIds, "builtin_friend_2x",   "Friendship Warga 2x",    1);
        addBuiltin(st, enabledIds, "builtin_friend_4x",   "Friendship Warga 4x",    1);
        addBuiltin(st, enabledIds, "builtin_animal_2x",   "Affection Hewan 2x",     2);
        addBuiltin(st, enabledIds, "builtin_animal_4x",   "Affection Hewan 4x",     2);
        addBuiltin(st, enabledIds, "builtin_stamina",     "Stamina Tak Terbatas",   0);
        addBuiltin(st, enabledIds, "builtin_water",       "Air Penyiram Tak Habis", 0);
        addBuiltin(st, enabledIds, "builtin_freeze_time", "Waktu Berhenti",         0);
        addBuiltin(st, enabledIds, "builtin_gold_2x",     "Gold / Uang 2x",            3);
        addBuiltin(st, enabledIds, "builtin_gold_4x",     "Gold / Uang 4x",            3);
        addBuiltin(st, enabledIds, "builtin_speed_15x",     "Kecepatan Jalan/Lari 1.5x",      4);
        addBuiltin(st, enabledIds, "builtin_speed_20x",     "Kecepatan Jalan/Lari 2.0x",      4);
        addBuiltin(st, enabledIds, "builtin_inf_held_item", "Item Tak Habis (Slot Dipegang)", 0);

        // 2. Imported Atmosphere .txt files
        File dir = getCheatsDir(ctx);
        if (dir.isDirectory()) {
            File[] files = dir.listFiles((d, name) -> name.toLowerCase(Locale.US).endsWith(".txt"));
            if (files != null) {
                Arrays.sort(files);
                for (File f : files) {
                    parseAtmFile(f, st, enabledIds);
                }
            }
        }
        return st;
    }

    private static void addBuiltin(State st, Set<String> enabledIds, String id, String name, int group) {
        st.entries.add(new Entry(id, name, "builtin", true, group, enabledIds.contains(id)));
    }

    private static void parseAtmFile(File f, State st, Set<String> enabledIds) {
        String content;
        try {
            content = CoreAssetSync.readFileUtf8(f);
        } catch (IOException e) {
            return;
        }
        String fname = f.getName();
        int idx = 0;
        String curName = null;
        int curWords = 0;
        for (String raw : content.split("\n")) {
            String line = raw.trim();
            if (line.startsWith("\uFEFF")) line = line.substring(1).trim();
            if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue;
            boolean isHdr = (line.startsWith("[") && line.endsWith("]")) || (line.startsWith("{") && line.endsWith("}"));
            if (isHdr) {
                if (curName != null && curWords > 0) {
                    String id = "atm:" + fname + ":" + idx;
                    st.entries.add(new Entry(id, curName, fname, false, 0, enabledIds.contains(id)));
                }
                if (curName != null) idx++;
                curName = line.substring(1, line.length() - 1).trim();
                curWords = 0;
                continue;
            }
            if (curName != null) {
                for (String tok : line.split("\\s+")) {
                    if (isHex8(tok)) curWords++;
                }
            }
        }
        if (curName != null && curWords > 0) {
            String id = "atm:" + fname + ":" + idx;
            st.entries.add(new Entry(id, curName, fname, false, 0, enabledIds.contains(id)));
        }
    }

    private static boolean isHex8(String s) {
        if (s == null || s.length() != 8) return false;
        for (int i = 0; i < 8; i++) {
            char c = s.charAt(i);
            boolean ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!ok) return false;
        }
        return true;
    }

    public static void save(Context ctx, State st) throws IOException {
        File gd = CoreAssetSync.getGameDataDir(ctx);
        if (!gd.exists()) {
            //noinspection ResultOfMethodCallIgnored
            gd.mkdirs();
        }
        StringBuilder sb = new StringBuilder();
        sb.append("master=").append(st.master ? 1 : 0).append('\n');
        for (Entry e : st.entries) {
            if (e.enabled) sb.append("on=").append(e.id).append('\n');
        }
        CoreAssetSync.writeUtf8Atomically(getStateFile(ctx), sb.toString());
    }

    public static void toggleEntry(State st, Entry target, boolean enabled) {
        if (enabled && target.group != 0) {
            for (Entry e : st.entries) {
                if (e != target && e.group == target.group) e.enabled = false;
            }
        }
        target.enabled = enabled;
        if (enabled && !st.master) st.master = true;
    }

    public static ImportResult importCheatFile(Context ctx, Uri uri) throws IOException {
        String rawName = queryDisplayName(ctx, uri);
        if (rawName == null || rawName.trim().isEmpty()) rawName = "imported_cheats.txt";
        String safeName = rawName.replaceAll("[^A-Za-z0-9._-]", "_");
        if (!safeName.toLowerCase(Locale.US).endsWith(".txt")) safeName += ".txt";

        byte[] data;
        try (InputStream is = ctx.getContentResolver().openInputStream(uri)) {
            if (is == null) throw new IOException("Cannot open selected cheat file");
            ByteArrayOutputStream bos = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            int total = 0;
            while ((n = is.read(buf)) > 0) {
                total += n;
                if (total > 2 * 1024 * 1024) throw new IOException("Cheat file too large (>2 MB)");
                bos.write(buf, 0, n);
            }
            data = bos.toByteArray();
        }
        String text = new String(data, StandardCharsets.UTF_8);

        File dir = getCheatsDir(ctx);
        if (!dir.exists()) {
            //noinspection ResultOfMethodCallIgnored
            dir.mkdirs();
        }
        File outFile = new File(dir, safeName);
        CoreAssetSync.writeUtf8Atomically(outFile, text);

        State tmp = new State();
        parseAtmFile(outFile, tmp, new LinkedHashSet<>());

        // Check if filename looks like a 16-hex Switch Build ID that differs from v1.1.2 (306DFCFC9EBA4A1A)
        String stem = safeName.substring(0, safeName.length() - 4).toUpperCase(Locale.US);
        boolean looksHex16 = stem.length() == 16 && stem.matches("[0-9A-F]{16}");
        boolean mismatch = looksHex16 && !EXPECTED_BUILD_ID_16.equals(stem);

        return new ImportResult(safeName, tmp.entries.size(), mismatch);
    }

    public static int clearImportedFiles(Context ctx) {
        int removed = 0;
        File dir = getCheatsDir(ctx);
        if (dir.isDirectory()) {
            File[] files = dir.listFiles();
            if (files != null) {
                for (File f : files) {
                    if (f.isFile() && f.delete()) removed++;
                }
            }
        }
        try {
            State st = load(ctx);
            save(ctx, st);
        } catch (IOException ignored) {
        }
        return removed;
    }

    private static String queryDisplayName(Context ctx, Uri uri) {
        if (uri == null) return null;
        try (Cursor c = ctx.getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Exception ignored) {
        }
        String p = uri.getLastPathSegment();
        return p != null ? new File(p).getName() : null;
    }
}
