# Doraemon Story of Seasons: Friends of the Great Kingdom — Android ARM64 Hybrid Port

Standalone Android ARM64 (16 KB page-aligned, minSdk 29 / targetSdk 35) native hybrid port for *DORAEMON STORY OF SEASONS: Friends of the Great Kingdom* (`01008EE0177A8000`, Update `1.1.2` / `v262144` + 3 DLCs `{1, 11, 201}`).

> **Legal Notice:** This repository contains **zero** copyrighted Nintendo Switch game binaries, RomFS assets, DLC data, or cryptographic keys (`prod.keys`). Users must legally dump and supply their own `prod.keys`, Base NSP, Update `1.1.2` NSP, and optional DLC NSPs directly on-device via the Android Launcher (SAF).

## Checkpoint Highlights (`v1.1.2-hybrid8`, `versionCode = 8`)
- **Unified ARM Mali + Qualcomm Adreno Vulkan Runtime (`runtime_src/`):**
  - Integrated `nx2android` + `nx2android-mali` GPU quirks (`VQ_NO_ZEROCOPY`, `VQ_NO_UBWC`, `VQ_NO_CB_SPLIT`, `VQ_NO_A2C`, `DM_VK_BC_CACHE=1`).
  - Mali copied-pool CPU-GPU overlap (`DM_VK_POOL_OVERLAP=1`) via staged `vkCmdCopyBuffer` on `s->up`, eliminating the synchronous `fence_wait` stall for 60 FPS unlock on Handheld 720p.
  - Native 2x MSAA with solid cutout foliage (`no_a2c=1`), `firstInstance` VBO offset emulation (`DM_VK_EMU_FIRST_INSTANCE=1`), `DM_VK_UBD_COPYBACK=1` fix, and 1-shot 3D render-graph dumper (`DM_VK_DUMP_3D_FRAME=0/1`).
  - Low-latency present queue (`PQ_N=2`), tightened analog/D-pad deadzones, high-priority input/present/replay threads, and full physical Bluetooth/USB gamepad + rumble + 2P co-op support.
- **Pure C/C++ On-Device Streaming Extractor (`native_extractor/`):**
  - Reads `prod.keys`, Base NSP, Update NSP (including NCA BKTR overlay), and 3x AOC DLCs directly from Android SAF file descriptors (`pread`, low RSS), verifies Build IDs, and applies `NXPT` binary patch recipes (`patches/*.nxpatch`) atomically on-device.
- **Standalone Android Launcher (`android_app/`):**
  - Gradle 8.11.1 + AGP 8.1.0 + JDK 17 + NDK `28.2.13676358`, zero external Maven dependencies (100% offline reproducible).
  - Live Vulkan Mali/Adreno probe, real `env.txt`/`settings.txt` controls (including `0.85x` render scale & 60 FPS unlock), SAF Save Backup/Restore (`.zip`), SAF Log Export, and RomFS Mod Overlay (`.zip`) installer with 1-second original restore.

## Repository Layout
- `android_app/` — Standalone Gradle Android project (`org.nx2a.doraemon`), prebuilt `arm64-v8a` JNI libraries, core `shaders.dmsv` bundle (`5,613` SPIR-V shaders), and `NXPT` patch recipes.
- `native_extractor/` — Pure C/C++ on-device streaming NSP/NCA/BKTR/RomFS extractor & JNI bridge (`libnxhybrid_extract.so`).
- `runtime_src/` — Unified Mali+Adreno `nxnative` Horizon/NVN→Vulkan runtime & Doraemon SoS (`01008EE0177A8000`) profile.
- `patches/` — Unified diff patches against upstream `nx2android`.
- `tools/` — Offline `NXPT` patch generator/applier, `.dmsv` validator, and 7-gate APK verifier (`verify_hybrid_apk.sh`).

## Offline Build Quickstart
```bash
# 1. Build libdmhost.so (ARM64 release, 16 KB aligned)
python3 runtime_src/nxnative/dm/android/build.py \
  --game 01008EE0177A8000 \
  --ndk "$ANDROID_NDK_HOME" \
  --out build_core/host --lib --release
cp build_core/host/libdmhost.so android_app/app/src/main/jniLibs/arm64-v8a/libdmhost.so

# 2. Assemble Release APK, 16 KB zipalign, sign with your own keystore, and verify
cd android_app && gradle --offline --no-daemon :app:assembleRelease
zipalign -P 16 -f 4 app/build/outputs/apk/release/app-release-unsigned.apk app-aligned16k.apk
apksigner sign --ks <your-keystore.jks> --out DoraemonSoS-Hybrid-signed.apk app-aligned16k.apk
bash ../tools/verify_hybrid_apk.sh DoraemonSoS-Hybrid-signed.apk
```
