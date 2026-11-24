package org.nx2a.doraemon;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

/**
 * Atomic core-asset synchronization and installation integrity verifier.
 *
 * Responsibilities:
 *  1. Extract APK-bundled NXPT patch recipes (assets/core/patches/*) into cacheDir/nxpatches/
 *     before invoking nativeExtractGame().
 *  2. Atomically sync assets/core/shaders.dmsv (and default env.txt if absent) into
 *     filesDir/game_data/ using .tmp + fsync + rename, recording SHA-256 and APK version in
 *     .core_assets.json.
 *  3. On APK upgrades, automatically refresh core assets without forcing users to re-extract NSPs.
 *  4. Verify both game_data/.ready.json (extracted game files) and .core_assets.json (core bundle)
 *     before enabling the Play button.
 */
public final class CoreAssetSync {
    private static final String TAG = "CoreAssetSync";

    public static final String GAME_DATA_DIR = "game_data";
    public static final String READY_MANIFEST = ".ready.json";
    public static final String CORE_MANIFEST = ".core_assets.json";
    public static final String SHADERS_FILE = "shaders.dmsv";
    public static final String ENV_FILE = "env.txt";
    public static final String SETTINGS_FILE = "settings.txt";

    private static final String[] PATCH_MODULES = {"rtld", "main", "subsdk0", "sdk"};
    private static final String[] PATCH_EXTS = {".nxpatch", ".fixups.tsv", ".gates.tsv"};

    private CoreAssetSync() {}

    public static File getGameDataDir(Context ctx) {
        return new File(ctx.getFilesDir(), GAME_DATA_DIR);
    }

    public static File getReadyManifestFile(Context ctx) {
        return new File(getGameDataDir(ctx), READY_MANIFEST);
    }

    public static File getCoreManifestFile(Context ctx) {
        return new File(getGameDataDir(ctx), CORE_MANIFEST);
    }

    public static final class Status {
        public boolean gameExtracted;
        public boolean coreSynced;
        public boolean readyToPlay;
        public String titleId = "";
        public String displayVersion = "";
        public long romfsFiles = 0;
        public long romfsBytes = 0;
        public String aocSummary = "None";
        public String mainBuildId = "";
        public String sdkBuildId = "";
        public String subsdk0BuildId = "";
        public String rtldBuildId = "";
        public String shadersSha256 = "";
        public int syncedApkVersionCode = 0;
        public String syncedApkVersionName = "";
        public String summary = "";
    }

    /** Reads bundled assets/core/profile.json as raw UTF-8 string. */
    public static String readBundledProfileJson(Context ctx) throws IOException {
        try (InputStream in = ctx.getAssets().open("core/profile.json")) {
            return new String(readAllBytes(in), StandardCharsets.UTF_8);
        }
    }

    /**
     * Atomically copies all 12 NXPT recipe files from assets/core/patches/ into cacheDir/nxpatches/.
     * Returns the prepared directory path for NativeBridge.nativeExtractGame().
     */
    public static File preparePatchesDir(Context ctx) throws IOException {
        File patchesDir = new File(ctx.getCacheDir(), "nxpatches");
        if (!patchesDir.exists() && !patchesDir.mkdirs()) {
            throw new IOException("Cannot create cache patches dir: " + patchesDir);
        }
        AssetManager am = ctx.getAssets();
        for (String mod : PATCH_MODULES) {
            for (String ext : PATCH_EXTS) {
                String fname = mod + ext;
                String assetPath = "core/patches/" + fname;
                File dst = new File(patchesDir, fname);
                copyAssetAtomically(am, assetPath, dst);
            }
        }
        return patchesDir;
    }

