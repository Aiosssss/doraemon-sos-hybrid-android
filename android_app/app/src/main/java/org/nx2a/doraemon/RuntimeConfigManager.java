package org.nx2a.doraemon;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Manages real runtime settings backed 1:1 by:
 *   - filesDir/game_data/env.txt      (read by front_android.c env_file())
 *   - filesDir/game_data/settings.txt (read by front_android.c settings_load())
 *
 * Also mirrors user preferences in SharedPreferences so settings survive game_data/
 * re-extraction (e.g. when the user adds DLCs later) and preserves custom in-game
 * pause menu layout lines (ui=, grid=, labels=) inside settings.txt.
 */
public final class RuntimeConfigManager {
    private static final String PREFS = "doraemon_runtime_prefs";

    /** Render scale presets mapped to real runtime knobs (DM_RENDER_SCALE + docked 1080p).
     *  SCALE_085 uses code 3 so existing SharedPreferences from hybrid6 (0=0.75x, 1=1.0x, 2=1.25x) stay intact. */
    public static final int SCALE_075 = 0; // DM_RENDER_SCALE=0.75
    public static final int SCALE_100 = 1; // DM_RENDER_SCALE=1.0
    public static final int SCALE_125 = 2; // Docked 1080p + DM_RENDER_SCALE=1.0 (1.5x over 720p handheld)
    public static final int SCALE_085 = 3; // DM_RENDER_SCALE=0.85 (Balanced 60 FPS on Mali-G720 Handheld 720p)

    /** Mali / Vulkan zero-copy quirk override (DM_VK_QUIRK_NO_ZEROCOPY). */
    public static final int ZEROCOPY_AUTO = 0;      // Unset: auto-enabled on ARM Mali (0x13B5), off on Adreno
    public static final int ZEROCOPY_FORCE_NO = 1;  // DM_VK_QUIRK_NO_ZEROCOPY=1 (safe Mali mode)
    public static final int ZEROCOPY_FORCE_YES = 2; // DM_VK_QUIRK_NO_ZEROCOPY=0 (force zero-copy)

    /** 2x MSAA mode override (DM_VK_DISABLE_MSAA). Default is native 2x MSAA on ALL GPUs. */
    public static final int MSAA_AUTO = 0;          // Unset: native 2x MSAA on Mali & Adreno (accurate; required for sampler2DMS)
    public static final int MSAA_DISABLE = 1;       // DM_VK_DISABLE_MSAA=1 (1x experimental — may break sampler2DMS passes)
    public static final int MSAA_NATIVE = 2;        // DM_VK_DISABLE_MSAA=0 (explicit native 2x MSAA)

    /** 2x MSAA Alpha-To-Coverage override (DM_VK_NO_A2C). Default Auto: OFF on ARM Mali (solid cutout trees), ON on Adreno. */
    public static final int A2C_AUTO = 0;           // Unset: VQ_NO_A2C auto-disables A2C on Mali, keeps native A2C on Adreno
    public static final int A2C_DISABLE = 1;        // DM_VK_NO_A2C=1 (solid cutout foliage via shader discard)
    public static final int A2C_NATIVE = 2;         // DM_VK_NO_A2C=0 (force native alphaToCoverageEnable)

