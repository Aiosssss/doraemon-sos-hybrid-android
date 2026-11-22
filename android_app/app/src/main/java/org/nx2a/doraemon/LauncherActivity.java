package org.nx2a.doraemon;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;
import org.nx2a.GameActivity;

import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;

/**
 * Standalone Hybrid Android Launcher for DORAEMON STORY OF SEASONS: Friends of the Great Kingdom.
 *
 * Sections:
 *   1. Setup & Extraction (SAF pickers for prod.keys, Base NSP, Update 1.1.2 NSP, 0..3 DLC NSPs,
 *      storage check, ExtractForegroundService progress + Cancel, strict Play gate).
 *   2. Runtime Settings (100% real knobs synced to game_data/env.txt & game_data/settings.txt).
 *   3. Diagnostics, Logs & Save Management (live Vulkan Mali/Adreno probe, Build IDs, AOC status,
 *      SAF Log Export, SAF Save Backup/Restore ZIP).
 */
public final class LauncherActivity extends Activity {
    private static final int REQ_PICK_KEYS = 101;
    private static final int REQ_PICK_BASE = 102;
    private static final int REQ_PICK_UPDATE = 103;
    private static final int REQ_PICK_DLCS = 104;
    private static final int REQ_EXPORT_LOG = 201;
    private static final int REQ_BACKUP_SAVE = 202;
    private static final int REQ_RESTORE_SAVE = 203;
    private static final int REQ_INSTALL_MOD = 204;
    private static final int REQ_POST_NOTIF = 301;

    private static final int COL_BG = Color.parseColor("#0B1320");
    private static final int COL_CARD = Color.parseColor("#132033");
    private static final int COL_BORDER = Color.parseColor("#233754");
    private static final int COL_TEXT = Color.parseColor("#EEF4FC");
    private static final int COL_MUTED = Color.parseColor("#9FB3CE");
    private static final int COL_ACCENT = Color.parseColor("#2196F3");
    private static final int COL_GREEN = Color.parseColor("#2E7D32");
    private static final int COL_AMBER = Color.parseColor("#E65100");
    private static final int COL_RED = Color.parseColor("#C62828");

    // Selected SAF URIs (in-memory for extraction session; prod.keys is NEVER persisted or logged)
    private Uri mKeysUri;
    private Uri mBaseUri;
    private Uri mUpdateUri;
    private final ArrayList<Uri> mDlcUris = new ArrayList<>();

    // UI references
    private TextView mStatusBanner;
    private TextView mStorageText;
    private TextView mKeysLabel;
    private TextView mBaseLabel;
    private TextView mUpdateLabel;
    private TextView mDlcsLabel;
    private ProgressBar mProgressBar;
    private TextView mProgressText;
    private Button mBtnExtract;
    private Button mBtnCancel;
    private Button mBtnPlay;

    private TextView mDiagGpuText;
    private TextView mDiagGameText;
    private TextView mModStatusText;
    private Button mBtnInstallMod;
    private Button mBtnRestoreMod;
    private Switch mDockedSwitch;

    private RuntimeConfigManager.Config mConfig;
    private boolean mUpdatingUi = false;
    private boolean mReceiverRegistered = false;
    private String mVulkanProbeJson = "";

