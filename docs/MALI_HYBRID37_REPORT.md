# Laporan Rilis `v1.1.2-hybrid37` (`versionCode = 37`)

**Target Aplikasi:** `org.nx2a.doraemon` (**SOS FOTGK**)  
**Game:** *DORAEMON STORY OF SEASONS: Friends of the Great Kingdom* (`01008EE0177A8000`, Update `1.1.2` / `v262144`, `main` Build ID `306dfcfc9eba4a1a7d0ae7ffcee4b9e4`)  
**Artefak APK:** `hybrid_port/dist/Story-Of-Seasons-Doraemon-FOTGK.apk` (33 MB)  
**APK SHA-256:** `76f0b2869f3b713630967e18e00e53300b84f89fb02b3ef87cbe27c8b0140c0a`  
**`libdmhost.so` SHA-256:** `a500460e7c5e770ccf139c6ce8156a0294592b818c17cdb5e9abf31b15d0cc4e`

---

## Ringkasan Perubahan `v1.1.2-hybrid37`

Dibangun langsung di atas `v1.1.2-hybrid36` tanpa mengubah jalur performa Mali modern (`drv > 0x08001000`, mis. Mali-G720), Qualcomm Adreno (`0x5143`), maupun Samsung Xclipse (`0x144d`).

### 1. Fix 100% Hang `PIPELINE_COMPILE` pada Mali Driver `<= 32.1.0` (`0x08001000`)

#### 1.1 Hasil Inspeksi `spirv-dis` pada `vs=2a27276db82273d7` + `fs=48736f6b5e6c55e3`
- **`vs=2a27276db82273d7`** (stage 0, 375 kata / 96 baris): shader passthrough minimal (`gl_Position = in0; gl_PointSize = 1.0`) dengan **0 output `Location`**, sehingga `gfx_vk_shader.c` mensintesis 3 output konstan `(0,0,0,1)` di `Location 0/2/3` via `spv_add_outputs(xo=0xD)` + `spv_vs_init()`. Mengandung 9 `OpCapability` tidak terpakai (termasuk `Float64` yang sebelumnya diubah menjadi duplikat `OpCapability Shader`, `GroupNonUniformBallot/Shuffle/Vote`, `ImageBuffer`, `SampledBuffer`, `ImageGatherExtended`, `ImageQuery`, `DrawParameters`).
- **`fs=48736f6b5e6c55e3`** (stage 1, 1401 kata / 311 baris): **0 loop (`OpLoopMerge` = 0)** dan **0 dynamic array indexing**. Shader cutout 3-MRT (`OpKill` di baris 144, 2 `OpImageSampleImplicitLod` ber-guard `OpIsNan`/`OpSelect`, 13 `GLSL.std.450 Fma`, 1 `FClamp` yang sebelumnya di-rewrite menjadi `NClamp`, 7 local `Function` `OpVariable`, 2 UBO `4096 x vec4`), serta 9 `OpCapability` tidak terpakai (termasuk `FragmentShaderPixelInterlockEXT` + duplikat `OpCapability Shader`).
- **Sanitasi SPIR-V khusus `vk_mali_legacy_r32()` (`gfx_vk_shader.c`):**
  - Menghapus duplikat `OpCapability Shader` serta seluruh `OpCapability` tidak terpakai (`GroupNonUniform*`, `DrawParameters`, `ImageQuery`, `ImageGatherExtended`, `ImageBuffer`/`SampledBuffer`) — tervalidasi 100% oleh `spirv-val --target-env vulkan1.1` (VS 422 -> 404 kata, FS 1389 -> 1373 kata).
  - Mempertahankan `FMin/FMax/FClamp` standar (tanpa rewrite `NMin/NMax/NClamp`) pada Mali `<= 32.1.0`.
- **Pre-seed Guard:** Karena compiler biner `libGLES_mali.so` r32p1 tidak dapat dijalankan di host Linux dan pasangan `2a27276db82273d7 + 48736f6b5e6c55e3` memiliki VS 0-output (bukan mesh karakter/dunia nyata), pasangan ini juga di-pre-seed pada blacklist default khusus `drv <= 0x08001000` (dapat di-override via `DM_VK_ALLOW_BAD_PIPES=1`).

