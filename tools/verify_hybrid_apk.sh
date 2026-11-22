#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <path-to-apk>"
    exit 1
fi

APK="$1"
SDK="/home/ilhamziqri/Android/Sdk"
BT="$SDK/build-tools/35.0.0"
NDK_BIN="$SDK/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin"

echo "================================================================"
echo "OFFLINE APK VERIFICATION SUITE: $APK"
echo "================================================================"

echo "[1/7] Size & SHA-256"
ls -lh "$APK"
sha256sum "$APK"

echo ""
echo "[2/7] 16 KB ZIP Alignment Check (zipalign -c -P 16 -v 4)"
"$BT/zipalign" -c -P 16 -v 4 "$APK" | grep -E "\.so|Verification"

echo ""
echo "[3/7] APK Signature Verification (apksigner verify --verbose --print-certs)"
"$BT/apksigner" verify --verbose --print-certs "$APK"

echo ""
echo "[4/7] Manifest & Metadata Badging (aapt2 dump badging)"
"$BT/aapt2" dump badging "$APK" | grep -E "package:|sdkVersion:|targetSdkVersion:|uses-permission:|uses-feature|native-code:|launchable-activity:|application-label:"

echo ""
echo "[5/7] Archive Content, Legal & Storage Mode Audit (zipinfo)"
FORBIDDEN=$(zipinfo -1 "$APK" | grep -iE "\.nsp$|\.xci$|prod\.keys$|title\.keys$|\.patched\.bin$" || true)
if [ -n "$FORBIDDEN" ]; then
    echo "FAIL: Forbidden files found inside APK:"
    echo "$FORBIDDEN"
    exit 2
fi
echo "PASS: 0 forbidden files (.nsp/.xci/prod.keys/title.keys/*.patched.bin) inside APK."
echo "--- Core assets & native libraries storage methods ---"
zipinfo -v "$APK" | awk '/^  [^ ]/ {name=$1} /compression method:/ {print name " -> " $0}' | grep -E "lib/arm64-v8a/|assets/core/|assets/overlay/" || \
  unzip -lv "$APK" | grep -E "lib/arm64-v8a/|assets/core/|assets/overlay/"

echo ""
echo "[6/7] Extracted .so ELF Header, 16 KB LOAD Alignment & NEEDED Audit"
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT
unzip -q "$APK" "lib/arm64-v8a/*.so" "classes*.dex" -d "$TMPD"
for so in "$TMPD"/lib/arm64-v8a/*.so; do
    echo "=== $(basename "$so") ==="
    "$NDK_BIN/llvm-readelf" -h "$so" | grep -E "Class:|Machine:"
    "$NDK_BIN/llvm-readelf" -l "$so" | grep -E "LOAD"
    "$NDK_BIN/llvm-readelf" -d "$so" | grep "NEEDED"
done

echo ""
echo "[7/7] DEX JNI Class & Method Survival Check (dexdump)"
"$BT/dexdump" "$TMPD"/classes*.dex | grep -E "Class descriptor.*Lorg/nx2a/|name.*: '(nativeExtractGame|nativeCancelExtract|nativeQueryFreeSpace|nativeProbeVulkan|onProgress|externalSurface|externalInput|externalAudio|externalAudioSelected|externalHaptic|externalTouch|externalControllerKey|externalControllerMotion|externalOverlay|externalMenuState|externalMenuPixels|readKeyboard|openAudioMenu)'"
echo "================================================================"
echo "ALL OFFLINE CHECKS COMPLETED SUCCESSFULLY"
echo "================================================================"
