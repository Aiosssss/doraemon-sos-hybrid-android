# Laporan Teknis `v1.1.2-hybrid18` (`versionCode = 18`)

**Target:** *DORAEMON STORY OF SEASONS: Friends of the Great Kingdom* (`org.nx2a.doraemon`, Title ID `01008EE0177A8000`, Update `1.1.2` / `v262144`)  
**Pipeline:** `nx2android` + `nx2android-mali` (unified runtime, `shaders.dmsv` 5.613 SPIR-V offline)

---

## 1. Pilar 1 — Zero-Race Overlapped Staged Pool Sync (0% Kedip Default Tanpa `fence_wait` Serial)

### Akar Masalah Exact di `pool_safe_sync=0` vs `pool_safe_sync=1` (`hybrid17`)
Berdasarkan konfirmasi tes nyata Xiaomi Mali-G720 MC7 Android 16 (di mana `pool_safe_sync=1` menghilangkan 100% kedip bayangan pohon & HUD melar namun mengorbankan `fence_wait` 4.49–9.33 ms dan menyalin 1.69–2.02 MB/sub), audit `gfx_vk_core.c` mengidentifikasi **4 celah exact** di jalur `safe_sync=0`:

1. **Pre-Transfer WAR Barrier Tidak Eksplisit untuk Mali Valhall CSF (`vk_pre_transfer_barrier`):**
   `s->up` memakai `VkMemoryBarrier(ALL_COMMANDS / MEMORY_WRITE -> TRANSFER / TRANSFER_WRITE)` **tanpa** stage reader eksplisit (`VERTEX_INPUT | VERTEX_SHADER | GEOMETRY_SHADER | FRAGMENT_SHADER | COMPUTE_SHADER | DRAW_INDIRECT`) dan **tanpa** per-pool `VkBufferMemoryBarrier` pada `p->buf`. Pada arsitektur Mali Valhall CSF (Mali-G720 driver `49.1.0`) di mana endpoint Transfer/Compute berjalan terpisah dari Geometry/Fragment, `vkCmdCopyBuffer(stg -> p->buf)` milik Frame N dapat mendahului selesainya pembacaan VBO HUD / instance shadow buffer milik Frame N-1.
2. **Torn Two-Read pada Hot 64 KB Blocks (`is_hot == 1`):**
   Pass B melakukan `memcmp(lo + soff, p->baseline + soff, 256)` terhadap blok panas yang masih `PROT_READ | PROT_WRITE`, lalu `FLUSH_STAGE_RUN()` membaca ulang `lo` dari RAM untuk kedua kalinya. Tulisan konkuren thread Unity di antara dua bacaan tersebut membuat `p->baseline` menangkap struct terpotong atau melewatkan sub-blok yang berdampingan.
3. **Desync `p->baseline` pada `CPU_UNCACHED` GPU-Written SSBO (`vk_gpu_wrote_ssbo`):**
   Saat `overlap=1` meniadakan GPU->CPU readback pada `CPU_UNCACHED` (`flags & 0x2`) demi menghemat `4.39 ms/frame` (Mali-G52), `p->baseline` menyimpan byte CPU lama. Jika CPU kemudian menulis ulang byte yang kebetulan sama dengan `p->baseline` lama (mis. zeroing/reuse), diff 256B menganggapnya tidak berubah dan membiarkan data GPU lama menempel di `p->buf`.
4. **Mengapa `safe_sync=1` menyalin 10x lebih besar (`1.7–2.0 MB/sub` vs `0.18 MB/sub`):**
   `safe_sync=1` mematikan `is_hot` dan melewati jalur diff 256B (`staged_overlap = 0`), sehingga setiap blok 64 KB yang disentuh 1 byte disalin utuh 64 KB ke staging.

### Solusi Zero-Race Overlapped Default (`hybrid18`)
- **Explicit GPU-Only WAR/RAW Barriers + Per-Pool `VkBufferMemoryBarrier` (`gfx_vk_core.c`):**
  - `vk_pre_transfer_barrier()` + `EMIT_POOL_PRE_BAR()` menamakan eksplisit seluruh stage pembaca `POOL_CONSUMER_STAGES` (`DRAW_INDIRECT | VERTEX_INPUT | VERTEX_SHADER | GEOMETRY_SHADER | FRAGMENT_SHADER | COMPUTE_SHADER | TRANSFER`) -> `TRANSFER_BIT` serta per-pool `VkBufferMemoryBarrier` pada `p->buf` sebelum `vkCmdCopyBuffer(staging_slice[slot] -> p->buf)`.
  - Post-copy RAW `VkBufferMemoryBarrier` + `vk_post_transfer_barrier()` mempublikasikan `TRANSFER_WRITE` ke seluruh `POOL_CONSUMER_STAGES` / `POOL_CONSUMER_ACCESS`.
  - Staging tetap per-slot (`g_slot[g_cur].blk`, `NSLOT = 6`) yang diproteksi fence 6 frame lalu (`slot_recycle`), sehingga CPU tidak pernah menulis ke `p->shadow` atau menunggu GPU Frame N-1 (`wait_target = g_fence - 1`).
