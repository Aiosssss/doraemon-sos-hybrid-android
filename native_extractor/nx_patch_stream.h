#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Streaming NXPT (*.nxpatch) applier with < 2 MiB RAM usage (64 KiB LZ4 sliding window).
 * Reads raw NSO and *.nxpatch via streaming I/O, verifies Build ID, raw NSO SHA-256,
 * decompressed base image SHA-256, and final patched output SHA-256.
 * Returns 0 on success; negative error code + human-readable message on failure.
 */
int nx_patch_apply_streaming(const char *raw_nso_path, const char *nxpatch_path,
                             const char *out_path, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