#### 1.2 100% Non-Blocking Async Pipeline Compile pada Mali `<= 32.1.0`
- `gfx_vk_exec_pipelines.inc`: saat `vk_mali_legacy_r32()` aktif, `graphics_pipeline()` **TIDAK PERNAH** memanggil `build_pipeline()` / `vkCreateGraphicsPipelines` secara sinkron di render thread. Seluruh pipeline baru selalu didorong ke worker latar belakang (`async_pipe(k, 1)`) memakai `pthread_mutex_trylock`, dan draw dilewati sementara hingga siap.
- `gfx_vk_exec_passes.inc` (`mc_pipeline`): menggunakan `vk_pipe_compile_trylock()` pada Mali `<= 32.1.0` sehingga render thread tidak pernah terblokir `g_pipe_compile_mu` meskipun worker sedang mengompilasi.

#### 1.3 Persistent Bad-Shader Guard (`compiling_pipeline.tmp` -> `bad_pipelines.txt`)
- `gfx_vk_core.c`:
  - `vk_pipe_worker_begin()` / `vk_pipe_worker_end()`: menulis `(driverVersion, vs_hash, fs_hash)` ke `game_data/compiling_pipeline.tmp` (`fflush` + `fsync`) sebelum `vkCreateGraphicsPipelines` dan langsung `unlink()` sesudahnya.
  - `vk_wd_main()` (Watchdog): jika worker macet di `PIPELINE_COMPILE > 8 detik` pada Mali `<= 32.1.0`, langsung menulis ke `game_data/bad_pipelines.txt` (`fflush` + `fsync`) dan mem-blacklist pasangan di memori.
  - `vk_bad_pipes_init()`: saat boot berikutnya, memindahkan `compiling_pipeline.tmp` (jika tersisa dari hang/kill) ke `bad_pipelines.txt`, memuat semua blacklist untuk `driverVersion` aktif, dan melewatkannya tanpa memanggil `vkCreateGraphicsPipelines` (`[vk] skipped blacklisted shader vs=... fs=...`).
  - `PCACHE_BURST = 30` tetap aktif.

---

### 2. Fix Cap Resolusi Surface WQHD+ (`3040x1440` -> `2280x1080`) & Touch 1:1

- **Akar Penyebab Sebelumnya di SM-G975F (`gpu_tier=1`):**
  1. `GameActivity.surfaceCreated()` memanggil `holder.setFixedSize()` lalu langsung memanggil `super.surfaceCreated(holder)` sebelum `ViewRootImpl` melakukan relayout; selain itu, pada `PhoneWindow.takeSurface()` Android 12 Samsung One UI, `ANativeWindow` tetap berukuran fisik `3040x1440` kecuali `ANativeWindow_setBuffersGeometry()` juga diterapkan sebelum `vkCreateAndroidSurfaceKHR`.
  2. `RuntimeConfigManager.getSurfaceMaxShortEdge()` belum membedakan `detected_gpu_tier == 0` (Flagship tetap native) vs `tier >= 1` (`1080`) vs `tier >= 3` (`720`).
- **Perbaikan (`GameActivity.java`, `RuntimeConfigManager.java`, `gfx_vk_present.c`, `front_android.c`):**
  - `RuntimeConfigManager`: menyimpan `detected_gpu_tier` (`0` = Flagship -> native `0`; `1..2` = Mid/Low -> `1080`; `3` atau `Turbo Kentang` -> `720`) dan menulis `DM_SURFACE_MAX_SHORT=auto` secara default.
  - `GameActivity.java`: memanggil `holder.setFixedSize(scaledW, scaledH)` di `surfaceCreated` (dan re-check di `surfaceChanged`) sambil mempertahankan `mTouchViewW`/`mTouchViewH` di dimensi layar fisik (`3040x1440`), serta mencetak `[surface] fixed size applied: ...`.
  - `gfx_vk_present.c` (`swap_make()`): sebelum `vkCreateAndroidSurfaceKHR`, mengecek `vk_surface_max_short()` dan memanggil `ANativeWindow_setBuffersGeometry(w, target_w, target_h, 0)` (contoh S10+ `3040x1440` -> `2280x1080`, hemat 56% piksel fillrate!) serta mengekspor `vk_present_extent(&pw, &ph)`.
  - `front_android.c` (`touch_event`): menggunakan `vk_present_extent(&pw, &ph)` bersama guard `vw >= 64 && surfW >= 64.f` sehingga `touch_sx`/`touch_sy` memetakan `3040x1440 -> 2280x1080` (rasio `0.7500`) dengan presisi 1:1 untuk seluruh tombol, Flexible Analog, dan Adaptive Button.