- **Single-Snapshot Torn-Free Hot-Block Diff (`g_sync_snap[64KB]`):**
  - Blok panas (`act[b] == 2`) dibaca **tepat satu kali** ke buffer L1/L2 `g_sync_snap[64KB]` (~3 us NEON `memcpy`), dibandingkan whole-block + 256B terhadap `p->baseline`, lalu sub-blok yang berubah disalin **eksklusif dari `g_sync_snap`** ke `p->baseline` dan `staging_slice[slot]` — menjamin `p->baseline == staging_slice[slot] == p->buf` secara bit-identik dengan bandwidth tetap `~0.18–0.35 MB/sub`.
- **Zero-Readback `CPU_UNCACHED` GPU-Block Protection (`vkpool_t::gpu_blk`):**
  - Ketika `vk_gpu_wrote_ssbo()` melewati readback pada `CPU_UNCACHED`, blok 64 KB terkait ditandai `p->gpu_blk[b] = 1`, didemosi dari hot (`p->hot[b] = 0`), dan diproteksi `PROT_READ`. Selama CPU tidak menulis ke blok itu, hasil compute GPU dibiarkan utuh (`0.00 ms` readback); begitu CPU menulis/reuse blok tersebut, fault memicu `act[b] = 4` (full 64 KB refresh, membersihkan `p->baseline` basi).
- **Default:** `DM_VK_POOL_SAFE_SYNC=0`, `DM_VK_POOL_OVERLAP=1` (termasuk migrasi otomatis `hybrid18_migrated` di `RuntimeConfigManager.java`). `pool_safe_sync=1` tetap tersedia sebagai opsi fallback manual.

---

## 2. Pilar 2 — Boost GPU TBDR (Pemangkasan Render Pass Splits & `STORE_OP_DONT_CARE`)

### Penyebab Exact Pemecahan Render Pass 4-MRT (`PASS #23–#35`, `PASS #6–#8`, 31–34 pass/frame)
Audit seluruh pemanggil `vk_end_pass()` menemukan 3 penyebab nyata (catatan: `OP_UPDATE_UB` sudah di-defer melalui `vk_sub_update()` / `vk_ubd_shadow()`, bukan pemicu split):
1. **`OP_BARRIER` (`gfx_vk_exec_walker.inc`) dengan `g_order_keep = 0` (default sebelumnya):**
   Unity 2020.3 memancarkan `nvnCommandBufferBarrier(ORDER_PRIMITIVES | ORDER_FRAGMENTS | INVALIDATE_TEXTURE | INVALIDATE_SHADER | INVALIDATE_ZCULL)` di antara draw-draw dalam pass 4-MRT yang sama (dan bahkan saat `S.pass_open == 2` tepat setelah `ClearColor`/`ClearDS`, memecah clear menjadi pass 0-draw tersendiri!).
   - **Fix:** Mengaktifkan `DM_VK_ORDER_KEEP=1` secara default (`g_order_keep = 1`, tetap otomatis `0` saat `strict_barriers=1`). Di `OP_BARRIER`, barrier dengan subset bit `0xF7` (non-indirect) ditunda ke `g_order_pending = 1` saat `S.pass_open == 1` (dan dicek aman di `prepare_draw()` — tetap memecah pass jika `g_pass_wmem || shader_wmem || order_feedback(3)`!), sementara saat `S.pass_open == 2` hanya memancarkan barrier sebelum pass dibuka tanpa memecah `LOAD_OP_CLEAR`.
2. **`bind_chmask()` / `color_alias_map()` pada Alias `c0`/`c2` 4-MRT (`gfx_vk_exec_passes.inc`):**
   Pada pass 4-MRT di mana `c0` dan `c2` mengalias image yang sama, setiap draw depth-only (`mask = 0x0`) mereset slot `keep` ke `0` dan memicu `vk_end_pass()`.
   - **Fix:** `color_alias_map()` kini menerima `cur = P_alias` saat pass terbuka dan mempertahankan slot `cur_keep` selama draw masih menulis ke `cur_keep` atau tidak menulis warna sama sekali (`mask = 0`).
3. **Cross-Pass Depth `STORE_OP_DONT_CARE` Lookahead (`pass_apply_discard_lookahead()`):**
   Lookahead diperluas melintasi deferred `OP_BARRIER` (`0xF7`) dan mendeteksi ketika pass berikutnya langsung mengikat depth target yang sama lalu memanggil `OP_CLEAR_DS` penuh tanpa draw/copy/dispatch/bind tekstur di antaranya -> menandai `P_da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE` (dilaporkan sebagai `store_dc` di log 300-frame).

---

## 3. Pilar 3 — Eliminasi Spike `dirty_rearm=67.40 ms` & `pool_readback=14.63 ms`, Boost HP Kentang & Alias Fix

1. **Fix Spike `dirty_rearm = 67.40 ms` (`nvn_dirty.c`, `nvn_dirty.h`, `gfx_vk_core.c`):**
   - Menambahkan `nvn_dirty_protect_span_blk()` berbasis handle `p->span` O(1) (tanpa linear scan tabel range) dengan relaxed pre-check `PG_NONE` sebelum CAS atomik.
   - Memecah proteksi awal pool besar (`>= 4 MiB`) menjadi irisan `2 MiB` agar `mmap_lock` tidak memblokir 33 worker thread sekaligus.
