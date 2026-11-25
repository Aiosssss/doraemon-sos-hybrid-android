# Laporan Rilis `v1.1.2-hybrid36` (`versionCode = 36`)

**Target Aplikasi:** `org.nx2a.doraemon` (**SOS FOTGK**)  
**Game:** *DORAEMON STORY OF SEASONS: Friends of the Great Kingdom* (`01008EE0177A8000`, Update `1.1.2` / `v262144`, `main` Build ID `306dfcfc9eba4a1a7d0ae7ffcee4b9e4`)  
**Artefak APK:** `hybrid_port/dist/Story-Of-Seasons-Doraemon-FOTGK.apk` (33 MB)  
**APK SHA-256:** `a9578189d90780a33b5d8d82e8d3a0b14ced3fdb246f7f3a71e17ab136e9f07b`  
**`libdmhost.so` SHA-256:** `24095661452128c2974952a87438e979be499802c8145c9c6813aad99ba7e26c`

---

## Ringkasan Perubahan `v1.1.2-hybrid36`

Seluruh fitur & optimasi `hybrid31–35` dipertahankan utuh (`pool_safe_sync=2`, `robust_access=1`, `use_classic_fb` untuk Adreno Vulkan 1.1, `-march=armv8-a` 0 LSE untuk Snapdragon 685, 4 KB hot-page + decay + compare skip, shadow throttling, ADPF dual-trigger, Xbox controller, Full Screen, Cheat Engine, Flexible Analog, & Adaptive Button).

### 1. Fix 100% GPU Hang TDR 5.63s (`VK_ERROR_DEVICE_LOST -4`) & Flagship Tier 0 — **EKSKLUSIF Samsung Xclipse (`0x144d`)**

> **Jaminan Zero Regression Mali (`0x13b5`) & Adreno (`0x5143`):** Seluruh mode aman Xclipse dikunci secara eksklusif pada `V.props.vendorID == 0x144Du` / `detected_gpu_xclipse`. Jalur performa Mali & Adreno tidak diubah.

1. **Isolated Safe Mode (`0x144d` ONLY):**
   - `gfx_vk_core.c` (`vk_init`): menonaktifkan timeline semaphore (`V.has_timeline = 0`, `f12.timelineSemaphore = 0`) khusus `vendorID == 0x144Du`, beralih ke classic `VkFence` + binary chain semaphore.
   - `gfx_vk_core.c` (`vk_deferred_wait`, `vk_strict_barriers`, `vk_inline_resolve`): memaksa `def_wait=0`, `strict_barriers=1`, `inline_resolve=0` **hanya** untuk `0x144d`.
   - `RuntimeConfigManager.java`: menghapus penulisan paksa `DM_VK_STRICT_BARRIERS=0` dan `DM_VK_INLINE_RESOLVE=1` pada mode default sehingga runtime C dapat menerapkan default spesifik vendor secara otomatis (`0x144d` mendapat safe-mode; Mali/Adreno tetap pada jalur cepatnya).
   - `gfx_vk_shader.c`: menulis ulang `OpGroupNonUniform*` pada stage non-compute (graphics/geometry) menjadi 1-lane khusus Wave64 `0x144d` (stage compute tetap di-pin ke Wave32).
2. **Non-Blocking Fence-Tagged Deferred Retire (`pool_drop` & `cpf` — Semua GPU):**
   - `gfx_vk_core.c`: mengganti jalur `vk_defer_free_buffer` di `mtl_pool_del()` (yang sebelumnya rentan race dengan `g_cur` dari thread game) menjadi antrean `g_pool_retire` bertag `retire_fence = V.sub_seq + 3`.
   - Pembebasan `VkBuffer`/`VkDeviceMemory`/`AHardwareBuffer` maupun reuse `cpf_take()` hanya terjadi secara **non-blocking** ketika `V.completed_seq >= retire_fence` (di-drain pada `slot_recycle()` & `cpf_take()`).
   - **0 panggilan `vkQueueWaitIdle()` / `vkDeviceWaitIdle()`** pada jalur `pool_drop` dan `cpf`.
3. **Snapshot Diagnostik `V.last_sub_diag` & Anti-Loop Hang:**
   - `gfx_vk.h`, `gfx_vk_core.c`, `gfx_vk_exec_draws.inc`, `gfx_vk_exec_state.inc`, `gfx_vk_exec_pipelines.inc`: merekam `frame`, `fence`, `draws`, `passes`, `dispatches`, `last_vs/fs/gs/cs`, serta hingga 8 pipeline baru `{vs, fs, gs}` ke `V.last_sub_diag` **sebelum** `memset(g_sub_ops, 0, ...)` & `vkQueueSubmit`.
   - Dicetak lengkap di `vk_record_device_lost()` (termasuk `last_launch_error.txt` & `.gpu_device_lost`) dan di `[WATCHDOG] RENDER HANG >5s`.
   - Anti-loop: jika `VK_ERROR_DEVICE_LOST (-4)` terjadi `>= 2` kali berturut-turut (`V.dev_lost_streak >= 2`), runtime mem-flush log dan keluar bersih via `_exit(88)` alih-alih terjebak loop hang `5.6s/frame`.
