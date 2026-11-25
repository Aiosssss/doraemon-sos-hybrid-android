# Doraemon SoS Hybrid Android — `v1.1.2-hybrid32` (`versionCode = 32`) Technical Report

## 1. Adaptive Button Level 2 Redesign & Full Customization (Device Feedback Fixes)

1. **Studio-Grade Story of Seasons SVG Artwork + Corner Button Clues** ([`gen_overlay_svgs.py`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/tools/gen_overlay_svgs.py) -> `assets/overlay/adaptive.png`, `1024×1024`, 32 icons × 2 states):
   - Replaced flat icons with layered Storybook RPG warm medallions, role-coded color tints (Sunlit Amber `Y`, Meadow Emerald `A`, Sunset Coral `B`, Caramel Leather `X`, Sky Cerulean `L`/`R`/`RS`/`LS`, Doraemon Blue-Gold Bell `ZR`, Parchment Wood `ZL`/`-`/`+`), dark espresso outlines, and glossy specular highlights.
   - Added a **compact high-contrast top-right Switch button clue badge** (`Y`, `A`, `B`, `X`, `L`, `R`, `ZL`, `ZR`, `-`, `+`, `RS`, `LS`) on every button so players immediately recognize its exact Switch mapping.
   - Added matching **Carved-Wood & Gold Adaptive D-Pad** (base + 4 glowing golden directional press overlays) and **Warm Oak Compass Analog Stick Base + Cream-Gold Tactile Knob** used by **both Left Stick and Right Stick**.

2. **Fixed Live Game Context Switching (`dm_game_tool_context()` & `dm_game_interact_context()`)** ([`cheat_engine.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/host/cheat_engine.c)):
   - **Root cause in `hybrid31`:** `dm_guest_heap_readable()` only checked `[g_heap, g_heap + g_heap_cur)`, rejecting IL2CPP Boehm GC objects living in `g_alias`/`mmap` and causing `dm_game_tool_context()` to stay stuck at `0` (`Hand`).
   - **Fix:** Switched pointer walking to `vm_read_overwrite()` (`safe_vm_read()`), safely traversing `UserManager` -> `UserModel` (`+0x18`) -> `InventoryModel` (`+0x30`) -> `mCurrentSectionType` (`+0x20`) / `mIsCarryingItem` (`+0x28`) -> `QuickSlots[QuickSlotIndex]->mId` (`+0x10`) and `Define.Item` static arrays.
   - **Live adaptive icons:**
     - Button `Y` morphs across **11 states**: Hand, Watering Can, Hoe, Hammer/Pickaxe, Axe, Sickle, Fishing Rod, Bug Net, Seed Pouch/Fertilizer, Animal Care (Brush/Milker/Shears/Bell/Feed), and Carrying/Rucksack Item (Apple/Crop/Gift).
     - Button `A` morphs across **3 states**: Confirm Checkmark (Title/Menu/Loading), Talk Speech Bubble (active exploration/NPC), and Harvest/Give Hand+Heart (`mIsCarryingItem == 1`).
     - Emits one-shot transition diagnostics `[adaptive] ctx: tool=%d interact=%d sec=%d qidx=%d carry=%u itemId=%d`.

3. **Full Layout Editor Customization in Adaptive Mode** ([`overlay_layout.h`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/overlay_layout.h), [`front_android.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/front_android.c), [`gfx_vk_present.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_present.c)):
   - Unified Adaptive Button geometry directly into `OV_CTLS[i]` + `ov_box()` + `g_cust`.
   - Opening **Edit Layout** while Adaptive Button is ON now renders the **Adaptive SVG controller** (including D-Pad & Right Stick) and lets users drag, resize, hide/show, snap to grid, reset, and persist custom positions for every control.

---

## 2. Vulkan Stability & Universal FPS Boost (`v1.1.2-hybrid32`)

### Bagian 1 — Mali-G76 / Bifrost (`SM-G975F`, `0x72110000`) Missing 3D Characters & Performance Collapse
- **Eviction vs. Descriptor Set Cache Bug Fixed** ([`gfx_vk_tex.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_tex.c)):
  - **Root cause:** Persistent descriptor sets (`70,870` reused) bypassed `vk_tex()`, leaving `im->pkg_used` stale; `[low-ram]` evicted `59` active character/world textures still registered in NVN `TexturePool`.
  - **Fix:** `vk_tex_sweep()` now **never evicts textures currently registered in `g_treg` (`tex_is_registered_in_pool()`)** or touched within `180` frames, and `img_drop()` atomically bumps `g_tex_gen` (`__atomic_add_fetch(..., __ATOMIC_RELEASE)`) to invalidate all cached descriptor sets & view caches.
- **36–126 ms `pool_readback` Stall Eliminated** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c)):
  - In `vk_gpu_wrote_ssbo()`, large (`> 2 MiB`) `CPU_UNCACHED` GPU-only compute SSBO buffers (Unity `SkinnedMeshRenderer` dynamic vertex buffers) skip full-pool CPU readback even under `pool_safe_sync=2`, while `serial_safe` only uploads CPU-dirty 16 KB blocks (so GPU-skinned vertex buffers are never overwritten).
