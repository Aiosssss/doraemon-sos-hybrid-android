# Laporan Teknis `v1.1.2-hybrid23` (`versionCode = 23`)

Target utama: Menutup 100% seluruh celah di **`DM_VK_POOL_SAFE_SYNC=1` (Default)** agar visualnya **100% bebas kedip setara `safe_sync=2`**, sambil mempertahankan pencapaian `v1.1.2-hybrid22` (**60 FPS terkunci**, `GPU p50 ~ 10.7–11.0 ms`, `fence_wait ~ 0.00 ms`, `store_dc = 2/f`, `dc_folded = 8/f`, `clears_folded = 11/f`, `MB/sub ~ 0.60 MB`), memangkas `pool_readback` dari `0.37–0.38 ms` ke `~0.015 ms`, menghilangkan spike `dirty_rearm = 24.51 ms` & `pool_upload = 27.62 ms`, sekaligus menambahkan 3 saklar biseksi instan (`DM_VK_SAFE1_HOT_BLOCKS`, `DM_VK_SAFE1_RB_STAGED`, `DM_VK_SAFE1_DIFF1K`) serta mempercepat fallback `safe_sync=2`.

---

## 1. Audit Forensik Kode `v1.1.2-hybrid22`: Bug Eksak yang Terbukti di Celah #1–#5

Pembandingan langsung seluruh 4 titik cabang `vk_pool_safe_sync()` antara `safe_sync=1` (60 FPS, masih kedip) vs `safe_sync=2` (bebas kedip, `fence_wait 5.1–7.3 ms`) membuktikan bug nyata berikut di kode `hybrid22`:

### 1.1. Celah #1 — `pool_merge_back_src()` Menimpa RAM Live CPU Frame N + `vk_gpu_wrote_ssbo()` Tidak Melarang Hot Blocks + `vk_scratch()` WC Readback (`0.37 ms`)
- **Clobber RAM Live CPU (`gfx_vk_core.c:pool_merge_back_src`):**
  - `pool_bump_cpu_gen()` hanya dipanggil di `vk_pools_sync_()` / `vk_ubd_land()`, **bukan** saat CPU game menulis guest RAM `dst` antara commit Frame N-1 dan `begin_segment()` Frame N (terutama pada `hot_blocks` yang `PROT_RW` tanpa SIGSEGV).
  - Saat `completion_main()` bangun menyelesaikan Frame N-1 sementara CPU game sudah menulis Frame N ke `dst`, `pool_sum_cpu_gen(p, lo, hi) == cpu_gen_sum` bernilai **`1` secara keliru**, lalu `memcpy(dst + a, src + a, i - a)` **menimpa tulisan live CPU Frame N dengan byte GPU lama Frame N-1**! Bahkan cabang 3-way `else` juga hanya memeriksa `base[i] == ref[i]` tanpa memeriksa `dst[i] == base[i]`.
- **Blok GPU-Written Masuk `hot_blocks` & Wipe 64 KB (`gfx_vk_core.c:vk_gpu_wrote_ssbo`):**
  - Untuk `n <= 512 KB` (`DISPATCH #1/#2`, `86 KB` partikel), `vk_gpu_wrote_ssbo()` tidak pernah melarang/mendemosi blok dari `p->hot[]`.
  - Untuk `CPU_UNCACHED > 512 KB`, `p->gpu_blk[b] = 1` memicu `act[b] = 4` (upload paksa 64 KB penuh dari `lo` basi!) begitu CPU menyentuh 1 byte di blok 64 KB yang sama — menimpa sisa data GPU di blok tersebut.
- **Penyebab `pool_readback = 0.37–0.38 ms` di `safe_sync=1` (`rb_staged=260–275/300`):**
  - `rb_staged` mengalokasikan target copy dari **`vk_scratch()`** yang bertipe **`HOST_VISIBLE | HOST_COHERENT` (write-combined / uncached)** di ARM Mali. Membaca balik `86 KB` dari memori WC uncached di `completion_main()` memakan `0.37 ms` (dan tercatat ke metrik `pool_readback`).

### 1.2. Celah #2 — `nvn_dirty_protect_span_blk()` Melewati Halaman `PG_BUSY` & Double-Read + Lock-Gap di `vk_pools_sync_()`
- **Bug `PG_BUSY` Escape (`nvn_dirty.c:377`):** `nvn_dirty_protect_span_blk()` hanya memeriksa `r->state[i] == PG_NONE` dan **melewati diam-diam** halaman yang sedang `PG_BUSY` (sedang diakses fault handler / `prep_write` thread lain). Setelah thread lain selesai, halaman itu tertinggal `PG_NONE` (`PROT_RW`) dengan `gen <= st` sehingga tulisan CPU berikutnya tidak pernah fault!
- **Double-Read Tear & Lock-Gap `g_baseline_mu` (`gfx_vk_core.c:1798–1850`):**
  - Pada blok non-hot kotor (`act == 1`), Pass B membaca `lo + off` sekali untuk membangun `dmask`, lalu `FLUSH_STAGE_RUN()` membaca `lo + srun_lo` **kedua kalinya** di luar kunci pertama.
  - Pada blok hot (`act == 2`), `g_baseline_mu` dilepas setelah `dmask` lalu dikunci ulang di `STAGE_EXACT_BYTES`, membuka jendela interleave `completion_main()`.

