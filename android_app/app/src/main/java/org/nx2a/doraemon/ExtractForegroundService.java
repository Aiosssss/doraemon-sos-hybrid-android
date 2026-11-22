package org.nx2a.doraemon;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.Uri;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Android 14/15 compliant Foreground Service (foregroundServiceType="dataSync")
 * executing streaming C11 NSP/NCA/BKTR extraction + NXPT binary patching +
 * atomic core asset synchronization without risking background process kills.
 *
 * FD Ownership (Android fdsan-safe):
 *   Every SAF URI is opened via ContentResolver.openFileDescriptor(uri, "r") -> detachFd().
 *   Native libnxhybrid_extract.so ONLY invokes pread() and NEVER closes caller fds.
 *   All detached fds are unconditionally closed once via ParcelFileDescriptor.adoptFd(fd).close()
 *   in the finally block across all exit paths (success, failure, exception, cancel).
 */
public final class ExtractForegroundService extends Service {
    private static final String TAG = "ExtractService";

    public static final String ACTION_START = "org.nx2a.doraemon.action.EXTRACT_START";
    public static final String ACTION_CANCEL = "org.nx2a.doraemon.action.EXTRACT_CANCEL";
    public static final String ACTION_STATE = "org.nx2a.doraemon.action.EXTRACT_STATE";

    public static final String EXTRA_KEYS_URI = "keys_uri";
    public static final String EXTRA_BASE_URI = "base_uri";
    public static final String EXTRA_UPDATE_URI = "update_uri";
    public static final String EXTRA_DLC_URIS = "dlc_uris";

    private static final String CHANNEL_ID = "doraemon_extract_channel";
    private static final int NOTIF_ID = 1001;
    private static final long MIN_FREE_BYTES = 3221225472L; // 3.0 GiB

    public static final class Snapshot {
        public final boolean running;
        public final boolean success;
        public final boolean cancelled;
        public final String stage;
        public final long filesDone;
        public final long filesTotal;
        public final long bytesDone;
        public final long bytesTotal;
        public final int percent;
        public final String message;

        public Snapshot(boolean running, boolean success, boolean cancelled, String stage,
                        long filesDone, long filesTotal, long bytesDone, long bytesTotal,
                        int percent, String message) {
            this.running = running;
            this.success = success;
            this.cancelled = cancelled;
            this.stage = stage;
            this.filesDone = filesDone;
            this.filesTotal = filesTotal;
            this.bytesDone = bytesDone;
            this.bytesTotal = bytesTotal;
            this.percent = percent;
            this.message = message;
        }
    }

    private static final AtomicBoolean sRunning = new AtomicBoolean(false);
    private static volatile Snapshot sSnapshot = new Snapshot(false, false, false, "idle", 0, 0, 0, 0, 0, "");

    private NotificationManager mNotifMgr;
    private long mLastNotifUpdateMs = 0L;

    public static boolean isRunning() {
        return sRunning.get();
    }

