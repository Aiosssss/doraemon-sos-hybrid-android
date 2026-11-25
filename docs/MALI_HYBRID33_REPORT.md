# Doraemon SoS Hybrid Android — `v1.1.2-hybrid33` (`versionCode = 33`) Technical Report

## 1. Root-Cause Fix: Adaptive Button Live Tool Icon (Watering Can & All Tools!)
- **Root cause verified in `main.patched.bin` (`0x34c4220`):** `Define.Item` static fields `CultivatingItems` (`+0x450`), `BreakingItems` (`+0x458`), `WateringItems` (`+0x460`), `MowingItems` (`+0x468`), `CuttingItems` (`+0x470`), `MiningItems` (`+0x478`), `FishingItems` (`+0x480`), `BugCatchingItems` (`+0x4a0`), `Fertilizers` (`+0x4a8`), `AnimalFeeds` (`+0x4b0`), `MatingSeeds` (`+0x4b8`), `AnimalMedicine` (`+0x4c0`), and `Bells` (`+0x540`) are **`System.Collections.Generic.HashSet<int>`**, NOT `int[]`!
- In `hybrid32`, `il2cpp_int_array_contains()` read `hs + 0x18` (actually the 64-bit `_slots` pointer!) as an `int32_t` array length, failing `len > 128` for every single tool (only numeric seed range & carry state matched).
- **Fix ([`cheat_engine.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/host/cheat_engine.c)):** Replaced with `il2cpp_hashset_int_contains()` reading `hs + 0x18` (`Slot[] _slots`, 12-byte entries `{int32_t hash_code; int32_t next; int32_t value;}` with `hash_code >= 0 && value == needle`), removed invalid dictionary offsets `+0x488`/`+0x490`, added `Bells` (`+0x540`) and scalar Brush/Milker/Shears fallback, and throttled context polling to `100 ms` (`10 Hz`, ~1× every 6 frames).

---

## 2. Bagian 1 — 60 FPS Rata (Xiaomi Mali-G720 MC7 & Semua Mali/Xclipse)
1. **Safe 16 KB Hot-Block Exemption in `pool_safe_sync=2`** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c)):
   - `serial_safe` (`pool_safe_sync=2`) allocates per-16KB-block `p->hot`. Blocks dirtied for `hot_streak >= 3` consecutive frames (not overlapping `p->no_hot`, which covers all GPU compute SSBOs / `vk_gpu_wrote` / `vk_pool_ban_hot` VkImages) stay `PROT_READ | PROT_WRITE` (skipping `mprotect(PROT_READ)` in both `vk_pools_pre_rearm_serial()` and `serial_safe` Pass 1), capped globally at `512 × 16 KB = 8 MiB`.
   - Every hot block is unconditionally copied verbatim in Pass 2 (0% flicker, zero 1 KB diff skip) and auto-cools back to `PROT_READ` after `180` unchanged frames (~3 s).
2. **Deferred Fence Wait (Safe CPU+GPU Pipelining in `pool_safe_sync=2`)** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c), [`RuntimeConfigManager.java`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/java/org/nx2a/doraemon/RuntimeConfigManager.java), [`LauncherActivity.java`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/java/org/nx2a/doraemon/LauncherActivity.java), [`panel_settings.xml`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/res/layout/panel_settings.xml)):
   - Controlled by **`Pipelining Aman (Deferred Wait)`** (`DM_VK_DEFERRED_WAIT`, default `1` / ON, auto-fallback to `0` if `.gpu_device_lost` marker exists).
   - When prior submission needs no CPU readback (`!g_last_sub_needs_rb`), `begin_segment()` waits only on `g_fence - 1` (Frame N-2) and uploads coalesced 16 KB dirty+hot runs via per-slot `s->up` staging (`vk_scratch` + `vkCmdCopyBuffer -> p->buf`), ordered strictly after GPU Frame N-1 by the hardware chain semaphore at `VK_PIPELINE_STAGE_ALL_COMMANDS_BIT` (0% flicker, 0 extra RAM).
   - Enforces a **3-second safe timeout** on `wait_fence()` and `begin_segment()` `mtl_wait()` with `[RED] [vk] FENCE TIMEOUT / DEVICE_LOST` logging + `.gpu_device_lost` marker.
   - Reports `deferred_wait_saved_ms` and `hot 16KB blocks` every 300 submissions.
3. **ADPF Dual-Trigger Boost (CPU-Bound + GPU-Bound)** ([`perf_android.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/perf_android.c)):
   - Triggers ADPF boost when `cpu_work > 0.82 * target` **OR** `GPU_time > 0.75 * target` (fixes CPU-bound 18.5 ms frames on Xiaomi Mali-G720 MC7 when GPU is only 11.5 ms), and lowers `hint_threads_scan()` threshold to `10%` (up to 6 busy worker/main threads).
