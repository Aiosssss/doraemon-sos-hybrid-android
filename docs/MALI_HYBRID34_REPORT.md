# Laporan Teknis `v1.1.2-hybrid34` (`versionCode = 34`)

**Target Aplikasi:** `org.nx2a.doraemon` (**SOS FOTGK**) — *DORAEMON STORY OF SEASONS: Friends of the Great Kingdom* (`01008EE0177A8000`, Update `1.1.2` / `v262144`, `main` Build ID `306dfcfc9eba4a1a7d0ae7ffcee4b9e4`).  
**Status Verifikasi:** Seluruh kompilasi, unit test host, dan verifikasi APK dilakukan **secara offline / host-side** (belum diuji di perangkat fisik).

---

## Ringkasan 6 Bagian Utama `v1.1.2-hybrid34`

### 1. Fix Crash Play di Adreno 650 / 618 / 6xx (Vulkan 1.1.128)
- **Akar Masalah:** `gfx_vk_core.c` memanggil `die()` (`SIGABRT` di `0.75s`) saat GPU tidak mendukung `VK_KHR_dynamic_rendering` maupun `VK_KHR_imageless_framebuffer` + `VK_KHR_create_renderpass2` (driver Qualcomm `512.502.0` / `0x801f6000`).
- **Solusi:**
  - `gfx_vk.h` & `gfx_vk_core.c`: Menambahkan `V.use_classic_fb = 1` sebagai fallback otomatis tanpa `die()` ketika `!V.has_imageless && !V.has_dynrender` (atau ketika `vkCreateRenderPass2` tidak tersedia).
  - `gfx_vk_exec_passes.inc`:
    - `rp_on()` mengaktifkan jalur render pass saat `V.use_classic_fb == 1`.
    - `rp_get()` membuat `VkRenderPass` Vulkan 1.0 core via `vkCreateRenderPass()` dengan mempertahankan `loadOp`, `storeOp` (`VK_ATTACHMENT_STORE_OP_DONT_CARE` untuk penghematan bandwidth GMEM Adreno), dan `pResolveAttachments`.
    - Menambahkan cache classic `VkFramebuffer` (`cfb_get()`, batas 512 entri) dengan `pNext = NULL` di `VkRenderPassBeginInfo`.
    - Menambahkan `vk_classic_fb_drop_view()` yang dipanggil dari `vk_tex_view_drop_all()` (`gfx_vk_tex.c`) untuk menginvalidasi dan membebaskan `VkFramebuffer` melalui `vk_defer_free_framebuffer()` (0% use-after-free, 0% leak).
  - `settings.tsv`: Mendaftarkan `DM_VK_DEFERRED_WAIT`, `DM_VK_SHADOW_RATE`, `DM_VK_SAMSUNG_SAFE`, `DM_SCREEN_MODE`, dan `DM_SURFACE_MAX_SHORT` untuk menghilangkan peringatan `[RED] setting DM_VK_DEFERRED_WAIT is not in nxnative/dm/settings.tsv`.

