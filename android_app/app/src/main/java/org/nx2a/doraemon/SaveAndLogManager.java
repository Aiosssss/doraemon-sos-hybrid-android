package org.nx2a.doraemon;

import android.content.Context;
import android.net.Uri;
import android.os.Build;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.LinkedHashSet;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Handles SAF export of runtime/crash logs (log.txt + log.prev.txt) and
 * atomic ZIP backup / restore of internal game saves (filesDir/save/).
 */
public final class SaveAndLogManager {
    private static final String LAUNCHER_LOG_FILE = "launcher_extract.log";
    private static final long MAX_LAUNCHER_LOG_BYTES = 262144L; // 256 KiB
    private static final Object LOG_LOCK = new Object();

    private SaveAndLogManager() {}

    /**
     * Appends a timestamped Launcher / Extractor diagnostic line into filesDir/launcher_extract.log.
     * Stored outside game_data/ so extraction errors are always preserved and exportable even before
     * game_data/log.txt exists. Never pass secret key material to this method.
     */
    public static void appendLauncherLog(Context ctx, String tag, String msg) {
        if (ctx == null || msg == null) return;
        synchronized (LOG_LOCK) {
            try {
                File f = new File(ctx.getFilesDir(), LAUNCHER_LOG_FILE);
                if (f.isFile() && f.length() > MAX_LAUNCHER_LOG_BYTES) {
                    File prev = new File(ctx.getFilesDir(), LAUNCHER_LOG_FILE + ".prev");
                    //noinspection ResultOfMethodCallIgnored
                    prev.delete();
                    //noinspection ResultOfMethodCallIgnored
                    f.renameTo(prev);
                }
                String ts = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
                String line = "[" + ts + "] [" + (tag != null ? tag : "launcher") + "] " + msg + "\n";
                try (FileOutputStream fos = new FileOutputStream(f, true)) {
                    fos.write(line.getBytes(StandardCharsets.UTF_8));
                    fos.flush();
                }
            } catch (Throwable ignored) {
            }
        }
    }

    public static final String JAVA_CRASH_LOG_FILE = "crash_java.log";
    private static volatile boolean sCrashHandlerInstalled = false;

