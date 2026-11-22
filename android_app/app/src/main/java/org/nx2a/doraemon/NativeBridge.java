package org.nx2a.doraemon;

/**
 * JNI bridge to libnxhybrid_extract.so (pure C11 streaming NSP/NCA/BKTR extractor,
 * streaming NXPT binary patch applier, storage query, and Vulkan GPU capability probe).
 *
 * FD Ownership Contract (Android fdsan-safe):
 *   nativeExtractGame() ONLY invokes pread() on keysFd, baseFd, updateFd, and dlcFds.
 *   Native code NEVER calls close() on any caller-supplied fd.
 *   The Java caller MUST detach and close all fds in a finally block.
 */
public final class NativeBridge {
    private static volatile boolean sLoaded = false;
    private static volatile String sLoadError = null;

    static {
        try {
            System.loadLibrary("nxhybrid_extract");
            sLoaded = true;
        } catch (Throwable t) {
            sLoaded = false;
            sLoadError = t.getMessage();
        }
    }

    private NativeBridge() {}

    public static boolean isLoaded() {
        return sLoaded;
    }

    public static String getLoadError() {
        return sLoadError;
    }

    /**
     * Callback invoked synchronously from the native extraction thread.
     * Signature MUST remain (Ljava/lang/String;JJJJ)V to match nx_jni.c.
     */
    public interface ProgressCallback {
        void onProgress(String stage, long filesDone, long filesTotal, long bytesDone, long bytesTotal);
    }

    /**
     * Runs the full atomic extraction + NXPT binary patch pipeline.
     *
     * @param keysFd       Raw POSIX fd for prod.keys (caller retains close responsibility)
     * @param baseFd       Raw POSIX fd for Base NSP
     * @param updateFd     Raw POSIX fd for Update 1.1.2 NSP
     * @param dlcFds       Array of 0..32 raw POSIX fds for DLC (AOC) NSPs
     * @param patchesDir   Directory containing {rtld,main,subsdk0,sdk}.{nxpatch,fixups.tsv,gates.tsv}
     * @param destRoot     Parent directory (context.getFilesDir()); writes extract_tmp/ then swaps to game_data/
     * @param minFreeBytes Minimum free bytes required before starting extraction
     * @param progressCb   Optional progress callback
     * @return Empty string "" on success, or human-readable error message on failure/cancellation
     */
    public static native String nativeExtractGame(
            int keysFd,
            int baseFd,
            int updateFd,
            int[] dlcFds,
            String patchesDir,
            String destRoot,
            long minFreeBytes,
            ProgressCallback progressCb
    );

    /** Sets the native atomic cancellation flag to abort an active extraction cleanly. */
    public static native void nativeCancelExtract();

    /** Queries available free storage bytes on the filesystem containing path via statvfs(). */
    public static native long nativeQueryFreeSpace(String path);

    /**
     * Probes the device's Vulkan physical device capabilities (GPU name, vendorID, API version,
     * BC7 sampled image support, ASTC LDR support, and unified Mali vs Adreno pipeline selection).
     *
     * @return JSON string describing GPU capabilities
     */
    public static native String nativeProbeVulkan();
}
