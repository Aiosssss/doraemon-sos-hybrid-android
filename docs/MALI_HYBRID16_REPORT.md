# Doraemon SoS Hybrid Port — v1.1.2-hybrid16 (`versionCode = 16`) Technical Report

## Ringkasan Build & Verifikasi Offline
- **APK Rilis:** `hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid16-arm64-release-signed.apk` (`32,352,279` byte / `31 MiB`)
- **APK SHA-256:** `103781183ae2fe15ba68e2f2f6a2720bb51cb9140fa42865b331d8a69bec0a22`
- **`libdmhost.so` (`arm64-v8a`, stripped 2234 KB, `PT_LOAD 0x4000` 16 KB aligned):** `42f5b37edd3c7076fb2de2a3365145004503795224ffd25cc9954afa04cc5d7e`
- **`shaders.dmsv` (v2, 5,613 SPIR-V tervalidasi):** `3315012d0adaa568b2dfd789c9c6ab1ec09c7d3315d87021e707fdb946346f2c`
- **Verifikasi Offline (`verify_hybrid_apk.sh`):** **7/7 PASS** (`zipalign -c -P 16 4` OK, `apksigner verify` v3 OK, 16 KB ELF alignment OK, DEX JNI survival OK).
- **Status Uji Fisik:** Belum diuji pada perangkat fisik; seluruh angka build & verifikasi pada laporan ini adalah hasil kompilasi, audit kode/SPIR-V, dan verifikasi APK offline.

---

## Bagian 1: Perbaikan Regresi Kritis `hybrid15` (Fault Storm 10.3x & GPU +2 ms)

### 1.1. Akar Masalah & Solusi Write-Protection Fault Storm 10.3x (`nvn_dirty.c`, `gfx_vk_core.c`)
- **Akar Masalah #1 — Cascading Multi-MB Span Release di `nvn_dirty_protect()`:**
  Di `hybrid15`, `ADD_PROT_RUN` menggabungkan banyak blok 64 KB bersebelahan (dan initial sync `nvn_dirty_protect(lo, size)` untuk seluruh pool 16 MB) menjadi satu panggilan `nvn_dirty_protect()`, sehingga seluruh halaman diberi tag `r->span[k]` yang sama. Di `nvn_dirty.c:on_fault()`, satu tulis 1 byte ke 1 halaman langsung membuka **seluruh span** (hingga 4.096 halaman / 16 MB sekaligus), menaikkan counter fault 10.3x (`679k -> 6.99M`), memaksa `memcmp` ulang pada puluhan blok bersih, lalu memproteksi ulang menjadi satu span raksasa lagi.
- **Akar Masalah #2 — Cooldown `HOT_IDLE_LIMIT = 19` Terlalu Pendek (0.25 detik):**
  Ring-buffer dinamis Unity baru kembali menyentuh blok 64 KB yang sama setelah 20–120 frame. Mengembalikan `p->hot[b] = 0` + `PROT_READ` setelah 16 frame tanpa perubahan memicu 48 fault SIGSEGV per siklus per blok.
- **Perbaikan:**
  - Menambahkan `nvn_dirty_protect_blk(addr, len, span_blk)` (`nvn_dirty.h`, `nvn_dirty.c`): tetap mengeluarkan **satu `mprotect(PROT_READ)` coalesced** untuk setiap rentang `PG_NONE` berurutan (nol fragmentasi VMA, `dirty_rearm < 1 ms`), tetapi menandai `r->span[k]` **per blok 64 KB** sehingga 1 page fault hanya membuka 1 blok 64 KB (16 halaman), tidak pernah merembet ke tetangga.
  - Menaikkan `HOT_IDLE_LIMIT` dari `19` menjadi `243` (**240 sync berturut-turut / ~4 detik pada 60 FPS**) di `vk_pools_sync_()` (`gfx_vk_core.c`).
  - Menambahkan **fast-path whole-block NEON `memcmp`** sebelum scan sub-blok 256 byte pada blok hot di mode `staged_overlap`, serta menurunkan `STAGE_MAX_RUN` ke `512 KB` untuk mencegah lonjakan slab `vk_scratch` / `vk_submit`.

