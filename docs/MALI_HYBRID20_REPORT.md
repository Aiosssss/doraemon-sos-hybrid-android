# Laporan Teknis `v1.1.2-hybrid20` (`versionCode = 20`)

Hotfix visual & performa lintas perangkat (flagship Mali-G720, mid-range Mali-G57 MC2, low-end Mali-G52 & Adreno) berdasarkan bukti video nyata (`video_1.mp4`, `video_2.mp4`) dan log `v1.1.2-hybrid19` pada **Infinix X678B** (Android 14, Mali-G57 MC2, Vulkan 1.1.177, driver 32.1.0, `render_scale=0.67`, `disable_msaa=1`, `pool_safe_sync=0`, `pool_overlap=1`, `ubd_copyback=1`).

---

## 1. Bug #1 — Kembalinya Kedip Merah-Oranye 3D (Ritme ~0.8–1.4 Detik)

### Bukti & Akar Penyebab Exact
Ditemukan **2 akar penyebab** yang bekerja bersamaan di `hybrid18–19`:

1. **`OP_BARRIER` menelan bit cache-invalidation `0x10 | 0x20 | 0x40` (`0xF7`):**
   Di `gfx_vk_exec_walker.inc` (`case OP_BARRIER`), mask penahanan barrier di dalam render pass dilebarkan dari `0x07` menjadi `0xF7` (`ORDER_PRIMITIVES 0x01 | ORDER_FRAGMENTS 0x02 | ORDER_FRAGMENTS_TILED 0x04 | INVALIDATE_TEXTURE 0x10 | INVALIDATE_SHADER 0x20 | INVALIDATE_TEXTURE_DESCRIPTOR 0x40 | INVALIDATE_ZCULL 0x80`).
   Ketika `g_order_check` di `prepare_draw()` mendapati `!g_pass_wmem && !order_feedback(3)`, barrier `INVALIDATE_TEXTURE` / `INVALIDATE_SHADER` / `INVALIDATE_TEXTURE_DESCRIPTOR` diabaikan total tanpa `vk_end_pass()` maupun `vk_img_barrier_all()`. Pada driver Mali Vulkan 1.1 (Mali-G57 driver 32.1.0, Mali-G52 driver 26.0.0), cache L1 tekstur/LUT/uniform post-process tidak ter-invalidasi sehingga 1 frame dunia 3D berubah oranye-merah terang sementara 2D HUD normal.
2. **Flip-flop `use_stage = 0` ke jalur `memcpy(p->shadow, 64 KB)` pada setiap frame `init` (~tiap 1 detik):**
   Setiap ~0.8–1.4 detik (5–7x per 300 frame), Unity mengalokasikan/mendaur ulang pool sementara 16 MB (`!p->synced`). `begin_segment()` menyetel `need_init = 1` -> `need_full = 1` -> `staged_overlap = 0` -> `use_stage = 0` untuk **seluruh** pool yang sudah ada. Pada frame `init` tersebut, seluruh pool aktif (termasuk 23–33 `hot_blocks` dan pool LUT `1024x32` / uniform lighting) keluar dari jalur 256B diff `s->up` dan melakukan **2x pembacaan tak terkunci 64 KB `memcpy(p->shadow + off, lo + off, 65536)` + `vkFlushMappedMemoryRanges`**, menimpa byte GPU-resident/`vk_ubd_land` dalam blok 64 KB yang sama pada memori non-koheren `HOST_CACHED`.

### Perbaikan
- **`gfx_vk_exec_walker.inc` & `gfx_vk_exec_passes.inc`:** Memperketat mask penahanan `OP_BARRIER` dari `0xF7` -> **`0x87`** (`ORDER_PRIMITIVES | ORDER_FRAGMENTS | ORDER_FRAGMENTS_TILED | INVALIDATE_ZCULL`). `INVALIDATE_TEXTURE (0x10)`, `INVALIDATE_SHADER (0x20)`, `INVALIDATE_TEXTURE_DESCRIPTOR (0x40)`, dan `ORDER_INDIRECT_DATA (0x08)` **tidak pernah ditelan** di dalam pass terbuka (`S.pass_open == 1` wajib `vk_end_pass()` + `vk_img_barrier_all()`, sementara `S.pass_open == 2` memancarkan `vk_img_barrier_all()` tanpa merusak folded `LOAD_OP_CLEAR`).
- **`gfx_vk_exec_draws.inc`:** Saat `nfb > 0` self-feedback memecah pass untuk `vk_tex_feedback_snapshot()`, `g_order_check` direset bersih.
- **`gfx_vk_core.c` (`vk_pools_sync_`):** `use_stage = safe_sync || staged_overlap || vk_pool_overlap()` dan `use_diff256 = !safe_sync && use_stage` kini **selalu aktif**, sehingga pool aktif tidak pernah jatuh kembali ke jalur 64 KB `p->shadow` clobber pada frame `need_full`. Semua pool baru `< 1 MiB` maupun `>= 1 MiB` di-stage bersih lewat `vk_scratch()` -> `s->up` `vkCmdCopyBuffer`.

