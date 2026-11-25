# Laporan Teknis `v1.1.2-hybrid35` (`versionCode = 35`)

Dibangun langsung di atas `v1.1.2-hybrid34` tanpa menghapus satu pun fitur `hybrid31/32/33/34` (`Flexible Analog`, `Adaptive Button Level 2`, `Atmosphere Cheat Engine`, `Full Screen 16:9/Stretch/Zoom`, `Xbox Controller`, `Samsung Xclipse Safe Path`, `Classic Vulkan 1.0/1.1 VkRenderPass + VkFramebuffer`, `shaders.dmsv`, `DM_VK_POOL_SAFE_SYNC=2`, `DM_VK_ROBUST_ACCESS=1`, branding `SOS FOTGK`). Path terlindungi (`hybrid_port/l10n_id/`, `Game/`, root `tools/`) tidak disentuh.

---

## 1. Fix 100% Crash `SIGILL` (`sig=4`) pada CPU ARMv8.0-A (Snapdragon 685 / 680 / 665 / 662)

### Akar Masalah Forensik
Crash nyata di Xiaomi `23021RAA2Y` (Redmi Note 12 4G, Snapdragon 685 Kryo 265 / Cortex-A73+A53 ARMv8.0-A):
- `CRASH sig=4 (SIGILL) addr=libdmhost.so+0x1c4198 pc=libdmhost.so+0x1c4198 lr=main+0x15830f0`
- `*x1 = "nvnDeviceBuilderSetDefaults"`, `*x21 = libdmhost.so+0x1c4184`, `x16 -> {0, +0x1c40a4}`, `x17 = 1`.

Inspeksi disassembly `libdmhost.so` lama membuktikan terdapat **554 instruksi ARMv8.1+ LSE** (`537` di `gfx/nvn/nvn_thunks.S`, `1` di `gfx/nvn/nvn_entry.S`, dan `16` compiler-rt outline atomics). Tepat pada offset `+0x14` dari awal setiap NVN trampoline (`_nvnth_<i>`):
```asm
  +0x00: adrp x16, _nvn_slot
  +0x04: add  x16, x16, :lo12:_nvn_slot
  +0x08: mov  x17, #<slot_off>
  +0x0c: add  x16, x16, x17
  +0x10: mov  x17, #1
  +0x14: f831021f  stadd x17, [x16]   // <--- SIGILL di ARMv8.0-A!
```

### Perbaikan
1. **`hybrid_port/runtime_src/nxnative/dm/gfx/nvn/nvn_thunks.S`** & **`hybrid_port/runtime_src/nxnative/dm/gfx/tools/gen_protos.py`**:
   - Menghapus `.arch_extension lse` dan mengganti seluruh 537 `stadd x17, [x16]` menjadi loop LL/SC murni ARMv8.0-A (`ldxr`/`stxr`, memakai scratch register AAPCS64 `x17` & `w9`, menjaga `x16` tetap menunjuk ke `nvn_slot[i]`):
     ```asm
     1:
         ldxr x17, [x16]
         add  x17, x17, #1
         stxr w9, x17, [x16]
         cbnz w9, 1b
     ```
2. **`hybrid_port/runtime_src/nxnative/dm/gfx/nvn/nvn_entry.S`**:
   - Menghapus `.arch_extension lse` dan mengganti `stadd x9, [x10]` pada `_nvn_prof_entry` menjadi LL/SC murni ARMv8.0-A (`ldxr x17, [x10]` / `add x17, x17, x9` / `stxr w16, x17, [x10]` / `cbnz w16, 1b`).
3. **`hybrid_port/runtime_src/nxnative/dm/android/build.py`**:
   - Menambahkan `-march=armv8-a -mno-outline-atomics` ke `CFLAGS`, `VPX_FLAGS`, `ZSTD_FLAGS`, serta `-march=armv8-a` ke seluruh file `.S`.
4. **Verifikasi `llvm-objdump -d` pada Binary Final**:
   - `libdmhost.so` (`arm64-v8a`): **`LSE_CNT=0`** (turun dari `554` -> `0`).
   - `libnxhybrid_extract.so` (`arm64-v8a`): **`LSE_CNT=0`**.

---

## 2. Pengamanan & Preset FPS Adreno 6xx Vulkan 1.1.128 (Adreno 610 / 618 / 619 / 640 / 650)