    /**
     * Checks whether game_data/ has a valid extraction AND whether core assets match the current APK.
     */
    public static Status inspectStatus(Context ctx) {
        Status st = new Status();
        File gameDir = getGameDataDir(ctx);
        File readyFile = getReadyManifestFile(ctx);

        // 1. Validate game extraction (.ready.json + required patched binaries + RomFS)
        if (readyFile.isFile()) {
            try {
                JSONObject j = new JSONObject(readFileUtf8(readyFile));
                st.titleId = j.optString("title_id", "");
                st.displayVersion = j.optString("display_version", "");
                st.romfsFiles = j.optLong("romfs_files", 0);
                st.romfsBytes = j.optLong("romfs_bytes", 0);
                JSONObject bids = j.optJSONObject("build_ids");
                if (bids != null) {
                    st.rtldBuildId = bids.optString("rtld", "");
                    st.mainBuildId = bids.optString("main", "");
                    st.subsdk0BuildId = bids.optString("subsdk0", "");
                    st.sdkBuildId = bids.optString("sdk", "");
                }
                JSONArray aoc = j.optJSONArray("aoc_indices");
                if (aoc != null && aoc.length() > 0) {
                    StringBuilder sb = new StringBuilder();
                    for (int i = 0; i < aoc.length(); i++) {
                        if (i > 0) sb.append(", ");
                        sb.append(aoc.getInt(i));
                    }
                    st.aocSummary = sb.toString();
                } else {
                    st.aocSummary = "None (0 DLC — Opsional)";
                }

                boolean filesIntact = new File(gameDir, "RomFS").isDirectory();
                for (String mod : PATCH_MODULES) {
                    if (!new File(gameDir, "mods/" + mod + ".patched.bin").isFile()) {
                        filesIntact = false;
                        break;
                    }
                }
                if (filesIntact) {
                    File aocDir = new File(gameDir, "aoc");
                    if (!aocDir.exists()) {
                        //noinspection ResultOfMethodCallIgnored
                        aocDir.mkdirs();
                    }
                }
                st.gameExtracted = filesIntact && st.romfsFiles > 0 && !st.mainBuildId.isEmpty();
            } catch (Exception e) {
                Log.w(TAG, "Invalid .ready.json: " + e.getMessage());
                st.gameExtracted = false;
            }
        }

        // 2. Validate core assets (.core_assets.json + shaders.dmsv size/version match)
        File coreFile = getCoreManifestFile(ctx);
        File shadersFile = new File(gameDir, SHADERS_FILE);
        if (coreFile.isFile() && shadersFile.isFile()) {
            try {
                JSONObject cj = new JSONObject(readFileUtf8(coreFile));
                st.syncedApkVersionCode = cj.optInt("apk_version_code", 0);
                st.syncedApkVersionName = cj.optString("apk_version_name", "");
                st.shadersSha256 = cj.optString("shaders_sha256", "");
                long expectedSize = cj.optLong("shaders_bytes", -1);

                JSONObject profile = new JSONObject(readBundledProfileJson(ctx));
                String expectedShaderSha = profile.getJSONObject("shader_bundle").getString("sha256");

                st.coreSynced = (st.syncedApkVersionCode == BuildConfig.VERSION_CODE)
                        && expectedSize > 0
                        && shadersFile.length() == expectedSize
                        && expectedShaderSha.equalsIgnoreCase(st.shadersSha256);
            } catch (Exception e) {
                Log.w(TAG, "Invalid .core_assets.json: " + e.getMessage());
                st.coreSynced = false;
            }
        }

        st.readyToPlay = st.gameExtracted && st.coreSynced;
        if (st.readyToPlay) {
            st.summary = "Ready to Play (v" + st.displayVersion + " • " + st.romfsFiles + " RomFS files • Shaders verified)";
        } else if (st.gameExtracted) {
            st.summary = "Game extracted — syncing updated core shaders & config...";
        } else {
            st.summary = "Game belum diekstrak — pilih prod.keys & Base Game (.nsp/.xci); Update v1.1.2 & DLC opsional";
        }
        return st;
    }

