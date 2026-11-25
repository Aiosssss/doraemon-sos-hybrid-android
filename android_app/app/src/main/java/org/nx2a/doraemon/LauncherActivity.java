package org.nx2a.doraemon;

import android.Manifest;
import android.app.Activity;
import android.app.ActivityManager;
import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;
import org.nx2a.GameActivity;

import java.io.File;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;

/**
 * Landscape Storybook-themed Launcher for DORAEMON STORY OF SEASONS: Friends of the Great Kingdom.
 *
 * Localization:
 *   - Auto-detects Android system locale via resource qualifiers:
 *     Indonesian (values-in / values-id) -> Bahasa Indonesia
 *     All other system locales (values)  -> English (default)
 *
 * Main Screen (Landscape Fullscreen):
 *   - HD Farm Key-Art wallpaper (2048x1024); title logo & Doraemon/Nobita 100% unobstructed
 *   - 3 clear primary buttons neatly positioned strictly beneath the title logo: PLAY, SETTINGS, IMPORT GAME
 *   - Minimal bottom-left wooden status pill + compact hybrid27 version badge
 *   - Tiny crisp @AiosPort watermark pinned to the bottom-right corner
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
    private static final int REQ_IMPORT_CHEAT = 205;
    private static final int REQ_POST_NOTIF = 301;

    private static final int[] SCALE_PRESET_MAP = {
            RuntimeConfigManager.SCALE_AUTO,
            RuntimeConfigManager.SCALE_050,
            RuntimeConfigManager.SCALE_055,
            RuntimeConfigManager.SCALE_067,
            RuntimeConfigManager.SCALE_075,
            RuntimeConfigManager.SCALE_100
    };
    private static final int[] SHADOW_PRESET_MAP = {
            RuntimeConfigManager.SHADOW_AUTO,
            RuntimeConfigManager.SHADOW_HALF,
            RuntimeConfigManager.SHADOW_THREE_QUARTER,
            RuntimeConfigManager.SHADOW_FULL
    };
    private static final int[] FPS_PRESET_MAP = {
            RuntimeConfigManager.FPS_AUTO,
            RuntimeConfigManager.FPS_30_COOL,
            RuntimeConfigManager.FPS_45,
            RuntimeConfigManager.FPS_60
    };

    // Selected SAF URIs (in-memory only; prod.keys is NEVER persisted or logged)
    private Uri mKeysUri;
    private Uri mBaseUri;
    private Uri mUpdateUri;
    private final ArrayList<Uri> mDlcUris = new ArrayList<>();

    // Main screen views
    private LinearLayout mBtnMainPlay;
    private TextView mTxtMainPlay;
    private LinearLayout mBtnMainSettings;
    private LinearLayout mBtnMainImport;
    private View mStatusDot;
    private TextView mTxtStatusShort;
    private TextView mTxtVersionBadge;

    // Overlays
    private FrameLayout mOverlaySettings;
    private FrameLayout mOverlayImport;

    // Settings overlay views
    private TextView mTabBtnBasic;
    private TextView mTabBtnCheats;
    private TextView mTabBtnAdvanced;
    private View mTabSettingsBasic;
    private View mTabSettingsCheats;
    private View mTabSettingsAdvanced;
    private Switch mSwFlexAnalog;
    private Switch mSwAdaptiveBtn;
    private Switch mSwCheatMaster;
    private TextView mBtnImportCheat;
    private TextView mBtnClearCheats;
    private LinearLayout mBoxBuiltinCheats;
    private LinearLayout mBoxImportedCheats;
    private CheatManager.State mCheatState;
    private TextView mBtnPresetAuto;
    private TextView mBtnPresetLow;
    private TextView mBtnPresetTurbo;
    private TextView mBtnPresetBalanced;
    private TextView mBtnPresetHigh;
    private Spinner mSpRenderScale;
    private Spinner mSpShadowScale;
    private Spinner mSpShadowRate;
    private Spinner mSpScreenMode;
    private Spinner mSpPadLayout;
    private Spinner mSpSamsungSafe;
    private Switch mSwNativeSurfaceRes;
    private Spinner mSpTargetFps;
    private Spinner mSpMsaaMode;
    private Switch mSwDockedMode;
    private LinearLayout mCardSafeSyncStatus;
    private TextView mTxtSafeSyncCardStatus;
    private Spinner mSpSafeSync;
    private TextView mTxtAudioVol;
    private SeekBar mSeekAudioVol;
    private Switch mSwOverlay;
    private Switch mSwRumble;
    private Switch mSwFpsCounter;
    private TextView mTxtModStatus;
    private TextView mBtnInstallMod;
    private TextView mBtnRestoreMod;
    private TextView mTxtGpuRamInfo;
    private Switch mSwRobustAccess;
    private Switch mSwInlineResolve;
    private Switch mSwDeferredWait;
    private Switch mSwStrictBarriers;
    private Switch mSwBcCache;
    private Spinner mSpLowRam;
    private Spinner mSpLowCostPp;

    // Import overlay views
    private ImageView mChkKeys;
    private ImageView mChkBase;
    private ImageView mChkUpdate;
    private ImageView mChkDlcs;
    private TextView mLblPickKeys;
    private TextView mLblPickBase;
    private TextView mLblPickUpdate;
    private TextView mLblPickDlcs;
    private TextView mTxtImportStatus;
    private TextView mTxtImportStorage;
    private ProgressBar mProgImport;
    private TextView mTxtImportProgress;
    private TextView mBtnStartExtract;
    private TextView mBtnCancelExtract;
    private TextView mBtnReimportClean;

    private RuntimeConfigManager.Config mConfig;
    private boolean mUpdatingUi = false;
    private boolean mReceiverRegistered = false;
    private boolean mDeviceLostPrompted = false;
    private boolean mReadyToPlay = false;
    private long mLastHandledExtractEventId = 0L;
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
        setContentView(R.layout.activity_launcher);
        applyImmersiveFullscreen();
        SaveAndLogManager.installJavaCrashHandler(this);
        mConfig = RuntimeConfigManager.load(this);
        bindViews();
        wireEvents();
        syncSettingsUiFromConfig();
        probeVulkanAsync();
        syncCoreIfApkUpdatedAsync();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) applyImmersiveFullscreen();
    }

    @SuppressWarnings("deprecation")
    private void applyImmersiveFullscreen() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            getWindow().setDecorFitsSystemWindows(false);
            WindowInsetsController ctl = getWindow().getInsetsController();
            if (ctl != null) {
                ctl.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                ctl.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        }
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
        SaveAndLogManager.installJavaCrashHandler(this);
        mConfig = RuntimeConfigManager.load(this);
        syncSettingsUiFromConfig();
        refreshInstallAndStorageStatus();
        updateExtractionUi(ExtractForegroundService.getSnapshot());
        checkGpuDeviceLostRecovery();
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

    @Override
    public void onBackPressed() {
        if (mOverlaySettings != null && mOverlaySettings.getVisibility() == View.VISIBLE) {
            mOverlaySettings.setVisibility(View.GONE);
            return;
        }
        if (mOverlayImport != null && mOverlayImport.getVisibility() == View.VISIBLE) {
            mOverlayImport.setVisibility(View.GONE);
            return;
        }
        super.onBackPressed();
    }

    private void bindViews() {
        mBtnMainPlay = findViewById(R.id.btn_main_play);
        mTxtMainPlay = findViewById(R.id.txt_main_play);
        mBtnMainSettings = findViewById(R.id.btn_main_settings);
        mBtnMainImport = findViewById(R.id.btn_main_import);
        mStatusDot = findViewById(R.id.status_dot);
        mTxtStatusShort = findViewById(R.id.txt_status_short);
        mTxtVersionBadge = findViewById(R.id.txt_version_badge);
        if (mTxtVersionBadge != null) {
            mTxtVersionBadge.setText("FOTGK");
        }

        mOverlaySettings = findViewById(R.id.overlay_settings);
        mOverlayImport = findViewById(R.id.overlay_import);

        // Settings views
        mTabBtnBasic = findViewById(R.id.tab_btn_basic);
        mTabBtnCheats = findViewById(R.id.tab_btn_cheats);
        mTabBtnAdvanced = findViewById(R.id.tab_btn_advanced);
        mTabSettingsBasic = findViewById(R.id.tab_settings_basic);
        mTabSettingsCheats = findViewById(R.id.tab_settings_cheats);
        mTabSettingsAdvanced = findViewById(R.id.tab_settings_advanced);
        mSwFlexAnalog = findViewById(R.id.sw_flex_analog);
        mSwAdaptiveBtn = findViewById(R.id.sw_adaptive_btn);
        mSwCheatMaster = findViewById(R.id.sw_cheat_master);
        mBtnImportCheat = findViewById(R.id.btn_import_cheat);
        mBtnClearCheats = findViewById(R.id.btn_clear_cheats);
        mBoxBuiltinCheats = findViewById(R.id.box_builtin_cheats);
        mBoxImportedCheats = findViewById(R.id.box_imported_cheats);
        mBtnPresetAuto = findViewById(R.id.btn_preset_auto);
        mBtnPresetLow = findViewById(R.id.btn_preset_low);
        mBtnPresetTurbo = findViewById(R.id.btn_preset_turbo);
        mBtnPresetBalanced = findViewById(R.id.btn_preset_balanced);
        mBtnPresetHigh = findViewById(R.id.btn_preset_high);

        mSpRenderScale = findViewById(R.id.sp_render_scale);
        mSpShadowScale = findViewById(R.id.sp_shadow_scale);
        mSpShadowRate = findViewById(R.id.sp_shadow_rate);
        mSpScreenMode = findViewById(R.id.sp_screen_mode);
        mSpPadLayout = findViewById(R.id.sp_pad_layout);
        mSpSamsungSafe = findViewById(R.id.sp_samsung_safe);
        mSwNativeSurfaceRes = findViewById(R.id.sw_native_surface_res);
        mSpTargetFps = findViewById(R.id.sp_target_fps);
        mSpMsaaMode = findViewById(R.id.sp_msaa_mode);
        mSwDockedMode = findViewById(R.id.sw_docked_mode);
        mCardSafeSyncStatus = findViewById(R.id.card_safe_sync_status);
        mTxtSafeSyncCardStatus = findViewById(R.id.txt_safe_sync_card_status);
        mSpSafeSync = findViewById(R.id.sp_safe_sync);
        mTxtAudioVol = findViewById(R.id.txt_audio_vol);
        mSeekAudioVol = findViewById(R.id.seek_audio_vol);
        mSwOverlay = findViewById(R.id.sw_overlay);
        mSwRumble = findViewById(R.id.sw_rumble);
        mSwFpsCounter = findViewById(R.id.sw_fps_counter);
        mTxtModStatus = findViewById(R.id.txt_mod_status);
        mBtnInstallMod = findViewById(R.id.btn_install_mod);
        mBtnRestoreMod = findViewById(R.id.btn_restore_mod);

        mTxtGpuRamInfo = findViewById(R.id.txt_gpu_ram_info);
        mSwRobustAccess = findViewById(R.id.sw_robust_access);
        mSwInlineResolve = findViewById(R.id.sw_inline_resolve);
        mSwDeferredWait = findViewById(R.id.sw_deferred_wait);
        mSwStrictBarriers = findViewById(R.id.sw_strict_barriers);
        mSwBcCache = findViewById(R.id.sw_bc_cache);
        mSpLowRam = findViewById(R.id.sp_low_ram);
        mSpLowCostPp = findViewById(R.id.sp_low_cost_pp);

        // Import views
        mChkKeys = findViewById(R.id.chk_keys);
        mChkBase = findViewById(R.id.chk_base);
        mChkUpdate = findViewById(R.id.chk_update);
        mChkDlcs = findViewById(R.id.chk_dlcs);
        mLblPickKeys = findViewById(R.id.lbl_pick_keys);
        mLblPickBase = findViewById(R.id.lbl_pick_base);
        mLblPickUpdate = findViewById(R.id.lbl_pick_update);
        mLblPickDlcs = findViewById(R.id.lbl_pick_dlcs);
        mTxtImportStatus = findViewById(R.id.txt_import_status);
        mTxtImportStorage = findViewById(R.id.txt_import_storage);
        mProgImport = findViewById(R.id.prog_import);
        mTxtImportProgress = findViewById(R.id.txt_import_progress);
        mBtnStartExtract = findViewById(R.id.btn_start_extract);
        mBtnCancelExtract = findViewById(R.id.btn_cancel_extract);
        mBtnReimportClean = findViewById(R.id.btn_reimport_clean);

        // Setup Spinners with localized strings (auto-detects EN / ID via system locale)
        setupSpinner(mSpRenderScale, new String[]{
                getString(R.string.sp_scale_auto),
                getString(R.string.sp_scale_050),
                getString(R.string.sp_scale_055),
                getString(R.string.sp_scale_067),
                getString(R.string.sp_scale_075),
                getString(R.string.sp_scale_100)
        });
        setupSpinner(mSpShadowScale, new String[]{
                getString(R.string.sp_shadow_auto),
                getString(R.string.sp_shadow_050),
                getString(R.string.sp_shadow_075),
                getString(R.string.sp_shadow_100)
        });
        setupSpinner(mSpTargetFps, new String[]{
                getString(R.string.sp_fps_auto),
                getString(R.string.sp_fps_30),
                getString(R.string.sp_fps_45),
                getString(R.string.sp_fps_60)
        });
        setupSpinner(mSpMsaaMode, new String[]{
                getString(R.string.sp_msaa_auto),
                getString(R.string.sp_msaa_off),
                getString(R.string.sp_msaa_on)
        });
        setupSpinner(mSpSafeSync, new String[]{
                getString(R.string.sp_safe_sync_mode2),
                getString(R.string.sp_safe_sync_mode1)
        });
        setupSpinner(mSpLowRam, new String[]{
                getString(R.string.sp_low_ram_auto),
                getString(R.string.sp_low_ram_std),
                getString(R.string.sp_low_ram_4gb),
                getString(R.string.sp_low_ram_3gb)
        });
        setupSpinner(mSpLowCostPp, new String[]{
                getString(R.string.sp_low_cost_pp_auto),
                getString(R.string.sp_low_cost_pp_on),
                getString(R.string.sp_low_cost_pp_off)
        });
        if (mSpShadowRate != null) {
            setupSpinner(mSpShadowRate, new String[]{
                    getString(R.string.sp_shadow_rate_auto),
                    getString(R.string.sp_shadow_rate_full),
                    getString(R.string.sp_shadow_rate_half),
                    getString(R.string.sp_shadow_rate_off)
            });
        }
        if (mSpScreenMode != null) {
            setupSpinner(mSpScreenMode, new String[]{
                    getString(R.string.sp_screen_16_9),
                    getString(R.string.sp_screen_stretch),
                    getString(R.string.sp_screen_zoom)
            });
        }
        if (mSpPadLayout != null) {
            setupSpinner(mSpPadLayout, new String[]{
                    getString(R.string.sp_pad_nintendo),
                    getString(R.string.sp_pad_xbox)
            });
        }
        if (mSpSamsungSafe != null) {
            setupSpinner(mSpSamsungSafe, new String[]{
                    getString(R.string.sp_samsung_safe_auto),
                    getString(R.string.sp_samsung_safe_on),
                    getString(R.string.sp_samsung_safe_off)
            });
        }
    }

    private void setupSpinner(Spinner sp, String[] items) {
        ArrayAdapter<String> ad = new ArrayAdapter<>(this, R.layout.item_spinner_wood, items);
        ad.setDropDownViewResource(R.layout.item_spinner_wood_dropdown);
        sp.setAdapter(ad);
    }

    private void wireEvents() {
        // Main 3 Buttons
        mBtnMainPlay.setOnClickListener(v -> {
            if (!mReadyToPlay) {
                openImportOverlay();
                Toast.makeText(this, R.string.toast_import_first, Toast.LENGTH_SHORT).show();
            } else {
                launchGame();
            }
        });
        mBtnMainSettings.setOnClickListener(v -> openSettingsOverlay());
        mBtnMainImport.setOnClickListener(v -> openImportOverlay());

        // Overlay scrim & close buttons
        mOverlaySettings.setOnClickListener(v -> mOverlaySettings.setVisibility(View.GONE));
        findViewById(R.id.btn_close_settings).setOnClickListener(v -> mOverlaySettings.setVisibility(View.GONE));
        mOverlayImport.setOnClickListener(v -> mOverlayImport.setVisibility(View.GONE));
        findViewById(R.id.btn_close_import).setOnClickListener(v -> mOverlayImport.setVisibility(View.GONE));

        // Settings Tabs
        mTabBtnBasic.setOnClickListener(v -> showSettingsTab(0));
        if (mTabBtnCheats != null) mTabBtnCheats.setOnClickListener(v -> showSettingsTab(1));
        mTabBtnAdvanced.setOnClickListener(v -> showSettingsTab(2));

        // 5 Quick Presets (1-Click)
        mBtnPresetAuto.setOnClickListener(v -> applyPresetFromUi(RuntimeConfigManager.PRESET_AUTO, R.string.toast_preset_auto));
        mBtnPresetLow.setOnClickListener(v -> applyPresetFromUi(RuntimeConfigManager.PRESET_LOW_END, R.string.toast_preset_low));
        if (mBtnPresetTurbo != null) {
            mBtnPresetTurbo.setOnClickListener(v -> applyPresetFromUi(RuntimeConfigManager.PRESET_TURBO, R.string.toast_preset_turbo));
        }
        mBtnPresetBalanced.setOnClickListener(v -> applyPresetFromUi(RuntimeConfigManager.PRESET_BALANCED, R.string.toast_preset_balanced));
        mBtnPresetHigh.setOnClickListener(v -> applyPresetFromUi(RuntimeConfigManager.PRESET_HIGH, R.string.toast_preset_high));

        // Basic Settings listeners
        onSpinnerSelect(mSpRenderScale, pos -> {
            mConfig.renderScalePreset = (pos >= 0 && pos < SCALE_PRESET_MAP.length)
                    ? SCALE_PRESET_MAP[pos] : RuntimeConfigManager.SCALE_AUTO;
            updatePresetHighlight();
            persistConfig();
        });
        onSpinnerSelect(mSpShadowScale, pos -> {
            mConfig.shadowScaleMode = (pos >= 0 && pos < SHADOW_PRESET_MAP.length)
                    ? SHADOW_PRESET_MAP[pos] : RuntimeConfigManager.SHADOW_AUTO;
            updatePresetHighlight();
            persistConfig();
        });
        onSpinnerSelect(mSpTargetFps, pos -> {
            mConfig.targetFps = (pos >= 0 && pos < FPS_PRESET_MAP.length)
                    ? FPS_PRESET_MAP[pos] : RuntimeConfigManager.FPS_AUTO;
            updatePresetHighlight();
            persistConfig();
        });
        onSpinnerSelect(mSpMsaaMode, pos -> {
            mConfig.msaaMode = pos;
            updatePresetHighlight();
            persistConfig();
        });
        mSwDockedMode.setOnCheckedChangeListener((b, checked) -> {
            if (mUpdatingUi) return;
            mConfig.dockedMode = checked;
            updatePresetHighlight();
            persistConfig();
        });
        onSpinnerSelect(mSpSafeSync, pos -> {
            mConfig.poolSafeSyncMode = (pos == 1)
                    ? RuntimeConfigManager.SAFE_SYNC_PIPELINED
                    : RuntimeConfigManager.SAFE_SYNC_SERIAL_DBG;
            updateSafeSyncStatusCard();
            persistConfig();
        });
        mSeekAudioVol.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                mTxtAudioVol.setText(getString(R.string.lbl_audio_vol_fmt, progress));
            }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
                if (mUpdatingUi) return;
                mConfig.audioVolumePct = seekBar.getProgress();
                persistConfig();
            }
        });
        mSwOverlay.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.overlayEnabled = checked; persistConfig(); } });
        if (mSwFlexAnalog != null) {
            mSwFlexAnalog.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.flexibleAnalog = checked; persistConfig(); } });
        }
        if (mSwAdaptiveBtn != null) {
            mSwAdaptiveBtn.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.adaptiveButton = checked; persistConfig(); } });
        }
        mSwRumble.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.rumbleEnabled = checked; persistConfig(); } });
        mSwFpsCounter.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.fpsCounter = checked; persistConfig(); } });

        // Cheats Tab listeners (all default OFF)
        if (mSwCheatMaster != null) {
            mSwCheatMaster.setOnCheckedChangeListener((b, checked) -> {
                if (mUpdatingUi || mCheatState == null) return;
                mCheatState.master = checked;
                saveCheatsUi();
            });
        }
        if (mBtnImportCheat != null) {
            mBtnImportCheat.setOnClickListener(v -> {
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                i.addCategory(Intent.CATEGORY_OPENABLE);
                i.setType("*/*");
                i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"text/plain", "application/octet-stream", "*/*"});
                startActivityForResult(i, REQ_IMPORT_CHEAT);
            });
        }
        if (mBtnClearCheats != null) {
            mBtnClearCheats.setOnClickListener(v -> {
                int removed = CheatManager.clearImportedFiles(this);
                refreshCheatsUi();
                Toast.makeText(this, getString(R.string.toast_cheats_cleared, removed), Toast.LENGTH_SHORT).show();
            });
        }

        // Mod buttons
        mBtnInstallMod.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/zip", "application/x-zip-compressed", "application/octet-stream"});
            startActivityForResult(i, REQ_INSTALL_MOD);
        });
        mBtnRestoreMod.setOnClickListener(v -> confirmRestoreOriginalRomfs());

        // Advanced & Diagnostics buttons
        findViewById(R.id.btn_export_log).setOnClickListener(v -> startCreateDocument(
                REQ_EXPORT_LOG, "text/plain", "doraemon_hybrid_log_" + timestampSlug() + ".txt"));
        findViewById(R.id.btn_backup_save).setOnClickListener(v -> startCreateDocument(
                REQ_BACKUP_SAVE, "application/zip", "doraemon_save_" + timestampSlug() + ".zip"));
        findViewById(R.id.btn_restore_save).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("application/zip");
            startActivityForResult(i, REQ_RESTORE_SAVE);
        });
        findViewById(R.id.btn_reset_defaults).setOnClickListener(v -> new AlertDialog.Builder(this)
                .setTitle(R.string.dlg_reset_title)
                .setMessage(R.string.dlg_reset_msg)
                .setPositiveButton(R.string.btn_reset, (d, w) -> {
                    mConfig = RuntimeConfigManager.createDefaultConfig();
                    syncSettingsUiFromConfig();
                    persistConfig();
                    Toast.makeText(this, R.string.toast_reset_done, Toast.LENGTH_SHORT).show();
                })
                .setNegativeButton(R.string.btn_cancel, null)
                .show());

        // Advanced switches
        mSwRobustAccess.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.robustAccess = checked; persistConfig(); } });
        mSwInlineResolve.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.inlineResolve = checked; persistConfig(); } });
        mSwDeferredWait.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.deferredWait = checked; persistConfig(); } });
        mSwStrictBarriers.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.strictBarriers = checked; persistConfig(); } });
        mSwBcCache.setOnCheckedChangeListener((b, checked) -> { if (!mUpdatingUi) { mConfig.bcDiskCache = checked; persistConfig(); } });
        onSpinnerSelect(mSpLowRam, pos -> { mConfig.lowRamMode = pos; persistConfig(); });
        onSpinnerSelect(mSpLowCostPp, pos -> { mConfig.lowCostPpMode = pos; persistConfig(); });
        if (mSpShadowRate != null) {
            onSpinnerSelect(mSpShadowRate, pos -> { mConfig.shadowRateMode = pos; updatePresetHighlight(); persistConfig(); });
        }
        if (mSpScreenMode != null) {
            onSpinnerSelect(mSpScreenMode, pos -> { mConfig.screenMode = pos; persistConfig(); });
        }
        if (mSpPadLayout != null) {
            onSpinnerSelect(mSpPadLayout, pos -> { mConfig.padXboxLayout = (pos == 1); persistConfig(); });
        }
        if (mSpSamsungSafe != null) {
            onSpinnerSelect(mSpSamsungSafe, pos -> { mConfig.samsungSafeMode = pos; persistConfig(); });
        }
        if (mSwNativeSurfaceRes != null) {
            mSwNativeSurfaceRes.setOnCheckedChangeListener((b, checked) -> {
                if (!mUpdatingUi) { mConfig.nativeSurfaceRes = checked; persistConfig(); }
            });
        }

        // Import panel file pickers & actions
        findViewById(R.id.row_pick_keys).setOnClickListener(v -> openDocumentPicker(REQ_PICK_KEYS, false));
        findViewById(R.id.row_pick_base).setOnClickListener(v -> openDocumentPicker(REQ_PICK_BASE, false));
        findViewById(R.id.row_pick_update).setOnClickListener(v -> openDocumentPicker(REQ_PICK_UPDATE, false));
        findViewById(R.id.row_pick_dlcs).setOnClickListener(v -> openDocumentPicker(REQ_PICK_DLCS, true));
        mBtnStartExtract.setOnClickListener(v -> onExtractClicked());
        mBtnCancelExtract.setOnClickListener(v -> {
            Intent ci = new Intent(this, ExtractForegroundService.class);
            ci.setAction(ExtractForegroundService.ACTION_CANCEL);
            startService(ci);
        });
        mBtnReimportClean.setOnClickListener(v -> confirmCleanGameData());
    }

    private interface IntConsumer { void accept(int pos); }

    private void onSpinnerSelect(Spinner sp, IntConsumer cb) {
        sp.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                if (!mUpdatingUi) cb.accept(pos);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
    }

    private void openSettingsOverlay() {
        mOverlayImport.setVisibility(View.GONE);
        syncSettingsUiFromConfig();
        refreshCheatsUi();
        showSettingsTab(0);
        mOverlaySettings.setVisibility(View.VISIBLE);
    }

    private void openImportOverlay() {
        mOverlaySettings.setVisibility(View.GONE);
        refreshInstallAndStorageStatus();
        mOverlayImport.setVisibility(View.VISIBLE);
    }

    private void showSettingsTab(int tab) {
        mTabSettingsBasic.setVisibility(tab == 0 ? View.VISIBLE : View.GONE);
        if (mTabSettingsCheats != null) mTabSettingsCheats.setVisibility(tab == 1 ? View.VISIBLE : View.GONE);
        mTabSettingsAdvanced.setVisibility(tab == 2 ? View.VISIBLE : View.GONE);
        stylePill(mTabBtnBasic, tab == 0);
        stylePill(mTabBtnCheats, tab == 1);
        stylePill(mTabBtnAdvanced, tab == 2);
        if (tab == 1) refreshCheatsUi();
    }

    private void saveCheatsUi() {
        if (mCheatState == null) return;
        try {
            CheatManager.save(this, mCheatState);
        } catch (Exception e) {
            Toast.makeText(this, getString(R.string.toast_config_save_fail, e.getMessage()), Toast.LENGTH_SHORT).show();
        }
    }

    private void refreshCheatsUi() {
        if (mBoxBuiltinCheats == null || mBoxImportedCheats == null) return;
        mCheatState = CheatManager.load(this);
        mUpdatingUi = true;
        if (mSwCheatMaster != null) mSwCheatMaster.setChecked(mCheatState.master);
        mBoxBuiltinCheats.removeAllViews();
        mBoxImportedCheats.removeAllViews();
        int importedCount = 0;
        for (CheatManager.Entry entry : mCheatState.entries) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            int padV = (int) (3 * getResources().getDisplayMetrics().density);
            row.setPadding(0, padV, 0, padV);

            LinearLayout col = new LinearLayout(this);
            col.setOrientation(LinearLayout.VERTICAL);
            col.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

            TextView tvName = new TextView(this);
            tvName.setText(entry.name);
            tvName.setTextColor(Color.parseColor("#4A2E14"));
            tvName.setTextSize(11.5f);
            tvName.setMaxLines(1);
            tvName.setEllipsize(android.text.TextUtils.TruncateAt.END);
            col.addView(tvName);

            if (!entry.builtin) {
                TextView tvSrc = new TextView(this);
                tvSrc.setText(entry.source);
                tvSrc.setTextColor(Color.parseColor("#8A6A4A"));
                tvSrc.setTextSize(9f);
                col.addView(tvSrc);
                importedCount++;
            }

            Switch sw = new Switch(this);
            sw.setChecked(entry.enabled);
            sw.setOnCheckedChangeListener((btn, checked) -> {
                if (mUpdatingUi) return;
                CheatManager.toggleEntry(mCheatState, entry, checked);
                saveCheatsUi();
                refreshCheatsUi();
            });

            row.addView(col);
            row.addView(sw);
            if (entry.builtin) mBoxBuiltinCheats.addView(row);
            else mBoxImportedCheats.addView(row);
        }
        if (importedCount == 0) {
            TextView empty = new TextView(this);
            empty.setText(R.string.cheats_imported_empty);
            empty.setTextColor(Color.parseColor("#8A6A4A"));
            empty.setTextSize(10.5f);
            mBoxImportedCheats.addView(empty);
        }
        mUpdatingUi = false;
    }

    private void applyPresetFromUi(int preset, int toastResId) {
        RuntimeConfigManager.applyQuickPreset(mConfig, preset);
        syncSettingsUiFromConfig();
        persistConfig();
        Toast.makeText(this, toastResId, Toast.LENGTH_SHORT).show();
    }

    private void updateSafeSyncStatusCard() {
        if (mTxtSafeSyncCardStatus == null || mCardSafeSyncStatus == null) return;
        boolean expMode1 = (mConfig.poolSafeSyncMode == RuntimeConfigManager.SAFE_SYNC_PIPELINED);
        if (expMode1) {
            mCardSafeSyncStatus.setBackgroundColor(Color.parseColor("#FDF1DC"));
            mTxtSafeSyncCardStatus.setText(R.string.lbl_safe_sync_card_exp);
            mTxtSafeSyncCardStatus.setTextColor(Color.parseColor("#9B4D18"));
        } else {
            mCardSafeSyncStatus.setBackgroundColor(Color.parseColor("#EBF5E6"));
            mTxtSafeSyncCardStatus.setText(R.string.lbl_safe_sync_card_active);
            mTxtSafeSyncCardStatus.setTextColor(Color.parseColor("#2E6120"));
        }
    }

    private void syncSettingsUiFromConfig() {
        mUpdatingUi = true;
        int scaleIdx = 0;
        for (int i = 0; i < SCALE_PRESET_MAP.length; i++) {
            if (SCALE_PRESET_MAP[i] == mConfig.renderScalePreset) { scaleIdx = i; break; }
        }
        if (mConfig.renderScalePreset == RuntimeConfigManager.SCALE_085) scaleIdx = 4;
        if (mConfig.renderScalePreset == RuntimeConfigManager.SCALE_125) scaleIdx = 5;
        mSpRenderScale.setSelection(scaleIdx);

        int shadowIdx = 0;
        for (int i = 0; i < SHADOW_PRESET_MAP.length; i++) {
            if (SHADOW_PRESET_MAP[i] == mConfig.shadowScaleMode) { shadowIdx = i; break; }
        }
        mSpShadowScale.setSelection(shadowIdx);

        int fpsIdx = 0;
        for (int i = 0; i < FPS_PRESET_MAP.length; i++) {
            if (FPS_PRESET_MAP[i] == mConfig.targetFps) { fpsIdx = i; break; }
        }
        mSpTargetFps.setSelection(fpsIdx);
        mSpMsaaMode.setSelection(Math.max(0, Math.min(2, mConfig.msaaMode)));
        mSwDockedMode.setChecked(mConfig.dockedMode);
        mSpSafeSync.setSelection(mConfig.poolSafeSyncMode == RuntimeConfigManager.SAFE_SYNC_PIPELINED ? 1 : 0);
        updateSafeSyncStatusCard();
        mSeekAudioVol.setProgress(mConfig.audioVolumePct);
        mTxtAudioVol.setText(getString(R.string.lbl_audio_vol_fmt, mConfig.audioVolumePct));
        mSwOverlay.setChecked(mConfig.overlayEnabled);
        if (mSwFlexAnalog != null) mSwFlexAnalog.setChecked(mConfig.flexibleAnalog);
        if (mSwAdaptiveBtn != null) mSwAdaptiveBtn.setChecked(mConfig.adaptiveButton);
        mSwRumble.setChecked(mConfig.rumbleEnabled);
        mSwFpsCounter.setChecked(mConfig.fpsCounter);

        mSwRobustAccess.setChecked(mConfig.robustAccess);
        mSwInlineResolve.setChecked(mConfig.inlineResolve);
        mSwDeferredWait.setChecked(mConfig.deferredWait);
        mSwStrictBarriers.setChecked(mConfig.strictBarriers);
        mSwBcCache.setChecked(mConfig.bcDiskCache);
        mSpLowRam.setSelection(Math.max(0, Math.min(3, mConfig.lowRamMode)));
        mSpLowCostPp.setSelection(Math.max(0, Math.min(2, mConfig.lowCostPpMode)));
        if (mSpShadowRate != null) mSpShadowRate.setSelection(Math.max(0, Math.min(3, mConfig.shadowRateMode)));
        if (mSpScreenMode != null) mSpScreenMode.setSelection(Math.max(0, Math.min(2, mConfig.screenMode)));
        if (mSpPadLayout != null) mSpPadLayout.setSelection(mConfig.padXboxLayout ? 1 : 0);
        if (mSpSamsungSafe != null) mSpSamsungSafe.setSelection(Math.max(0, Math.min(2, mConfig.samsungSafeMode)));
        if (mSwNativeSurfaceRes != null) mSwNativeSurfaceRes.setChecked(mConfig.nativeSurfaceRes);
        updatePresetHighlight();
        mUpdatingUi = false;
    }

    private void updatePresetHighlight() {
        int active = RuntimeConfigManager.detectQuickPreset(mConfig);
        stylePill(mBtnPresetAuto, active == RuntimeConfigManager.PRESET_AUTO);
        stylePill(mBtnPresetLow, active == RuntimeConfigManager.PRESET_LOW_END);
        stylePill(mBtnPresetTurbo, active == RuntimeConfigManager.PRESET_TURBO);
        stylePill(mBtnPresetBalanced, active == RuntimeConfigManager.PRESET_BALANCED);
        stylePill(mBtnPresetHigh, active == RuntimeConfigManager.PRESET_HIGH);
    }

    private void stylePill(TextView tv, boolean active) {
        if (tv == null) return;
        tv.setBackgroundResource(active ? R.drawable.bg_preset_active : R.drawable.bg_preset_idle);
        tv.setTextColor(active ? Color.parseColor("#FFFDF5") : Color.parseColor("#5C3919"));
    }

    private void setDotColor(int color) {
        if (mStatusDot == null) return;
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(color);
        mStatusDot.setBackground(d);
    }

    private void probeVulkanAsync() {
        new Thread(() -> {
            String json = NativeBridge.isLoaded()
                    ? NativeBridge.nativeProbeVulkan()
                    : "{\"available\":false,\"error\":\"libnxhybrid_extract.so not loaded\"}";
            mVulkanProbeJson = json;
            String formatted = buildShortGpuRamSummary(json);
            runOnUiThread(() -> {
                if (mTxtGpuRamInfo != null) mTxtGpuRamInfo.setText(formatted);
            });
        }, "vk-probe").start();
    }

    private String buildShortGpuRamSummary(String json) {
        StringBuilder sb = new StringBuilder();
        try {
            JSONObject j = new JSONObject(json);
            if (j.optBoolean("available", false)) {
                String gpuName = j.optString("gpu_name");
                String vendorId = j.optString("vendor_id");
                String devId = j.optString("device_id");
                RuntimeConfigManager.saveDetectedGpuProfile(this, gpuName, vendorId, devId);
                boolean bcFeat = j.optBoolean("bc_feature", false);
                boolean bc7 = j.optBoolean("bc7_sampled", false);
                sb.append("GPU : ").append(gpuName).append("\n");
                sb.append("API : Vulkan ").append(j.optString("vulkan_api"))
                        .append(" (Drv ").append(j.optString("driver_version")).append(")\n");
                sb.append("Tex : ").append((bcFeat && bc7) ? "Native BCn" : "CPU BC Decode + Cache").append("\n");
            } else {
                sb.append(getString(R.string.gpu_not_available)).append("\n");
            }
        } catch (Exception e) {
            sb.append(getString(R.string.gpu_probe_error)).append("\n");
        }
        ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(mi);
            sb.append(getString(R.string.ram_summary_fmt,
                    mi.totalMem / 1073741824.0, mi.availMem / 1073741824.0)).append("\n");
        }
        sb.append("APK : v").append(BuildConfig.VERSION_NAME).append(" (code ").append(BuildConfig.VERSION_CODE).append(")");
        return sb.toString();
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
                            getString(R.string.toast_core_sync_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                }
            }
            runOnUiThread(this::refreshInstallAndStorageStatus);
        }, "core-sync").start();
    }

    private void refreshInstallAndStorageStatus() {
        CoreAssetSync.Status st = CoreAssetSync.inspectStatus(this);
        boolean extracting = ExtractForegroundService.isRunning();
        mReadyToPlay = st.readyToPlay && !extracting;

        // Minimalist status pill & PLAY / IMPORT button styling on Main Screen
        if (extracting) {
            setDotColor(Color.parseColor("#DF7E15"));
            mTxtStatusShort.setText(R.string.status_importing);
            mTxtMainPlay.setText(R.string.btn_play_waiting_import);
            mBtnMainImport.setBackgroundResource(R.drawable.bg_btn_import_highlight);
        } else if (mReadyToPlay) {
            setDotColor(Color.parseColor("#4E8C3B"));
            String ver = st.displayVersion != null ? st.displayVersion : "1.1.2";
            mTxtStatusShort.setText(getString(R.string.status_ready_fmt, ver));
            mTxtMainPlay.setText(R.string.btn_play);
            mBtnMainImport.setBackgroundResource(R.drawable.bg_btn_parchment);
        } else {
            setDotColor(Color.parseColor("#DF7E15"));
            mTxtStatusShort.setText(R.string.status_not_imported);
            mTxtMainPlay.setText(R.string.btn_play_import_first);
            mBtnMainImport.setBackgroundResource(R.drawable.bg_btn_import_highlight);
        }

        // Import panel status & storage
        if (st.readyToPlay) {
            mTxtImportStatus.setText(getString(R.string.import_status_ready_fmt,
                    st.displayVersion != null ? st.displayVersion : "1.1.2",
                    st.romfsFiles,
                    st.aocSummary != null ? st.aocSummary : "0"));
            mTxtImportStatus.setTextColor(Color.parseColor("#2E6120"));
            mBtnStartExtract.setText(R.string.btn_reimport_add_dlc);
            mBtnReimportClean.setVisibility(extracting ? View.GONE : View.VISIBLE);
        } else if (st.gameExtracted) {
            mTxtImportStatus.setText(R.string.import_status_syncing);
            mTxtImportStatus.setTextColor(Color.parseColor("#8A4B08"));
            mBtnStartExtract.setText(R.string.btn_start_extract);
            mBtnReimportClean.setVisibility(View.VISIBLE);
        } else {
            mTxtImportStatus.setText(R.string.import_status_none);
            mTxtImportStatus.setTextColor(Color.parseColor("#6B421E"));
            mBtnStartExtract.setText(R.string.btn_start_extract);
            mBtnReimportClean.setVisibility(View.GONE);
        }

        long freeBytes = NativeBridge.isLoaded()
                ? NativeBridge.nativeQueryFreeSpace(getFilesDir().getAbsolutePath())
                : getFilesDir().getUsableSpace();
        double freeGib = freeBytes / 1073741824.0;
        mTxtImportStorage.setText(getString(R.string.import_storage_fmt, freeGib));
        mTxtImportStorage.setTextColor(freeGib >= 3.2 ? Color.parseColor("#3E702E") : Color.parseColor("#A6281A"));

        // Mod status inside Settings
        SaveAndLogManager.ModStatus mod = SaveAndLogManager.getRomfsModStatus(this);
        if (mod.active) {
            mTxtModStatus.setText(getString(R.string.mod_status_active_fmt, mod.modName, mod.totalFiles));
            mTxtModStatus.setTextColor(Color.parseColor("#2E6120"));
        } else {
            mTxtModStatus.setText(R.string.mod_status_original);
            mTxtModStatus.setTextColor(Color.parseColor("#6B421E"));
        }
        mBtnInstallMod.setEnabled(st.gameExtracted && !extracting);
        mBtnInstallMod.setAlpha((st.gameExtracted && !extracting) ? 1.0f : 0.45f);
        mBtnRestoreMod.setEnabled(st.gameExtracted && mod.active && !extracting);
        mBtnRestoreMod.setAlpha((st.gameExtracted && mod.active && !extracting) ? 1.0f : 0.45f);
    }

    private void updateExtractionUi(ExtractForegroundService.Snapshot snap) {
        boolean running = snap.running || ExtractForegroundService.isRunning();
        mBtnStartExtract.setEnabled(!running);
        mBtnStartExtract.setAlpha(running ? 0.55f : 1.0f);
        mBtnCancelExtract.setVisibility(running ? View.VISIBLE : View.GONE);
        if (running || !snap.message.isEmpty()) {
            mProgImport.setVisibility(running ? View.VISIBLE : View.GONE);
            mProgImport.setIndeterminate(running && snap.percent == 0);
            mProgImport.setProgress(snap.percent);
            mTxtImportProgress.setVisibility(View.VISIBLE);
            mTxtImportProgress.setText(running ? (snap.percent + "% — " + snap.message) : snap.message);
        }
        if (!running && snap.eventId != 0L && snap.eventId != mLastHandledExtractEventId) {
            mLastHandledExtractEventId = snap.eventId;
            if (!snap.success && !snap.cancelled && !snap.errorDetail.isEmpty()) {
                showExtractFailureDialog(snap.errorDetail);
            } else if (snap.success) {
                Toast.makeText(this, R.string.toast_extract_done, Toast.LENGTH_LONG).show();
            }
        }
        refreshInstallAndStorageStatus();
    }

    private void showExtractFailureDialog(String rawErr) {
        boolean missingUpd = rawErr.startsWith("UPDATE_112_MISSING:")
                || rawErr.contains("Update NSP mismatch")
                || rawErr.contains("Build ID mismatch");
        String detail = rawErr.startsWith("UPDATE_112_MISSING:")
                ? rawErr.substring("UPDATE_112_MISSING:".length()).trim()
                : rawErr;
        if (missingUpd) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.dlg_upd_missing_title)
                    .setMessage(getString(R.string.dlg_upd_missing_msg, detail))
                    .setPositiveButton(R.string.btn_understood, null)
                    .show();
        } else {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.dlg_import_fail_title)
                    .setMessage(getString(R.string.dlg_import_fail_msg, detail))
                    .setPositiveButton(R.string.btn_ok, null)
                    .show();
        }
    }

    private void onExtractClicked() {
        if (mKeysUri == null || mBaseUri == null) {
            Toast.makeText(this, R.string.toast_pick_required, Toast.LENGTH_LONG).show();
            return;
        }
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
        if (requestCode == REQ_POST_NOTIF) startExtractionService();
    }

    private void startExtractionService() {
        Intent i = new Intent(this, ExtractForegroundService.class);
        i.setAction(ExtractForegroundService.ACTION_START);
        i.putExtra(ExtractForegroundService.EXTRA_KEYS_URI, mKeysUri.toString());
        i.putExtra(ExtractForegroundService.EXTRA_BASE_URI, mBaseUri.toString());
        i.putExtra(ExtractForegroundService.EXTRA_UPDATE_URI, mUpdateUri != null ? mUpdateUri.toString() : "");
        ArrayList<String> dlcs = new ArrayList<>();
        for (Uri u : mDlcUris) dlcs.add(u.toString());
        i.putStringArrayListExtra(ExtractForegroundService.EXTRA_DLC_URIS, dlcs);
        SaveAndLogManager.appendLauncherLog(this, "ui",
                "Extract clicked: base=" + queryDisplayName(mBaseUri)
                        + " update=" + (mUpdateUri != null ? queryDisplayName(mUpdateUri) : "(none/auto)")
                        + " dlcs=" + mDlcUris.size());
        startForegroundService(i);
        mBtnStartExtract.setEnabled(false);
        mBtnStartExtract.setAlpha(0.55f);
        mBtnCancelExtract.setVisibility(View.VISIBLE);
        mProgImport.setVisibility(View.VISIBLE);
        mProgImport.setIndeterminate(true);
        mTxtImportProgress.setVisibility(View.VISIBLE);
        mTxtImportProgress.setText(R.string.msg_extract_starting);
    }

    private void confirmCleanGameData() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.dlg_clean_title)
                .setMessage(R.string.dlg_clean_msg)
                .setPositiveButton(R.string.btn_clean_confirm, (d, w) -> new Thread(() -> {
                    File gd = CoreAssetSync.getGameDataDir(this);
                    deleteSubdir(new File(gd, "RomFS"));
                    deleteSubdir(new File(gd, "ExeFS"));
                    deleteSubdir(new File(gd, "romfs_orig_backup"));
                    //noinspection ResultOfMethodCallIgnored
                    new File(gd, ".ready.json").delete();
                    //noinspection ResultOfMethodCallIgnored
                    new File(gd, ".core_assets.json").delete();
                    runOnUiThread(() -> {
                        Toast.makeText(this, R.string.toast_clean_done, Toast.LENGTH_SHORT).show();
                        refreshInstallAndStorageStatus();
                    });
                }, "clean-data").start())
                .setNegativeButton(R.string.btn_cancel, null)
                .show();
    }

    private void deleteSubdir(File f) {
        if (f == null || !f.exists()) return;
        File[] list = f.listFiles();
        if (list != null) {
            for (File c : list) deleteSubdir(c);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private void checkGpuDeviceLostRecovery() {
        if (mDeviceLostPrompted) return;
        File gameDir = CoreAssetSync.getGameDataDir(this);
        File marker = new File(gameDir, ".gpu_device_lost");
        if (!marker.isFile()) return;
        mDeviceLostPrompted = true;
        String detail = "";
        File errFile = new File(gameDir, "last_launch_error.txt");
        if (errFile.isFile()) {
            try { detail = CoreAssetSync.readFileUtf8(errFile).trim(); } catch (Exception ignored) {}
        }
        String extraDetail = detail.isEmpty() ? "" : (detail + "\n\n");
        new AlertDialog.Builder(this)
                .setTitle(R.string.dlg_gpu_lost_title)
                .setMessage(getString(R.string.dlg_gpu_lost_msg, extraDetail))
                .setPositiveButton(R.string.btn_enable_safe_mode, (d, w) -> {
                    mConfig.poolSafeSyncMode = RuntimeConfigManager.SAFE_SYNC_SERIAL_DBG;
                    mConfig.msaaMode = RuntimeConfigManager.MSAA_DISABLE;
                    mConfig.robustAccess = true;
                    syncSettingsUiFromConfig();
                    persistConfig();
                    //noinspection ResultOfMethodCallIgnored
                    marker.delete();
                    Toast.makeText(this, R.string.toast_safe_mode_on, Toast.LENGTH_LONG).show();
                })
                .setNegativeButton(R.string.btn_ignore, (d, w) -> {
                    //noinspection ResultOfMethodCallIgnored
                    marker.delete();
                })
                .show();
    }

    private void launchGame() {
        CoreAssetSync.Status st = CoreAssetSync.inspectStatus(this);
        if (!st.readyToPlay) {
            Toast.makeText(this, R.string.toast_game_not_verified, Toast.LENGTH_SHORT).show();
            refreshInstallAndStorageStatus();
            return;
        }
        try {
            RuntimeConfigManager.save(this, mConfig);
        } catch (Exception e) {
            Toast.makeText(this, getString(R.string.toast_config_save_warn, e.getMessage()), Toast.LENGTH_SHORT).show();
        }
        startActivity(new Intent(this, GameActivity.class));
    }

    private void persistConfig() {
        try {
            RuntimeConfigManager.save(this, mConfig);
        } catch (Exception e) {
            Toast.makeText(this, getString(R.string.toast_config_save_fail, e.getMessage()), Toast.LENGTH_SHORT).show();
        }
    }

    private void openDocumentPicker(int reqCode, boolean allowMultiple) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        if (allowMultiple) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
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
            String nm = queryDisplayName(mKeysUri);
            mLblPickKeys.setText(getString(R.string.picked_fmt, nm));
            mLblPickKeys.setTextColor(Color.parseColor("#2E6120"));
            mChkKeys.setAlpha(1.0f);
            SaveAndLogManager.appendLauncherLog(this, "picker", "prod.keys selected (" + nm + ")");
        } else if (requestCode == REQ_PICK_BASE && data.getData() != null) {
            mBaseUri = data.getData();
            String nm = queryDisplayName(mBaseUri);
            mLblPickBase.setText(getString(R.string.picked_fmt, nm));
            mLblPickBase.setTextColor(Color.parseColor("#2E6120"));
            mChkBase.setAlpha(1.0f);
            SaveAndLogManager.appendLauncherLog(this, "picker", "Base Game selected (" + nm + ")");
        } else if (requestCode == REQ_PICK_UPDATE && data.getData() != null) {
            mUpdateUri = data.getData();
            String nm = queryDisplayName(mUpdateUri);
            mLblPickUpdate.setText(getString(R.string.picked_fmt, nm));
            mLblPickUpdate.setTextColor(Color.parseColor("#2E6120"));
            mChkUpdate.setAlpha(1.0f);
            SaveAndLogManager.appendLauncherLog(this, "picker", "Update selected (" + nm + ")");
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
            if (mDlcUris.isEmpty()) {
                mLblPickDlcs.setText(R.string.picked_dlc_none);
                mLblPickDlcs.setTextColor(Color.parseColor("#8A6A4A"));
                mChkDlcs.setAlpha(0.22f);
            } else {
                mLblPickDlcs.setText(getString(R.string.picked_dlc_count_fmt, mDlcUris.size()));
                mLblPickDlcs.setTextColor(Color.parseColor("#2E6120"));
                mChkDlcs.setAlpha(1.0f);
            }
            SaveAndLogManager.appendLauncherLog(this, "picker", "DLC selected count=" + mDlcUris.size());
        } else if (requestCode == REQ_EXPORT_LOG && data.getData() != null) {
            final Uri dest = data.getData();
            new Thread(() -> {
                try {
                    int n = SaveAndLogManager.exportRuntimeLogs(this, dest, mVulkanProbeJson);
                    runOnUiThread(() -> Toast.makeText(this,
                            getString(R.string.toast_log_exported, n / 1024), Toast.LENGTH_LONG).show());
                } catch (Exception e) {
                    runOnUiThread(() -> Toast.makeText(this,
                            getString(R.string.toast_log_export_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                }
            }).start();
        } else if (requestCode == REQ_BACKUP_SAVE && data.getData() != null) {
            final Uri dest = data.getData();
            new Thread(() -> {
                try {
                    int count = SaveAndLogManager.exportSaveZip(this, dest);
                    runOnUiThread(() -> Toast.makeText(this,
                            getString(R.string.toast_save_backup_ok, count), Toast.LENGTH_LONG).show());
                } catch (Exception e) {
                    runOnUiThread(() -> Toast.makeText(this,
                            getString(R.string.toast_save_backup_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                }
            }).start();
        } else if (requestCode == REQ_RESTORE_SAVE && data.getData() != null) {
            final Uri src = data.getData();
            new AlertDialog.Builder(this)
                    .setTitle(R.string.dlg_restore_save_title)
                    .setMessage(R.string.dlg_restore_save_msg)
                    .setPositiveButton(R.string.btn_restore, (d, w) -> new Thread(() -> {
                        try {
                            int count = SaveAndLogManager.importSaveZip(this, src);
                            runOnUiThread(() -> Toast.makeText(this,
                                    getString(R.string.toast_save_restore_ok, count), Toast.LENGTH_LONG).show());
                        } catch (Exception e) {
                            runOnUiThread(() -> Toast.makeText(this,
                                    getString(R.string.toast_save_restore_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                        }
                    }).start())
                    .setNegativeButton(R.string.btn_cancel, null)
                    .show();
        } else if (requestCode == REQ_INSTALL_MOD && data.getData() != null) {
            final Uri modUri = data.getData();
            final String modName = queryDisplayName(modUri);
            new AlertDialog.Builder(this)
                    .setTitle(R.string.dlg_install_mod_title)
                    .setMessage(getString(R.string.dlg_install_mod_msg, modName))
                    .setPositiveButton(R.string.btn_install_mod_confirm, (d, w) -> new Thread(() -> {
                        try {
                            SaveAndLogManager.ModStatus res = SaveAndLogManager.installRomfsModZip(this, modUri, modName);
                            runOnUiThread(() -> {
                                Toast.makeText(this, getString(R.string.toast_mod_installed, res.modName, res.totalFiles), Toast.LENGTH_LONG).show();
                                refreshInstallAndStorageStatus();
                            });
                        } catch (Exception e) {
                            runOnUiThread(() -> Toast.makeText(this, getString(R.string.toast_mod_install_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                        }
                    }, "mod-install").start())
                    .setNegativeButton(R.string.btn_cancel, null)
                    .show();
        } else if (requestCode == REQ_IMPORT_CHEAT && data.getData() != null) {
            final Uri src = data.getData();
            new Thread(() -> {
                try {
                    CheatManager.ImportResult res = CheatManager.importCheatFile(this, src);
                    runOnUiThread(() -> {
                        refreshCheatsUi();
                        String msg = res.buildIdMismatch
                                ? getString(R.string.toast_cheat_imported_mismatch, res.fileName, res.cheatCount)
                                : getString(R.string.toast_cheat_imported, res.fileName, res.cheatCount);
                        Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> Toast.makeText(this,
                            getString(R.string.toast_cheat_import_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                }
            }, "import-cheat").start();
        }
    }

    private void confirmRestoreOriginalRomfs() {
        new AlertDialog.Builder(this)
                .setTitle(R.string.dlg_restore_romfs_title)
                .setMessage(R.string.dlg_restore_romfs_msg)
                .setPositiveButton(R.string.btn_restore, (d, w) -> new Thread(() -> {
                    try {
                        int n = SaveAndLogManager.restoreOriginalRomfs(this);
                        runOnUiThread(() -> {
                            Toast.makeText(this, getString(R.string.toast_romfs_restored, n), Toast.LENGTH_LONG).show();
                            refreshInstallAndStorageStatus();
                        });
                    } catch (Exception e) {
                        runOnUiThread(() -> Toast.makeText(this, getString(R.string.toast_romfs_restore_fail, e.getMessage()), Toast.LENGTH_LONG).show());
                    }
                }, "mod-restore").start())
                .setNegativeButton(R.string.btn_cancel, null)
                .show();
    }

    private String queryDisplayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String name = c.getString(0);
                if (name != null && !name.isEmpty()) return name;
            }
        } catch (Exception ignored) {
        }
        return uri.getLastPathSegment() != null ? uri.getLastPathSegment() : "file";
    }

    private String timestampSlug() {
        return new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(new Date());
    }
}
