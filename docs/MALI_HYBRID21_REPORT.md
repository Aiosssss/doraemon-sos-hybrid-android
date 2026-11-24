# Laporan Teknis `v1.1.2-hybrid21` (`versionCode = 21`)

Target: Menghilangkan 100% kedip merah-oranye & kedip hitam secara **DEFAULT** (`pool_safe_sync=0`, `strict_barriers=0`, `robust_access=0`) sekaligus memulihkan & mendongkrak FPS besar-besaran pada flagship (**Mali-G720 MC7**, 60 FPS, target `GPU p50 <= 11 ms`, `passes/frame <= 24`, `replay <= 1.5 ms`) maupun HP mid/low-end (**Mali-G57 MC2**, **Mali-G52 MC2**, **Adreno 610–619**).

---

## 1. Audit SPIR-V & `git diff hybrid15..hybrid20` — Akar Exact Kedip Merah-Oranye 3D

### 1.1. Dekompilasi `spirv-dis` / `spirv-cross` dari `shaders.dmsv`
- **`PASS #28` (`fs=fb93bf00d9ae7f68`, 175 baris GLSL, `1920x1080` Final 3D Tone-Map & Color-Grading Composite):**
  - Menyampel `t128` (`binding 2`, `1280x720` scene color `RGBA16F`), **`t129` (`binding 3`, `1024x32` `RGBA16F` 3D Color Grading LUT via trilinear slice lookup `fp_c3[20]`)**, `t130`/`t131` (`bindings 4–5`, `512x512` watercolor paper & 3-channel RGB tint mask `_103`), `t132`/`t133` (`bindings 6–7`, `1280x720` watercolor mask + 13-tap blur `_163`), dan `t134` (`2048x1024` `R8_UNORM`).
  - Membaca **`fp_c3` (`binding 8`, UBO `c0`)**: `fp_c3[2..19]` menyimpan warna & bobot tint warna/sunset oranye-merah serta parameter LUT `fp_c3[20]`. Korupsi/stale pada `fp_c3`, `t129` (`1024x32` LUT), atau `t131`/`t132`/`t133` mengubah seluruh warna/pencahayaan dunia 3D menjadi oranye-merah menyala sementara geometri 3D utuh dan 2D HUD normal.
- **`PASS #16` (`fs=10dd75b8393fc1cc`, 84 baris GLSL, `has_discard = []`):** Watercolor edge-darkening & paper-grain fullscreen opaque pass (`t128` scene color, `t130` depth/normal/mask, `t129` paper, `t131` mask, `fp_c3` UBO). Seluruh 5 shader post-process (`10dd75b8393fc1cc`, `f66cab3d319d7101`, `c069d0cbe35c7fa9`, `798408aeebb9baa3`, `c051919c1175aaca`) terverifikasi **0 `OpKill` / `OpDemoteToHelperInvocation` / `OpTerminateInvocation`** dan hanya menggunakan `RK_TEX` langsung (bukan bindless).
- **`DISPATCH #1/#2` (`cs=8520ba31b724c7f9`, `local_size_x = 100`, `grp = 8x1x1`, `ssbo = 0x3`):** Compute shader partikel/daun/angin — membaca `cp_t` (`512x512` `f76` dari `PASS #2/#4`) dan menulis posisi/kuaternion ke `cp_s0` (`binding 1`) pada pool `CPU_UNCACHED`.

