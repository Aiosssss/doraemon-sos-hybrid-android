# Laporan Teknis `v1.1.2-hybrid24` (`versionCode = 24`)

Target utama: Menuntaskan akar masalah **kedip merah-oranye 3D** yang terbukti masih muncul bahkan di `pool_safe_sync=2` serial pada `v1.1.2-hybrid23` (`robust_access=0`), memperbaiki konflik palsu `conf=9..36` di `pool_safe_sync=1`, mengaudit & mengeraskan 3 perubahan `hybrid22`, sekaligus mendongkrak FPS dan menghilangkan stutter berat di flagship Mali-G720 (target kunci 60 FPS) maupun HP mid/low-end (Mali-G57/G52, Adreno 610–642).

---

## 1. Terobosan Forensik Lintas Build (`hybrid20` → `hybrid23`)

Tabel kebenaran dari pengujian fisik Xiaomi Mali-G720 MC7 (Android 16):

| Build / Run | `pool_safe_sync` | `strict_barriers` | `robust_access` | `store_dc` | Hasil Kedip Merah 3D |
|---|---|---|---|---|---|
| `hybrid20 Log2` | serial | `1` | **`1`** | `0` | **0% KEDIP** |
| `hybrid21 Curr` | serial | `0` | **`1`** | `0` | **0% KEDIP** |
| `hybrid23 Curr` (13.660 frame) | serial (`2`, `overlap=0/300`) | `0` | **`0`** | `2/f` | **MASIH KEDIP MERAH** |

### 1.1. Akar Masalah #1 — OOB Constant-Buffer / UBO Read Saat `robustBufferAccess = 0`
- Di [`cbuf_desc()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_descriptors.inc#L254-L300), Unity mengikat UBO berukuran eksak kecil (`u->size`, misalnya 160–512 B), sementara shader offline `shaders.dmsv` mendeklarasikan array UBO penuh (`vec4[4096]`). Pada hardware Maxwell Switch, pembacaan constant-buffer di luar batas selalu mengembalikan `0.0f`.
- Ketika `robustBufferAccess = 0` di Vulkan Mali, bacaan OOB melewati `VkDescriptorBufferInfo.range` membaca byte tetangga di `vk_scratch()` atau `p->buf` yang berisi data acak/`NaN`/`Inf` → memicu ledakan warna merah-oranye 3D selama 1–2 frame bahkan ketika CPU–GPU 100% serial (`safe_sync=2`)!
- Selain itu, `pcache_identity()` sebelumnya tidak memasukkan status `robust_buf` ke dalam header cache `pipeline.cache`, sehingga perubahan `robustBufferAccess` `0 -> 1` berisiko memuat ulang biner shader driver lama yang belum dikompilasi dengan bounds-check robust.

### 1.2. Audit 3 Perubahan `hybrid22`
1. **`vk_end_pass()` skip Mali barrier pada `STORE_OP_DONT_CARE`:** Pada `hybrid22`/`hybrid23`, ketika attachment depth/colour berstatus `STORE_OP_DONT_CARE`, barrier per-image di `vk_end_pass()` dilewati seluruhnya. Pada Mali TBDR, melewatkan sinkronisasi ownership attachment dapat menyebabkan metadata AFBC/Z-compression korup saat target yang sama dipakai ulang di pass berikutnya.
2. **`prepare_draw()` `fb_clean_hit` memo bug (`gfx_vk_exec_draws.inc`):** `fb_clean_hit` sebelumnya membandingkan `fb_pass == g_sub_pass` **sebelum** `pass_commit()` menaikkan `g_sub_pass`. Akibatnya, draw #0 dari pass baru (`S.pass_open == 2`) bisa keliru mencocokkan memo pass sebelumnya ketika `P_att[1..7]` berubah.
3. **`drv_shared()` 4-entry cache & `shader_cpu_ready()` early-return (`setmemo == 1`):** Keduanya diaudit dan terverifikasi aman — `drv_shared()` mencocokkan hash & byte eksak, sementara `shader_cpu_ready()` hanya melewatkan cek bila shader tidak membaca pointer/handle dari UBO (`setmemo == 1`).

### 1.3. Akar Masalah `conf=9..36/300` di `hybrid23 Prev` (`safe_sync=1`)
- Di `hybrid23`, [`rb_conflicts_with_dirty()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c) memanggil `nvn_dirty_since_span()` untuk mendeteksi konflik readback. Namun `completion_main()` milik kita sendiri memanggil `dm_guest_write_prepare()` → `stamp_page()`, yang menaikkan `r->gen` di blok readback itu sendiri sehingga `rb_conflicts_with_dirty()` menghasilkan false-positive `conf=9..36` per 300 frame dan memutus `overlap`!