---

## 2. Bug #2 — Kedip Hitam 3D Saat Transisi Scene (`video_1.mp4`, `858x482`, `disable_msaa=1`)

### Bukti & Akar Penyebab Exact
1. **Spekulatif `STORE_OP_DONT_CARE` pada depth `samples=1` di `OP_END` (`gfx_vk_exec_passes.inc`):**
   Log `hybrid19` mencatat `[vk] TBDR DiscardDepthStencil -> STORE_OP_DONT_CARE active (samples=1 858x482)` (`store_dc=1901`). Setiap `NVNcommandHandle` dalam 1 frame multi-CB Unity diakhiri `OP_END`. Ketika CB #1 (pass 3D utama `samples=1`) mencapai `OP_END`, `pass_apply_discard_lookahead()` mengira depth buffer tidak dipakai lagi dan menyetel `STORE_OP_DONT_CARE` — lalu CB #2 pada frame yang sama melakukan `LOAD_OP_LOAD` terhadap depth buffer tersebut -> seluruh depth test gagal -> area 3D berubah hitam polos sementara 2D HUD tetap tampil normal.
2. **Guard `dc_folded` (`pass_apply_fullscreen_load_opt`) pada `render_scale=0.67`:**
   Pemeriksaan viewport sebelumnya tidak menghitung batas `min/max` untuk `vw`/`vh` negatif, tidak memverifikasi unscaled guest scissor (`S.sc`) sebelum pembulatan skala `0.67`, dan tidak menolak shader dengan binding tekstur bindless/UBO (`RK_TEX_UBO`/`RK_TEXD`/`RK_IMG`) maupun `S.poly.mode != 0` / A2C.
3. **`vk_tex_unscale()` membuang piksel render target `858x482 -> 1280x720` menjadi `LAYOUT_UNDEFINED` (hitam):**
   Saat `copy_b2t` (termasuk `CopyTextureToBuffer` / transisi scene) memanggil `vk_tex_unscale()` pada target 3D `858x482` yang sudah dirender GPU, `fresh->img` dibuat baru dalam keadaan `LAYOUT_UNDEFINED` tanpa menyalin isi piksel sebelumnya.

### Perbaikan
- **`gfx_vk_exec_passes.inc` (`pass_apply_discard_lookahead`):** Menghapus spekulasi `OP_END` discard untuk depth `samples == 1`. `STORE_OP_DONT_CARE` kini hanya aktif pada: (a) eksplisit `OP_DISCARD_DS`/`OP_DISCARD_COLOR`, (b) terbukti di-`OP_CLEAR_DS` full-rect pada pass berikutnya dalam CB yang sama sebelum draw/copy/dispatch/call, atau (c) depth MSAA (`samples > 1`) tak tersampel di `OP_END`.
- **`gfx_vk_exec_passes.inc` (`pass_apply_fullscreen_load_opt`):** Menambahkan validasi ketat batas viewport `[min(v, v+w), max(v, v+w)]`, validasi unscaled guest scissor, `S.poly.mode == 0`, no A2C, dan penolakan shader `RK_TEX_UBO`/`RK_TEXD`/`RK_IMG`.
- **`gfx_vk_tex.c` (`vk_tex_unscale_cb`) & `gfx_vk_exec_copies.inc`:** Mempertahankan isi piksel warna target GPU aktif via `vkCmdBlitImage(old -> fresh)` dengan barrier pre/post sebelum menukar handle image `858x482 -> 1280x720`.

---

## 3. Bug #3 — Stutter Periodik 50–66 ms (Pool 16 MB Tiap ~1 Detik), `rb_staged=300/300`, Barrier GPU & Optimasi Mid/Low-End

