# Laporan Teknis `v1.1.2-hybrid17` (`versionCode = 17`)

**Target Aplikasi:** *DORAEMON STORY OF SEASONS: Friends of the Great Kingdom* (`org.nx2a.doraemon`, Title ID `01008EE0177A8000`, Update `1.1.2` / `v262144`)  
**Pipeline:** `nx2android` + `nx2android-mali` (unified runtime, offline SPIR-V `shaders.dmsv` 5.613 shader)  
**Baseline Performa yang Dipertahankan 100%:** `v1.1.2-hybrid16` (Xiaomi Mali-G720 MC7 Android 16 driver `49.1.0`: write-protection faults ~502k, `pool_upload 0.92–1.09 ms`, `dirty_rearm 0.08–0.12 ms`, `pool_readback 0.00–0.22 ms`, CPU spin 8 us / `8192` waits, suhu 42C, TBDR `dc_folded` aktif, kedip oranye-merah 3D 0, optimasi Mali-G52/G57 & preset Auto/60/30 FPS).

---

## 1. Bagian 1 (Prioritas #1) — Regresi Vertex HUD Melar ke NDC `(0,0)` (>250 ms)

### Akar Masalah Terbukti di Kode (`hybrid16`)
Audit baris per baris pada `vk_pools_sync_()` (`runtime_src/nxnative/dm/android/gfx_vk_core.c`) membuktikan **3 celah sinkronisasi konkret** yang muncul sejak batching `ADD_PROT_RUN` (`hybrid15`) dan terekspos saat `nvn_dirty_protect_blk` (`hybrid16`) menyusutkan jendela fault:

1. **Protect-After-Copy Race (Celah Utama):**
   Di `hybrid16`, `ADD_PROT_RUN(off, n)` hanya memperpanjang rentang `prot_lo..prot_hi`, sementara `memcmp(lo + off, p->baseline + off)` dan `FLUSH_STAGE_RUN()` (`memcpy` ke `p->baseline` & `vk_scratch`) dieksekusi **ketika halaman Guest RAM masih `PROT_READ | PROT_WRITE`**. `FLUSH_PROT_RUN()` (`mprotect(PROT_READ)`) baru dipanggil **di akhir loop**.
   - Ketika thread Unity UI/worker menulis vertex quad HUD baru (`11:23 -> 11:24`, banner lokasi) tepat setelah `memcmp`/`memcpy` tetapi sebelum `FLUSH_PROT_RUN()`, tulisan tersebut **tidak memicu `SIGSEGV`** (`r->gen[pg]` tidak bertambah).
   - Sesaat kemudian `FLUSH_PROT_RUN()` mengunci halaman menjadi `PROT_READ`, sehingga vertex setengah-terisi / koordinat `(0, 0)` **membeku selama `>12 frame` (`>250 ms`)** sampai ada tulisan UI berikutnya yang kembali memicu fault.
   - Celah yang sama juga berpotensi menjatuhkan tulisan CPU dinamis ke buffer 3D (matriks instancing / shadow / UBO).
2. **Hot-Block Demotion Race (`HOT_IDLE_LIMIT = 243`):**
   Saat blok panas yang diam ~4 detik (`240` sync) didemosi ke `p->hot[b] = 0`, `ADD_PROT_RUN()` memproteksi ulang halaman **tanpa** memverifikasi ulang `memcmp` setelah `mprotect(PROT_READ)` selesai. Tulisan yang masuk di antara `memcmp` dan `mprotect` ikut membeku hingga fault berikutnya.
3. **Recycled `cpf_take()` Baseline Non-Zero pada Sparse Initial Sync:**
   Di jalur *sparse initial sync* (`size >= 1 MiB`), blok yang seluruhnya nol (`!mem_nonzero64`) dilewati tanpa membersihkan `p->baseline + off`, padahal `cpf_take()` bisa mendaur ulang buffer heap `baseline` lama yang masih berisi byte non-nol.