1. **Akurasi Probe Vulkan & Jalur BC Decode (`nx_jni.c` & `LauncherActivity.java`)**:
   - Saat `bc_feature == false` atau `bc7_sampled == false` (seperti driver stok Adreno `512.502.0`), `nativeProbeVulkan()` kini melaporkan `"Qualcomm Adreno (CPU BC Decode + Cache)"` (bukan lagi `"Native BCn"`), dan ringkasan GPU di Launcher menampilkan `Tex : CPU BC Decode + Cache` vs `Native BCn`.
2. **Audit Jalur tanpa `VK_EXT_transform_feedback` & `VK_KHR_timeline_semaphore`**:
   - Terverifikasi pada `gfx_vk_core.c`, `gfx_vk_exec_passes.inc`, `gfx_vk_exec_xfb.inc`, `gfx_vk_shader.c`, dan `gfx_vk_present.c`: `V.use_classic_fb = 1` menggunakan classic `vkCreateRenderPass` + cached `VkFramebuffer` dengan invalidasi deferred (`vk_classic_fb_drop_view`); `!V.has_xfb` men-strip `OpCapability TransformFeedback` & melewati XFB tanpa crash; `!V.has_timeline` menggunakan fence ring + binary swapchain semaphores.
3. **Klasifikasi Tier & Preset Akurat Adreno 6xx (`gfx_vk_core.c` & `RuntimeConfigManager.java`)**:
   - **Adreno 650 (`0x6050002`) & Adreno 640 (`0x6040001`)**: Tier 1 — `render_scale=0.85`, `shadow_scale=0.75`, `shadow_rate=1` (60Hz), `disable_msaa=1`, `low_cost_pp=0`, `strict_barriers=0`, `tex_downscale=1`.
   - **Adreno 619 (`0x6010900`) & Adreno 618 (`0x6010800`)**: Tier 2 — `render_scale=0.65`, `shadow_scale=0.50`, `shadow_rate=2` (30Hz), `low_cost_pp=1`, `disable_msaa=1`, `strict_barriers=0`, `tex_downscale=2`.
   - **Adreno 610 (`0x6010001`)**: Tier 3 — `render_scale=0.50`, `shadow_scale=0.50`, `shadow_rate=2` (30Hz), `low_cost_pp=1`, `tex_downscale=2`, `disable_msaa=1`, `strict_barriers=0`, plus auto **surface cap 720p** lewat `RuntimeConfigManager.saveDetectedGpuProfile()`.

---

## 3. Penutupan 3 Celah `hybrid34`

### 3.1 Audit Mendalam Cheat Kecepatan Jalan/Lari (`main+0x3dd1dac`)
Disassembly langsung `FarmPlayerController::ConvertRateToSpeed` (`0x03dd1b20..0x03dd1dd0`) dari `main` (`306dfcfc9eba4a1a`):
```asm
  03dd1da8: 1e220280  scvtf    s0, w20          // s0 = (float)mMapSpeedRatio (default 100 di .ctor 0x3dd5598)
  03dd1dac: 52a85909  mov      w9, #0x42c80000  // w9 = 100.0f
  03dd1db0: 1e270121  fmov     s1, w9           // s1 = 100.0f
  03dd1db4: 1e211800  fdiv     s0, s0, s1       // s0 = mMapSpeedRatio / 100.0f  (FDIV, bukan FMUL!)
  03dd1db8: bd400101  ldr      s1, [x8]         // s1 = baseSpeed (Define.Player: walk/run/ride/carry/chimney)
  03dd1dbc: a9424ff4  ldp      x20, x19, [sp, #0x20]
  03dd1dc0: f9400ff5  ldr      x21, [sp, #0x18]
  03dd1dc4: a940fbfd  ldp      x29, x30, [sp, #8]
  03dd1dc8: 1e210800  fmul     s0, s0, s1       // s0 = (mMapSpeedRatio / 100.0f) * baseSpeed
  03dd1dcc: fc4307e8  ldr      d8, [sp], #0x30
  03dd1dd0: d65f03c0  ret
```
- Seluruh cabang jalan/lari/naik kuda/bawa barang di `ConvertRateToSpeed` bermuara ke `0x03dd1da8` -> `0x03dd1db4 (fdiv)` -> `FarmPlayerController::Move (0x03dd1de0)` -> `UnitController::Move (0x01e501d0)` -> `UnityEngine.AI.NavMeshAgent::Move_Injected (0x03a70000)`.
- Karena `0x03dd1db4` adalah **`fdiv`**, mengubah divisor `100.0f` (`0x52a85909`) menjadi `66.5f` (`0x52a850a9`) menghasilkan **`1.504x`**, dan `50.0f` (`0x52a84909`) menghasilkan **`2.000x`**.
- Pengerasan pada `cheat_engine.c`:
  - `write_main_text_u32()` memakai volatile store + `__builtin___clear_cache()` sebelum **dan** sesudah `mprotect(..., PROT_READ | PROT_EXEC)` (menjamin koherensi I-cache ARM64 bahkan pada fallback W^X `RW -> RX`) + verifikasi readback `*(const volatile uint32_t *)ptr == val`.
  - `reload_locked()` langsung memanggil `apply_builtin_patches_locked()` bila `main` sudah dimuat agar membuka menu cheat in-game tidak menciptakan jeda restore sementara.
  - Host unit test `test_cheat_engine`: **100% PASS** (14 cheat: 11 bawaan + 3 Atmosphere).