---

### 3. Cheat Baru ke-12: `Item Tak Habis (Slot Dipegang)` (`builtin_inf_held_item`)

- **Analisis Disassembly Nyata v1.1.2 (`main.patched.bin` Build ID `306dfcfc9eba4a1a` + `global-metadata.dat`):**
  - `InventoryModel::TryReduceItemInHand` (`0x03d7e650`) -> `InventorySectionModel::TryReduceItemInHand` (`0x03df4a60`):
    - `0x03df4c54..0x03df4c84`: game sendiri menolak slot kosong (`index == -1`), `item == NULL`, dan `mCount < reduce_count`.
    - **`main+0x03df4d2c`**: `0x4b170129` (`sub w9, w9, w23`) -> dipatch menjadi **`0xd503201f`** (`nop`), mempertahankan `mCount` utuh (bahkan saat `mCount == 1` tidak pernah habis/hilang!) dan otomatis dicerminkan ke `mQuickSlots` oleh game pada `0x03df4d78`.
  - Offset field terverifikasi dari disassembly: `InventorySectionModel::mMaxStack` (`+0x28` @ `0x03c3cd20`), `ItemModel::mId` (`+0x10` @ `0x03c352f0`), `ItemModel::mCount` (`+0x18` @ `0x03c35310`), `ItemModel::mMaster` (`+0x48` @ `0x03c35a8c`), `ItemModel::mTool` (`+0x50` @ `0x03c35570`), `ItemMasterModel::mIsImportantItem` (`+0x1e` @ `0x03c351d0`), `ItemMasterModel::mToolId` (`+0x34` @ `0x03c35240`).
- **Dynamic Live Safety Guard (`dm_cheat_eval_held_item_guard` di `cheat_engine.c`):**
  - Berbagi pembacaan konteks 10 Hz dengan Adaptive Button (`refresh_game_context()`) hanya saat `master == 1 && builtin_inf_held_item.enabled == 1` (**0 beban saat OFF**).
  - Menolak otomatis (mengembalikan `0x03df4d2c` ke `0x4b170129` secara dinamis) bila: `sec_type != 1` (ToolPocket), slot kosong (`item_id <= 0 || count <= 0`), non-stackable (`max_stack <= 1 || count > max_stack`), Tool (`mTool != NULL || mToolId > 0`), Key/Quest Item (`mIsImportantItem != 0`), atau konteks alat (`WATERING_CAN..BUG_NET`, `ANIMAL`).
  - Saat Master OFF / cheat OFF: `0x03df4d2c` dipulihkan instan ke `0x4b170129`.
- **UI:** Terdaftar di `CheatManager.java` dan `cheat_engine.c` (muncul otomatis di Launcher & In-Game Pause Menu, default `OFF`).

---

## Bukti Verifikasi Offline (Host-Side)

> **Catatan:** Seluruh pemeriksaan di bawah ini dilakukan secara offline di host Linux (belum dijalankan di perangkat Android fisik).

| Pemeriksaan | Hasil |
|---|---|
| `spirv-val --target-env vulkan1.1` (VS & FS tersanitasi) | **PASS** — VS `422 -> 404` kata, FS `1389 -> 1373` kata |
| Host Unit Test `test_cheat_engine` (12 built-in + 10 kasus safety guard + restore + Atm VM) | **100% PASS** |
| Kompilasi `libdmhost.so` (`build.py --release`, `-march=armv8-a`) | **PASS** — 197/197 sumber, 0 error (2339 KB stripped) |
| Audit instruksi LSE ARMv8.1+ (`llvm-objdump -d`) | **PASS** — `0` instruksi LSE ilegal |
| Build Gradle Release + R8 (`:app:assembleRelease --offline`) | **PASS** — `versionCode = 37`, `versionName = 1.1.2` |
| `verify_hybrid_apk.sh` (16 KB zipalign, V3 RSA-4096 sign, legal, storage) | **7/7 PASS** |
| Path terlindungi (`hybrid_port/l10n_id/`, `Game/`, `tools/`) | **UNTOUCHED** |