### 1.2. Aktivasi `pass_apply_fullscreen_load_opt()` & Optimasi `vk_tex_feedback_snapshot()`
- **Akar Masalah:** `pass_apply_fullscreen_load_opt()` (`gfx_vk_exec_passes.inc:144`) menolak semua pass jika `S.ds.dt || S.ds.st` aktif meskipun pass tidak memiliki attachment depth (`dep=none` pada `PASS #2, #11-#16, #21`).
- **Perbaikan:**
  - Mengubah kondisi menjadi `(P_ri.pDepthAttachment && S.ds.dt) || (P_ri.pStencilAttachment && S.ds.st)` agar pass fullscreen opaque post-process tanpa depth aktif otomatis memakai `VK_ATTACHMENT_LOAD_OP_DONT_CARE` (`D/S` di `[3D-DUMP]`).
  - Mengekspor `g_tbdr_fs_load_opt` dan mencetak counter `dc_folded` di log replay per-300 frame (`gfx_vk_present.c`).
  - Mengganti 2x global `vk_img_barrier_all(cb)` di `vk_tex_feedback_snapshot()` (`gfx_vk_tex.c`) menjadi 2x `VkImageMemoryBarrier` spesifik hanya pada `im->img` dan `fb->img`.

---

## Bagian 2: Audit Forensik & Perbaikan Bayangan Pohon Kedap-Kedip

Audit kode C runtime + dekompilasi SPIR-V (`shaders.dmsv`: 2.753 fragment shader, 1.573 `OpKill`, 268 `OpImageSampleDref*`, serta shader pohon/bayangan `bf467892b4656e57` dan `d1e98cacd2b6a405`) menemukan 4 faktor penyebab:
1. **AFBC / Z-Compression Flush pada Depth & Shadow Map R8/R16 (`gfx_vk_exec_passes.inc`):**
   Di `vk_end_pass()` (saat `strict_barriers=0`), tidak ada `VkImageMemoryBarrier` per-image; `begin_pass()` hanya mengeluarkan global `VkMemoryBarrier`. Pada ARM Mali TBDR (Mali-G720 maupun Mali-G52/G57), depth map (`512x512 dep=f129`) dan proyeksi bayangan top-down (`R8_UNORM` `2048x1024/vk9`, disampel `.x` di `bf467892b4656e57` / `d1e98cacd2b6a405`) memerlukan `VkImageMemoryBarrier` eksplisit dari `DEPTH_STENCIL_ATTACHMENT_WRITE` / `COLOR_ATTACHMENT_WRITE` menuju `FRAGMENT_SHADER_BIT` (`SHADER_READ_BIT`). Kini diterapkan secara selektif hanya pada attachment depth dan color target 1-channel (`R8_UNORM`, `R16_SFLOAT`, `R16_UNORM`, `R32_SFLOAT`).
2. **Alpha-to-Coverage (`VQ_NO_A2C`) pada Mali Vulkan 1.3+ 2x MSAA (`gfx_vk_quirks.c`):**
   `bf467892b4656e57` & `d1e98cacd2b6a405` menulis alpha foliage ke `_38.w` tanpa `OpKill`. Memaksa `no_a2c=1` pada Mali modern (driver `>= 40.0`, Vulkan 1.3, 2x MSAA aktif) menjadikan kartu daun semi-transparan 100% opaque coverage dan memicu Z-fighting/kedipan. `VQ_NO_A2C` kini hanya aktif default pada Mali legacy (`api < 1.3 || driver < 40.0`), tetap bisa di-override di Launcher (`DM_VK_NO_A2C`).
3. **`depthBiasClamp` Safety (`gfx_vk.h`, `gfx_vk_core.c`, `gfx_vk_exec_draws.inc`):**
   Menyimpan `V.has_depth_bias_clamp = ff.features.depthBiasClamp` dan meng-clamp argumen clamp `vkCmdSetDepthBias` ke `0.0f` jika fitur tidak tersedia agar state depth-bias shadow caster selalu valid.
4. **Ketepatan Instancing (`gfx_vk_exec_xfb.inc`):**
   - Memperbaiki `vbuf_inst_bound()` agar melewati stream yang sudah diekspansi `emulate_first_instance()` (`skip_mask`).
   - Memanggil `vk_cpu_ready()` sebelum `vk_ubd_read()` pada jalur ekspansi per-instance jika ada GPU write tertunda (`vk_gpu_pending`).
   - Menangani sisa `fi % dv != 0` pada perangkat dengan `V.has_divisor && !V.has_nonzero_first_inst` (Mali-G720) dengan ekspansi ke `vk_scratch` sehingga indeks `(fi + k) / dv` eksak 100%.

---

## Bagian 3: Optimasi HP Kentang & Fix Bug Lintas Perangkat