### 3.2 Fix Koordinat Sentuh saat `SurfaceHolder.setFixedSize()` Aktif
- `GameActivity.java`: menyimpan ukuran fisik View (`mTouchViewW x mTouchViewH`) pada `surfaceCreated` & `surfaceChanged`, dan mengekspos `getTouchViewWidth()` / `getTouchViewHeight()` (terverifikasi tetap utuh di DEX final oleh R8).
- `front_android.c`: menyegarkan `(g_touch_vw, g_touch_vh)` pada `on_window_created`, `on_window_resized`, dan `on_window_focus`, lalu menskalakan seluruh koordinat `AMotionEvent_getX/Y` di `touch_event`, `menu_touch`, `sp_touch`, `cp_touch`, `ed_touch`, dan `tx_touch` secara proporsional (`x * surfW / viewW`, `y * surfH / viewH`), dengan bypass `1:1` eksak saat `|view - surf| <= 1.5 px`.

### 3.3 Migrasi `hybrid35_migrated`, Auto-Hide Overlay Xbox, & Pesan Error Import
- `RuntimeConfigManager.java`: menambahkan migrasi satu kali `hybrid35_migrated` yang mengunci `poolSafeSyncMode = 2` (`SAFE_SYNC_SERIAL_DBG`) dan `strictBarriers = false`.
- `front_android.c`: `pad_mark_used()` langsung menyembunyikan overlay sentuh (`atomic_exchange(&g_overlay, 0)` + `dm_overlay_redraw()`) saat tombol/stick/trigger Xbox dipakai, dan `touch_event()` langsung memulihkannya (`atomic_exchange(&g_overlay, 1)` + `dm_overlay_redraw()`) saat layar disentuh.
- `nx_nsp.c`: pesan error header tidak valid kini menampilkan ukuran file (`size=... bytes`) + 4 byte magic (`PK` = arsip ZIP, `Rar!` = RAR, `7z` = 7Z, `<!`/`<h` = halaman HTML, `0 bytes` = file kosong, atau hex mentah).

---

## 4. Artefak Build & Verifikasi Offline

> Catatan: Seluruh verifikasi berikut dilakukan secara **offline/host-side** (bukan eksekusi di perangkat fisik).

- **APK Final:** `hybrid_port/dist/Story-Of-Seasons-Doraemon-FOTGK.apk` (`33 MB`, `versionCode = 35`, `versionName = 1.1.2`, `core_version = 1.1.2-hybrid35`)
- **APK SHA-256:** `62d5028f063acb167720463a77655ed8418e37f292dbfad4f9fafdb3fe060bf1`
- **`libdmhost.so` SHA-256:** `47c411e5789242029c5b73ef01b04089a83341d2374e0803d6b04c506e917d08` (**0 instruksi LSE**)
- **`libnxhybrid_extract.so` SHA-256:** `500d4e71b6bfba719d52a6a1675e08122aaf5cbb5215c8f7929ba8152ed23c3e` (**0 instruksi LSE**)
- **`verify_hybrid_apk.sh`:** **7/7 PASS** (16 KB alignment OK, APK Signature v3 valid, R8 DEX verified).
- **Host Unit Test (`test_cheat_engine`):** **100% PASS**.