    /**
     * Atomically syncs shaders.dmsv (and default env.txt if missing) into filesDir/game_data/
     * whenever game_data/.ready.json exists and core assets are missing or outdated (e.g. after APK update).
     *
     * @param force Always re-copy shaders.dmsv even if manifest matches (used right after extraction)
     */
    public static void syncCoreAssets(Context ctx, boolean force) throws IOException {
        File gameDir = getGameDataDir(ctx);
        if (!gameDir.isDirectory()) {
            return;
        }
        if (!force && inspectStatus(ctx).coreSynced) {
            return;
        }

        String profileStr = readBundledProfileJson(ctx);
        String expectedShaderSha;
        try {
            JSONObject profile = new JSONObject(profileStr);
            expectedShaderSha = profile.getJSONObject("shader_bundle").getString("sha256");
        } catch (Exception e) {
            throw new IOException("Corrupt bundled core/profile.json", e);
        }

        AssetManager am = ctx.getAssets();
        File shadersDst = new File(gameDir, SHADERS_FILE);
        String actualShaderSha = copyAssetAtomically(am, "core/" + SHADERS_FILE, shadersDst);
        if (!expectedShaderSha.equalsIgnoreCase(actualShaderSha)) {
            // Remove corrupt file so Play stays safely blocked
            //noinspection ResultOfMethodCallIgnored
            shadersDst.delete();
            throw new IOException("shaders.dmsv SHA-256 mismatch: expected " + expectedShaderSha
                    + ", got " + actualShaderSha);
        }

        // Only write default env.txt if user does not already have one
        File envDst = new File(gameDir, ENV_FILE);
        if (!envDst.isFile()) {
            copyAssetAtomically(am, "core/" + ENV_FILE, envDst);
        }

        // Ensure save/ and temp/ exist under internal filesDir
        File saveDir = new File(ctx.getFilesDir(), "save");
        File tempDir = new File(ctx.getFilesDir(), "temp");
        //noinspection ResultOfMethodCallIgnored
        saveDir.mkdirs();
        //noinspection ResultOfMethodCallIgnored
        tempDir.mkdirs();

        // Atomically write .core_assets.json
        String profileSha = sha256Hex(profileStr.getBytes(StandardCharsets.UTF_8));
        try {
            JSONObject manifest = new JSONObject();
            manifest.put("apk_version_code", BuildConfig.VERSION_CODE);
            manifest.put("apk_version_name", BuildConfig.VERSION_NAME);
            manifest.put("shaders_sha256", actualShaderSha);
            manifest.put("shaders_bytes", shadersDst.length());
            manifest.put("profile_sha256", profileSha);
            manifest.put("synced_at_ms", System.currentTimeMillis());
            writeUtf8Atomically(getCoreManifestFile(ctx), manifest.toString(2) + "\n");
        } catch (Exception e) {
            throw new IOException("Failed writing .core_assets.json", e);
        }
        Log.i(TAG, "Core assets synced atomically (APK v" + BuildConfig.VERSION_NAME
                + ", shaders=" + shadersDst.length() + " B)");
    }

    /** Copies an APK asset to dst via dst.tmp + fsync + atomic rename, returning SHA-256 hex. */
    private static String copyAssetAtomically(AssetManager am, String assetPath, File dst) throws IOException {
        File parent = dst.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Cannot create directory " + parent);
        }
        File tmp = new File(dst.getParentFile(), dst.getName() + ".tmp");
        MessageDigest md;
        try {
            md = MessageDigest.getInstance("SHA-256");
        } catch (Exception e) {
            throw new IOException("SHA-256 unavailable", e);
        }

        byte[] buf = new byte[65536];
        try (InputStream in = am.open(assetPath);
             FileOutputStream out = new FileOutputStream(tmp)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                md.update(buf, 0, n);
                out.write(buf, 0, n);
            }
            out.flush();
            out.getFD().sync();
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw e;
        }

        if (!tmp.renameTo(dst)) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw new IOException("Atomic rename failed: " + tmp + " -> " + dst);
        }
        return toHex(md.digest());
    }

    public static void writeUtf8Atomically(File dst, String content) throws IOException {
        File parent = dst.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("Cannot create directory " + parent);
        }
        File tmp = new File(dst.getParentFile(), dst.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(content.getBytes(StandardCharsets.UTF_8));
            out.flush();
            out.getFD().sync();
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw e;
        }
        if (!tmp.renameTo(dst)) {
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            throw new IOException("Atomic rename failed: " + tmp + " -> " + dst);
        }
    }

    public static String readFileUtf8(File f) throws IOException {
        try (InputStream in = new FileInputStream(f)) {
            return new String(readAllBytes(in), StandardCharsets.UTF_8);
        }
    }

    private static byte[] readAllBytes(InputStream in) throws IOException {
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        byte[] buf = new byte[16384];
        int n;
        while ((n = in.read(buf)) > 0) {
            bos.write(buf, 0, n);
        }
        return bos.toByteArray();
    }

    private static String sha256Hex(byte[] data) throws IOException {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            return toHex(md.digest(data));
        } catch (Exception e) {
            throw new IOException(e);
        }
    }

    private static String toHex(byte[] bytes) {
        StringBuilder sb = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) {
            sb.append(Character.forDigit((b >> 4) & 0xF, 16));
            sb.append(Character.forDigit(b & 0xF, 16));
        }
        return sb.toString();
    }
}