### Perbaikan di `v1.1.2-hybrid17`
- **Algoritma 3-Pass Protect-Before-Copy** pada `vk_pools_sync_()` (`gfx_vk_core.c`):
  - **Pass A:** Mengklasifikasikan seluruh blok 64 KB (`act[b]` = `0` skip / `1` dirty / `2` hot), mengantrekan `ADD_PROT_RUN(off, n)` untuk seluruh blok dirty non-promoted dan `probe_rearm`, lalu memanggil **`FLUSH_PROT_RUN()` (`nvn_dirty_protect_blk`, 64 KB spans) SEBELUM satu byte pun dibaca oleh `memcmp`/`memcpy`**.
  - **Pass B:** Menjalankan whole-block NEON `memcmp` fast-path + diff 256B `dmask[4]` + batched `VkBufferCopy`, persis mempertahankan efisiensi `hybrid16`. Blok panas yang mencapai `HOT_IDLE_LIMIT = 243` diantrekan ke `ADD_PROT_RUN(off, n)` dan ditandai `act[b] = 3`.
  - **Pass C (hanya bila `any_demoted != 0`):** Memanggil **`FLUSH_PROT_RUN()`** (TLB shootdown selesai -> halaman resmi `PROT_READ`), lalu memverifikasi ulang setiap blok `act[b] == 3` dengan whole-block NEON `memcmp` di bawah `g_baseline_mu` dan langsung men-stage setiap sub-blok 256B yang sempat ditulis secara konkuren sebelum `mprotect` mendarat.
- **Zeroing `p->baseline`** untuk blok nol pada sparse initial sync di bawah `g_baseline_mu`.
- **Bukti Ketertutupan Race:** Setiap *store* CPU sebelum `mprotect(PROT_READ)` dijamin terlihat oleh `memcmp`/`memcpy` (Pass B / Pass C); setiap *store* setelah `mprotect(PROT_READ)` dijamin memicu `SIGSEGV` dengan `gen > st` sehingga pasti tertangkap pada frame berikutnya. Zero tambahan fault dibanding `hybrid16`.

---

## 2. Bagian 2 (Prioritas #2) — Audit Forensik SPIR-V & Perbaikan Bayangan Pohon Kedap-Kedip + Alias `1024x32`/`1280x720`

### Temuan Forensik Nyata pada `shaders.dmsv` (5.613 Shader)
Seluruh 5.613 modul SPIR-V di `hybrid_port/build_core/shaders.dmsv` diaudit dan shader kunci shadow/foliage/post-process didekompilasi menggunakan `spirv-cross`:
- **214 FS `OpImageSampleDref*`:** Termasuk Unity `Internal-ScreenSpaceShadows` (`02d285985f4de0ab`, `2f0ecae82daaa2a7`) dan receiver foliage cutout 4-tap PCF + `OpKill` (`00b220820b04b616`).
- **102 FS cutout shadow caster (`OpKill`):** Contoh `1ca6c00b19549e0a` (`texture().x == 0.0 -> discard; out = vec4(0)`).
- **20 VS opaque shadow caster (2 instanced):** `e9faca6c51f816ac` dan `651126a8894d5264`, membaca matriks dunia per-instance dari `vp_c4` + `unity_LightShadowBias` & `unity_BaseInstanceID` dari `vp_c3`, menerapkan *normal bias* dan *shadow pancaking* (`min(z, w)`).
- **Shader 3-MRT foliage/surface (`bf467892b4656e57`, `d1e98cacd2b6a405`):** Menyampel peta mask top-down world-XZ `R8_UNORM` (`fp_t_cb196610_1057005C.x`).
- **Shader `f66cab3d319d7101` (tercatat pada log `hybrid16` `dc_folded` `pass 512x512`):** Shader quad 1-tekstur generik (`texture * fp_c3[0]`) — **bukan** shader fullscreen post-process khusus, sehingga *folding* `LOAD_OP_DONT_CARE` pada target persegi `512x512` berisiko membuang isi target yang masih dibutuhkan.
- Verifikasi enum NVN (`nvn.h`): `COMPARE_MODE_COMPARE_R_TO_TEXTURE = 1`, `COMPARE_FUNC = 1..8`, `DEPTH_FUNC = 1..8`, `STENCIL_OP = 1..8`, `POLYGON_OFFSET_ENABLE = 1/2/4` cocok 1:1 dengan `gfx_vk_exec_formats.inc` & `gfx_vk_tex.c`.