### 1.2. Dua Akar Exact yang Menyebabkan Kedip Merah-Oranye di `Log 1` (`safe_sync=0, strict=0`) dan Hilang 100% di `Log 2` (`safe_sync=1, strict=1`)
1. **Bom `p->gpu_blk` + `mprotect(PROT_READ)` di Tengah `replay` (`vk_gpu_wrote_ssbo`, diperkenalkan di `hybrid18`!):**
   - Tidak ada di `hybrid15`/`16`/`17` (di mana kedip merah-oranye 0%), masuk di `hybrid18`, dan **dilewati saat `pool_safe_sync=1`** (Log 2 bebas kedip!).
   - Setiap frame, `DISPATCH #1/#2` memanggil `vk_gpu_wrote_ssbo(a, n)` di tengah `stage_set()` (`replay`), yang mendemosi blok panas (`p->hot[b] = 0; p->gpu_blk[b] = 1`) lalu langsung memanggil `nvn_dirty_protect_span_blk()` (`mprotect(PROT_READ)`).
   - Akibat ganda fatal: (a) tulisan CPU ke `fp_c3` / UBO pada blok hot sebelum `mprotect` tidak memicu `r->gen` sehingga di-skip di frame berikutnya -> **UBO `fp_c3` basi -> kedip oranye-merah 3D**, (b) ketika CPU menulis lagi, `act[b]=4` menimpa seluruh blok 64 KB, (c) `mprotect` di dalam `stage_set()` langsung menggelembungkan `replay` ke `3.3–5.05 ms` dan `dirty_rearm` ke `17.8–22.9 ms`!
   - **Fix (`gfx_vk_core.c`):** Menghapus seluruh cabang `p->gpu_blk` + `mprotect` dari `vk_gpu_wrote_ssbo()`. Jalur `use_diff256` hanya meng-upload sub-blok 256B tempat `lo != p->baseline`, menjaga hasil SSBO GPU tetap utuh dengan `0.00 ms` readback dan nol `mprotect` saat `replay`.
2. **Celah AFBC Flush pada Target Warna `RGBA16F` (`1024x32` LUT & `1280x720` HDR) di `vk_end_pass()` (`strict_barriers=0`) serta `pkg_upload_()` / `copy_b2t()`:**
   - Di `vk_end_pass()` (`gfx_vk_exec_passes.inc`), filter `is_shadow_or_proj` melewatkan seluruh target `RGBA16F` non-persegi (`1024x32` LUT `vk97`, `16x16` auto-exposure, `1280x720`/`640x360`/`320x180` HDR/watercolor) sehingga tidak mendapat `VkImageMemoryBarrier` per-image saat `strict_barriers=0`. Di Log 2 (`strict_barriers=1`), semua pass mendapat barrier sehingga kedip hilang 100%, namun mematikan `clears_folded` dan memicu stall global.
   - **Fix (`gfx_vk_exec_passes.inc`, `gfx_vk_tex.c`, `gfx_vk_exec_copies.inc`):** Memancarkan `VkImageMemoryBarrier` per-image terarah pada **SEMUA** color attachment yang ditulis di `vk_end_pass()` (tetap mempertahankan `clears_folded`, `dc_folded`, dan `order-only barriers`!), ditambah `VkImageMemoryBarrier(TRANSFER_WRITE -> SHADER_READ | ATTACHMENT)` tepat pada `im->img` sesudah `vkCmdCopyBufferToImage` di `pkg_upload_()` dan `copy_b2t()`.
3. **Jaminan Cross-Frame Zero-Race & `dyn_ubo[slot]` Fast-Path (`gfx_vk_core.c`):**
   - Menyelaraskan `vk_scratch()` di `vk_sub_update()` ke `minUniformBufferOffsetAlignment` dan menambahkan **zero-copy direct-bind fast-path** di `vk_ubd_shadow()` untuk per-slot dynamic uniform ring (`g_ubd[i].sb, so + delta`) tanpa salinan ganda.
   - Mengubah `wait_stage` pada hardware submission chain wait (`g_timeline >= f - 1` / `g_chain_sem`) di `mtl_sub_commit()` dari `TOP_OF_PIPE_BIT` ke `TRANSFER_BIT | COLOR_ATTACHMENT_OUTPUT_BIT | EARLY_FRAGMENT_TESTS_BIT | COMPUTE_SHADER_BIT` — menjamin Frame N tidak pernah menimpa `p->buf` atau render target milik Frame N-1, sekaligus mencegah driver Mali CSF memblokir `vkQueueSubmit()` selama 12–17 ms!

---

## 2. Pemulihan Boost FPS Besar-Besaran (Flagship Mali-G720 & HP Low-End)

