// GPU driver quirks (gfx_vk_quirks.h): which GPU and driver the device has, and the table of workarounds for it.
// Add a row when a fix is needed by one vendor's hardware or one driver only; keep it off for the others until a device of theirs shows it is needed.
#include "gfx_vk.h"   // the Vulkan declarations, then gfx_vk_quirks.h
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VENDOR_QUALCOMM = 0x5143, VENDOR_ARM = 0x13B5, VENDOR_IMG = 0x1010, VENDOR_SAMSUNG = 0x144D };

unsigned char g_vk_quirks[VQ_COUNT];
vk_gpu g_vk_gpu;

static int adreno(const vk_gpu *g) { return g->vendor == VENDOR_QUALCOMM; }   // both drivers: Qualcomm's own and Mesa's Turnip
static int mali(const vk_gpu *g) { return g->vendor == VENDOR_ARM; }         // ARM Mali / Immortalis (0x13B5)
static int mali_legacy_a2c(const vk_gpu *g) {
    // Only disable A2C by default on legacy Mali drivers (< 40.0 or Vulkan < 1.3).
    // Modern Mali Valhall/5th-gen (Vulkan 1.3, driver >= 40.0, e.g. Mali-G720 r49p1) supports native 2x MSAA A2C,
    // which prevents opaque coverage Z-fighting/flickering on layered foliage cards and tree shadows.
    return g->vendor == VENDOR_ARM && (g->api < VK_API_VERSION_1_3 || g->version < 0x0A000000u);
}

static const struct {
    enum vk_quirk q; const char *name, *setting; int (*match)(const vk_gpu *); const char *why;
} ROWS[] = {
    // found with NEO's menu wipe and Xenoblade Chronicles 3's cloud dome
    {VQ_LRZ_TOUCH, "lrz_touch", "DM_VK_QUIRK_LRZ_TOUCH", adreno,
     "Adreno's low-resolution Z keeps depth from before a command-buffer cut or a pass switch: draws behind objects vanish"},
    // written for NieR:Automata
    {VQ_GS_SPLIT, "gs_split", "DM_VK_QUIRK_GS_SPLIT", adreno,
     "geometry-shader draws split into vertex and fragment passes: written and checked on Adreno only"},
    // integrated from nx2android-mali (gfx_vk_core.c vendorID == 0x13b5)
    {VQ_NO_ZEROCOPY, "no_zerocopy", "DM_VK_QUIRK_NO_ZEROCOPY", mali,
     "ARM Mali gralloc AHardwareBuffer zero-copy causes faults/aborts; use cached copied pools with range readback instead"},
    // hybrid6: Mali Valhall DDK AFBC / 2x MSAA MRT GENERAL-layout resolve + split-submit + 2x MSAA A2C fixes
    {VQ_NO_UBWC, "no_ubwc", "DM_VK_QUIRK_NO_UBWC", mali,
     "Adreno UBWC VkImageFormatListCreateInfo opts Mali Valhall GENERAL-layout 2x MSAA MRTs into AFBC hazards; default DM_VK_UBWC=0 on Mali"},
    {VQ_NO_CB_SPLIT, "no_cb_split", "DM_VK_QUIRK_NO_CB_SPLIT", mali,
     "Adreno 128-draw mid-frame vkQueueSubmit splits are unnecessary on Mali and cause UBO/tile flicker on copied pools; default DM_VK_CB_DRAWS=0 on Mali"},
    {VQ_NO_A2C, "no_a2c", "DM_VK_QUIRK_NO_A2C", mali_legacy_a2c,
     "Legacy Mali (<40.0 / Vulkan <1.3) 2x MSAA alphaToCoverageEnable dither workaround; off by default on modern Mali Vulkan 1.3+ (driver >= 40.0)"},
};
_Static_assert(sizeof ROWS / sizeof *ROWS == VQ_COUNT, "one row a quirk");

const char *vk_driver_kind(void) {
    switch (g_vk_gpu.driver) {
    case VK_DRIVER_ID_QUALCOMM_PROPRIETARY: return "qualcomm";
    case VK_DRIVER_ID_MESA_TURNIP: return "turnip";
    case VK_DRIVER_ID_ARM_PROPRIETARY: return "mali";
    case VK_DRIVER_ID_MESA_PANVK: return "panvk";
    case VK_DRIVER_ID_SAMSUNG_PROPRIETARY: return "xclipse";
    case VK_DRIVER_ID_IMAGINATION_PROPRIETARY: return "powervr";
    default:
        if (g_vk_gpu.vendor == VENDOR_QUALCOMM) return "adreno (driver not named)";
        if (g_vk_gpu.vendor == VENDOR_ARM) return "mali (driver not named)";
        return "unknown";
    }
}

void vk_quirks_init(VkPhysicalDevice pd, const VkPhysicalDeviceProperties *p) {
    vk_gpu *g = &g_vk_gpu; memset(g, 0, sizeof *g);
    g->vendor = p->vendorID; g->device = p->deviceID; g->version = p->driverVersion; g->api = p->apiVersion;
    if (VK_API_VERSION_MINOR(p->apiVersion) >= 2 || VK_API_VERSION_MAJOR(p->apiVersion) > 1) {   // VkPhysicalDeviceDriverProperties is core from Vulkan 1.2
        VkPhysicalDeviceDriverProperties dp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp};
        vkGetPhysicalDeviceProperties2(pd, &p2);
        g->driver = (int)dp.driverID; snprintf(g->name, sizeof g->name, "%s", dp.driverName);
    }
    char list[512]; size_t n = 0; list[0] = 0;
    for (size_t i = 0; i < sizeof ROWS / sizeof *ROWS; i++) {
        int on = ROWS[i].match(g); const char *e = getenv(ROWS[i].setting), *how = "";
        if (e && *e) { on = atoi(e) != 0; how = " (forced)"; }
        g_vk_quirks[ROWS[i].q] = (unsigned char)on;
        n += (size_t)snprintf(list + n, n < sizeof list ? sizeof list - n : 0, "%s%s %s%s", i ? ", " : "", ROWS[i].name, on ? "on" : "off", how);
        if (n >= sizeof list) n = sizeof list - 1;
    }
    VKLOG("driver %s (%s, id %d); quirks: %s", vk_driver_kind(), g->name[0] ? g->name : "no name", g->driver, list);
}