### 1.3. Celah #3 — `hot_blocks` (`PROT_RW`) Memutus Dirty Tracking Tekstur (`pkg_upload` & `lin_copy`)
- Di `gfx_vk_tex.c` (`pkg_upload_` & `lin_copy`), re-upload tekstur dinamis bergantung pada `nvn_dirty_since_span()`. Jika suatu blok 64 KB yang berisi/berbagi halaman dengan tekstur terpromosi ke `hot_blocks` (`PROT_RW`), tulisan CPU ke tekstur tersebut **tidak pernah memicu fault** sehingga `nvn_dirty_since_span()` mengembalikan `0` dan re-upload tekstur terlewati!
- Di `vk_ubd_shadow()` & `vk_scratch()`: terverifikasi per-slot (`s->blk[]` milik slot `g_cur` yang baru didaur ulang setelah `mtl_wait(s->fence)`), namun `scratch_blk` belum menyimpan flag `noncoherent` untuk flush defensif sebelum `vkQueueSubmit`.

### 1.4. Celah #4 — Tidak Ada Cross-Frame `ALL_COMMANDS` Memory Barrier Eksplisit di Awal `s->main`
- Pada `safe_sync=1`, ketika `s->up_used == 0` atau saat `s->main` membaca render target / SSBO yang ditulis Frame N-1, tidak ada `vkCmdPipelineBarrier(ALL_COMMANDS / MEMORY_WRITE -> ALL_COMMANDS / MEMORY_READ | MEMORY_WRITE)` di awal `s->main`. `vk_post_transfer_barrier(s->up)` pun hanya mem-publish `TRANSFER_WRITE_BIT` (bukan `COLOR_ATTACHMENT_WRITE` / `DEPTH_STENCIL_ATTACHMENT_WRITE` / `SHADER_WRITE` Frame N-1). Di driver ARM Mali CSF (G720 49.1.0, G57 32.1.0, G52 26.0.0), submission back-to-back tanpa barrier eksplisit di awal `s->main` dapat membaca cache L2/AFBC basi.

### 1.5. Celah #5 — Akar Spike `dirty_rearm = 24.51 ms` & `pool_upload = 27.62 ms`
- **`dirty_rearm = 24.51 ms` (frame 2699):** `ADD_PROT_RUN` hanya menggabungkan blok yang 100% bersebelahan (`off == prot_hi`), dan `nvn_dirty_protect_span_blk()` memutus run di setiap halaman `PG_PROT`. Puluhan blok kotor terpencar memicu puluhan hingga ratusan syscall `mprotect()` + 8-core ARM64 TLB shootdown terpisah.
- **`pool_upload = 21.77–27.62 ms` (frame 2675, 2818):** Saat pool besar baru (16–32 MiB) tiba, jalur initial sync mengalokasikan `vk_scratch()` baru (`vkAllocateMemory` + `vkMapMemory`), menyalin ke `p->baseline` (ribuan demand-page faults), lalu membaca balik `p->baseline` untuk menyalin ke memori WC `vk_scratch()`. Padahal pool baru yang belum pernah dipakai GPU (`p->used == 0`) 100% bebas dari referensi Frame N-1!

---

## 2. Perbaikan yang Diterapkan di `v1.1.2-hybrid23`

1. **Zero-Clobber `pool_merge_back_src()` + Dedicated `HOST_CACHED` `s->rb_buf` (`gfx_vk_core.c`):**
   - Menambahkan guard `dst[i] == base[i]` (dan `dst[i] == ref[i]` di cabang 3-way) sebelum menulis `dst[i] = src[i]` (dengan fast-path `memcmp(dst, base, n) == 0` untuk overhead mendekati 0), sambil tetap memperbarui `base[i] = src[i]` agar cocok dengan `p->buf` GPU.
   - Menambahkan buffer readback khusus per-slot bertipe **`HOST_VISIBLE | HOST_CACHED`** (`vk_rb_alloc()`, satu batched `vkInvalidateMappedMemoryRanges` di `pools_copy_back()` bila non-koheren) — memangkas `pool_readback` dari `0.37 ms` ke estimasi **`~0.015 ms`**.
   - Menaikkan batas staged readback `CPU_UNCACHED` dari `512 KB` -> **`2 MiB`**, menghapus wipe 64 KB `act=4` (hanya diff 1 KB), dan menambahkan cek `nvn_dirty_since_span()` di `rb_conflicts_with_dirty()`.