- **`low_cost_pp` Restricted Strictly to HDR Float Bloom** ([`gfx_vk_exec_passes.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_passes.inc)):
  - Removed `R8G8B8A8_UNORM` / `R8G8B8A8_SRGB` from `pass_commit()` skip so only verified HDR float bloom mip passes (`R16G16B16A16_SFLOAT` / `B10G11R11_UFLOAT_PACK32`, `<= 160×90`, no depth) are skipped.
- **`tex_downscale` Aspect-Ratio Guard** ([`gfx_vk_tex.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_tex.c)):
  - Strictly excludes thin/1D LUTs & toon ramps (`w >= 4*h` or `h >= 4*w`) in addition to `< 3 mips` and `< 256×256`.
- **Mali-G76 Tier 1 & Clean Migration** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c), [`RuntimeConfigManager.java`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/java/org/nx2a/doraemon/RuntimeConfigManager.java)):
  - `Mali-G76` (`0x72110000`) stays locked in **Tier 1 (Mid-High)** (`render_scale=0.75`, `low_cost_pp=0`, `tex_downscale=1`, `strict_barriers=0`), and `hybrid32_migrated` clears stale `strict_barriers=1` / forced `low_ram` overrides.

### Bagian 2 — Samsung Xclipse 940 (`SM-S921B`, `0x144d`, AMD RDNA3) FC & 5.6s GPU Hang
- **Vendor Quirks Extended to Samsung Xclipse (`0x144d`)** ([`gfx_vk_quirks.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_quirks.c)):
  - Disabled Adreno-only `UBWC` (`VQ_NO_UBWC`), unsafe `AHardwareBuffer` zero-copy (`VQ_NO_ZEROCOPY`), and `CB_SPLIT` (`VQ_NO_CB_SPLIT`) on `VENDOR_SAMSUNG` (`0x144D`), and disabled Mali-TBDR `vk_inline_resolve` on `0x144D`.
- **GPU Drain Before `mmap(MAP_FIXED)` Remap** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c)):
  - `vk_pool_remap_ro()` now drains in-flight GPU work via `vkQueueWaitIdle(V.q)` before remapping any zero-copy dma-buf range.
- **Full Native + Vulkan + Java Crash Telemetry** ([`crash.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/host/crash.c), [`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c), [`gfx_vk_present.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_present.c), [`SaveAndLogManager.java`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/android_app/app/src/main/java/org/nx2a/doraemon/SaveAndLogManager.java)):
  - Added `SIGFPE` alongside `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGTRAP`/`SIGABRT`; logged `vkQueueSubmit > 1500 ms` TDR stalls and non-`VK_SUCCESS` errors from `vkWaitForFences`/`vkQueuePresentKHR`; installed process-wide Java `UncaughtExceptionHandler` writing `game_data/crash_java.log` (automatically bundled into Export Logs).

### Bagian 3 — Mali-G57 MC2 (`RMX3636`) & Universal Low/Mid-End FPS Boost
- **Fixed Shadow Map Unscaling (`1024×1024 -> 512×512`)** ([`gfx_vk.h`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk.h), [`gfx_vk_tex.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_tex.c), [`gfx_vk_exec_copies.inc`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_exec_copies.inc)):
  - Added `im->is_shadow` flag on shadow-scaled targets so `copy_b2t` never misclassifies `1024×1024` shadow targets as font atlases and keeps them at `512×512` via temporary 1:1 blit.
- **Vulkan 1.1 Swapchain Pre-Rotation** ([`gfx_vk_present.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_present.c)):
  - Implemented `rot_rp()` + `rot_fb()` + `ov_fb_for()` classic `VkRenderPass` fallback so hardware pre-rotation activates on Vulkan 1.1 drivers (`Mali-G57`/`Mali-G76` r32p1) without requiring `VK_KHR_dynamic_rendering`.
- **Responsive Android 14/15/16 ADPF Boost** ([`perf_android.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/perf_android.c)):
  - Triggers boost at `GPU > 78%` of target (3 frames, 1.5 s cooldown); uses `APerformanceHint_notifyWorkloadIncrease` on API 36 and falls back to 15% tighter target-pressure updates via `updateTargetWorkDuration` on Android 14/15 (`API 34/35`) to prevent MediaTek/Samsung DVFS downclocking during `fence_wait`.
- **Mali-G57 MC2 Sub-Preset & Pool Keep Budget** ([`gfx_vk_core.c`](file:///home/ilhamziqri/Documents/Port-game/Sos-doraemon/hybrid_port/runtime_src/nxnative/dm/android/gfx_vk_core.c)):
  - Dedicated `Mali-G57 MC2` auto preset (`render_scale=0.55`, `shadow_scale=0.50`, `low_cost_pp=1`, `tex_downscale=2`, `disable_msaa=1`) and raised Tier-1 kept pool buffer cap to `32 MiB` (`128 MiB` on Tier 0).

---

## 3. Build & Verification (Host/Offline)
- `libdmhost.so`: 197/197 C sources compiled cleanly (`0 failed`).
- Host `cheat_engine.c` unit test: **100% PASS**.
- APK (`hybrid_port/dist/Story-Of-Seasons-Doraemon-FOTGK.apk`, label `SOS FOTGK`, `versionCode = 32`, `versionName = 1.1.2`): signed, 16 KB page-aligned (`zipalign -P 16`), verified **7/7 PASS** via `verify_hybrid_apk.sh`.
- All verification above is offline/host-side; physical device testing is required on `SM-G975F`, `SM-S921B`, and `RMX3636` to measure live FPS and verify on-screen visuals.