    private final BroadcastReceiver mExtractReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            updateExtractionUi(ExtractForegroundService.getSnapshot());
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mConfig = RuntimeConfigManager.load(this);
        buildUi();
        probeVulkanAsync();
        syncCoreIfApkUpdatedAsync();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mReceiverRegistered) {
            IntentFilter filter = new IntentFilter(ExtractForegroundService.ACTION_STATE);
            if (Build.VERSION.SDK_INT >= 33) {
                registerReceiver(mExtractReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
            } else {
                registerReceiver(mExtractReceiver, filter);
            }
            mReceiverRegistered = true;
        }
        mConfig = RuntimeConfigManager.load(this);
        refreshInstallAndStorageStatus();
        updateExtractionUi(ExtractForegroundService.getSnapshot());
    }

    @Override
    protected void onPause() {
        if (mReceiverRegistered) {
            try {
                unregisterReceiver(mExtractReceiver);
            } catch (IllegalArgumentException ignored) {
            }
            mReceiverRegistered = false;
        }
        super.onPause();
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(COL_BG);
        scroll.setFillViewport(true);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, dp(24), pad, dp(32));
        scroll.addView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        // ---- Header ----
        TextView title = makeText("Doraemon Story of Seasons", 22, COL_TEXT, true);
        TextView sub = makeText(getString(R.string.app_subtitle)
                + " • v" + BuildConfig.VERSION_NAME + " (ARM64 Unified Mali + Adreno)", 12, COL_MUTED, false);
        root.addView(title);
        root.addView(sub, marginTop(4));

        // ---- Card 1: Setup & Extraction ----
        LinearLayout setupCard = makeCard();
        setupCard.addView(makeText("1. Setup & Game Extraction (SAF)", 16, COL_TEXT, true));

        mStatusBanner = makeText("Checking game data...", 13, COL_TEXT, true);
        mStatusBanner.setPadding(dp(10), dp(8), dp(10), dp(8));
        mStatusBanner.setBackground(makeRoundedBg(Color.parseColor("#1B2D47"), 8));
        setupCard.addView(mStatusBanner, marginTop(10));

        mStorageText = makeText("Free internal storage: checking...", 12, COL_MUTED, false);
        setupCard.addView(mStorageText, marginTop(6));

        mBtnPlay = makeButton("PLAY GAME", COL_GREEN);
        mBtnPlay.setEnabled(false);
        mBtnPlay.setOnClickListener(v -> launchGame());
        setupCard.addView(mBtnPlay, marginTop(10));

        // SAF File pickers
        mKeysLabel = addFilePickerRow(setupCard, "Select prod.keys", "Not selected (required)",
                v -> openDocumentPicker(REQ_PICK_KEYS, false));
        mBaseLabel = addFilePickerRow(setupCard, "Select Base NSP", "01008EE0177A8000 (required)",
                v -> openDocumentPicker(REQ_PICK_BASE, false));
        mUpdateLabel = addFilePickerRow(setupCard, "Select Update 1.1.2 NSP", "01008EE0177A8800 v262144 (required)",
                v -> openDocumentPicker(REQ_PICK_UPDATE, false));
        mDlcsLabel = addFilePickerRow(setupCard, "Select DLC NSPs (0-3)", "Optional (AOC 1, 11, 201 — multi-select)",
                v -> openDocumentPicker(REQ_PICK_DLCS, true));

        // Extract & Cancel buttons
        LinearLayout actionRow = new LinearLayout(this);
        actionRow.setOrientation(LinearLayout.HORIZONTAL);
        mBtnExtract = makeButton("Extract & Verify", COL_ACCENT);
        mBtnExtract.setOnClickListener(v -> onExtractClicked());
        mBtnCancel = makeButton("Cancel", COL_RED);
        mBtnCancel.setVisibility(View.GONE);
        mBtnCancel.setOnClickListener(v -> {
            Intent ci = new Intent(this, ExtractForegroundService.class);
            ci.setAction(ExtractForegroundService.ACTION_CANCEL);
            startService(ci);
        });
        LinearLayout.LayoutParams lpLeft = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        actionRow.addView(mBtnExtract, lpLeft);
        LinearLayout.LayoutParams lpRight = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 0.6f);
        lpRight.leftMargin = dp(8);
        actionRow.addView(mBtnCancel, lpRight);
        setupCard.addView(actionRow, marginTop(12));

        mProgressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        mProgressBar.setMax(100);
        mProgressBar.setVisibility(View.GONE);
        setupCard.addView(mProgressBar, marginTop(10));

        mProgressText = makeText("", 12, COL_MUTED, false);
        mProgressText.setVisibility(View.GONE);
        setupCard.addView(mProgressText, marginTop(4));

        root.addView(setupCard, marginTop(14));

        // ---- Card 2: Runtime Settings ----
        LinearLayout settingsCard = makeCard();
        settingsCard.addView(makeText("2. Runtime Settings (env.txt & settings.txt)", 16, COL_TEXT, true));
        settingsCard.addView(makeText("Every switch directly controls libdmhost.so. Gamepad button layout can also be customized live in the in-game Pause Menu.", 11, COL_MUTED, false), marginTop(4));

        // Render Scale spinner (0.75x / 0.85x Balanced 60 FPS / 1.0x Native / 1.25x Docked 1080p HQ)
        final int[] scalePresetMap = {
                RuntimeConfigManager.SCALE_075,
                RuntimeConfigManager.SCALE_085,
                RuntimeConfigManager.SCALE_100,
                RuntimeConfigManager.SCALE_125
        };
        int initScaleIdx = 2; // default 1.0x
        for (int i = 0; i < scalePresetMap.length; i++) {
            if (scalePresetMap[i] == mConfig.renderScalePreset) {
                initScaleIdx = i;
                break;
            }
        }
        addSpinnerRow(settingsCard, "Render Scale",
                new String[]{"0.75x (Performance)", "0.85x (Balanced 60 FPS)", "1.0x (Native)", "1.25x (Docked 1080p HQ)"},
                initScaleIdx,
                pos -> {
                    int preset = (pos >= 0 && pos < scalePresetMap.length) ? scalePresetMap[pos] : RuntimeConfigManager.SCALE_100;
                    mConfig.renderScalePreset = preset;
                    if (preset == RuntimeConfigManager.SCALE_125 && !mConfig.dockedMode) {
                        mConfig.dockedMode = true;
                        if (mDockedSwitch != null) {
                            mUpdatingUi = true;
                            mDockedSwitch.setChecked(true);
                            mUpdatingUi = false;
                        }
                    }
                    persistConfig();
                });

        // Target FPS spinner with explicit real 60 FPS unlock & Mali-G720 Handheld 720p recommendation
        Spinner fpsSpinner = addSpinnerRow(settingsCard, "Target FPS",
                new String[]{"30 FPS (Default / Switch Native)", "60 FPS Unlock (Rec: Handheld 720p + 0.85x–1.0x)"},
                mConfig.targetFps == 60 ? 1 : 0,
                null);
        fpsSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                if (mUpdatingUi) return;
                if (pos == 1 && mConfig.targetFps != 60) {
                    new AlertDialog.Builder(LauncherActivity.this)
                            .setTitle("60 FPS Unlock (DM_PRESENT_INTERVAL=display)")
                            .setMessage("Doraemon Story of Seasons natively locks to 30 FPS via Unity vSyncCount=2. "
                                    + "Enabling 60 FPS forces DM_PRESENT_INTERVAL=display, DM_FPS_CEILING=0, DM_FPS_HOLD=off, "
                                    + "and pairs with Mali Copied-Pool Overlap (DM_VK_POOL_OVERLAP=1).\n\n"
                                    + "• Recommended for stable 60 FPS on Mali-G720: keep Docked Mode OFF (Handheld 720p) "
                                    + "and use Render Scale 0.85x–1.0x (GPU p50 ~11–13.5 ms < 16.67 ms budget).\n"
                                    + "• Cutscenes/UI timers tied to frame counts may run faster.\n\n"
                                    + "Enable 60 FPS unlock?")
                            .setPositiveButton("Enable 60 FPS", (d, w) -> {
                                mConfig.targetFps = 60;
                                persistConfig();
                            })
                            .setNegativeButton("Keep 30 FPS", (d, w) -> {
                                mUpdatingUi = true;
                                fpsSpinner.setSelection(0);
                                mUpdatingUi = false;
                            })
                            .setCancelable(false)
                            .show();
                } else if (pos == 0 && mConfig.targetFps != 30) {
                    mConfig.targetFps = 30;
                    persistConfig();
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {}
        });

        // Docked / Handheld switch
        mDockedSwitch = addSwitchRow(settingsCard, "Docked Mode (1080p vs 720p Handheld)", mConfig.dockedMode, checked -> {
            mConfig.dockedMode = checked;
            persistConfig();
        });

        // Audio Volume SeekBar
        addSeekBarRow(settingsCard, "Audio Volume (DM_AUDIO_VOLUME)", 0, 100, mConfig.audioVolumePct, "%", val -> {
            mConfig.audioVolumePct = val;
            persistConfig();
        });

        // Virtual Gamepad switches
        addSwitchRow(settingsCard, "Virtual On-Screen Gamepad", mConfig.overlayEnabled, checked -> {
            mConfig.overlayEnabled = checked;
            persistConfig();
        });
        addSeekBarRow(settingsCard, "Virtual Gamepad Opacity", 20, 80, mConfig.overlayOpacity, "%", val -> {
            mConfig.overlayOpacity = val;
            persistConfig();
        });
        addSwitchRow(settingsCard, "Controller Rumble & Tap Haptics", mConfig.rumbleEnabled, checked -> {
            mConfig.rumbleEnabled = checked;
            persistConfig();
        });
        addSwitchRow(settingsCard, "In-Game FPS Counter", mConfig.fpsCounter, checked -> {
            mConfig.fpsCounter = checked;
            persistConfig();
        });

        // Mali / Vulkan Advanced Settings
        addSwitchRow(settingsCard, "Mali Persistent BC Texture Disk Cache", mConfig.bcDiskCache, checked -> {
            mConfig.bcDiskCache = checked;
            persistConfig();
        });
        addSpinnerRow(settingsCard, "2x MSAA Render Targets",
                new String[]{"Native 2x MSAA (Default / Accurate)", "Disable MSAA 1x (Experimental)", "Force Native 2x MSAA (Explicit)"},
                mConfig.msaaMode,
                pos -> {
                    mConfig.msaaMode = pos;
                    persistConfig();
                });
        addSwitchRow(settingsCard, "Async NVN Replay Thread (Experimental • Big-Core)", mConfig.asyncReplay, checked -> {
            mConfig.asyncReplay = checked;
            persistConfig();
        });
        addSpinnerRow(settingsCard, "Vulkan Zero-Copy Quirk (Mali M1)",
                new String[]{"Auto (Mali=Safe / Adreno=ZeroCopy)", "Force Safe Mode (NO_ZEROCOPY=1)", "Force Zero-Copy (NO_ZEROCOPY=0)"},
                mConfig.zeroCopyQuirk,
                pos -> {
                    mConfig.zeroCopyQuirk = pos;
                    persistConfig();
                });

        // Mali TBDR / Visual & Sync Diagnostics (v1.1.2-hybrid7)
        settingsCard.addView(makeText("Mali TBDR / Visual & Sync Diagnostics (A/B)", 13, COL_ACCENT, true), marginTop(14));
        addSpinnerRow(settingsCard, "2x MSAA Alpha-To-Coverage (Foliage Solid Fix)",
                new String[]{"Auto (Mali=Solid Cutout / Adreno=Native A2C)", "Disable A2C (Solid Cutout Trees)", "Force Native A2C (Debug)"},
                mConfig.a2cMode,
                pos -> {
                    mConfig.a2cMode = pos;
                    persistConfig();
                });
        addSwitchRow(settingsCard, "Mali Pool CPU-GPU Overlap (DM_VK_POOL_OVERLAP — Cuts fence_wait for 60 FPS)",
                mConfig.poolOverlap, checked -> {
                    mConfig.poolOverlap = checked;
                    persistConfig();
                });
        addSwitchRow(settingsCard, "Strict Pool Sync (DM_VK_POOL_SAFE_SYNC — Disables Overlap/Hot-Blocks, 30 FPS)",
                mConfig.poolSafeSync, checked -> {
                    mConfig.poolSafeSync = checked;
                    persistConfig();
                });
        addSwitchRow(settingsCard, "Strict Vulkan Barriers (DM_VK_STRICT_BARRIERS — VkRenderPass2, No Clear-Fold)",
                mConfig.strictBarriers, checked -> {
                    mConfig.strictBarriers = checked;
                    persistConfig();
                });
        addSwitchRow(settingsCard, "FirstInstance Emulation (DM_VK_EMU_FIRST_INSTANCE — Mali Divisor Fix)",
                mConfig.emuFirstInstance, checked -> {
                    mConfig.emuFirstInstance = checked;
                    persistConfig();
                });
        addSwitchRow(settingsCard, "1-Shot 3D Render-Graph Dump (DM_VK_DUMP_3D_FRAME — Logs 1 Frame to log.txt)",
                mConfig.dump3dFrame, checked -> {
                    mConfig.dump3dFrame = checked;
                    persistConfig();
                });

        root.addView(settingsCard, marginTop(14));

        // ---- Card 3: Diagnostics, Logs & Saves ----
        LinearLayout diagCard = makeCard();
        diagCard.addView(makeText("3. Diagnostics, Logs & Save Data", 16, COL_TEXT, true));

        mDiagGpuText = makeText("Probing Vulkan GPU capabilities...", 12, COL_MUTED, false);
        mDiagGpuText.setTypeface(Typeface.MONOSPACE);
        diagCard.addView(mDiagGpuText, marginTop(8));

        mDiagGameText = makeText("Inspecting installation manifest...", 12, COL_MUTED, false);
        mDiagGameText.setTypeface(Typeface.MONOSPACE);
        diagCard.addView(mDiagGameText, marginTop(8));

        Button btnLog = makeButton("Export Runtime & Crash Log (.txt)", Color.parseColor("#37474F"));
        btnLog.setOnClickListener(v -> startCreateDocument(REQ_EXPORT_LOG, "text/plain",
                "doraemon_hybrid_log_" + timestampSlug() + ".txt"));
        diagCard.addView(btnLog, marginTop(12));

        LinearLayout saveRow = new LinearLayout(this);
        saveRow.setOrientation(LinearLayout.HORIZONTAL);
        Button btnBackup = makeButton("Backup Save (.zip)", Color.parseColor("#00695C"));
        btnBackup.setOnClickListener(v -> startCreateDocument(REQ_BACKUP_SAVE, "application/zip",
                "doraemon_save_" + timestampSlug() + ".zip"));
        Button btnRestore = makeButton("Restore Save (.zip)", Color.parseColor("#4E342E"));
        btnRestore.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            startActivityForResult(i, REQ_RESTORE_SAVE);
        });
        LinearLayout.LayoutParams sLp1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        LinearLayout.LayoutParams sLp2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        sLp2.leftMargin = dp(8);
        saveRow.addView(btnBackup, sLp1);
        saveRow.addView(btnRestore, sLp2);
        diagCard.addView(saveRow, marginTop(8));

        root.addView(diagCard, marginTop(14));

        // ---- Card 4: Mod Bahasa Indonesia / RomFS Overlay (.zip) ----
        LinearLayout modCard = makeCard();
        modCard.addView(makeText("4. Mod Bahasa Indonesia / RomFS Overlay (.zip)", 16, COL_TEXT, true));
        modCard.addView(makeText("Applies mod .zip directly into game_data/RomFS/ with automatic first-time backup of original files (romfs_orig_backup/) so you can restore original in 1 second without re-extracting NSP.", 11, COL_MUTED, false), marginTop(4));

        mModStatusText = makeText("Status: Original RomFS (no mod active)", 12, COL_TEXT, true);
        mModStatusText.setPadding(dp(10), dp(8), dp(10), dp(8));
        mModStatusText.setBackground(makeRoundedBg(Color.parseColor("#1B2D47"), 8));
        modCard.addView(mModStatusText, marginTop(10));

        LinearLayout modRow = new LinearLayout(this);
        modRow.setOrientation(LinearLayout.HORIZONTAL);
        mBtnInstallMod = makeButton("Install Mod (.zip)", Color.parseColor("#1565C0"));
        mBtnInstallMod.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            startActivityForResult(i, REQ_INSTALL_MOD);
        });
        mBtnRestoreMod = makeButton("Disable / Restore Original", Color.parseColor("#6A1B9A"));
        mBtnRestoreMod.setOnClickListener(v -> confirmRestoreOriginalRomfs());

        LinearLayout.LayoutParams mLp1 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        LinearLayout.LayoutParams mLp2 = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        mLp2.leftMargin = dp(8);
        modRow.addView(mBtnInstallMod, mLp1);
        modRow.addView(mBtnRestoreMod, mLp2);
        modCard.addView(modRow, marginTop(10));

        root.addView(modCard, marginTop(14));
        setContentView(scroll);
    }

    private void probeVulkanAsync() {
        new Thread(() -> {
            String json = NativeBridge.isLoaded()
                    ? NativeBridge.nativeProbeVulkan()
                    : "{\"available\":false,\"error\":\"libnxhybrid_extract.so not loaded\"}";
            mVulkanProbeJson = json;
            String formatted = formatVulkanProbe(json);
            runOnUiThread(() -> mDiagGpuText.setText(formatted));
        }, "vk-probe").start();
    }

    private void syncCoreIfApkUpdatedAsync() {
        new Thread(() -> {
            CoreAssetSync.Status st = CoreAssetSync.inspectStatus(this);
            if (st.gameExtracted && !st.coreSynced) {
                try {
                    CoreAssetSync.syncCoreAssets(this, false);
                    RuntimeConfigManager.applyToGameData(this, RuntimeConfigManager.load(this), null);
                } catch (Exception e) {
                    runOnUiThread(() -> Toast.makeText(this,
                            "Core asset sync error: " + e.getMessage(), Toast.LENGTH_LONG).show());
                }
            }
            runOnUiThread(this::refreshInstallAndStorageStatus);
        }, "core-sync").start();
    }

    private void refreshInstallAndStorageStatus() {
        CoreAssetSync.Status st = CoreAssetSync.inspectStatus(this);
        boolean extracting = ExtractForegroundService.isRunning();

        mStatusBanner.setText(st.summary);
        mStatusBanner.setBackground(makeRoundedBg(
                st.readyToPlay ? COL_GREEN : (st.gameExtracted ? COL_AMBER : Color.parseColor("#1B2D47")), 8));

        // Strict Play button gate: disabled until BOTH .ready.json AND .core_assets.json verified intact
        mBtnPlay.setEnabled(st.readyToPlay && !extracting);
        mBtnPlay.setAlpha((st.readyToPlay && !extracting) ? 1.0f : 0.45f);

        long freeBytes = NativeBridge.isLoaded()
                ? NativeBridge.nativeQueryFreeSpace(getFilesDir().getAbsolutePath())
                : getFilesDir().getUsableSpace();
        mStorageText.setText(String.format(Locale.US,
                "Free internal storage: %.2f GiB (minimum 3.00 GiB required for extraction)",
                freeBytes / 1073741824.0));

        SaveAndLogManager.ModStatus mod = SaveAndLogManager.getRomfsModStatus(this);
        if (mModStatusText != null) {
            if (mod.active) {
                mModStatusText.setText(String.format(Locale.US,
                        "Active Mod: %s (%d files • %d overwritten, %d added • %s)",
                        mod.modName, mod.totalFiles, mod.overwrittenCount, mod.addedCount, mod.installedAt));
                mModStatusText.setBackground(makeRoundedBg(COL_GREEN, 8));
            } else {
                mModStatusText.setText("Status: Original RomFS (no mod overlay active)");
                mModStatusText.setBackground(makeRoundedBg(Color.parseColor("#1B2D47"), 8));
            }
        }
        if (mBtnInstallMod != null) {
            mBtnInstallMod.setEnabled(st.gameExtracted && !extracting);
            mBtnInstallMod.setAlpha((st.gameExtracted && !extracting) ? 1.0f : 0.45f);
        }
        if (mBtnRestoreMod != null) {
            mBtnRestoreMod.setEnabled(st.gameExtracted && mod.active && !extracting);
            mBtnRestoreMod.setAlpha((st.gameExtracted && mod.active && !extracting) ? 1.0f : 0.45f);
        }

        StringBuilder diag = new StringBuilder();
        diag.append("APK Version : ").append(BuildConfig.VERSION_NAME)
                .append(" (code ").append(BuildConfig.VERSION_CODE).append(")\n");
        if (st.gameExtracted) {
            diag.append("Title ID    : ").append(st.titleId).append(" (v").append(st.displayVersion).append(")\n");
            diag.append("RomFS       : ").append(st.romfsFiles).append(" files (")
                    .append(String.format(Locale.US, "%.2f GiB", st.romfsBytes / 1073741824.0)).append(")\n");
            diag.append("RomFS Mod   : ").append(mod.active
                    ? (mod.modName + " (" + mod.totalFiles + " files active)")
                    : "Original (none)").append("\n");
            diag.append("DLC (AOC)   : ").append(st.aocSummary).append("\n");
            diag.append("main BID    : ").append(st.mainBuildId).append("\n");
            diag.append("subsdk0 BID : ").append(st.subsdk0BuildId).append("\n");
            diag.append("sdk BID     : ").append(st.sdkBuildId).append("\n");
            diag.append("Core Assets : ").append(st.coreSynced ? "VERIFIED (shaders.dmsv SHA-256 OK)" : "SYNC PENDING");
        } else {
            diag.append("Game Data   : Not installed yet\n");
            diag.append("RomFS Mod   : Original (awaiting extraction)\n");
            diag.append("Core Assets : Bundled inside APK (awaiting extraction)");
        }
        mDiagGameText.setText(diag.toString());
    }

    private void updateExtractionUi(ExtractForegroundService.Snapshot snap) {
        boolean running = snap.running || ExtractForegroundService.isRunning();
        mBtnExtract.setEnabled(!running);
        mBtnCancel.setVisibility(running ? View.VISIBLE : View.GONE);
        if (running || !snap.message.isEmpty()) {
            mProgressBar.setVisibility(running ? View.VISIBLE : View.GONE);
            mProgressBar.setIndeterminate(running && snap.percent == 0);
            mProgressBar.setProgress(snap.percent);
            mProgressText.setVisibility(View.VISIBLE);
            mProgressText.setText(snap.message);
        }
        refreshInstallAndStorageStatus();
    }

    private void onExtractClicked() {
        if (mKeysUri == null || mBaseUri == null || mUpdateUri == null) {
            Toast.makeText(this, "Please select prod.keys, Base NSP, and Update 1.1.2 NSP first.",
                    Toast.LENGTH_LONG).show();
            return;
        }
        // Handle POST_NOTIFICATIONS on Android 13+ gracefully (never blocks extraction if denied)
        if (Build.VERSION.SDK_INT >= 33
                && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[]{Manifest.permission.POST_NOTIFICATIONS}, REQ_POST_NOTIF);
            return;
        }
        startExtractionService();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_POST_NOTIF) {
            // Proceed even if notification permission was denied
            startExtractionService();
        }
    }

    private void startExtractionService() {
        Intent i = new Intent(this, ExtractForegroundService.class);
        i.setAction(ExtractForegroundService.ACTION_START);
        i.putExtra(ExtractForegroundService.EXTRA_KEYS_URI, mKeysUri.toString());
        i.putExtra(ExtractForegroundService.EXTRA_BASE_URI, mBaseUri.toString());
        i.putExtra(ExtractForegroundService.EXTRA_UPDATE_URI, mUpdateUri.toString());
        ArrayList<String> dlcs = new ArrayList<>();
        for (Uri u : mDlcUris) dlcs.add(u.toString());
        i.putStringArrayListExtra(ExtractForegroundService.EXTRA_DLC_URIS, dlcs);
        startForegroundService(i);
        mBtnExtract.setEnabled(false);
        mBtnCancel.setVisibility(View.VISIBLE);
        mProgressBar.setVisibility(View.VISIBLE);
        mProgressBar.setIndeterminate(true);
        mProgressText.setVisibility(View.VISIBLE);
        mProgressText.setText("Starting extraction service...");
    }

    private void launchGame() {
        CoreAssetSync.Status st = CoreAssetSync.inspectStatus(this);
        if (!st.readyToPlay) {
            Toast.makeText(this, "Game data or core shaders not verified yet.", Toast.LENGTH_SHORT).show();
            refreshInstallAndStorageStatus();
            return;
        }
        try {
            RuntimeConfigManager.save(this, mConfig);
        } catch (Exception e) {
            Toast.makeText(this, "Warning: could not flush config: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
        Intent gameIntent = new Intent(this, GameActivity.class);
        startActivity(gameIntent);
    }

    private void persistConfig() {
        try {
            RuntimeConfigManager.save(this, mConfig);
        } catch (Exception e) {
            Toast.makeText(this, "Failed saving runtime config: " + e.getMessage(), Toast.LENGTH_SHORT).show();
        }
    }

    private void openDocumentPicker(int reqCode, boolean allowMultiple) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        if (allowMultiple) {
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        }
        startActivityForResult(i, reqCode);
    }

    private void startCreateDocument(int reqCode, String mime, String defaultName) {
        Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(mime);
        i.putExtra(Intent.EXTRA_TITLE, defaultName);
        startActivityForResult(i, reqCode);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;

        if (requestCode == REQ_PICK_KEYS && data.getData() != null) {
            mKeysUri = data.getData();
            mKeysLabel.setText("Selected: " + queryDisplayName(mKeysUri));
        } else if (requestCode == REQ_PICK_BASE && data.getData() != null) {
            mBaseUri = data.getData();
            mBaseLabel.setText("Selected: " + queryDisplayName(mBaseUri));
        } else if (requestCode == REQ_PICK_UPDATE && data.getData() != null) {
            mUpdateUri = data.getData();
            mUpdateLabel.setText("Selected: " + queryDisplayName(mUpdateUri));
        } else if (requestCode == REQ_PICK_DLCS) {
            mDlcUris.clear();
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) {
                    Uri u = clip.getItemAt(i).getUri();
                    if (u != null) mDlcUris.add(u);
                }
            } else if (data.getData() != null) {
                mDlcUris.add(data.getData());
            }
            mDlcsLabel.setText(mDlcUris.isEmpty()
                    ? "Optional (0 selected)"
                    : (mDlcUris.size() + " DLC file(s) selected"));
        } else if (requestCode == REQ_EXPORT_LOG && data.getData() != null) {
            final Uri dest = data.getData();
            new Thread(() -> {
                try {
                    int n = SaveAndLogManager.exportRuntimeLogs(this, dest, mVulkanProbeJson);
                    runOnUiThread(() -> Toast.makeText(this,
                            "Exported log report (" + (n / 1024) + " KiB)", Toast.LENGTH_LONG).show());
                } catch (Exception e) {
                    runOnUiThread(() -> Toast.makeText(this,
                            "Log export failed: " + e.getMessage(), Toast.LENGTH_LONG).show());
                }
            }).start();
        } else if (requestCode == REQ_BACKUP_SAVE && data.getData() != null) {
            final Uri dest = data.getData();
            new Thread(() -> {
                try {
                    int count = SaveAndLogManager.exportSaveZip(this, dest);
                    runOnUiThread(() -> Toast.makeText(this,
                            "Save backup complete (" + count + " files)", Toast.LENGTH_LONG).show());
                } catch (Exception e) {
                    runOnUiThread(() -> Toast.makeText(this,
                            "Save backup failed: " + e.getMessage(), Toast.LENGTH_LONG).show());
                }
            }).start();
        } else if (requestCode == REQ_RESTORE_SAVE && data.getData() != null) {
            final Uri src = data.getData();
            new AlertDialog.Builder(this)
                    .setTitle("Restore Save Data?")
                    .setMessage("This will atomically replace your current internal game save with the selected ZIP archive.")
                    .setPositiveButton("Restore", (d, w) -> new Thread(() -> {
                        try {
                            int count = SaveAndLogManager.importSaveZip(this, src);
                            runOnUiThread(() -> Toast.makeText(this,
                                    "Save restored (" + count + " files)", Toast.LENGTH_LONG).show());
                        } catch (Exception e) {
                            runOnUiThread(() -> Toast.makeText(this,
                                    "Save restore failed: " + e.getMessage(), Toast.LENGTH_LONG).show());
                        }
                    }).start())
                    .setNegativeButton("Cancel", null)
                    .show();
        } else if (requestCode == REQ_INSTALL_MOD && data.getData() != null) {
            final Uri modUri = data.getData();
            final String modName = queryDisplayName(modUri);
            new AlertDialog.Builder(this)
                    .setTitle("Install Mod Overlay (.zip)?")
                    .setMessage("Apply \"" + modName + "\" into game_data/RomFS/?\n\n"
                            + "Original files overwritten for the first time will be backed up automatically to "
                            + "game_data/romfs_orig_backup/ so you can disable and restore original RomFS in 1 second.")
                    .setPositiveButton("Install Mod", (d, w) -> {
                        if (mBtnInstallMod != null) mBtnInstallMod.setEnabled(false);
                        if (mBtnRestoreMod != null) mBtnRestoreMod.setEnabled(false);
                        if (mModStatusText != null) mModStatusText.setText("Installing mod overlay: " + modName + "...");
                        new Thread(() -> {
                            try {
                                SaveAndLogManager.ModStatus res = SaveAndLogManager.installRomfsModZip(this, modUri, modName);
                                runOnUiThread(() -> {
                                    Toast.makeText(this,
                                            "Mod installed: " + res.modName + " (" + res.totalFiles + " files)",
                                            Toast.LENGTH_LONG).show();
                                    refreshInstallAndStorageStatus();
                                });
                            } catch (Exception e) {
                                runOnUiThread(() -> {
                                    Toast.makeText(this, "Mod install failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                                    refreshInstallAndStorageStatus();
                                });
                            }
                        }, "mod-install").start();
                    })
                    .setNegativeButton("Cancel", null)
                    .show();
        }
    }

    private void confirmRestoreOriginalRomfs() {
        new AlertDialog.Builder(this)
                .setTitle("Disable Mod & Restore Original RomFS?")
                .setMessage("This will restore all backed-up original RomFS files and remove mod-added files "
                        + "in ~1 second without re-extracting your 2.24 GiB NSP.")
                .setPositiveButton("Restore Original", (d, w) -> {
                    if (mBtnInstallMod != null) mBtnInstallMod.setEnabled(false);
                    if (mBtnRestoreMod != null) mBtnRestoreMod.setEnabled(false);
                    if (mModStatusText != null) mModStatusText.setText("Restoring original RomFS...");
                    new Thread(() -> {
                        try {
                            int n = SaveAndLogManager.restoreOriginalRomfs(this);
                            runOnUiThread(() -> {
                                Toast.makeText(this,
                                        "Original RomFS restored (" + n + " files reverted)",
                                        Toast.LENGTH_LONG).show();
                                refreshInstallAndStorageStatus();
                            });
                        } catch (Exception e) {
                            runOnUiThread(() -> {
                                Toast.makeText(this, "Restore failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
                                refreshInstallAndStorageStatus();
                            });
                        }
                    }, "mod-restore").start();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                if (name != null && !name.isEmpty()) return name;
            }
        } catch (Exception ignored) {
        }
        return uri.getLastPathSegment() != null ? uri.getLastPathSegment() : "selected file";
    }

    private String formatVulkanProbe(String json) {
        try {
            JSONObject j = new JSONObject(json);
            if (!j.optBoolean("available", false)) {
                return "Vulkan GPU  : Unavailable (" + j.optString("error", "unknown") + ")";
            }
            return "GPU Device  : " + j.optString("gpu_name") + "\n"
                    + "Vendor / ID : " + j.optString("vendor_id") + " / " + j.optString("device_id") + "\n"
                    + "Vulkan API  : " + j.optString("vulkan_api") + " (Driver " + j.optString("driver_version") + ")\n"
                    + "Compression : BC7=" + (j.optBoolean("bc7_sampled") ? "Native" : "Mali Fallback+Cache")
                    + " • ASTC_LDR=" + (j.optBoolean("astc_ldr") ? "Yes" : "No") + "\n"
                    + "Active Path : " + j.optString("pipeline_path");
        } catch (Exception e) {
            return "Vulkan Probe: " + json;
        }
    }

    private String timestampSlug() {
        return new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    }

    // ---- Lightweight UI builders (zero external dependencies) ----

    private LinearLayout makeCard() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        int p = dp(14);
        card.setPadding(p, p, p, p);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(COL_CARD);
        bg.setCornerRadius(dp(12));
        bg.setStroke(dp(1), COL_BORDER);
        card.setBackground(bg);
        return card;
    }

    private GradientDrawable makeRoundedBg(int color, int radiusDp) {
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(dp(radiusDp));
        return bg;
    }

    private TextView makeText(String text, int sp, int color, boolean bold) {
        TextView tv = new TextView(this);
        tv.setText(text);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        tv.setTextColor(color);
        if (bold) tv.setTypeface(tv.getTypeface(), Typeface.BOLD);
        return tv;
    }

    private Button makeButton(String label, int bgColor) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextColor(Color.WHITE);
        b.setAllCaps(false);
        b.setBackground(makeRoundedBg(bgColor, 8));
        b.setPadding(dp(12), dp(10), dp(12), dp(10));
        return b;
    }

    private TextView addFilePickerRow(LinearLayout parent, String btnLabel, String initSub, View.OnClickListener click) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        Button btn = makeButton(btnLabel, Color.parseColor("#1E3A5F"));
        btn.setOnClickListener(click);
        TextView tv = makeText(initSub, 11, COL_MUTED, false);
        tv.setPadding(dp(10), 0, 0, 0);

        row.addView(btn, new LinearLayout.LayoutParams(dp(170), ViewGroup.LayoutParams.WRAP_CONTENT));
        row.addView(tv, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        parent.addView(row, marginTop(8));
        return tv;
    }

    private interface IntConsumer { void accept(int value); }
    private interface BoolConsumer { void accept(boolean value); }

    private Spinner addSpinnerRow(LinearLayout parent, String label, String[] items, int initialIdx, IntConsumer onChange) {
        parent.addView(makeText(label, 12, COL_MUTED, true), marginTop(10));
        Spinner sp = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(adapter);
        mUpdatingUi = true;
        sp.setSelection(Math.max(0, Math.min(items.length - 1, initialIdx)));
        mUpdatingUi = false;
        if (onChange != null) {
            sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                @Override
                public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                    if (!mUpdatingUi) onChange.accept(pos);
                }

                @Override
                public void onNothingSelected(AdapterView<?> p) {}
            });
        }
        parent.addView(sp, marginTop(2));
        return sp;
    }

    private Switch addSwitchRow(LinearLayout parent, String label, boolean initial, BoolConsumer onChange) {
        Switch sw = new Switch(this);
        sw.setText(label);
        sw.setTextColor(COL_TEXT);
        sw.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        sw.setChecked(initial);
        sw.setOnCheckedChangeListener((btn, checked) -> {
            if (!mUpdatingUi) onChange.accept(checked);
        });
        parent.addView(sw, marginTop(10));
        return sw;
    }

    private void addSeekBarRow(LinearLayout parent, String label, int min, int max, int initial,
                               String suffix, IntConsumer onChange) {
        TextView tv = makeText(label + ": " + initial + suffix, 12, COL_TEXT, false);
        SeekBar sb = new SeekBar(this);
        sb.setMax(max - min);
        sb.setProgress(Math.max(0, Math.min(max - min, initial - min)));
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                int v = min + progress;
                tv.setText(label + ": " + v + suffix);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                onChange.accept(min + seekBar.getProgress());
            }
        });
        parent.addView(tv, marginTop(10));
        parent.addView(sb, marginTop(2));
    }

    private LinearLayout.LayoutParams marginTop(int topDp) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(topDp);
        return lp;
    }

    private int dp(int v) {
        return Math.round(TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
    }
}