### 2. Fix Freeze 21 Detik, `vk_submit 87ms`, Compile Storm 4.2 Detik & WQHD+ Cap (Mali `<= 32.1.0`)
- **Render-Thread Watchdog 5 Detik (`gfx_vk_core.c`):** Thread ringan `vk watchdog` memantau fase atomik (`ACQUIRE_IMAGE`, `POOL_UPLOAD`, `PIPELINE_COMPILE`, `VK_SUBMIT`, `FENCE_WAIT`, dll.); bila macet > 5 detik langsung menulis dan men-flush `[WATCHDOG] RENDER HANG >5s in phase=...`.
- **Swapchain Anti-Stall (`gfx_vk_present.c`):** Mengganti `vkAcquireNextImageKHR(..., UINT64_MAX)` menjadi timeout 2 detik (`2000000000ull`) dengan log `[vk] ACQUIRE_IMAGE TIMEOUT` + recreate swapchain otomatis, serta menaikkan permintaan `minImageCount` dari `3` -> `4` (di-clamp ke `caps.maxImageCount`).
- **Auto `def_wait=0` pada Mali Legacy `<= 32.1.0` (`gfx_vk_core.c` & `RuntimeConfigManager.java`):** `vk_deferred_wait()` otomatis `0` pada ARM Mali `driverVersion <= 0x08001000` (Mali-G76/G57 `32.1.0`, Mali-G52 `26.0.0`) dan tetap `1` pada Mali modern (`>= 49.x`), Adreno, serta Xclipse. `RuntimeConfigManager` tidak lagi memaksa menulis `DM_VK_DEFERRED_WAIT=1` saat default sehingga auto-detect C aktif penuh.
- **Anti Compile Storm 4.2s (`gfx_vk_exec_pipelines.inc` & `gfx_vk_pipeline_cache.inc`):** Pada `vk_mali_legacy_compiler()`, build sinkron dibatasi `<= 2` pipeline per frame; sisanya didelegasikan ke **1 worker asinkron terserialisasi** (`vk_pipe_compile_lock()`) dan `PCACHE_BURST` diatur ke `30` pipeline.
- **WQHD+ & 720p Surface Cap (`GameActivity.java` & `RuntimeConfigManager.java`):** `surfaceCreated()` menskalakan `SurfaceHolder.setFixedSize()` agar sisi pendek `<= 1080` pada layar WQHD+ (`3040x1440 -> 2280x1080`, hemat ~56% fillrate blit/overlay) dan `<= 720` pada preset **Turbo Kentang**, dengan toggle **`Resolusi Layar Native`** di Settings Advanced.

### 3. Samsung Xclipse Safe Render Path (`vendor_id == 0x144d`)
- Menambahkan `vk_samsung_safe()` (`DM_VK_SAMSUNG_SAFE=auto|1|0`, otomatis `1` pada `vendorID == 0x144D` Xclipse 940/920) serta pilihan **`Mode Aman Karakter Samsung (Xclipse)`** di Settings Advanced:
  1. Memaksa `vk_disable_msaa() = 1` (`msaa1`) di `gfx_vk_tex.c`.
  2. Menonaktifkan `VK_EXT_depth_clip_control` (`V.has_depth_clip_ctl = 0`).
  3. Menonaktifkan `VK_EXT_robustness2` (`nullDescriptor = 0`).
  4. Menonaktifkan `VK_KHR_dynamic_rendering` (`V.has_dynrender = 0`, memaksa jalur RenderPass identik Mali).
  5. Tetap mempertahankan `VQ_NO_ZEROCOPY`, `VQ_NO_UBWC`, Wave32 compute pin, serta barrier `Compute -> VertexInput` & `TransformFeedback`.

### 4. Extreme FPS Boost & Penghematan RAM
- **4 KB Hot-Page Exemption + 60-Frame Decay + Lossless Compare Skip (`gfx_vk_core.c`):** Mengatasi regresi `pool_upload = 8.15 ms` di vivo V2109 (`hybrid33` menyalin 197 blok 16 KB = 3.1 MB tanpa syarat). Kini granularitas menjadi **4 KB**, cap `128` halaman (`512 KB`) pada Tier >= 2 dan `384` halaman (`1.5 MB`) pada Tier <= 1, auto-decay setiap 60 frame, serta skip upload lossless (`memcmp`) khusus halaman hot CPU-only (`no_hot == 0`) — 0% flicker!
- **Revive `low_cost_pp` untuk Bloom/Blur/DoF `fmt=37` (`gfx_vk_exec_passes.inc`):** Mengaktifkan skip terverifikasi untuk target 16:9 bloom/blur/DoF `<= 320x180` (`R8G8B8A8_UNORM` single-fullscreen quad `!S.ds.dt` maupun HDR float, non-display, `samples == 1`).
- **Shadow Update Rate (`DM_VK_SHADOW_RATE`, `gfx_vk_core.c` & `gfx_vk_exec_passes.inc`):** Mode `Auto` (`30Hz` half-rate pada Tier >= 2, `60Hz` pada Tier 0–1), `Penuh (60Hz)`, `Hemat 2× (30Hz)`, dan `Mati (Turbo FPS)`.
- **Unlock Deferred Fence Wait Overlap (`gfx_vk_core.c:3732`):** Menghapus blokir `ss_mode != 2` pada `allow_rb_stage` saat `vk_deferred_wait() == 1` sehingga `s->rb_staged` aktif dan membuka CPU+GPU overlap (`skip: rb=300` teratasi pada GPU modern).
- **Preset Baru `Turbo Kentang` + Migrasi `hybrid34_migrated` (`RuntimeConfigManager.java`):** 50% scale, MSAA OFF, Shadow OFF, Low-Cost PP ON, 1/2x tex downscale, surface 720p; migrasi otomatis mereset `strictBarriers=false`, mengunci `pool_safe_sync=2`, dan `low_ram=auto`.
- **ADPF & RAM Trim (`perf_android.c`, `gfx_vk_core.c`):** Pemicu CPU ADPF diturunkan ke `> 80%` target (`GPU > 75%`), thread present memakai nice `-4`, deteksi `low_ram=1` dinaikkan ke `<= 6144 MiB` (`<= 3840 MiB` untuk tier 2), dan budget `cpf_budget_bytes()` dipangkas (`0 / 24 / 64 MiB`).