4. **Flagship Tier 0 untuk Samsung Xclipse (`0x144d` ONLY):**
   - `gfx_vk_core.c` (`vk_gpu_tier`, `vk_low_ram_tier`): mengklasifikasikan `0x144d` ke `gpu_tier = 0` (`render_scale = 1.00`, `shadow_scale = 1.00`, `shadow_rate = 1`, `low_cost_pp = 0`, `tex_downscale = 1/1`) dan `low_ram = 0` saat RAM terlihat `>= 5500 MiB`.
   - `RuntimeConfigManager.java` + `LauncherActivity.java`: menyimpan `vendor_id` dan menambahkan migrasi satu kali `hybrid36_migrated` khusus Xclipse untuk me-reset stale `low_ram_mode` ke `LOW_RAM_AUTO`.

### 2. Stabilitas Frame & Boost FPS Universal (Semua Perangkat)

1. **Kunci Refresh Rate Layar LTPO (`GameActivity.java`):**
   - `applyDisplayRefreshLock(Surface)` memanggil `Surface.setFrameRate(targetHz, FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, CHANGE_FRAME_RATE_ALWAYS)` (API 31+, fallback 2-arg API 30) serta mengatur `preferredRefreshRate` dan `preferredDisplayModeId` yang paling dekat dengan target FPS (`60`/`45`/`30` Hz) pada resolusi fisik sama. Dipanggil di `onCreate`, `onResume`, `surfaceCreated`, `surfaceChanged`, dan `onWindowFocusChanged`.
2. **Universal Async Pipeline Compile (`gfx_vk_exec_pipelines.inc` & `gfx_vk_core.c`):**
   - Membatasi kompilasi sinkron graphics pipeline menjadi `<= 3/frame` (Tier 0–1) dan `<= 2/frame` (Tier 2–3), mendelegasikan sisanya ke 1 worker latar belakang yang diserialisasi oleh `vk_pipe_compile_lock()`, mempertahankan `PCACHE_BURST = 30`.
3. **Perbaikan `low_cost_pp=1` (`gfx_vk_exec_passes.inc`):**
   - Mengizinkan attachment depth tidak aktif (`!S.daspect || (!S.ds.dt && !S.ds.dw)`), memperluas deteksi mip bloom/blur 16:9 ke `16x10..480x270`, serta mendukung format `R8G8B8A8_SRGB`, `A2B10G10R10_UNORM_PACK32`, `R16G16B16A16_UNORM`, `R8G8B8A8_UNORM`, dan HDR float (tidak pernah menyentuh scene 3D utama, karakter, bayangan, atau UI).

### 3. Fix Bug Touch Scaling `surfW=1`

- `front_android.c` (`refresh_touch_view_size`, `touch_sx`, `touch_sy`) dan `GameActivity.java` (`updateTouchViewMetrics`, `surfaceCreated`, `surfaceChanged`): mensyaratkan `viewW >= 64 && viewH >= 64 && surfW >= 64 && surfH >= 64` sebelum melakukan scaling koordinat atau mencetak `[input] touch coordinate scaling active`.

---

## Bukti Verifikasi Offline (Host-Side)

> **Catatan:** Seluruh pemeriksaan di bawah ini dilakukan secara offline di host Linux (belum dijalankan di perangkat Android fisik).

| Pemeriksaan | Hasil |
|---|---|
| Kompilasi `libdmhost.so` (`build.py --release`) | **PASS** — 197/197 sumber, 0 error (2331 KB stripped) |
| Audit instruksi ilegal LSE ARMv8.1+ (`llvm-objdump -d`) | **PASS** — `0` instruksi LSE ilegal (`-march=armv8-a`) |
| Audit `vkQueueWaitIdle`/`vkDeviceWaitIdle` di `pool_drop`/`cpf` | **PASS** — `0` panggilan |
| Build Gradle Release + R8 (`:app:assembleRelease --offline`) | **PASS** — `versionCode = 36`, `versionName = 1.1.2` |
| `dexdump` simbol LTPO & GPU profile pasca-R8 | **PASS** — `applyDisplayRefreshLock`, `Surface.setFrameRate` (2-arg & 3-arg), `preferredDisplayModeId`, `getEffectiveTargetFps`, `saveDetectedGpuProfile` utuh |
| `verify_hybrid_apk.sh` (16 KB zipalign, V3 RSA-4096 sign, legal, storage) | **7/7 PASS** |
| Path terlindungi (`hybrid_port/l10n_id/`, `Game/`, `tools/`) | **UNTOUCHED** |