1. **Mali-G52 MC2 (vivo V2109) — Eliminasi `pool_readback = 4.39 ms/frame` & `emu_first_instance`:**
   - Di `gfx_vk_exec_descriptors.inc:395`, panggilan `vk_gpu_wrote(a, n)` tak bersyarat pada semua SSBO compute (`st == 5`) sebelumnya memaksa staged GPU->CPU readback (`rb_staged=300`) bahkan untuk SSBO read-only dan pool `CPU_UNCACHED` GPU-only (skinning/MassLeaf). Kini diganti `if (st == 5 && b->kind != RK_UBO_PTR && shader_wmem(s)) vk_gpu_wrote_ssbo(a, n);`.
   - `vk_emu_first_instance()` selalu aktif saat `!V.has_divisor`, dan `strict_barriers` tetap `0` (clear folding utuh).
2. **Fix `[low-ram]` Eviction pada `memory tier: 0` (`gfx_vk_tex.c`):**
   - Eviksi agresif 80/128 MiB kini strictly hanya untuk `low_ram >= 1`.
   - Pada `low_ram == 0` (RAM >= 6 GB, termasuk Mali-G52/G57 8 GB), tidak pernah menjalankan `[low-ram]` eviction; hanya ada pengaman `[vram-guard]` jika VRAM tekstur melebihi `600 MiB` (`max_age = 3600` frame, melewati `gpu_w`/`rt_last`/`1024x32` LUT serta tekstur dengan `im->last_use > vk_fence_done()`).
3. **Fix Kedip Hitam 1-Frame Infinix X678B Mali-G57 (`gfx_vk_present.c`):**
   - Menambahkan `VkImageMemoryBarrier` `TRANSFER_WRITE -> TRANSFER_WRITE` di antara `vkCmdClearColorImage(black)` dan `vkCmdBlitImage` (serta skip clear jika full-screen tanpa letterbox) untuk menghilangkan data race WAW yang membuat layar berkedip hitam.
   - Menambahkan transisi layout eksplisit pada `im->img` (`GENERAL -> TRANSFER_SRC_OPTIMAL -> GENERAL`) sebelum/sesudah `vkCmdBlitImage` untuk memastikan AFBC flush di Mali-G57/G52.
   - Memperluas wait stage `g_acq[k]` ke `VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT` dan menambahkan `VkSubpassDependency` eksplisit pada `ov_rp()` (jalur Vulkan 1.1 `!V.has_dynrender`).

---

## Bagian 4: Efisiensi Daya / Termal (Anti-Panas) & Preset Launcher

1. **Pemangkasan CPU Busy-Spin (`hle_thread.c`):**
   - Default `g_spin_us` diturunkan dari `30 us` -> `8 us`, `G_lock()` spin dari `64x16` -> `16x8`, dan backoff `spin_fail` dipercepat (`< 4` dengan stride 32) agar core CPU cepat masuk C-state idle saat tidak ada handoff instan.
2. **ADPF Ramah Termal (`perf_android.c`):**
   - Mengabaikan frame loading/transisi (`GPU < 2 ms` atau `CPU > 60 ms`), menaikkan ambang trigger 60 FPS GPU ke `94%` (`~15.6 ms`) / work `102%`, membutuhkan `5` frame berturut-turut dengan cooldown `2.0 s`.
3. **Preset Mode Performa / Target FPS di Launcher & C Runtime (`dm_pacing.c`, `RuntimeConfigManager.java`, `LauncherActivity.java`, `env.txt`):**
   - **Auto (Default):** 60 FPS pada GPU flagship/mid-high (Mali-G720/G615/G57, Adreno 640+), otomatis **30 FPS Stabil & Adem** + `render_scale=0.67` pada GPU ultra-low-end (`Mali-G52`, `Mali-G51`, `Mali-G31`, `PowerVR`, `Adreno 610` ke bawah).
   - **60 FPS — Performa Penuh:** tetap dapat dipilih manual di semua perangkat.
   - **30 FPS — Stabil & Adem (Battery / Cooler):** mengunci 30 FPS (`DM_FPS_TARGET=30`, `DM_FPS_CEILING=30`, `DM_FPS_HOLD=on`, `DM_PRESENT_INTERVAL=game`) untuk suhu dingin & baterai awet di perangkat mana pun.
   - `DM_VK_DUMP_3D_FRAME=0` dipastikan OFF secara default (termasuk migrasi satu kali `hybrid16_migrated`).