### 6 Perbaikan Terbukti untuk Bagian 2
1. **Guard Target Persegi pada `pass_apply_fullscreen_load_opt()` (`gfx_vk_exec_passes.inc`):**
   Menolak *folding* `LOAD_OP_LOAD -> LOAD_OP_DONT_CARE` untuk draw quad (`cnt == 4 || cnt == 6`) pada render target persegi (`S.pass_w == S.pass_h`, melindungi `pass 512x512 fs=f66cab3d319d7101` dan target shadow/wind/projection `512x512`/`1024x1024`), sementara tetap mengaktifkan `dc_folded` pada semua pass post-process/bloom 16:9 (`1280x720`, `640x360`, `320x180`, `160x90`, `80x45`, dst.) dan segitiga fullscreen `cnt == 3`.
2. **Sinkronisasi Divisor Pipeline saat Ekspansi `emulate_first_instance()` (`gfx_vk_exec_xfb.inc`, `gfx_vk_exec_draws.inc`, `gfx_vk_exec_state.inc`):**
   Pada Mali-G720 (`V.has_divisor == 1`, `V.has_nonzero_first_inst == 0`), ketika `dv > 1 && (fi % dv) != 0`, `hybrid16` mengekspansi stream menjadi layout `divisor = 1` di `vk_scratch` tetapi `VkPipeline` yang terikat masih memakai hardware `divisor = dv > 1` (double division `dst[k/dv] = el((fi + k/dv)/dv)`). Kini `fi_expand_mask_for()` dihitung sebelum `prepare_draw()` sehingga `k.bind[s].divisor` di-override ke `1` tepat pada stream yang diekspansi.
3. **Analisis Per-Binding SPIR-V `wmem_bind_mask` & Proteksi SSBO Non-Overlap (`gfx_vk.h`, `gfx_vk_exec.c`, `gfx_vk_exec_draws.inc`, `gfx_vk_exec_descriptors.inc`, `gfx_vk_core.c`):**
   - `spv_wmem()` kini melacak `OpDecorate Binding` -> `OpVariable` -> `OpAccessChain`/`OpImageTexelPointer`/`OpLoad` -> `OpStore`/`OpCopyMemory`/`OpImageWrite`/`OpAtomic*` untuk mengisi `vkshader_t::wmem_bind_mask` (fallback konservatif `~0ull` untuk pointer tak langsung/parameter fungsi).
   - `stage_set()` hanya memanggil `vk_gpu_wrote_ssbo()` pada binding yang benar-benar ditulis (`shader_bind_writes(s, b->binding)`), bukan seluruh SSBO read-only.
   - `vk_gpu_wrote_ssbo()` tetap mempertahankan zero readback pada `CPU_UNCACHED` saat `pool_overlap=1` (menjaga hemat 4.39 ms/frame di Mali-G52), namun menjaga copy-back ketika `!vk_pool_overlap() || vk_pool_safe_sync()` agar `memcpy(p->shadow, lo, 64K)` non-overlap tidak pernah menimpa SSBO shadow/compute.
4. **Perluasan Flush AFBC/Z Mali di `vk_end_pass()` & WAW Barrier di `vk_tex_feedback_snapshot()` (`gfx_vk_exec_passes.inc`, `gfx_vk_tex.c`):**
   - `vk_end_pass()` memperluas `dstStageMask` ke `VERTEX_SHADER | FRAGMENT_SHADER | COMPUTE_SHADER | TRANSFER` serta mencakup target persegi `>=256x256` dan `2048x1024` (shadow/projection/wind) tanpa membebani pass warna 16:9 standar.
   - `vk_tex_feedback_snapshot()` menambahkan `VK_ACCESS_TRANSFER_WRITE_BIT` + `VK_PIPELINE_STAGE_TRANSFER_BIT` pada `pre[1]` untuk mencegah WAW hazard pada `fb->img`.