### Akar Penyebab & Perbaikan
1. **Siklus 16 MB `full copies` + `init` Overlap-Skip (`pool_upload 11–15 ms` + `fence_wait 25–48 ms`):**
   - `begin_segment()` sebelumnya menyetel `need_init = 1` whenever `!p->synced`, memaksa `mtl_wait(g_fence)` (drain penuh GPU 25–48 ms) padahal `VkBuffer` baru (`vkCreateBuffer`) maupun hasil daur ulang `cpf_take()` (`fence <= vk_fence_done()`) **tidak sedang dipakai GPU Frame N-1**.
   - **Fix (`gfx_vk_core.c`):** Eager resolve `p->span = nvn_dirty_span(mem, size)` di `pool_add_one()` dan menghilangkan syarat `!g_pool[i].synced` dari `need_init` di `begin_segment()` (hanya pool untracked `> 16 MiB` yang masih drain).
   - **Zero-page-fault baseline (`cpf_put` + `baseline_fresh`) & NEON `mem_nonzero64`:** `cpf_put()` kini membebaskan `p->baseline` (O(1) `munmap`, menghemat 16–128 MB RAM) sehingga `calloc(1, 16 MB)` berikutnya menghasilkan halaman zero demand-paged (`p->baseline_fresh = 1`) — blok nol tidak lagi menyentuh `p->baseline` (menghapus 4.096 page fault / ~8–10 ms `memset` per 16 MB pool!). Scan `mem_nonzero64()` diakselerasi ARM64 NEON 64-byte/iterasi (`vld1q_u64`/`vorrq_u64`).
2. **`rb_staged = 300/300` (`pool_readback = 0.8–1.26 ms`, spike `3.94 ms` + `ALL_COMMANDS` GPU stall tiap frame):**
   - Di `hybrid19`, `vk_gpu_wrote_ssbo()` memaksa readback pada compute SSBO `<= 512 KB` di pool `CPU_UNCACHED` (`flags & 0x2`).
   - **Fix (`gfx_vk_core.c`):** Menghapus `|| n <= (512u << 10)` untuk `CPU_UNCACHED` (tetap dilindungi `p->gpu_blk[b] = 1` tanpa readback, sementara `CPU_CACHED` tetap readback penuh).
3. **Penyempitan Scope Barrier GPU (`s->up`):**
   - Menghilangkan `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` dari `vk_pre_transfer_barrier()` dan `vk_post_transfer_barrier()` (hanya menarget `pool_consumer_stages()` tanpa meng-stall `COLOR_ATTACHMENT_OUTPUT` / `EARLY_LATE_FRAGMENT_TESTS`), serta menghapus 4 barrier duplikat per frame (`EMIT_POOL_PRE_BAR`, per-pool `ALL_COMMANDS`, `vk_img_barrier_all(s->up)`, dan duplikat `vk_post_transfer_barrier(s->main)`).
4. **Percepatan `replay` (`descriptor sets` / `collecting` di Cortex-A55/A76):**
   - Memperbesar `MEMO_WAYS` dari `8 -> 64` dan memangkas `memset`/`memcpy` di `drv_segment()`/`drv_shared()` hanya ke prefix aktif (`0x648` / `0x968`).
5. **Preset `45 FPS` Mid-Range (Mali-G57 / Adreno 619):**
   - Menambahkan dukungan `DM_FPS_CEILING=45` (`22.22 ms`) di `gfx_vk_present.c`, `perf_android.c`, `RuntimeConfigManager.java`, dan `LauncherActivity.java` bersama preset `Auto`, `60 FPS`, `30 FPS Stabil & Adem`, serta `Render Scale 0.55x / 0.67x / 0.75x / 1.00x`.

---

## 4. Hasil Build & Verifikasi Offline (`v1.1.2-hybrid20`)

> **Transparansi:** Seluruh verifikasi di bawah dilakukan secara offline/host-side; pengujian perangkat fisik Android menunggu instalasi APK oleh pengguna.

- **`libdmhost.so` (`arm64-v8a`, 16 KB `PT_LOAD` aligned):** `2245 KB`, SHA-256 `bebc8bbfe35ffb374b093948a19cd7c91900b1c66670c1e9839f0b6ca8161b16`
- **Unified Patch:** `hybrid_port/patches/nx2android_unified_mali_adreno.patch` (`4549` baris vs `de6ddd8`)
- **Signed Release APK (`versionCode = 20`, `versionName = '1.1.2-hybrid20'`):** `hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid20-arm64-release-signed.apk` (`31 MB`), SHA-256 `56942f3546c9cd7f3fcc865fb9c0e6dba5c5a68d6140035ff5bc29eba075739f`
- **Offline Verification (`verify_hybrid_apk.sh`):** **7/7 PASS** (`zipalign -c -P 16 4` OK di `114688`/`2424832`, `apksigner verify` v3 OK, 0 file terlarang, `shaders.dmsv` & `.nxpatch` Stored `0%`, seluruh kelas/metode JNI utuh pasca-R8)