    public static Snapshot getSnapshot() {
        return sSnapshot;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mNotifMgr = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        ensureNotificationChannel();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent == null) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }
        String action = intent.getAction();
        if (ACTION_CANCEL.equals(action)) {
            if (sRunning.get() && NativeBridge.isLoaded()) {
                NativeBridge.nativeCancelExtract();
                publishState(new Snapshot(true, false, true, "cancelling",
                        sSnapshot.filesDone, sSnapshot.filesTotal,
                        sSnapshot.bytesDone, sSnapshot.bytesTotal,
                        sSnapshot.percent, "Cancelling extraction & cleaning temporary files..."), true);
            }
            return START_NOT_STICKY;
        }

        if (!ACTION_START.equals(action)) {
            stopSelf(startId);
            return START_NOT_STICKY;
        }

        if (!sRunning.compareAndSet(false, true)) {
            Log.w(TAG, "Extraction already running; ignoring duplicate start");
            return START_NOT_STICKY;
        }

        final String keysStr = intent.getStringExtra(EXTRA_KEYS_URI);
        final String baseStr = intent.getStringExtra(EXTRA_BASE_URI);
        final String updStr = intent.getStringExtra(EXTRA_UPDATE_URI);
        final ArrayList<String> dlcStrs = intent.getStringArrayListExtra(EXTRA_DLC_URIS);

        Notification initialNotif = buildNotification("Preparing extraction...", 0, true);
        startForeground(NOTIF_ID, initialNotif, ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);

        publishState(new Snapshot(true, false, false, "starting", 0, 1, 0, 1, 0,
                "Preparing NXPT recipes and validating files..."), true);

        Thread worker = new Thread(() -> runExtraction(keysStr, baseStr, updStr, dlcStrs, startId),
                "nx-extract-worker");
        worker.start();
        return START_NOT_STICKY;
    }

    private void runExtraction(String keysStr, String baseStr, String updStr,
                               ArrayList<String> dlcStrs, int startId) {
        PowerManager.WakeLock wakeLock = null;
        final List<Integer> ownedFds = new ArrayList<>();
        try {
            PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
            if (pm != null) {
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nx2a:doraemon_extract");
                wakeLock.acquire(45L * 60L * 1000L);
            }

            if (!NativeBridge.isLoaded()) {
                throw new IOException("Native extractor library unavailable: " + NativeBridge.getLoadError());
            }
            if (keysStr == null || baseStr == null || updStr == null) {
                throw new IOException("Missing required SAF URI (prod.keys, Base NSP, or Update 1.1.2 NSP)");
            }

            // Capture existing user config & custom in-game layout lines before atomic swap
            RuntimeConfigManager.Config savedCfg = RuntimeConfigManager.load(this);
            List<String> preservedLayoutLines = RuntimeConfigManager.capturePreservedSettingsLines(this);

            // Stage bundled 12 NXPT recipe files from APK assets into cacheDir/nxpatches/
            publishState(new Snapshot(true, false, false, "prepare_patches", 0, 1, 0, 1, 2,
                    "Staging deterministic NXPT recipes..."), true);
            File patchesDir = CoreAssetSync.preparePatchesDir(this);

            // Open all SAF URIs with fdsan-safe detachFd() tracking
            int keysFd = openDetachedReadFd(Uri.parse(keysStr), ownedFds);
            int baseFd = openDetachedReadFd(Uri.parse(baseStr), ownedFds);
            int updFd = openDetachedReadFd(Uri.parse(updStr), ownedFds);

            int dlcCount = (dlcStrs != null) ? Math.min(dlcStrs.size(), 32) : 0;
            int[] dlcFds = new int[dlcCount];
            for (int i = 0; i < dlcCount; i++) {
                dlcFds[i] = openDetachedReadFd(Uri.parse(dlcStrs.get(i)), ownedFds);
            }

            // Invoke native streaming extractor (writes filesDir/extract_tmp -> atomic swap to filesDir/game_data)
            String destRoot = getFilesDir().getAbsolutePath();
            String err = NativeBridge.nativeExtractGame(
                    keysFd, baseFd, updFd, dlcFds,
                    patchesDir.getAbsolutePath(),
                    destRoot,
                    MIN_FREE_BYTES,
                    (stage, fDone, fTotal, bDone, bTotal) -> {
                        int pct = computePercent(stage, fDone, fTotal, bDone, bTotal);
                        String msg = formatStageMessage(stage, fDone, fTotal, bDone, bTotal);
                        publishState(new Snapshot(true, false, false, stage,
                                fDone, fTotal, bDone, bTotal, pct, msg), false);
                    }
            );

            if (err != null && !err.isEmpty()) {
                boolean wasCancelled = err.toLowerCase(Locale.US).contains("cancel");
                publishState(new Snapshot(false, false, wasCancelled, wasCancelled ? "cancelled" : "error",
                        0, 0, 0, 0, 0,
                        wasCancelled ? "Extraction cancelled safely. Previous valid data preserved."
                                : ("Extraction failed: " + err)), true);
                return;
            }

            // Atomically sync bundled shaders.dmsv + .core_assets.json and restore user settings
            publishState(new Snapshot(true, false, false, "sync_core", 0, 1, 0, 1, 98,
                    "Syncing pre-translated Vulkan shaders (5,613 SPIR-V) & runtime config..."), true);
            CoreAssetSync.syncCoreAssets(this, true);
            RuntimeConfigManager.applyToGameData(this, savedCfg, preservedLayoutLines);

            publishState(new Snapshot(false, true, false, "done", 1, 1, 1, 1, 100,
                    "Extraction & verification complete! Ready to Play."), true);
        } catch (Throwable t) {
            Log.e(TAG, "Extraction failed", t);
            publishState(new Snapshot(false, false, false, "error", 0, 0, 0, 0, 0,
                    "Extraction error: " + (t.getMessage() != null ? t.getMessage() : t.getClass().getSimpleName())),
                    true);
        } finally {
            // fdsan-safe guaranteed single-close of every detached POSIX fd across ALL exit paths
            for (Integer fdObj : ownedFds) {
                if (fdObj != null && fdObj >= 0) {
                    try (ParcelFileDescriptor adopted = ParcelFileDescriptor.adoptFd(fdObj)) {
                        // try-with-resources invokes ParcelFileDescriptor.close() exactly once
                    } catch (Throwable closeErr) {
                        Log.w(TAG, "Error closing adopted fd " + fdObj + ": " + closeErr.getMessage());
                    }
                }
            }
            ownedFds.clear();

            if (wakeLock != null && wakeLock.isHeld()) {
                try {
                    wakeLock.release();
                } catch (Throwable ignored) {
                }
            }

            sRunning.set(false);
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf(startId);
        }
    }

    private int openDetachedReadFd(Uri uri, List<Integer> ownedFds) throws IOException {
        ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
        if (pfd == null) {
            throw new IOException("Cannot open SAF file descriptor for " + uri);
        }
        int fd = pfd.detachFd();
        if (fd < 0) {
            throw new IOException("Invalid POSIX fd returned for " + uri);
        }
        ownedFds.add(fd);
        return fd;
    }

    private int computePercent(String stage, long fDone, long fTotal, long bDone, long bTotal) {
        if (stage == null) return 0;
        switch (stage) {
            case "validate_keys": return 3;
            case "validate_nsp": return 5;
            case "exefs": return 7;
            case "patch_nso":
                return 8 + (fTotal > 0 ? (int) ((fDone * 7) / fTotal) : 0); // 8..15%
            case "romfs":
                if (bTotal > 0) return 15 + (int) Math.min(78, (bDone * 78) / bTotal);
                if (fTotal > 0) return 15 + (int) Math.min(78, (fDone * 78) / fTotal);
                return 15;
            case "aoc":
                return 93 + (fTotal > 0 ? (int) ((fDone * 3) / fTotal) : 0); // 93..96%
            case "commit": return 97;
            case "sync_core": return 98;
            case "done": return 100;
            default: return 0;
        }
    }

    private String formatStageMessage(String stage, long fDone, long fTotal, long bDone, long bTotal) {
        if (stage == null) return "Working...";
        switch (stage) {
            case "validate_keys": return "Validating prod.keys cryptographic keys...";
            case "validate_nsp": return "Validating Base, Update 1.1.2 & DLC CNMT headers...";
            case "exefs": return "Extracting ExeFS v1.1.2 modules & verifying Build IDs...";
            case "patch_nso": return String.format(Locale.US, "Applying NXPT patches (%d / %d modules)...", fDone, fTotal);
            case "romfs":
                return String.format(Locale.US, "Extracting RomFS (%d / %d files • %.1f / %.1f MiB)",
                        fDone, fTotal, bDone / 1048576.0, bTotal / 1048576.0);
            case "aoc": return String.format(Locale.US, "Extracting DLC AOC (%d / %d)...", fDone, fTotal);
            case "commit": return "Committing atomic installation (.ready.json)...";
            case "done": return "Native extraction finished.";
            default: return stage;
        }
    }

    private void publishState(Snapshot snap, boolean forceNotif) {
        sSnapshot = snap;
        long now = System.currentTimeMillis();
        if (forceNotif || (now - mLastNotifUpdateMs) >= 250L) {
            mLastNotifUpdateMs = now;
            if (snap.running && mNotifMgr != null) {
                try {
                    mNotifMgr.notify(NOTIF_ID, buildNotification(snap.message, snap.percent, snap.percent == 0));
                } catch (SecurityException ignored) {
                    // Safe if POST_NOTIFICATIONS was denied on Android 13+; extraction continues uninterrupted
                }
            }
            Intent bc = new Intent(ACTION_STATE);
            bc.setPackage(getPackageName());
            sendBroadcast(bc);
        }
    }

    private void ensureNotificationChannel() {
        if (mNotifMgr == null) return;
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID,
                getString(R.string.notif_channel_extract),
                NotificationManager.IMPORTANCE_LOW
        );
        ch.setDescription(getString(R.string.notif_channel_extract_desc));
        mNotifMgr.createNotificationChannel(ch);
    }

    private Notification buildNotification(String text, int percent, boolean indeterminate) {
        Intent openIntent = new Intent(this, LauncherActivity.class);
        openIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent contentPi = PendingIntent.getActivity(
                this, 0, openIntent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        Intent cancelIntent = new Intent(this, ExtractForegroundService.class);
        cancelIntent.setAction(ACTION_CANCEL);
        PendingIntent cancelPi = PendingIntent.getService(
                this, 1, cancelIntent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        return new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle(getString(R.string.app_name))
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_download)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setProgress(100, Math.max(0, Math.min(100, percent)), indeterminate)
                .setContentIntent(contentPi)
                .addAction(new Notification.Action.Builder(
                        null, "Cancel", cancelPi).build())
                .build();
    }
}