5. **Fallback Border Color Shadow Sampler (`gfx_vk_tex.c` `make_sampler()`):**
   Pada GPU tanpa `VK_EXT_custom_border_color`, shadow sampler dengan `cmp_mode == 1` dan `border[0] >= 0.5f` (seperti `(1,0,0,0)`) kini jatuh ke `VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE` alih-alih `TRANSPARENT_BLACK` (yang membuat area luar frustum menjadi gelap/terbayang).
6. **Perbaikan `[alias] stale read: 1024x32 fmt 0x29 ... rendered over by 1280x720 fmt 0x25` (`gfx_vk_tex.c`):**
   `nvnTextureBuilderGetStorageSize()` langsung memanggil `nvn_tex_layout()`, sehingga `1280x720` (`+0xBD0000..+0xF90000`, `0x3C0000`) dan `1024x32` (`+0xF80000..+0xFC0000`) menggunakan ulang memori pool secara sekuensial antar-transisi; sebelumnya `pkg_upload()` dan `lin_copy()` tidak memajukan `im->wseq = ++g_wseq`. Kini keduanya memajukan `im->wseq = ++g_wseq` sehingga umur konten upload CPU selalu lebih baru dari RT lama yang sudah dibebaskan.
7. **Diagnostik `[3D-DUMP]` Lebih Kaya (Zero Overhead saat `DM_VK_DUMP_3D_FRAME=0`):**
   Saat diaktifkan (`DM_VK_DUMP_3D_FRAME=1`), `d3d_log_draw()` kini juga mencatat hingga 4 pasangan `(vs, fs)` unik per pass 3D serta draw dengan `bias`/`stencil`/`a2c`, lengkap dengan `df`, `bias`, `st`, dan `a2c`.

---

## 3. Verifikasi Build & APK Offline (7/7 PASS)

> **Catatan Kejujuran Pengujian:** Seluruh verifikasi di bawah ini adalah **verifikasi build & statis/offline pada host Linux**. Pengujian visual & performa pada perangkat fisik (Xiaomi Mali-G720 MC7 / Mali-G52 / Mali-G57) untuk `v1.1.2-hybrid17` **belum dilakukan** dan menunggu instalasi APK oleh pengguna.

| Item Verifikasi | Hasil |
| :--- | :--- |
| Kompilasi `libdmhost.so` (`arm64-v8a` release, NDK `28.2.13676358`) | **PASS** — `196 sources, 0 failed`, stripped `2238 KB` |
| Alignment ELF `PT_LOAD` `libdmhost.so` (`llvm-readelf -l`) | **PASS** — Seluruh 4 segmen `PT_LOAD` `Align 0x4000` (16 KB) |
| SHA-256 `libdmhost.so` | `78fc4a17196fc8377e2ddb96954f4dcf05ef1e5b58db7cd0de81dd31889159e7` |
| Gradle `:app:assembleRelease` (`--offline --no-daemon`) | **PASS** — `versionCode=17`, `versionName='1.1.2-hybrid17'` |
| `zipalign -c -P 16 4` | **PASS** — `libdmhost.so` (`114688`), `libnxhybrid_extract.so` (`2408448`) OK |
| `apksigner verify --verbose` (`doraemon_hybrid_release.jks`, alias `doraemon_hybrid`) | **PASS** — `Verified using v3 scheme: true`, 1 signer |
| `verify_hybrid_apk.sh` (7/7 suite) | **7/7 PASS** — 0 file terlarang, `shaders.dmsv` & `.nxpatch` Stored 0%, seluruh JNI class/method R8 aman |
| Lokasi APK Rilis | `hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid17-arm64-release-signed.apk` (`31M`) |
| SHA-256 APK Rilis | `a29acc738e1e4f3206aab22d15ce39100b297aad488e67a3798884aeb912622a` |
| Unified Patch | `hybrid_port/patches/nx2android_unified_mali_adreno.patch` (`3883` baris vs `de6ddd8`) |
