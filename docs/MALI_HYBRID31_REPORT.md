# v1.1.2-hybrid31 Release Report — UX, Adaptive SVG Controls & Atmosphere Cheat Engine

- **APK:** `hybrid_port/dist/Story-Of-Seasons-Doraemon-FOTGK.apk`
- **Package / Label:** `org.nx2a.doraemon` / `SOS FOTGK`
- **Version:** `versionCode=31`, `versionName=1.1.2`, `hybrid_release=1.1.2-hybrid31`
- **Target Game Build ID:** `306dfcfc9eba4a1a7d0ae7ffcee4b9e4` (Update v1.1.2 / v262144)
- **APK SHA-256:** `06bc8e86e412f260b3bfa6ac3beb0c2433e23ac04288063b80e120b82be1e5b3`
- **Verification:** Offline host-side checks only (`verify_hybrid_apk.sh` 7/7 PASS + `cheat_engine.c` host unit tests 100% PASS). No physical Android device testing performed in this environment.

## Summary of 4 Optional Features (ALL DEFAULT = `OFF`)

1. **Flexible Analog (`DM_TOUCH_FLEXIBLE_ANALOG`, `flex_analog=0` default)**
   - Bottom-left floating capture zone (~42% width × 72% height, avoiding top menu pill).
   - Touching anywhere in the zone snaps left-stick origin to thumb `(tx, ty)` safely clamped from edges; dragging beyond `1.35 * R` softly follows the thumb; releasing resets analog to `(0, 0)` and fades ring to ~25% idle opacity.
   - Live toggleable in **Launcher Settings -> General** AND **In-Game Pause Menu -> Gamepad -> Flexible analog** (zero restart).

2. **Adaptive Button — Level 2 (`DM_TOUCH_ADAPTIVE_BTN`, `adaptive_btn=0` default, Pure SVG Vector, ZERO Text)**
   - Generated 25 pure-vector `.svg` icons (`hybrid_port/docs/overlay_svgs/`) and high-DPI 1024×768 RGBA atlas (`assets/overlay/adaptive.png`) in warm *Doraemon Story of Seasons* oak/gold/cream palette (`#8D5B34` / `#F6D07A` / `#FFF8E7`) with soft shadow and `0.92` scale + golden glow press state.
   - Lazy-loaded into Vulkan VRAM **only** when enabled (`0 bytes` VRAM when OFF).
   - **Live Equipped Tool Context Detection (`dm_game_tool_context()` in `cheat_engine.c`):** safely reads `UserManager` -> `UserModel` -> `InventoryModel` -> `QuickSlots[QuickSlotIndex]->itemId` and matches against `Define.Item` static tool arrays (`main+0x6626770`) to dynamically swap the main button icon across **Hand/Interact, Watering Can, Hoe, Hammer/Pickaxe, Axe, Sickle, Fishing Rod, Bug Net, and Seed Pouch**.
   - Includes **Collapsible Secondary Drawer** (`ZL` Pocket Swap, `ZR` Doraemon Bell, `-` Folded Map, `+` Open Journal, `RS` Camera Eye + D-Pad/R-Stick when expanded) and **3.5s idle auto-fade** to 38% opacity.

3. **Redesigned In-Game Pop-Up / Quick Menu Button**
   - Sleeker (~18% smaller visually) warm carved wood & gold rounded pill with sage leaf emblem and 3 crisp cream bars, patched into both `assets/overlay/phone.png` and `assets/overlay/tablet.png`.

4. **Atmosphere Cheat Engine + 10 Verified Built-in v1.1.2 Cheats (`DM_CHEATS`, default `OFF`)**
   - Native VM in `dm/host/cheat_engine.c` (1-instruction atomic return when OFF -> 0 FPS impact), safe `mprotect(RWX)` + `__builtin___clear_cache` + full original-instruction backup & restore when toggled OFF.
   - Supports importing any Atmosphere `.txt` file (`game_data/cheats/*.txt`, e.g., `306DFCFC9EBA4A1A.txt` or `4EE6FFDA7267A6E0.txt`) via **Launcher Settings -> Cheats**, with build-ID mismatch notice if filename differs from v1.1.2, plus live in-game toggles via **Pause Menu -> Game -> Cheats (v1.1.2)**.
   - **10 Built-in Verified v1.1.2 Patches (`main` build `306dfcfc9eba4a1a`):**
     - `Friendship Warga 2x` / `4x`: `NpcModel::AddLikabilityDegree` (`0x03b70a44`, `0x0b1502e0` -> `0x0b1506e0` / `0x0b150ae0`, gradual multiplier with game clamp & events intact)
     - `Affection Hewan 2x` / `4x`: `AnimalModel::AddAffection` (`0x03b8fec4`, `0x0b140101` -> `0x0b140501` / `0x0b140901`, gradual multiplier with `0..1000` clamp intact)
     - `Stamina Tak Terbatas`: `StaminaModel::Consume` (`0x03cab7f8`, `0x4b1602e0` -> `0x2a1703e0`)
     - `Air Penyiram Tak Habis`: `ItemModel::Use` (`0x03c366e0`, `0x51000508` -> `0xd503201f`)
     - `Waktu Berhenti`: `TimeModel::AdvanceMinute` (`0x01bef750`, `0x52800021` -> `0xd65f03c0`, sleeping/nap still advances day)
     - `Gold / Uang 2x` / `4x`: `PlayerModel::AddMoney` (`0x01a0b74c`, `0x0b140101` -> `0x0b140501` / `0x0b140901`)
     - `Kecepatan Jalan 1.5x`: `FarmPlayerController::ConvertRateToSpeed` (`0x03dd1dac`, `0x52a85909` -> `0x52a850a9`)