    public static final class Config {
        public int renderScalePreset = SCALE_100;
        public int targetFps = 30;            // 30 (default) or 60 (unlocked via DM_PRESENT_INTERVAL=display)
        public boolean dockedMode = false;    // settings.txt docked=0|1 -> DM_OPMODE
        public int audioVolumePct = 100;      // 0..100 -> DM_AUDIO_VOLUME=0.00..1.00
        public boolean overlayEnabled = true; // settings.txt overlay=1|0
        public int overlayOpacity = 60;       // settings.txt opacity=20..80
        public boolean rumbleEnabled = true;  // settings.txt rumble=1|0
        public boolean fpsCounter = false;    // settings.txt fps=0|1
        public boolean bcDiskCache = true;    // env.txt DM_VK_BC_CACHE=1|0
        public int zeroCopyQuirk = ZEROCOPY_AUTO; // env.txt DM_VK_QUIRK_NO_ZEROCOPY
        public int msaaMode = MSAA_AUTO;      // env.txt DM_VK_DISABLE_MSAA
        public int a2cMode = A2C_AUTO;        // env.txt DM_VK_NO_A2C
        public boolean asyncReplay = false;   // env.txt DM_NVN_ASYNC=1|0 (+ DM_PIN_REPLAY=big)
        public boolean poolSafeSync = false;  // env.txt DM_VK_POOL_SAFE_SYNC=1|0 (staged vkCmdCopyBuffer, keeps 30 FPS readback_ranges)
        public boolean poolOverlap = true;    // env.txt DM_VK_POOL_OVERLAP=1|0 (Mali CPU-GPU overlap via s->up DMA; cuts fence_wait for 60 FPS)
        public boolean dump3dFrame = false;   // env.txt DM_VK_DUMP_3D_FRAME=1|0 (1-shot 3D render-graph dumper for log diagnostics)
        public boolean strictBarriers = false; // env.txt DM_VK_STRICT_BARRIERS=1|0 (VkRenderPass2, no clear-folding, single submit)
        public boolean emuFirstInstance = true; // env.txt DM_VK_EMU_FIRST_INSTANCE=1|0 (Mali non-zero firstInstance VBO offset emulation)
    }

    private RuntimeConfigManager() {}

    /** Loads persisted settings, syncing any changes made in-game via settings.txt. */
    public static Config load(Context ctx) {
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        Config c = new Config();
        c.renderScalePreset = sp.getInt("render_scale", SCALE_100);
        c.targetFps = sp.getInt("target_fps", 30);
        c.dockedMode = sp.getBoolean("docked", false);
        c.audioVolumePct = clamp(sp.getInt("audio_vol", 100), 0, 100);
        c.overlayEnabled = sp.getBoolean("overlay", true);
        c.overlayOpacity = clamp(sp.getInt("opacity", 60), 20, 80);
        c.rumbleEnabled = sp.getBoolean("rumble", true);
        c.fpsCounter = sp.getBoolean("fps_counter", false);
        c.bcDiskCache = sp.getBoolean("bc_cache", true);
        c.zeroCopyQuirk = sp.getInt("zerocopy_quirk", ZEROCOPY_AUTO);
        c.msaaMode = sp.getInt("msaa_mode", MSAA_AUTO);
        c.a2cMode = sp.getInt("a2c_mode", A2C_AUTO);
        c.asyncReplay = sp.getBoolean("async_replay", false);
        c.poolSafeSync = sp.getBoolean("pool_safe_sync", false);
        c.poolOverlap = sp.getBoolean("pool_overlap", true);
        c.dump3dFrame = sp.getBoolean("dump_3d_frame", false);
        c.strictBarriers = sp.getBoolean("strict_barriers", false);
        c.emuFirstInstance = sp.getBoolean("emu_first_instance", true);

        // Reflect any in-game pause menu edits back from game_data/settings.txt
        File sf = new File(CoreAssetSync.getGameDataDir(ctx), CoreAssetSync.SETTINGS_FILE);
        if (sf.isFile()) {
            try {
                for (String raw : CoreAssetSync.readFileUtf8(sf).split("\n")) {
                    String line = raw.trim();
                    if (line.startsWith("overlay=")) c.overlayEnabled = parseInt(line.substring(8), 1) != 0;
                    else if (line.startsWith("rumble=")) c.rumbleEnabled = parseInt(line.substring(7), 1) != 0;
                    else if (line.startsWith("docked=")) c.dockedMode = parseInt(line.substring(7), 0) != 0;
                    else if (line.startsWith("fps=")) c.fpsCounter = parseInt(line.substring(4), 0) != 0;
                    else if (line.startsWith("opacity=")) c.overlayOpacity = clamp(parseInt(line.substring(8), 60), 20, 80);
                }
            } catch (IOException ignored) {
            }
        }
        return c;
    }