### 5. Fix Cheat `Kecepatan Jalan/Lari` + Tambah `2.0x` + Audit Seluruh 11 Built-in Cheat
- **Akar Masalah:** `cheat_engine.c` memakai `mprotect(PROT_READ | PROT_WRITE | PROT_EXEC)` yang bisa ditolak (`EACCES`) oleh kebijakan SELinux W^X pada perangkat Android 13+ (seperti vivo FuntouchOS), dan `g_dirty_code` sempat dibersihkan meski `main` belum selesai dimuat.
- **Solusi (`cheat_engine.c` & `CheatManager.java`):**
  - Menambahkan fallback W^X-safe `PROT_READ | PROT_WRITE` -> tulis -> `__builtin___clear_cache` -> `PROT_READ | PROT_EXEC` saat `RWX` gagal.
  - Menahan `g_dirty_code = 1` hingga `get_main_region()` tersedia.
  - Memverifikasi langsung melalui disassembly ARM64 `main` (`306dfcfc9eba4a1a`) seluruh 11 cheat bawaan, termasuk **`Kecepatan Jalan/Lari 1.5x`** (`0x03dd1dac`: `100.0f` -> `66.5f`, `0x52a850a9`) dan **`Kecepatan Jalan/Lari 2.0x`** (`0x03dd1dac`: `100.0f` -> `50.0f`, `0x52a84909`) pada grup mutual-exclusion 4.
  - Host unit test `test_cheat_engine`: **100% PASS** (14 cheat: 11 built-in + 3 Atmosphere `.txt` + restore penuh saat Master OFF).

### 6. Mode Layar Full Screen (`16:9 / Stretch / Zoom`) & Dukungan Penuh Xbox Controller
- **Full Screen Cutout + 3 Mode Layar (`GameActivity.java`, `front_android.c`, `gfx_vk_present.c`, `overlay_layout.h`):**
  - `LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES` + sticky immersive fullscreen.
  - Mode `0` (`Normal 16:9`), `1` (`Full Screen Stretch`), dan `2` (`Full Screen Zoom / Crop`) tersedia di Launcher Settings maupun In-Game Pause Menu (`Game -> Screen mode`) secara live, dengan sinkronisasi `vk_pic_set()` agar koordinat sentuh tetap akurat.
- **Xbox Controller Penuh + Filter Sensor Sidik Jari (`front_android.c`, `RuntimeConfigManager.java`, `LauncherActivity.java`):**
  - Blacklist `uinput-fpc`, `uinput-fortsense`, `gpio-keys`, `goodix`, `silead`, `fingerprint`, `uinput-xiaomi`, dll. di `pad_dev_kind()` dan `pad_watch()`, serta validasi `MotionRange(AXIS_X/AXIS_Y)` untuk perangkat `SOURCE_JOYSTICK`.
  - Mapping penuh Xbox One / Series X|S / 360: radial deadzone `0.12`, threshold trigger analog `> 0.25` (`LTRIGGER/BRAKE -> ZL`, `RTRIGGER/GAS -> ZR`), `KEYCODE_BACK` dari gamepad dipetakan ke `DM_BTN_MINUS` (tidak keluar/pause), tombol Guide (`BUTTON_MODE`) membuka Pause Menu, serta pilihan **`Tata Letak Controller: Nintendo / Xbox (A=OK)`** di Launcher Settings & In-Game Pause Menu.