### 1.4. Akar Spike `pool_readback = 40.99 ms` & `replay = 38–111 ms`
- **`pool_readback = 40.99 ms` (frame 11380):** [`lin_back()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc#L781-L795) memanggil `vk_readback_unknown()` tepat setelah `vk_gpu_wrote((uint64_t)(uintptr_t)t->mem, len)`, memaksa `pools_copy_back()` meng-invalidate & mem-merge seluruh pool 64–128 MB.
- **`replay = 38–111 ms` (frame 3591/4443/7014/7278/11096):** [`bc_decode_ex()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_tex.c) menulis file cache `.bc` multi-MB secara sinkron (`open`/`write`/`rename`) langsung di render thread, sementara `PCACHE_BURST = 64` menunda penyimpanan pipeline cache baru saat gameplay.

---

## 2. Perbaikan yang Diterapkan di `v1.1.2-hybrid24`

1. **OOB Protection Default ON (`DM_VK_ROBUST_ACCESS=1`) + 16-Byte `vec4` Tail Padding + Cache Key `-r%d`:**
   - [`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c): Mengaktifkan `robustBufferAccess = VK_TRUE` secara **DEFAULT** pada semua GPU Mali & Adreno (`V.robust_buf = 1`), dapat dimatikan untuk A/B lewat `DM_VK_ROBUST_ACCESS=0`.
   - `vk_sub_update()` & `vk_ubd_shadow()` membulatkan alokasi `vk_scratch()` ke kelipatan 16 byte (`vec4`) dengan zero-padding ekor; [`cbuf_desc()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_descriptors.inc) menyelaraskan `range` UBO ke kelipatan 16 byte (di-clamp ke batas pool/buffer).
   - [`pcache_identity()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_pipeline_cache.inc) diperbarui menjadi `"h24-m%d-a%d-r%d"` dan `PIPE_MAGIC_BASE` dinaikkan ke `"PPE9"` (`0x39455050u`) agar cache driver lama tanpa robust bounds-check otomatis diperbarui bersih.
2. **Hardening 3 Perubahan `hybrid22` & Fix `conf=9..36`:**
   - [`vk_end_pass()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc#L828-L909): Selalu memancarkan `VkImageMemoryBarrier` per-image pada seluruh attachment depth & colour (termasuk `STORE_OP_DONT_CARE` via sinkronisasi attachment-to-attachment ringan + full AFBC flush untuk target stored & inline-resolved).
   - [`prepare_draw()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_draws.inc): Mensyaratkan `S.pass_open == 1` untuk membaca & mengisi memo `fb_clean_hit`.
   - [`rb_conflicts_with_dirty()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c): Menghapus cek `nvn_dirty_since_span()` penyebab konflik palsu `conf=9..36` agar `safe_sync=1` kembali ke `overlap >= 298/300` & `fence_wait <= 0.1 ms`.
   - [`pools_copy_back()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c): Menjaga agar pool `CPU_UNCACHED` (`p->flags & 0x2`) tidak pernah memicu whole-pool `pool_cache_sync()` meski `readback_ranges == 0`.
3. **Inline TBDR MSAA Resolve (`DM_VK_INLINE_RESOLVE=1`, Default ON):**
   - [`gfx_vk_exec_passes.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc), [`gfx_vk_exec_copies.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_copies.inc), [`gfx_vk_exec_pipelines.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_pipelines.inc): Mendeteksi pass MSAA 1-colour yang langsung diikuti `CommandBufferDownsample` (`OP_COPY_T2T DM_COPY_RESOLVE`) tanpa op pemecah pass di antaranya, lalu menyatukan resolve langsung ke tile hardware (`pResolveAttachments` + `STORE_OP_DONT_CARE`) dengan `vk_view_usage`/`vk_view_layers` spec-valid, menghemat bandwidth tulis+baca MSAA 2x ke RAM, dan mencatat counter `inline_res` di log 300-frame.
4. **Eliminasi Spike `pool_readback = 40.99 ms` & `replay = 38–111 ms`:**
   - [`lin_back()`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc#L781-L795): Menghapus panggilan redundan `vk_readback_unknown()` karena byte range persis sudah dicatat `vk_gpu_wrote()`.
   - [`gfx_vk_tex.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_tex.c): Memindahkan penulisan disk cache `.bc` (`bc_cache_write_async()`) ke thread latar belakang `bc-cache-io` (antrean terbatas 16 job / 16 MiB, `nice +10`) sehingga render thread bebas blokir I/O flash.
   - [`gfx_vk_pipeline_cache.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_pipeline_cache.inc) & [`front_android.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/front_android.c): Menurunkan `PCACHE_BURST` `64 -> 10`, `g_pcache_gap_ns` `15s -> 10s`, dan menyimpan `VkPipelineCache` secara asinkron saat `onPause()`.
5. **ADPF Responsif, Kebijakan Low-End Diperluas, Registrasi `settings.tsv`, & Kontrol Launcher:**
   - [`perf_android.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/perf_android.c): ADPF 60 FPS boost dipicu di `g_frame_gpu > target * 92 / 100` (`over >= 4`, cooldown `2.5s`) sambil tetap mengabaikan loading screen (`GPU < 3 ms` atau `CPU > 50 ms`).
   - [`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c): Kebijakan auto low-end diperluas mencakup `Adreno 610/612/618/619/642` dan default `0.55x` untuk `Mali-G52` / ultra-low-end (`0.67x` untuk `Mali-G57/G68`).
   - [`settings.tsv`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/settings.tsv): Mendaftarkan `DM_VK_INLINE_RESOLVE`, `DM_VK_SAFE1_DIFF1K`, `DM_VK_SAFE1_HOT_BLOCKS`, `DM_VK_SAFE1_RB_STAGED` — **0 peringatan `[RED] setting ... is not in settings.tsv`** (26/26 kunci `env.txt` dan 29/29 kunci `RuntimeConfigManager` terdaftar).
   - [`RuntimeConfigManager.java`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/java/org/nx2a/doraemon/RuntimeConfigManager.java) & [`LauncherActivity.java`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/java/org/nx2a/doraemon/LauncherActivity.java): Migrasi `hybrid24_migrated` mengaktifkan `robustAccess=true`, `inlineResolve=true`, `poolSafeSyncMode=1`, mempertahankan preset low-end user, serta memperjelas kontrol **MSAA 2x Mode** dan **Resolution Scale** (`Auto / 100% / 85% / 75% / 67% / 55% / Docked 1080p`).

---

## 3. Hasil Build & Verifikasi Offline (`v1.1.2-hybrid24`)

> **Transparansi:** Seluruh pemeriksaan di bawah adalah **verifikasi offline / host-side**; pengujian perangkat fisik Android belum dijalankan dan menunggu instalasi APK oleh pengguna.

- **`libdmhost.so` (`arm64-v8a`, 16 KB `PT_LOAD` aligned):** `2259 KB`, `compiled 196 sources, 0 failed`, 4/4 `LOAD` `Align 0x4000`, SHA-256 `781a1c2447b105fdda74945a1d187fef5af3b92b93b2eefdbde1e1ed15a25dd3`
- **Unified Patch:** [`hybrid_port/patches/nx2android_unified_mali_adreno.patch`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/patches/nx2android_unified_mali_adreno.patch) (`5752` baris, 28 file vs `de6ddd8`)
- **Signed Release APK (`versionCode = 24`, `versionName = '1.1.2-hybrid24'`):** [`hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid24-arm64-release-signed.apk`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid24-arm64-release-signed.apk) (`31 MB` / `32,385,047 bytes`), SHA-256 `d58fab3c23463fe1c65306d90bf87d6e80a68dd2272d01b3c87f95a9cdebac2a`
- **Offline Verification (`verify_hybrid_apk.sh`):** **7/7 PASS** (`zipalign -c -P 16 4` OK di offset `114688`/`2441216`, `apksigner verify` v3 OK, 0 file terlarang, `shaders.dmsv` & `.nxpatch` Stored `0%`, semua segmen ELF `LOAD` `0x4000`, seluruh kelas/metode JNI utuh pasca-R8)
- **Direktori Terlindungi:** `hybrid_port/l10n_id/`, `Game/`, dan `tools/` 100% tidak tersentuh.