    /** Saves config to SharedPreferences and atomically writes env.txt & settings.txt if game_data/ exists. */
    public static void save(Context ctx, Config c) throws IOException {
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        sp.edit()
                .putInt("render_scale", c.renderScalePreset)
                .putInt("target_fps", c.targetFps)
                .putBoolean("docked", c.dockedMode)
                .putInt("audio_vol", clamp(c.audioVolumePct, 0, 100))
                .putBoolean("overlay", c.overlayEnabled)
                .putInt("opacity", clamp(c.overlayOpacity, 20, 80))
                .putBoolean("rumble", c.rumbleEnabled)
                .putBoolean("fps_counter", c.fpsCounter)
                .putBoolean("bc_cache", c.bcDiskCache)
                .putInt("zerocopy_quirk", c.zeroCopyQuirk)
                .putInt("msaa_mode", c.msaaMode)
                .putInt("a2c_mode", c.a2cMode)
                .putBoolean("async_replay", c.asyncReplay)
                .putBoolean("pool_safe_sync", c.poolSafeSync)
                .putBoolean("pool_overlap", c.poolOverlap)
                .putBoolean("dump_3d_frame", c.dump3dFrame)
                .putBoolean("strict_barriers", c.strictBarriers)
                .putBoolean("emu_first_instance", c.emuFirstInstance)
                .apply();

        applyToGameData(ctx, c, null);
    }

    /** Captures any custom layout/grid/labels lines from game_data/settings.txt before re-extraction. */
    public static List<String> capturePreservedSettingsLines(Context ctx) {
        List<String> preserved = new ArrayList<>();
        File sf = new File(CoreAssetSync.getGameDataDir(ctx), CoreAssetSync.SETTINGS_FILE);
        if (!sf.isFile()) return preserved;
        try {
            for (String raw : CoreAssetSync.readFileUtf8(sf).split("\n")) {
                String line = raw.trim();
                if (line.startsWith("ui=") || line.startsWith("grid=") || line.startsWith("labels=")) {
                    preserved.add(line);
                }
            }
        } catch (IOException ignored) {
        }
        return preserved;
    }