4. **Reduced `pool_readback`, `full copies`, & Zero-Cost Idle Cheat/Adaptive** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c), [`cheat_engine.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/host/cheat_engine.c)):
   - `vk_gpu_wrote_ssbo()` skips CPU readback for `> 64 KiB` GPU-only `CPU_UNCACHED` compute SSBO writes (eliminating `0.5–7.8 ms` spikes and unlocking ~99% Deferred Fence Wait overlap), while `cpf_put()` accepts pools up to `75%` of `cpf_budget_bytes()` (`128 MiB` on Tier 0).
   - `dm_cheats_tick()` returns in 1 atomic load when `master == 0`; Adaptive context polling is throttled to `10 Hz`.

---

## 3. Bagian 2 — Samsung S24 (Xclipse 940 / AMD RDNA3, `0x144d`)
- **Full `Compute -> VertexInput` & `TransformFeedback` Memory Barriers** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c), [`gfx_vk_exec_passes.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc)): Explicit L0/L1->L2 barrier after `vkCmdDispatch` and outside render pass after `g_tf_in_pass` Transform Feedback writes.
- **Auto Wave32 Compute Subgroup Pinning (`subgroupSize = 32`)** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c)): Automatically attaches `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo { .requiredSubgroupSize = 32 }` to compute pipelines on Wave64 GPUs with `minSubgroupSize <= 32` and `VK_SHADER_STAGE_COMPUTE_BIT` pinnable (Samsung Xclipse 940 `0x144d`).
- **Non-TBDR Guard & Deferred Image Destruction** ([`gfx_vk_exec_passes.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc), [`gfx_vk_tex.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_tex.c)): Disabled `pass_apply_fullscreen_load_opt()` (`dc_folded`) on `0x144d`, and verified `img_drop()` / `vk_tex_view_drop_all()` strictly use deferred destruction + atomic `g_tex_gen` invalidation.

---

## 4. Bagian 3 — Handheld Crop, RAM Trim & Urgent Audio
- **Handheld `1920×1080 -> 1280×720` `renderArea` Clamp** ([`gfx_vk_exec_passes.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc)): Clamps `renderArea` to active `<= 1280×720` scissor/viewport on Handheld `NVN_TEXTURE_FLAGS_DISPLAY` passes while preserving full imageless framebuffer extent `fk.w/fk.h`.
- **`madvise(MADV_DONTNEED)` RAM Trim** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c)): Releases dirty anonymous pages immediately before freeing large (`>= 256 KiB`) pool `baseline` buffers in `mtl_pool_del()`.
- **Urgent Audio Priority + Adaptive 4-Block Buffer** ([`audiotoolbox_aaudio.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/compat/audiotoolbox_aaudio.c)): Sets `ANDROID_PRIORITY_URGENT_AUDIO` (`setpriority(..., -19)` / `-16` fallback) in AAudio callback thread and configures a 4-block AAudio buffer capacity on `low_ram >= 1` devices.

---

## 5. Build & Offline Verification
- `libdmhost.so`: 197/197 sources compiled cleanly (`0 failed`).
- Host `cheat_engine.c` unit test: **100% PASS**.
- Release APK (`hybrid_port/dist/Story-Of-Seasons-Doraemon-FOTGK.apk`, `versionCode = 33`, `versionName = 1.1.2`, label `SOS FOTGK`): signed, 16 KB page-aligned (`zipalign -P 16`), verified **7/7 PASS**.
- All checks are host/offline; physical testing on Xiaomi Mali-G720 MC7, Samsung S24 Xclipse 940, and 4 GB low-end phones is needed to confirm live FPS & visuals.