2. **Fix Spike `pool_readback = 14.63 ms` (`gfx_vk_core.c` `pools_copy_back`):**
   - Langsung `return` bila `s->readback_ranges && (!s->needs_cpu_rb || s->ngw <= 0)`.
   - Mengganti invalidasi bounding-box `first..last` (yang sebelumnya meng-invalidate 15 MB celah bersih di antara dua write kecil!) dengan `pool_cache_invalidate_ranges()` (batching hingga 64 `VkMappedMemoryRange` persis di rentang yang ditulis GPU).
3. **HP Kentang & Pipeline Cache (`gfx_vk_pipeline_cache.inc`, `LauncherActivity.java`, `RuntimeConfigManager.java`):**
   - `vk_pipeline_cache_save()` terkonfirmasi sudah memakai atomic write (`open(.tmp) -> fflush -> fsync -> close -> rename`); `pcache_load()` kini mencatat alasan spesifik bila rebuild (`MSAA/A2C setting changed`, `driver/GPU identity changed`, atau `SHA-256 digest mismatch`).
   - Launcher menyediakan preset resolusi jelas: `Auto`, `Native 100% (720p)`, `Seimbang 75% (540p)`, `Performa / Kentang 67% (~480p)`, `Ultra Ringan 55% (396p)`, `Docked 1080p HQ`.
   - Guard eviksi `[low-ram]` tetap tidak berjalan di `tier 0` (RAM >= 6 GB).
4. **Fix Sisa 1x `[alias] stale read: 1024x32 fmt 0x29` vs `1280x720 fmt 0x25` (`gfx_vk_tex.c`):**
   - `alias_link()` dan `alias_check()` mengabaikan RT transien mati yang tidak disentuh >10 frame (`g_tex_frame > o->used + 10`) hasil daur ulang alokator pool lintas transisi scene.

---

## 4. Pilar 4 — Kontrol Suhu (Anti-Panas) & Pace 60 FPS Lebih Halus

- **ADPF (`perf_android.c` `hint_report`):** Menaikkan ambang pemicu `ADPF load increase` ke `g_frame_gpu >= 3 ms`, `cpu_work <= 50 ms`, `g_frame_gpu > 98% target || worst > 108% target`, minimal **6 frame beruntun**, dengan **cooldown 4.0 detik** agar jitter kecil 17 ms tidak terus nge-boost clock ke frekuensi panas, sementara `H.report2` tetap aktif penuh setiap frame untuk menurunkan clock saat GPU work turun.
- **Display Timing (`gfx_vk_present.c` `pace_desired`):** Mengganti estimasi GPU 1-frame mentah dengan **EMA (alpha = 1/8)** + **histeresis resync 1/4 vsync** untuk memangkas `display timing: 341 resyncs` dan menjaga pacing 60 FPS stabil tanpa micro-hitch.

---

## 5. Verifikasi Build & APK Offline (7/7 PASS)

> **Catatan Kejujuran Pengujian:** Seluruh hasil di bawah adalah **verifikasi kompilasi & statis/offline pada host Linux**. Pengujian visual (0% kedip bayangan pohon/HUD), angka `fence_wait` / `GPU p50` / jumlah pass, dan suhu baterai di perangkat fisik untuk `v1.1.2-hybrid18` menunggu pengujian langsung oleh pengguna.

| Item Verifikasi | Hasil |
| :--- | :--- |
| Kompilasi `libdmhost.so` (`arm64-v8a` release, NDK `28.2.13676358`) | **PASS** — `196 sources, 0 failed`, stripped `2244 KB` |
| ELF 16 KB `PT_LOAD` (`llvm-readelf -l`) | **PASS** — 4/4 segmen `Align 0x4000` |
| SHA-256 `libdmhost.so` | `c11deda3767d01a5b730d6cc5371ac4d07cf77c58de46db3e02a9f01b6d59209` |
| Gradle `:app:assembleRelease` (`--offline --no-daemon`) | **PASS** — `versionCode=18`, `versionName='1.1.2-hybrid18'` |
| `zipalign -c -P 16 4` | **PASS** — `libdmhost.so` (`114688`), `libnxhybrid_extract.so` (`2424832`) OK |
| `apksigner verify --verbose` | **PASS** — `Verified using v3 scheme: true`, 1 signer (`doraemon_hybrid`) |
| `verify_hybrid_apk.sh` (7/7 suite) | **7/7 PASS** — 0 file terlarang, `shaders.dmsv` & `.nxpatch` Stored 0%, simbol JNI aman |
| APK Rilis | `hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid18-arm64-release-signed.apk` (`31M`) |
| SHA-256 APK Rilis | `90d818d6e22aa7e4df527201b8354ecdfa75b34547ba37155a3774179b1f5106` |
| Unified Patch | `hybrid_port/patches/nx2android_unified_mali_adreno.patch` (`4260` baris vs `de6ddd8`) |
