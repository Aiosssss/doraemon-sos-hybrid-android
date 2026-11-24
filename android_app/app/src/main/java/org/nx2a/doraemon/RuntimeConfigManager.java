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

    /** Render scale presets mapped to DM_VK_RENDER_SCALE (+ docked 1080p).
     *  Preserves legacy codes (0=0.75x, 1=1.0x, 2=Docked 1080p, 3=0.85x) and adds Auto/67%/55% for Mali-G52/G57. */
    public static final int SCALE_075 = 0;  // 75% (960x540)
    public static final int SCALE_100 = 1;  // 100% Native Handheld (1280x720)
    public static final int SCALE_125 = 2;  // Docked 1080p HQ (1920x1080)
    public static final int SCALE_085 = 3;  // Legacy 85%
    public static final int SCALE_AUTO = 4; // Auto: 100% High-End >=6 GB, 67% Mali-G52..G68 / Tier 1, 55% Tier 2
    public static final int SCALE_067 = 5;  // 67% (~854x480)
    public static final int SCALE_055 = 6;  // 55% (704x396, Low-End 60 FPS)
    public static final int SCALE_050 = 7;  // 50% (640x360, Ultra FPS HP Kentang)

    /** Mali / Vulkan zero-copy quirk override (DM_VK_QUIRK_NO_ZEROCOPY). */
    public static final int ZEROCOPY_AUTO = 0;      // Unset: auto-enabled on ARM Mali (0x13B5), off on Adreno
    public static final int ZEROCOPY_FORCE_NO = 1;  // DM_VK_QUIRK_NO_ZEROCOPY=1 (safe Mali mode)
    public static final int ZEROCOPY_FORCE_YES = 2; // DM_VK_QUIRK_NO_ZEROCOPY=0 (force zero-copy)

    /** 2x MSAA mode override (DM_VK_DISABLE_MSAA). Auto = native 2x MSAA on >=6 GB high-end, 1x on low-end Mali / Low-RAM. */
    public static final int MSAA_AUTO = 0;          // DM_VK_DISABLE_MSAA=auto
    public static final int MSAA_DISABLE = 1;       // DM_VK_DISABLE_MSAA=1 (1x MSAA ON)
    public static final int MSAA_NATIVE = 2;        // DM_VK_DISABLE_MSAA=0 (Native 2x MSAA forced)

    /** Target FPS / Thermal Mode preset (DM_FPS_TARGET / DM_FPS_CEILING). */
    public static final int FPS_AUTO = 0;           // Auto: 60 FPS on flagship/mid-high GPU, 30 FPS Stabil on Mali-G52/PowerVR/Adreno 610
    public static final int FPS_60 = 60;            // 60 FPS Full Performance (Display Pacing)
    public static final int FPS_45 = 45;            // 45 FPS Smooth Mid-Range (22.22 ms budget for Mali-G57 / Adreno 619)
    public static final int FPS_30_COOL = 30;       // 30 FPS Stabil & Adem (Battery / Cooler)

    /** 2x MSAA Alpha-To-Coverage override (DM_VK_NO_A2C). Default Auto: Native A2C on modern Mali Vulkan 1.3+ (r40+) & Adreno, OFF on legacy Mali. */
    public static final int A2C_AUTO = 0;           // Unset: VQ_NO_A2C auto-enables native A2C on Mali Vulkan 1.3+ (driver >= 40.0) & Adreno
    public static final int A2C_DISABLE = 1;        // DM_VK_NO_A2C=1 (solid cutout foliage via shader discard)
    public static final int A2C_NATIVE = 2;         // DM_VK_NO_A2C=0 (force native alphaToCoverageEnable)

    /** Memory / Anti-LMK profile (DM_LOW_RAM). Default Auto detects <=5.5 GB (tier 1) and <=3.5 GB (tier 2) automatically. */
    public static final int LOW_RAM_AUTO = 0;       // DM_LOW_RAM=auto
    public static final int LOW_RAM_STANDARD = 1;   // DM_LOW_RAM=0 (>=6 GB phones)
    public static final int LOW_RAM_4GB = 2;        // DM_LOW_RAM=1 (4 GB Anti-LMK: 1x MSAA, 0.67x scale, 1/2x BC tex)
    public static final int LOW_RAM_3GB = 3;        // DM_LOW_RAM=2 (3 GB Ultra: 1x MSAA, 0.55x scale, 1/4x BC tex, no prewarm)

    /** UBD Copyback Mode (DM_VK_UBD_COPYBACK). */
    public static final int UBD_SYNC_DEFAULT = 1;   // DM_VK_UBD_COPYBACK=1 (Sync mirror, 60 FPS default)
    public static final int UBD_ASYNC_HYBRID6 = 2;  // DM_VK_UBD_COPYBACK=2 (Legacy async pool_merge_back A/B)

    /** Pool Safe Sync Mode (DM_VK_POOL_SAFE_SYNC). */
    public static final int SAFE_SYNC_LEGACY_OFF = 0; // DM_VK_POOL_SAFE_SYNC=0 (Legacy overlap A/B)
    public static final int SAFE_SYNC_PIPELINED = 1;  // DM_VK_POOL_SAFE_SYNC=1 (Default: Pipelined Zero-Stall Safe Sync)
    public static final int SAFE_SYNC_SERIAL_DBG = 2; // DM_VK_POOL_SAFE_SYNC=2 (Hard Serial Safe Sync)

    /** Low-Cost Post-Process Mode (DM_VK_LOW_COST_PP). */
    public static final int LOW_COST_PP_AUTO = 0;     // auto (ON on Low-End / Ultra Low-End Tier 2-3)
    public static final int LOW_COST_PP_ON = 1;       // DM_VK_LOW_COST_PP=1
    public static final int LOW_COST_PP_OFF = 2;      // DM_VK_LOW_COST_PP=0

    /** Shadow Map Scale Mode (DM_VK_SHADOW_SCALE). */
    public static final int SHADOW_AUTO = 0;          // auto (1.0x Flagship / 0.75x Mid / 0.5x Low-End & Ultra)
    public static final int SHADOW_HALF = 1;          // 0.50 (75% less shadow fillrate)
    public static final int SHADOW_FULL = 2;          // 1.00 (Native shadow resolution)
    public static final int SHADOW_THREE_QUARTER = 3; // 0.75 (44% less shadow fillrate, crisp mid-range)

    /** Texture Downscale Mode (DM_VK_TEX_DOWNSCALE). */
    public static final int TEX_DS_AUTO = 0;          // auto
    public static final int TEX_DS_FULL = 1;          // 1 (1/1 Native)
    public static final int TEX_DS_HALF = 2;          // 2 (1/2 Low-End)
    public static final int TEX_DS_QUARTER = 4;       // 4 (1/4 Ultra Low-RAM)

    /** 1-Click Quick Performance Presets for Launcher Settings. */
    public static final int PRESET_AUTO = 0;          // Otomatis (Disarankan)
    public static final int PRESET_LOW_END = 1;       // HP Kentang / Low-End (60 FPS)
    public static final int PRESET_BALANCED = 2;      // Seimbang
    public static final int PRESET_HIGH = 3;          // Kualitas Tinggi
    public static final int PRESET_CUSTOM = 4;        // Kustom

    public static final class Config {
        public int renderScalePreset = SCALE_AUTO;
        public int targetFps = FPS_AUTO;      // 0 = Auto (60 FPS unlocked on all tiers), 60, 45, 30
        public boolean dockedMode = false;    // settings.txt docked=0|1 -> DM_OPMODE
        public int audioVolumePct = 100;      // 0..100 -> DM_AUDIO_VOLUME=0.00..1.00
        public boolean overlayEnabled = true; // settings.txt overlay=1|0
        public int overlayOpacity = 60;       // settings.txt opacity=20..80
        public boolean rumbleEnabled = true;  // settings.txt rumble=1|0
        public boolean fpsCounter = false;    // settings.txt fps=0|1
        public boolean bcDiskCache = true;    // env.txt DM_VK_BC_CACHE=1|0
        public int zeroCopyQuirk = ZEROCOPY_AUTO; // env.txt DM_VK_QUIRK_NO_ZEROCOPY
        public int msaaMode = MSAA_AUTO;      // env.txt DM_VK_DISABLE_MSAA=auto|1|0
        public int a2cMode = A2C_AUTO;        // env.txt DM_VK_NO_A2C
        public int lowRamMode = LOW_RAM_AUTO; // env.txt DM_LOW_RAM=auto|0|1|2
        public int texDownscaleMode = TEX_DS_AUTO; // env.txt DM_VK_TEX_DOWNSCALE=auto|1|2|4
        public int shadowScaleMode = SHADOW_AUTO;  // env.txt DM_VK_SHADOW_SCALE=auto|0.5|1.0
        public int lowCostPpMode = LOW_COST_PP_AUTO; // env.txt DM_VK_LOW_COST_PP=auto|1|0
        public int ubdCopybackMode = UBD_SYNC_DEFAULT; // env.txt DM_VK_UBD_COPYBACK=1|2
        public boolean robustAccess = true;   // env.txt DM_VK_ROBUST_ACCESS=1|0 (default ON: fixes Mali OOB UBO red flash)
        public boolean inlineResolve = true;  // env.txt DM_VK_INLINE_RESOLVE=1|0 (default ON: inline TBDR MSAA resolve)
        public boolean asyncReplay = false;   // env.txt DM_NVN_ASYNC=1|0 (+ DM_PIN_REPLAY=big)
        public int poolSafeSyncMode = SAFE_SYNC_SERIAL_DBG; // env.txt DM_VK_POOL_SAFE_SYNC=2 (Default: Pure 16KB Serial 0% flicker) | 1 (Pipelined)
        public boolean safe1HotBlocks = false; // env.txt DM_VK_SAFE1_HOT_BLOCKS=0 (disabled by default to avoid flicker)
        public boolean safe1RbStaged = true;  // env.txt DM_VK_SAFE1_RB_STAGED=1|0
        public boolean safe1Diff1k = false;   // env.txt DM_VK_SAFE1_DIFF1K=0 (disabled by default to avoid stale baseline flicker)
        public boolean poolOverlap = true;    // env.txt DM_VK_POOL_OVERLAP=1|0
        public boolean dump3dFrame = false;   // env.txt DM_VK_DUMP_3D_FRAME=0|1
        public boolean strictBarriers = false; // env.txt DM_VK_STRICT_BARRIERS=0|1
        public boolean emuFirstInstance = true; // env.txt DM_VK_EMU_FIRST_INSTANCE=1|0
    }

    /** Applies one of the 4 user-friendly 1-click presets while keeping anti-flicker protections locked ON (Mode 2 default). */
    public static void applyQuickPreset(Config c, int preset) {
        if (c == null) return;
        c.poolSafeSyncMode = SAFE_SYNC_SERIAL_DBG;
        c.safe1HotBlocks = false;
        c.safe1Diff1k = false;
        c.robustAccess = true;
        c.inlineResolve = true;
        c.asyncReplay = false;
        c.ubdCopybackMode = UBD_SYNC_DEFAULT;
        c.poolOverlap = true;
        c.strictBarriers = false;
        switch (preset) {
            case PRESET_LOW_END:
                c.renderScalePreset = SCALE_050;
                c.msaaMode = MSAA_DISABLE;
                c.texDownscaleMode = TEX_DS_HALF;
                c.shadowScaleMode = SHADOW_HALF;
                c.targetFps = FPS_AUTO;
                c.dockedMode = false;
                c.lowRamMode = LOW_RAM_4GB;
                c.lowCostPpMode = LOW_COST_PP_ON;
                break;
            case PRESET_BALANCED:
                c.renderScalePreset = SCALE_075;
                c.msaaMode = MSAA_DISABLE;
                c.texDownscaleMode = TEX_DS_FULL;
                c.shadowScaleMode = SHADOW_THREE_QUARTER;
                c.targetFps = FPS_60;
                c.dockedMode = false;
                c.lowRamMode = LOW_RAM_AUTO;
                c.lowCostPpMode = LOW_COST_PP_OFF;
                break;
            case PRESET_HIGH:
                c.renderScalePreset = SCALE_100;
                c.msaaMode = MSAA_NATIVE;
                c.texDownscaleMode = TEX_DS_FULL;
                c.shadowScaleMode = SHADOW_FULL;
                c.targetFps = FPS_60;
                c.dockedMode = false;
                c.lowRamMode = LOW_RAM_STANDARD;
                c.lowCostPpMode = LOW_COST_PP_OFF;
                break;
            case PRESET_AUTO:
            default:
                c.renderScalePreset = SCALE_AUTO;
                c.msaaMode = MSAA_AUTO;
                c.texDownscaleMode = TEX_DS_AUTO;
                c.shadowScaleMode = SHADOW_AUTO;
                c.targetFps = FPS_AUTO;
                c.dockedMode = false;
                c.lowRamMode = LOW_RAM_AUTO;
                c.lowCostPpMode = LOW_COST_PP_AUTO;
                break;
        }
    }

    /** Detects which 1-click preset matches the current Config (or PRESET_CUSTOM). */
    public static int detectQuickPreset(Config c) {
        if (c == null) return PRESET_AUTO;
        if (c.renderScalePreset == SCALE_AUTO && c.msaaMode == MSAA_AUTO && c.targetFps == FPS_AUTO && !c.dockedMode) {
            return PRESET_AUTO;
        }
        if ((c.renderScalePreset == SCALE_050 || c.renderScalePreset == SCALE_055)
                && c.msaaMode == MSAA_DISABLE && !c.dockedMode) {
            return PRESET_LOW_END;
        }
        if (c.renderScalePreset == SCALE_075 && c.msaaMode == MSAA_DISABLE && c.targetFps == FPS_60 && !c.dockedMode) {
            return PRESET_BALANCED;
        }
        if (c.renderScalePreset == SCALE_100 && c.msaaMode == MSAA_NATIVE && c.targetFps == FPS_60 && !c.dockedMode) {
            return PRESET_HIGH;
        }
        return PRESET_CUSTOM;
    }

    /** Resets every knob back to optimal v1.1.2-hybrid25 defaults (keeps overlay/audio comfortable). */
    public static Config createDefaultConfig() {
        return new Config();
    }

    private RuntimeConfigManager() {}

    /** Loads persisted settings, syncing any changes made in-game via settings.txt. */
    public static Config load(Context ctx) {
        SharedPreferences sp = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        Config c = new Config();
        c.renderScalePreset = sp.getInt("render_scale", SCALE_AUTO);
        c.targetFps = sp.getInt("target_fps", FPS_AUTO);
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
        c.lowRamMode = sp.getInt("low_ram_mode", LOW_RAM_AUTO);
        c.texDownscaleMode = sp.getInt("tex_downscale_mode", TEX_DS_AUTO);
        c.shadowScaleMode = clamp(sp.getInt("shadow_scale_mode", SHADOW_AUTO), 0, 3);
        c.lowCostPpMode = clamp(sp.getInt("low_cost_pp_mode", LOW_COST_PP_AUTO), 0, 2);
        c.ubdCopybackMode = sp.getInt("ubd_copyback_mode", UBD_SYNC_DEFAULT);
        c.robustAccess = sp.getBoolean("robust_access", true);
        c.inlineResolve = sp.getBoolean("inline_resolve", true);
        c.asyncReplay = sp.getBoolean("async_replay", false);
        c.poolSafeSyncMode = clamp(sp.getInt("pool_safe_sync_mode", SAFE_SYNC_SERIAL_DBG), 0, 2);
        c.safe1HotBlocks = sp.getBoolean("safe1_hot_blocks", false);
        c.safe1RbStaged = sp.getBoolean("safe1_rb_staged", true);
        c.safe1Diff1k = sp.getBoolean("safe1_diff1k", false);
        c.poolOverlap = sp.getBoolean("pool_overlap", true);
        c.dump3dFrame = sp.getBoolean("dump_3d_frame", false);
        c.strictBarriers = sp.getBoolean("strict_barriers", false);
        if (!sp.getBoolean("hybrid14_v2_migrated", false)) {
            c.targetFps = 60;
            c.poolOverlap = true;
            c.poolSafeSyncMode = SAFE_SYNC_PIPELINED;
            c.strictBarriers = false;
            c.dockedMode = false;
            if (c.renderScalePreset == SCALE_100 || c.renderScalePreset == SCALE_085 || c.renderScalePreset == SCALE_125) {
                c.renderScalePreset = SCALE_AUTO;
            }
            sp.edit()
                    .putBoolean("hybrid10_ovl_migrated", true)
                    .putBoolean("hybrid12_reset_migrated", true)
                    .putBoolean("hybrid14_v2_migrated", true)
                    .putInt("target_fps", 60)
                    .putInt("render_scale", c.renderScalePreset)
                    .putBoolean("docked", false)
                    .putBoolean("pool_overlap", true)
                    .putInt("pool_safe_sync_mode", SAFE_SYNC_PIPELINED)
                    .putBoolean("strict_barriers", false)
                    .apply();
        }
        c.emuFirstInstance = sp.getBoolean("emu_first_instance", true);
        if (!sp.getBoolean("hybrid16_migrated", false)) {
            if (c.targetFps == 60) c.targetFps = FPS_AUTO;
            c.dump3dFrame = false;
            c.strictBarriers = false;
            c.emuFirstInstance = true;
            c.poolOverlap = true;
            c.poolSafeSyncMode = SAFE_SYNC_PIPELINED;
            sp.edit()
                    .putBoolean("hybrid16_migrated", true)
                    .putInt("target_fps", c.targetFps)
                    .putBoolean("dump_3d_frame", false)
                    .putBoolean("strict_barriers", false)
                    .putBoolean("emu_first_instance", true)
                    .putBoolean("pool_overlap", true)
                    .putInt("pool_safe_sync_mode", SAFE_SYNC_PIPELINED)
                    .apply();
        }
        if (!sp.getBoolean("hybrid26_migrated", false)) {
            // hybrid26: restore Pure Serial Safe Sync Mode 2 (DM_VK_POOL_SAFE_SYNC=2, 16 KB exact dirty blocks,
            // zero hot_blocks/baseline-diff skip -> 0% red & shadow flicker!) + Low-End 60 FPS optimizations.
            c.robustAccess = true;
            c.inlineResolve = true;
            c.poolSafeSyncMode = SAFE_SYNC_SERIAL_DBG;
            c.safe1HotBlocks = false;
            c.safe1RbStaged = true;
            c.safe1Diff1k = false;
            c.shadowScaleMode = SHADOW_AUTO;
            c.lowCostPpMode = LOW_COST_PP_AUTO;
            c.strictBarriers = false;
            c.dump3dFrame = false;
            c.poolOverlap = true;
            sp.edit()
                    .putBoolean("hybrid18_migrated", true)
                    .putBoolean("hybrid19_migrated", true)
                    .putBoolean("hybrid20_migrated", true)
                    .putBoolean("hybrid21_migrated", true)
                    .putBoolean("hybrid22_migrated", true)
                    .putBoolean("hybrid23_migrated", true)
                    .putBoolean("hybrid24_migrated", true)
                    .putBoolean("hybrid25_migrated", true)
                    .putBoolean("hybrid26_migrated", true)
                    .putBoolean("robust_access", true)
                    .putBoolean("inline_resolve", true)
                    .putInt("pool_safe_sync_mode", SAFE_SYNC_SERIAL_DBG)
                    .putBoolean("safe1_hot_blocks", false)
                    .putBoolean("safe1_rb_staged", true)
                    .putBoolean("safe1_diff1k", false)
                    .putInt("shadow_scale_mode", SHADOW_AUTO)
                    .putInt("low_cost_pp_mode", LOW_COST_PP_AUTO)
                    .putBoolean("strict_barriers", false)
                    .putBoolean("dump_3d_frame", false)
                    .putBoolean("pool_overlap", true)
                    .apply();
        }

        if (!sp.getBoolean("hybrid27_migrated", false)) {
            // hybrid27: lock Serial Safe Sync Mode 2 + Robust Access as default across all tiers
            // while preserving the user's graphics/low-end preset (render scale, MSAA, low-RAM, FPS).
            c.robustAccess = true;
            c.inlineResolve = true;
            c.poolSafeSyncMode = SAFE_SYNC_SERIAL_DBG;
            c.safe1HotBlocks = false;
            c.safe1RbStaged = true;
            c.safe1Diff1k = false;
            c.strictBarriers = false;
            c.dump3dFrame = false;
            c.poolOverlap = true;
            sp.edit()
                    .putBoolean("hybrid27_migrated", true)
                    .putBoolean("robust_access", true)
                    .putBoolean("inline_resolve", true)
                    .putInt("pool_safe_sync_mode", SAFE_SYNC_SERIAL_DBG)
                    .putBoolean("safe1_hot_blocks", false)
                    .putBoolean("safe1_rb_staged", true)
                    .putBoolean("safe1_diff1k", false)
                    .putBoolean("strict_barriers", false)
                    .putBoolean("dump_3d_frame", false)
                    .putBoolean("pool_overlap", true)
                    .apply();
        }

        if (!sp.getBoolean("hybrid30_migrated", false)) {
            // hybrid30: disable asyncReplay (fixes low-end copied-pool flicker on Mali-G52), lock ubdCopyback=1,
            // lock safe_sync=2 + robustAccess=1, enable auto low-cost PP & shadow scale, and avoid 1/4x tex downscale.
            c.asyncReplay = false;
            c.ubdCopybackMode = UBD_SYNC_DEFAULT;
            c.poolSafeSyncMode = SAFE_SYNC_SERIAL_DBG;
            c.robustAccess = true;
            c.inlineResolve = true;
            c.shadowScaleMode = SHADOW_AUTO;
            c.lowCostPpMode = LOW_COST_PP_AUTO;
            if (c.texDownscaleMode == TEX_DS_QUARTER) {
                c.texDownscaleMode = TEX_DS_AUTO;
            }
            sp.edit()
                    .putBoolean("hybrid30_migrated", true)
                    .putBoolean("async_replay", false)
                    .putInt("ubd_copyback_mode", UBD_SYNC_DEFAULT)
                    .putInt("pool_safe_sync_mode", SAFE_SYNC_SERIAL_DBG)
                    .putBoolean("robust_access", true)
                    .putBoolean("inline_resolve", true)
                    .putInt("shadow_scale_mode", SHADOW_AUTO)
                    .putInt("low_cost_pp_mode", LOW_COST_PP_AUTO)
                    .putInt("tex_downscale_mode", c.texDownscaleMode)
                    .apply();
        }

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
                .putInt("low_ram_mode", c.lowRamMode)
                .putInt("tex_downscale_mode", c.texDownscaleMode)
                .putInt("shadow_scale_mode", clamp(c.shadowScaleMode, 0, 3))
                .putInt("low_cost_pp_mode", clamp(c.lowCostPpMode, 0, 2))
                .putInt("ubd_copyback_mode", c.ubdCopybackMode)
                .putBoolean("robust_access", c.robustAccess)
                .putBoolean("inline_resolve", c.inlineResolve)
                .putBoolean("async_replay", c.asyncReplay)
                .putInt("pool_safe_sync_mode", clamp(c.poolSafeSyncMode, 0, 2))
                .putBoolean("safe1_hot_blocks", c.safe1HotBlocks)
                .putBoolean("safe1_rb_staged", c.safe1RbStaged)
                .putBoolean("safe1_diff1k", c.safe1Diff1k)
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

        boolean fpsAuto = (c.targetFps == FPS_AUTO);
        boolean fps45 = (c.targetFps == FPS_45);
        boolean fps30 = (c.targetFps == FPS_30_COOL);
        boolean effectiveDocked = c.dockedMode || (c.renderScalePreset == SCALE_125);
        String vkScaleVal;
        if (c.renderScalePreset == SCALE_050) vkScaleVal = "0.50";
        else if (c.renderScalePreset == SCALE_055) vkScaleVal = "0.55";
        else if (c.renderScalePreset == SCALE_067) vkScaleVal = "0.67";
        else if (c.renderScalePreset == SCALE_075) vkScaleVal = "0.75";
        else if (c.renderScalePreset == SCALE_085) vkScaleVal = "0.85";
        else if (c.renderScalePreset == SCALE_100 || c.renderScalePreset == SCALE_125) vkScaleVal = "1.0";
        else vkScaleVal = "auto";
        String volVal = String.format(Locale.US, "%.2f", clamp(c.audioVolumePct, 0, 100) / 100.0);

        env.put("DM_OPMODE", effectiveDocked ? "1" : "0");
        if (fpsAuto) {
            env.put("DM_FPS_TARGET", "auto");
            env.put("DM_FPS_CEILING", "auto");
            env.put("DM_FPS_HOLD", "auto");
            env.put("DM_PRESENT_INTERVAL", "auto");
            env.put("DM_VK_WINDOW_GAME_RATE", "auto");
        } else if (fps30) {
            env.put("DM_FPS_TARGET", "30");
            env.put("DM_FPS_CEILING", "30");
            env.put("DM_FPS_HOLD", "on");
            env.put("DM_PRESENT_INTERVAL", "game");
            env.put("DM_VK_WINDOW_GAME_RATE", "0");
        } else if (fps45) {
            env.put("DM_FPS_TARGET", "60");
            env.put("DM_FPS_CEILING", "45");
            env.put("DM_FPS_HOLD", "off");
            env.put("DM_PRESENT_INTERVAL", "display");
            env.put("DM_VK_WINDOW_GAME_RATE", "1");
        } else {
            env.put("DM_FPS_TARGET", "60");
            env.put("DM_FPS_CEILING", "0");
            env.put("DM_FPS_HOLD", "off");
            env.put("DM_PRESENT_INTERVAL", "display");
            env.put("DM_VK_WINDOW_GAME_RATE", "1");
        }
        env.remove("DM_GPU_TURBO");
        env.put("DM_ADAPTIVE_FPS", "0");
        env.put("DM_VK_RENDER_SCALE", vkScaleVal);
        if (c.shadowScaleMode == SHADOW_HALF) env.put("DM_VK_SHADOW_SCALE", "0.50");
        else if (c.shadowScaleMode == SHADOW_THREE_QUARTER) env.put("DM_VK_SHADOW_SCALE", "0.75");
        else if (c.shadowScaleMode == SHADOW_FULL) env.put("DM_VK_SHADOW_SCALE", "1.0");
        else env.put("DM_VK_SHADOW_SCALE", "auto");
        if (c.texDownscaleMode == TEX_DS_FULL) env.put("DM_VK_TEX_DOWNSCALE", "1");
        else if (c.texDownscaleMode == TEX_DS_HALF) env.put("DM_VK_TEX_DOWNSCALE", "2");
        else if (c.texDownscaleMode == TEX_DS_QUARTER) env.put("DM_VK_TEX_DOWNSCALE", "4");
        else env.put("DM_VK_TEX_DOWNSCALE", "auto");
        if (c.lowCostPpMode == LOW_COST_PP_ON) env.put("DM_VK_LOW_COST_PP", "1");
        else if (c.lowCostPpMode == LOW_COST_PP_OFF) env.put("DM_VK_LOW_COST_PP", "0");
        else env.put("DM_VK_LOW_COST_PP", "auto");
        env.put("DM_VK_ROBUST_ACCESS", c.robustAccess ? "1" : "0");
        env.put("DM_VK_INLINE_RESOLVE", c.inlineResolve ? "1" : "0");
        env.remove("DM_RENDER_SCALE");
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
            env.put("DM_VK_DISABLE_MSAA", "auto");
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
        env.put("DM_VK_POOL_SAFE_SYNC", String.valueOf(clamp(c.poolSafeSyncMode, 0, 2)));
        env.put("DM_VK_SAFE1_HOT_BLOCKS", c.safe1HotBlocks ? "1" : "0");
        env.put("DM_VK_SAFE1_RB_STAGED", c.safe1RbStaged ? "1" : "0");
        env.put("DM_VK_SAFE1_DIFF1K", c.safe1Diff1k ? "1" : "0");
        env.put("DM_VK_POOL_OVERLAP", c.poolOverlap ? "1" : "0");
        env.put("DM_VK_UBD_COPYBACK", c.ubdCopybackMode == UBD_ASYNC_HYBRID6 ? "2" : "1");
        env.put("DM_VK_DUMP_3D_FRAME", c.dump3dFrame ? "1" : "0");
        env.put("DM_VK_STRICT_BARRIERS", c.strictBarriers ? "1" : "0");
        if (c.strictBarriers) {
            env.put("DM_VK_ORDER_KEEP", "0");
        } else {
            env.remove("DM_VK_ORDER_KEEP");
        }
        env.put("DM_VK_EMU_FIRST_INSTANCE", c.emuFirstInstance ? "1" : "0");
        if (c.lowRamMode == LOW_RAM_STANDARD) env.put("DM_LOW_RAM", "0");
        else if (c.lowRamMode == LOW_RAM_4GB) env.put("DM_LOW_RAM", "1");
        else if (c.lowRamMode == LOW_RAM_3GB) env.put("DM_LOW_RAM", "2");
        else env.put("DM_LOW_RAM", "auto");

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
