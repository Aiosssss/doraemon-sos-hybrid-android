// GPU driver quirks: one table of the workarounds the Vulkan backend applies for a GPU or driver (gfx_vk_quirks.c), decided once when the device is made.
// A workaround that only one vendor's hardware or one driver needs is a row there, keyed by vendor, driver and version, instead of a check where it is used.
// Each row has a settings registry switch (DM_VK_QUIRK_<NAME>=0/1, nxnative/dm/settings.tsv) that forces it, to test it on another GPU.
#pragma once
// No includes: gfx_vk.h includes this header at its end, after the Vulkan declarations (VK_NO_PROTOTYPES: the entry points are loaded), and test harnesses
// that copy gfx_vk.h bring their own. Include gfx_vk.h, not this header.
#include <stdint.h>

enum vk_quirk {
    VQ_LRZ_TOUCH,    // Adreno LRZ: one-texel depth reads around passes that load depth (gfx_vk_exec_passes.inc lrz_touch); default of DM_VK_LRZ_TOUCH
    VQ_GS_SPLIT,     // geometry-shader split draws (gfx_vk_gs_split.c) run on this GPU when a profile asks (DM_VK_GS_SPLIT=1)
    VQ_NO_ZEROCOPY,  // ARM Mali (0x13B5): disable AHardwareBuffer zero-copy pools (nx2android-mali integration) and enable cached copied pools + range readback by default
    VQ_NO_UBWC,      // ARM Mali (0x13B5): disable Adreno UBWC VkImageFormatListCreateInfo (default DM_VK_UBWC=0) to avoid Mali DDK AFBC hazards in GENERAL layout / 2x MSAA MRTs
    VQ_NO_CB_SPLIT,  // ARM Mali (0x13B5): disable Adreno mid-frame DM_VK_CB_DRAWS=128 split submits (default 0) so copied pools + deferred UBOs stay coherent per submission
    VQ_NO_A2C,       // ARM Mali (0x13B5): disable alphaToCoverageEnable on 2x MSAA cutout foliage that already executes shader discard (prevents intersecting card transparency flicker)
    VQ_COUNT
};

typedef struct {
    uint32_t vendor, device, version, api;   // VkPhysicalDeviceProperties
    int driver;                              // VkDriverId, 0 when the device does not say (Vulkan 1.1 without VK_KHR_driver_properties)
    char name[64];                           // the driver's own name ("Qualcomm Technologies Inc. Adreno Vulkan Driver", "turnip Mesa driver")
} vk_gpu;

extern unsigned char g_vk_quirks[VQ_COUNT];
extern vk_gpu g_vk_gpu;
void vk_quirks_init(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *p);   // after vkGetPhysicalDeviceProperties: fills both and logs the table
static inline int vk_quirk(enum vk_quirk q) { return g_vk_quirks[q]; }
const char *vk_driver_kind(void);   // "qualcomm", "turnip", "mali", ... or "unknown": for logs and porter's driver choice