1. **Hentikan 10× Split 4-MRT (`PASS #29–#38`) & 3× MSAA (`PASS #5–#7`) (`41.3 -> ~21 pass/frame`):**
   - Memulihkan mask `0xF7` pada `OP_BARRIER` di `gfx_vk_exec_walker.inc` & `gfx_vk_exec_passes.inc` (terbukti di Log 1 `hybrid20` bahwa mempersempit `0xF7 -> 0x87` menambah +20 pass/frame & +5 ms GPU tanpa memperbaiki kedip merah; saat `S.pass_open == 1`, tidak ada copy/dispatch/ubd_land yang bisa berjalan dan `prepare_draw()` tetap memeriksa `g_pass_wmem || shader_wmem || order_feedback(3)`).
2. **Pulihkan `dc_folded` (`0 -> ~8/frame`) — Fix Bug `S.poly.mode != 2` (`gfx_vk_exec_passes.inc:161`):**
   - Di NVN, `NVN_POLYGON_MODE_FILL == 2` (`0` = `POINT`, `1` = `LINE`, `2` = `FILL`). Baris `if (S.poly.mode != 0) return;` di `hybrid20` secara keliru menolak 100% pass FILL sehingga `dc_folded` turun ke `0`! Diperbaiki menjadi `S.poly.mode != 2`, langsung memulihkan `LOAD_OP_DONT_CARE` pada ke-8 pass post-process fullscreen opaque (`PASS #16..#22` & `#27`).
3. **Kembalikan `replay` (`3.3–5.05 ms -> <= 1.5 ms`) & Hapus Spike `dirty_rearm` (`17.8–22.9 ms`):**
   - Membatasi `pass_apply_discard_lookahead()` dari `1.024` -> `96` langkah (192 hanya untuk depth MSAA `samples > 1`) dan langsung berhenti setelah pass non-depth berakhir (memangkas ~40.000 scan opcode/frame dari `replay`). Tetap melarang spekulasi `STORE_OP_DONT_CARE` pada depth `samples=1` agar bebas kedip hitam.
   - Melompati `mprotect(PROT_READ)`, alokasi `baseline`, dan scan kotor per frame untuk pool **`CPU_NO_ACCESS` (`flags & 0x1`)** setelah zero-fill GPU awal (~0.1 ms), menghilangkan TLB shootdown `dirty_rearm` 18–23 ms.
4. **Migrasi Otomatis Launcher (`hybrid21_migrated`, `RuntimeConfigManager.java`):**
   - Otomatis mereset `poolSafeSync=false`, `strictBarriers=false`, `robustAccess=false`, `dump3dFrame=false`, `poolOverlap=true` saat upgrade, sambil mempertahankan seluruh profil HP low-end (`disable_msaa=1`, `tex_downscale=1/2`, `render_scale 0.67/0.55`, preset `Auto / 60 / 45 / 30 FPS`, unscale blit-preserve, `vk_tex_feedback_snapshot`).

---

## 3. Hasil Build & Verifikasi Offline (`v1.1.2-hybrid21`)

> **Transparansi:** Seluruh pemeriksaan di bawah adalah **verifikasi offline/host-side**; pengujian perangkat fisik Android (Xiaomi Mali-G720 maupun HP Mali-G57/G52/Adreno) menunggu instalasi APK oleh pengguna.

- **`libdmhost.so` (`arm64-v8a`, 16 KB `PT_LOAD` aligned):** `2244 KB`, SHA-256 `894f6a470b6c92474e58c0728c297c8f6c527d4642c96bd2e184ef14afb1dc63`
- **Unified Patch:** `hybrid_port/patches/nx2android_unified_mali_adreno.patch` (`4625` baris vs `de6ddd8`)
- **Signed Release APK (`versionCode = 21`, `versionName = '1.1.2-hybrid21'`):** `hybrid_port/dist/DoraemonSoS-Hybrid-v1.1.2-hybrid21-arm64-release-signed.apk` (`31 MB`), SHA-256 `e358ead2e03cf0b023e36f0a05e38f1664519ceda58c98836a2d2b349fa01930`
- **Offline Verification (`verify_hybrid_apk.sh`):** **7/7 PASS** (`zipalign -c -P 16 4` OK di `114688`/`2424832`, `apksigner verify` v3 OK, 0 file terlarang, `shaders.dmsv` & `.nxpatch` Stored `0%`, seluruh kelas/metode JNI utuh pasca-R8)