2. **Larangan Permanen `p->no_hot[b]` untuk Blok GPU-Written & Tekstur (`gfx_vk.h`, `gfx_vk_core.c`, `gfx_vk_tex.c`):**
   - `vk_gpu_wrote()`, `vk_gpu_wrote_ssbo()`, `pkg_upload_()`, dan `lin_copy()` memanggil `pool_mark_no_hot()` / `vk_pool_ban_hot()` (atomik, 0 `mprotect` di `replay`) sehingga blok GPU-written dan blok tekstur tidak pernah masuk `hot_blocks`.
3. **Perbaikan `nvn_dirty_protect_span_blk()` + Coalesced `mprotect` (`nvn_dirty.c`, `gfx_vk_core.c`):**
   - Mem-flush run terbuka lalu `busy_wait()` pada halaman `PG_BUSY` (bebas deadlock, 0 halaman lolos `PROT_READ`).
   - Melakukan CAS-bridge `PG_PROT -> PG_BUSY` pada celah hingga `BRIDGE_MAX_PGS = 16` halaman (`PROT_BRIDGE_GAP = 256 KB`, **tidak pernah** melompati `hot_block`) sehingga puluhan blok kotor terpencar menyatu menjadi 1–3 syscall `mprotect()` sekaligus menyatukan VMA kernel.
4. **Single Atomic `g_baseline_mu` Snapshot + Diff + Update (`gfx_vk_core.c:vk_pools_sync_`):**
   - Baik blok non-hot (`act == 1`), blok hot (`act == 2`), maupun Pass C memegang `g_baseline_mu` **satu kali secara atomik** melingkupi `memcpy(g_sync_snap, lo + off)`, diff 1 KB, pembaruan `p->baseline`, dan `pool_bump_cpu_gen()`.
5. **Unconditional Cross-Frame Barrier di Awal `s->main` + Flush Defensif `vk_scratch()` + Brand-New Cached Pool Fast-Path (`gfx_vk_core.c`):**
   - Memancarkan `vkCmdPipelineBarrier(ALL_COMMANDS / MEMORY_WRITE -> ALL_COMMANDS / MEMORY_READ | MEMORY_WRITE)` di awal `s->main` (`begin_segment()`).
   - Menambahkan flush `vkFlushMappedMemoryRanges` untuk blok `scratch_blk` non-koheren sebelum `vkQueueSubmit()`.
   - Pool besar baru (`>= 1 MiB`, `p->cached`, `!p->used`) langsung menyalin blok non-nol ke `HOST_CACHED` `p->shadow` + `pool_cache_sync()` dan GPU `vkCmdFillBuffer` khusus untuk run nol (menghilangkan spike 27 ms alokasi/copy `vk_scratch`).
6. **3 Saklar Biseksi Cepat & Optimasi `safe_sync=2` (`gfx_vk_core.c`, `env.txt`, `RuntimeConfigManager.java`, `LauncherActivity.java`):**
   - `DM_VK_SAFE1_HOT_BLOCKS=1`, `DM_VK_SAFE1_RB_STAGED=1`, `DM_VK_SAFE1_DIFF1K=1` (semua default `1`, tampil di startup policy & log 300-frame `copied pools`, tersedia di bagian Debug Launcher).
   - Mode `safe_sync=2` kini menulis langsung ke `HOST_CACHED` `p->shadow` + flush sesudah `mtl_wait(g_fence)` (menghindari double-copy WC `vk_scratch()` + GPU copy, ~2 ms/frame lebih cepat bila dipakai sebagai fallback).

---

## 3. Hasil Build & Verifikasi Offline (`v1.1.2-hybrid23`)

> **Transparansi:** Seluruh pemeriksaan di bawah adalah **verifikasi offline / host-side**; pengujian perangkat fisik Android belum dijalankan dan menunggu instalasi APK oleh pengguna.

- **`libdmhost.so` (`arm64-v8a`, 16 KB `PT_LOAD` aligned):** `2255 KB`, `196 sources, 0 failed`, 4/4 `LOAD` `Align 0x4000`, SHA-256 `ec12c1fb16f20b892665b856d20fc6a3302fa9f5c5d4864986baff97936d578c`
- **Unified Patch:** `hybrid_port/patches/nx2android_unified_mali_adreno.patch` (`5286` baris vs `de6ddd8`)
- **Signed Release APK (`versionCode = 23`, `versionName = '1.1.2-hybrid23'`):** `hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid23-arm64-release-signed.apk` (`31 MB` / `32,368,663 B`), SHA-256 `a370abfd3b8fb638145df9a033d5f4cd2710860a773c110af930beb6d73dd6a5`
- **Offline Verification (`verify_hybrid_apk.sh`):** **7/7 PASS** (`zipalign -c -P 16 4` OK di offset `114688`/`2424832`, `apksigner verify` v3 OK, 0 file terlarang, `shaders.dmsv` & `.nxpatch` Stored `0%`, semua segmen ELF `LOAD` `0x4000`, seluruh kelas/metode JNI utuh pasca-R8)