    /** Atomically writes game_data/env.txt and game_data/settings.txt from Config. */
    public static void applyToGameData(Context ctx, Config c, List<String> extraSettingsLines) throws IOException {
        File gameDir = CoreAssetSync.getGameDataDir(ctx);
        if (!gameDir.isDirectory()) return;

        // 1. Preserve custom non-managed lines in env.txt if present
        File envFile = new File(gameDir, CoreAssetSync.ENV_FILE);
        Map<String, String> env = new LinkedHashMap<>();
        if (envFile.isFile()) {
            try {
                for (String raw : CoreAssetSync.readFileUtf8(envFile).split("\n")) {
                    String line = raw.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int eq = line.indexOf('=');
                    if (eq > 0) env.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
                }
            } catch (IOException ignored) {
            }
        }

        boolean effectiveDocked = c.dockedMode || (c.renderScalePreset == SCALE_125);
        String scaleVal = (c.renderScalePreset == SCALE_075) ? "0.75"
                : (c.renderScalePreset == SCALE_085) ? "0.85" : "1.0";
        int fps = (c.targetFps == 60) ? 60 : 30;
        String volVal = String.format(Locale.US, "%.2f", clamp(c.audioVolumePct, 0, 100) / 100.0);

        env.put("DM_FPS_TARGET", String.valueOf(fps));
        env.put("DM_FPS_CEILING", fps == 60 ? "0" : "30");
        env.put("DM_FPS_HOLD", fps == 60 ? "off" : "on");
        env.put("DM_PRESENT_INTERVAL", fps == 60 ? "display" : "game");
        env.put("DM_ADAPTIVE_FPS", "0");
        env.put("DM_RENDER_SCALE", scaleVal);
        env.put("DM_AUDIO_VOLUME", volVal);
        env.put("DM_VK_BC_CACHE", c.bcDiskCache ? "1" : "0");

        if (c.zeroCopyQuirk == ZEROCOPY_FORCE_NO) {
            env.put("DM_VK_QUIRK_NO_ZEROCOPY", "1");
        } else if (c.zeroCopyQuirk == ZEROCOPY_FORCE_YES) {
            env.put("DM_VK_QUIRK_NO_ZEROCOPY", "0");
        } else {
            env.remove("DM_VK_QUIRK_NO_ZEROCOPY");
        }

        if (c.msaaMode == MSAA_DISABLE) {
            env.put("DM_VK_DISABLE_MSAA", "1");
        } else if (c.msaaMode == MSAA_NATIVE) {
            env.put("DM_VK_DISABLE_MSAA", "0");
        } else {
            env.remove("DM_VK_DISABLE_MSAA");
        }

        if (c.a2cMode == A2C_DISABLE) {
            env.put("DM_VK_NO_A2C", "1");
        } else if (c.a2cMode == A2C_NATIVE) {
            env.put("DM_VK_NO_A2C", "0");
        } else {
            env.remove("DM_VK_NO_A2C");
        }

        env.put("DM_PIN_REPLAY", "big");
        if (c.asyncReplay) {
            env.put("DM_NVN_ASYNC", "1");
        } else {
            env.remove("DM_NVN_ASYNC");
        }
        env.put("DM_VK_POOL_SAFE_SYNC", c.poolSafeSync ? "1" : "0");
        env.put("DM_VK_POOL_OVERLAP", c.poolOverlap ? "1" : "0");
        env.put("DM_VK_DUMP_3D_FRAME", c.dump3dFrame ? "1" : "0");
        env.put("DM_VK_STRICT_BARRIERS", c.strictBarriers ? "1" : "0");
        env.put("DM_VK_EMU_FIRST_INSTANCE", c.emuFirstInstance ? "1" : "0");

        StringBuilder envOut = new StringBuilder();
        envOut.append("# Doraemon SoS Hybrid Runtime Configuration (auto-managed by Launcher)\n");
        for (Map.Entry<String, String> e : env.entrySet()) {
            envOut.append(e.getKey()).append('=').append(e.getValue()).append('\n');
        }
        CoreAssetSync.writeUtf8Atomically(envFile, envOut.toString());

        // 2. Write settings.txt preserving ui=, grid=, labels= lines from in-game editor
        List<String> preserved = (extraSettingsLines != null)
                ? extraSettingsLines
                : capturePreservedSettingsLines(ctx);
        boolean hasGrid = false;
        for (String line : preserved) {
            if (line.startsWith("grid=")) {
                hasGrid = true;
                break;
            }
        }

        StringBuilder setOut = new StringBuilder();
        setOut.append("overlay=").append(c.overlayEnabled ? 1 : 0).append('\n');
        setOut.append("rumble=").append(c.rumbleEnabled ? 1 : 0).append('\n');
        setOut.append("docked=").append(effectiveDocked ? 1 : 0).append('\n');
        setOut.append("fps=").append(c.fpsCounter ? 1 : 0).append('\n');
        setOut.append("opacity=").append(clamp(c.overlayOpacity, 20, 80)).append('\n');
        if (!hasGrid) {
            setOut.append("grid=0\n");
        }
        for (String line : preserved) {
            setOut.append(line).append('\n');
        }
        File setFile = new File(gameDir, CoreAssetSync.SETTINGS_FILE);
        CoreAssetSync.writeUtf8Atomically(setFile, setOut.toString());
    }

    private static int clamp(int v, int lo, int hi) {
        return Math.max(lo, Math.min(hi, v));
    }

    private static int parseInt(String s, int def) {
        try {
            return Integer.parseInt(s.trim());
        } catch (Exception e) {
            return def;
        }
    }
}