    /**
     * Installs a process-wide Java UncaughtExceptionHandler that persists full stack traces
     * to game_data/crash_java.log before delegating to the system handler (zero silent Java FCs).
     */
    public static void installJavaCrashHandler(final Context ctx) {
        if (sCrashHandlerInstalled || ctx == null) return;
        sCrashHandlerInstalled = true;
        final Context appCtx = ctx.getApplicationContext();
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                File gameDir = CoreAssetSync.getGameDataDir(appCtx);
                if (!gameDir.exists()) gameDir.mkdirs();
                File out = new File(gameDir, JAVA_CRASH_LOG_FILE);
                String ts = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date());
                StringBuilder sb = new StringBuilder();
                sb.append("=== JAVA UNCAUGHT EXCEPTION ===\n");
                sb.append("Time: ").append(ts).append("\n");
                sb.append("APK: ").append(BuildConfig.APPLICATION_ID)
                        .append(" v").append(BuildConfig.VERSION_NAME)
                        .append(" (").append(BuildConfig.VERSION_CODE).append(")\n");
                sb.append("Device: ").append(Build.MANUFACTURER).append(" ").append(Build.MODEL)
                        .append(" (Android ").append(Build.VERSION.RELEASE)
                        .append(", API ").append(Build.VERSION.SDK_INT).append(")\n");
                sb.append("Thread: ").append(thread != null ? thread.getName() : "unknown").append("\n");
                java.io.StringWriter sw = new java.io.StringWriter();
                throwable.printStackTrace(new java.io.PrintWriter(sw));
                sb.append(sw.toString()).append("\n");
                try (FileOutputStream fos = new FileOutputStream(out, false)) {
                    fos.write(sb.toString().getBytes(StandardCharsets.UTF_8));
                    fos.getFD().sync();
                }
            } catch (Throwable ignored) {
            }
            if (prev != null) {
                prev.uncaughtException(thread, throwable);
            }
        });
    }

    /**
     * Exports diagnostics header + launcher_extract.log + crash_java.log + last_launch_error.txt + log.txt + log.prev.txt
     * to a SAF destination URI. Always succeeds even before game_data/log.txt exists.
     */
    public static int exportRuntimeLogs(Context ctx, Uri destUri, String vulkanProbeJson) throws IOException {
        File gameDir = CoreAssetSync.getGameDataDir(ctx);
        File launcherLog = new File(ctx.getFilesDir(), LAUNCHER_LOG_FILE);
        File javaCrashLog = new File(gameDir, JAVA_CRASH_LOG_FILE);
        File lastErr = new File(gameDir, "last_launch_error.txt");
        File curLog = new File(gameDir, "log.txt");
        File prevLog = new File(gameDir, "log.prev.txt");
        CoreAssetSync.Status st = CoreAssetSync.inspectStatus(ctx);

        int totalBytes = 0;
        try (OutputStream out = ctx.getContentResolver().openOutputStream(destUri, "wt")) {
            if (out == null) throw new IOException("Cannot open destination URI for writing");

            String ts = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss Z", Locale.US).format(new Date());
            String header = "=== Doraemon SoS Hybrid Runtime & Crash Log Report ===\n"
                    + "Exported: " + ts + "\n"
                    + "APK: " + BuildConfig.APPLICATION_ID + " v" + BuildConfig.VERSION_NAME
                    + " (" + BuildConfig.VERSION_CODE + ")\n"
                    + "Device: " + Build.MANUFACTURER + " " + Build.MODEL
                    + " (Android " + Build.VERSION.RELEASE + ", API " + Build.VERSION.SDK_INT + ")\n"
                    + "Install Status: " + st.summary + "\n"
                    + "Vulkan Probe: " + (vulkanProbeJson != null && !vulkanProbeJson.isEmpty() ? vulkanProbeJson : "(unavailable)") + "\n\n";
            byte[] hb = header.getBytes(StandardCharsets.UTF_8);
            out.write(hb);
            totalBytes += hb.length;

            totalBytes += appendLogSection(out, "=== LAUNCHER & EXTRACT LOG (launcher_extract.log) ===", launcherLog,
                    "(no extraction/launcher events recorded yet)\n");
            if (javaCrashLog.isFile() && javaCrashLog.length() > 0) {
                totalBytes += appendLogSection(out, "\n=== JAVA CRASH LOG (game_data/crash_java.log) ===", javaCrashLog,
                        "(none)\n");
            }
            if (lastErr.isFile() && lastErr.length() > 0) {
                totalBytes += appendLogSection(out, "\n=== LAST LAUNCH ERROR (game_data/last_launch_error.txt) ===", lastErr,
                        "(none)\n");
            }
            totalBytes += appendLogSection(out, "\n=== CURRENT RUNTIME LOG (game_data/log.txt) ===", curLog,
                    "(file not present or empty — launch the game first)\n");
            totalBytes += appendLogSection(out, "\n=== PREVIOUS RUNTIME LOG (game_data/log.prev.txt) ===", prevLog,
                    "(file not present or empty)\n");
            out.flush();
        }
        return totalBytes;
    }

    private static int appendLogSection(OutputStream out, String title, File f, String emptyNote) throws IOException {
        int written = 0;
        byte[] tb = (title + "\n").getBytes(StandardCharsets.UTF_8);
        out.write(tb);
        written += tb.length;

        if (!f.isFile() || f.length() == 0) {
            byte[] empty = emptyNote.getBytes(StandardCharsets.UTF_8);
            out.write(empty);
            return written + empty.length;
        }
        byte[] buf = new byte[32768];
        try (FileInputStream in = new FileInputStream(f)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                written += n;
            }
        }
        return written;
    }

    /**
     * Zips filesDir/save/ recursively to a SAF destination URI. Returns number of files archived.
     */
    public static int exportSaveZip(Context ctx, Uri destUri) throws IOException {
        File saveDir = new File(ctx.getFilesDir(), "save");
        if (!saveDir.isDirectory()) {
            throw new IOException("No save directory exists yet. Launch the game and save first.");
        }
        int[] count = {0};
        try (OutputStream rawOut = ctx.getContentResolver().openOutputStream(destUri, "w")) {
            if (rawOut == null) throw new IOException("Cannot open backup ZIP URI for writing");
            try (ZipOutputStream zos = new ZipOutputStream(new BufferedOutputStream(rawOut))) {
                zipDirectory(saveDir, "", zos, count);
                zos.finish();
            }
        }
        if (count[0] == 0) {
            throw new IOException("Save folder is empty (0 files found).");
        }
        return count[0];
    }

    private static void zipDirectory(File dir, String prefix, ZipOutputStream zos, int[] count) throws IOException {
        File[] children = dir.listFiles();
        if (children == null) return;
        byte[] buf = new byte[32768];
        for (File f : children) {
            String rel = prefix.isEmpty() ? f.getName() : (prefix + "/" + f.getName());
            if (f.isDirectory()) {
                zipDirectory(f, rel, zos, count);
            } else if (f.isFile()) {
                ZipEntry entry = new ZipEntry(rel);
                entry.setTime(f.lastModified());
                zos.putNextEntry(entry);
                try (FileInputStream in = new FileInputStream(f)) {
                    int n;
                    while ((n = in.read(buf)) > 0) zos.write(buf, 0, n);
                }
                zos.closeEntry();
                count[0]++;
            }
        }
    }

    /**
     * Atomically restores a Save ZIP into filesDir/save/ with Zip-Slip path traversal protection
     * and automatic rollback if anything fails. Returns number of files restored.
     */
    public static int importSaveZip(Context ctx, Uri srcUri) throws IOException {
        File filesDir = ctx.getFilesDir();
        File saveDir = new File(filesDir, "save");
        File tmpDir = new File(filesDir, "save_import_tmp");
        File bakDir = new File(filesDir, "save_backup_prev");
        deleteRecursive(tmpDir);
        if (!tmpDir.mkdirs()) throw new IOException("Cannot create temporary restore directory");

        String canonicalTmp = tmpDir.getCanonicalPath() + File.separator;
        int restoredFiles = 0;

        try (InputStream rawIn = ctx.getContentResolver().openInputStream(srcUri)) {
            if (rawIn == null) throw new IOException("Cannot open selected Save ZIP");
            byte[] buf = new byte[32768];
            try (ZipInputStream zis = new ZipInputStream(rawIn)) {
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    String name = entry.getName();
                    if (name == null || name.isEmpty()) continue;
                    File outFile = new File(tmpDir, name);
                    // Zip-Slip guard
                    if (!outFile.getCanonicalPath().startsWith(canonicalTmp)) {
                        throw new IOException("Blocked unsafe ZIP entry path: " + name);
                    }
                    if (entry.isDirectory()) {
                        if (!outFile.exists() && !outFile.mkdirs()) {
                            throw new IOException("Cannot create directory: " + name);
                        }
                    } else {
                        File parent = outFile.getParentFile();
                        if (parent != null && !parent.exists() && !parent.mkdirs()) {
                            throw new IOException("Cannot create parent dir for: " + name);
                        }
                        try (FileOutputStream fos = new FileOutputStream(outFile)) {
                            int n;
                            while ((n = zis.read(buf)) > 0) fos.write(buf, 0, n);
                            fos.flush();
                            fos.getFD().sync();
                        }
                        restoredFiles++;
                    }
                    zis.closeEntry();
                }
            }
            if (restoredFiles == 0) {
                throw new IOException("ZIP archive contains no save files");
            }

            // Atomic swap with rollback
            deleteRecursive(bakDir);
            boolean hadOld = saveDir.exists();
            if (hadOld && !saveDir.renameTo(bakDir)) {
                throw new IOException("Cannot move previous save directory aside");
            }
            if (!tmpDir.renameTo(saveDir)) {
                if (hadOld) {
                    //noinspection ResultOfMethodCallIgnored
                    bakDir.renameTo(saveDir);
                }
                throw new IOException("Atomic swap of restored save failed");
            }
            if (hadOld) deleteRecursive(bakDir);
            return restoredFiles;
        } catch (IOException e) {
            deleteRecursive(tmpDir);
            throw e;
        }
    }

    private static void deleteRecursive(File f) {
        if (f == null || !f.exists()) return;
        if (f.isDirectory()) {
            File[] children = f.listFiles();
            if (children != null) {
                for (File c : children) deleteRecursive(c);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    // ---- RomFS Mod Overlay (.zip) Installer & Instant Restore (e.g. Mod Bahasa Indonesia) ----

    public static final String MOD_MANIFEST_FILE = ".mod_overlay.json";
    public static final String MOD_BACKUP_DIR = "romfs_orig_backup";

    public static final class ModStatus {
        public boolean active;
        public String modName = "";
        public String installedAt = "";
        public int totalFiles;
        public int overwrittenCount;
        public int addedCount;
    }

    public static ModStatus getRomfsModStatus(Context ctx) {
        ModStatus st = new ModStatus();
        File manifest = new File(CoreAssetSync.getGameDataDir(ctx), MOD_MANIFEST_FILE);
        if (!manifest.isFile()) return st;
        try {
            JSONObject j = new JSONObject(CoreAssetSync.readFileUtf8(manifest));
            st.active = j.optBoolean("active", false);
            st.modName = j.optString("mod_name", "RomFS Mod Overlay");
            st.installedAt = j.optString("installed_at", "");
            JSONArray ov = j.optJSONArray("overwritten");
            JSONArray ad = j.optJSONArray("added");
            st.overwrittenCount = ov != null ? ov.length() : 0;
            st.addedCount = ad != null ? ad.length() : 0;
            st.totalFiles = st.overwrittenCount + st.addedCount;
        } catch (Exception ignored) {
            st.active = false;
        }
        return st;
    }

    private static String normalizeModEntryPath(String rawName) {
        if (rawName == null) return null;
        String p = rawName.replace('\\', '/').trim();
        while (p.startsWith("/")) p = p.substring(1);
        while (p.startsWith("./")) p = p.substring(2);
        if (p.isEmpty() || p.endsWith("/")) return null;
        if (p.startsWith("__MACOSX/") || p.contains("/__MACOSX/")) return null;
        String base = p.substring(p.lastIndexOf('/') + 1);
        if (base.equals(".DS_Store") || base.equals("Thumbs.db") || base.startsWith("._")) return null;

        // Support both "RomFS/Data/...", "<folder>/RomFS/Data/...", and direct "Data/..." ZIP roots.
        String lower = p.toLowerCase(Locale.US);
        if (lower.startsWith("romfs/")) {
            p = p.substring(6);
        } else {
            int idx = lower.indexOf("/romfs/");
            if (idx >= 0) {
                p = p.substring(idx + 7);
            }
        }
        while (p.startsWith("/")) p = p.substring(1);
        return p.isEmpty() ? null : p;
    }

    private static void copyFileAtomic(File src, File dst) throws IOException {
        File parent = dst.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Cannot create directory: " + parent.getAbsolutePath());
        }
        File tmp = new File(dst.getParentFile(), dst.getName() + ".modtmp");
        byte[] buf = new byte[65536];
        try (FileInputStream in = new FileInputStream(src);
             FileOutputStream out = new FileOutputStream(tmp)) {
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.flush();
            out.getFD().sync();
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw e;
        }
        if (!tmp.renameTo(dst)) {
            if (dst.exists()) {
                //noinspection ResultOfMethodCallIgnored
                dst.delete();
            }
            if (!tmp.renameTo(dst)) {
                //noinspection ResultOfMethodCallIgnored
                tmp.delete();
                throw new IOException("Atomic replace failed for " + dst.getName());
            }
        }
    }

    /**
     * Installs a RomFS Mod Overlay (.zip) directly into filesDir/game_data/RomFS/ with:
     * - Strict Zip-Slip canonical-path validation
     * - First-time automatic backup of overwritten original RomFS files to filesDir/game_data/romfs_orig_backup/
     * - Atomic per-file replacement and persistent .mod_overlay.json manifest for 1-second Restore Original.
     */
    public static ModStatus installRomfsModZip(Context ctx, Uri srcUri, String displayName) throws IOException {
        File gameDir = CoreAssetSync.getGameDataDir(ctx);
        File romfsDir = new File(gameDir, "RomFS");
        if (!romfsDir.isDirectory()) {
            throw new IOException("RomFS folder not found. Extract Base + Update NSP first.");
        }
        File backupDir = new File(gameDir, MOD_BACKUP_DIR);
        if (!backupDir.exists() && !backupDir.mkdirs()) {
            throw new IOException("Cannot create original RomFS backup directory");
        }

        // Preserve any prior mod manifest entries so installing a second mod update never loses original backups.
        Set<String> overwritten = new LinkedHashSet<>();
        Set<String> added = new LinkedHashSet<>();
        File manifestFile = new File(gameDir, MOD_MANIFEST_FILE);
        if (manifestFile.isFile()) {
            try {
                JSONObject prev = new JSONObject(CoreAssetSync.readFileUtf8(manifestFile));
                JSONArray ov = prev.optJSONArray("overwritten");
                if (ov != null) for (int i = 0; i < ov.length(); i++) overwritten.add(ov.getString(i));
                JSONArray ad = prev.optJSONArray("added");
                if (ad != null) for (int i = 0; i < ad.length(); i++) added.add(ad.getString(i));
            } catch (Exception ignored) {
            }
        }

        String canonicalRomfs = romfsDir.getCanonicalPath() + File.separator;
        String canonicalBak = backupDir.getCanonicalPath() + File.separator;
        int appliedCount = 0;
        byte[] buf = new byte[65536];

        try (InputStream rawIn = ctx.getContentResolver().openInputStream(srcUri)) {
            if (rawIn == null) throw new IOException("Cannot open selected Mod ZIP");
            try (ZipInputStream zis = new ZipInputStream(rawIn)) {
                ZipEntry entry;
                while ((entry = zis.getNextEntry()) != null) {
                    if (entry.isDirectory()) {
                        zis.closeEntry();
                        continue;
                    }
                    String rel = normalizeModEntryPath(entry.getName());
                    if (rel == null) {
                        zis.closeEntry();
                        continue;
                    }
                    File dest = new File(romfsDir, rel);
                    File bak = new File(backupDir, rel);
                    // Strict Zip-Slip path traversal guard on both target and backup paths
                    if (!dest.getCanonicalPath().startsWith(canonicalRomfs)
                            || !bak.getCanonicalPath().startsWith(canonicalBak)) {
                        throw new IOException("Blocked unsafe Mod ZIP path: " + entry.getName());
                    }

                    // Backup original untouched RomFS file once (never overwrite an existing original backup!)
                    if (!overwritten.contains(rel) && !added.contains(rel)) {
                        if (dest.isFile()) {
                            if (!bak.exists()) {
                                copyFileAtomic(dest, bak);
                            }
                            overwritten.add(rel);
                        } else {
                            added.add(rel);
                        }
                    }

                    File parent = dest.getParentFile();
                    if (parent != null && !parent.exists() && !parent.mkdirs()) {
                        throw new IOException("Cannot create RomFS directory for: " + rel);
                    }
                    File tmp = new File(parent, dest.getName() + ".modtmp");
                    try (FileOutputStream fos = new FileOutputStream(tmp)) {
                        int n;
                        while ((n = zis.read(buf)) > 0) fos.write(buf, 0, n);
                        fos.flush();
                        fos.getFD().sync();
                    } catch (IOException e) {
                        //noinspection ResultOfMethodCallIgnored
                        tmp.delete();
                        throw e;
                    }
                    if (!tmp.renameTo(dest)) {
                        if (dest.exists()) {
                            //noinspection ResultOfMethodCallIgnored
                            dest.delete();
                        }
                        if (!tmp.renameTo(dest)) {
                            //noinspection ResultOfMethodCallIgnored
                            tmp.delete();
                            throw new IOException("Atomic write failed for RomFS/" + rel);
                        }
                    }
                    appliedCount++;
                    zis.closeEntry();
                }
            }
        }

        if (appliedCount == 0) {
            throw new IOException("Mod ZIP contains no applicable RomFS files.");
        }

        String ts = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US).format(new Date());
        try {
            JSONObject manifest = new JSONObject();
            manifest.put("active", true);
            manifest.put("mod_name", (displayName != null && !displayName.isEmpty()) ? displayName : "RomFS Mod Overlay");
            manifest.put("installed_at", ts);
            manifest.put("last_applied_files", appliedCount);
            JSONArray ovArr = new JSONArray();
            for (String s : overwritten) ovArr.put(s);
            JSONArray adArr = new JSONArray();
            for (String s : added) adArr.put(s);
            manifest.put("overwritten", ovArr);
            manifest.put("added", adArr);
            CoreAssetSync.writeUtf8Atomically(manifestFile, manifest.toString(2));
        } catch (Exception e) {
            throw new IOException("Failed writing mod overlay manifest: " + e.getMessage(), e);
        }

        return getRomfsModStatus(ctx);
    }

    /**
     * Disables active RomFS mod overlay and restores all backed-up original RomFS files in < 1 second
     * without re-extracting the 2.24 GiB Base/Update NSP. Returns total files restored + cleaned.
     */
    public static int restoreOriginalRomfs(Context ctx) throws IOException {
        File gameDir = CoreAssetSync.getGameDataDir(ctx);
        File romfsDir = new File(gameDir, "RomFS");
        File backupDir = new File(gameDir, MOD_BACKUP_DIR);
        File manifestFile = new File(gameDir, MOD_MANIFEST_FILE);
        if (!manifestFile.isFile()) {
            deleteRecursive(backupDir);
            return 0;
        }
        String canonicalRomfs = romfsDir.getCanonicalPath() + File.separator;
        String canonicalBak = backupDir.getCanonicalPath() + File.separator;
        int restored = 0;

        try {
            JSONObject j = new JSONObject(CoreAssetSync.readFileUtf8(manifestFile));
            JSONArray ov = j.optJSONArray("overwritten");
            if (ov != null) {
                for (int i = 0; i < ov.length(); i++) {
                    String rel = ov.getString(i);
                    File bak = new File(backupDir, rel);
                    File dest = new File(romfsDir, rel);
                    if (!dest.getCanonicalPath().startsWith(canonicalRomfs)
                            || !bak.getCanonicalPath().startsWith(canonicalBak)) {
                        continue;
                    }
                    if (bak.isFile()) {
                        copyFileAtomic(bak, dest);
                        restored++;
                    }
                }
            }
            JSONArray ad = j.optJSONArray("added");
            if (ad != null) {
                for (int i = 0; i < ad.length(); i++) {
                    String rel = ad.getString(i);
                    File dest = new File(romfsDir, rel);
                    if (!dest.getCanonicalPath().startsWith(canonicalRomfs)) continue;
                    if (dest.isFile() && dest.delete()) {
                        restored++;
                    }
                }
            }
        } catch (Exception e) {
            throw new IOException("Failed restoring original RomFS: " + e.getMessage(), e);
        }

        deleteRecursive(backupDir);
        //noinspection ResultOfMethodCallIgnored
        manifestFile.delete();
        return restored;
    }
}
