// Android port, Vulkan backend: device, memory pools, submissions, completion (see gfx_vk.h).
//   Memory pools: NVN gives the GPU the game's own memory (GPU address = CPU address). Each pool's pages are replaced by an AHardwareBuffer mapped at the same
//   address (its dma-buf mmapped MAP_FIXED, contents copied over), and the same AHardwareBuffer is imported as a VkBuffer: the game's pointer is GPU memory,
//   CPU-cached and coherent on Adreno (measured: probes/vkzc.c). A pool the allocator refuses keeps its anonymous pages and gets a GPU copy refreshed before
//   every submission (vk_pools_sync), correct but slow, reported once.
//   Submissions: a ring of slots, each with a command pool (upload + main command buffers), descriptor pools, scratch memory and objects freed once the GPU has
//   passed it. A submission signals a timeline semaphore with its fence; a completion thread waits for it and then applies what the CPU may see of its work
//   (event writes, counter reports), as nvn_mtl.m does in its completion handler.
#define _GNU_SOURCE
#include "gfx_vk.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

#define VK_DEF(f) PFN_##f f;
VK_FNS(VK_DEF) VK_DEV_FNS(VK_DEF)
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
vkdev_t V;
#include "gfx_vk_pipeline_cache.inc"
pthread_mutex_t g_vk_queue_mu = PTHREAD_MUTEX_INITIALIZER;   // VkQueue access is externally synchronized: submissions and presents

// DM_VK_MEM_DIAG=1 at startup: observe copied-pool costs without changing memory protection or rendering.
static int mem_diag_on(void) {
    static _Thread_local int on = -1;
    if (on < 0) { const char *e = getenv("DM_VK_MEM_DIAG"); on = e && atoi(e); }
    return on;
}
static uint64_t mem_diag_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static void mem_diag_zc(unsigned stage, const char *name, uint64_t size, const char *kind, int error) {
    if (!mem_diag_on()) return;
    static _Atomic unsigned seen;
    if (atomic_fetch_or(&seen, 1u << stage) & (1u << stage)) return;
    VKLOG("[memdiag] zero-copy failed at %s: %s=%d, size=%llu, page_size=%d", name, kind, error, (unsigned long long)size, getpagesize());
}
static void mem_diag_note(unsigned phase, uint64_t start, uint64_t bytes) {
    static const char *const names[] = {"completion-wait", "cpu-to-gpu", "gpu-to-cpu", "pool-flush", "pool-invalidate", "merge-scan", "sync-scan", "sync-protect", "sync-upload", "sync-baseline"};
    static _Thread_local struct { uint64_t calls, ns, bytes, last; } stats[10];
    uint64_t end = mem_diag_ns(), elapsed = end - start;
    stats[phase].calls++; stats[phase].ns += elapsed; stats[phase].bytes += bytes;
    if (stats[phase].calls > 3 && end - stats[phase].last < 1000000000ull) return;
    uint64_t faults, preps; nvn_dirty_stats(&faults, &preps);
    // These page counts are process-wide, may overlap other threads, and are not SIGSEGV event counts.
    VKLOG("[memdiag] %s: last %.2f ms / %.2f MiB, mean %.2f ms / %.2f MiB (%llu calls); process fault-released-pages=%llu prep-pages=%llu",
          names[phase], elapsed / 1e6, bytes / 1048576.0, stats[phase].ns / 1e6 / stats[phase].calls,
          stats[phase].bytes / 1048576.0 / stats[phase].calls, (unsigned long long)stats[phase].calls,
          (unsigned long long)faults, (unsigned long long)preps);
    stats[phase].last = end;
}

static void mem_diag_copyback(uint64_t total, uint64_t invalidate, uint64_t calls, uint64_t bytes) {
    static _Thread_local uint64_t n, last, total_ns, invalidate_ns, total_bytes;
    n++; total_ns += total; invalidate_ns += invalidate; total_bytes += bytes;
    uint64_t now = mem_diag_ns(); if (n > 3 && now - last < 1000000000ull) return;
    last = now;
    // Invalidation includes its existing wrapper diagnostics; bytes are logical ranges, not allocation padding.
    VKLOG("[memdiag] copyback-breakdown: last total %.2f ms, invalidate %.2f ms (%.1f%%, %llu calls / %.2f MiB), other %.2f ms; mean total %.2f ms, invalidate %.2f ms (%.1f%%, %.2f MiB), %llu submissions",
        total / 1e6, invalidate / 1e6, total ? 100.0 * invalidate / total : 0.0,
        (unsigned long long)calls, bytes / 1048576.0, (total - invalidate) / 1e6,
        total_ns / 1e6 / n, invalidate_ns / 1e6 / n, total_ns ? 100.0 * invalidate_ns / total_ns : 0.0,
        total_bytes / 1048576.0 / n, (unsigned long long)n);
}

static void mem_diag_baselines(const uint64_t count[4], const uint64_t bytes[4]) {
    static _Thread_local uint64_t calls, last, total_count[4], total_bytes[4];
    for (int i = 0; i < 4; i++) { total_count[i] += count[i]; total_bytes[i] += bytes[i]; }
    uint64_t now = mem_diag_ns(); if (++calls > 3 && now - last < 1000000000ull) return;
    last = now;
    static const char *const names[] = {"resync", "untracked", "incomplete-uncached", "incomplete-other"};
    for (int i = 0; i < 4; i++) VKLOG("[memdiag] baseline-reason %s: last %llu copies / %.2f MiB; total %llu copies / %.2f MiB (%llu syncs)",
        names[i], (unsigned long long)count[i], bytes[i] / 1048576.0, (unsigned long long)total_count[i], total_bytes[i] / 1048576.0, (unsigned long long)calls);
}

uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want && !(V.mem.memoryTypes[i].propertyFlags & avoid)) return i;
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static void mem_diag_pool(uint64_t size, uint32_t bits, uint32_t selected) {
    if (!mem_diag_on()) return;
    VkMemoryPropertyFlags flags = V.mem.memoryTypes[selected].propertyFlags;
    uint32_t cached = vk_mem_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    VKLOG("[memdiag] copied buffer %.2f MiB: allowed-types %#x, selected type %u flags %#x (host-cached %s); cached candidate %d (candidate lookup only)",
        size / 1048576.0, bits, selected, flags, flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "yes" : "no", cached == UINT32_MAX ? -1 : (int)cached);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
static int is_on(const char *const *on, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(on[i], name)) return 1; return 0; }
// a feature struct as vk_init queries it (sType, pNext, then VkBool32s; the largest, descriptor indexing, has 20); featx: its VkBool32s, all off if it is not chained
typedef struct { VkBaseOutStructure h; VkBool32 b[24]; } featx_t;
static VkBool32 *featx(featx_t *x, uint32_t n, VkStructureType s) { static VkBool32 off[24]; for (uint32_t i = 0; i < n; i++) if (x[i].h.sType == s) return x[i].b; return off; }   // (off: only 0 is written to it)

static void completion_start(void);
static void memlog_start(void);
static void slots_init(void);
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_validation_msg(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    (void)type; (void)u;   // each distinct message ID logged its first 3 times
    enum { NID = 512 }; static int32_t ids[NID]; static uint8_t cnt[NID]; static int nid; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu); int k = 0; for (; k < nid; k++) if (ids[k] == d->messageIdNumber) break;
    if (k == nid && nid < NID) ids[nid++] = d->messageIdNumber;
    int show = k < NID && cnt[k] < 3; if (k < NID && cnt[k] < 255) cnt[k]++; pthread_mutex_unlock(&mu);
    if (show) VKLOG("[RED] validation %s: %s", sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", d->pMessage);
    return VK_FALSE;
}

// Whether a driver offers a Vulkan 1.3 GPU (DM_VK_DRIVER: a Turnip built for another Adreno generation loads and then has none)
static int drv_has_gpu(PFN_vkGetInstanceProcAddr g) {
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)g(NULL, "vkCreateInstance"); VkInstance in; int ok = 0;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmhost", 1, "nxnative", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (!ci || ci(&ici, NULL, &in)) return 0;
    PFN_vkEnumeratePhysicalDevices ep = (PFN_vkEnumeratePhysicalDevices)g(in, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)g(in, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)g(in, "vkDestroyInstance");
    VkPhysicalDevice pd[8]; uint32_t n = 8;
    if (ep && gp && ep(in, &n, pd) >= 0) for (uint32_t i = 0; i < n && !ok; i++) { VkPhysicalDeviceProperties p; gp(pd[i], &p); ok = p.apiVersion >= VK_API_VERSION_1_3;
        VKLOG("DM_VK_DRIVER: %s, Vulkan %u.%u, driver version 0x%x", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion); }
    if (di) di(in, NULL);
    return ok;
}

static int pool_cached_default(void);
static int pool_readback_ranges_default(void);
void vk_init(void) {
    static int done; if (done) return; done = 1;
    // DM_VK_DRIVER=<library>: a Vulkan driver the APK carries (Mesa's Turnip for Adreno; porter --vulkan-driver), loaded through libadrenotools (BSD-2-Clause), which the
    // APK carries beside it with its hook libraries: a second copy of the system's loader in a linker namespace of its own, whose driver is this library. The loader keeps
    // making the surface and the swapchain, which Turnip on Android has none of (the platform loader makes them over VK_ANDROID_native_buffer). TOTKDroid, a port of Tears
    // of the Kingdom 1.4.3 that also replays NVN on Vulkan, measured its Great Sky Island forest at GPU 16-17 ms a frame on Turnip on an Adreno 830 (docs/FPS-2026-10-31.md),
    // against 22-27 ms here on Qualcomm's driver. A driver that does not load, or offers no Vulkan 1.3 GPU, leaves the system's
    const char *drv = getenv("DM_VK_DRIVER"); void *lib = NULL;
    if (drv && *drv) {
        Dl_info di; char dir[512] = "", dirs[520] = "";   // the APK's extracted libraries (mkapk.py: extractNativeLibs), where adrenotools finds its hooks and the driver
        if (dladdr((void *)vk_init, &di) && di.dli_fname && strrchr(di.dli_fname, '/')) {
            snprintf(dir, sizeof dir, "%.*s", (int)(strrchr(di.dli_fname, '/') - di.dli_fname), di.dli_fname); snprintf(dirs, sizeof dirs, "%s/", dir); }
        void *at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
        void *(*open_vk)(int, int, const char *, const char *, const char *, const char *, const char *, void **) =
            at ? (void *(*)(int, int, const char *, const char *, const char *, const char *, const char *, void **))dlsym(at, "adrenotools_open_libvulkan") : NULL;
        if (!open_vk || !*dir) VKLOG("[RED] DM_VK_DRIVER %s: no libadrenotools in the APK (%s); the system's driver instead", drv, at ? "no adrenotools_open_libvulkan" : dlerror());
        else if (!(lib = open_vk(RTLD_NOW | RTLD_LOCAL, 1 /* ADRENOTOOLS_DRIVER_CUSTOM */, NULL, dir, dirs, drv, NULL, NULL)))
            VKLOG("[RED] DM_VK_DRIVER %s: adrenotools could not load it from %s; the system's driver instead", drv, dir);
        else if (!(p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr")) || !drv_has_gpu(p_vkGetInstanceProcAddr)) {
            VKLOG("[RED] DM_VK_DRIVER %s: no Vulkan 1.3 GPU through it; the system's driver instead", drv); lib = NULL; }   // (left loaded: a driver does not always unload cleanly)
        else VKLOG("Vulkan driver %s through adrenotools, not the system's (%s)", drv, dir);
    }
    if (!lib) {
        if (!(lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL))) die("[vk] libvulkan.so: %s", dlerror());
        p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    }
    vkCreateInstance = (PFN_vkCreateInstance)p_vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    const char *iext[] = {"VK_KHR_surface", "VK_KHR_android_surface", "VK_EXT_debug_utils"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmhost", 1, "nxnative", 1, VK_API_VERSION_1_3};
    // DM_VK_VALIDATE=1: Khronos's validation layer, which the APK carries only when built with it (mkapk.py --extra-so); its messages go to the log
    int validate = getenv("DM_VK_VALIDATE") && atoi(getenv("DM_VK_VALIDATE"));
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, validate ? 1u : 0u, layers, validate ? 3u : 2u, iext};
    VkResult ir = vkCreateInstance(&ici, NULL, &V.inst);
    if (ir && validate) { VKLOG("[RED] validation layer not available (%d): running without it", ir); validate = 0; ici.enabledLayerCount = 0; ici.enabledExtensionCount = 2; ir = vkCreateInstance(&ici, NULL, &V.inst); }
    VKCHK(ir);
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT mk = (PFN_vkCreateDebugUtilsMessengerEXT)p_vkGetInstanceProcAddr(V.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, vk_validation_msg, NULL};
        VkDebugUtilsMessengerEXT m; if (mk && !mk(V.inst, &mi, NULL, &m)) VKLOG("validation layer on");
    }
#define VK_LOADI(f) f = (PFN_##f)p_vkGetInstanceProcAddr(V.inst, #f);
    VK_FNS(VK_LOADI)
    uint32_t n = 1; VKCHK(vkEnumeratePhysicalDevices(V.inst, &n, &V.pd)); if (!n) die("[vk] no GPU");
    vkGetPhysicalDeviceProperties(V.pd, &V.props); vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mem);
    // the device names itself before any check below can stop: a Moto g 5G 2024's log (Adreno 619) ended at the timeline check with no GPU or driver named.
    // driver: as Qualcomm encodes it (VK_MAKE_VERSION: 512.530.0), with the raw value for other vendors
    { uint32_t d = V.props.driverVersion;
      VKLOG("GPU %s (vendor %#x, device %#x), Vulkan %u.%u.%u, driver %u.%u.%u (%#x)", V.props.deviceName, V.props.vendorID, V.props.deviceID, VK_API_VERSION_MAJOR(V.props.apiVersion),
            VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), d >> 22, d >> 12 & 0x3ff, d & 0xfff, d); }
    vk_quirks_init(V.pd, &V.props);   // the driver and its quirk table (gfx_vk_quirks.c), before anything that depends on them
    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, NULL);
    VkQueueFamilyProperties qp[16]; if (qn > 16) qn = 16; vkGetPhysicalDeviceQueueFamilyProperties(V.pd, &qn, qp);
    V.qf = 0; while (V.qf < qn && (qp[V.qf].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) V.qf++;
    if (V.qf == qn) die("[vk] no graphics + compute queue");
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(V.pd, NULL, &ne, ex);
    const char *want[] = {"VK_KHR_swapchain", "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_EXT_custom_border_color",
                          "VK_EXT_robustness2", "VK_EXT_depth_clip_control", "VK_EXT_provoking_vertex", "VK_EXT_index_type_uint8", "VK_EXT_transform_feedback",
                          "VK_GOOGLE_display_timing"};
    const char *on[64]; uint32_t non = 0;
    for (unsigned i = 0; i < sizeof want / sizeof *want; i++) if (has_ext(ex, ne, want[i])) on[non++] = want[i]; else VKLOG("device has no %s", want[i]);
    // NVN vertex streams step per N instances (gfx_vk_exec.c build_pipeline's divisor state): VK_EXT_vertex_attribute_divisor, or the KHR one (core in Vulkan 1.4, the same
    // structures and type values) on a device that offers only that (the Mali-G715). Not enabled before, while pipelines chained its state all the same (invalid usage)
    const char *div_ext = has_ext(ex, ne, "VK_EXT_vertex_attribute_divisor") ? "VK_EXT_vertex_attribute_divisor" : has_ext(ex, ne, "VK_KHR_vertex_attribute_divisor") ? "VK_KHR_vertex_attribute_divisor" : NULL;
    if (div_ext) on[non++] = div_ext;
    // A Vulkan 1.1 or 1.2 device (Legion Y700 2023, Adreno 730: its stock driver reports 1.1) has what the replay uses from 1.2 / 1.3 as extensions: they are enabled
    // when present, so the features chained below and the functions loaded by their KHR names are valid there. A 1.3 device needs none of them
    int v12 = V.props.apiVersion >= VK_API_VERSION_1_2, v13 = V.props.apiVersion >= VK_API_VERSION_1_3; uint32_t npromo = 0;
    static const char *promo12[] = {"VK_KHR_create_renderpass2", "VK_KHR_imageless_framebuffer", "VK_KHR_image_format_list", "VK_KHR_depth_stencil_resolve", "VK_KHR_timeline_semaphore",
        "VK_KHR_shader_float16_int8", "VK_KHR_8bit_storage", "VK_EXT_descriptor_indexing", "VK_EXT_scalar_block_layout", "VK_KHR_uniform_buffer_standard_layout",
        "VK_KHR_shader_float_controls", "VK_KHR_spirv_1_4", "VK_KHR_vulkan_memory_model", "VK_KHR_separate_depth_stencil_layouts", "VK_EXT_host_query_reset",
        "VK_KHR_sampler_mirror_clamp_to_edge", "VK_KHR_draw_indirect_count", "VK_KHR_buffer_device_address", "VK_EXT_shader_viewport_index_layer", "VK_EXT_sampler_filter_minmax",
        "VK_KHR_shader_subgroup_extended_types", "VK_KHR_shader_atomic_int64", "VK_KHR_driver_properties"};
    static const char *promo13[] = {"VK_EXT_image_robustness", "VK_KHR_dynamic_rendering", "VK_KHR_synchronization2", "VK_EXT_shader_demote_to_helper_invocation",
        "VK_EXT_subgroup_size_control", "VK_KHR_copy_commands2", "VK_KHR_maintenance4", "VK_KHR_format_feature_flags2", "VK_EXT_extended_dynamic_state",
        "VK_KHR_zero_initialize_workgroup_memory", "VK_KHR_shader_terminate_invocation", "VK_EXT_inline_uniform_block", "VK_EXT_pipeline_creation_cache_control",
        "VK_EXT_texel_buffer_alignment", "VK_EXT_4444_formats", "VK_KHR_shader_integer_dot_product", "VK_EXT_private_data"};
    for (unsigned i = 0; !v12 && i < sizeof promo12 / sizeof *promo12 && non < 64; i++) if (has_ext(ex, ne, promo12[i])) { on[non++] = promo12[i]; npromo++; }
    for (unsigned i = 0; !v13 && i < sizeof promo13 / sizeof *promo13 && non < 64; i++) if (has_ext(ex, ne, promo13[i])) { on[non++] = promo13[i]; npromo++; }
    // features: what the replay uses when the device has it. VkPhysicalDeviceVulkan11/12/13Features are valid only on a 1.2 / 1.3 device (a 1.1 driver ignores
    // them and nothing would be enabled); below that, the same features come through Vulkan 1.1's own structs and those of the extensions enabled above (xf)
    static const struct { const char *ext; VkStructureType s; } XF[] = {   // ext NULL: Vulkan 1.1 core, VkPhysicalDeviceVulkan11Features's members
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES},
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES},
        {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES}, {NULL, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES},
        {"VK_KHR_imageless_framebuffer", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES}, {"VK_KHR_timeline_semaphore", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES},
        {"VK_KHR_shader_float16_int8", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES}, {"VK_KHR_8bit_storage", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES},
        {"VK_EXT_descriptor_indexing", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES}, {"VK_EXT_scalar_block_layout", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES},
        {"VK_KHR_uniform_buffer_standard_layout", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES}, {"VK_KHR_vulkan_memory_model", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES},
        {"VK_KHR_separate_depth_stencil_layouts", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES}, {"VK_EXT_host_query_reset", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES},
        {"VK_KHR_buffer_device_address", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES}, {"VK_KHR_shader_subgroup_extended_types", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES},
        {"VK_KHR_shader_atomic_int64", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES},   // (mirror clamp, indirect count, minmax, viewport layer: the extension is the feature)
        {"VK_KHR_dynamic_rendering", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES}, {"VK_KHR_synchronization2", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES},
        {"VK_EXT_shader_demote_to_helper_invocation", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES}, {"VK_EXT_subgroup_size_control", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES},
        {"VK_KHR_maintenance4", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES}, {"VK_KHR_zero_initialize_workgroup_memory", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES},
        {"VK_KHR_shader_terminate_invocation", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES}, {"VK_EXT_inline_uniform_block", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES},
        {"VK_EXT_pipeline_creation_cache_control", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES}, {"VK_KHR_shader_integer_dot_product", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES},
        {"VK_EXT_private_data", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES},
        {"VK_EXT_texel_buffer_alignment", VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT}};   // (VK_EXT_image_robustness: robustImageAccess stays off, as on 1.3)
    uint32_t nxf = sizeof XF / sizeof *XF; featx_t xf[sizeof XF / sizeof *XF]; memset(xf, 0, sizeof xf); VkBaseOutStructure *xs = NULL;
    for (int i = (int)nxf - 1; i >= 0; i--) if (XF[i].ext ? is_on(on, non, XF[i].ext) : !v12) { xf[i].h.sType = XF[i].s; xf[i].h.pNext = xs; xs = &xf[i].h; }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, xs};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13 ? (void *)&f13 : xs};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 ff = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v12 ? (void *)&f11 : xs};
    vkGetPhysicalDeviceFeatures2(V.pd, &ff);
    // without timeline semaphores (Vulkan 1.1 drivers: the Adreno 618 / 619 of 2021, 512.530) each submission signals a fence of a ring instead (slots_init).
    // DM_VK_X_FENCES=1 (start-up): the fences on a device that has them; with DM_VK_X_BC_CPU=1 below, a device of today stands in for those drivers
    { const char *e = getenv("DM_VK_X_FENCES");
      V.has_timeline = (v12 ? f12.timelineSemaphore : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)[0]) && !(e && atoi(e)); }
    if (!V.has_timeline) VKLOG("no timeline semaphores (Vulkan 1.2 or VK_KHR_timeline_semaphore): submissions signal fences");
    // dynamic rendering is 1.3 or its extension with the feature on; a loaded pointer does not tell, since the loader hands out a trampoline for core functions the driver lacks
    V.has_dynrender = v13 ? f13.dynamicRendering : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)[0];
    // imageless framebuffers need render pass 2 (gfx_vk_exec.c rp_get): core in 1.2, else its extension
    V.has_draw_count = v12 ? f12.drawIndirectCount : is_on(on, non, "VK_KHR_draw_indirect_count");
    V.has_imageless = v12 ? f12.imagelessFramebuffer : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES)[0] && is_on(on, non, "VK_KHR_create_renderpass2");
    V.use_classic_fb = 0;
    if (!V.has_imageless && !V.has_dynrender) {
        V.use_classic_fb = 1;
        VKLOG("Vulkan %u.%u fallback: neither dynamicRendering nor imagelessFramebuffer — activating classic vkCreateRenderPass + cached VkFramebuffer",
              VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion));
    }
    if (!v13) VKLOG("Vulkan %u.%u device: %u extensions enabled for the features promoted to 1.2 / 1.3; dynamic rendering %s", VK_API_VERSION_MAJOR(V.props.apiVersion),
                    VK_API_VERSION_MINOR(V.props.apiVersion), npromo, V.has_dynrender ? "yes" : (V.use_classic_fb ? "no (classic VkRenderPass + VkFramebuffer)" : "no (render pass objects only)"));
    VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceRobustness2FeaturesEXT fr2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &fcb};
    VkPhysicalDeviceDepthClipControlFeaturesEXT fdc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &fr2};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT fu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, &fdc};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT fxf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &fu8};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, &fxf};
    VkPhysicalDeviceFeatures2 fx = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fdv};
    vkGetPhysicalDeviceFeatures2(V.pd, &fx);
    V.has_divisor = div_ext && fdv.vertexAttributeInstanceRateDivisor;
    V.has_nonzero_first_inst = 1;
    if (!V.has_divisor) VKLOG("no vertexAttributeInstanceRateDivisor (%s): vertex streams stepping per N > 1 instances are read per instance", div_ext ? div_ext : "no vertex attribute divisor extension");
    else if (div_ext[3] == 'K') {   // the KHR one says whether a draw may start past instance 0 with such a stream (the EXT one always may)
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR dvp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dvp}; vkGetPhysicalDeviceProperties2(V.pd, &p2);
        V.has_nonzero_first_inst = dvp.supportsNonZeroFirstInstance ? 1 : 0;
        VKLOG("vertex streams stepping per N instances: VK_KHR_vertex_attribute_divisor%s",
              V.has_nonzero_first_inst ? "" : " (supportsNonZeroFirstInstance=0: emulating non-zero firstInstance via per-instance vertex buffer offsets)");
    }
    // transform feedback (NVN BeginTransformFeedback; gfx_vk_exec.c xfb_*): Xenoblade Chronicles 3 captures particle state with it
    V.has_xfb = fxf.transformFeedback && has_ext(ex, ne, "VK_EXT_transform_feedback"); fxf.geometryStreams = 0;
    int robust = 0; { const char *e = getenv("DM_VK_ROBUST"); robust = e ? atoi(e) : 0; }   // DM_VK_ROBUST (start-up, A/B): 1 robust buffer access, 2 also robust image access and robustness2's
    if (robust < 2) { fr2.robustBufferAccess2 = 0; fr2.robustImageAccess2 = 0; }   // nullDescriptor only: unbound slots read 0 without a dummy
    V.has_custom_border = fcb.customBorderColors && has_ext(ex, ne, "VK_EXT_custom_border_color");
    V.has_robust2 = fr2.nullDescriptor && has_ext(ex, ne, "VK_EXT_robustness2");
    V.has_depth_clip_ctl = fdc.depthClipControl && has_ext(ex, ne, "VK_EXT_depth_clip_control");
    // Samsung Xclipse Safe Render Path (DM_VK_SAMSUNG_SAFE, auto-ON on vendor 0x144D Xclipse 940/920):
    // Mirrors exact Mali-safe feature set so 3D skinned characters render reliably on Exynos RDNA3:
    // disables depth_clip_control, robustness2 nullDescriptor, dynamicRendering (plus disable_msaa=1 in gfx_vk_tex.c),
    // and EXCLUSIVELY on vendor 0x144D disables timeline semaphores (switches to classic VkFence).
    if (vk_samsung_safe()) {
        V.has_depth_clip_ctl = 0; fdc.depthClipControl = 0;
        V.has_robust2 = 0; fr2.nullDescriptor = 0; fr2.robustBufferAccess2 = 0; fr2.robustImageAccess2 = 0;
        V.has_dynrender = 0; f13.dynamicRendering = 0;
        featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES)[0] = 0;
        if (!V.has_imageless) V.use_classic_fb = 1;
        if (V.props.vendorID == 0x144Du) {
            V.has_timeline = 0;
            f12.timelineSemaphore = 0;
            featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)[0] = 0;
        }
        VKLOG("[samsung-safe] Xclipse isolated safe mode active (vendor=%#x dev=%s): msaa=1, depth_clip_control=0, robustness2=0, dynamicRendering=0, timeline=%d, def_wait=%d, inline_resolve=%d, strict_barriers=%d, classic_fb=%d",
              V.props.vendorID, V.props.deviceName, V.has_timeline, vk_deferred_wait(), vk_inline_resolve(), vk_strict_barriers(), V.use_classic_fb);
    }
    V.has_depth_bias_clamp = ff.features.depthBiasClamp ? 1 : 0;
    V.has_dtiming = has_ext(ex, ne, "VK_GOOGLE_display_timing");   // frame pacing on the display (gfx_vk_present.c pace_*)
    V.has_bc = ff.features.textureCompressionBC; V.has_astc = ff.features.textureCompressionASTC_LDR; V.has_gs = ff.features.geometryShader;
    V.has_gs_pointsize = ff.features.shaderTessellationAndGeometryPointSize;
    { const char *e = getenv("DM_VK_X_BC_CPU"); if (e && atoi(e)) V.has_bc = 0; }   // (start-up) BC textures decoded on the CPU as on a device without them (gfx_vk_tex.c bc_decode)
    V.max_aniso = ff.features.samplerAnisotropy ? fminf(16, V.props.limits.maxSamplerAnisotropy) : 1;
    // subgroups: where the shaders' votes, ballots and shuffles may run; a module whose stage lacks them runs as subgroups of one invocation (gfx_vk_shader.c, spv_one_lane).
    // Turnip lists graphics stages and quad operations only on a GPU with getfiberid (tu_device.cc): the Adreno 7xx have it, the Adreno 650 (a6xx gen3) not (compute alone).
    // DM_VK_SUBGROUP_PIN=<lanes> (start-up): game pipelines' fragment and compute stages run at that subgroup size (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo)
    // where requiredSubgroupSizeStages allows it; by default the driver picks per pipeline (Turnip on Adreno 6xx / 7xx: 64 or 128, only those two stages pinnable)
    { VkPhysicalDeviceTexelBufferAlignmentProperties tba = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};
      VkPhysicalDeviceSubgroupSizeControlProperties sc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
      VkPhysicalDeviceSubgroupProperties sg = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, v13 || has_ext(ex, ne, "VK_EXT_subgroup_size_control") ? (void *)&sc : NULL};
      if (v13 || has_ext(ex, ne, "VK_EXT_texel_buffer_alignment")) {
          tba.pNext = (void *)sg.pNext;
          sg.pNext = (void *)&tba;
      }
      VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg}; vkGetPhysicalDeviceProperties2(V.pd, &p2);
      int tbuf_feat = v13 || featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT)[0];   // on 1.1/1.2 the relaxed offsets are valid only with the feature enabled (VUID-VkBufferViewCreateInfo-offset-02749); without it V.tbuf_* stay 0 and minTexelBufferOffsetAlignment applies
      if (tbuf_feat) {
          V.tbuf_align_uniform = tba.uniformTexelBufferOffsetAlignmentBytes;
          V.tbuf_single_uniform = tba.uniformTexelBufferOffsetSingleTexelAlignment;
          V.tbuf_align_storage = tba.storageTexelBufferOffsetAlignmentBytes;
          V.tbuf_single_storage = tba.storageTexelBufferOffsetSingleTexelAlignment;
          VKLOG("texel buffer alignment: uniform %llu B (single: %s), storage %llu B (single: %s), minTexelBufferOffsetAlignment %llu B",
                (unsigned long long)V.tbuf_align_uniform, V.tbuf_single_uniform ? "yes" : "no",
                (unsigned long long)V.tbuf_align_storage, V.tbuf_single_storage ? "yes" : "no",
                (unsigned long long)V.props.limits.minTexelBufferOffsetAlignment);
      }
      int ctl = v13 ? f13.subgroupSizeControl : featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES)[0];   // (enabled below with the rest of the chain)
      V.sg_size = sg.subgroupSize; V.sg_stages = sg.supportedStages; V.sg_ops = sg.supportedOperations; V.sg_quad_all = sg.quadOperationsInAllStages;
      V.sg_min = sc.minSubgroupSize ? sc.minSubgroupSize : sg.subgroupSize; V.sg_max = sc.maxSubgroupSize ? sc.maxSubgroupSize : sg.subgroupSize; V.sg_maxwgs = sc.maxComputeWorkgroupSubgroups;
      uint32_t pinnable = ctl ? sc.requiredSubgroupSizeStages : 0, st = pinnable & (VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
      VKLOG("subgroups: size %u (%u-%u), stages %#x, ops %#x, quad all stages %s, pinnable %#x", V.sg_size, V.sg_min, V.sg_max, V.sg_stages, V.sg_ops, V.sg_quad_all ? "yes" : "no", pinnable);
      const char *e = getenv("DM_VK_SUBGROUP_PIN"); uint32_t pin = (e && *e) ? (uint32_t)atoi(e) : 0;
      // Samsung Xclipse 940 (AMD RDNA3, vendor 0x144d) and other Wave64 GPUs report subgroupSize=64 (32-64) with
      // compute pinnable (0x20). Switch shaders are authored for Maxwell Warp32: auto-pin compute shaders to 32 lanes!
      int auto_pin_cs = 0;
      if (!e && V.sg_size > 32 && V.sg_min <= 32 && (pinnable & VK_SHADER_STAGE_COMPUTE_BIT)) {
          pin = 32; st = VK_SHADER_STAGE_COMPUTE_BIT; auto_pin_cs = 1;
      }
      if (pin && (!st || (pin & (pin - 1)) || pin < V.sg_min || pin > V.sg_max)) VKLOG("[RED] DM_VK_SUBGROUP_PIN=%u: the device cannot pin fragment or compute shaders to it (sizes %u-%u, pinnable stages %#x): the driver picks", pin, V.sg_min, V.sg_max, pinnable);
      else if (pin) { V.sg_pin_stages = st; V.sg_pin = (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, pin};
                      VKLOG("subgroups: stages %#x pinned to %u lanes (%s, vendor=%#x)", st, pin, auto_pin_cs ? "auto Wave32 compute pin" : "DM_VK_SUBGROUP_PIN", V.props.vendorID); } }
    // Load memory tier (DM_LOW_RAM) & low-end Mali profile EARLY — BEFORE vkCreateDevice and unified policy —
    // so MSAA, render scale, texture downscale, and shader robustness are active from frame 0.
    int low_ram_early = vk_low_ram_tier();
    int mali_low_early = vk_mali_low_end();
    // enable: everything the shaders may use that the device has
    VkPhysicalDeviceFeatures en = ff.features; en.robustBufferAccess = 0; en.shaderFloat64 = 0;
    // Default robustBufferAccess = 1 on ALL GPUs with hardware support (Mali & Adreno):
    // Nintendo Switch Maxwell returns 0.0f on constant-buffer/VBO/SSBO reads past bound range; with
    // robustBufferAccess=0 on Mali, Unity's small UBO bindings read adjacent scratch/pool garbage/NaN
    // and trigger 1-frame 3D red-orange flashes even under safe_sync=2 serial mode! Hardware LSU bounds
    // checks cost only ~0.2-0.3 ms. Set DM_VK_ROBUST_ACCESS=0 explicitly only for A/B testing.
    int robust_opt = 1;
    { const char *e = getenv("DM_VK_ROBUST_ACCESS"); if (e && *e) robust_opt = atoi(e) != 0; }
    int prev_dev_lost = 0;
    {
        const char *root = getenv("DM_ROOT");
        if (root && *root) {
            char mp[1024]; snprintf(mp, sizeof mp, "%s/.gpu_device_lost", root);
            struct stat st; if (stat(mp, &st) == 0) prev_dev_lost = 1;
        }
    }
    if ((robust || robust_opt || (vk_mali_old_driver() && prev_dev_lost)) && ff.features.robustBufferAccess) {
        en.robustBufferAccess = 1;
        V.robust_buf = 1;
        VKLOG("robust buffer access on (%s)%s",
              robust ? "DM_VK_ROBUST" : (robust_opt ? "default/DM_VK_ROBUST_ACCESS=1" : "auto recovery after .gpu_device_lost"),
              robust >= 2 ? ", with robust image access and robustness2 where the device has them" : "");
    } else { V.robust_buf = 0; fr2.robustBufferAccess2 = 0; }   // (robustBufferAccess2 needs robustBufferAccess)
    f12.bufferDeviceAddressCaptureReplay = 0; f12.bufferDeviceAddressMultiDevice = 0;
    featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[1] = featx(xf, nxf, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)[2] = 0;   // (its extension's: the same two)
    if (robust < 2) f13.robustImageAccess = 0;
    fcb.pNext = NULL; fr2.pNext = V.has_custom_border ? (void *)&fcb : NULL; fdc.pNext = V.has_robust2 ? (void *)&fr2 : (V.has_custom_border ? (void *)&fcb : NULL);
    fu8.pNext = V.has_depth_clip_ctl ? (void *)&fdc : fdc.pNext;
    fxf.pNext = &fu8;
    VkBaseOutStructure *t = ff.pNext; while (t->pNext) t = t->pNext;   // the queried chain (core / promoted features), then the extensions'
    fdv.pNext = V.has_xfb ? (void *)&fxf : (void *)&fu8;
    t->pNext = V.has_divisor ? (VkBaseOutStructure *)&fdv : V.has_xfb ? (VkBaseOutStructure *)&fxf : (VkBaseOutStructure *)&fu8;
    VkPhysicalDeviceFeatures2 enf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, ff.pNext, en};
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, V.qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &enf, 0, 1, &qci, 0, NULL, non, on, NULL};
    VKCHK(vkCreateDevice(V.pd, &dci, NULL, &V.dev));
    vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)p_vkGetInstanceProcAddr(V.inst, "vkGetDeviceProcAddr");
    // On Vulkan 1.1 devices (e.g. Mali-G57 r32p1), prefer the *KHR entry point for promoted 1.2/1.3 functions
    // so Android's Vulkan 1.3 loader cannot hand back a broken core trampoline over a 1.1 ICD.
#define VK_LOADD(f) do { \
        f = NULL; \
        if (!v12) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f "KHR"); \
        if (!f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f); \
        if (!f && v12) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f "KHR"); \
        if (!f) f = (PFN_##f)p_vkGetInstanceProcAddr(V.inst, #f); \
    } while (0);
    VK_DEV_FNS(VK_LOADD)
    if (V.has_imageless && !vkCreateRenderPass2) {
        VKLOG("[vk] imagelessFramebuffer reported but vkCreateRenderPass2(KHR) failed to load: falling back to classic VkRenderPass + VkFramebuffer");
        V.has_imageless = 0;
        if (!V.has_dynrender) V.use_classic_fb = 1;
    }
    if (V.has_dynrender && (!vkCmdBeginRendering || !vkCmdEndRendering)) {
        VKLOG("[vk] dynamicRendering reported but vkCmdBeginRendering(KHR) did not load: falling back to render pass objects");
        V.has_dynrender = 0;
        if (!V.has_imageless) V.use_classic_fb = 1;
    }
    // A driver can report the timeline-semaphore feature (and Vulkan 1.3) and still export no vkWaitSemaphores: the emulator's ranchu / gfxstream driver does
    // (both the device and the instance dispatcher return null for it and for vkGetSemaphoreCounterValue), and the first wait then called address 0
    // (SIGSEGV pc=0, lr=wait_fence+0x70, thread 'dm-vk-complete', South Park: The Stick of Truth). The feature query is not proof the entry points load:
    // without them the fence ring is used for both the submit and the wait, which V.has_timeline decides.
    if (V.has_timeline && (!vkWaitSemaphores || !vkGetSemaphoreCounterValue)) {
        VKLOG("[vk] timeline semaphores reported but vkWaitSemaphores / vkGetSemaphoreCounterValue did not load: submissions signal fences");
        V.has_timeline = 0;
    }
    { void vk_dq_init(void); vk_dq_init(); }   // DM_VK_MT_REPLAY: the vkCmd* calls through the recorder thread (gfx_vk_dq.c)
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    size_t dn = pcache_load(); free(ex);
    VKLOG("%s, Vulkan %u.%u, driver %#x; timeline semaphores %s, BC %s, ASTC %s, geometry shaders %s, nullDescriptor %s, custom border colours %s, depth clip control %s, transform feedback %s; pipeline cache %zu bytes",
          V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), V.props.driverVersion, V.has_timeline ? "yes" : "no (fences)",
          V.has_bc ? "yes" : "no (decoded on the CPU)", V.has_astc ? "yes" : "no", V.has_gs ? "yes" : "no", V.has_robust2 ? "yes" : "no", V.has_custom_border ? "yes" : "no", V.has_depth_clip_ctl ? "yes" : "no", V.has_xfb ? "yes" : "no", dn);
    {
        int mali_zc_off = vk_quirk(VQ_NO_ZEROCOPY);
        int pool_cached = pool_cached_default();
        int rb_ranges = pool_readback_ranges_default();
        int no_msaa = vk_disable_msaa();
        int safe_sync = vk_pool_safe_sync();
        int pool_ovl = vk_pool_overlap();
        int ubd_cb = vk_ubd_copyback();
        int dump_3d = vk_dump_3d_frame();
        int strict_bar = vk_strict_barriers();
        int emu_fi = vk_emu_first_instance();
        int no_a2c = vk_no_a2c();
        float rscale = vk_render_scale();
        int tex_ds = vk_tex_downscale();
        const char *ubwc_e = getenv("DM_VK_UBWC");
        int ubwc_val = (ubwc_e && *ubwc_e) ? atoi(ubwc_e) : (vk_quirk(VQ_NO_UBWC) ? 0 : 3);
        const char *cbd_e = getenv("DM_VK_CB_DRAWS");
        int cbd_val = (cbd_e && *cbd_e) ? atoi(cbd_e) : ((vk_quirk(VQ_NO_CB_SPLIT) || pool_ovl) ? 0 : 128);
        VKLOG("unified Mali+Adreno policy: vendor=%#x (%s), gpu_tier=%d, low_ram=%d, mali_low_end=%d, ultra_low_end=%d, bifrost_legacy=%d, samsung_safe=%d, classic_fb=%d, bc_path=%s, zerocopy=%s, pool_cached=%d, readback_ranges=%d, disable_msaa=%d, inline_resolve=%d, low_cost_pp=%d, shadow_scale=%.2f, shadow_rate=%d, no_a2c=%d, depth_bias_clamp=%d, render_scale=%.2f, tex_downscale=1/%d, robust_access=%d, ubwc=%d, cb_draws=%d, pool_safe_sync=%d (def_wait=%d safe1: hot=%d rb=%d d1k=%d), pool_overlap=%d, ubd_copyback=%d, dump_3d_frame=%d, strict_barriers=%d, emu_first_instance=%d",
              V.props.vendorID, vk_driver_kind(), vk_gpu_tier(), low_ram_early, mali_low_early, vk_gpu_ultra_low_end(), vk_mali_bifrost_legacy(), vk_samsung_safe(), V.use_classic_fb, V.has_bc ? "hardware-native (per-format verified)" : "cpu-bcdec+disk-cache",
              (getenv("DM_VK_NO_ZEROCOPY") || mali_zc_off) ? "disabled (copied pools)" : "enabled", pool_cached, rb_ranges, no_msaa, vk_inline_resolve(), vk_low_cost_pp(), vk_shadow_scale(), vk_shadow_rate(), no_a2c, V.has_depth_bias_clamp, rscale, tex_ds, en.robustBufferAccess ? 1 : 0, ubwc_val, cbd_val, safe_sync, vk_deferred_wait(), vk_safe1_hot_blocks(), vk_safe1_rb_staged(), vk_safe1_diff1k(), pool_ovl, ubd_cb, dump_3d, strict_bar, emu_fi);
    }
    if (mem_diag_on()) VKLOG("[memdiag] enabled storage-image features: extended-formats %s, read-without-format %s, write-without-format %s",
        en.shaderStorageImageExtendedFormats ? "yes" : "no", en.shaderStorageImageReadWithoutFormat ? "yes" : "no", en.shaderStorageImageWriteWithoutFormat ? "yes" : "no");
    slots_init();
    vk_bad_pipes_init();
    completion_start();
    memlog_start();
    { void vk_wd_start(void); vk_wd_start(); }
    { void crash_handlers_check(const char *when); crash_handlers_check("after the Vulkan device was made"); }   // (again at present 600, gfx_vk_present.c: the shader compiler loads with the first pipelines)
}
const void *vk_sg_pin(VkShaderStageFlagBits stage, const uint32_t *local) {
    if (!(V.sg_pin_stages & stage)) return NULL;
    // a compute workgroup must fit in maxComputeWorkgroupSubgroups subgroups of the pinned size (VUID-VkPipelineShaderStageCreateInfo-pNext-02756): a larger one keeps the driver's size
    if (local && (uint64_t)local[0] * local[1] * local[2] > (uint64_t)V.sg_pin.requiredSubgroupSize * V.sg_maxwgs) return NULL;
    return &V.sg_pin;
}

static int pool_cached_default(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_POOL_CACHED"); v = (e && *e) ? (atoi(e) != 0) : 1; }
    return v;
}
static int pool_readback_ranges_default(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_POOL_READBACK_RANGES"); v = (e && *e) ? (atoi(e) != 0) : 1; }
    return v;
}
int vk_pool_safe_sync(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_POOL_SAFE_SYNC");
        if (e && *e) {
            int x = atoi(e);
            v = x <= 0 ? 0 : (x >= 2 ? 2 : 1);
        } else {
            // Default Mode 2: Pure Serial Safe Sync (0% red/shadow flicker, 16 KB dirty blocks, zero hot/diff skip!)
            v = 2;
        }
    }
    return v;
}
static int vk_has_device_lost_marker(void);
int vk_samsung_safe(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_SAMSUNG_SAFE");
        if (e && *e && strcmp(e, "auto") != 0) v = atoi(e) != 0;
        else v = (V.props.vendorID == 0x144Du) ? 1 : 0;   // Auto-ON on Samsung Xclipse (AMD RDNA3/2 0x144D)
    }
    return v;
}
int vk_deferred_wait(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_DEFERRED_WAIT");
        if (e && *e) v = atoi(e) != 0;
        else if (vk_has_device_lost_marker()) v = 0;
        else if (V.props.vendorID == 0x144Du) v = 0;   // Samsung Xclipse (0x144d) EXCLUSIVE: pure serial avoids RDNA3 async fence/pool TDR
        else if (V.props.vendorID == 0x13B5u && V.props.driverVersion <= 0x08001000u) v = 0;   // Legacy Mali <= 32.1.0 (G76/G57/G52): pure serial avoids 21s submit hangs
        else v = 1;   // Modern Mali (>= 40.x/49.x) & Adreno: safe CPU+GPU pipelining via per-slot s->up + rb_staged
    }
    return v;
}
// ---- Last-Submitted Diagnostic Snapshot & 5-second Render-Thread Watchdog (v1.1.2-hybrid36) ----
extern uint64_t g_tex_frame;
static pthread_mutex_t g_diag_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_diag_note_draw(uint64_t vs, uint64_t fs, uint64_t gs) {
    V.cur_sub_diag.draws++;
    if (vs) V.cur_sub_diag.last_vs = vs;
    if (fs) V.cur_sub_diag.last_fs = fs;
    if (gs) V.cur_sub_diag.last_gs = gs;
}
void vk_diag_note_dispatch(uint64_t cs) {
    V.cur_sub_diag.dispatches++;
    if (cs) V.cur_sub_diag.last_cs = cs;
}
void vk_diag_note_new_pipe(uint64_t vs, uint64_t fs, uint64_t gs) {
    pthread_mutex_lock(&g_diag_mu);
    if (V.cur_sub_diag.n_new_pipes < 8u) {
        uint32_t i = V.cur_sub_diag.n_new_pipes++;
        V.cur_sub_diag.new_pipes[i].vs = vs;
        V.cur_sub_diag.new_pipes[i].fs = fs;
        V.cur_sub_diag.new_pipes[i].gs = gs;
    }
    pthread_mutex_unlock(&g_diag_mu);
}
static void vk_diag_format_pipes(const vk_sub_diag_t *d, char *buf, size_t nbuf) {
    if (!d || !d->n_new_pipes) { snprintf(buf, nbuf, "none"); return; }
    size_t o = 0;
    uint32_t cnt = d->n_new_pipes > 8u ? 8u : d->n_new_pipes;
    for (uint32_t i = 0; i < cnt && o + 56 < nbuf; i++) {
        o += (size_t)snprintf(buf + o, nbuf - o, "%s(%016llx/%016llx/%016llx)",
            i ? "," : "",
            (unsigned long long)d->new_pipes[i].vs,
            (unsigned long long)d->new_pipes[i].fs,
            (unsigned long long)d->new_pipes[i].gs);
    }
}
static int g_wd_phase = VK_WD_IDLE;
static uint64_t g_wd_since_ns = 0, g_wd_vs = 0, g_wd_fs = 0;
static inline uint64_t wd_now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
void vk_wd_enter(int phase, uint64_t vs, uint64_t fs) {
    __atomic_store_n(&g_wd_vs, vs, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wd_fs, fs, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wd_since_ns, wd_now_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_wd_phase, phase, __ATOMIC_RELEASE);
}
void vk_wd_leave(void) {
    __atomic_store_n(&g_wd_phase, VK_WD_IDLE, __ATOMIC_RELEASE);
}
static const char *wd_phase_name(int ph) {
    switch (ph) {
        case VK_WD_ACQUIRE_IMAGE:    return "ACQUIRE_IMAGE";
        case VK_WD_POOL_UPLOAD:      return "POOL_UPLOAD";
        case VK_WD_REPLAY_DRAW:      return "REPLAY_DRAW";
        case VK_WD_PIPELINE_COMPILE: return "PIPELINE_COMPILE";
        case VK_WD_VK_SUBMIT:        return "VK_SUBMIT";
        case VK_WD_FENCE_WAIT:       return "FENCE_WAIT";
        case VK_WD_PRESENT:          return "PRESENT";
        case VK_WD_POOL_READBACK:    return "POOL_READBACK";
        default:                     return "IDLE";
    }
}
// ---- Persistent Bad-Shader Guard & Worker Compile Watchdog for Mali <= 32.1.0 (v1.1.2-hybrid37) ----
enum { BAD_PIPE_MAX = 256 };
static struct { uint64_t vs, fs; int logged; } g_bad_pipes[BAD_PIPE_MAX];
static int g_nbad_pipes = 0;
static pthread_mutex_t g_bad_pipe_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_wd_w_active = 0;
static uint64_t g_wd_w_since_ns = 0, g_wd_w_vs = 0, g_wd_w_fs = 0;

static void bad_pipe_insert_mem_locked(uint64_t vs, uint64_t fs) {
    if (!vs && !fs) return;
    for (int i = 0; i < g_nbad_pipes; i++) {
        if (g_bad_pipes[i].vs == vs && g_bad_pipes[i].fs == fs) return;
    }
    if (g_nbad_pipes < BAD_PIPE_MAX) {
        g_bad_pipes[g_nbad_pipes].vs = vs;
        g_bad_pipes[g_nbad_pipes].fs = fs;
        g_bad_pipes[g_nbad_pipes].logged = 0;
        g_nbad_pipes++;
    }
}
static void bad_pipe_append_disk(uint32_t drv, uint64_t vs, uint64_t fs) {
    const char *root = getenv("DM_ROOT");
    if (!root || !*root) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/bad_pipelines.txt", root);
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%#x %016llx %016llx\n", drv, (unsigned long long)vs, (unsigned long long)fs);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
}
void vk_bad_pipe_add(uint64_t vs, uint64_t fs, const char *reason) {
    if (!vk_mali_legacy_r32()) return;
    int is_new = 0;
    pthread_mutex_lock(&g_bad_pipe_mu);
    int prev = g_nbad_pipes;
    bad_pipe_insert_mem_locked(vs, fs);
    is_new = (g_nbad_pipes > prev);
    pthread_mutex_unlock(&g_bad_pipe_mu);
    if (is_new) {
        bad_pipe_append_disk(V.props.driverVersion, vs, fs);
        VKLOG("[vk] blacklisted bad shader pair (%s): drv=%#x vs=%016llx fs=%016llx -> bad_pipelines.txt",
              reason ? reason : "hang", V.props.driverVersion, (unsigned long long)vs, (unsigned long long)fs);
    }
}
int vk_bad_pipe_check(uint64_t vs, uint64_t fs) {
    if (!vk_mali_legacy_r32()) return 0;
    int hit = 0, do_log = 0;
    pthread_mutex_lock(&g_bad_pipe_mu);
    for (int i = 0; i < g_nbad_pipes; i++) {
        if (g_bad_pipes[i].vs == vs && g_bad_pipes[i].fs == fs) {
            hit = 1;
            if (!g_bad_pipes[i].logged) { g_bad_pipes[i].logged = 1; do_log = 1; }
            break;
        }
    }
    pthread_mutex_unlock(&g_bad_pipe_mu);
    if (do_log) {
        VKLOG("[vk] skipped blacklisted shader vs=%016llx fs=%016llx (drv=%#x)",
              (unsigned long long)vs, (unsigned long long)fs, V.props.driverVersion);
    }
    return hit;
}
void vk_bad_pipes_init(void) {
    if (!vk_mali_legacy_r32()) return;
    const char *root = getenv("DM_ROOT");
    if (root && *root) {
        char tmp_path[1024], bad_path[1024];
        snprintf(tmp_path, sizeof tmp_path, "%s/compiling_pipeline.tmp", root);
        snprintf(bad_path, sizeof bad_path, "%s/bad_pipelines.txt", root);
        // 1. Recover any previous run killed/hung mid-compile: move compiling_pipeline.tmp -> bad_pipelines.txt
        FILE *tf = fopen(tmp_path, "r");
        if (tf) {
            unsigned int drv = 0;
            unsigned long long vs = 0, fs = 0;
            if (fscanf(tf, "%x %llx %llx", &drv, &vs, &fs) == 3 && (vs || fs)) {
                VKLOG("[vk] detected interrupted compile in compiling_pipeline.tmp (drv=%#x vs=%016llx fs=%016llx) -> promoting to bad_pipelines.txt",
                      drv, vs, fs);
                bad_pipe_append_disk((uint32_t)drv, (uint64_t)vs, (uint64_t)fs);
            }
            fclose(tf);
            unlink(tmp_path);
        }
        // 2. Load persistent bad_pipelines.txt entries matching this Mali driver
        FILE *bf = fopen(bad_path, "r");
        if (bf) {
            char line[256];
            pthread_mutex_lock(&g_bad_pipe_mu);
            while (fgets(line, sizeof line, bf)) {
                unsigned int drv = 0;
                unsigned long long vs = 0, fs = 0;
                if (sscanf(line, "%x %llx %llx", &drv, &vs, &fs) == 3) {
                    if (drv == 0 || drv == V.props.driverVersion) {
                        bad_pipe_insert_mem_locked((uint64_t)vs, (uint64_t)fs);
                    }
                }
            }
            pthread_mutex_unlock(&g_bad_pipe_mu);
            fclose(bf);
        }
    }
    // 3. Pre-seed known Mali r32p1 (0x08001000) hang pair (passthrough 0-output VS + 3-MRT OpKill FS)
    //    unless explicitly disabled for A/B testing via DM_VK_ALLOW_BAD_PIPES=1.
    const char *allow = getenv("DM_VK_ALLOW_BAD_PIPES");
    if (!(allow && atoi(allow) != 0)) {
        pthread_mutex_lock(&g_bad_pipe_mu);
        bad_pipe_insert_mem_locked(0x2a27276db82273d7ull, 0x48736f6b5e6c55e3ull);
        pthread_mutex_unlock(&g_bad_pipe_mu);
    }
    VKLOG("[vk] Mali <= 32.1.0 bad-shader guard active (drv=%#x): %d blacklisted pair(s), 100%% non-blocking async compile ON",
          V.props.driverVersion, g_nbad_pipes);
}
void vk_pipe_worker_begin(uint64_t vs, uint64_t fs) {
    if (!vk_mali_legacy_r32()) return;
    const char *root = getenv("DM_ROOT");
    if (root && *root) {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof tmp_path, "%s/compiling_pipeline.tmp", root);
        FILE *f = fopen(tmp_path, "w");
        if (f) {
            fprintf(f, "%#x %016llx %016llx\n", V.props.driverVersion, (unsigned long long)vs, (unsigned long long)fs);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
        }
    }
    __atomic_store_n(&g_wd_w_vs, vs, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wd_w_fs, fs, __ATOMIC_RELAXED);
    __atomic_store_n(&g_wd_w_since_ns, wd_now_ns(), __ATOMIC_RELEASE);
    __atomic_store_n(&g_wd_w_active, 1, __ATOMIC_RELEASE);
}
void vk_pipe_worker_end(void) {
    if (!vk_mali_legacy_r32()) return;
    __atomic_store_n(&g_wd_w_active, 0, __ATOMIC_RELEASE);
    const char *root = getenv("DM_ROOT");
    if (root && *root) {
        char tmp_path[1024];
        snprintf(tmp_path, sizeof tmp_path, "%s/compiling_pipeline.tmp", root);
        unlink(tmp_path);
    }
}
static void *vk_wd_main(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk watchdog");
    uint64_t last_log_ns = 0;
    uint64_t last_w_vs = 0, last_w_fs = 0;
    for (;;) {
        sleep(1);
        uint64_t now = wd_now_ns();
        // Check background pipeline compiler worker hang (> 8s on Mali <= 32.1.0 -> auto-blacklist to bad_pipelines.txt)
        if (__atomic_load_n(&g_wd_w_active, __ATOMIC_ACQUIRE) && vk_mali_legacy_r32()) {
            uint64_t wsince = __atomic_load_n(&g_wd_w_since_ns, __ATOMIC_ACQUIRE);
            if (wsince && now > wsince && (now - wsince) >= 8000000000ull) {
                uint64_t wvs = __atomic_load_n(&g_wd_w_vs, __ATOMIC_RELAXED);
                uint64_t wfs = __atomic_load_n(&g_wd_w_fs, __ATOMIC_RELAXED);
                if (wvs != last_w_vs || wfs != last_w_fs) {
                    last_w_vs = wvs; last_w_fs = wfs;
                    vk_bad_pipe_add(wvs, wfs, "watchdog_worker_hang_8s");
                    VKLOG("[WATCHDOG] WORKER PIPELINE_COMPILE HANG >8s (elapsed=%.1fs) vs=%016llx fs=%016llx drv=%#x -> recorded in bad_pipelines.txt (render thread stays unblocked)",
                          (double)(now - wsince) / 1e9, (unsigned long long)wvs, (unsigned long long)wfs, V.props.driverVersion);
                    fflush(stderr);
                }
            }
        } else {
            last_w_vs = 0; last_w_fs = 0;
        }
        int ph = __atomic_load_n(&g_wd_phase, __ATOMIC_ACQUIRE);
        if (ph == VK_WD_IDLE) { last_log_ns = 0; continue; }
        uint64_t since = __atomic_load_n(&g_wd_since_ns, __ATOMIC_ACQUIRE);
        if (since && now > since && (now - since) >= 5000000000ull) {
            if (!last_log_ns || (now - last_log_ns) >= 5000000000ull) {
                last_log_ns = now;
                uint64_t vs = __atomic_load_n(&g_wd_vs, __ATOMIC_RELAXED);
                uint64_t fs = __atomic_load_n(&g_wd_fs, __ATOMIC_RELAXED);
                uint64_t fr = __atomic_load_n(&g_tex_frame, __ATOMIC_RELAXED);
                vk_sub_diag_t d;
                pthread_mutex_lock(&g_diag_mu);
                d = V.last_sub_diag;
                pthread_mutex_unlock(&g_diag_mu);
                char pbuf[512];
                vk_diag_format_pipes(&d, pbuf, sizeof pbuf);
                uint64_t dvs = vs ? vs : d.last_vs;
                uint64_t dfs = fs ? fs : d.last_fs;
                VKLOG("[WATCHDOG] RENDER HANG >5s in phase=%s elapsed=%.1fs frame=%llu (sub_frame=%llu fence=%llu draws=%u passes=%u dispatches=%u) vs=%016llx fs=%016llx gs=%016llx cs=%016llx new_pipes[%u]=%s dev=%s drv=%#x",
                      wd_phase_name(ph), (double)(now - since) / 1e9, (unsigned long long)fr,
                      (unsigned long long)d.frame, (unsigned long long)d.fence, d.draws, d.passes, d.dispatches,
                      (unsigned long long)dvs, (unsigned long long)dfs, (unsigned long long)d.last_gs, (unsigned long long)d.last_cs,
                      d.n_new_pipes, pbuf, V.props.deviceName, V.props.driverVersion);
                fflush(stderr);
            }
        }
    }
    return NULL;
}
void vk_wd_start(void) {
    static int started;
    if (started++) return;
    pthread_t th; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&th, &a, vk_wd_main, NULL);
    pthread_attr_destroy(&a);
}
int vk_safe1_hot_blocks(void) {
    static int v = -1;
    // Default 0: hot_blocks PROT_RW bypassed dirty tracking and caused shadow/red flicker.
    if (v < 0) { const char *e = getenv("DM_VK_SAFE1_HOT_BLOCKS"); v = (e && *e) ? (atoi(e) != 0) : 0; }
    return v;
}
int vk_safe1_rb_staged(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_SAFE1_RB_STAGED"); v = (e && *e) ? (atoi(e) != 0) : 1; }
    return v;
}
int vk_safe1_diff1k(void) {
    static int v = -1;
    // Default 0: 1KB baseline memcmp skipped uploads when GPU overwrote p->buf and CPU wrote back prior value.
    if (v < 0) { const char *e = getenv("DM_VK_SAFE1_DIFF1K"); v = (e && *e) ? (atoi(e) != 0) : 0; }
    return v;
}
int vk_inline_resolve(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_INLINE_RESOLVE");
        // Mali-TBDR inline MSAA resolve is not beneficial on Samsung Xclipse AMD RDNA3 (0x144D); default off on Xclipse.
        v = (e && *e) ? (atoi(e) != 0) : (V.props.vendorID == 0x144Du ? 0 : 1);
    }
    return v;
}
static pthread_mutex_t g_pipe_compile_mu = PTHREAD_MUTEX_INITIALIZER;
int vk_mali_legacy_compiler(void) {
    // ARM Mali (0x13B5) Vulkan 1.1 or driver < 40.0 (e.g. Mali-G57 r32p1 = 0x08001000):
    // concurrent vkCreate*Pipelines across vk present + vk compute warm crashes in libGLES_mali.so (SIGSEGV).
    return V.props.vendorID == 0x13B5u && (V.props.apiVersion < VK_API_VERSION_1_2 || V.props.driverVersion < 0x0A000000u);
}
int vk_mali_legacy_r32(void) {
    // ARM Mali driver <= 32.1.0 (0x08001000: Mali-G76 SM-G975F, Mali-G57 r32p1, Mali-G52 r26p0).
    // Modern Mali (e.g. Mali-G720 49.1.0 = 0x0C401000), Adreno (0x5143), and Xclipse (0x144d) return 0!
    return V.props.vendorID == 0x13B5u && V.props.driverVersion > 0u && V.props.driverVersion <= 0x08001000u;
}
int vk_mali_bifrost_legacy(void) {
    // ARM Mali Bifrost driver < 32.0 (0x08000000; e.g. Mali-G52 MC2 driver 26.0.0 = 0x06800000 on vivo V2109):
    // requires order_keep=0 so NVN order-only barriers emit explicit pass cuts / memory barriers on Bifrost L2.
    return V.props.vendorID == 0x13B5u && V.props.driverVersion > 0u && V.props.driverVersion < 0x08000000u;
}
int vk_mali_old_driver(void) {
    // ARM Mali (0x13B5) driver < 49.0 (0x0C400000; e.g. Mali-G615 44.1.0 = 0x0B001000, Mali-G57 32.1.0 = 0x08001000, Mali-G52 26.0.0 = 0x06800000).
    // Xiaomi Mali-G720 MC7 driver 49.1.0 (0x0C401000) is >= 0x0C400000 -> modern driver.
    return V.props.vendorID == 0x13B5u && (V.props.apiVersion < VK_API_VERSION_1_2 || V.props.driverVersion < 0x0C400000u);
}
static int vk_adreno_640_650(void) {
    if (V.props.vendorID != 0x5143u) return 0;
    const char *nm = V.props.deviceName;
    return V.props.deviceID == 0x6050002u || V.props.deviceID == 0x6040001u ||
           strstr(nm, "Adreno (TM) 650") != NULL || strstr(nm, "Adreno (TM) 640") != NULL;
}
static int vk_adreno_618_619(void) {
    if (V.props.vendorID != 0x5143u) return 0;
    const char *nm = V.props.deviceName;
    return V.props.deviceID == 0x6010800u || V.props.deviceID == 0x6010900u ||
           strstr(nm, "Adreno (TM) 618") != NULL || strstr(nm, "Adreno (TM) 619") != NULL;
}
int vk_gpu_tier(void) {
    // 4-tier automatic mobile GPU & RAM classifier:
    //   0 = Flagship / Upper-Mid (Mali-G720/G715/G710/G78, Adreno 730+, Xclipse, >=6 GB): 1.00x, MSAA 2x, Tex 1/1
    //   1 = Mid-Range / Upper-Mid Adreno 6xx (Adreno 650/640 0.85x + shadow 0.75x; Mali-G76/G68/G610 0.75x + shadow 0.50x): MSAA OFF, Low-Cost PP OFF, Tex 1/1 -> 60 FPS
    //   2 = Low-End / Mid Adreno 6xx (Adreno 618/619 0.65x; Mali-G57 MC2 0.55x / MC3+ 0.67x; or 4 GB RAM): MSAA OFF, Shadow 0.50x 30Hz, Low-Cost PP ON, Tex 1/2
    //   3 = Ultra Low-End (Adreno 610 0x6010001, Mali-G52/G51/G31, Mali-G57 MC1, PowerVR, or <=3.5 GB RAM): 0.50x, MSAA OFF, Shadow 0.50x 30Hz, Low-Cost PP ON, Tex 1/2
    if (!V.props.deviceName[0]) return 0;
    static int tier = -1;
    if (tier >= 0) return tier;
    // Samsung Xclipse (0x144d, Galaxy S22/S23/S24/S25 RDNA2/RDNA3): always flagship Tier 0
    // (1.00x render scale, 1.00x shadow 60Hz, low_cost_pp=0, tex 1/1). Mali & Adreno untouched!
    if (V.props.vendorID == 0x144Du) { tier = 0; return tier; }
    const char *nm = V.props.deviceName;
    int ram_t = vk_low_ram_tier();
    int is_ultra = (ram_t >= 2) ||
                   strstr(nm, "Mali-G52") != NULL || strstr(nm, "Mali-G51") != NULL ||
                   strstr(nm, "Mali-G31") != NULL || strstr(nm, "Mali-T") != NULL ||
                   (strstr(nm, "Mali-G57") != NULL && (strstr(nm, "MC1") != NULL || (V.sg_size > 0 && V.sg_size <= 4))) ||
                   strstr(nm, "PowerVR") != NULL || V.props.vendorID == 0x1010u ||
                   V.props.deviceID == 0x6010001u ||
                   strstr(nm, "Adreno (TM) 610") != NULL || strstr(nm, "Adreno (TM) 612") != NULL ||
                   strstr(nm, "Adreno (TM) 5") != NULL || strstr(nm, "Adreno (TM) 4") != NULL ||
                   strstr(nm, "Adreno (TM) 3") != NULL;
    if (is_ultra) { tier = 3; return tier; }
    int is_mid = strstr(nm, "Mali-G76") != NULL || V.props.deviceID == 0x72110000u ||
                 strstr(nm, "Mali-G77") != NULL ||
                 strstr(nm, "Mali-G68") != NULL || strstr(nm, "Mali-G610") != NULL ||
                 vk_adreno_640_650() ||
                 strstr(nm, "Adreno (TM) 642") != NULL ||
                 strstr(nm, "Adreno (TM) 660") != NULL || strstr(nm, "Adreno (TM) 710") != NULL ||
                 strstr(nm, "Adreno (TM) 720") != NULL;
    // Keep Mid-High GPUs (e.g. Mali-G76 0x72110000, Adreno 650/640) in Tier 1 even if low_ram==1!
    if (is_mid) { tier = 1; return tier; }
    int is_low = (ram_t == 1) ||
                 strstr(nm, "Mali-G57") != NULL ||
                 vk_adreno_618_619() ||
                 (V.props.vendorID == 0x13B5u && ((V.sg_size > 0 && V.sg_size <= 8) || V.props.driverVersion < 0x0A000000u));
    if (is_low) { tier = 2; return tier; }
    tier = 0;
    return tier;
}
static int vk_mali_g57_mc2(void) {
    const char *nm = V.props.deviceName;
    if (!nm[0]) return 0;
    if (strstr(nm, "Mali-G57 MC2") != NULL) return 1;
    // Mali-G57 Vulkan 1.1 drivers (r32p1 / Helio G99 / Dimensity 6080 / 6100+ / Unisoc T760) are 2-core MC2 class
    return strstr(nm, "Mali-G57") != NULL && VK_API_VERSION_MINOR(V.props.apiVersion) < 3;
}
int vk_mali_low_end(void) {
    // Returns 1 for Mid-Range / Low-End / Ultra Low-End GPUs (Tier >= 1) where auto MSAA is disabled.
    return vk_gpu_tier() >= 1;
}
int vk_gpu_ultra_low_end(void) {
    // Returns 1 for Ultra Low-End GPUs (Tier 3: Mali-G52/G51/G31, PowerVR, Adreno 610/612/5xx, <=3.5 GB RAM).
    return vk_gpu_tier() >= 3;
}
int vk_low_cost_pp(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_LOW_COST_PP");
        if (e && *e && strcmp(e, "auto") != 0) v = atoi(e) != 0;
        else v = (vk_gpu_tier() >= 2) ? 1 : 0;   // Active on Low-End & Ultra Low-End (Tier 2-3: Mali-G57 MC2 / Mali-G52); off on Tier 0-1 (Flagship / Mali-G76)
    }
    return v;
}
float vk_shadow_scale(void) {
    static float sc = -1.0f;
    if (sc >= 0.0f) return sc;
    const char *e = getenv("DM_VK_SHADOW_SCALE");
    if (e && *e && strcmp(e, "auto") != 0) {
        float v = strtof(e, NULL);
        sc = (v >= 0.25f && v <= 1.0f) ? v : 1.0f;
    } else {
        int gt = vk_gpu_tier();
        // 4-tier shadow target resolution policy:
        //   Tier 0 (Flagship): 1.00x | Adreno 640/650 Tier 1: 0.75x | Other Tier 1-3: 0.50x (75% less shadow fillrate)
        sc = (gt == 0) ? 1.0f : (gt == 1 && vk_adreno_640_650()) ? 0.75f : 0.50f;
    }
    if (sc > 0.995f) sc = 1.0f;
    return sc;
}
int vk_shadow_rate(void) {
    static int r = -1;
    if (r >= 0) return r;
    const char *e = getenv("DM_VK_SHADOW_RATE");
    if (e && *e && strcmp(e, "auto") != 0) {
        int v = atoi(e);
        r = v <= 0 ? 0 : (v >= 2 ? 2 : 1);
    } else {
        // Auto: 30Hz half-rate shadows on Tier >= 2 (Mali-G57/G52/Adreno 618), full 60Hz on Tier 0-1
        r = (vk_gpu_tier() >= 2) ? 2 : 1;
    }
    return r;
}
int vk_low_ram_tier(void) {
    static int tier = -1;
    if (tier >= 0) return tier;
    const char *e = getenv("DM_LOW_RAM");
    if (e && *e && strcmp(e, "auto") != 0) {
        int v = atoi(e);
        tier = v < 0 ? 0 : (v > 2 ? 2 : v);
        VKLOG("memory tier: %d (DM_LOW_RAM=%s)", tier, e);
        return tier;
    }
    long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGE_SIZE);
    uint64_t total_mb = (pages > 0 && psize > 0) ? ((uint64_t)pages * (uint64_t)psize) >> 20 : 8192;
    // Samsung Xclipse (0x144d) EXCLUSIVE: Exynos DMA carveouts can reduce _SC_PHYS_PAGES to <= 6144 MiB on 8 GB phones.
    // Keep Xclipse at standard RAM tier 0 whenever >= 5500 MiB is visible. Mali & Adreno thresholds untouched!
    if (V.props.vendorID == 0x144Du && total_mb >= 5500ull) {
        tier = 0;
        VKLOG("memory tier: 0 (Xclipse 0x144d flagship override: %llu MiB visible RAM)", (unsigned long long)total_mb);
        return tier;
    }
    // Trim RAM (< 1.8 GB target on 4-6 GB phones): <=3.75 GB -> tier 2, <=6.0 GB -> tier 1, >6 GB -> tier 0
    tier = total_mb <= 3840 ? 2 : (total_mb <= 6144 ? 1 : 0);
    VKLOG("memory tier: %d (auto-detected %llu MiB RAM: %s)", tier, (unsigned long long)total_mb,
          tier == 2 ? "ultra low-RAM <=3.8 GB" : tier == 1 ? "low-RAM 4-6 GB Anti-LMK" : "standard >6 GB");
    return tier;
}
float vk_render_scale(void) {
    static float sc = -1.0f;
    if (sc >= 0.0f) return sc;
    const char *e = getenv("DM_VK_RENDER_SCALE");
    if (e && *e && strcmp(e, "auto") != 0) {
        float v = strtof(e, NULL);
        sc = (v >= 0.45f && v <= 1.0f) ? v : 1.0f;
    } else {
        int gt = vk_gpu_tier();
        if (gt >= 3) sc = 0.50f;                     // Ultra Low-End (Adreno 610, Mali-G52, <=3.5 GB) -> 640x360 3D, UI 720p crisp
        else if (gt == 2 && vk_mali_g57_mc2()) sc = 0.55f; // Mali-G57 MC2 (2-core, e.g. RMX3636 / Helio G99) -> 704x396 3D for 45-60 FPS push
        else if (gt == 2 && vk_adreno_618_619()) sc = 0.65f; // Adreno 618/619 (M2101K6G / SM-M236B) -> 832x468 3D + 30Hz shadow
        else if (gt == 2) sc = 0.67f;                // Low-End (Mali-G57 MC3+, 4-6 GB) -> 858x482 3D
        else if (gt == 1 && vk_adreno_640_650()) sc = 0.85f; // Adreno 640/650 (SD 855/865/870) -> 1088x612 3D -> crisp 60 FPS
        else if (gt == 1) sc = 0.75f;                // Mid-Range (Mali-G76/G68/G610, Adreno 642/710) -> 960x540 3D -> locked 60 FPS
        else sc = 1.0f;                              // Flagship (Mali-G720/G715, Adreno 730+, Xclipse 940) -> 1280x720 native
    }
    if (sc > 0.995f) sc = 1.0f;
    return sc;
}
int vk_tex_downscale(void) {
    static int ds = -1;
    if (ds >= 0) return ds;
    const char *e = getenv("DM_VK_TEX_DOWNSCALE");
    if (e && *e && strcmp(e, "auto") != 0) {
        int v = atoi(e);
        ds = (v >= 4) ? 4 : (v >= 2 ? 2 : 1);
    } else {
        int gt = vk_gpu_tier();
        // Auto mode caps BC texture downscale at 1/2x (never 1/4x in auto mode) so 3D world textures stay sharp & never flicker
        if (gt >= 2) ds = 2;   // Low-End (Mali-G57, Adreno 618/619) & Ultra Low-End (Mali-G52) -> 1/2x BC textures (skip mip 0)
        else ds = 1;           // Flagship & Mid-Range (Mali-G76/G68/G610, Adreno 642/650/710) -> full 1/1 textures
    }
    return ds;
}
static int vk_has_device_lost_marker(void) {
    static int seen = -1;
    if (seen >= 0) return seen;
    const char *root = getenv("DM_ROOT");
    if (!root || !*root) { seen = 0; return 0; }
    char path[1024];
    snprintf(path, sizeof path, "%s/.gpu_device_lost", root);
    struct stat st;
    seen = (stat(path, &st) == 0) ? 1 : 0;
    return seen;
}
void vk_pipe_compile_lock(void) {
    pthread_mutex_lock(&g_pipe_compile_mu);
}
int vk_pipe_compile_trylock(void) {
    return pthread_mutex_trylock(&g_pipe_compile_mu) == 0;
}
void vk_pipe_compile_unlock(void) {
    pthread_mutex_unlock(&g_pipe_compile_mu);
}
int vk_pool_overlap(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_POOL_OVERLAP");
        int req = (e && *e) ? atoi(e) : 1;
        // Allow safe pool_overlap=1 (with deferred buffer deletion) on Mali-G52/G57/old drivers;
        // auto-fallback to 0 ONLY if a prior VK_ERROR_DEVICE_LOST (-4) marker exists.
        if (vk_mali_old_driver() && vk_has_device_lost_marker() && req != 2) {
            if (req) VKLOG("Mali driver < 49.0 (%#x) with prior .gpu_device_lost: auto-fallback pool_overlap -> 0 (set DM_VK_POOL_OVERLAP=2 to force)", V.props.driverVersion);
            v = 0;
        } else {
            v = req != 0;
        }
    }
    return v;
}
int vk_ubd_copyback(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_UBD_COPYBACK");
        int req = (e && *e) ? atoi(e) : 1;
        // When copied-pool safe_sync >= 1 is active, force synchronous ubd_copyback=1 (never async mode 2)
        // so uniform buffer updates mirror atomically under g_baseline_mu without racing pool_merge_back().
        v = (req == 2 && vk_pool_safe_sync() >= 1) ? 1 : req;
    }
    return v;
}
int vk_dump_3d_frame(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_DUMP_3D_FRAME"); v = (e && *e) ? (atoi(e) != 0) : 0; }
    return v;
}
int vk_strict_barriers(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_STRICT_BARRIERS");
        // Default 1 EXCLUSIVELY on Samsung Xclipse (0x144d); Mali & Adreno stay 0!
        v = (e && *e) ? (atoi(e) != 0) : (V.props.vendorID == 0x144Du ? 1 : 0);
    }
    return v;
}
int vk_emu_first_instance(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_VK_EMU_FIRST_INSTANCE");
        // Always enable on GPUs lacking hardware vertexAttributeInstanceRateDivisor (e.g. Mali-G52/G57/G615)
        // unless explicitly forced off via DM_VK_EMU_FIRST_INSTANCE=0 on devices that have divisor.
        v = (!V.has_divisor) ? 1 : ((e && *e) ? (atoi(e) != 0) : 1);
    }
    return v;
}
int vk_no_a2c(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_NO_A2C"); v = (e && *e) ? (atoi(e) != 0) : vk_quirk(VQ_NO_A2C); }
    return v;
}
static uint64_t g_st_ns[7];   // 0=pool_upload, 1=pool_readback, 2=dirty_rearm, 3=replay, 4=vk_submit, 5=fence_wait, 6=present
static uint64_t st_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
void vk_sub_timing_add(int idx, uint64_t ns) { if (idx >= 0 && idx < 7 && ns) __atomic_fetch_add(&g_st_ns[idx], ns, __ATOMIC_RELAXED); }
void vk_sub_timing_snapshot(uint64_t out[7]) {
    extern uint64_t vk_replay_ns(void);
    for (int i = 0; i < 7; i++) out[i] = __atomic_load_n(&g_st_ns[i], __ATOMIC_RELAXED);
    out[3] = vk_replay_ns();
}
static uint64_t g_rb_seen, g_rb_narrowed;
static uint64_t g_ovl_used, g_ovl_skip_off, g_ovl_skip_rb, g_ovl_skip_init, g_ovl_skip_conflict, g_rb_staged_cnt;
static uint64_t g_defwait_saved_ns, g_hot16_last;
static pthread_mutex_t g_baseline_mu = PTHREAD_MUTEX_INITIALIZER;   // guards p->baseline between vk_pools_sync/vk_ubd_land and completion_main's pool_merge_back

// ---------------- memory pools ----------------
// DARK SOULS: REMASTERED made more than 4,096 pools within 3 s of boot (FromSoftware's engine gives many resources a small pool of its own: 0x1000, 0x12000 bytes), which stopped it
// with "too many memory pools"; the index (g_pool_ord) is 16-bit, so up to 65,535
#define MAXPOOL 32768
static vkpool_t g_pool[MAXPOOL]; static int g_npool;
static pthread_rwlock_t g_pool_mu = PTHREAD_RWLOCK_INITIALIZER;
static uint32_t g_pool_gen;   // bumped under the write lock when a pool is added or removed: vk_pool_of's per-thread cache holds while it is unchanged
// g_pool's indices by lo, remade under the write lock whenever a pool is added or removed: vk_pool_of searches it. A linear scan of g_pool behind the per-thread
// cache was 18% of the profiler's samples of Super Mario Bros. Wonder's replay in its first level (a draw looks up more pools than the cache holds); with the
// search its world map's replay took 4.2 ms a frame instead of 4.6-4.9
static uint16_t g_pool_ord[MAXPOOL];
// The index kept in order one pool at a time (sorting it whole at every add is O(n log n) a pool, which thousands of pools made slow): pool k (just appended) goes in at its place
static void pool_ord_insert(int k) {
    int lo = 0, hi = k; uint64_t a = g_pool[k].lo;
    while (lo < hi) { int m = (lo + hi) / 2; if (g_pool[g_pool_ord[m]].lo <= a) lo = m + 1; else hi = m; }
    memmove(g_pool_ord + lo + 1, g_pool_ord + lo, (size_t)(k - lo) * sizeof *g_pool_ord); g_pool_ord[lo] = (uint16_t)k;
}
// pool i removed and the last pool (index g_npool, already decremented) moved into its place: i leaves the index and the moved pool's entry takes index i
static void pool_ord_remove(int i) {
    int n = g_npool + 1, w = 0;
    for (int r = 0; r < n; r++) { uint16_t v = g_pool_ord[r]; if (v == i) continue; g_pool_ord[w++] = v == g_npool ? (uint16_t)i : v; }
}
static int recv_fds(int sock, int *fds, int max) {
    char data[4096], ctl[CMSG_SPACE(sizeof(int) * 16)]; struct iovec iov = {data, sizeof data};
    struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
    if (recvmsg(sock, &m, 0) < 0) return -1;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            for (size_t i = 0; i < (c->cmsg_len - CMSG_LEN(0)) / sizeof(int) && n < max; i++) fds[n++] = ((int *)CMSG_DATA(c))[i];
    return n;
}
static const VkBufferUsageFlags POOL_USAGE0 = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
#define POOL_USAGE (POOL_USAGE0 | (V.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0))   // any pool range may be a transform feedback buffer
// Copied-pool buffer reuse cache (Mali / non-zero-copy pools): keeps freed VkBuffer + mapped VkDeviceMemory + baseline
// so 16 MB Unity temporary asset-loading pools do not stall 23-144 ms in vkAllocateMemory/vkMapMemory/wait_fence.
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *shadow, *baseline; uint64_t size, fence; int noncoherent, cached; } cpfree_t;
enum { CPF_N = 64 };
#define CPF_BYTES (128ull << 20)
static cpfree_t g_cpf[CPF_N]; static int g_ncpf; static uint64_t g_cpf_bytes, g_cpf_hit, g_cpf_kept;
static pthread_mutex_t g_cpf_mu = PTHREAD_MUTEX_INITIALIZER;
static int zcf_on(void);
uint64_t mtl_last_fence(void);
// Non-blocking fence-tagged deferred retire queue for pool buffers (pool_drop & cpf):
// Every retired VkBuffer + VkDeviceMemory is tagged with retire_fence = V.sub_seq + 3 and ONLY
// destroyed non-blockingly once V.completed_seq >= retire_fence. ZERO vkQueueWaitIdle/vkDeviceWaitIdle!
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *ahb; uint64_t retire_fence; } pool_retire_t;
static pool_retire_t *g_pool_retire;
static int g_npool_retire, g_cappool_retire;
static pthread_mutex_t g_pool_retire_mu = PTHREAD_MUTEX_INITIALIZER;
static void pool_retire_push(VkBuffer buf, VkDeviceMemory mem, void *ahb, uint64_t retire_fence) {
    if (!buf && !mem && !ahb) return;
    { const char *e = getenv("DM_VK_X_LEAK"); if (e && (atoi(e) & 1)) return; }
    pthread_mutex_lock(&g_pool_retire_mu);
    if (g_npool_retire == g_cappool_retire) {
        g_cappool_retire = g_cappool_retire ? g_cappool_retire * 2 : 64;
        g_pool_retire = realloc(g_pool_retire, (size_t)g_cappool_retire * sizeof *g_pool_retire);
    }
    g_pool_retire[g_npool_retire++] = (pool_retire_t){buf, mem, ahb, retire_fence};
    pthread_mutex_unlock(&g_pool_retire_mu);
}
void vk_pool_retire_drain(void) {
    uint64_t done = __atomic_load_n(&V.completed_seq, __ATOMIC_ACQUIRE);
    uint64_t fd = vk_fence_done();
    if (fd > done) done = fd;
    pool_retire_t ready[64];
    for (;;) {
        int n = 0;
        pthread_mutex_lock(&g_pool_retire_mu);
        int o = 0;
        for (int i = 0; i < g_npool_retire; i++) {
            if (g_pool_retire[i].retire_fence <= done && n < 64) ready[n++] = g_pool_retire[i];
            else g_pool_retire[o++] = g_pool_retire[i];
        }
        g_npool_retire = o;
        pthread_mutex_unlock(&g_pool_retire_mu);
        if (!n) break;
        for (int i = 0; i < n; i++) {
            if (ready[i].buf) vkDestroyBuffer(V.dev, ready[i].buf, NULL);
            if (ready[i].mem) vkFreeMemory(V.dev, ready[i].mem, NULL);
            if (ready[i].ahb) AHardwareBuffer_release((AHardwareBuffer *)ready[i].ahb);
        }
    }
}
static void cpf_free_now(const cpfree_t *e) {
    vkDestroyBuffer(V.dev, e->buf, NULL);
    vkFreeMemory(V.dev, e->mem, NULL);
    free(e->baseline);
}
static int cpf_take(uint64_t size, int cached, vkpool_t *p) {
    vk_pool_retire_drain();
    if (!zcf_on()) return 0;
    uint64_t done = __atomic_load_n(&V.completed_seq, __ATOMIC_ACQUIRE);
    uint64_t fd = vk_fence_done();
    if (fd > done) done = fd;
    int hit = 0; cpfree_t e = {0};
    pthread_mutex_lock(&g_cpf_mu);
    for (int i = 0; i < g_ncpf; i++) {
        if (g_cpf[i].size == size && g_cpf[i].cached == cached && g_cpf[i].fence <= done) {
            e = g_cpf[i];
            memmove(g_cpf + i, g_cpf + i + 1, (size_t)(g_ncpf - i - 1) * sizeof *g_cpf);
            g_ncpf--; g_cpf_bytes -= size; g_cpf_hit++; hit = 1; break;
        }
    }
    pthread_mutex_unlock(&g_cpf_mu);
    if (!hit) return 0;
    p->buf = e.buf; p->mem = e.mem; p->shadow = e.shadow; p->baseline = NULL;
    p->noncoherent = e.noncoherent; p->cached = e.cached; p->synced = 0; p->baseline_complete = 0; p->baseline_fresh = 0; p->cpf_reused = 1;
    // Invalidate any residual non-coherent CPU cache lines left over from the prior pool owner
    // before brand_new_cached zero-fills or writes to p->shadow (prevents stale cache-line bleed).
    if (p->noncoherent && p->mem && vkInvalidateMappedMemoryRanges) {
        VkMappedMemoryRange rr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, p->mem, 0, VK_WHOLE_SIZE};
        VKCHK(vkInvalidateMappedMemoryRanges(V.dev, 1, &rr));
    }
    return 1;
}
static uint64_t cpf_budget_bytes(void) {
    int t = vk_low_ram_tier();
    // Tight RAM trim (< 1.8 GB on 4-6 GB devices):
    //   tier >= 2 (<=3.8 GB RAM): 0 MiB kept buffers
    //   tier == 1 (4-6 GB RAM):  24 MiB kept buffers
    //   tier == 0 (>6 GB RAM):   64 MiB kept buffers
    return t >= 2 ? 0ull : (t == 1 ? (24ull << 20) : (64ull << 20));
}
static void madvise_release_buf(void *ptr, size_t len) {
    if (!ptr || len < (256u << 10)) return;
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t a = ((uintptr_t)ptr + (uintptr_t)ps - 1u) & ~((uintptr_t)ps - 1u);
    uintptr_t end = ((uintptr_t)ptr + len) & ~((uintptr_t)ps - 1u);
    if (end > a) madvise((void *)a, (size_t)(end - a), MADV_DONTNEED);
}
static int cpf_put(const vkpool_t *p) {
    uint64_t size = p->hi - p->lo, cap = cpf_budget_bytes();
    if (!zcf_on() || !p->buf || !p->shadow || cap == 0 || size > (cap * 3u) / 4u) return 0;
    // IMPORTANT: p->baseline is NEVER freed inside cpf_put() — mtl_pool_del() is the sole owner that
    // frees p.baseline exactly once (prevents hybrid24 SIGSEGV double-free when kept == 0 on Scudo!).
    // Wait at least 3 full submissions (V.sub_seq + 3) before reuse or eviction to prevent GPU MMU -4 faults.
    uint64_t sub_now = __atomic_load_n(&V.sub_seq, __ATOMIC_ACQUIRE);
    uint64_t lf = mtl_last_fence();
    if (lf > sub_now) sub_now = lf;
    cpfree_t e = {p->buf, p->mem, p->shadow, NULL, size, sub_now + 3, p->noncoherent, p->cached};
    cpfree_t old[CPF_N]; int nold = 0;
    uint64_t done = __atomic_load_n(&V.completed_seq, __ATOMIC_ACQUIRE);
    uint64_t fd = vk_fence_done();
    if (fd > done) done = fd;
    pthread_mutex_lock(&g_cpf_mu);
    while (g_ncpf == CPF_N || g_cpf_bytes + size > cap) {
        int i = 0; while (i < g_ncpf && g_cpf[i].fence > done) i++;
        if (i == g_ncpf) break;
        old[nold++] = g_cpf[i]; g_cpf_bytes -= g_cpf[i].size;
        memmove(g_cpf + i, g_cpf + i + 1, (size_t)(g_ncpf - i - 1) * sizeof *g_cpf);
        g_ncpf--;
    }
    int kept = g_ncpf < CPF_N && g_cpf_bytes + size <= cap;
    if (kept) { g_cpf[g_ncpf++] = e; g_cpf_bytes += size; g_cpf_kept++; }
    pthread_mutex_unlock(&g_cpf_mu);
    for (int i = 0; i < nold; i++) cpf_free_now(&old[i]);
    return kept;
}

static VkResult pool_copied_alloc(vkpool_t *p, uint64_t size, int cached) {
    if (cpf_take(size, cached, p)) return VK_SUCCESS;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VkResult r = vkCreateBuffer(V.dev, &bi, NULL, &b); if (r) return r;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    uint32_t type = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT), 0);
    if (type == UINT32_MAX) { vkDestroyBuffer(V.dev, b, NULL); return VK_ERROR_FEATURE_NOT_PRESENT; }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    VkDeviceMemory m; r = vkAllocateMemory(V.dev, &ai, NULL, &m);
    if (r) { vkDestroyBuffer(V.dev, b, NULL); return r; }
    void *map; r = vkBindBufferMemory(V.dev, b, m, 0);
    if (!r) r = vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, &map);
    if (r) { vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL); return r; }
    p->buf = b; p->mem = m; p->shadow = map; p->cached = cached;
    p->noncoherent = !(V.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    mem_diag_pool(size, req.memoryTypeBits, type); return VK_SUCCESS;
}
static void pool_cache_sync(vkpool_t *p, int invalidate, uint64_t lo, uint64_t hi) {
    if (!p->noncoherent) return;
    // Each pool owns/maps its allocation; round cache ranges out to whole atoms.
    VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, p->mem, 0, VK_WHOLE_SIZE};
    uint64_t atom = V.props.limits.nonCoherentAtomSize ? V.props.limits.nonCoherentAtomSize : 1, size = p->hi - p->lo;
    range.offset = lo - lo % atom;
    uint64_t pad = (atom - hi % atom) % atom;
    // Use the allocation's end at the pool tail; subtract before rounding to avoid overflow.
    if (hi < size && pad < size - hi) range.size = hi + pad - range.offset;
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0;
    if (invalidate) { VKCHK(vkInvalidateMappedMemoryRanges(V.dev, 1, &range)); }
    else { VKCHK(vkFlushMappedMemoryRanges(V.dev, 1, &range)); }
    if (diag) mem_diag_note(invalidate ? 4 : 3, t, range.size == VK_WHOLE_SIZE ? p->hi - p->lo - range.offset : range.size);
}
typedef struct { uint64_t lo, hi; } sub_gw_t;
// Invalidate ONLY the actual GPU-written disjoint ranges inside a non-coherent copied pool (up to 64 batched
// VkMappedMemoryRange entries per call), eliminating 14+ ms spikes from invalidating multi-MB clean gaps.
static uint64_t pool_cache_invalidate_ranges(vkpool_t *p, const sub_gw_t *gw, int ngw, uint64_t *calls_out) {
    if (!p->noncoherent || ngw <= 0) return 0;
    uint64_t atom = V.props.limits.nonCoherentAtomSize ? V.props.limits.nonCoherentAtomSize : 1;
    uint64_t size = p->hi - p->lo, total_bytes = 0;
    VkMappedMemoryRange ranges[64]; uint32_t nr = 0;
    for (int k = 0; k < ngw; k++) {
        // Clamp in guest address space BEFORE subtracting p->lo to prevent unsigned underflow when gw[k].hi < p->lo!
        uint64_t glo = gw[k].lo > p->lo ? gw[k].lo : p->lo;
        uint64_t ghi = gw[k].hi < p->hi ? gw[k].hi : p->hi;
        if (glo >= ghi) continue;
        uint64_t lo = glo - p->lo, hi = ghi - p->lo;
        uint64_t alo = lo - lo % atom;
        uint64_t pad = (atom - hi % atom) % atom;
        uint64_t ahi = (hi < size && pad < size - hi) ? (hi + pad) : size;
        uint64_t rsz = (ahi >= size) ? VK_WHOLE_SIZE : (ahi - alo);
        // Coalesce with previous range if overlapping or adjacent (within 1 atom).
        if (nr > 0 && ranges[nr - 1].size != VK_WHOLE_SIZE) {
            uint64_t prev_end = ranges[nr - 1].offset + ranges[nr - 1].size;
            if (alo <= prev_end + atom && alo >= ranges[nr - 1].offset) {
                if (ahi >= size) { ranges[nr - 1].size = VK_WHOLE_SIZE; total_bytes += (size > prev_end ? size - prev_end : 0); }
                else if (ahi > prev_end) { ranges[nr - 1].size = ahi - ranges[nr - 1].offset; total_bytes += ahi - prev_end; }
                continue;
            }
        }
        if (nr == 64) {
            VKCHK(vkInvalidateMappedMemoryRanges(V.dev, nr, ranges));
            if (calls_out) (*calls_out)++;
            nr = 0;
        }
        ranges[nr++] = (VkMappedMemoryRange){VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, p->mem, alo, rsz};
        total_bytes += (rsz == VK_WHOLE_SIZE) ? (size - alo) : rsz;
    }
    if (nr > 0) {
        VKCHK(vkInvalidateMappedMemoryRanges(V.dev, nr, ranges));
        if (calls_out) (*calls_out)++;
    }
    return total_bytes;
}
// Freed zero-copy pools' buffers, kept for the next pool of the same size (DM_VK_POOL_REUSE=0 turns it off): a pool made from one skips the hardware buffer's
// allocation (a binder call to the allocator service) and the driver's import, which took Pokémon Scarlet's three loading threads ~130 + ~100 ms each while a thrown
// ball's capture assets load (they drop old pools as they make new ones; the game waits on them, frames of 100-220 ms), and a pool dropped from a game thread no longer
// waits for the GPU (df_add outside a submission waits for every submission so far). An entry is taken only once the GPU has passed the submission open or next when
// it was kept; the oldest the GPU is done with make room
typedef struct { AHardwareBuffer *ahb; VkBuffer buf; VkDeviceMemory mem; uint64_t size, fence; } zcfree_t;
enum { ZCF_N = 128 };
#define ZCF_BYTES (384ull << 20)
static zcfree_t g_zcf[ZCF_N]; static int g_nzcf; static uint64_t g_zcf_bytes; static pthread_mutex_t g_zcf_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_zcf_hit, g_zcf_miss, g_zcf_kept;   // under g_zcf_mu (the pool line)
uint64_t mtl_last_fence(void);
static int pool_log_on(void) { const char *e = getenv("DM_VK_POOL_LOG"); return e && atoi(e); }   // DM_VK_POOL_LOG=1 (live, debugging): every pool made and finalized, logged
static int zcf_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_POOL_REUSE"); v = !(e && !atoi(e)); } return v; }
static void zcf_free_now(const zcfree_t *e) { vkDestroyBuffer(V.dev, e->buf, NULL); vkFreeMemory(V.dev, e->mem, NULL); AHardwareBuffer_release(e->ahb); }
static int zcf_take(uint64_t size, zcfree_t *out) {
    if (!zcf_on()) return 0;
    uint64_t done = vk_fence_done(); int r = 0;
    pthread_mutex_lock(&g_zcf_mu);
    for (int i = 0; i < g_nzcf; i++) if (g_zcf[i].size == size && g_zcf[i].fence <= done) {
        *out = g_zcf[i]; memmove(g_zcf + i, g_zcf + i + 1, (size_t)(g_nzcf - i - 1) * sizeof *g_zcf); g_nzcf--; g_zcf_bytes -= size; r = 1; break; }
    if (r) g_zcf_hit++; else g_zcf_miss++;
    pthread_mutex_unlock(&g_zcf_mu);
    return r;
}
static int zcf_put(AHardwareBuffer *ahb, VkBuffer buf, VkDeviceMemory mem, uint64_t size) {   // 1: kept (the caller frees nothing)
    int rt = vk_low_ram_tier();
    uint64_t zcap = rt >= 2 ? 0ull : (rt == 1 ? (16ull << 20) : ZCF_BYTES);
    if (!zcf_on() || size > zcap / 4) return 0;
    zcfree_t e = {ahb, buf, mem, size, mtl_last_fence() + 3}, old[ZCF_N]; int nold = 0;
    uint64_t done = vk_fence_done();
    pthread_mutex_lock(&g_zcf_mu);
    while (g_nzcf == ZCF_N || g_zcf_bytes + size > zcap) {   // the oldest the GPU is done with go
        int i = 0; while (i < g_nzcf && g_zcf[i].fence > done) i++;
        if (i == g_nzcf) break;
        old[nold++] = g_zcf[i]; g_zcf_bytes -= g_zcf[i].size; memmove(g_zcf + i, g_zcf + i + 1, (size_t)(g_nzcf - i - 1) * sizeof *g_zcf); g_nzcf--;
    }
    int kept = g_nzcf < ZCF_N && g_zcf_bytes + size <= zcap;
    if (kept) { g_zcf[g_nzcf++] = e; g_zcf_bytes += size; g_zcf_kept++; }
    pthread_mutex_unlock(&g_zcf_mu);
    for (int i = 0; i < nold; i++) zcf_free_now(&old[i]);
    return kept;
}
// Small pools take a slot of a slab (DM_VK_POOL_SLABS=0 turns it off): one hardware buffer allocated and imported once, its slots mapped at the pools' addresses
// (its dma-buf mmapped at the slot's offset) and bound at the slot's offset (vkpool_t.boff). A hardware buffer of its own cost a 48 KB pool ~170 us to allocate (a binder
// call to the allocator service) and ~180 us to import on an AYN Thor, Turnip and Qualcomm's driver alike, ~0.9 ms a pool with the mappings, and Pokémon Scarlet makes
// ~1,000 pools of 4-256 KB a second while a thrown ball's capture assets load (1,500 a second while a save loads): its loading threads spent ~0.5 s a throw on them while
// the game waited, in frames of 100-190 ms. Neither driver offers VK_EXT_external_memory_host, which would take the game's own pages. Slots are 1, 2, 3, 4, 6, 8 ... 64 pages
// (at most 50% larger than the pool), 64 a slab, between 2 and 8 MB; slabs are kept for good, as the image blocks are, and a freed slot is taken again once the GPU has
// passed the submission open or next when it was freed
#define SLAB_POOL_MAX (256ull << 10)
static const uint32_t SLAB_PAGES[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
enum { SLAB_NCLS = sizeof SLAB_PAGES / sizeof *SLAB_PAGES, SLAB_SLOTS = 512 };
typedef struct slab_t { struct slab_t *next; AHardwareBuffer *ahb; int fd; uint8_t *map; VkBuffer buf; VkDeviceMemory mem; uint64_t slot_bytes; uint32_t nslot, nfree;
                        uint64_t bits[SLAB_SLOTS / 64], fence[SLAB_SLOTS]; } slab_t;   // bits: 1 = slot free; fence: the submission the GPU must have passed before it is taken
static slab_t *g_slab[SLAB_NCLS]; static pthread_mutex_t g_slab_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_slab_used, g_slab_bytes;   // pools placed in slabs so far, slab memory (under g_slab_mu: the pool line)
static int slab_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_POOL_SLABS"); v = !(e && !atoi(e)); } return v; }
static slab_t *slab_new(int c) {   // a slab of class c, allocated and imported (no lock held)
    uint64_t sb = (uint64_t)SLAB_PAGES[c] << 12; uint32_t n = 64;
    if (sb * n < (2u << 20)) n = (uint32_t)((2u << 20) / sb); if (sb * n > (8u << 20)) n = (uint32_t)((8u << 20) / sb); if (n > SLAB_SLOTS) n = SLAB_SLOTS;
    uint64_t size = sb * n; slab_t *s = calloc(1, sizeof *s); if (!s) return NULL;
    s->slot_bytes = sb; s->nslot = s->nfree = n; s->fd = -1;
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (AHardwareBuffer_allocate(&d, &s->ahb)) { free(s); return NULL; }
    int sv[2], fds[16], nfd = -1;
    if (!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        if (!AHardwareBuffer_sendHandleToUnixSocket(s->ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
        close(sv[0]); close(sv[1]);
    }
    if (nfd >= 1) { s->fd = fds[0]; for (int i = 1; i < nfd; i++) close(fds[i]); }
    s->map = s->fd >= 0 ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0) : MAP_FAILED;
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    int ok = s->map != MAP_FAILED && !vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, s->ahb, &ap) && !vkCreateBuffer(V.dev, &bi, NULL, &s->buf);
    if (ok) {
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, s->buf};
        VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, s->ahb};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
        ok = !vkAllocateMemory(V.dev, &ai, NULL, &s->mem);
        if (ok && vkBindBufferMemory(V.dev, s->buf, s->mem, 0)) { vkFreeMemory(V.dev, s->mem, NULL); ok = 0; }
        if (!ok) vkDestroyBuffer(V.dev, s->buf, NULL);
    }
    if (!ok) {
        static int said; if (!said++) VKLOG("[RED] memory pool slab of %llu KB could not be made: small pools get hardware buffers of their own", (unsigned long long)(size >> 10));
        if (s->map != MAP_FAILED) munmap(s->map, size); if (s->fd >= 0) close(s->fd); AHardwareBuffer_release(s->ahb); free(s); return NULL;
    }
    for (uint32_t i = 0; i < n; i++) s->bits[i >> 6] |= 1ull << (i & 63);
    return s;
}
static int slab_take(uint64_t size, slab_t **out, uint32_t *slot) {   // 1: a slot of at least size bytes
    if (!slab_on() || size > SLAB_POOL_MAX) return 0;
    uint64_t pages = (size + 4095) >> 12; int c = 0; while (c < SLAB_NCLS && SLAB_PAGES[c] < pages) c++;
    if (c == SLAB_NCLS) return 0;
    uint64_t done = vk_fence_done();
    for (int pass = 0; pass < 2; pass++) {
        pthread_mutex_lock(&g_slab_mu);
        for (slab_t *s = g_slab[c]; s; s = s->next) {
            if (!s->nfree) continue;
            for (uint32_t w = 0; w < (s->nslot + 63) / 64; w++) for (uint64_t b = s->bits[w]; b; b &= b - 1) {
                uint32_t i = w * 64 + (uint32_t)__builtin_ctzll(b); if (s->fence[i] > done) continue;
                s->bits[w] &= ~(1ull << (i & 63)); s->nfree--; g_slab_used++; pthread_mutex_unlock(&g_slab_mu); *out = s; *slot = i; return 1;
            }
        }
        pthread_mutex_unlock(&g_slab_mu);
        if (pass) break;
        slab_t *s = slab_new(c); if (!s) return 0;   // every slot taken or still the GPU's: a new slab, made outside the lock
        pthread_mutex_lock(&g_slab_mu); s->next = g_slab[c]; g_slab[c] = s; g_slab_bytes += s->slot_bytes * s->nslot; pthread_mutex_unlock(&g_slab_mu);
    }
    return 0;
}
static void slab_give(slab_t *s, uint32_t i, uint64_t fence) {
    pthread_mutex_lock(&g_slab_mu); s->fence[i] = fence; s->bits[i >> 6] |= 1ull << (i & 63); s->nfree++; pthread_mutex_unlock(&g_slab_mu);
}
// The pools inside a lazy pool that pool_back gives its buffer (see "Pools inside pools" below): n ranges [r[2i], r[2i+1]), by address and apart, whose pages stay their own
typedef struct { const uint64_t *r; int n; } zcin_t;
static void zc_next_in(const zcin_t *in, uint64_t a, uint64_t e, uint64_t *lo, uint64_t *hi) {   // the first of them in [a, e), cut to it; none: *lo = *hi = e
    *lo = *hi = e;
    for (int i = 0; in && i < in->n; i++) if (in->r[2 * i + 1] > a && in->r[2 * i] < e) { *lo = in->r[2 * i] > a ? in->r[2 * i] : a; *hi = in->r[2 * i + 1] < e ? in->r[2 * i + 1] : e; return; }
}
static int zc_anon_fill(void *at, size_t n, size_t off, void *ctx);
// [at, at+n), off bytes into the pool, copied into its buffer, then the buffer's pages mapped over it (also nvn_dirty_swap's fill), but for the pools inside it (in), whose
// pages stay their own: the buffer gets their bytes too. map: the buffer's own mapping (a slab's), else a temporary one is made. 0 on success; -5 / -6: the temporary / a
// fixed mapping failed (the pieces of [at, at+n) mapped before it go back on anonymous pages first, so a failed slice is on no buffer's pages)
typedef struct { int fd; uint64_t off; uint8_t *map; int prot; const zcin_t *in; } zcfill_t;   // off: the pool's offset in the buffer (a slab slot's)
static int zc_fill(void *at, size_t n, size_t off, void *ctx) {
    const zcfill_t *f = ctx; uint64_t o = f->off + off; uint8_t *tmp = f->map ? f->map + o : mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, (off_t)o);
    if (tmp == MAP_FAILED) return -5;
    memcpy(tmp, at, n); if (!f->map) munmap(tmp, n);
    for (uint64_t a = (uintptr_t)at, e = a + n, lo, hi; a < e; a = hi) {   // [a, lo): the piece up to the next pool inside it, [lo, hi)
        zc_next_in(f->in, a, e, &lo, &hi);
        if (lo > a && mmap((void *)(uintptr_t)a, lo - a, f->prot, MAP_SHARED | MAP_FIXED, f->fd, (off_t)(o + (a - (uintptr_t)at))) != (void *)(uintptr_t)a) {
            if (a > (uintptr_t)at) zc_anon_fill(at, a - (uintptr_t)at, 0, (void *)f->in);
            return -6;
        }
    }
    return 0;
}
// lazy: the game runs on, so the pages are replaced under the dirty-page tracker (nvn_dirty_swap: no write lost, 16 MB at a time; 1 = it does not hold them, nothing done;
// 2 = the host wrote into them lately, try later) and mapped read-only for it. done: the bytes from mem now on the buffer's pages, also when it failed part way (a later slice).
// A pool of one slice is swapped whole or not at all, so a slab's slot (SLAB_POOL_MAX) is never left half mapped when it goes back to its slab
static int zc_place(void *mem, size_t len, zcfill_t *f, int lazy, size_t *done) {
    f->prot = lazy ? PROT_READ : PROT_READ | PROT_WRITE;
    if (lazy) return nvn_dirty_swap(mem, len, zc_fill, f, done);
    int rc = zc_fill(mem, len, 0, f); *done = rc ? 0 : len; return rc;
}
// Pages that were a hardware buffer's put back on anonymous memory with their contents, before the buffer is dropped (a lazy pool's swap that failed part way, or an import
// that failed after the copy): left on the dropped buffer, they would stay pinned and, on a Retroid Pocket 5, uncached for the CPU while the game goes on using the memory
static void copy_back(uint8_t *mem, uint64_t size) {   // no game write under it: a pool being made, or one the game has removed
    uint8_t *save = malloc(size); if (save) memcpy(save, mem, size);
    mmap(mem, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (save) { memcpy(mem, save, size); free(save); }
}
// nvn_dirty_swap's fill: anonymous pages, filled, then moved over [at, at+n) read-only in one call per piece between the pools inside it (ctx: zcin_t, or NULL for none)
static int zc_anon_fill(void *at, size_t n, size_t off, void *ctx) {
    (void)off;
    for (uint64_t a = (uintptr_t)at, e = a + n, lo, hi; a < e; a = hi) {
        zc_next_in(ctx, a, e, &lo, &hi); if (lo == a) continue;
        size_t m = lo - a; void *to = (void *)(uintptr_t)a; uint8_t *tmp = mmap(NULL, m, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (tmp == MAP_FAILED) return -5;
        memcpy(tmp, to, m);
        if (mprotect(tmp, m, PROT_READ) || mremap(tmp, m, m, MREMAP_MAYMOVE | MREMAP_FIXED, to) != to) { munmap(tmp, m); return -6; }
    }
    return 0;
}
// [mem, mem+n) back on anonymous pages. lazy: the game is running, so under the tracker as the swap was (nvn_dirty_swap with zc_anon_fill, which may also say try later);
// → a length from mem (vkpool_t left), which mtl_pool_del copies back when the game removes the pool if no later try covered it: 0 when all went back, else n. A put-back
// that stops part way leaves [mem+back, mem+n) on the buffer's pages, and copy_back and pool_back's merge read left from mem, so the slices already back are copied again too
// (anonymous pages copied onto anonymous pages: no harm)
static uint64_t zc_unplace(uint8_t *mem, uint64_t n, int lazy, const zcin_t *in) {   // (in: the pools inside it keep their pages, as in zc_fill)
    if (!n) return 0;
    if (!lazy) { copy_back(mem, n); return 0; }
    size_t back = 0; nvn_dirty_swap(mem, n, zc_anon_fill, (void *)in, &back); return back < n ? n : 0;
}
// Zero copy: the pages of [mem, mem+size) become an AHardwareBuffer's, imported as a VkBuffer (a slab's slot for a small pool, or a kept buffer of a freed pool of that size).
// lazy: a pool given its buffer when the GPU first uses it (pool_back). 0 on success; 1: lazy, and the dirty-page tracker does not hold the pages (nothing done); 2: lazy, and
// the host wrote into them lately (nothing done, try later). On a failure p->left is the bytes from mem still on a dropped buffer's pages (zc_unplace). in: the pools
// inside a lazy one, whose pages stay their own (pool_back; NULL: none); such a pool takes no slab's slot, which would go back to its slab half mapped if a piece failed
static int pool_zero_copy(vkpool_t *p, uint8_t *mem, uint64_t size, int lazy, const zcin_t *in) {
    slab_t *sl; uint32_t si; size_t done = 0;
    // lazy: the tracker's answer before a slab's slot, a kept buffer or a new one is taken. Asked only inside the swap, a pool the host keeps writing into (the game's
    // streaming) cost an allocation of its size, up to 256 MB, on the replay's thread each second it was tried again, and dropping it destroyed a kept buffer another pool
    // could use; try later is not a counted try, so with no limit. (nvn_dirty_swap asks again with each slice's pages BUSY, for a write prepared in between.)
    if (lazy) { int q = nvn_dirty_quiet(mem, size); if (q) return q == 1 || q == 2 ? q : -1; }
    if (!(in && in->n) && slab_take(size, &sl, &si)) {
        uint64_t off = (uint64_t)si * sl->slot_bytes; zcfill_t f = {sl->fd, off, sl->map, 0, NULL};
        int rc = zc_place(mem, size, &f, lazy, &done);
        if (!rc) { p->buf = sl->buf; p->mem = VK_NULL_HANDLE; p->ahb = NULL; p->boff = off; p->slab = sl; p->slot = si; p->zero_copy = 1; return 0; }
        slab_give(sl, si, 0);   // (one slice: nothing of it was swapped)
        if (rc != -5 && rc != -6) return rc == 1 || rc == 2 ? rc : -1;   // the tracker's answer, the same for a buffer of its own
        mem_diag_zc(11, "mmap-fixed slab", size, "errno", errno);   // (the old pages are still there: a buffer of its own)
    }
    AHardwareBuffer_Desc d = {(uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    if (size > 0xffffffffull) { mem_diag_zc(0, "size", size, "limit", -1); return -1; }
    zcfree_t re; int reuse = zcf_take(size, &re);
    AHardwareBuffer *ahb = reuse ? re.ahb : NULL; int ar = reuse ? 0 : AHardwareBuffer_allocate(&d, &ahb);
    if (ar) { mem_diag_zc(1, "AHardwareBuffer_allocate", size, "return", ar); return -1; }
#define ZC_DROP() do { if (reuse) zcf_free_now(&re); else AHardwareBuffer_release(ahb); } while (0)
    int sv[2]; if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        mem_diag_zc(2, "socketpair", size, "errno", errno); ZC_DROP(); return -1; }
    int fds[16], nfd = -1;
    ar = AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]);
    if (!ar) { nfd = recv_fds(sv[1], fds, 16); if (nfd < 1) mem_diag_zc(4, "recv_fds", size, "return", nfd); }
    else mem_diag_zc(3, "AHardwareBuffer_sendHandleToUnixSocket", size, "return", ar);
    close(sv[0]); close(sv[1]);
    if (nfd < 1) { ZC_DROP(); return -1; }
    // copy the pool's current contents into the buffer through a temporary mapping, then put the buffer's pages at the pool's address
    zcfill_t f = {fds[0], 0, NULL, 0, in}; int rc = zc_place(mem, size, &f, lazy, &done);
    if (rc == -5) mem_diag_zc(5, "mmap-temporary", size, "errno", errno);
    else if (rc == -6) mem_diag_zc(6, "mmap-fixed", size, "errno", errno);
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (rc) { p->left = zc_unplace(mem, done, lazy, in); ZC_DROP(); return rc == 1 || rc == 2 ? rc : -1; }   // (a slice swapped before the failing one goes back first)
#undef ZC_DROP
    if (reuse) { p->buf = re.buf; p->mem = re.mem; p->ahb = ahb; p->zero_copy = 1; return 0; }   // imported already
    VkAndroidHardwareBufferPropertiesANDROID ap = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VkResult vr = vkGetAndroidHardwareBufferPropertiesANDROID(V.dev, ahb, &ap);
    if (vr) mem_diag_zc(7, "vkGetAndroidHardwareBufferPropertiesANDROID", size, "VkResult", vr);
    else if ((vr = vkCreateBuffer(V.dev, &bi, NULL, &p->buf))) { mem_diag_zc(8, "vkCreateBuffer", size, "VkResult", vr); p->buf = VK_NULL_HANDLE; }
    else {
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, p->buf};   // the driver requires a dedicated allocation for an imported AHardwareBuffer
        VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, &ded, ahb};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ap.allocationSize, vk_mem_type(ap.memoryTypeBits, 0, 0)};
        if ((vr = vkAllocateMemory(V.dev, &ai, NULL, &p->mem))) { mem_diag_zc(9, "vkAllocateMemory", size, "VkResult", vr); p->mem = VK_NULL_HANDLE; }
        else if ((vr = vkBindBufferMemory(V.dev, p->buf, p->mem, 0))) { mem_diag_zc(10, "vkBindBufferMemory", size, "VkResult", vr); vkFreeMemory(V.dev, p->mem, NULL); p->mem = VK_NULL_HANDLE; }   // (as slab_new)
        if (vr) { vkDestroyBuffer(V.dev, p->buf, NULL); p->buf = VK_NULL_HANDLE; }
    }
    if (vr) { p->left = zc_unplace(mem, size, lazy, in); AHardwareBuffer_release(ahb); return -2; }   // the pool's pages back off the buffer before it goes
    p->ahb = ahb; p->zero_copy = 1;
    return 0;
}
// Pools inside pools. A game may make a pool over memory that lies inside another of its pools: Kirby and the Forgotten Land keeps its resource heap (1.4 GB) in one
// pool, loads each model file into it and makes a small pool over the file's vertex data, and its movie player's textures come and go the same way. The inner pool's pages
// are its own buffer's while it lives, and the outer buffer's again when it goes (pool_copy_back): left as anonymous memory, they were no longer the outer buffer's, and
// the next vk_pool_remap_ro of the outer pool put the buffer's pages back there with what they held when the inner pool was made. The game then found files it had loaded
// since in an older state (a model file relocated twice, or not relocated after ResCast: a hang in the SDK's relocation loop, crashes at main+0x506c30 and others, each
// shortly after a movie, whose frames' uploads bring the mprotect count to the defragmenting's threshold). A live inner pool lost its pages the same way, so the outer
// pool is mapped again only around them.
// g_nest_mu: an inner pool is made (its pages mapped, then the entry added) and dropped (the entry removed, then its pages given back) as one step against the outer
// pool's remap. Taken before g_pool_mu; nothing waits on the dirty-page tracking under it (nvn_dirty_defrag calls the remap with its pages busy)
static pthread_mutex_t g_nest_mu = PTHREAD_MUTEX_INITIALIZER;
static int pool_within(const vkpool_t *q, const vkpool_t *p) {   // q: a chunk of a zero-copy pool that overlaps p and belongs to a smaller pool than p's
    return q->zero_copy && q->base != p->base && q->end - q->base < p->end - p->base && q->lo < p->hi && q->hi > p->lo;
}
// The dma-buf behind a zero-copy pool's pages, whose offset p->boff is p->lo's: a slab's file, or the hardware buffer's (*own: the caller closes it). -1: none
static int pool_fd(const vkpool_t *p, int *own) {
    *own = 0;
    if (!p->zero_copy) return -1;
    if (p->slab) return ((slab_t *)p->slab)->fd;
    int sv[2], fds[16], nfd = -1; if (!p->ahb || socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(p->ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16);
    close(sv[0]); close(sv[1]);
    for (int k = 1; k < nfd; k++) close(fds[k]);
    if (nfd < 1) return -1;
    *own = 1; return fds[0];
}
// A zero-copy pool's dma-buf mapped again over [mem, mem+len) in one piece per chunk, read-only (nvn_dirty_defrag: the mappings its protection split never merge), but for
// the pools inside it, whose pages stay their own. 0 = done; 1 = nothing mapped (no pool is exactly [mem, mem+len): a pool not whole host pages, whose tracked range is
// rounded up to them; or the first mmap failed); -1 = failed after mapping a part read-only (nvn_dirty_defrag makes the writable pages writable again)
int vk_pool_remap_ro(void *mem, size_t len) {
    int rc = 0, mapped = 0; uint64_t done = 0;
    // Drain in-flight GPU work ONLY on zero-copy enabled GPUs before mmap(MAP_FIXED) remaps dma-buf memory.
    if (V.q && !vk_quirk(VQ_NO_ZEROCOPY)) {
        pthread_mutex_lock(&g_vk_queue_mu);
        vkQueueWaitIdle(V.q);
        pthread_mutex_unlock(&g_vk_queue_mu);
    }
    pthread_mutex_lock(&g_nest_mu);
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool && !rc; i++) {
        vkpool_t *p = &g_pool[i]; if (p->base != (uint64_t)(uintptr_t)mem || p->end - p->base != len) continue;
        if (__atomic_load_n(&p->lazy, __ATOMIC_ACQUIRE)) { done += p->hi - p->lo; continue; }   // no buffer yet: anonymous pages, which the kernel merges again by itself
        if (!p->zero_copy) { if (!p->shadow) { rc = -1; break; } done += p->hi - p->lo; continue; }   // a copied pool: nothing to map again (its protection stays as it is), and done, so defrag stops taking its pages busy
        int own, fd = pool_fd(p, &own); if (fd < 0) { rc = -1; break; }
        for (uint64_t a = p->lo; a < p->hi && !rc; ) {   // [a, lo): the piece up to the next inner pool [lo, hi)
            uint64_t lo = p->hi, hi = p->hi;
            for (int j = 0; j < g_npool; j++) {
                const vkpool_t *q = &g_pool[j]; if (!pool_within(q, p) || q->hi <= a) continue;
                uint64_t ql = q->lo > a ? q->lo : a, qh = q->hi < p->hi ? q->hi : p->hi;
                if (ql < lo) { lo = ql; hi = qh; } else if (ql == lo && qh > hi) hi = qh;
            }
            if (lo > a) { void *at = (void *)(uintptr_t)a; rc = mmap(at, lo - a, PROT_READ, MAP_SHARED | MAP_FIXED, fd, (off_t)(a - p->lo + p->boff)) == at ? 0 : -1; mapped |= !rc; }
            a = hi;
        }
        if (own) close(fd);
        if (!rc) done += p->hi - p->lo;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    pthread_mutex_unlock(&g_nest_mu);
    // Only return 0 ("remapped read-only") when dma-buf pages were actually mmap'd; for copied/lazy anonymous
    // pools (mapped == 0), return 1 so nvn_dirty_defrag restores states & resets nmp WITHOUT calling
    // mprotect(RW) + stamp_page() across the whole copied pool (which caused 50-126 ms pool_upload spikes every 300 frames).
    return !rc && mapped && done == len ? 0 : mapped ? -1 : 1;
}
// A pool that goes: its pages take the buffer's contents with them (the game may reuse the memory), as the pages of the pool they lie inside (see "Pools inside pools":
// written into that pool's buffer first, then mapped from it, so the bytes are there at every moment) or else as anonymous memory. p has left the table
static void pool_copy_back(vkpool_t *p) {
    for (uint64_t a = p->lo; a < p->hi; ) {
        uint8_t *mem = (uint8_t *)(uintptr_t)a; uint64_t e = p->hi, next = p->hi; int fd = -1, own = 0; off_t off = 0;
        pthread_rwlock_rdlock(&g_pool_mu);
        const vkpool_t *o = NULL;   // the smallest pool around a
        for (int j = 0; j < g_npool; j++) {
            const vkpool_t *q = &g_pool[j]; if (!pool_within(p, q)) continue;
            if (q->lo > a) { if (q->lo < next) next = q->lo; }
            else if (q->hi > a && (!o || q->end - q->base < o->end - o->base)) o = q;
        }
        if (o) { if (o->hi < e) e = o->hi; fd = pool_fd(o, &own); off = (off_t)(a - o->lo + o->boff); } else e = next;
        pthread_rwlock_unlock(&g_pool_mu);
        uint64_t n = e - a; int ok = 0;
        if (fd >= 0) {
            uint8_t *tmp = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
            if (tmp != MAP_FAILED) { memcpy(tmp, mem, n); munmap(tmp, n); ok = mmap(mem, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, off) == mem; }
            if (own) close(fd);
            if (!ok) { static int said; if (!said++) VKLOG("[RED] memory pool %p +%#llx inside another pool: its pages could not go back to that pool's buffer (errno %d): anonymous memory, which the outer pool's next remap replaces", (void *)mem, (unsigned long long)n, errno); }
        }
        if (!ok) copy_back(mem, n);
        a = e;
    }
}
// Pools over ZC_CHUNK are made of chunks of at most that size, each a pool entry with an AHardwareBuffer of its own: the Adreno 830's gralloc allocator service
// (vendor.qti.hardware.display.allocator-service, snapalloc) aborts on a BLOB of 812 MB ("ubsan: mul-overflow" in SnapConstraintManager::AlignmentToAlignedConstraints),
// and the whole Android UI restarts with it; 400 MB ones work (13 Sentinels: Aegis Rim's 0x32c00000-byte pool). A range across two chunks has no buffer (vk_pool_of)
#define ZC_CHUNK 0x10000000ull
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end, int lazy);
static uint64_t g_zc_bytes, g_cp_bytes, g_lazy_total, g_lazy_bytes, g_used_bytes;   // made zero-copy, copied, made lazy, lazy ones given their buffer, found by vk_pool_of so far (DM_VK_MEMLOG; removed pools are not taken off)
// Lazy pools: a pool's buffer is made only when the GPU first uses it (vk_pool_of, pool_back), through the slabs and kept buffers as at creation; until then its pages stay
// the game's anonymous memory, taken only where it writes and swappable. A hardware buffer's pages are all taken when it is allocated and never swapped: Pokemon Pokopia
// makes 4.96 GB of pools in its first seconds, of which the GPU had used 512 MB by its third frame, and with all of it backed Android's low-memory killer ended the game
// within seconds on an AYN Thor (11.5 GB). The cost moves to the first use: the copy of the pool's contents (up to ZC_CHUNK) on the replay's thread (vk_pool_of: a buffer
// binding, copy, fill or upload; mtl_addr only finds the pool), while the game's writes into each 16 MB slice wait for that slice. Only pools the dirty-page tracker holds
// and whole host pages (nvn_dirty_swap replaces their pages with no write lost: nvnMemoryPoolInitialize registers the range first; a sparse pool's runs are not registered
// and are backed when made).
// DM_VK_ZEROCOPY_LAZY: 0, none; 1, also the chunks of pools over ZC_CHUNK (Pokopia on the Thor; the big pools of Xenoblade Chronicles 3, Tears of the Kingdom and
// 13 Sentinels are not measured with it); 2, every page-aligned pool on any device. Unset: none, or the pools of one chunk where the CPU reads hardware buffers uncached
static int zc_lazy_mode(void) { static int v = -2; if (v == -2) { const char *e = getenv("DM_VK_ZEROCOPY_LAZY"); v = e && *e ? atoi(e) : -1; } return v; }
// Where the CPU reads hardware buffers uncached, the pools of one chunk are lazy (DM_VK_ZEROCOPY_LAZY unset or 1): a pool the GPU never uses (command memory, shader code,
// texture and sampler descriptors, which the replay reads and the GPU does not) keeps the CPU's cached pages, and one it uses is backed the first time. On a Retroid
// Pocket 5 (Adreno 650, Android 13, kernel 4.19) every BLOB hardware buffer, whatever its CPU usage, and every Vulkan memory type, HOST_CACHED included, read at
// 1.7-1.9 GB/s against 16-21 GB/s for malloc (163 ns a scattered line against 43), and wrote at full speed (write-combined); an AYN Thor's read at 35-47 GB/s, as its
// malloc memory. Pokemon Legends: Z-A's replay walked its 36 MB command pool through such pages, 24 ms of a 37 ms frame; with the pool left cached it took 12 ms and
// the game held its 30 fps in handheld mode. The check, at the first pool, reads 1 MB of a new hardware buffer and of malloc memory, best of 3 each
static volatile uint64_t g_read_sink;
static uint64_t read_ns(const uint8_t *p, size_t n) {
    uint64_t best = UINT64_MAX;
    for (int k = 0; k < 3; k++) {
        struct timespec t0, t1; uint64_t s = 0; clock_gettime(CLOCK_MONOTONIC, &t0);
        for (size_t i = 0; i < n / 8; i++) s += ((const uint64_t *)p)[i];
        clock_gettime(CLOCK_MONOTONIC, &t1); g_read_sink += s;
        uint64_t ns = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec)); if (ns < best) best = ns;
    }
    return best ? best : 1;
}
static int zc_uncached(void) {
    static int v = -1; if (v >= 0) return v;
    enum { N = 1 << 20 }; v = 0;
    AHardwareBuffer_Desc d = {N, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0};
    AHardwareBuffer *ahb = NULL; if (AHardwareBuffer_allocate(&d, &ahb)) return v;
    int sv[2], fds[16], nfd = -1;
    if (!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) { if (!AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) nfd = recv_fds(sv[1], fds, 16); close(sv[0]); close(sv[1]); }
    uint8_t *a = nfd >= 1 ? mmap(NULL, N, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0) : MAP_FAILED, *m = malloc(N);
    for (int i = 0; i < nfd; i++) close(fds[i]);
    if (a != MAP_FAILED && m) {
        memset(a, 1, N); memset(m, 1, N);
        uint64_t ta = read_ns(a, N), tm = read_ns(m, N); v = ta > 4 * tm;
        VKLOG("hardware buffers: the CPU reads one at %.0f MB/s, malloc memory at %.0f MB/s%s", N / (ta / 1e9) / 1e6, N / (tm / 1e9) / 1e6,
              v ? ": uncached, so pools of one chunk stay in cached memory until the GPU first uses them" : "");
    }
    if (a != MAP_FAILED) munmap(a, N);
    free(m); AHardwareBuffer_release(ahb);
    return v;
}
// DM_VK_ZEROCOPY_MAX_MB=<n>: pools bigger than n MB are copied, not zero-copy, and kept whole (a copied pool takes no hardware buffer, so no chunks). The Legion Y700
// 2023's gralloc aborts the process (no error returned) allocating a hardware buffer for Unicorn Overlord's 860 MB pool, while its pools up to 250 MB are fine
static int zc_over(uint64_t size) { static uint64_t m = 1; if (m == 1) { const char *e = getenv("DM_VK_ZEROCOPY_MAX_MB"); m = e && atoi(e) > 0 ? (uint64_t)atoi(e) << 20 : 0; } return m && size > m; }
static int zc_off(void) { static int v = -1; if (v < 0) v = (getenv("DM_VK_NO_ZEROCOPY") != NULL) || vk_quirk(VQ_NO_ZEROCOPY); return v; }   // DM_VK_NO_ZEROCOPY or ARM Mali VQ_NO_ZEROCOPY: every pool copied
void mtl_pool_add(const void *mem, uint64_t size) {
    vk_init();
    if (!size) return;
    uint64_t a = (uint64_t)(uintptr_t)mem; int whole = ((a | size) & 0xfff) || zc_over(size);   // copied in one piece
    uint64_t c = whole ? size : ZC_CHUNK; int m = zc_lazy_mode();
    static uint64_t pg; if (!pg) pg = (uint64_t)getpagesize();   // (a 16 KB-page kernel: a pool only 4 KB-aligned is not swapped in place, it is copied as before)
    // (zero copy off: no lazy pools, and no uncached-read check either)
    int lazy = !whole && !zc_off() && !((a | size) & (pg - 1)) && m && (m >= 2 || (size > c ? m == 1 : zc_uncached())) && nvn_dirty_tracked(mem, size);
    for (uint64_t o = 0; o < size; o += c) pool_add_one((const uint8_t *)mem + o, size - o < c ? size - o : c, a, a + size, lazy);
    if (size > c) VKLOG("memory pool %p +%#llx: %llu chunks of at most %#llx", mem, (unsigned long long)size, (unsigned long long)((size + c - 1) / c), c);
}
// The pool line: pools made and dropped with their time on the calling threads and the reuse counts, at most once a second and only for a second in which
// pools took 20 ms or more (loads): "memory pools in the last 1.0 s: 212 made (148.0 MB, 640.0 ms; 150 from kept buffers), 180 dropped (...)"; with lazy pools
// given their buffer that second, also how many, their size and time ("4 given their buffer at first use (96.0 MB, 41.0 ms)")
static uint64_t pool_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static pthread_mutex_t g_pn_mu = PTHREAD_MUTEX_INITIALIZER; static uint64_t g_pn_t0, g_pn_n[3], g_pn_b[3], g_pn_ns[3], g_pn_hit0, g_pn_slab0;
static void pool_note(int kind, uint64_t bytes, uint64_t ns) {   // kind 0: made, 1: dropped, 2: a lazy pool given its buffer (lock order: g_pn_mu, then g_zcf_mu or g_slab_mu)
    uint64_t now = pool_ns(); char b[480] = "";
    pthread_mutex_lock(&g_pn_mu);
    if (!g_pn_t0) g_pn_t0 = now;
    g_pn_n[kind]++; g_pn_b[kind] += bytes; g_pn_ns[kind] += ns;
    if (now - g_pn_t0 >= 1000000000ull) {
        pthread_mutex_lock(&g_zcf_mu); uint64_t hit = g_zcf_hit, kn = (uint64_t)g_nzcf, kb = g_zcf_bytes; pthread_mutex_unlock(&g_zcf_mu);
        pthread_mutex_lock(&g_cpf_mu); hit += g_cpf_hit; kn += (uint64_t)g_ncpf; kb += g_cpf_bytes; pthread_mutex_unlock(&g_cpf_mu);
        pthread_mutex_lock(&g_slab_mu); uint64_t su = g_slab_used, sbytes = g_slab_bytes; pthread_mutex_unlock(&g_slab_mu);
        if (g_pn_ns[0] + g_pn_ns[1] + g_pn_ns[2] >= 20000000ull) {
            int k = snprintf(b, sizeof b, "memory pools in the last %.1f s: %llu made (%.1f MB, %.1f ms; %llu in slab slots, %llu from kept buffers), %llu dropped (%.1f MB, %.1f ms); slabs %.1f MB, %llu buffers kept (%.1f MB)",
                     (now - g_pn_t0) / 1e9, (unsigned long long)g_pn_n[0], g_pn_b[0] / 1048576.0, g_pn_ns[0] / 1e6, (unsigned long long)(su - g_pn_slab0), (unsigned long long)(hit - g_pn_hit0),
                     (unsigned long long)g_pn_n[1], g_pn_b[1] / 1048576.0, g_pn_ns[1] / 1e6, sbytes / 1048576.0, (unsigned long long)kn, kb / 1048576.0);
            if (g_pn_n[2] && k > 0 && (size_t)k < sizeof b)
                snprintf(b + k, sizeof b - (size_t)k, "; %llu given their buffer at first use (%.1f MB, %.1f ms)", (unsigned long long)g_pn_n[2], g_pn_b[2] / 1048576.0, g_pn_ns[2] / 1e6);
        }
        g_pn_t0 = now; g_pn_hit0 = hit; g_pn_slab0 = su; memset(g_pn_n, 0, sizeof g_pn_n); memset(g_pn_b, 0, sizeof g_pn_b); memset(g_pn_ns, 0, sizeof g_pn_ns);
    }
    pthread_mutex_unlock(&g_pn_mu);
    if (*b) VKLOG("%s", b);
}
// The pool's buffer is made before the write lock is taken, and the entry only put in under it: the hardware buffer (a binder call to the allocator service), the copy
// into it and the driver's import take milliseconds each, and every lookup waits on the lock (vk_pool_of: each buffer binding of each draw on the replay thread, and the
// game threads' mtl_addr, once a pool was added or removed). Pokémon Scarlet makes and drops pools on three threads at once while a thrown ball flies (its capture assets,
// ~180 MB read in 1.5 s): the replay thread stood almost still for a second with the GPU idle (simpleperf), in frames of 100-580 ms
static pthread_mutex_t g_lazy_mu = PTHREAD_MUTEX_INITIALIZER;   // one pool backed at a time (pool_back); mtl_pool_del takes it first, so no entry moves while one is backed
static void pool_add_one(const void *mem, uint64_t size, uint64_t base, uint64_t end, int lazy) {
    uint64_t t0 = pool_ns();
    vkpool_t q, *p = &q; memset(p, 0, sizeof *p);
    p->lo = (uint64_t)(uintptr_t)mem; p->hi = p->lo + size; p->base = base; p->end = end;
    // A pool over memory that another pool covers is never lazy: it is made now, as "Pools inside pools" says (lazy, its buffer would come when the GPU first used it, and
    // a pool made inside it in between, or the one around it, would then lose or take pages). One that overlaps a lazy pool still without its buffer is made under
    // g_lazy_mu, so not while pool_back maps that pool around the pools inside it (lock order: g_lazy_mu, g_nest_mu, g_pool_mu)
    int near = 0;   // 1: it overlaps another pool; 3: one of them is lazy, with no buffer yet
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int j = 0; j < g_npool; j++) if (g_pool[j].base != base && g_pool[j].lo < p->hi && g_pool[j].hi > p->lo) {
        near |= __atomic_load_n(&g_pool[j].lazy, __ATOMIC_ACQUIRE) == 1 ? 3 : 1;
        if (g_pool[j].end - g_pool[j].base > end - base) __atomic_store_n(&g_pool[j].has_inner, 1, __ATOMIC_RELEASE);   // the new pool lies inside it (pool_find's caches)
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (near && lazy) { lazy = 0; static int said; if (said++ < 8) VKLOG("memory pool %p +%#llx overlaps another pool: zero-copy now, not lazy", mem, (unsigned long long)size); }
    if (near & 2) pthread_mutex_lock(&g_lazy_mu);
    int inner = 0;   // inside another pool: made as one step against that pool's remap (see "Pools inside pools")
    pthread_mutex_lock(&g_nest_mu);
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int j = 0; j < g_npool && !inner; j++) inner = g_pool[j].zero_copy && g_pool[j].base != base && g_pool[j].lo < p->hi && g_pool[j].hi > p->lo;
    pthread_rwlock_unlock(&g_pool_mu);
    if (!inner) pthread_mutex_unlock(&g_nest_mu);
    int zc = ((uintptr_t)mem & 0xfff) || (size & 0xfff) ? -3 : vk_quirk(VQ_NO_ZEROCOPY) ? -6 : zc_off() ? -4 : zc_over(end - base) ? -5 : lazy ? (p->lazy = 1, 0) : pool_zero_copy(p, (uint8_t *)mem, size, 0, NULL);
    if (zc) {
        static int said;
        if (!said++) {
            if (zc == -6) VKLOG("memory pool %p +%#llx: ARM Mali quirk VQ_NO_ZEROCOPY active (copied pool + cached/range-readback defaults)", mem, (unsigned long long)size);
            else VKLOG("[RED] memory pool %p +%#llx could not be zero-copy (%d): a GPU copy is refreshed before each submission (slow)", mem, (unsigned long long)size, zc);
        }
        int cached = pool_cached_default();
        VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
        if (cached && vkFlushMappedMemoryRanges && vkInvalidateMappedMemoryRanges) r = pool_copied_alloc(p, size, 1);
        if (r == VK_ERROR_DEVICE_LOST) VKCHK(r);
        if (r) {
            if (cached && mem_diag_on()) VKLOG("[memdiag] cached pool unavailable (%d), using coherent memory", r);
            VKCHK(pool_copied_alloc(p, size, 0));
        }
        p->span = nvn_dirty_span(mem, size);
        size_t nblk = (size_t)((size + (64u << 10) - 1u) / (64u << 10));
        if (nblk) p->no_hot = calloc(nblk, 1);
    }
    pthread_rwlock_wrlock(&g_pool_mu);
    if (g_npool == MAXPOOL) { pthread_rwlock_unlock(&g_pool_mu); die("[vk] too many memory pools"); }
    { static uint64_t ser; p->serial = ++ser; }
    g_pool[g_npool] = q; pool_ord_insert(g_npool++); __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
    int np = g_npool;
    pthread_rwlock_unlock(&g_pool_mu);
    if (inner) pthread_mutex_unlock(&g_nest_mu);
    if (near & 2) pthread_mutex_unlock(&g_lazy_mu);
    if (np >= 1024 && !(np & (np - 1))) VKLOG("%d memory pools live", np);
    __atomic_fetch_add(zc ? &g_cp_bytes : lazy ? &g_lazy_total : &g_zc_bytes, size, __ATOMIC_RELAXED);
    static int nlog; if (nlog++ < 24 || pool_log_on()) VKLOG("memory pool %p +%#llx: %s%s (zero-copy %.1f MB, lazy %.1f MB, copied %.1f MB)", mem, (unsigned long long)size, zc ? "copied" : lazy ? "zero-copy when the GPU first uses it" : "zero-copy", q.slab ? " (a slab slot)" : "",
                                        g_zc_bytes / 1048576.0, g_lazy_total / 1048576.0, g_cp_bytes / 1048576.0);
    pool_note(0, size, pool_ns() - t0);
}
// A lazy pool given its buffer, the first time vk_pool_of finds it. serial: the pool's, read with the entry under the read lock. The buffer is made with g_lazy_mu held and the
// entry's own lock free (lookups go on), and put in under the write lock, as pool_add_one does. A failed try is made again a second later, up to LAZY_TRIES in all (an
// allocation refused under memory pressure may pass later); meanwhile, and after the last, the pool has no buffer, and the draws that use it are drawn with a zero buffer
// or skipped ([RED] lines). A pool the host wrote into in the last second (nvn_dirty_swap's 2: a file read may still be writing into it) is tried again a second later
// without counting as a failed try. → 0: backed; 1: the entry holds another pool now (look again); -1: no buffer
enum { LAZY_TRIES = 8 };
static int pair_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static int pool_back(vkpool_t *p, uint64_t serial) {
    uint64_t t0 = pool_ns(), lo = 0, size = 0; int rc, said = 0, gave_up = 0;
    pthread_mutex_lock(&g_lazy_mu);
    // still that pool: a removed last entry keeps its old contents (serial included) and pool_add_one may fill it, so the index is checked too, under the read lock;
    // with g_lazy_mu held no entry is removed or moved, and new ones go after it
    pthread_rwlock_rdlock(&g_pool_mu);
    int st = p - g_pool < g_npool && p->serial == serial ? __atomic_load_n(&p->lazy, __ATOMIC_ACQUIRE) : -1;
    if (st == 1 && t0 < p->lazy_at) st = 2;   // waiting to try again
    lo = p->lo; size = p->hi - p->lo;
    // the pools inside it keep their own pages, as in vk_pool_remap_ro (see "Pools inside pools"): none of them is lazy (pool_add_one), and with g_lazy_mu held none is made
    // or dropped over this pool meanwhile. Kirby and the Forgotten Land makes small pools over files loaded into its 1.4 GB heap pool (#192, #239)
    uint64_t *inr = NULL; int inn = 0, incap = 0;
    if (st == 1) for (int j = 0; j < g_npool; j++) if (pool_within(&g_pool[j], p)) {
        if (inn == incap) { uint64_t *r = realloc(inr, (size_t)(incap = incap ? 2 * incap : 8) * 2 * sizeof *r); if (!r) { st = 2; break; } inr = r; }
        inr[2 * inn] = g_pool[j].lo > lo ? g_pool[j].lo : lo; inr[2 * inn + 1] = g_pool[j].hi < p->hi ? g_pool[j].hi : p->hi; inn++;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (inn > 1) {   // by address, and the ones that touch or overlap joined
        qsort(inr, (size_t)inn, 2 * sizeof *inr, pair_cmp); int k = 0;
        for (int i = 1; i < inn; i++) if (inr[2 * i] <= inr[2 * k + 1]) { if (inr[2 * i + 1] > inr[2 * k + 1]) inr[2 * k + 1] = inr[2 * i + 1]; } else { k++; inr[2 * k] = inr[2 * i]; inr[2 * k + 1] = inr[2 * i + 1]; }
        inn = k + 1;
    }
    if (st == 1) {
        vkpool_t q; memset(&q, 0, sizeof q); zcin_t in = {inr, inn};
        rc = pool_zero_copy(&q, (uint8_t *)(uintptr_t)lo, size, 1, &in);
        if (inn && !rc) { static int n; if (n++ < 8) VKLOG("memory pool %#llx +%#llx: given its buffer around the %d pools inside it", (unsigned long long)lo, (unsigned long long)size, inn); }
        pthread_rwlock_wrlock(&g_pool_mu);
        if (!rc) {   // (the whole pool on the new buffer: pages an earlier try left on a dropped one included)
            p->buf = q.buf; p->mem = q.mem; p->ahb = q.ahb; p->boff = q.boff; p->slab = q.slab; p->slot = q.slot; p->zero_copy = 1; p->left = 0; __atomic_store_n(&p->lazy, 0, __ATOMIC_RELEASE);
            __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);   // a descriptor set memo built with vk_zero_buffer() for it is not reused (set_memo_get keys on vk_pool_gen)
        } else {
            if (q.left > p->left) p->left = q.left;   // from mem, so the longest covers the others
            if (rc == 2) p->lazy_at = pool_ns() + 1000000000ull;   // written by the host lately: later, and not a failed try
            else { gave_up = rc == 1 || ++p->lazy_n >= LAZY_TRIES; p->lazy_at = pool_ns() + 1000000000ull; if (gave_up) __atomic_store_n(&p->lazy, 2, __ATOMIC_RELEASE); }   // (1: the tracker will not hold it later either)
        }
        uint32_t tries = p->lazy_n;
        pthread_rwlock_unlock(&g_pool_mu);
        if (rc == 2) { static uint64_t n; uint64_t k = ++n; if (k <= 8 || !(k & (k - 1))) VKLOG("memory pool %#llx +%#llx: the host wrote into it in the last second, so its buffer is made a second later (drawn with a zero buffer or skipped meanwhile; %llu so far)",
                                                                                      (unsigned long long)lo, (unsigned long long)size, (unsigned long long)k); rc = -1; }
        else if (rc) { static int n; if (n++ < 8 || gave_up) said = rc; rc = -1; if (said) VKLOG("[RED] memory pool %#llx +%#llx: no buffer when the GPU first used it (%d%s), try %u; %s", (unsigned long long)lo,
                  (unsigned long long)size, said, said == 1 ? ": the dirty-page tracker does not hold its pages" : "", tries ? tries : 1, gave_up ? "no more tries: draws that use it are drawn with a zero buffer or skipped" :
                  "tried again in a second, its draws drawn with a zero buffer or skipped meanwhile"); }
        else __atomic_fetch_add(&g_lazy_bytes, size, __ATOMIC_RELAXED);
    } else rc = st < 0 ? 1 : st ? -1 : 0;   // removed, waiting or failed, or backed by another thread meanwhile
    pthread_mutex_unlock(&g_lazy_mu);
    free(inr);
    if (st == 1 && !rc) {
        uint64_t ns = pool_ns() - t0; pool_note(2, size, ns);
        static int nlog; if (ns >= 16000000ull && nlog++ < 32) VKLOG("memory pool %#llx +%#llx: its buffer took %.1f ms when the GPU first used it", (unsigned long long)lo, (unsigned long long)size, ns / 1e6);
    }
    return rc;
}
static uint64_t g_pool_drops_total;
void mtl_pool_del(const void *mem) {
    if (pool_log_on()) VKLOG("memory pool %p finalized (present %llu)", mem, (unsigned long long)mtl_last_fence());
    for (;;) {   // every chunk of the pool
        pthread_mutex_lock(&g_lazy_mu); pthread_mutex_lock(&g_nest_mu); pthread_rwlock_wrlock(&g_pool_mu);   // (lock order: g_lazy_mu, g_nest_mu, g_pool_mu)
        int i = 0; while (i < g_npool && g_pool[i].base != (uint64_t)(uintptr_t)mem) i++;
        if (i == g_npool) { pthread_rwlock_unlock(&g_pool_mu); pthread_mutex_unlock(&g_nest_mu); pthread_mutex_unlock(&g_lazy_mu); return; }
        vkpool_t p = g_pool[i]; g_pool[i] = g_pool[--g_npool]; pool_ord_remove(i); __atomic_fetch_add(&g_pool_gen, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_pool_drops_total, 1, __ATOMIC_RELAXED);
        int inner = 0, inlazy = 0;   // its pages go back to the pool around it, as one step against that pool's remap, and against pool_back when that pool is lazy, still without its buffer
        for (int j = 0; j < g_npool && p.zero_copy; j++) { inner |= pool_within(&p, &g_pool[j]); inlazy |= pool_within(&p, &g_pool[j]) && __atomic_load_n(&g_pool[j].lazy, __ATOMIC_ACQUIRE) == 1; }
        pthread_rwlock_unlock(&g_pool_mu); if (!inlazy) pthread_mutex_unlock(&g_lazy_mu);
        if (!inner) pthread_mutex_unlock(&g_nest_mu);
        uint64_t t0 = pool_ns();
        if (p.zero_copy) pool_copy_back(&p);
        else if (p.left) copy_back((uint8_t *)(uintptr_t)p.lo, p.left);   // a lazy pool's pages that a failed try left on a dropped buffer (zc_unplace)
        if (inner) pthread_mutex_unlock(&g_nest_mu);
        if (inlazy) pthread_mutex_unlock(&g_lazy_mu);
        free(p.hot); p.hot = NULL;
        free(p.gpu_blk); p.gpu_blk = NULL;
        free(p.no_hot); p.no_hot = NULL;
        free(p.cpu_blk_gen); p.cpu_blk_gen = NULL;
        // Single authoritative free of p.baseline (calloc'd in vk_pools_sync_): NEVER freed inside cpf_put(),
        // eliminating 100% of Scudo double-free SIGSEGVs (0x75e2820ff0) when cpf_put() returns kept == 0!
        if (p.baseline) madvise_release_buf(p.baseline, (size_t)(p.hi - p.lo));
        free(p.baseline); p.baseline = NULL;
        { void vk_tbuf_drop_pool(uint64_t); vk_tbuf_drop_pool(p.serial); }   // the texel buffer views over its buffer (gfx_vk_tex.c), before the buffer itself goes or is kept
        uint64_t sub_now = __atomic_load_n(&V.sub_seq, __ATOMIC_ACQUIRE);
        uint64_t lf = mtl_last_fence();
        if (lf > sub_now) sub_now = lf;
        if (p.slab) { slab_give(p.slab, p.slot, sub_now + 3); }   // 3-submission margin before slab slot reuse
        else if (!p.zero_copy && p.shadow && cpf_put(&p)) {
            // Kept (buf, mem, shadow) in copied-pool reuse cache (baseline already freed above)
        } else {
            if (p.buf && !(p.zero_copy && p.ahb && zcf_put(p.ahb, p.buf, p.mem, p.hi - p.lo))) {
                // Non-blocking fence-tagged deferred retire (retire_fence = V.sub_seq + 3):
                // thread-safe from game threads without touching g_cur or calling vkQueueWaitIdle!
                pool_retire_push(p.buf, p.mem, p.ahb, sub_now + 3);
            }
        }
        pool_note(1, p.hi - p.lo, pool_ns() - t0);
    }
}
extern int g_pool_slots;   // gfx_vk_exec.c: DM_VK_POOL_SLOTS (live)
// back: a lazy pool is given its buffer (vk_pool_of); 0: it is only found (mtl_addr: the CPU's address of shader code at SetShaders, which the GPU may never read)
static vkpool_t *pool_find(uint64_t addr, uint64_t len, int back) {
    // the pools this thread found, while none was added or removed: the replay looks up every buffer binding of every draw, and the read lock alone was ~6% of it.
    // One slot per hash of the address's 4 KB page (a hit is checked against the pool's range, so a slot shared by two pages only costs a miss). The last four
    // pools found, tried in turn, were too few for Breath of the Wild: it keeps over 4,096 pools live and a frame's draws bind buffers in hundreds of them, so
    // most lookups took the lock and the search: 13% of a Galaxy S25 Ultra's replay thread (DM_VK_PROF: 9% in the lock calls, 4% in the function) and 4% with
    // these slots; the replay of 4,850 draws in its woods went from 11.3 to 8.1 ms a frame, and that spot from 30 fps to 60. Pages of different pools that hash to
    // one slot evict each other, so a slot miss tries the last four pools found, as before the slots, before it takes the lock: a game with a few hot pools and
    // more than 512 pages bound a frame loses nothing to them. DM_VK_POOL_SLOTS=0 (live): the last four alone, for A/B runs against `replay per frame`.
    // A pool in the slots or the last four has its buffer: a lazy one found without back is not kept, and giving a lazy pool its buffer bumps g_pool_gen.
    // A hit is checked against the pool's range only, so a pool with another pool inside it (has_inner, "Pools inside pools") is never answered from them: an address in the
    // inner pool lies in the outer pool's range too, and the outer pool's buffer no longer holds those pages. Fire Emblem: Three Houses, once its title's class demo had
    // played, bound a dialogue sprite's vertices and indices through the outer pool found earlier (a stale copy), and the GPU stopped with "RBBM: GPC error" (a device
    // lost 0.2 s into the first dialogue line after character creation, 20 runs of 20; 0 of 3 with DM_VK_POOL_SLOTS=0, which left only the last four)
    enum { PC = 512 }; static __thread struct { uint32_t gen; vkpool_t *p[PC], *last[4]; } c;   // gen: g_pool_gen + 1 (0: empty)
    uint32_t g = __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE) + 1; unsigned ck = (unsigned)(((addr >> 12) * 0x9E3779B97F4A7C15ull) >> 55);   // (9 bits)
    if (c.gen == g) {
        if (g_pool_slots) { vkpool_t *p = c.p[ck]; if (p && addr >= p->lo && addr + len <= p->hi && !__atomic_load_n(&p->has_inner, __ATOMIC_ACQUIRE)) return p; }
        for (int i = 0; i < 4; i++) { vkpool_t *p = c.last[i]; if (p && addr >= p->lo && addr + len <= p->hi && !__atomic_load_n(&p->has_inner, __ATOMIC_ACQUIRE)) { c.p[ck] = p; return p; } }
    }
    vkpool_t *r; int across, lazy; uint64_t serial;
again:   // (pool_back found another pool in the entry: the one looked up was removed meanwhile)
    r = NULL; across = 0; lazy = 0; serial = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    int lo = 0, hi = g_npool;   // the last pool by lo that starts at or below addr
    while (lo < hi) { int m = (lo + hi) / 2; if (g_pool[g_pool_ord[m]].lo <= addr) lo = m + 1; else hi = m; }
    if (lo > 0) { vkpool_t *p = &g_pool[g_pool_ord[lo - 1]]; if (addr + len <= p->hi) r = p; }
    if (!r)   // not in that one: the scan as before (pools over the same memory, and the range across two chunks it reports)
        for (int i = 0; i < g_npool; i++) if (addr >= g_pool[i].lo && addr + len <= g_pool[i].hi) { r = &g_pool[i]; break; }
                                           else if (addr >= g_pool[i].lo && addr < g_pool[i].hi && addr + len <= g_pool[i].end) across = 1;
    if (r) { serial = r->serial; lazy = __atomic_load_n(&r->lazy, __ATOMIC_ACQUIRE); if (lazy == 1 && r->lazy_at && pool_ns() < r->lazy_at) lazy = 2; }
    pthread_rwlock_unlock(&g_pool_mu);
    if (lazy && !back) return r;   // (not kept in the cache, which holds pools with their buffer)
    if (lazy == 2) r = NULL;   // its buffer could not be made (pool_back said why), for now or for good
    else if (lazy) { int b = pool_back(r, serial); if (b > 0) goto again; if (b) r = NULL; }
    if (r && back && !__atomic_load_n(&r->used, __ATOMIC_RELAXED) && !__atomic_exchange_n(&r->used, 1, __ATOMIC_RELAXED)) __atomic_fetch_add(&g_used_bytes, r->hi - r->lo, __ATOMIC_RELAXED);
    if (r) { if (c.gen != g) { memset(&c, 0, sizeof c); c.gen = g; } c.p[ck] = r; memmove(&c.last[1], &c.last[0], 3 * sizeof *c.last); c.last[0] = r; }
    if (!r && across) { static uint64_t n; uint64_t k = ++n; if (!(k & (k - 1))) LOG("[RED] [vk] %llu ranges across two chunks of a memory pool (ZC_CHUNK; this one %#llx +%#llx): no buffer for them", (unsigned long long)k, (unsigned long long)addr, (unsigned long long)len); }
    return r;
}
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len) { return pool_find(addr, len, 1); }
int vk_pool_is_copied(const void *addr, size_t len) {
    if (!addr || !len) return 0;
    vkpool_t *p = pool_find((uint64_t)(uintptr_t)addr, (uint64_t)len, 0);
    return p && !p->zero_copy && p->shadow != NULL;
}
static inline void pool_mark_no_hot(vkpool_t *p, uint64_t a, uint64_t n) {
    if (!p || !p->no_hot || !n || a > UINT64_MAX - n) return;
    uint64_t glo = a > p->lo ? a : p->lo;
    uint64_t ghi = (a + n) < p->hi ? (a + n) : p->hi;
    if (glo >= ghi) return;
    uint64_t size = p->hi - p->lo;
    size_t nblk = (size_t)((size + (64u << 10) - 1u) / (64u << 10));
    size_t b0 = (size_t)((glo - p->lo) >> 16), b1 = (size_t)((ghi - 1u - p->lo) >> 16);
    if (b1 >= nblk) b1 = nblk ? nblk - 1 : 0;
    for (size_t b = b0; b <= b1 && b < nblk; b++) __atomic_store_n(&p->no_hot[b], 1, __ATOMIC_RELEASE);
}
void vk_pool_ban_hot(const void *addr, size_t len) {
    if (!addr || !len) return;
    uint64_t a = (uint64_t)(uintptr_t)addr, n = (uint64_t)len;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        vkpool_t *p = &g_pool[i];
        if (!p->shadow || !p->no_hot || p->hi <= a || p->lo >= a + n) continue;
        pool_mark_no_hot(p, a, n);
    }
    pthread_rwlock_unlock(&g_pool_mu);
}
uint32_t vk_pool_gen(void) { return __atomic_load_n(&g_pool_gen, __ATOMIC_ACQUIRE); }   // pools added or removed (gfx_vk_exec.c's descriptor set memo: vk_pool_of's results hold while it is unchanged)
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    vkpool_t *p = vk_pool_of(addr, len ? len : 1);
    if (!p) return 0;
    *buf = p->buf; *off = addr - p->lo + p->boff; return 1;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    vkpool_t *p = pool_find(addr, len ? len : 1, 0);
    if (!p) return 0;
    if (buf) *buf = (void *)(uintptr_t)p->lo; if (off) *off = addr - p->lo; return 1;
}
void mtl_alias_drop_pool(const void *pool) { void vk_tex_drop_pool(const void *); vk_tex_drop_pool(pool); }
// The NVN flags of the pool at mem (hle_nvn.c, after mtl_pool_add): a copied pool the CPU maps uncached (CPU_UNCACHED, 0x2) is one the game writes and the GPU reads;
// the game reads GPU results from cached memory, so such a pool is not copied back after each submission
void vk_pool_set_flags(const void *mem, int flags) {
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pool[i].base == (uint64_t)(uintptr_t)mem) g_pool[i].flags = flags;   // (every chunk of it)
    pthread_rwlock_unlock(&g_pool_mu);
}
static uint64_t g_sync_bytes, g_sync_full;   // copied pools: bytes copied by the incremental sync, full copies
// Copied pools: the guest's bytes into the GPU copy. The first sync copies the whole pool; later ones copy only the 64 KB blocks with pages the CPU wrote since the last
// sync (nvn_dirty's write-protect tracking: a block is protected again before it is copied, so a write during the copy is caught by the next sync). An 860 MB
// pool copied whole at every submission (Unicorn Overlord on a Legion Y700 2023) ran at a few frames a second
// Explicit GPU-only WAR/WAW pre-transfer and RAW post-transfer barriers for overlapped staged uploads:
// On ARM Mali Valhall CSF (e.g. Mali-G720 driver 49.1.0), Geometry/Fragment/Compute and Transfer run on
// separate hardware endpoints; naming every shader/vertex-input/indirect reader stage explicitly guarantees
// Frame N-1 finishes reading p->buf BEFORE Frame N's vkCmdCopyBuffer overwrites it — with zero CPU fence_wait!
static const VkPipelineStageFlags POOL_CONSUMER_STAGES =
    VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
    VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
    VK_PIPELINE_STAGE_TRANSFER_BIT;
static const VkAccessFlags POOL_CONSUMER_ACCESS =
    VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
    VK_ACCESS_INDEX_READ_BIT |
    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
    VK_ACCESS_UNIFORM_READ_BIT |
    VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_SHADER_WRITE_BIT |
    VK_ACCESS_TRANSFER_READ_BIT |
    VK_ACCESS_TRANSFER_WRITE_BIT;
static inline VkPipelineStageFlags pool_consumer_stages(void) {
    return POOL_CONSUMER_STAGES | (V.has_gs ? VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT : 0);
}
static void vk_pre_transfer_barrier(VkCommandBuffer cb) {
    // Full GPU-only WAR/WAW barrier before s->up buffer/texture transfers: guarantees ALL prior GPU
    // graphics/compute/attachment/transfer reads & writes complete before staging writes land in p->buf or im->img.
    VkMemoryBarrier mb = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
        VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void vk_post_transfer_barrier(VkCommandBuffer cb) {
    // Full GPU-only RAW/WAW barrier after s->up transfers: publishes ALL staged pool copies and texture
    // uploads (pkg_upload_, lin_copy, vk_tex_prepare) to EVERY graphics, compute, transfer, and attachment stage.
    VkMemoryBarrier mb = {
        VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        POOL_CONSUMER_ACCESS |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        pool_consumer_stages() | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        0, 1, &mb, 0, NULL, 0, NULL);
}
static inline void pool_bump_cpu_gen(vkpool_t *p, uint64_t off, uint64_t len) {
    if (!p || !p->cpu_blk_gen || !len) return;
    uint64_t size = p->hi - p->lo;
    size_t nblk = (size_t)((size + (64u << 10) - 1u) / (64u << 10));
    size_t b0 = (size_t)(off >> 16), b1 = (size_t)((off + len - 1u) >> 16);
    if (b1 >= nblk) b1 = nblk ? nblk - 1 : 0;
    for (size_t b = b0; b <= b1 && b < nblk; b++) __atomic_fetch_add(&p->cpu_blk_gen[b], 1u, __ATOMIC_RELEASE);
}
static inline uint32_t pool_sum_cpu_gen(const vkpool_t *p, uint64_t glo, uint64_t ghi) {
    if (!p || !p->cpu_blk_gen || glo >= ghi || glo < p->lo || ghi > p->hi) return 0u;
    uint64_t size = p->hi - p->lo;
    size_t nblk = (size_t)((size + (64u << 10) - 1u) / (64u << 10));
    size_t b0 = (size_t)((glo - p->lo) >> 16), b1 = (size_t)((ghi - 1u - p->lo) >> 16);
    if (b1 >= nblk) b1 = nblk ? nblk - 1 : 0;
    uint32_t sum = 0u;
    for (size_t b = b0; b <= b1 && b < nblk; b++) sum += __atomic_load_n(&p->cpu_blk_gen[b], __ATOMIC_ACQUIRE);
    return sum;
}
static void vk_pools_sync_(VkCommandBuffer cb, int staged_overlap);
static void vk_pools_sync_ex(VkCommandBuffer cb, int staged_overlap) { extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; if (g_prof_on) clock_gettime(CLOCK_MONOTONIC, &a);
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0, bytes = diag ? g_sync_bytes : 0;
    vk_pools_sync_(cb, staged_overlap); if (diag) mem_diag_note(1, t, g_sync_bytes - bytes);
    if (g_prof_on) { clock_gettime(CLOCK_MONOTONIC, &b); g_prof_ns[2] += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); } }
void vk_pools_sync(VkCommandBuffer cb) { vk_pools_sync_ex(cb, 0); }
static int mem_nonzero64(const uint8_t *p, uint64_t n) {
    uint64_t i = 0;
#ifdef __aarch64__
    for (; i + 64 <= n; i += 64) {
        uint64x2_t v0 = vld1q_u64((const uint64_t *)(p + i));
        uint64x2_t v1 = vld1q_u64((const uint64_t *)(p + i + 16));
        uint64x2_t v2 = vld1q_u64((const uint64_t *)(p + i + 32));
        uint64x2_t v3 = vld1q_u64((const uint64_t *)(p + i + 48));
        uint64x2_t o = vorrq_u64(vorrq_u64(v0, v1), vorrq_u64(v2, v3));
        if ((vgetq_lane_u64(o, 0) | vgetq_lane_u64(o, 1)) != 0) return 1;
    }
#else
    for (; i + 32 <= n; i += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p + i, 8); memcpy(&w1, p + i + 8, 8);
        memcpy(&w2, p + i + 16, 8); memcpy(&w3, p + i + 24, 8);
        if (w0 | w1 | w2 | w3) return 1;
    }
#endif
    for (; i < n; i++) if (p[i]) return 1;
    return 0;
}
static void vk_pools_sync_(VkCommandBuffer cb, int staged_overlap) {
    (void)cb;
    uint64_t sync_t0 = st_now_ns(), rearm_ns = 0;
    int diag = mem_diag_on(); uint64_t ns[4] = {0}, amount[4] = {0};
    uint64_t reason_count[4] = {0}, reason_bytes[4] = {0};
#define SYNC_MEASURE(k, n, op) do { if (diag) { uint64_t t_ = mem_diag_ns(); op; ns[k] += mem_diag_ns() - t_; amount[k] += (n); } else { op; } } while (0)
    // FLUSH_GAP=0: never flush clean non-coherent CPU cache lines between dirty blocks (prevents clobbering
    // GPU-written SSBO/copy ranges on ARM Mali).
    // Coalesced mprotect() with per-64KB fault-release spans (nvn_dirty_protect_blk) cuts dirty_rearm spikes
    // from 22-30 ms -> <1 ms WITHOUT cascading 1-byte writes across multi-MB merged spans (fixes hybrid15 10.3x fault storm).
    // Batched VkBufferCopy (up to 64 regions per vkCmdCopyBuffer) cuts upload command overhead.
    // Idle-tracked hot 64KB blocks stay writable for 240 unchanged syncs (~4 s at 60 FPS) so active Unity ring buffers
    // never thrash SIGSEGV faults while genuinely dead loading blocks still cool down cleanly.
    enum { BLK = 64 << 10, HOT_THR = 3, HOT_IDLE_LIMIT = 243, HOT_MAX_BLOCKS = 48, FLUSH_GAP = 0, STAGE_MAX_RUN = 512 << 10, COPY_BATCH = 64, PROT_BRIDGE_GAP = 4 * (64 << 10) };
    int safe_sync = vk_pool_safe_sync();   // 2=Pure Serial Safe Sync (default, 0% flicker), 1=Pipelined Staged Safe Sync, 0=Legacy
    int serial_safe = (safe_sync >= 2);
    // CRITICAL: Mode 2 (serial_safe) NEVER uses hot_blocks (PROT_RW) or 1KB p->baseline diff skip!
    // Every dirty block is write-protected and copied verbatim from Guest RAM to HOST_CACHED p->shadow (100% 0% flicker!).
    int allow_hot = (!serial_safe) && (safe_sync != 1 || vk_safe1_hot_blocks());
    int use_stage = (!serial_safe) && ((safe_sync == 1) || staged_overlap || vk_pool_overlap());
    int use_diff = (!serial_safe) && use_stage && (safe_sync != 1 || vk_safe1_diff1k());
    int staged_any = 0, host_wrote_any = 0;
    uint64_t hot_blocks_now = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        vkpool_t *p = &g_pool[i]; if (!p->shadow) continue;
        // NVN CPU_NO_ACCESS (flags & 0x1) pools are strictly GPU-only after nvnMemoryPoolInitialize:
        // skip mprotect(PROT_READ) and per-frame dirty scans once initially synced (eliminates 18-23 ms dirty_rearm spikes!).
        if ((p->flags & 0x1) && p->synced) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)p->lo; uint64_t size = p->hi - p->lo, st = nvn_dirty_now();
        size_t nblk = (size_t)((size + BLK - 1) / BLK);
        // Lazy baseline allocation for CPU_UNCACHED (p->flags & 0x2) pools in serial_safe mode:
        // Unity creates & recycles dozens of 16 MB CPU_UNCACHED streaming/vertex pools that the GPU never writes to.
        // Since pools_copy_back() skips CPU_UNCACHED pools that have 0 overlap in s->gw[], allocating p->baseline
        // ONLY when !serial_safe, !(p->flags & 0x2), or p->gpu_wrote_ever saves ~400-800 MB of RAM on 4 GB phones
        // and cuts 50% of CPU memcpy + page-fault traffic on streaming pools (eliminating 18-39 ms pool_upload spikes)!
        int need_base = !(p->flags & 0x1) && (!serial_safe || !(p->flags & 0x2) || p->gpu_wrote_ever);
        if (!p->baseline && need_base) {
            p->baseline = calloc(1, size);
            if (!p->baseline) die("[RED] [vk] copied-pool baseline allocation failed (%llu bytes)", (unsigned long long)size);
            if (p->synced && p->shadow) {
                pthread_mutex_lock(&g_baseline_mu);
                memcpy(p->baseline, p->shadow, (size_t)size);
                pthread_mutex_unlock(&g_baseline_mu);
                p->baseline_complete = 1;
                p->baseline_fresh = 0;
            } else {
                p->synced = 0;
                p->baseline_fresh = 1;
            }
            if (mem_diag_on()) VKLOG("[memdiag] copy-back baseline allocated: %.2f MiB", size / 1048576.0);
        }
        if (!p->hot && !(p->flags & 0x1)) {
            size_t hcnt = serial_safe ? (size_t)((size + (4u << 10) - 1u) / (4u << 10)) : nblk;
            if (hcnt) p->hot = calloc(hcnt, 1);
        }
        if (!p->gpu_blk && nblk && !(p->flags & 0x1) && !serial_safe) p->gpu_blk = calloc(nblk, 1);
        if (!p->no_hot && nblk && !(p->flags & 0x1)) p->no_hot = calloc(nblk, 1);
        if (!p->cpu_blk_gen && nblk && !(p->flags & 0x1)) p->cpu_blk_gen = calloc(nblk, sizeof *p->cpu_blk_gen);
        if (!p->span && !(p->flags & 0x1) && (p->span = nvn_dirty_span(lo, size))) p->synced = 0;   // tracked only now: copied whole once more, protected
        if (!p->synced || (!p->span && !(p->flags & 0x1))) {   // untracked or initial sync
            if (diag) { int reason = !p->synced ? 0 : 1; reason_count[reason]++; reason_bytes[reason] += size; }
            if (p->flags & 0x1) {
                // CPU_NO_ACCESS initial GPU zero-fill (~0.1 ms, zero CPU baseline RAM, zero mprotect TLB shootdown!).
                VkCommandBuffer ucb = vk_upload_cmd();
                if (!(size & 3ull)) {
                    vkCmdFillBuffer(ucb, p->buf, p->boff, size, 0);
                    staged_any = 1;
                }
                g_sync_full++; p->synced = 1; p->baseline_complete = 1; p->baseline_fresh = 0; p->cpf_reused = 0; p->sync_gen = 0; p->stamp0 = p->stamp = st; continue;
            }
        if (p->span) {
            // One coalesced O(1) mprotect(PROT_READ) per fresh pool (4 KB pages in safe_sync=2, 64 KB otherwise).
            uint64_t tr = st_now_ns();
            SYNC_MEASURE(1, size, nvn_dirty_protect_span_blk(p->span, lo, size, serial_safe ? (4u << 10) : BLK));
            rearm_ns += st_now_ns() - tr;
        }
            if (!p->synced && !p->baseline_complete && p->span && size >= (1ull << 20) && !(size & 3ull)) {
                // Sparse initial sync for large (>=1 MiB) tracked copied pools:
                // In serial_safe (safe_sync=2), GPU Frame N-1 is already 100% idle via mtl_wait(g_fence), so ANY
                // cached pool (even if p->used == 1) safely writes non-zero blocks directly into HOST_CACHED p->shadow
                // + L1-hot baseline copy + flush and GPU vkCmdFillBuffer ONLY for disjoint zero runs (eliminates 19-24 ms spikes!).
                int brand_new_cached = p->cached && (serial_safe || (__atomic_load_n(&p->used, __ATOMIC_ACQUIRE) == 0));
                VkCommandBuffer ucb = vk_upload_cmd();
                if (brand_new_cached) {
                    uint64_t nz_lo = size, nz_hi = 0, z_lo = size, z_hi = 0, nz_bytes = 0;
                    for (size_t b = 0; b < nblk; b++) {
                        uint64_t off = (uint64_t)b * BLK;
                        uint64_t n = size - off < BLK ? size - off : BLK;
                        if (!mem_nonzero64(lo + off, n)) {
                            if (p->baseline && !p->baseline_fresh) {
                                pthread_mutex_lock(&g_baseline_mu);
                                memset(p->baseline + off, 0, (size_t)n);
                                pthread_mutex_unlock(&g_baseline_mu);
                            }
                            if (nz_hi > nz_lo) {
                                uint64_t rn = nz_hi - nz_lo;
                                pthread_mutex_lock(&g_baseline_mu);
                                SYNC_MEASURE(2, rn, memcpy(p->shadow + nz_lo, lo + nz_lo, rn));
                                if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + nz_lo, lo + nz_lo, rn));
                                pool_bump_cpu_gen(p, nz_lo, rn);
                                pthread_mutex_unlock(&g_baseline_mu);
                                pool_cache_sync(p, 0, nz_lo, nz_hi);
                                host_wrote_any = 1; nz_bytes += rn; nz_lo = size; nz_hi = 0;
                            }
                            if (z_hi == 0) { z_lo = off; z_hi = off + n; }
                            else if (off == z_hi) { z_hi = off + n; }
                            else { vkCmdFillBuffer(ucb, p->buf, p->boff + z_lo, z_hi - z_lo, 0); staged_any = 1; z_lo = off; z_hi = off + n; }
                            continue;
                        }
                        if (z_hi > z_lo) { vkCmdFillBuffer(ucb, p->buf, p->boff + z_lo, z_hi - z_lo, 0); staged_any = 1; z_lo = size; z_hi = 0; }
                        if (nz_hi == 0) { nz_lo = off; nz_hi = off + n; }
                        else if (off == nz_hi) { nz_hi = off + n; }
                        else {
                            uint64_t rn = nz_hi - nz_lo;
                            pthread_mutex_lock(&g_baseline_mu);
                            SYNC_MEASURE(2, rn, memcpy(p->shadow + nz_lo, lo + nz_lo, rn));
                            if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + nz_lo, lo + nz_lo, rn));
                            pool_bump_cpu_gen(p, nz_lo, rn);
                            pthread_mutex_unlock(&g_baseline_mu);
                            pool_cache_sync(p, 0, nz_lo, nz_hi);
                            host_wrote_any = 1; nz_bytes += rn; nz_lo = off; nz_hi = off + n;
                        }
                    }
                    if (z_hi > z_lo) { vkCmdFillBuffer(ucb, p->buf, p->boff + z_lo, z_hi - z_lo, 0); staged_any = 1; }
                    if (nz_hi > nz_lo) {
                        uint64_t rn = nz_hi - nz_lo;
                        pthread_mutex_lock(&g_baseline_mu);
                        SYNC_MEASURE(2, rn, memcpy(p->shadow + nz_lo, lo + nz_lo, rn));
                        if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + nz_lo, lo + nz_lo, rn));
                        pool_bump_cpu_gen(p, nz_lo, rn);
                        pthread_mutex_unlock(&g_baseline_mu);
                        pool_cache_sync(p, 0, nz_lo, nz_hi);
                        host_wrote_any = 1; nz_bytes += rn;
                    }
                    g_sync_bytes += nz_bytes;
                } else {
                    vkCmdFillBuffer(ucb, p->buf, p->boff, size, 0);
                    VkBufferMemoryBarrier fb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, p->buf, p->boff, size};
                    vkCmdPipelineBarrier(ucb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 1, &fb, 0, NULL);
                    VkBufferCopy cbatch[COPY_BATCH]; uint32_t ncb = 0; VkBuffer cur_sb = VK_NULL_HANDLE;
                    uint64_t nz_lo = size, nz_hi = 0, nz_bytes = 0;
                    for (size_t b = 0; b < nblk; b++) {
                        uint64_t off = (uint64_t)b * BLK;
                        uint64_t n = size - off < BLK ? size - off : BLK;
                        if (!mem_nonzero64(lo + off, n)) {
                            if (p->baseline && !p->baseline_fresh) {
                                pthread_mutex_lock(&g_baseline_mu);
                                memset(p->baseline + off, 0, (size_t)n);
                                pthread_mutex_unlock(&g_baseline_mu);
                            }
                            if (nz_hi > nz_lo) {
                                uint64_t rn = nz_hi - nz_lo;
                                VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_scratch(rn, 16, &sb, &so);
                                pthread_mutex_lock(&g_baseline_mu);
                                if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + nz_lo, lo + nz_lo, rn));
                                SYNC_MEASURE(2, rn, memcpy(stg, lo + nz_lo, rn));
                                pool_bump_cpu_gen(p, nz_lo, rn);
                                pthread_mutex_unlock(&g_baseline_mu);
                                if (ncb && (sb != cur_sb || ncb == COPY_BATCH)) { vkCmdCopyBuffer(ucb, cur_sb, p->buf, ncb, cbatch); ncb = 0; }
                                cur_sb = sb; cbatch[ncb++] = (VkBufferCopy){so, p->boff + nz_lo, rn};
                                nz_bytes += rn; nz_lo = size; nz_hi = 0;
                            }
                            continue;
                        }
                        if (nz_hi == 0) { nz_lo = off; nz_hi = off + n; }
                        else if (off == nz_hi && (off + n - nz_lo) <= STAGE_MAX_RUN) { nz_hi = off + n; }
                        else {
                            uint64_t rn = nz_hi - nz_lo;
                            VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_scratch(rn, 16, &sb, &so);
                            pthread_mutex_lock(&g_baseline_mu);
                            if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + nz_lo, lo + nz_lo, rn));
                            SYNC_MEASURE(2, rn, memcpy(stg, lo + nz_lo, rn));
                            pool_bump_cpu_gen(p, nz_lo, rn);
                            pthread_mutex_unlock(&g_baseline_mu);
                            if (ncb && (sb != cur_sb || ncb == COPY_BATCH)) { vkCmdCopyBuffer(ucb, cur_sb, p->buf, ncb, cbatch); ncb = 0; }
                            cur_sb = sb; cbatch[ncb++] = (VkBufferCopy){so, p->boff + nz_lo, rn};
                            nz_bytes += rn; nz_lo = off; nz_hi = off + n;
                        }
                    }
                    if (nz_hi > nz_lo) {
                        uint64_t rn = nz_hi - nz_lo;
                        VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_scratch(rn, 16, &sb, &so);
                        pthread_mutex_lock(&g_baseline_mu);
                        if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + nz_lo, lo + nz_lo, rn));
                        SYNC_MEASURE(2, rn, memcpy(stg, lo + nz_lo, rn));
                        pool_bump_cpu_gen(p, nz_lo, rn);
                        pthread_mutex_unlock(&g_baseline_mu);
                        if (ncb && (sb != cur_sb || ncb == COPY_BATCH)) { vkCmdCopyBuffer(ucb, cur_sb, p->buf, ncb, cbatch); ncb = 0; }
                        cur_sb = sb; cbatch[ncb++] = (VkBufferCopy){so, p->boff + nz_lo, rn};
                        nz_bytes += rn;
                    }
                    if (ncb) vkCmdCopyBuffer(ucb, cur_sb, p->buf, ncb, cbatch);
                    staged_any = 1;
                    g_sync_bytes += nz_bytes;
                }
            } else if (use_stage) {
                // Stage small/already-used untracked pools (chunked <= STAGE_MAX_RUN) through per-slot vk_scratch() -> s->up:
                VkCommandBuffer ucb = vk_upload_cmd();
                VkBufferCopy cbatch[COPY_BATCH]; uint32_t ncb = 0; VkBuffer cur_sb = VK_NULL_HANDLE;
                for (uint64_t off = 0; off < size; ) {
                    uint64_t rn = size - off < STAGE_MAX_RUN ? size - off : STAGE_MAX_RUN;
                    VkBuffer sb; VkDeviceSize so; uint8_t *stg = vk_scratch(rn, 16, &sb, &so);
                    pthread_mutex_lock(&g_baseline_mu);
                    if (p->baseline) SYNC_MEASURE(3, rn, memcpy(p->baseline + off, lo + off, rn));
                    SYNC_MEASURE(2, rn, memcpy(stg, lo + off, rn));
                    pool_bump_cpu_gen(p, off, rn);
                    pthread_mutex_unlock(&g_baseline_mu);
                    if (ncb && (sb != cur_sb || ncb == COPY_BATCH)) { vkCmdCopyBuffer(ucb, cur_sb, p->buf, ncb, cbatch); ncb = 0; }
                    cur_sb = sb; cbatch[ncb++] = (VkBufferCopy){so, p->boff + off, rn};
                    off += rn;
                }
                if (ncb) vkCmdCopyBuffer(ucb, cur_sb, p->buf, ncb, cbatch);
                staged_any = 1;
                g_sync_bytes += size;
            } else {
                pthread_mutex_lock(&g_baseline_mu);
                SYNC_MEASURE(2, size, memcpy(p->shadow, lo, size));
                if (p->baseline) SYNC_MEASURE(3, size, memcpy(p->baseline, lo, size));
                pool_bump_cpu_gen(p, 0, size);
                pthread_mutex_unlock(&g_baseline_mu);
                pool_cache_sync(p, 0, 0, size);
                host_wrote_any = 1;
                g_sync_bytes += size;
            }
            if (p->hot) memset(p->hot, 0, serial_safe ? (size_t)((size + (4u << 10) - 1u) / (4u << 10)) : nblk);
            if (p->gpu_blk) memset(p->gpu_blk, 0, nblk);
            g_sync_full++; p->synced = 1; p->baseline_complete = (p->baseline != NULL); p->baseline_fresh = 0; p->cpf_reused = 0; p->sync_gen = 0; p->stamp0 = p->stamp = st; continue; }
        p->sync_gen++;
        // ---- Mode 2 (serial_safe) 4 KB Safe Hot-Page Exemption + 60-Frame Decay + Lossless Compare Skip (v1.1.2-hybrid34) ----
        // 100% 0% flicker AND ultra-low upload bandwidth (fixes hybrid33 vivo V2109 3.1 MB unconditional copy regression!):
        //   1. 4 KB page granularity (SBLK = 4096) with tiered global cap: 128 pages (512 KB) on Tier >= 2, 384 pages (1.5 MB) on Tier <= 1.
        //   2. GPU-written 64 KB blocks (p->no_hot[b >> 4]) are NEVER promoted to hot, so every hot 4 KB page is 100% CPU-only!
        //   3. Lossless compare skip: for hot CPU-only 4 KB pages, memcmp against p->baseline (or cached p->shadow) skips
        //      redundant uploads when unchanged (0% flicker!) and auto-cools to PROT_READ after 60 frames (HOT4_DECAY)!
        if (serial_safe) {
            enum { SBLK = 4 << 10, FLUSH_BATCH = 64, HOT4_THR = 3, HOT4_DECAY = 63 };
            uint32_t hot_cap = (vk_gpu_tier() >= 2) ? 128u : 384u;
            size_t snblk = (size_t)((size + SBLK - 1) / SBLK);
            uint8_t sact_stk[4096];
            uint8_t *sact = snblk <= sizeof sact_stk ? sact_stk : (uint8_t *)malloc(snblk);
            if (!sact) die("[RED] [vk] serial_safe action table alloc failed (%zu blocks)", snblk);
            uint64_t s_prot_lo = size, s_prot_hi = 0, s_run_lo = size, s_run_hi = 0;
            uint64_t scan_start = diag ? mem_diag_ns() : 0, work_start = ns[1] + ns[2] + ns[3];
            int has_base = (p->baseline != NULL);
            int can_cmp_base = has_base && p->baseline_complete && !p->gpu_wrote_ever;
            int can_cmp_shadow = !has_base && p->cached && p->shadow && !p->gpu_wrote_ever;
            int any_cooled = 0;
            // Pass 1: classify dirty & hot 4 KB pages; skip unchanged CPU-only hot pages & decay after 60 frames.
            for (size_t b = 0; b < snblk; b++) {
                uint64_t off = (uint64_t)b * SBLK;
                uint64_t n = size - off < SBLK ? size - off : SBLK;
                uint8_t nohot = p->no_hot ? __atomic_load_n(&p->no_hot[b >> 4], __ATOMIC_ACQUIRE) : 0;
                uint8_t hcnt = p->hot ? __atomic_load_n(&p->hot[b], __ATOMIC_ACQUIRE) : 0;
                if (hcnt >= HOT4_THR) {
                    if (nohot || hot_blocks_now >= hot_cap) {
                        __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                        sact[b] = 1;
                        if (s_prot_hi == 0) { s_prot_lo = off; s_prot_hi = off + n; }
                        else if (off == s_prot_hi) { s_prot_hi = off + n; }
                        else {
                            uint64_t tr = st_now_ns();
                            SYNC_MEASURE(1, s_prot_hi - s_prot_lo, nvn_dirty_protect_span_blk(p->span, lo + s_prot_lo, s_prot_hi - s_prot_lo, SBLK));
                            rearm_ns += st_now_ns() - tr;
                            s_prot_lo = off; s_prot_hi = off + n;
                        }
                        continue;
                    }
                    // 60-frame decay: periodically re-arm hot page to PROT_READ so cold pages stop being scanned
                    uint8_t next_hc = (uint8_t)(hcnt + 1);
                    int unchanged = 0;
                    if (can_cmp_base) unchanged = (memcmp(lo + off, p->baseline + off, (size_t)n) == 0);
                    else if (can_cmp_shadow) unchanged = (memcmp(lo + off, p->shadow + off, (size_t)n) == 0);
                    if (next_hc >= HOT4_DECAY) {
                        __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                        any_cooled = 1;
                        sact[b] = unchanged ? 4 : 3;   // 4 = unchanged + re-protect; 3 = upload + re-protect
                        continue;
                    }
                    __atomic_store_n(&p->hot[b], next_hc, __ATOMIC_RELEASE);
                    hot_blocks_now++;
                    sact[b] = unchanged ? 0 : 2;       // lossless skip when CPU-only page is byte-identical!
                    continue;
                }
                if (!nvn_dirty_since_span(p->span, lo + off, n, p->stamp0)) {
                    if (p->hot && hcnt) __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                    sact[b] = 0;
                    continue;
                }
                int promote = (!nohot && p->hot && hot_blocks_now < hot_cap && (hcnt + 1 >= HOT4_THR));
                if (p->hot) __atomic_store_n(&p->hot[b], nohot ? 0 : (promote ? HOT4_THR : (uint8_t)(hcnt + 1)), __ATOMIC_RELEASE);
                if (promote) {
                    hot_blocks_now++;
                    sact[b] = 2;   // promoted to hot: skip mprotect(PROT_READ) so next frame incurs 0 SIGSEGV faults!
                } else {
                    sact[b] = 1;
                    if (s_prot_hi == 0) { s_prot_lo = off; s_prot_hi = off + n; }
                    else if (off == s_prot_hi) { s_prot_hi = off + n; }
                    else {
                        uint64_t tr = st_now_ns();
                        SYNC_MEASURE(1, s_prot_hi - s_prot_lo, nvn_dirty_protect_span_blk(p->span, lo + s_prot_lo, s_prot_hi - s_prot_lo, SBLK));
                        rearm_ns += st_now_ns() - tr;
                        s_prot_lo = off; s_prot_hi = off + n;
                    }
                }
            }
            if (s_prot_hi > s_prot_lo) {
                uint64_t tr = st_now_ns();
                SYNC_MEASURE(1, s_prot_hi - s_prot_lo, nvn_dirty_protect_span_blk(p->span, lo + s_prot_lo, s_prot_hi - s_prot_lo, SBLK));
                rearm_ns += st_now_ns() - tr;
            }
            // Pass 2: copy changed/dirty 4 KB runs -> either slot-owned s->up staging (when staged_overlap==1)
            // or directly to p->shadow + batched vkFlushMappedMemoryRanges (serial). Coalesces adjacent 4 KB runs!
            VkMappedMemoryRange fbatch[FLUSH_BATCH]; uint32_t nfb = 0;
            VkBufferCopy scbatch[COPY_BATCH]; uint32_t nscb = 0; VkBuffer scur_sb = VK_NULL_HANDLE;
            uint64_t fatom = V.props.limits.nonCoherentAtomSize ? V.props.limits.nonCoherentAtomSize : 1;
            for (size_t b = 0; b < snblk; ) {
                if (sact[b] == 0 || sact[b] == 4) { b++; continue; }
                // Coalesce contiguous active 4 KB blocks (up to 512 KB per staging/copy run)
                size_t b0 = b;
                uint64_t off0 = (uint64_t)b0 * SBLK;
                uint64_t rlen = 0;
                while (b < snblk && (sact[b] == 1 || sact[b] == 2 || sact[b] == 3) && rlen < STAGE_MAX_RUN) {
                    uint64_t boff = (uint64_t)b * SBLK;
                    uint64_t bn = size - boff < SBLK ? size - boff : SBLK;
                    rlen += bn;
                    b++;
                }
                if (staged_overlap) {
                    VkCommandBuffer ucb = vk_upload_cmd();
                    VkBuffer sb; VkDeviceSize so;
                    uint8_t *stg = vk_scratch(rlen, 16, &sb, &so);
                    if (has_base) {
                        pthread_mutex_lock(&g_baseline_mu);
                        SYNC_MEASURE(2, rlen, memcpy(stg, lo + off0, (size_t)rlen)); g_sync_bytes += rlen;
                        SYNC_MEASURE(3, rlen, memcpy(p->baseline + off0, stg, (size_t)rlen));
                        pool_bump_cpu_gen(p, off0, rlen);
                        pthread_mutex_unlock(&g_baseline_mu);
                    } else {
                        SYNC_MEASURE(2, rlen, memcpy(stg, lo + off0, (size_t)rlen)); g_sync_bytes += rlen;
                    }
                    if (nscb && (sb != scur_sb || nscb == COPY_BATCH)) {
                        vkCmdCopyBuffer(ucb, scur_sb, p->buf, nscb, scbatch);
                        nscb = 0;
                    }
                    scur_sb = sb;
                    scbatch[nscb++] = (VkBufferCopy){so, p->boff + off0, rlen};
                    staged_any = 1;
                } else {
                    if (has_base) {
                        pthread_mutex_lock(&g_baseline_mu);
                        SYNC_MEASURE(2, rlen, memcpy(p->shadow + off0, lo + off0, (size_t)rlen)); g_sync_bytes += rlen;
                        SYNC_MEASURE(3, rlen, memcpy(p->baseline + off0, lo + off0, (size_t)rlen));
                        pool_bump_cpu_gen(p, off0, rlen);
                        pthread_mutex_unlock(&g_baseline_mu);
                    } else {
                        SYNC_MEASURE(2, rlen, memcpy(p->shadow + off0, lo + off0, (size_t)rlen)); g_sync_bytes += rlen;
                    }
                    if (s_run_hi == 0) { s_run_lo = off0; s_run_hi = off0 + rlen; }
                    else if (off0 <= s_run_hi + FLUSH_GAP) { s_run_hi = off0 + rlen; }
                    else {
                        if (p->noncoherent) {
                            uint64_t alo = s_run_lo - s_run_lo % fatom;
                            uint64_t pad = (fatom - s_run_hi % fatom) % fatom;
                            VkDeviceSize rsz = (s_run_hi < size && pad < size - s_run_hi) ? (s_run_hi + pad - alo) : VK_WHOLE_SIZE;
                            fbatch[nfb++] = (VkMappedMemoryRange){VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, p->mem, alo, rsz};
                            if (nfb == FLUSH_BATCH) { VKCHK(vkFlushMappedMemoryRanges(V.dev, nfb, fbatch)); nfb = 0; }
                        }
                        s_run_lo = off0; s_run_hi = off0 + rlen;
                    }
                    host_wrote_any = 1;
                }
            }
            if (nscb) {
                vkCmdCopyBuffer(vk_upload_cmd(), scur_sb, p->buf, nscb, scbatch);
                nscb = 0;
            }
            if (s_run_hi > s_run_lo && p->noncoherent) {
                uint64_t alo = s_run_lo - s_run_lo % fatom;
                uint64_t pad = (fatom - s_run_hi % fatom) % fatom;
                VkDeviceSize rsz = (s_run_hi < size && pad < size - s_run_hi) ? (s_run_hi + pad - alo) : VK_WHOLE_SIZE;
                fbatch[nfb++] = (VkMappedMemoryRange){VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, p->mem, alo, rsz};
            }
            if (nfb > 0) {
                int dflush = diag; uint64_t tf = dflush ? mem_diag_ns() : 0;
                VKCHK(vkFlushMappedMemoryRanges(V.dev, nfb, fbatch));
                if (dflush) mem_diag_note(3, tf, nfb * SBLK);
            }
            // Pass 3: re-protect any hot 4 KB pages that hit 60-frame decay
            if (any_cooled) {
                uint64_t clo = size, chi = 0;
                for (size_t b = 0; b < snblk; b++) {
                    if (sact[b] != 3 && sact[b] != 4) continue;
                    uint64_t off = (uint64_t)b * SBLK;
                    uint64_t n = size - off < SBLK ? size - off : SBLK;
                    if (chi == 0) { clo = off; chi = off + n; }
                    else if (off == chi) { chi = off + n; }
                    else {
                        uint64_t tr = st_now_ns();
                        nvn_dirty_protect_span_blk(p->span, lo + clo, chi - clo, SBLK);
                        rearm_ns += st_now_ns() - tr;
                        clo = off; chi = off + n;
                    }
                }
                if (chi > clo) {
                    uint64_t tr = st_now_ns();
                    nvn_dirty_protect_span_blk(p->span, lo + clo, chi - clo, SBLK);
                    rearm_ns += st_now_ns() - tr;
                }
            }
            if (sact != sact_stk) free(sact);
            if (diag) { ns[0] += mem_diag_ns() - scan_start - (ns[1] + ns[2] + ns[3] - work_start); amount[0] += size; }
            if (!p->baseline_complete && p->baseline && !staged_overlap) {
                pthread_mutex_lock(&g_baseline_mu);
                SYNC_MEASURE(3, size, memcpy(p->baseline, p->shadow, size));
                pthread_mutex_unlock(&g_baseline_mu);
                p->baseline_complete = 1;
                p->baseline_fresh = 0;
            }
            p->stamp0 = p->stamp; p->stamp = st;
            continue;
        }
        int probe_rearm = !use_diff && ((p->sync_gen & 511u) == 0);
        uint64_t scan_start = diag ? mem_diag_ns() : 0, work_start = ns[1] + ns[2] + ns[3];
        uint64_t run_lo = size, run_hi = 0;
        uint64_t prot_lo = size, prot_hi = 0;
        VkBufferCopy cbatch[COPY_BATCH]; uint32_t ncb = 0; VkBuffer cur_sb = VK_NULL_HANDLE;
#define FLUSH_PROT_RUN() do { \
            if (prot_hi > prot_lo) { \
                uint64_t tr_ = st_now_ns(); \
                SYNC_MEASURE(1, prot_hi - prot_lo, nvn_dirty_protect_span_blk(p->span, lo + prot_lo, prot_hi - prot_lo, BLK)); \
                rearm_ns += st_now_ns() - tr_; \
                prot_lo = size; prot_hi = 0; \
            } \
        } while (0)
#define ADD_PROT_RUN(off_, n_) do { \
            if (prot_hi == 0) { prot_lo = (off_); prot_hi = (off_) + (n_); } \
            else if ((off_) <= prot_hi + PROT_BRIDGE_GAP) { prot_hi = (off_) + (n_); } \
            else { FLUSH_PROT_RUN(); prot_lo = (off_); prot_hi = (off_) + (n_); } \
        } while (0)
#define FLUSH_COPY_BATCH(ucb_) do { \
            if (ncb) { vkCmdCopyBuffer((ucb_), cur_sb, p->buf, ncb, cbatch); ncb = 0; } \
        } while (0)
#define STAGE_SNAP_BYTES(dst_off_, src_ptr_, rn_) do { \
            VkCommandBuffer ucb_ = vk_upload_cmd(); \
            VkBuffer sb_; VkDeviceSize so_; uint8_t *stg_ = vk_scratch((rn_), 16, &sb_, &so_); \
            SYNC_MEASURE(2, (rn_), memcpy(stg_, (src_ptr_), (rn_))); \
            g_sync_bytes += (rn_); \
            if (ncb && (sb_ != cur_sb || ncb == COPY_BATCH)) FLUSH_COPY_BATCH(ucb_); \
            cur_sb = sb_; cbatch[ncb++] = (VkBufferCopy){so_, p->boff + (dst_off_), (rn_)}; \
            staged_any = 1; \
        } while (0)
        // Pipelined Zero-Stall Safe Sync (v1.1.2-hybrid23):
        //   Pass A: Classify 64 KB blocks & flush coalesced mprotect(PROT_READ) BEFORE reading Guest RAM.
        //           GPU-written blocks (p->no_hot[b]) and CPU-backed VkImage blocks (vk_pool_ban_hot -> p->no_hot[b])
        //           are NEVER allowed into hot_blocks so dynamic textures & SSBOs always fault cleanly.
        //           Non-hot dirty runs coalesce across up to PROT_BRIDGE_GAP (256 KB) of already-PROT pages
        //           (never across a hot block!) into one mprotect() syscall, cutting 24 ms dirty_rearm spikes -> <0.3 ms.
        //   Pass B: BOTH frozen dirty blocks (act=1) and hot blocks (act=2) hold g_baseline_mu ONCE atomically across
        //           memcpy(g_sync_snap, lo+off), 1 KB NEON diff, p->baseline update, AND pool_bump_cpu_gen — closing
        //           100% of double-read tears and lock-order races against completion_main()'s async merge!
        //   Pass C: Demoted idle hot blocks flush mprotect(PROT_READ) and re-verify atomically under g_baseline_mu.
        static uint8_t g_sync_snap[BLK];
        uint8_t act_stk[2048];
        uint8_t *act = nblk <= sizeof act_stk ? act_stk : (uint8_t *)malloc(nblk);
        if (!act) die("[RED] [vk] vk_pools_sync_ action table allocation failed (%zu blocks)", nblk);
        // ---- Pass A: classify & write-protect non-hot dirty blocks FIRST ----
        for (size_t b = 0; b < nblk; b++) {
            uint64_t off = (uint64_t)b * BLK;
            uint64_t n = size - off < BLK ? size - off : BLK;
            uint8_t gblk = p->gpu_blk ? __atomic_load_n(&p->gpu_blk[b], __ATOMIC_ACQUIRE) : 0;
            uint8_t nohot = p->no_hot ? __atomic_load_n(&p->no_hot[b], __ATOMIC_ACQUIRE) : 0;
            uint8_t hot_cnt = p->hot ? __atomic_load_n(&p->hot[b], __ATOMIC_ACQUIRE) : 0;
            if ((gblk || nohot || !allow_hot) && hot_cnt >= HOT_THR) {
                // Previously hot writable block touched by GPU write, overlapping VkImage, or hot_blocks disabled:
                // demote & re-protect now in Pass A BEFORE Pass B reads it!
                ADD_PROT_RUN(off, n);
                if (p->hot) __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                act[b] = 1;
                continue;
            }
            int is_hot = allow_hot && !gblk && !nohot && p->hot && hot_cnt >= HOT_THR;
            if (is_hot) {
                if (hot_blocks_now >= HOT_MAX_BLOCKS) {
                    // Global hot-block cap (48 blocks = 3 MiB): demote overflow block cleanly.
                    ADD_PROT_RUN(off, n);
                    __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                    act[b] = 1;
                    continue;
                }
                // Never bridge a PROT_READ run across an active PROT_RW hot block!
                FLUSH_PROT_RUN();
                hot_blocks_now++;
                if (probe_rearm) {
                    ADD_PROT_RUN(off, n);
                    __atomic_store_n(&p->hot[b], (uint8_t)(HOT_THR - 1), __ATOMIC_RELEASE);
                }
                act[b] = 2;
            } else {
                if (!nvn_dirty_since_span(p->span, lo + off, n, p->stamp0)) {
                    if (p->hot && hot_cnt) __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                    act[b] = 0;
                    continue;
                }
                if (gblk || nohot) {
                    // GPU-written or texture-overlapping dirty block: always re-protect, never promote to hot.
                    ADD_PROT_RUN(off, n);
                    if (p->hot) __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                    act[b] = 1;
                    continue;
                }
                int can_hot = allow_hot && p->hot && (hot_blocks_now < HOT_MAX_BLOCKS);
                int promote = can_hot && (++hot_cnt >= HOT_THR);
                if (p->hot && allow_hot) __atomic_store_n(&p->hot[b], can_hot ? hot_cnt : (uint8_t)(HOT_THR - 1), __ATOMIC_RELEASE);
                if (!promote) {
                    ADD_PROT_RUN(off, n);
                } else {
                    FLUSH_PROT_RUN();
                    hot_blocks_now++;
                }
                act[b] = promote ? 2 : 1;
            }
        }
        FLUSH_PROT_RUN();
        // ---- Pass B: single atomic snapshot + 1 KB p->baseline diff/update under ONE g_baseline_mu lock ----
        int any_demoted = 0;
        for (size_t b = 0; b < nblk; b++) {
            uint8_t a = act[b];
            if (!a) continue;
            uint64_t off = (uint64_t)b * BLK;
            uint64_t n = size - off < BLK ? size - off : BLK;
            int is_hot = (a == 2);
            if (p->gpu_blk && __atomic_load_n(&p->gpu_blk[b], __ATOMIC_RELAXED))
                __atomic_store_n(&p->gpu_blk[b], 0, __ATOMIC_RELEASE);
            if (use_stage) {
                if (use_diff && p->baseline_complete && p->baseline) {
                    enum { SUB_BLK = 1024 };   // 64 x 1KB chunks per 64KB block -> single 64-bit bitmask
                    uint64_t dmask = 0;
                    uint32_t nsub = (uint32_t)((n + SUB_BLK - 1) / SUB_BLK);
                    pthread_mutex_lock(&g_baseline_mu);
                    memcpy(g_sync_snap, lo + off, (size_t)n);
                    if (memcmp(g_sync_snap, p->baseline + off, (size_t)n) != 0) {
                        for (uint32_t c = 0; c < nsub; c++) {
                            uint64_t coff = (uint64_t)c * SUB_BLK;
                            uint64_t sn = n - coff < SUB_BLK ? n - coff : SUB_BLK;
                            if (memcmp(g_sync_snap + coff, p->baseline + off + coff, (size_t)sn) != 0) {
                                SYNC_MEASURE(3, sn, memcpy(p->baseline + off + coff, g_sync_snap + coff, (size_t)sn));
                                dmask |= 1ull << c;
                            }
                        }
                        if (dmask) pool_bump_cpu_gen(p, off, n);
                    }
                    pthread_mutex_unlock(&g_baseline_mu);
                    if (is_hot && p->hot) {
                        uint8_t hot_cnt = __atomic_load_n(&p->hot[b], __ATOMIC_ACQUIRE);
                        if (dmask) {
                            if (hot_cnt != HOT_THR) __atomic_store_n(&p->hot[b], HOT_THR, __ATOMIC_RELEASE);
                        } else if (++hot_cnt >= HOT_IDLE_LIMIT) {
                            // Block stayed unchanged for 240 consecutive syncs (~4 s at 60 FPS): cool & re-protect in Pass C + re-verify!
                            ADD_PROT_RUN(off, n);
                            __atomic_store_n(&p->hot[b], 0, __ATOMIC_RELEASE);
                            act[b] = 3;
                            any_demoted = 1;
                        } else {
                            __atomic_store_n(&p->hot[b], hot_cnt, __ATOMIC_RELEASE);
                        }
                    }
                    if (!dmask) continue;
                    uint64_t hlo = n, hhi = 0;
                    for (uint64_t m = dmask; m; m &= m - 1) {
                        uint32_t c = (uint32_t)__builtin_ctzll(m);
                        uint64_t coff = (uint64_t)c * SUB_BLK;
                        uint64_t sn = n - coff < SUB_BLK ? n - coff : SUB_BLK;
                        if (hhi == 0) { hlo = coff; hhi = coff + sn; }
                        else if (coff == hhi) { hhi = coff + sn; }
                        else { STAGE_SNAP_BYTES(off + hlo, g_sync_snap + hlo, hhi - hlo); hlo = coff; hhi = coff + sn; }
                    }
                    if (hhi > hlo) STAGE_SNAP_BYTES(off + hlo, g_sync_snap + hlo, hhi - hlo);
                } else {
                    // Full 64 KB staged upload (e.g. DM_VK_SAFE1_DIFF1K=0 bisection mode): still atomic under g_baseline_mu!
                    pthread_mutex_lock(&g_baseline_mu);
                    memcpy(g_sync_snap, lo + off, (size_t)n);
                    if (p->baseline_complete && p->baseline) {
                        SYNC_MEASURE(3, n, memcpy(p->baseline + off, g_sync_snap, (size_t)n));
                    }
                    pool_bump_cpu_gen(p, off, n);
                    pthread_mutex_unlock(&g_baseline_mu);
                    STAGE_SNAP_BYTES(off, g_sync_snap, n);
                }
            } else {
                // Non-overlap direct HOST_CACHED shadow path (GPU already idle via mtl_wait(g_fence)):
                // Pure exact copy (NO baseline diff skip, NO hot blocks).
                pthread_mutex_lock(&g_baseline_mu);
                SYNC_MEASURE(2, n, memcpy(p->shadow + off, lo + off, n)); g_sync_bytes += n;
                if (!(p->flags & 0x2) && p->baseline_complete && p->baseline) SYNC_MEASURE(3, n, memcpy(p->baseline + off, p->shadow + off, n));
                pool_bump_cpu_gen(p, off, n);
                pthread_mutex_unlock(&g_baseline_mu);
                if (run_hi == 0) { run_lo = off; run_hi = off + n; }
                else if (off <= run_hi + FLUSH_GAP) { run_hi = off + n; }
                else { pool_cache_sync(p, 0, run_lo, run_hi); run_lo = off; run_hi = off + n; }
                host_wrote_any = 1;
            }
        }
        // ---- Pass C: flush demoted-block mprotect(PROT_READ) & re-verify atomically under g_baseline_mu ----
        if (any_demoted) {
            FLUSH_PROT_RUN();
            for (size_t b = 0; b < nblk; b++) {
                if (act[b] != 3) continue;
                uint64_t off = (uint64_t)b * BLK;
                uint64_t n = size - off < BLK ? size - off : BLK;
                enum { SUB_BLK = 1024 };
                uint64_t dmask = 0;
                uint32_t nsub = (uint32_t)((n + SUB_BLK - 1) / SUB_BLK);
                pthread_mutex_lock(&g_baseline_mu);
                memcpy(g_sync_snap, lo + off, (size_t)n);
                if (memcmp(g_sync_snap, p->baseline + off, (size_t)n) != 0) {
                    for (uint32_t c = 0; c < nsub; c++) {
                        uint64_t coff = (uint64_t)c * SUB_BLK;
                        uint64_t sn = n - coff < SUB_BLK ? n - coff : SUB_BLK;
                        if (memcmp(g_sync_snap + coff, p->baseline + off + coff, (size_t)sn) != 0) {
                            SYNC_MEASURE(3, sn, memcpy(p->baseline + off + coff, g_sync_snap + coff, (size_t)sn));
                            dmask |= 1ull << c;
                        }
                    }
                    if (dmask) pool_bump_cpu_gen(p, off, n);
                }
                pthread_mutex_unlock(&g_baseline_mu);
                if (dmask) {
                    if (p->hot) __atomic_store_n(&p->hot[b], 1, __ATOMIC_RELEASE);
                    uint64_t hlo = n, hhi = 0;
                    for (uint64_t m = dmask; m; m &= m - 1) {
                        uint32_t c = (uint32_t)__builtin_ctzll(m);
                        uint64_t coff = (uint64_t)c * SUB_BLK;
                        uint64_t sn = n - coff < SUB_BLK ? n - coff : SUB_BLK;
                        if (hhi == 0) { hlo = coff; hhi = coff + sn; }
                        else if (coff == hhi) { hhi = coff + sn; }
                        else { STAGE_SNAP_BYTES(off + hlo, g_sync_snap + hlo, hhi - hlo); hlo = coff; hhi = coff + sn; }
                    }
                    if (hhi > hlo) STAGE_SNAP_BYTES(off + hlo, g_sync_snap + hlo, hhi - hlo);
                }
            }
        }
        FLUSH_PROT_RUN();
        if (ncb) FLUSH_COPY_BATCH(vk_upload_cmd());
        if (act != act_stk) free(act);
#undef FLUSH_PROT_RUN
#undef ADD_PROT_RUN
#undef FLUSH_COPY_BATCH
#undef STAGE_SNAP_BYTES
        if (!use_stage && run_hi > run_lo) pool_cache_sync(p, 0, run_lo, run_hi);
        if (diag) { ns[0] += mem_diag_ns() - scan_start - (ns[1] + ns[2] + ns[3] - work_start); amount[0] += size; }
        if (!p->baseline_complete && p->baseline) {
            if (diag) { int reason = p->flags & 0x2 ? 2 : 3; reason_count[reason]++; reason_bytes[reason] += size; }
            SYNC_MEASURE(3, size, memcpy(p->baseline, p->shadow, size));
            p->baseline_complete = 1;
            p->baseline_fresh = 0;
        }
        p->stamp0 = p->stamp; p->stamp = st;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (host_wrote_any) {
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
            VK_ACCESS_HOST_WRITE_BIT,
            POOL_CONSUMER_ACCESS | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(vk_upload_cmd(), VK_PIPELINE_STAGE_HOST_BIT,
            pool_consumer_stages() | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    }
    if (staged_any) {
        // Order s->up pool copies before any subsequent texture pkg_upload/lin_copy in the same s->up
        // (Transfer->Transfer only: zero graphics/fragment stall; final Transfer->Consumers barrier runs at s->up end).
        VkMemoryBarrier tt = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        vkCmdPipelineBarrier(vk_upload_cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &tt, 0, NULL, 0, NULL);
    }
    uint64_t sync_tot = st_now_ns() - sync_t0;
    vk_sub_timing_add(2, rearm_ns);
    vk_sub_timing_add(0, sync_tot > rearm_ns ? sync_tot - rearm_ns : 0);
    if (diag) for (unsigned k = 0; k < 4; k++) mem_diag_note(6 + k, mem_diag_ns() - ns[k], amount[k]);
    if (diag) mem_diag_baselines(reason_count, reason_bytes);
#undef SYNC_MEASURE
    static uint64_t n, last, last_rb_seen, last_rb_nar, last_ovl, last_soff, last_srb, last_sinit, last_sconf, last_rbstg, last_defsaved;
    __atomic_store_n(&g_hot16_last, hot_blocks_now, __ATOMIC_RELAXED);
    if (++n % 300 == 0) {
        uint64_t rbs = __atomic_load_n(&g_rb_seen, __ATOMIC_RELAXED);
        uint64_t rbn = __atomic_load_n(&g_rb_narrowed, __ATOMIC_RELAXED);
        uint64_t ovl = __atomic_load_n(&g_ovl_used, __ATOMIC_RELAXED);
        uint64_t soff = __atomic_load_n(&g_ovl_skip_off, __ATOMIC_RELAXED);
        uint64_t srb = __atomic_load_n(&g_ovl_skip_rb, __ATOMIC_RELAXED);
        uint64_t sinit = __atomic_load_n(&g_ovl_skip_init, __ATOMIC_RELAXED);
        uint64_t sconf = __atomic_load_n(&g_ovl_skip_conflict, __ATOMIC_RELAXED);
        uint64_t rbstg = __atomic_load_n(&g_rb_staged_cnt, __ATOMIC_RELAXED);
        uint64_t dsaved = __atomic_load_n(&g_defwait_saved_ns, __ATOMIC_RELAXED);
        VKLOG("copied pools (last 300, safe_sync=%d def_wait=%d overlap=%llu/300 [skip: off=%llu rb=%llu init=%llu conf=%llu, rb_staged=%llu], deferred_wait_saved_ms=%.2f): %.1f MB synced (%.2f MB/sub), %llu full copies, %llu hot %s blocks (%.1f MB), readback narrowed %llu/%llu",
              safe_sync, vk_deferred_wait(),
              (unsigned long long)(ovl - last_ovl), (unsigned long long)(soff - last_soff), (unsigned long long)(srb - last_srb), (unsigned long long)(sinit - last_sinit), (unsigned long long)(sconf - last_sconf), (unsigned long long)(rbstg - last_rbstg),
              (dsaved - last_defsaved) / 3e8,
              (g_sync_bytes - last) / 1048576.0, (g_sync_bytes - last) / 300.0 / 1048576.0,
              (unsigned long long)g_sync_full, (unsigned long long)hot_blocks_now, serial_safe ? "16KB" : "64KB",
              hot_blocks_now * (serial_safe ? 16.0 : 64.0) / 1024.0,
              (unsigned long long)(rbn - last_rb_nar), (unsigned long long)(rbs - last_rb_seen));
        last = g_sync_bytes; last_rb_seen = rbs; last_rb_nar = rbn;
        last_ovl = ovl; last_soff = soff; last_srb = srb; last_sinit = sinit; last_sconf = sconf; last_rbstg = rbstg; last_defsaved = dsaved;
    }
}
// Copied pools after a submission: merge GPU changes against their baseline, preserving independent CPU writes made after the sync. Never-synced pools are left alone.
// A CPU-uncached pool (vk_pool_set_flags) takes back only the ranges the submission's GPU writes covered (sub_gpu_wrote):
// copying 860 MB back after every submission held Unicorn Overlord to a few frames a second
struct vksub; static void pools_copy_back(const struct vksub *s);

// ---------------- submission slots ----------------
#define NSLOT 6
#define MAIN_CBS 32   // (allocated as used: DM_VK_PASS_SPLIT cuts inside passes as well)
// GPU time: a timestamp at the start and end of each submission's main command buffer (one queue: their sum is the time the GPU was busy)
static VkQueryPool g_tsq; uint64_t g_gpu_ns;
// Timestamp counters (ReportCounter TIMESTAMP): each writes a GPU timestamp where it is in the command stream (CQ per slot), and at completion it gets the guest
// clock now less how long before the submission's end the GPU passed it. They used to all get the completion time of their submission: a game timing its GPU
// work between two counters read 0 when both were in one submission and the gap between completions (our CPU replay included) when not. Xenoblade Chronicles 3
// sizes its frame by that time (dynamic resolution): its main pass went between 960x544 and 776x440 every few frames while the camera turned; with GPU times
// the size holds steady
enum { CQ = 64 };
static VkQueryPool g_cq;
#define SCRATCH_BLOCK (8u << 20)
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize size, used; int noncoherent; } scratch_blk;
typedef struct { int kind; void *a; void *b; } dfree_t;   // 0 buffer+memory, 1 image+memory, 2 view
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; int q; } post_op;   // q: a timestamp counter's query in g_cq, or -1
typedef struct { uint64_t pool_serial, lo, hi; uint8_t *stg, *base_snap; uint32_t cpu_gen_sum; } rb_stage_t;
struct vksub {
    VkCommandPool pool; VkCommandBuffer up, main; int up_used;
    VkCommandBuffer mains[MAIN_CBS]; int nmain_alloc, nmain_used, cb_err;
    VkCommandBuffer ups[MAIN_CBS]; int nup_alloc, nup_used; int nmain_sent;   // upload command buffers in order (one per part sent early), main buffers already submitted   // the submission's main command buffers in order (vk_cmd_split); main = the one being recorded
    uint64_t fence;   // the fence this slot last committed with (0 = never)
    int ncq;          // timestamp counter queries used in this submission (g_cq)
    scratch_blk *blk; int nblk, capblk;
    VkDescriptorPool *dp; int ndp, capdp, dp_cur;
    uint32_t dp_used[9], dp_sets, dp_total_sets, dp_pool_gen; int dp_counted; int no_ts;   // what pool dp_cur has given out (vk_alloc_set)
    uint64_t dp_tex_gen, dp_samp_gen, dp_epoch;   // persistent per-slot descriptor pool cache invalidation gens
    dfree_t *df; int ndf, capdf;
    post_op *post; int npost, cappost;
    sub_gw_t gw[256]; int ngw, readback_ranges, needs_cpu_rb;   // GPU writes into pool memory in this submission (sub_gpu_wrote), for pools_copy_back
    rb_stage_t rb[256]; int nrb, rb_staged;   // slot-owned HOST_CACHED readback copies recorded at end of s->main
    uint8_t *rb_snap; size_t rb_snap_cap, rb_snap_used;   // reusable per-slot CPU heap arena for base_snap (zero per-frame malloc/free churn)
    VkBuffer rb_buf; VkDeviceMemory rb_mem; uint8_t *rb_ptr; VkDeviceSize rb_cap, rb_used; int rb_noncoherent;   // dedicated HOST_CACHED readback staging buffer (~20x faster than WC vk_scratch!)
};
enum { GW_SUB = 256, RB_STAGE_MAX_BYTES = 8u << 20 };
static vksub_t g_slot[NSLOT]; static int g_cur = -1; uint64_t g_cmd_gen;   // g_cmd_gen: main command buffers begun (gfx_vk_exec.c bind cache)
static pthread_mutex_t g_df_mu = PTHREAD_MUTEX_INITIALIZER;   // the slots' deferred-free lists (df, ndf, capdf): df_add says why
static uint64_t pool_merge_back_src(vkpool_t *p, uint64_t lo, uint64_t hi, const uint8_t *staged_src, const uint8_t *base_snap, int has_cpu_gen, uint32_t cpu_gen_sum) {
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0;
    uint64_t bytes = 0;
    uint8_t after[4096];
    // Safe zero-clobber GPU->CPU merge:
    // NEVER overwrite a guest RAM byte (dst[i]) that the guest CPU has ALREADY modified since the last sync
    // (dst[i] != base[i] / dst[i] != ref[i]), while STILL updating p->baseline[i] = src[i] to match GPU p->buf
    // so the next vk_pools_sync_() sees dst[i] != baseline[i] and uploads the newer CPU value cleanly!
    for (uint64_t off = lo - p->lo, end = hi - p->lo, soff = 0; off < end; ) {
        size_t n = end - off < sizeof after ? (size_t)(end - off) : sizeof after;
        const uint8_t *src;
        if (staged_src) {
            src = staged_src + soff;   // already in HOST_CACHED CPU-readable s->rb_ptr!
        } else if (p->cached) {
            src = p->shadow + off;     // already HOST_CACHED (post-invalidate): avoid extra 4 KB stack copy!
        } else {
            memcpy(after, p->shadow + off, n);
            src = after;
        }
        uint8_t *base = p->baseline + off, *dst = (uint8_t *)(uintptr_t)(p->lo + off);
        if (base_snap) {
            const uint8_t *ref = base_snap + soff;
            pthread_mutex_lock(&g_baseline_mu);
            int cpu_unchanged = has_cpu_gen && (pool_sum_cpu_gen(p, lo, hi) == cpu_gen_sum);
            if (cpu_unchanged) {
                if (memcmp(base, src, n)) {
                    if (memcmp(dst, base, n) == 0) {
                        // Fast path: guest CPU has not touched this 4 KB chunk at all since last sync.
                        dm_guest_write_prepare(dst, n);
                        for (size_t i = 0; i < n; ) {
                            if (base[i] == src[i]) { i++; continue; }
                            size_t a = i++;
                            while (i < n && base[i] != src[i]) i++;
                            memcpy(dst + a, src + a, i - a);
                            memcpy(base + a, src + a, i - a);
                            bytes += i - a;
                        }
                    } else {
                        // Live CPU writes exist in dst: only merge bytes untouched by CPU (dst[i] == base[i]);
                        // always sync base[i] = src[i] to match GPU p->buf so next vk_pools_sync_ uploads newer CPU bytes!
                        int prepped = 0;
                        for (size_t i = 0; i < n; i++) {
                            if (base[i] == src[i]) continue;
                            if (dst[i] == base[i]) {
                                if (!prepped) { dm_guest_write_prepare(dst, n); prepped = 1; }
                                dst[i] = src[i];
                                bytes++;
                            }
                            base[i] = src[i];
                        }
                    }
                }
            } else if (memcmp(ref, src, n)) {
                int prepped = 0;
                for (size_t i = 0; i < n; i++) {
                    if (ref[i] == src[i] || base[i] != ref[i]) continue;
                    if (dst[i] == ref[i]) {
                        if (!prepped) { dm_guest_write_prepare(dst, n); prepped = 1; }
                        dst[i] = src[i];
                        bytes++;
                    }
                    base[i] = src[i];
                }
            }
            pthread_mutex_unlock(&g_baseline_mu);
        } else {
            pthread_mutex_lock(&g_baseline_mu);
            if (memcmp(base, src, n)) {
                if (memcmp(dst, base, n) == 0) {
                    dm_guest_write_prepare(dst, n);
                    for (size_t i = 0; i < n; ) {
                        if (base[i] == src[i]) { i++; continue; }
                        size_t a = i++;
                        while (i < n && base[i] != src[i]) i++;
                        memcpy(dst + a, src + a, i - a);
                        memcpy(base + a, src + a, i - a);
                        bytes += i - a;
                    }
                } else {
                    int prepped = 0;
                    for (size_t i = 0; i < n; i++) {
                        if (base[i] == src[i]) continue;
                        if (dst[i] == base[i]) {
                            if (!prepped) { dm_guest_write_prepare(dst, n); prepped = 1; }
                            dst[i] = src[i];
                            bytes++;
                        }
                        base[i] = src[i];
                    }
                }
            }
            pthread_mutex_unlock(&g_baseline_mu);
        }
        off += n; soff += n;
    }
    if (diag) mem_diag_note(5, t, hi - lo);
    return bytes;
}
static uint64_t pool_merge_back(vkpool_t *p, uint64_t lo, uint64_t hi) { return pool_merge_back_src(p, lo, hi, NULL, NULL, 0, 0); }
static void pools_copy_back(const struct vksub *s) {
    uint64_t rb_t0 = st_now_ns();
    int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0, bytes = 0;
    uint64_t invalidate_ns = 0, invalidate_calls = 0, invalidate_bytes = 0;
    __atomic_fetch_add(&g_rb_seen, 1, __ATOMIC_RELAXED);
    if (s->readback_ranges) __atomic_fetch_add(&g_rb_narrowed, 1, __ATOMIC_RELAXED);
    if (s->readback_ranges && (!s->needs_cpu_rb || s->ngw <= 0)) return;
    pthread_rwlock_rdlock(&g_pool_mu);
    if (s->rb_staged) {
        // One batched cache invalidate on slot's HOST_CACHED readback buffer (if non-coherent), then cached CPU merge (~0.02 ms!).
        if (s->rb_noncoherent && s->rb_mem && s->rb_used > 0) {
            uint64_t atom = V.props.limits.nonCoherentAtomSize ? V.props.limits.nonCoherentAtomSize : 1;
            VkDeviceSize sz = ((s->rb_used + atom - 1) / atom) * atom;
            if (sz > s->rb_cap) sz = VK_WHOLE_SIZE;
            VkMappedMemoryRange rr = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, s->rb_mem, 0, sz};
            VKCHK(vkInvalidateMappedMemoryRanges(V.dev, 1, &rr));
        }
        for (int k = 0; k < s->nrb; k++) {
            const rb_stage_t *rb = &s->rb[k];
            vkpool_t *p = NULL;
            for (int i = 0; i < g_npool; i++) if (g_pool[i].serial == rb->pool_serial) { p = &g_pool[i]; break; }
            if (p && p->shadow && p->synced && p->baseline && rb->lo >= p->lo && rb->hi <= p->hi && rb->lo < rb->hi) {
                p->baseline_complete = 1;
                bytes += pool_merge_back_src(p, rb->lo, rb->hi, rb->stg, rb->base_snap, p->cpu_blk_gen != NULL, rb->cpu_gen_sum);
            }
        }
        pthread_rwlock_unlock(&g_pool_mu);
        vk_sub_timing_add(1, st_now_ns() - rb_t0);
        if (diag) { mem_diag_note(2, t, bytes); mem_diag_copyback(mem_diag_ns() - t, 0, 0, 0); }
        return;
    }
    for (int i = 0; i < g_npool; i++) { vkpool_t *p = &g_pool[i]; if (!p->shadow || !p->synced || !p->baseline || (p->flags & 0x1)) continue;
        // Fast-skip pools that have ZERO overlap with any s->gw[k] range when narrowed (or CPU_UNCACHED)!
        if (s->readback_ranges || (p->flags & 0x2)) {
            int any_ovl = 0;
            for (int k = 0; k < s->ngw; k++) {
                if (s->gw[k].lo < p->hi && s->gw[k].hi > p->lo) { any_ovl = 1; break; }
            }
            if (!any_ovl) continue;
            uint64_t start = diag && p->noncoherent ? mem_diag_ns() : 0;
            uint64_t icalls = 0;
            uint64_t ibytes = pool_cache_invalidate_ranges(p, s->gw, s->ngw, &icalls);
            if (ibytes && diag && p->noncoherent) {
                uint64_t dt = mem_diag_ns() - start;
                invalidate_ns += dt; invalidate_calls += icalls; invalidate_bytes += ibytes;
                mem_diag_note(4, start, ibytes);
            }
        } else {
            uint64_t start = diag && p->noncoherent ? mem_diag_ns() : 0;
            pool_cache_sync(p, 1, 0, p->hi - p->lo);
            if (diag && p->noncoherent) { invalidate_ns += mem_diag_ns() - start; invalidate_calls++; invalidate_bytes += p->hi - p->lo; }
        }
        if (!(p->flags & 0x2) && !s->readback_ranges) { bytes += pool_merge_back(p, p->lo, p->hi); p->baseline_complete = 1; continue; }
        // The baseline stays complete after a merge of the tracked ranges, for NVN CPU_UNCACHED pools too after a submission with an unknown writer: those pools
        // trust the tracked ranges, as before the baseline (an untracked GPU write stays in the GPU copy only and is merged with a later tracked range over it).
        // Taking a full snapshot at the next sync instead copied the whole pool before every submission: Unicorn Overlord's 860 MB CPU_UNCACHED pool, as before
        p->baseline_complete = 1;
        for (int k = 0; k < s->ngw; k++) { uint64_t lo = s->gw[k].lo > p->lo ? s->gw[k].lo : p->lo, hi = s->gw[k].hi < p->hi ? s->gw[k].hi : p->hi;
            if (lo < hi) bytes += pool_merge_back(p, lo, hi); } }
    pthread_rwlock_unlock(&g_pool_mu);
    vk_sub_timing_add(1, st_now_ns() - rb_t0);
    if (diag) {
        uint64_t total = mem_diag_ns() - t;
        mem_diag_note(2, t, bytes);
        mem_diag_copyback(total, invalidate_ns, invalidate_calls, invalidate_bytes);
        static uint64_t seen, narrowed; if (s->readback_ranges) narrowed++;
        if (++seen <= 3 || seen % 300 == 0) VKLOG("[memdiag] readback coverage: %llu range submissions / %llu total (others retain full cached-pool merges)", (unsigned long long)narrowed, (unsigned long long)seen);
    }
}
int vk_readback_ranges(void) { return g_cur >= 0 && g_slot[g_cur].readback_ranges; }
void vk_readback_unknown(void) { if (g_cur >= 0) g_slot[g_cur].readback_ranges = 0; }
static VkCommandBuffer g_slot_main_cb(void) { return g_slot[g_cur].main; }
// DM_VK_PASS_TIMES=1: a timestamp before and after every render pass (the replay's per-frame line then lists the passes that cost the most GPU time)
enum { PASSQ = 512 };   // passes (and dispatches) timed per submission
static VkQueryPool g_pq; static int g_pq_on = -1;
typedef struct { uint32_t w, h, fmt, draws, fidx; } pass_meta;   // fidx: the pass's place in its frame (submissions are cut every DM_VK_CB_DRAWS draws)
enum { FPASSQ = 512 }; static int g_fpass;   // passes of the frame so far
void vk_pass_frame(void) { g_fpass = 0; }   // a present: the next pass is the frame's first
static pass_meta g_pmeta[NSLOT][PASSQ]; static int g_npass[NSLOT];
static double g_pacc[FPASSQ]; static uint32_t g_pcnt[FPASSQ]; static pass_meta g_plast[FPASSQ]; static int g_pmax;   // by the pass's place in its frame (the completion thread adds)
static int pass_times_on(void) { if (g_pq_on < 0) { const char *e = getenv("DM_VK_PASS_TIMES"); g_pq_on = e && atoi(e); } return g_pq_on && g_pq; }
void vk_pass_ts(int end, uint32_t w, uint32_t h, uint32_t fmt, uint32_t draws) {
    if (!pass_times_on() || g_cur < 0) return; int *n = &g_npass[g_cur]; if (*n >= PASSQ) return;
    vkCmdWriteTimestamp(g_slot_main_cb(), end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_pq, (uint32_t)(g_cur * PASSQ * 2 + *n * 2 + end));
    if (end) { g_pmeta[g_cur][*n] = (pass_meta){w, h, fmt, draws, (uint32_t)(g_fpass < FPASSQ - 1 ? g_fpass : FPASSQ - 1)}; (*n)++; g_fpass++; }
}
void vk_pass_report(void) {   // the most expensive passes since the last report, then the counters start again
    if (!pass_times_on()) return;
    static int idx[FPASSQ]; int m = g_pmax; for (int i = 0; i < m; i++) idx[i] = i;
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (g_pcnt[idx[j]] && (!g_pcnt[idx[i]] || g_pacc[idx[j]] / g_pcnt[idx[j]] > g_pacc[idx[i]] / g_pcnt[idx[i]])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    double tot = 0; for (int i = 0; i < m; i++) if (g_pcnt[i]) tot += g_pacc[i] / g_pcnt[i];
    char b[3000]; int o = snprintf(b, sizeof b, "passes by GPU time (%d passes, %.2f ms in passes per frame):", m, tot);
    for (int i = 0; i < m && i < 20 && o < (int)sizeof b - 80; i++) { int k = idx[i]; if (!g_pcnt[k]) continue;
        if (g_plast[k].fmt == PT_DISPATCH) o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (dispatch %ux%ux%u);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].draws);
        else o += snprintf(b + o, sizeof b - (size_t)o, " #%d %.2f ms (%ux%u fmt %u, %u draws over %u px wide);", k, g_pacc[k] / g_pcnt[k], g_plast[k].w, g_plast[k].h, g_plast[k].fmt, g_plast[k].draws & 0xffff, g_plast[k].draws >> 16); }
    { extern uint64_t g_nvn_discards[2]; static uint64_t pd0, pd1; o += snprintf(b + o, sizeof b - (size_t)o, " discards since: colour %llu, depth %llu", (unsigned long long)(g_nvn_discards[0] - pd0), (unsigned long long)(g_nvn_discards[1] - pd1)); pd0 = g_nvn_discards[0]; pd1 = g_nvn_discards[1]; }
    VKLOG("%s", b);
    memset(g_pacc, 0, sizeof g_pacc); memset(g_pcnt, 0, sizeof g_pcnt); g_pmax = 0;
}
// one submission at a time: the game's (hle_nvn.c holds its own lock around them) and the backend's own (texel readback, present)
static pthread_mutex_t g_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_fence;   // fence of the last commit
static VkSemaphore g_timeline;   // submission f signals the value f (V.has_timeline), else g_fring[f % NFRING]
#define NFRING 16   // more than the submissions in flight (NSLOT: mtl_sub_begin's slot_recycle waits for the slot's last one)
static VkFence g_fring[NFRING];
static VkSemaphore g_chain_sem[NFRING];   // Vulkan 1.1 GPU-to-GPU submission chain (when !V.has_timeline)
static VkSemaphore g_chain_pending;
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_done;
static void slots_init(void) {
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (V.has_timeline) VKCHK(vkCreateSemaphore(V.dev, &si, NULL, &g_timeline));
    else for (int i = 0; i < NFRING; i++) {
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHK(vkCreateFence(V.dev, &fi, NULL, &g_fring[i]));
        VkSemaphoreCreateInfo bsi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHK(vkCreateSemaphore(V.dev, &bsi, NULL, &g_chain_sem[i]));
    }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * NSLOT};
    if (V.props.limits.timestampComputeAndGraphics && vkCreateQueryPool(V.dev, &qi, NULL, &g_tsq)) g_tsq = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo cqi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * CQ};
    if (!g_tsq || vkCreateQueryPool(V.dev, &cqi, NULL, &g_cq)) g_cq = VK_NULL_HANDLE;
    { const char *e = getenv("DM_VK_PASS_TIMES"); if (e && atoi(e)) { VkQueryPoolCreateInfo pq = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, NSLOT * PASSQ * 2}; if (vkCreateQueryPool(V.dev, &pq, NULL, &g_pq)) g_pq = VK_NULL_HANDLE; } }
    for (int i = 0; i < NSLOT; i++) {
        // not TRANSIENT, and reset by command buffer (see slot_recycle)
        VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, V.qf};
        VKCHK(vkCreateCommandPool(V.dev, &ci, NULL, &g_slot[i].pool));
        VkCommandBuffer cb[2]; VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_slot[i].pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
        VKCHK(vkAllocateCommandBuffers(V.dev, &ai, cb)); g_slot[i].up = g_slot[i].ups[0] = cb[0]; g_slot[i].main = g_slot[i].mains[0] = cb[1]; g_slot[i].nmain_alloc = 1; g_slot[i].nup_alloc = 1;
    }
}
uint64_t vk_fence_done(void) { return __atomic_load_n(&g_done, __ATOMIC_ACQUIRE); }   // written under g_done_mu (for the waiters) with a release store
VkSemaphore vk_timeline_semaphore(void) { return V.has_timeline ? g_timeline : VK_NULL_HANDLE; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
// Without timeline semaphores only the completion thread waits on the ring's fences, in submission order (a fence tells nothing of the submissions before
// it); everyone else waits for the completion thread to pass the fence (g_done), so a fence is never waited on while the commit resets it for reuse
static void wait_fence(uint64_t f) {
    if (!f) return;
    if (!V.has_timeline) {
        if (!mtl_wait(f, 3000000000ull)) {
            VKLOG("[RED] [vk] FENCE TIMEOUT / DEVICE_LOST (wait=%llu done=%llu vendor=%#x dev=%s)",
                  (unsigned long long)f, (unsigned long long)vk_fence_done(), V.props.vendorID, V.props.deviceName);
            vk_record_device_lost("wait_fence_timeout", VK_ERROR_DEVICE_LOST);
            mtl_wait(f, 10000000000ull);
        } else {
            __atomic_store_n(&V.dev_lost_streak, 0, __ATOMIC_RELAXED);
        }
        return;
    }
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_timeline, &f};
    VkResult r = vkWaitSemaphores(V.dev, &wi, 3000000000ull);
    if (r == VK_TIMEOUT) {
        VKLOG("[RED] [vk] FENCE TIMEOUT / DEVICE_LOST (timeline=%llu done=%llu vendor=%#x dev=%s)",
              (unsigned long long)f, (unsigned long long)vk_fence_done(), V.props.vendorID, V.props.deviceName);
        vk_record_device_lost("wait_semaphore_timeout", VK_ERROR_DEVICE_LOST);
        r = vkWaitSemaphores(V.dev, &wi, 10000000000ull);
    }
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) vk_record_device_lost("wait_fence", r);
        die("[vk] GPU timeline wait did not complete: %d", r);
    }
    __atomic_store_n(&V.dev_lost_streak, 0, __ATOMIC_RELAXED);
}
// The slot's command buffers are reset on the completion thread once its submission is done (completion_main), not when the replay begins them again: an
// implicit reset at vkBeginCommandBuffer took 7% of Xenoblade Chronicles 3's replay thread at a submission's start and more at every part (its battles
// replay ~3,000 draws a frame on a thread that is the frame's limit). The slot is then taken only after the completion thread has passed it (mtl_wait, g_done)
// rather than as soon as the GPU has. DM_VK_X_NOPRERESET=1 (start-up): reset as they are begun
static int g_prereset = -1;
static void df_destroy_one(dfree_t d);
static void df_drain_pending(void);
static uint64_t g_dp_epoch_counter = 1;
static void slot_recycle(vksub_t *s) {   // the GPU has passed this slot's last submission: free what it held, rewind its allocators
    if (g_prereset < 0) { const char *e = getenv("DM_VK_X_NOPRERESET"); g_prereset = !(e && atoi(e)); }
    uint64_t tw = st_now_ns();
    if (g_prereset) mtl_wait(s->fence, ~0ull); else wait_fence(s->fence);
    vk_sub_timing_add(5, st_now_ns() - tw);
    pthread_mutex_lock(&g_df_mu); dfree_t *df = s->df; int ndf = s->ndf; s->df = NULL; s->ndf = s->capdf = 0; pthread_mutex_unlock(&g_df_mu);   // the list taken whole: a game thread may add to it meanwhile
    for (int i = 0; i < ndf; i++) df_destroy_one(df[i]);
    free(df);
    df_drain_pending();
    vk_pool_retire_drain();
    s->rb_snap_used = 0;
    s->rb_used = 0;
    s->nrb = 0; s->rb_staged = 0;
    for (int i = 0; i < s->nblk; i++) s->blk[i].used = 0;
    // Persistent per-slot descriptor pools: keep cached VkDescriptorSets across frames while textures, samplers,
    // and memory pools are unchanged and total sets stay below 3072 (reset cleanly on any invalidation).
    extern uint64_t g_tex_gen, g_samp_gen;
    uint64_t tgen = __atomic_load_n(&g_tex_gen, __ATOMIC_ACQUIRE);
    uint64_t sgen = __atomic_load_n(&g_samp_gen, __ATOMIC_ACQUIRE);
    uint32_t pgen = vk_pool_gen();
    int keep_dp = s->ndp > 0 && s->dp_epoch != 0 && s->dp_tex_gen == tgen && s->dp_samp_gen == sgen &&
                  s->dp_pool_gen == pgen && s->dp_total_sets < 3072u;
    if (!keep_dp) {
        for (int i = 0; i < s->ndp; i++) vkResetDescriptorPool(V.dev, s->dp[i], 0);
        s->dp_cur = 0; s->dp_counted = -1; s->dp_total_sets = 0;
        s->dp_tex_gen = tgen; s->dp_samp_gen = sgen; s->dp_pool_gen = pgen;
        s->dp_epoch = ++g_dp_epoch_counter;
    }
    // The pool is not reset: each command buffer is reset as it is begun again (the pool allows it). A reset of the transient pool made Adreno's driver free the
    // command memory and map it again as the next submission grew (ioctl, mmap and munmap): in Xenoblade Chronicles 3's battles the submission's start took 21%
    // of the replay thread, now 9%, and frames went from about 42 to 38 ms
}
static vksub_t *cur(void) { if (g_cur < 0) die("[vk] no open submission"); return &g_slot[g_cur]; }
int vk_sub_open(void) { return g_cur >= 0; }
int vk_sub_slot_index(void) { return g_cur; }
uint64_t vk_sub_dp_epoch(void) { return g_cur >= 0 ? g_slot[g_cur].dp_epoch : 0; }
// A long submission is recorded into several command buffers, submitted together in order: Adreno's driver refuses a command buffer past some size at
// vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY), which NEO's handheld frames reached at about the 712th draw. The caller has ended the render pass.
static void vk_cmd_split_(void);
static void pools_readback_barrier(VkCommandBuffer cb);
void vk_cmd_split(void) { extern int g_prof_on; extern uint64_t g_prof_ns[8]; struct timespec a, b; if (g_prof_on) clock_gettime(CLOCK_MONOTONIC, &a);
    vk_cmd_split_(); if (g_prof_on) { clock_gettime(CLOCK_MONOTONIC, &b); g_prof_ns[6] += (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); } }
static void vk_cmd_split_(void) {
    vksub_t *s = cur(); if (s->nmain_used == MAIN_CBS || s->nup_used == MAIN_CBS) return;
    pools_readback_barrier(s->main);
    if (vkEndCommandBuffer(s->main) < 0) { s->cb_err = 1; static int n; if (n++ < 3) VKLOG("[RED] a part of a long submission was refused at its end (part %d)", s->nmain_used); }
    // what is recorded so far goes to the GPU now, as a submission of its own (with its uploads first): Adreno refuses a single submission past about
    // 700 draws (NEO's handheld frames: VK_ERROR_OUT_OF_HOST_MEMORY at vkEndCommandBuffer whatever the draws were, and in one command buffer or several)
    const char *se = getenv("DM_VK_SUBMIT_SPLIT"); int send = (vk_strict_barriers() || vk_pool_overlap() || vk_pool_safe_sync() == 1) ? 0 : !(se && !atoi(se));   // disabled when safe_sync=1, pool_overlap=1, or strict_barriers=1 so submissions stay atomic behind hardware chain wait
    if (send && !s->cb_err) {
        VkCommandBuffer cbs[2]; uint32_t n = 0;
        if (s->up_used) { vk_post_transfer_barrier(s->up); if (vkEndCommandBuffer(s->up) < 0) s->cb_err = 1; cbs[n++] = s->up; }
        cbs[n++] = s->main;
        if (!s->cb_err) {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, n, cbs, 0, NULL};
            uint64_t ts0 = st_now_ns();
            pthread_mutex_lock(&g_vk_queue_mu); VkResult r = vkQueueSubmit(V.q, 1, &si, VK_NULL_HANDLE); pthread_mutex_unlock(&g_vk_queue_mu);
            vk_sub_timing_add(4, st_now_ns() - ts0);
            if (r) { if (r == VK_ERROR_DEVICE_LOST) vk_record_device_lost("vk_cmd_split", r); void vk_crumbs_report(void); vk_crumbs_report(); die("[vk] vkQueueSubmit (part): %d (device lost?)", r); }
            { static int n; if (n++ < 3) VKLOG("a long submission: part %d sent early", s->nmain_used); }
            s->nmain_sent = s->nmain_used;
            if (s->up_used) {   // the next part records its uploads into a fresh upload buffer
                if (s->nup_used == s->nup_alloc) { VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
                    VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &s->ups[s->nup_alloc])); s->nup_alloc++; }
                s->up = s->ups[s->nup_used++]; s->up_used = 0;
            }
        }
    }
    if (s->nmain_used == s->nmain_alloc) { VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &s->mains[s->nmain_alloc])); s->nmain_alloc++; }
    s->main = s->mains[s->nmain_used++];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;
}   // outside a submission nothing may be recorded (present, a pool finalized between frames)
VkCommandBuffer vk_cmd(void) { return cur()->main; }
int vk_cmd_parts_left(void) { vksub_t *s = cur(); return MAIN_CBS - (s->nmain_used > s->nup_used ? s->nmain_used : s->nup_used); }   // parts vk_cmd_split can still begin (gfx_vk_exec.c pass splits)
uint64_t g_up_gen;   // upload command buffers begun (gfx_vk_tex.c lin_copy: once per upload buffer)
VkCommandBuffer vk_upload_cmd(void) {
    vksub_t *s = cur();
    if (!s->up_used) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VKCHK(vkBeginCommandBuffer(s->up, &bi));
        s->up_used = 1; g_up_gen++;
        // Always order s->up uploads (buffer copies, pkg_upload, lin_copy, vk_tex_prepare) after Frame N-1's GPU reads/writes.
        vk_pre_transfer_barrier(s->up);
    }
    return s->up;
}
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off) {
    vksub_t *s = cur(); if (!align) align = 16;
    for (int i = 0; i < s->nblk; i++) {
        scratch_blk *b = &s->blk[i]; VkDeviceSize o = (b->used + align - 1) & ~(align - 1);
        if (o + n <= b->size) { b->used = o + n; *buf = b->b; *off = o; return b->p + o; }
    }
    if (s->nblk == s->capblk) { s->capblk = s->capblk ? 2 * s->capblk : 8; s->blk = realloc(s->blk, s->capblk * sizeof *s->blk); }
    scratch_blk *b = &s->blk[s->nblk++]; memset(b, 0, sizeof *b);
    static VkDeviceSize blk = 0; if (!blk) { const char *e = getenv("DM_VK_SCRATCH_MB"); blk = e && atoi(e) > 0 ? (VkDeviceSize)atoi(e) << 20 : SCRATCH_BLOCK; }
    b->size = n > blk ? (n + 0xffff) & ~0xffffull : blk;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, b->size, POOL_USAGE, VK_SHARING_MODE_EXCLUSIVE};
    VKCHK(vkCreateBuffer(V.dev, &bi, NULL, &b->b));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b->b, &req);
    uint32_t mtype = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (mtype == UINT32_MAX) mtype = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, mtype};
    VKCHK(vkAllocateMemory(V.dev, &ai, NULL, &b->m)); VKCHK(vkBindBufferMemory(V.dev, b->b, b->m, 0));
    void *m; VKCHK(vkMapMemory(V.dev, b->m, 0, VK_WHOLE_SIZE, 0, &m)); b->p = m;
    b->noncoherent = !(V.mem.memoryTypes[mtype].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    b->used = n; *buf = b->b; *off = 0; return b->p;
}
// Dedicated per-slot HOST_VISIBLE | HOST_CACHED readback staging buffer:
// Reading 86 KB back from uncached WC vk_scratch() on ARM Mali took 0.37 ms; reading from HOST_CACHED takes ~0.015 ms!
static uint8_t *vk_rb_alloc(vksub_t *s, VkDeviceSize n, VkBuffer *buf, VkDeviceSize *off) {
    VkDeviceSize align = V.props.limits.nonCoherentAtomSize > 64 ? V.props.limits.nonCoherentAtomSize : 64;
    VkDeviceSize o = (s->rb_used + align - 1) & ~(align - 1);
    if (s->rb_buf && o + n <= s->rb_cap) {
        s->rb_used = o + n; *buf = s->rb_buf; *off = o; return s->rb_ptr + o;
    }
    // Grow only at submission end (rare: once when first readback occurs or exceeds current cap); defer-free old buffer.
    VkDeviceSize need = o + n;
    VkDeviceSize ncap = s->rb_cap ? s->rb_cap * 2 : (512u << 10);
    while (ncap < need) ncap *= 2;
    if (ncap > RB_STAGE_MAX_BYTES + (256u << 10)) ncap = (need + 0xffffull) & ~0xffffull;
    VkBuffer nb = VK_NULL_HANDLE; VkDeviceMemory nm = VK_NULL_HANDLE; void *np = NULL;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, ncap, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(V.dev, &bi, NULL, &nb) != VK_SUCCESS) return vk_scratch(n, 16, buf, off);
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, nb, &req);
    uint32_t mtype = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0);
    if (mtype == UINT32_MAX) mtype = vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, mtype};
    if (mtype == UINT32_MAX || vkAllocateMemory(V.dev, &ai, NULL, &nm) != VK_SUCCESS ||
        vkBindBufferMemory(V.dev, nb, nm, 0) != VK_SUCCESS || vkMapMemory(V.dev, nm, 0, VK_WHOLE_SIZE, 0, &np) != VK_SUCCESS) {
        if (nm) vkFreeMemory(V.dev, nm, NULL);
        vkDestroyBuffer(V.dev, nb, NULL);
        return vk_scratch(n, 16, buf, off);
    }
    if (s->rb_buf) { void vk_defer_free_buffer(VkBuffer, VkDeviceMemory); vk_defer_free_buffer(s->rb_buf, s->rb_mem); }
    // Keep prior allocations in same submission intact if growing mid-submission? Caller only grows when s->rb_used == 0
    // (we pre-reserve total_rb before loop in mtl_sub_commit!).
    s->rb_buf = nb; s->rb_mem = nm; s->rb_ptr = (uint8_t *)np; s->rb_cap = ncap;
    s->rb_noncoherent = !(V.mem.memoryTypes[mtype].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    s->rb_used = n; *buf = nb; *off = 0; return s->rb_ptr;
}
// The deferred frees have a lock of their own: the replay thread adds to its slot's list while it records (holding g_sub_lock for the whole submission), and a game
// thread that finalizes a pool (nvnMemoryPoolFinalize → mtl_pool_del) adds to the same list without g_sub_lock. Unlocked, two first adds to a recycled slot crashed
// Animal Crossing: New Horizons' release build (SIGSEGV in df_add: one thread had set capdf, the other wrote through the df still NULL). g_df_mu is above slot_recycle
typedef struct { dfree_t d; uint64_t fence; } df_pend_t;
static df_pend_t *g_dfp; static int g_ndfp, g_capdfp;
static void df_destroy_one(dfree_t d) {
    if (d.kind == 0) { vkDestroyBuffer(V.dev, (VkBuffer)d.a, NULL); if (d.b) vkFreeMemory(V.dev, (VkDeviceMemory)d.b, NULL); }
    else if (d.kind == 1) { vkDestroyImage(V.dev, (VkImage)d.a, NULL); if (d.b) vkFreeMemory(V.dev, (VkDeviceMemory)d.b, NULL); }
    else if (d.kind == 4) { void img_sub_free_(void *); vkDestroyImage(V.dev, (VkImage)d.a, NULL); img_sub_free_(d.b); }
    else if (d.kind == 2) vkDestroyImageView(V.dev, (VkImageView)d.a, NULL);
    else if (d.kind == 3) vkDestroyBufferView(V.dev, (VkBufferView)d.a, NULL);
    else if (d.kind == 5) vkDestroyFramebuffer(V.dev, (VkFramebuffer)d.a, NULL);
}
static void df_drain_pending(void) {
    uint64_t done = vk_fence_done();
    dfree_t ready[64]; int n = 0;
    for (;;) {
        n = 0;
        pthread_mutex_lock(&g_df_mu);
        int o = 0;
        for (int i = 0; i < g_ndfp; i++) {
            if (g_dfp[i].fence <= done && n < 64) ready[n++] = g_dfp[i].d;
            else g_dfp[o++] = g_dfp[i];
        }
        g_ndfp = o;
        pthread_mutex_unlock(&g_df_mu);
        if (!n) break;
        for (int i = 0; i < n; i++) df_destroy_one(ready[i]);
    }
}
static void df_add(int kind, void *a, void *b) {
    // DM_VK_X_LEAK=<mask> (live, debugging): the kinds whose bit is set (1 buffers, 2 images, 4 image views, 8 buffer views, 16 images in a block's slot) are never destroyed,
    // so no handle value is used twice (a cache keyed by a handle cannot meet a new object at an old address). Memory grows: for A/B runs only
    { const char *e = getenv("DM_VK_X_LEAK"); if (e && (atoi(e) >> kind & 1)) { static int n; if (!n++) VKLOG("[RED] DM_VK_X_LEAK=%s: freed Vulkan objects of those kinds are leaked", e); return; } }
    pthread_mutex_lock(&g_df_mu);
    if (g_cur >= 0) {
        vksub_t *s = &g_slot[g_cur]; if (s->ndf == s->capdf) { s->capdf = s->capdf ? 2 * s->capdf : 64; s->df = realloc(s->df, s->capdf * sizeof *s->df); }
        s->df[s->ndf++] = (dfree_t){kind, a, b};
        pthread_mutex_unlock(&g_df_mu); return;
    }
    // Outside a submission (a pool finalized between frames on a game thread): defer destruction by 3 completed
    // submissions so Mali GPUs never suffer MMU page faults (-4) from destroying buffers still referenced in flight.
    if (g_ndfp == g_capdfp) { g_capdfp = g_capdfp ? 2 * g_capdfp : 64; g_dfp = realloc(g_dfp, (size_t)g_capdfp * sizeof *g_dfp); }
    g_dfp[g_ndfp++] = (df_pend_t){{kind, a, b}, g_fence + 3};
    pthread_mutex_unlock(&g_df_mu);
}
// Image memory. Small images share blocks: each allocation the driver makes is a mapping of the process (Adreno maps them all), and a game with thousands of textures
// ran towards the kernel's per-process mapping limit (Xenoblade Chronicles 3: about 9,400 driver mappings in its first area, 2,216 of them 172 KB). Images up to
// IMG_SLOT_MAX take a slot of a block (64 slots, 2 MB to IMG_BLOCK bytes), in size classes 2^(k/2) x 4 KB apart (a slot is at most 41% larger than the image); larger ones, or
// ones that need more than 4 KB alignment, get memory of their own
enum { IMG_CLASSES = 18, IMG_BLOCK = 32 << 20 };
#define IMG_SLOT_MAX ((VkDeviceSize)2 << 20)
typedef struct imgblk { VkDeviceMemory m; uint32_t type, cls, nslot, nfree; uint64_t bits[128]; struct imgblk *next; } imgblk;   // bits: 1 = slot free
typedef struct { imgblk *b; uint32_t slot; } imgsub;
static imgblk *g_imgblk[IMG_CLASSES]; static pthread_mutex_t g_imgblk_mu = PTHREAD_MUTEX_INITIALIZER;
static VkDeviceSize img_class_size(int c) { return (c & 1 ? 6144ull : 4096ull) << (c >> 1); }   // 4K, 6K, 8K, 12K, 16K, 24K, ... 2M
VkDeviceMemory vk_image_memory(const VkMemoryRequirements *req, uint32_t type, VkDeviceSize *off, void **sub) {
    *off = 0; *sub = NULL;
    int c = 0; while (c < IMG_CLASSES && img_class_size(c) < req->size) c++;
    static int blocks = -1; if (blocks < 0) { const char *e = getenv("DM_VK_IMG_BLOCKS"); blocks = !(e && !atoi(e)); }   // DM_VK_IMG_BLOCKS=0 (start-up, A/B): every image gets memory of its own
    if (!blocks || c == IMG_CLASSES || req->size > IMG_SLOT_MAX || req->alignment > 4096) {   // memory of its own
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req->size, type}; VkDeviceMemory m;
        return vkAllocateMemory(V.dev, &ai, NULL, &m) ? VK_NULL_HANDLE : m;
    }
    pthread_mutex_lock(&g_imgblk_mu);
    imgblk *b = g_imgblk[c]; while (b && (b->type != type || !b->nfree)) b = b->next;
    if (!b) {
        b = calloc(1, sizeof *b); b->type = type; b->cls = (uint32_t)c; VkDeviceSize bs = img_class_size(c) * 64; if (bs < (2 << 20)) bs = 2 << 20; if (bs > IMG_BLOCK) bs = IMG_BLOCK; b->nslot = (uint32_t)(bs / img_class_size(c));   // 64 slots, 2 MB to 32 MB
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, (VkDeviceSize)b->nslot * img_class_size(c), type};
        if (vkAllocateMemory(V.dev, &ai, NULL, &b->m)) { pthread_mutex_unlock(&g_imgblk_mu); free(b); return VK_NULL_HANDLE; }
        for (uint32_t i = 0; i < b->nslot; i++) b->bits[i >> 6] |= 1ull << (i & 63);
        b->nfree = b->nslot; b->next = g_imgblk[c]; g_imgblk[c] = b;
    }
    uint32_t i = 0; while (!b->bits[i >> 6]) i += 64; i += (uint32_t)__builtin_ctzll(b->bits[i >> 6]);
    b->bits[i >> 6] &= ~(1ull << (i & 63)); b->nfree--;
    pthread_mutex_unlock(&g_imgblk_mu);
    imgsub *s = malloc(sizeof *s); s->b = b; s->slot = i; *sub = s; *off = (VkDeviceSize)i * img_class_size(c);
    return b->m;
}
static void img_sub_free(imgsub *s) {   // the slot is free again; blocks are kept (the next images of the class take them)
    pthread_mutex_lock(&g_imgblk_mu); s->b->bits[s->slot >> 6] |= 1ull << (s->slot & 63); s->b->nfree++; pthread_mutex_unlock(&g_imgblk_mu); free(s);
}
void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m) { df_add(0, (void *)b, (void *)m); }
void vk_defer_free_image_sub(VkImage i, void *sub) { df_add(4, (void *)i, sub); }
void img_sub_free_(void *s) { img_sub_free((imgsub *)s); }
void vk_defer_free_image(VkImage i, VkDeviceMemory m) { df_add(1, (void *)i, (void *)m); }
void vk_defer_free_view(VkImageView v) { df_add(2, (void *)v, NULL); }
void vk_defer_free_bview(VkBufferView v) { df_add(3, (void *)v, NULL); }
void vk_defer_free_framebuffer(VkFramebuffer fb) { df_add(5, (void *)fb, NULL); }
// Descriptor pools: the allocator counts what each set takes and moves to a new pool before any count runs out. Adreno's driver does not refuse an
// allocation past a pool's sizes: it succeeds and the submission then fails as a whole at vkEndCommandBuffer (VK_ERROR_OUT_OF_HOST_MEMORY; NEO's
// handheld frames with 32-slot texture arrays crossed the old 8192 combined-sampler limit at about the 712th draw)
enum { DP_SETS = 4096 };
static const uint32_t DP_CAP[9] = {[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC] = 16384, [VK_DESCRIPTOR_TYPE_SAMPLER] = 1024, [VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] = 65536, [VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] = 1024,
    [VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] = 2048, [VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER] = 4096, [VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER] = 2048,
    [VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] = 32768, [VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] = 4096};
static nvn_hmap g_dsl_need = {.klen = sizeof(VkDescriptorSetLayout)}; static pthread_mutex_t g_dsl_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_dsl_register(VkDescriptorSetLayout l, const VkDescriptorSetLayoutBinding *b, uint32_t n) {   // what one set of this layout takes, by type
    uint32_t *need = calloc(9, sizeof *need);
    for (uint32_t i = 0; i < n; i++) if ((uint32_t)b[i].descriptorType < 9) need[b[i].descriptorType] += b[i].descriptorCount;
    pthread_mutex_lock(&g_dsl_mu); uint32_t *old = nvn_hm_put(&g_dsl_need, &l, need); pthread_mutex_unlock(&g_dsl_mu);
    free(old);   // (a non-dispatchable handle may be the same value for two identical layouts: the counts are the same, the older array goes)
}
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout l) {
    vksub_t *s = cur();
    pthread_mutex_lock(&g_dsl_mu); const uint32_t *need = nvn_hm_get(&g_dsl_need, &l); pthread_mutex_unlock(&g_dsl_mu);
    for (;;) {
        if (s->dp_cur == s->ndp) {
            VkDescriptorPoolSize sz[9]; uint32_t nz = 0;
            for (int t = 0; t < 9; t++) if (DP_CAP[t]) sz[nz++] = (VkDescriptorPoolSize){(VkDescriptorType)t, DP_CAP[t]};
            VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, DP_SETS, nz, sz};
            if (s->ndp == s->capdp) { s->capdp = s->capdp ? 2 * s->capdp : 4; s->dp = realloc(s->dp, s->capdp * sizeof *s->dp); }
            VKCHK(vkCreateDescriptorPool(V.dev, &pi, NULL, &s->dp[s->ndp++]));
        }
        if (s->dp_cur != s->dp_counted) { memset(s->dp_used, 0, sizeof s->dp_used); s->dp_sets = 0; s->dp_counted = s->dp_cur; }
        int fits = s->dp_sets < DP_SETS;
        if (need) for (int t = 0; t < 9 && fits; t++) if (need[t] && s->dp_used[t] + need[t] > DP_CAP[t]) fits = 0;
        if (!fits) { s->dp_cur++; continue; }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, s->dp[s->dp_cur], 1, &l};
        VkDescriptorSet set; VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &set);
        if (r == VK_SUCCESS) { s->dp_sets++; s->dp_total_sets++; if (need) for (int t = 0; t < 9; t++) s->dp_used[t] += need[t]; return set; }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) die("[vk] vkAllocateDescriptorSets: %d", r);
        s->dp_cur++;
    }
}

// ---------------- submissions: begin, CPU-visible effects, commit ----------------
static void post(post_op o) { vksub_t *s = cur(); if (s->npost == s->cappost) { s->cappost = s->cappost ? 2 * s->cappost : 64; s->post = realloc(s->post, s->cappost * sizeof *s->post); } s->post[s->npost++] = o; }
void mtl_init(void) { vk_init(); }
static int g_shadow_pools;
static int g_last_sub_needs_rb;   // previous submission wrote CPU-readable GPU outputs or disabled readback_ranges
static int g_prev_rb_slot = -1;   // slot index of g_fence when it has staged readbacks (checked for 64KB block overlap in begin_segment)
static int rb_conflicts_with_dirty(const vksub_t *prev) {
    if (!prev || !prev->rb_staged || !prev->nrb || prev->fence <= vk_fence_done()) return 0;
    int conflict = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int k = 0; k < prev->nrb && !conflict; k++) {
        const rb_stage_t *rb = &prev->rb[k];
        vkpool_t *p = NULL;
        for (int i = 0; i < g_npool; i++) if (g_pool[i].serial == rb->pool_serial) { p = &g_pool[i]; break; }
        if (!p || !p->shadow || !p->synced) continue;
        if (p->flags & 0x1) continue;   // CPU_NO_ACCESS pool is never written by CPU
        if (!p->span) { conflict = 1; break; }
        // Check exact 1KB-aligned overlap with Frame N's dirty CPU bytes so Frame N's 1KB diff upload
        // can NEVER overwrite Frame N-1's pending GPU write in the same 1KB sub-block with stale guest RAM!
        uint64_t size = p->hi - p->lo;
        uint64_t glo = rb->lo > p->lo ? rb->lo : p->lo;
        uint64_t ghi = rb->hi < p->hi ? rb->hi : p->hi;
        if (glo >= ghi) continue;
        uint64_t rlo = (glo - p->lo) & ~1023ull;
        uint64_t rhi = ((ghi - p->lo) + 1023ull) & ~1023ull;
        if (rhi > size) rhi = size;
        if (rlo < rhi && p->baseline_complete && p->baseline) {
            uint8_t *lo = (uint8_t *)(uintptr_t)p->lo;
            // Compare actual bytes against p->baseline (do NOT test nvn_dirty_since_span here: completion_main()'s
            // own dm_guest_write_prepare() stamps page gen and falsely triggered conf=9..36/300 stalls in hybrid23!).
            pthread_mutex_lock(&g_baseline_mu);
            int changed = memcmp(lo + rlo, p->baseline + rlo, (size_t)(rhi - rlo)) != 0;
            pthread_mutex_unlock(&g_baseline_mu);
            if (changed) { conflict = 1; break; }
        }
    }
    pthread_rwlock_unlock(&g_pool_mu);
    return conflict;
}
static void pools_readback_barrier(VkCommandBuffer cb) {
    if (!g_shadow_pools) return;
    // The last part can be dropped after earlier ones were sent; each part must publish its writes for the CPU merge.
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
// Late-Wait CPU Work Hiding for Mode 2 (serial_safe):
// Runs BEFORE mtl_wait(g_fence) while GPU Frame N-1 is still executing!
// Pure-CPU work ONLY (touches ZERO p->shadow, ZERO p->baseline, ZERO Vulkan GPU objects, ZERO p->stamp updates):
// pre-scans dirty 16 KB blocks and executes strictly-adjacent mprotect(PROT_READ) + TLB shootdowns in parallel
// with GPU Frame N-1. After mtl_wait(g_fence) finishes, vk_pools_sync_() sees these pages already PG_PROT
// (~0.00 ms post-wait dirty_rearm!) while still re-protecting any page newly dirtied during the wait before memcpy.
static void vk_pools_pre_rearm_serial(void) {
    enum { SBLK = 4 << 10 };
    uint64_t rearm_ns = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) {
        vkpool_t *p = &g_pool[i];
        if (!p->shadow || !p->synced || (p->flags & 0x1) || !p->span) continue;
        uint8_t *lo = (uint8_t *)(uintptr_t)p->lo;
        uint64_t size = p->hi - p->lo;
        size_t snblk = (size_t)((size + SBLK - 1) / SBLK);
        uint64_t plo = size, phi = 0;
        for (size_t b = 0; b < snblk; b++) {
            uint64_t off = (uint64_t)b * SBLK;
            uint64_t n = size - off < SBLK ? size - off : SBLK;
            // Respect active Hot 4 KB pages: never mprotect(PROT_READ) a hot page unless no_hot is set!
            if (p->hot && __atomic_load_n(&p->hot[b], __ATOMIC_ACQUIRE) >= 3 &&
                !(p->no_hot && __atomic_load_n(&p->no_hot[b >> 4], __ATOMIC_ACQUIRE))) continue;
            if (!nvn_dirty_since_span(p->span, lo + off, n, p->stamp0)) continue;
            if (phi == 0) { plo = off; phi = off + n; }
            else if (off == phi) { phi = off + n; }
            else {
                uint64_t tr = st_now_ns();
                nvn_dirty_protect_span_blk(p->span, lo + plo, phi - plo, SBLK);
                rearm_ns += st_now_ns() - tr;
                plo = off; phi = off + n;
            }
        }
        if (phi > plo) {
            uint64_t tr = st_now_ns();
            nvn_dirty_protect_span_blk(p->span, lo + plo, phi - plo, SBLK);
            rearm_ns += st_now_ns() - tr;
        }
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (rearm_ns) vk_sub_timing_add(2, rearm_ns);
}
double vk_gpu_ms_recent(void);
static void begin_segment(int continuing) {
    vk_init();
    pthread_mutex_lock(&g_sub_lock);
    // the next slot of the ring (g_cur is -1 between submissions, so it cannot say where the ring is: every submission took slot 0 and waited in slot_recycle
    // for the one before to finish on the GPU, which kept the replay of a frame from overlapping the GPU work of the last)
    static int next; g_cur = next; next = (next + 1) % NSLOT; vksub_t *s = &g_slot[g_cur];
    slot_recycle(s);
    s->npost = 0; s->up_used = 0; s->ngw = 0; s->needs_cpu_rb = 0; s->nrb = 0; s->rb_staged = 0;
    s->readback_ranges = pool_readback_ranges_default();
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->up = s->ups[0]; s->nup_used = 1; s->nmain_sent = 0;
    VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++;
    // Pure-CPU replay state reset BEFORE mtl_wait(g_fence) so it overlaps GPU Frame N-1 in serial_safe mode!
    if (continuing) vk_exec_resume(); else vk_exec_reset();
    // copied pools: what the game wrote before submitting goes into the GPU copy before any part of the submission can reach the GPU.
    // Mode 2 (DEFAULT — Pure Serial Safe Sync): hides pure-CPU dirty_rearm (mprotect) + replay reset BEFORE
    // mtl_wait(g_fence), while strictly waiting for GPU Frame N-1 + completion_main() copy-back BEFORE touching
    // p->shadow, p->baseline, or vkQueueSubmit(Frame N) -> 100% 0% flicker with minimized PostWait_CPU_Time!
    pthread_rwlock_rdlock(&g_pool_mu);
    int shadow = 0;
    for (int i = 0; i < g_npool; i++) if (g_pool[i].shadow) { shadow = 1; break; }
    pthread_rwlock_unlock(&g_pool_mu);
    g_shadow_pools = shadow;
    if (shadow) {
        int diag = mem_diag_on(); uint64_t t = diag ? mem_diag_ns() : 0;
        int safe_sync = vk_pool_safe_sync();
        // Deferred Fence Wait (v1.1.2-hybrid33): in Mode 2 (serial_safe), enable safe CPU+GPU pipelining via
        // per-slot s->up staging whenever DM_VK_DEFERRED_WAIT=1 (default ON) and prior sub needs no CPU readback!
        int overlap = (safe_sync >= 2) ? vk_deferred_wait() : vk_pool_overlap();
        int rb_conf = (overlap && !g_last_sub_needs_rb && g_fence && g_prev_rb_slot >= 0)
            ? rb_conflicts_with_dirty(&g_slot[g_prev_rb_slot]) : 0;
        int need_full = !overlap || g_last_sub_needs_rb || !g_fence || rb_conf;
        if (!need_full) {
            __atomic_fetch_add(&g_ovl_used, 1, __ATOMIC_RELAXED);
            if (vk_fence_done() < g_fence) {
                double g_recent = vk_gpu_ms_recent();
                if (g_recent > 0.5 && g_recent < 30.0)
                    __atomic_fetch_add(&g_defwait_saved_ns, (uint64_t)(g_recent * 500000.0), __ATOMIC_RELAXED);
            }
        }
        else if (!overlap) __atomic_fetch_add(&g_ovl_skip_off, 1, __ATOMIC_RELAXED);
        else if (g_last_sub_needs_rb) __atomic_fetch_add(&g_ovl_skip_rb, 1, __ATOMIC_RELAXED);
        else if (rb_conf) __atomic_fetch_add(&g_ovl_skip_conflict, 1, __ATOMIC_RELAXED);
        uint64_t wait_target = need_full ? g_fence : (g_fence > 1 ? g_fence - 1 : 0);
        // Late-Wait CPU Work Hiding (Mode 2): execute non-hot mprotect(PROT_READ) rearm while GPU finishes!
        if (safe_sync >= 2 && wait_target) vk_pools_pre_rearm_serial();
        if (wait_target) {
            uint64_t tw = st_now_ns();
            vk_wd_enter(VK_WD_FENCE_WAIT, V.last_sub_diag.last_vs, V.last_sub_diag.last_fs);
            if (!mtl_wait(wait_target, 3000000000ull)) {
                VKLOG("[RED] [vk] FENCE TIMEOUT / DEVICE_LOST in begin_segment (wait=%llu done=%llu vendor=%#x dev=%s)",
                      (unsigned long long)wait_target, (unsigned long long)vk_fence_done(), V.props.vendorID, V.props.deviceName);
                vk_record_device_lost("begin_segment_fence_timeout", VK_ERROR_DEVICE_LOST);
                mtl_wait(wait_target, 10000000000ull);
            } else {
                __atomic_store_n(&V.dev_lost_streak, 0, __ATOMIC_RELAXED);
            }
            vk_wd_leave();
            vk_sub_timing_add(5, st_now_ns() - tw);
        }
        if (diag) mem_diag_note(0, t, 0);
        vk_pools_sync_ex(NULL, !need_full);
        int serial_safe = (safe_sync >= 2);
        int used_stage = (!need_full) || ((!serial_safe) && ((safe_sync == 1) || vk_pool_overlap()));
        if (!used_stage) {
            VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                VK_ACCESS_HOST_WRITE_BIT, POOL_CONSUMER_ACCESS};
            vkCmdPipelineBarrier(s->main, VK_PIPELINE_STAGE_HOST_BIT, pool_consumer_stages(), 0, 1, &hb, 0, NULL, 0, NULL);
        }
        // Unconditional cross-frame GPU L2/AFBC/tiler coherency barrier at the very start of s->main:
        // guarantees all Frame N-1 color/depth/shader/transfer writes are visible to Frame N on ARM Mali CSF
        // drivers (G720 49.1.0, G57 32.1.0, G52 26.0.0) even when s->up_used == 0.
        {
            VkMemoryBarrier xfb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
            vkCmdPipelineBarrier(s->main, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &xfb, 0, NULL, 0, NULL);
        }
    }   // (after the completion thread's copy-back when need_full, else GPU-ordered via s->up DMA)
    if (g_tsq) { vkCmdResetQueryPool(s->main, g_tsq, 2 * (uint32_t)g_cur, 2); vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)g_cur); }
    if (g_cq) vkCmdResetQueryPool(s->main, g_cq, (uint32_t)g_cur * CQ, CQ);
    s->ncq = 0;
    g_npass[g_cur] = 0; if (pass_times_on()) vkCmdResetQueryPool(s->main, g_pq, (uint32_t)(g_cur * PASSQ * 2), PASSQ * 2);
}
void mtl_sub_begin(void) { begin_segment(0); }
void vk_sub_resume(void) { begin_segment(1); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, -1}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    vksub_t *s = cur(); int q = -1;
    if (g_cq && (type == 0 || type == 0x10) && s->ncq < CQ) { q = g_cur * CQ + s->ncq++; vkCmdWriteTimestamp(vk_cmd(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_cq, (uint32_t)q); }
    post((post_op){2, NULL, 0, 0, addr, type, q});
}
static VkMemoryBarrier2 full_barrier(void) {
    return (VkMemoryBarrier2){VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
}
void vk_img_barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}
void vk_compute_barrier(VkCommandBuffer cb) {
    // Explicit compute-write publication to vertex/index/uniform/shader/transfer/indirect consumers
    // (Unity GPU skinning, BlendShape, MassLeaf, Boid, NormalMapper on Mali TBDR) plus full fallback barrier.
    VkMemoryBarrier cb_b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
        0, 1, &cb_b, 0, NULL, 0, NULL);
    vk_img_barrier_all(cb);
}
void vk_gpu_wrote(uint64_t a, uint64_t n);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!n) return;
    if (!vk_buf_at(src, n, &sb, &so) || !vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(1, "buffer copy outside the memory pools (%#llx → %#llx, %llu bytes)", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); return; }
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    VkBufferCopy r = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &r);
    vk_img_barrier_all(cb); vk_gpu_wrote(dst, n);
}
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    VkBuffer db; VkDeviceSize dof;
    if (!n) return;
    if (!vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(2, "buffer fill outside the memory pools"); return; }
    vk_end_pass_land();
    VkCommandBuffer cb = vk_cmd(); vk_img_barrier_all(cb);
    vkCmdFillBuffer(cb, db, dof, n & ~3ull, value);
    vk_img_barrier_all(cb); vk_gpu_wrote(dst, n);
}
// an inline uniform update (UpdateUniformBuffer): the bytes are taken at record time, copied in GPU order from scratch memory
// Inline uniform updates (UpdateUniformBuffer) are deferred instead of ending the render pass (as nvn_mtl.m's ubd_*): a copy cannot run inside a pass, and
// ending it stores every attachment and loads it again in the next pass, which on a tiler is a round trip of the whole tile memory per update (Unity updates
// before nearly every draw: NEO had about 145 passes for 148 draws a frame). NVN applies the update in command order, so a draw after it whose uniform
// binding overlaps it reads a shadow: memory with the deferred updates applied in order (vk_ubd_shadow). The real copies are made in order when a pass
// ends anyway, and at the end of the submission (vk_ubd_land), so later passes, copies, dispatches and the CPU see them as before.
typedef struct { uint64_t lo, hi; VkBuffer sb; VkDeviceSize so; const uint8_t *p; uint8_t *own; } ubd_t;   // p: the bytes for CPU readers (own, or the scratch copy); own: their heap copy, freed with the entry
enum { UBD_MAX = 512 };
static ubd_t g_ubd[UBD_MAX]; static int g_nubd; extern int g_ubd_heap; uint64_t g_ubd_gen = 1, g_ubd_landed, g_ubd_shadows, g_ubd_bytes, g_ns_submit, g_ubd_superseded;
void vk_end_pass_land(void);
void vk_sub_update(uint64_t dst, const void *data, uint64_t n) {
    VkBuffer db, sb; VkDeviceSize dof, so;
    if (!n) return;
    if (!vk_buf_at(dst, n, &db, &dof)) { VKRED_ONCE(3, "uniform update outside the memory pools"); return; }
    // an update of exactly the range of a deferred one supersedes it: the older one goes (Unity rewrites the same few constant buffers before every draw,
    // so the list keeps about one entry per buffer region, which keeps the landing to a few copies and the shadows cheap to build)
    for (int i = g_nubd - 1; i >= 0; i--) if (g_ubd[i].lo == dst && g_ubd[i].hi == dst + n) { free(g_ubd[i].own); memmove(&g_ubd[i], &g_ubd[i + 1], sizeof *g_ubd * (size_t)(g_nubd - i - 1)); g_nubd--; g_ubd_superseded++; break; }
    if (g_nubd == UBD_MAX) vk_end_pass_land();   // too many distinct ranges: they land now
    uint64_t pad_n = (n + 15ull) & ~15ull;
    uint8_t *p = vk_scratch(pad_n, V.props.limits.minUniformBufferOffsetAlignment, &sb, &so);
    memcpy(p, data, n);
    if (pad_n > n) memset(p + n, 0, (size_t)(pad_n - n));
    // The shadows read the update's bytes again (vk_ubd_read), and the scratch copy is the wrong place to read them from: it is mapped GPU memory, host-visible and
    // coherent but on Adreno not cached (write-combined), where a read costs many times a write. Breath of the Wild rewrites the same 64 bytes of a block about 460
    // times a frame and builds about 280 shadows from them: the memcpy calls that read those bytes back were 17 to 33% of a Galaxy S25 Ultra's replay thread
    // (DM_VK_PROF: 12-27% for the in-flight list, 5-6% for this one), against 3-4% for the 18 MB a frame of bindings copied out of pool memory. An update of up
    // to 4 KB keeps a copy in ordinary memory for them, freed with its entry; a larger one is read from scratch as before (its second copy would cost more than its
    // few reads). Switched within one run, standing in that game's woods (4,850 draws, Turnip 26.2.3): descriptor sets 7.0 -> 4.7 ms a frame, the replay 12.8 ->
    // 11.3 ms, vk_ubd_read 25-39% -> 6% of the samples. DM_VK_UBD_HEAP=0 (live): none, as before
    uint8_t *own = g_ubd_heap && n <= 4096 ? malloc(n) : NULL; if (own) memcpy(own, data, n);
    g_ubd[g_nubd++] = (ubd_t){dst, dst + n, sb, so, own ? own : p, own}; g_ubd_gen++;
}
// the draw bisection (gfx_vk_exec.c draw_allowed): the outcome of this submission
extern int g_bis_on, g_bis_lo, g_bis_hi, g_bis_trace, g_bis_var; void vk_bisect_reset_count(void);
static void vk_bisect_result(int ok) {
    static int done, draws_seen; extern uint32_t g_sub_ops[64]; int nd = (int)(g_sub_ops[25] + g_sub_ops[26]);
    if (done) {   // the found draw: one change per submission, then the draw is no longer traced
        if (g_bis_trace >= 0) {
            static const char *what[] = {"as recorded", "depth bias values 0", "no depth bias", "dummy textures", "vertex offset 0", "zero vertex buffer", "in a new pass", "skipped alone (the rest drawn)", "without their fragment shader", "with zero uniform buffers", "with their quad draws skipped", "with all but their quad draws skipped"};
            VKLOG("bisection: draws from %d on %s: %s", g_bis_trace, what[g_bis_var], ok ? "ACCEPTED" : "refused");
            if (++g_bis_var > 11) { g_bis_trace = -1; g_bis_var = 0; }
        }
        vk_bisect_reset_count(); return; }
    if (!g_bis_on) { if (!ok) { g_bis_on = 1; g_bis_lo = 0; g_bis_hi = nd + 1; draws_seen = nd; VKLOG("bisecting the refused submission's %d draws", nd); } vk_bisect_reset_count(); return; }
    int mid = (g_bis_lo + g_bis_hi) / 2;
    if (ok) g_bis_lo = mid; else g_bis_hi = mid;
    if (g_bis_hi - g_bis_lo <= 1) { done = 1; g_bis_on = 0; g_bis_trace = g_bis_lo; VKLOG("bisection: draw %d of %d is the first the driver refuses (traced in the next submission)", g_bis_lo, draws_seen); }
    vk_bisect_reset_count();
}
void vk_ubd_land_drop(void) { for (int i = 0; i < g_nubd; i++) free(g_ubd[i].own); g_nubd = 0; g_ubd_gen++; }
// Landed updates still in flight: their copies are queued but the GPU may not have run them, so memory may not hold them yet; a shadow built on the
// CPU (vk_ubd_read) applies them over memory until their submission completes. Without this a shadow mixed the newest update with the frame before's
// memory (shadows detached from moving characters, far pedestrians flickering).
typedef struct { uint64_t lo, hi, fence; const uint8_t *data; uint8_t *own; } ubd_infl_t;   // data: the update's bytes: its heap copy (own, freed when the entry goes), or its submission's scratch memory, which is not reused before that submission completes
static ubd_infl_t *g_infl; static int g_ninfl, g_capinfl;
static void infl_prune(void) {
    uint64_t done = vk_fence_done(); int o = 0;
    for (int i = 0; i < g_ninfl; i++) if (g_infl[i].fence > done) g_infl[o++] = g_infl[i]; else free(g_infl[i].own);
    g_ninfl = o;
}
static void sub_gpu_wrote(uint64_t a, uint64_t n);
extern int g_x_inflkeep;
void vk_ubd_land(VkCommandBuffer cb) {   // outside a render pass
    if (!g_nubd) return;
    infl_prune();
    int ubd_cb = vk_ubd_copyback();
    vk_img_barrier_all(cb);
    for (int i = 0; i < g_nubd; i++) {
        uint64_t len = g_ubd[i].hi - g_ubd[i].lo;
        VkBuffer db; VkDeviceSize dof;
        int mirrored = 0;
        if (vk_buf_at(g_ubd[i].lo, len, &db, &dof)) {
            VkBufferCopy r = {g_ubd[i].so, dof, len}; vkCmdCopyBuffer(cb, g_ubd[i].sb, db, 1, &r);
            if (ubd_cb >= 2) {
                // Legacy GPU->CPU async copy-back via completion_main() pool_merge_back() (A/B fallback).
                sub_gpu_wrote(g_ubd[i].lo, len);
                if (g_cur >= 0) g_slot[g_cur].needs_cpu_rb = 1;
            } else if (ubd_cb == 1) {
                // Synchronous CPU-RAM + baseline mirror for copied pools (Mali VQ_NO_ZEROCOPY):
                // Hold g_pool_mu rdlock -> g_baseline_mu atomically so mtl_pool_del() and completion_main()
                // can never race with p->baseline even when DM_NVN_ASYNC=1 is enabled.
                pthread_rwlock_rdlock(&g_pool_mu);
                vkpool_t *p = NULL;
                for (int pi = 0; pi < g_npool; pi++) {
                    if (g_ubd[i].lo >= g_pool[pi].lo && g_ubd[i].hi <= g_pool[pi].hi) { p = &g_pool[pi]; break; }
                }
                if (p && p->shadow && g_ubd[i].p) {
                    uint8_t *dst = (uint8_t *)(uintptr_t)g_ubd[i].lo;
                    pthread_mutex_lock(&g_baseline_mu);
                    dm_guest_write_prepare(dst, (size_t)len);
                    memcpy(dst, g_ubd[i].p, (size_t)len);
                    if (p->baseline) memcpy(p->baseline + (g_ubd[i].lo - p->lo), g_ubd[i].p, (size_t)len);
                    pool_bump_cpu_gen(p, g_ubd[i].lo - p->lo, len);
                    pthread_mutex_unlock(&g_baseline_mu);
                    mirrored = 1;
                }
                pthread_rwlock_unlock(&g_pool_mu);
            }
        }
        if (mirrored) {
            // Guest CPU RAM already holds these bytes synchronously: drop any superseded exact or subset
            // g_infl entries covered by [lo, hi) and free heap copy immediately.
            for (int k = g_ninfl - 1; k >= 0; k--)
                if (g_infl[k].lo >= g_ubd[i].lo && g_infl[k].hi <= g_ubd[i].hi) {
                    free(g_infl[k].own);
                    memmove(&g_infl[k], &g_infl[k + 1], sizeof *g_infl * (size_t)(g_ninfl - k - 1));
                    g_ninfl--;
                }
            free(g_ubd[i].own);
        } else {
            if (g_ninfl == g_capinfl) { g_capinfl = g_capinfl ? 2 * g_capinfl : 256; g_infl = realloc(g_infl, sizeof *g_infl * (size_t)g_capinfl); }
            for (int k = g_x_inflkeep ? -1 : g_ninfl - 1; k >= 0; k--)
                if (g_infl[k].lo == g_ubd[i].lo && g_infl[k].hi == g_ubd[i].hi) {
                    free(g_infl[k].own);
                    memmove(&g_infl[k], &g_infl[k + 1], sizeof *g_infl * (size_t)(g_ninfl - k - 1));
                    g_ninfl--; break;
                }
            g_infl[g_ninfl++] = (ubd_infl_t){g_ubd[i].lo, g_ubd[i].hi, g_fence + 1, g_ubd[i].p, g_ubd[i].own};
        }
    }
    vk_img_barrier_all(cb);
    g_ubd_landed += (uint64_t)g_nubd; g_nubd = 0; g_ubd_gen++;
}
int vk_ubd_overlaps(uint64_t a, uint64_t n) { for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo < a + n && a < g_ubd[i].hi) return 1; return 0; }
// GPU writes to pool memory (buffer copies and fills, texture → buffer copies, storage buffers of dispatches) whose submission may not have completed:
// until it has, a CPU read of that memory (vk_ubd_read: a shadow, a handle in a uniform buffer) may see the bytes from before them or after them
typedef struct { uint64_t lo, hi, fence; } gpuw_t;
static gpuw_t *g_gw; static size_t g_ngw, g_cgw; uint64_t g_ubd_racy;
int g_vk_cpu_ready = 1;   // refreshed by replay at each submission (DM_VK_CPU_READY)
static void sub_gpu_wrote(uint64_t a, uint64_t n) {   // into the open submission's list (pools_copy_back)
    if (g_cur < 0 || !n) return;
    vksub_t *s = &g_slot[g_cur]; uint64_t lo = a, hi = a + n, bd = ~0ull; int best = -1;
    for (int k = 0; k < s->ngw; k++) { uint64_t d = lo > s->gw[k].hi ? lo - s->gw[k].hi : s->gw[k].lo > hi ? s->gw[k].lo - hi : 0; if (d < bd) { bd = d; best = k; } }
    // Merge only within 4 KB (1 page) unless GW_SUB (256) is full, avoiding 64 KB blind spans across live CPU uniforms.
    if (best >= 0 && (bd <= 4096 || s->ngw == GW_SUB)) { if (lo < s->gw[best].lo) s->gw[best].lo = lo; if (hi > s->gw[best].hi) s->gw[best].hi = hi; }
    else { s->gw[s->ngw].lo = lo; s->gw[s->ngw].hi = hi; s->ngw++; }
}
static void gw_track_only(uint64_t a, uint64_t n) {
    uint64_t f = g_fence + 1;
    for (size_t i = g_ngw; i-- > 0 && g_gw[i].fence == f;) if (g_gw[i].lo <= a && a + n <= g_gw[i].hi) return;
    if (g_ngw == g_cgw) {
        uint64_t done = vk_fence_done(); size_t o = 0;
        for (size_t i = 0; i < g_ngw; i++) if (g_gw[i].fence > done) g_gw[o++] = g_gw[i];
        g_ngw = o;
    }
    if (g_ngw == g_cgw) {
        if (g_cgw > SIZE_MAX / 2 / sizeof *g_gw) die("[vk] GPU write tracking size overflow");
        size_t capacity = g_cgw ? g_cgw * 2 : 512;
        gpuw_t *next = realloc(g_gw, capacity * sizeof *next);
        if (!next) die("[vk] cannot track live GPU buffer writes");
        g_gw = next; g_cgw = capacity;
    }
    g_gw[g_ngw++] = (gpuw_t){a, a + n, f};
}
void vk_gpu_wrote(uint64_t a, uint64_t n) {
    if (!n || a > UINT64_MAX - n) return;
    gw_track_only(a, n);
    vkpool_t *p = vk_pool_of(a, 1);
    if (!p || !(p->flags & 0x1)) {   // CPU-accessible pool (CPU_CACHED or CPU_UNCACHED)
        if (p) {
            pool_mark_no_hot(p, a, n);
            if (!p->gpu_wrote_ever) {
                p->gpu_wrote_ever = 1;
                if (!p->baseline && p->shadow) {
                    uint64_t sz = p->hi - p->lo;
                    uint8_t *nb = calloc(1, (size_t)sz);
                    if (nb) {
                        pthread_mutex_lock(&g_baseline_mu);
                        if (!p->baseline) {
                            memcpy(nb, (const uint8_t *)(uintptr_t)p->lo, (size_t)sz);
                            p->baseline = nb;
                            p->baseline_complete = 1;
                            p->baseline_fresh = 0;
                            nb = NULL;
                        }
                        pthread_mutex_unlock(&g_baseline_mu);
                        free(nb);
                    }
                }
            }
        }
        sub_gpu_wrote(a, n);
        if (g_cur >= 0) g_slot[g_cur].needs_cpu_rb = 1;
    }
}
void vk_gpu_wrote_ssbo(uint64_t a, uint64_t n) {
    if (!n || a > UINT64_MAX - n) return;
    gw_track_only(a, n);
    int is_no_access = 0, is_uncached = 0;
    int safe_sync = vk_pool_safe_sync();
    int pipelined = vk_pool_overlap() && safe_sync != 2 && (safe_sync != 1 || vk_safe1_rb_staged());
    pthread_rwlock_rdlock(&g_pool_mu);
    vkpool_t *p = NULL;
    for (int pi = 0; pi < g_npool; pi++) {
        if (a >= g_pool[pi].lo && a < g_pool[pi].hi) { p = &g_pool[pi]; break; }
    }
    if (p) {
        if (p->flags & 0x1) {
            is_no_access = 1;
        } else {
            // Permanently ban ANY GPU-written 64 KB block from hot_blocks (pure atomic store, 0 mprotect in replay!)
            pool_mark_no_hot(p, a, n);
            if (!p->gpu_wrote_ever) {
                p->gpu_wrote_ever = 1;
                if (!p->baseline && p->shadow) {
                    uint64_t sz = p->hi - p->lo;
                    uint8_t *nb = calloc(1, (size_t)sz);
                    if (nb) {
                        pthread_mutex_lock(&g_baseline_mu);
                        if (!p->baseline) {
                            memcpy(nb, (const uint8_t *)(uintptr_t)p->lo, (size_t)sz);
                            p->baseline = nb;
                            p->baseline_complete = 1;
                            p->baseline_fresh = 0;
                            nb = NULL;
                        }
                        pthread_mutex_unlock(&g_baseline_mu);
                        free(nb);
                    }
                }
            }
            is_uncached = (p->flags & 0x2) != 0;
            if (is_uncached && pipelined && n > (2ull << 20) && p->gpu_blk) {
                uint64_t glo = a > p->lo ? a : p->lo;
                uint64_t ghi = (a + n) < p->hi ? (a + n) : p->hi;
                if (glo < ghi) {
                    uint64_t size = p->hi - p->lo;
                    size_t nblk = (size_t)((size + (64u << 10) - 1u) / (64u << 10));
                    size_t b0 = (size_t)((glo - p->lo) >> 16), b1 = (size_t)((ghi - 1u - p->lo) >> 16);
                    if (b1 >= nblk) b1 = nblk ? nblk - 1 : 0;
                    for (size_t b = b0; b <= b1 && b < nblk; b++) __atomic_store_n(&p->gpu_blk[b], 1, __ATOMIC_RELEASE);
                }
            }
        }
    }
    pthread_rwlock_unlock(&g_pool_mu);
    if (is_no_access) return;   // CPU_NO_ACCESS pools are strictly GPU-only (0 readback)
    // GPU-only compute SSBO buffers (>64 KiB) in CPU_UNCACHED pools (e.g. Unity SkinnedMeshRenderer dynamic vertex pools)
    // are GPU-produced / GPU-consumed (never read back by CPU); skipping CPU readback here eliminates 0.5-126 ms
    // pool_readback stalls AND unlocks Deferred Fence Wait overlap (~99%) while keeping <=64 KiB queries & CPU_CACHED intact!
    if (!is_uncached || n <= (64ull << 10)) {
        sub_gpu_wrote(a, n);
        if (g_cur >= 0) g_slot[g_cur].needs_cpu_rb = 1;
    }
}
int vk_gpu_pending(uint64_t a, uint64_t n) {
    if (!g_ngw) return 0;
    uint64_t done = vk_fence_done();
    for (size_t i = g_ngw; i-- > 0 && g_gw[i].fence > done;) if (g_gw[i].lo < a + n && a < g_gw[i].hi) return 1;
    return 0;
}
// Port of nxnative 26c898e4e / 1f01e28c cpu_access_ready and gw_wait_prior.
// Called only by replay, before this command allocates descriptors or scratch.
// Target inline updates are already visible through vk_ubd_read. Any updates
// still deferred are later than all tracked GPU writes (GPU producers land
// them first), so they are the donor's later cpu_known ranges. Fills and
// already-landed updates conservatively retain the GPU wait on this backend.
void vk_cpu_ready(uint64_t a, uint64_t n) {
    if (!g_vk_cpu_ready || !n || a > UINT64_MAX - n) return;
    uint64_t done = vk_fence_done();
    // Appends and compaction preserve fence order. Only the live suffix matters;
    // waiting for its newest uncovered overlap also completes every older one.
    for (size_t i = g_ngw; i-- > 0 && g_gw[i].fence > done;) {
        const gpuw_t *w = &g_gw[i];
        if (w->lo >= a + n || a >= w->hi) continue;
        uint64_t at = a > w->lo ? a : w->lo;
        uint64_t end = a + n < w->hi ? a + n : w->hi;
        while (at < end) {
            uint64_t covered = at;
            for (int j = 0; j < g_nubd; j++) {
                const ubd_t *k = &g_ubd[j];
                if (at >= k->lo && at < k->hi && k->hi > covered) covered = k->hi;
            }
            if (covered == at) break;
            at = covered < end ? covered : end;
        }
        if (at == end) continue;   // deferred bytes cover prior and open producers alike
        if (w->fence <= g_fence) mtl_wait(w->fence, UINT64_MAX);
        else { mtl_wait(mtl_sub_commit(), UINT64_MAX); vk_sub_resume(); }
        return;
    }
}
// what a GPU read of [a, a + n) sees at this point of the command stream, into dst (memory, then the deferred updates in order)
void vk_ubd_read(uint64_t a, uint64_t n, uint8_t *dst) {
    if (vk_gpu_pending(a, n)) { static int k; g_ubd_racy++; if (k < 20) { k++; int cov = 0; for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo <= a && a + n <= g_ubd[i].hi) cov = 1;
        VKLOG("[RED] a CPU read of %#llx +%llu (%s) overlaps a GPU write not known to be complete", (unsigned long long)a, (unsigned long long)n, cov ? "covered by a deferred update" : "partly from memory"); } }
    memcpy(dst, (const void *)(uintptr_t)a, n);
    if (g_ninfl) { uint64_t done = vk_fence_done();
        for (int i = 0; i < g_ninfl; i++) { if (g_infl[i].fence <= done || !g_infl[i].data) continue; uint64_t lo = g_infl[i].lo > a ? g_infl[i].lo : a, hi = g_infl[i].hi < a + n ? g_infl[i].hi : a + n;
            if (lo < hi) memcpy(dst + (lo - a), g_infl[i].data + (lo - g_infl[i].lo), hi - lo); } }
    for (int i = 0; i < g_nubd; i++) { uint64_t lo = g_ubd[i].lo > a ? g_ubd[i].lo : a, hi = g_ubd[i].hi < a + n ? g_ubd[i].hi : a + n;
        if (lo < hi) memcpy(dst + (lo - a), g_ubd[i].p + (lo - g_ubd[i].lo), hi - lo); }
}
// a uniform binding [a, a + n) that deferred updates overlap: its shadow (0 when none overlaps, hybrid8 verbatim).
int vk_ubd_shadow(uint64_t a, uint64_t n, VkBuffer *buf, VkDeviceSize *off) {
    if (!g_nubd || !vk_ubd_overlaps(a, n)) return 0;   // (a binding no deferred update touches reads memory on the GPU, after the in-flight copies: no shadow needed)
    // Zero-copy per-slot dynamic uniform ring fast-path: if the latest matching update in g_ubd[] covers
    // [a, a+n) completely and no subsequent update overlaps it, bind (sb, so + delta) straight from slot scratch!
    VkDeviceSize al = V.props.limits.minUniformBufferOffsetAlignment;
    for (int i = g_nubd - 1; i >= 0; i--) {
        if (g_ubd[i].lo < a + n && a < g_ubd[i].hi) {
            if (g_ubd[i].lo <= a && a + n <= g_ubd[i].hi) {
                VkDeviceSize d_off = g_ubd[i].so + (a - g_ubd[i].lo);
                if (al <= 1 || (d_off % al) == 0) {
                    *buf = g_ubd[i].sb; *off = d_off; g_ubd_shadows++;
                    return 1;
                }
            }
            break;   // partial overlap or unaligned: fall back to composite shadow copy below
        }
    }
    enum { NC = 256 }; static struct { uint64_t a, n, gen, fence; VkBuffer b; VkDeviceSize o; } c[NC];
    unsigned k = (unsigned)((a ^ n * 0x9E3779B97F4A7C15ull) >> 6) % NC;
    extern int g_x_noshc; if (c[k].a == a && c[k].n == n && c[k].gen == g_ubd_gen && c[k].fence == g_fence && !g_x_noshc) { *buf = c[k].b; *off = c[k].o; return 1; }
    uint64_t pad_n = (n + 15ull) & ~15ull;
    uint8_t *d = vk_scratch(pad_n, V.props.limits.minUniformBufferOffsetAlignment, buf, off);
    vk_ubd_read(a, n, d);
    if (pad_n > n) memset(d + n, 0, (size_t)(pad_n - n));
    g_ubd_bytes += n;
    c[k].a = a; c[k].n = n; c[k].gen = g_ubd_gen; c[k].fence = g_fence; c[k].b = *buf; c[k].o = *off; g_ubd_shadows++;
    return 1;
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }

// completion: a queue of committed submissions, each with its CPU-visible effects
typedef struct cpend { uint64_t fence, frame; post_op *ops; int n; int slot, rslot; int npass; pass_meta pmeta[PASSQ]; struct cpend *next; } cpend;
static cpend *g_ch, *g_ct; static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); if (!g) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); g = (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; } return g * 12 / 625; }
// GPU time per NVN frame (the sum of its submissions' timestamp spans), for the timing line's GPU column: the frame a submission belongs to is the one being replayed
static uint64_t g_cur_frame, g_frame_gpu[64], g_frame_gpu_of[64];
void mtl_set_frame(uint64_t f) { __atomic_store_n(&g_cur_frame, f, __ATOMIC_RELAXED); }
double mtl_gpu_ms(uint64_t f) { return g_frame_gpu_of[f % 64] == f + 1 ? g_frame_gpu[f % 64] / 1e6 : -1; }
double vk_gpu_ms_recent(void) {   // the latest finished frame's GPU time among the last 8 (-1: none known)
    uint64_t f = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED);
    for (uint64_t k = 2; k < 8 && k <= f; k++) { double g = mtl_gpu_ms(f - k); if (g >= 0) return g; }
    return -1;
}
// DM_VK_PASS_SPIKE=<ms> (read at start-up, with DM_VK_PASS_TIMES; default 25): a frame whose timed passes and dispatches, or whose submissions, take more than that lists its costliest
// ones, to find what a one-frame GPU spike drew (Tears of the Kingdom: 25-180 ms frames among 10-13 ms ones while fighting). Completion thread only
static uint64_t g_sf_frame = ~0ull; static struct { double ms; pass_meta m; } g_sf[PASSQ * 4]; static int g_nsf; static double g_sf_tot;
static void pass_spike_add(double ms, const pass_meta *m) { g_sf_tot += ms; if (g_nsf < PASSQ * 4) { g_sf[g_nsf].ms = ms; g_sf[g_nsf].m = *m; g_nsf++; } }
static void pass_spike_flush(void) {
    static double lim = -1; if (lim < 0) { const char *e = getenv("DM_VK_PASS_SPIKE"); lim = e && atof(e) > 0 ? atof(e) : 25; }
    double fg = g_sf_frame != ~0ull ? mtl_gpu_ms(g_sf_frame) : -1;   // the frame's submissions end to end: a spike outside the timed passes shows as the difference
    if (g_sf_frame != ~0ull && (g_sf_tot > lim || fg > lim)) {
        for (int i = 0; i < g_nsf && i < 6; i++) for (int j = i + 1; j < g_nsf; j++) if (g_sf[j].ms > g_sf[i].ms) { __typeof__(g_sf[0]) x = g_sf[i]; g_sf[i] = g_sf[j]; g_sf[j] = x; }
        char b[1200]; int o = snprintf(b, sizeof b, "GPU spike: frame %llu, %.2f ms in %d passes and dispatches (frame GPU %.2f ms, %.2f outside them):", (unsigned long long)g_sf_frame, g_sf_tot, g_nsf, fg, fg - g_sf_tot);
        for (int i = 0; i < g_nsf && i < 6 && o < (int)sizeof b - 100; i++) { const pass_meta *m = &g_sf[i].m;
            if (m->fmt == PT_DISPATCH) o += snprintf(b + o, sizeof b - (size_t)o, " #%u %.2f ms (dispatch %ux%ux%u);", m->fidx, g_sf[i].ms, m->w, m->h, m->draws);
            else o += snprintf(b + o, sizeof b - (size_t)o, " #%u %.2f ms (%ux%u fmt %u, %u draws);", m->fidx, g_sf[i].ms, m->w, m->h, m->fmt, m->draws & 0xffff); }
        VKLOG("%s", b);
    }
    g_nsf = 0; g_sf_tot = 0;
}
static void *completion_main(void *a) {
    (void)a; pthread_setname_np(pthread_self(), "dm-vk-complete");
    for (;;) {
        pthread_mutex_lock(&g_cmu);
        while (!g_ch) pthread_cond_wait(&g_ccv, &g_cmu);
        cpend *c = g_ch; pthread_mutex_unlock(&g_cmu);
        if (V.has_timeline) {
            wait_fence(c->fence);
        } else {
            VkResult r = vkWaitForFences(V.dev, 1, &g_fring[c->fence % NFRING], VK_TRUE, 3000000000ull);
            if (r == VK_TIMEOUT) {
                VKLOG("[RED] [vk] FENCE TIMEOUT / DEVICE_LOST (vkWaitForFences=%llu done=%llu vendor=%#x dev=%s)",
                      (unsigned long long)c->fence, (unsigned long long)vk_fence_done(), V.props.vendorID, V.props.deviceName);
                vk_record_device_lost("completion_fence_timeout", VK_ERROR_DEVICE_LOST);
                r = vkWaitForFences(V.dev, 1, &g_fring[c->fence % NFRING], VK_TRUE, 10000000000ull);
            }
            if (r != VK_SUCCESS) {
                if (r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) vk_record_device_lost("completion_fence", r);
                die("[vk] GPU fence wait did not complete: %d", r);
            }
            __atomic_store_n(&V.dev_lost_streak, 0, __ATOMIC_RELAXED);
        }
        if (g_shadow_pools && c->rslot >= 0) pools_copy_back(&g_slot[c->rslot]);
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
            { uint64_t ns = (uint64_t)((double)(t[1] - t[0]) * V.props.limits.timestampPeriod); g_gpu_ns += ns; int k = (int)(c->frame % 64);
              if (g_frame_gpu_of[k] != c->frame + 1) { g_frame_gpu[k] = 0; g_frame_gpu_of[k] = c->frame + 1; } g_frame_gpu[k] += ns; } }
        if (g_pq && pass_times_on() && c->slot >= 0) {   // this submission's passes
            int np = c->npass; uint64_t t[PASSQ * 2];
            if (c->frame != g_sf_frame) { pass_spike_flush(); g_sf_frame = c->frame; }
            if (np && vkGetQueryPoolResults(V.dev, g_pq, (uint32_t)(c->slot * PASSQ * 2), (uint32_t)np * 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                for (int i = 0; i < np; i++) if (t[2 * i + 1] > t[2 * i]) { pass_spike_add((double)(t[2 * i + 1] - t[2 * i]) * V.props.limits.timestampPeriod / 1e6, &c->pmeta[i]); int f = (int)c->pmeta[i].fidx; g_pacc[f] += (double)(t[2 * i + 1] - t[2 * i]) * V.props.limits.timestampPeriod / 1e6; g_pcnt[f]++; g_plast[f] = c->pmeta[i]; if (f + 1 > g_pmax) g_pmax = f + 1; }
        }
        uint64_t now = gpu_ticks(), tend = 0;
        if (g_tsq && c->slot >= 0) { uint64_t t[2]; if (vkGetQueryPoolResults(V.dev, g_tsq, 2 * (uint32_t)c->slot, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) tend = t[1]; }
        for (int i = 0; i < c->n; i++) {
            post_op *o = &c->ops[i];
            if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; continue; }
            uint64_t ts = now, tq;
            if (o->q >= 0 && tend && vkGetQueryPoolResults(V.dev, g_cq, (uint32_t)o->q, 1, 8, &tq, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && tq <= tend) {
                uint64_t back = (uint64_t)((double)(tend - tq) * V.props.limits.timestampPeriod * 12 / 625); if (back < now) ts = now - back; }
            // render_cond: a timestamp report's counter field is 0, as on the GPU (gfx_vk_exec.c render_cond evaluates conditions on it)
            uint64_t rec[2] = {o->type == 0 || o->type == 0x10 ? (DM_GAME_RENDER_COND ? 0 : ts) : o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? (1u << 20) : 0, ts}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
        }
        pthread_mutex_lock(&g_cmu); g_ch = c->next; if (!g_ch) g_ct = NULL; pthread_mutex_unlock(&g_cmu);
        if (g_prereset > 0 && c->rslot >= 0) { vksub_t *s = &g_slot[c->rslot];   // its command buffers reset here (slot_recycle); the slot is not taken before g_done passes this fence
            if (s->fence == c->fence) { for (int i = 0; i < s->nmain_alloc; i++) vkResetCommandBuffer(s->mains[i], 0); for (int i = 0; i < s->nup_alloc; i++) vkResetCommandBuffer(s->ups[i], 0); } }
        pthread_mutex_lock(&g_done_mu);
        if (c->fence > g_done) {
            __atomic_store_n(&g_done, c->fence, __ATOMIC_RELEASE);
            __atomic_store_n(&V.completed_seq, c->fence, __ATOMIC_RELEASE);
        }
        pthread_cond_broadcast(&g_done_cv);
        pthread_mutex_unlock(&g_done_mu);
        free(c->ops); free(c);
    }
    return NULL;
}
static void completion_start(void) { pthread_t t; if (pthread_create(&t, NULL, completion_main, NULL)) die("[vk] completion thread"); pthread_detach(t); }

// DM_VK_MEMLOG=<s>: every s seconds a line with the process's memory (/proc/self/status: resident anonymous / file / shared, swapped), the system's (/proc/meminfo, memory pressure
// from /proc/pressure/memory) and the pools made zero-copy (hardware buffers: never swapped) or copied. Android's low-memory killer ends a game with SIGKILL and nothing in its log;
// sampling from outside (dumpsys meminfo, run-as) stalled for half a minute under that pressure on an AYN Thor (Pokemon Pokopia, 2026-11-02), and these reads do not
static long kb_of(const char *buf, const char *key) { const char *p = strstr(buf, key); return p ? strtol(p + strlen(key), NULL, 10) : -1; }
static int slurp_small(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t k = fread(buf, 1, n - 1, f); fclose(f); buf[k] = 0; return 1;
}
static void *memlog_main(void *arg) {
    unsigned s = (unsigned)(uintptr_t)arg; char st[4096], mi[4096], ps[256];
    for (;;) {
        sleep(s);
        if (!slurp_small("/proc/self/status", st, sizeof st) || !slurp_small("/proc/meminfo", mi, sizeof mi)) continue;
        const char *some = slurp_small("/proc/pressure/memory", ps, sizeof ps) ? strstr(ps, "avg10=") : NULL;
        VKLOG("[mem] process: anon %ld MB, file %ld MB, shared %ld MB, swapped %ld MB | system: available %ld MB, free %ld MB, swap free %ld MB, pressure %.*s | pools: zero-copy %.0f MB (%.0f MB of it backed when first used, %.0f MB not used yet), copied %.0f MB, used %.0f MB",
              kb_of(st, "RssAnon:") >> 10, kb_of(st, "RssFile:") >> 10, kb_of(st, "RssShmem:") >> 10, kb_of(st, "VmSwap:") >> 10,
              kb_of(mi, "MemAvailable:") >> 10, kb_of(mi, "MemFree:") >> 10, kb_of(mi, "SwapFree:") >> 10, some ? 11 : 1, some ? some : "?", (g_zc_bytes + g_lazy_bytes) / 1048576.0, g_lazy_bytes / 1048576.0, (g_lazy_total - g_lazy_bytes) / 1048576.0, g_cp_bytes / 1048576.0, g_used_bytes / 1048576.0);
    }
    return NULL;
}
static void memlog_start(void) {
    const char *e = getenv("DM_VK_MEMLOG"); int s = e ? atoi(e) : 0; if (s <= 0) return;
    pthread_t t; if (!pthread_create(&t, NULL, memlog_main, (void *)(uintptr_t)s)) pthread_detach(t);
}
void vk_record_device_lost(const char *where, VkResult r) {
    int streak = __atomic_add_fetch(&V.dev_lost_streak, 1, __ATOMIC_ACQ_REL);
    vk_sub_diag_t d;
    pthread_mutex_lock(&g_diag_mu);
    d = V.last_sub_diag;
    pthread_mutex_unlock(&g_diag_mu);
    char pbuf[512];
    vk_diag_format_pipes(&d, pbuf, sizeof pbuf);
    uint64_t frame = d.frame ? d.frame : __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED);
    uint64_t fence = d.fence ? d.fence : __atomic_load_n(&V.sub_seq, __ATOMIC_RELAXED);
    uint64_t drops = __atomic_load_n(&g_pool_drops_total, __ATOMIC_RELAXED);
    VKLOG("[RED] [vk] DEVICE_LOST diagnostics (%s, VkResult=%d, streak=%d): GPU=%s vendor=%#x driver=%#x api=%u.%u | frame=%llu fence=%llu (done=%llu) | sub draws=%u passes=%u dispatches=%u | last_vs=%016llx last_fs=%016llx last_gs=%016llx last_cs=%016llx | new_pipes[%u]=%s | timeline=%d def_wait=%d pool_overlap=%d disable_msaa=%d strict_barriers=%d low_ram=%d | pool_drops=%llu cpf_kept=%llu cpf_hit=%llu copied=%.1f MB",
          where ? where : "?", (int)r, streak, V.props.deviceName, V.props.vendorID, V.props.driverVersion,
          VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion),
          (unsigned long long)frame, (unsigned long long)fence, (unsigned long long)vk_fence_done(),
          d.draws, d.passes, d.dispatches,
          (unsigned long long)d.last_vs, (unsigned long long)d.last_fs, (unsigned long long)d.last_gs, (unsigned long long)d.last_cs,
          d.n_new_pipes, pbuf, V.has_timeline, vk_deferred_wait(),
          vk_pool_overlap(), vk_disable_msaa(), vk_strict_barriers(), vk_low_ram_tier(),
          (unsigned long long)drops, (unsigned long long)g_cpf_kept, (unsigned long long)g_cpf_hit, g_cp_bytes / 1048576.0);
    fflush(stderr);
    const char *root = getenv("DM_ROOT");
    if (root && *root) {
        char path[1024];
        snprintf(path, sizeof path, "%s/last_launch_error.txt", root);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "VK_ERROR_DEVICE_LOST (%d, streak=%d) at %s\nGPU: %s (vendor=%#x, driver=%#x, Vulkan %u.%u)\nFrame: %llu (fence %llu, done %llu)\nSubmission: draws=%u, passes=%u, dispatches=%u\nShaders: vs=%016llx fs=%016llx gs=%016llx cs=%016llx\nNewPipelines[%u]: %s\nSettings: timeline=%d, def_wait=%d, pool_overlap=%d, disable_msaa=%d, strict_barriers=%d, low_ram_tier=%d\nPools: drops=%llu, cpf_kept=%llu, cpf_hit=%llu, copied=%.1f MiB\n",
                    (int)r, streak, where ? where : "?", V.props.deviceName, V.props.vendorID, V.props.driverVersion,
                    VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion),
                    (unsigned long long)frame, (unsigned long long)fence, (unsigned long long)vk_fence_done(),
                    d.draws, d.passes, d.dispatches,
                    (unsigned long long)d.last_vs, (unsigned long long)d.last_fs, (unsigned long long)d.last_gs, (unsigned long long)d.last_cs,
                    d.n_new_pipes, pbuf, V.has_timeline, vk_deferred_wait(),
                    vk_pool_overlap(), vk_disable_msaa(), vk_strict_barriers(), vk_low_ram_tier(),
                    (unsigned long long)drops, (unsigned long long)g_cpf_kept, (unsigned long long)g_cpf_hit, g_cp_bytes / 1048576.0);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/.gpu_device_lost", root);
        f = fopen(path, "w");
        if (f) {
            fprintf(f, "code=%d\nstreak=%d\ngpu=%s\nvendor=%#x\ndriver=%#x\nframe=%llu\nvs=%016llx\nfs=%016llx\n",
                    (int)r, streak, V.props.deviceName, V.props.vendorID, V.props.driverVersion,
                    (unsigned long long)frame, (unsigned long long)d.last_vs, (unsigned long long)d.last_fs);
            fclose(f);
        }
    }
    if (streak >= 2) {
        VKLOG("[RED] [vk] Consecutive VK_ERROR_DEVICE_LOST streak=%d (>=2): flushing logs and exiting cleanly (_exit 88) to prevent infinite TDR hang loop", streak);
        fflush(stderr);
        _exit(88);
    }
}
uint64_t mtl_sub_commit(void) {
    { void vk_prof_end(void); vk_prof_end(); }
    vksub_t *s = cur();
    vk_end_pass_land();
    // Stage small GPU->CPU readbacks into slot-owned HOST_CACHED s->rb_buf inside s->main before the submission
    // finishes, so completion_main() reads slot N-1's cached copy (~0.015 ms!) while GPU frame N's s->up updates p->buf.
    // Enabled for Mode 2 when vk_deferred_wait()==1 (unlocks >90% CPU+GPU overlap on modern GPUs!), Mode 1, and Mode 0.
    int ss_mode = vk_pool_safe_sync();
    int allow_rb_stage = (ss_mode >= 2) ? (vk_deferred_wait() && vk_safe1_rb_staged())
                                        : (vk_pool_overlap() && (ss_mode != 1 || vk_safe1_rb_staged()));
    if (g_shadow_pools && allow_rb_stage && s->readback_ranges && s->needs_cpu_rb && s->ngw > 0) {
        uint64_t total_rb = 0; int count_rb = 0;
        VkDeviceSize rb_align = V.props.limits.nonCoherentAtomSize > 64 ? V.props.limits.nonCoherentAtomSize : 64;
        uint64_t total_rb_aligned = 0;
        pthread_rwlock_rdlock(&g_pool_mu);
        for (int i = 0; i < g_npool; i++) {
            vkpool_t *p = &g_pool[i]; if (!p->shadow || !p->synced) continue;
            for (int k = 0; k < s->ngw; k++) {
                uint64_t lo = s->gw[k].lo > p->lo ? s->gw[k].lo : p->lo;
                uint64_t hi = s->gw[k].hi < p->hi ? s->gw[k].hi : p->hi;
                if (lo < hi) {
                    uint64_t rl = hi - lo;
                    total_rb += rl;
                    total_rb_aligned += (rl + rb_align - 1) & ~(rb_align - 1);
                    count_rb++;
                }
            }
        }
        if (count_rb > 0 && count_rb <= 256 && total_rb <= RB_STAGE_MAX_BYTES) {
            // Ensure slot-owned reusable CPU heap arena fits total_rb (zero per-frame malloc/free churn!).
            if (total_rb > s->rb_snap_cap) {
                size_t ncap = (size_t)total_rb;
                if (ncap < (256u << 10)) ncap = (256u << 10);
                uint8_t *nb = realloc(s->rb_snap, ncap);
                if (nb) { s->rb_snap = nb; s->rb_snap_cap = ncap; }
            }
            s->rb_snap_used = 0;
            s->rb_used = 0;
            // Pre-ensure s->rb_buf can hold all aligned ranges in one buffer without mid-loop reallocation.
            if (!s->rb_buf || total_rb_aligned > s->rb_cap) {
                VkBuffer dummy_b; VkDeviceSize dummy_o;
                (void)vk_rb_alloc(s, total_rb_aligned, &dummy_b, &dummy_o);
                s->rb_used = 0;
            }
            VkMemoryBarrier pre = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
            vkCmdPipelineBarrier(s->main, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0, NULL, 0, NULL);
            for (int i = 0; i < g_npool; i++) {
                vkpool_t *p = &g_pool[i]; if (!p->shadow || !p->synced) continue;
                for (int k = 0; k < s->ngw; k++) {
                    uint64_t lo = s->gw[k].lo > p->lo ? s->gw[k].lo : p->lo;
                    uint64_t hi = s->gw[k].hi < p->hi ? s->gw[k].hi : p->hi;
                    if (lo >= hi) continue;
                    uint64_t rlen = hi - lo;
                    VkBuffer sb; VkDeviceSize so;
                    uint8_t *stg = vk_rb_alloc(s, rlen, &sb, &so);
                    uint8_t *bsnap = NULL;
                    uint32_t cgen = 0;
                    if (p->baseline && p->baseline_complete) {
                        // Cached CPU heap snapshot from slot's reusable arena (zero per-frame malloc/free!).
                        if (s->rb_snap && s->rb_snap_used + (size_t)rlen <= s->rb_snap_cap) {
                            bsnap = s->rb_snap + s->rb_snap_used;
                            s->rb_snap_used += (size_t)rlen;
                            pthread_mutex_lock(&g_baseline_mu);
                            memcpy(bsnap, p->baseline + (lo - p->lo), (size_t)rlen);
                            cgen = pool_sum_cpu_gen(p, lo, hi);
                            pthread_mutex_unlock(&g_baseline_mu);
                        }
                    }
                    VkBufferCopy cp = {p->boff + (lo - p->lo), so, rlen};
                    vkCmdCopyBuffer(s->main, p->buf, sb, 1, &cp);
                    s->rb[s->nrb++] = (rb_stage_t){p->serial, lo, hi, stg, bsnap, cgen};
                }
            }
            VkMemoryBarrier post_b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(s->main, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &post_b, 0, NULL, 0, NULL);
            s->rb_staged = 1;
            __atomic_fetch_add(&g_rb_staged_cnt, 1, __ATOMIC_RELAXED);
        }
        pthread_rwlock_unlock(&g_pool_mu);
    }
    if (!s->rb_staged) pools_readback_barrier(s->main);
    if (g_tsq) vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)g_cur + 1);
    VkResult er = vkEndCommandBuffer(s->main); if (s->cb_err && er >= 0) er = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (er < 0) {   // the driver refused something recorded in it: logged with what the submission held, then it is dropped and the game goes on
        s->rb_snap_used = 0; s->rb_used = 0;
        s->rb_staged = 0; s->nrb = 0;
        extern uint32_t g_sub_ops[64]; char ops[600]; int o = 0;
        for (int i = 0; i < 64 && o < (int)sizeof ops - 16; i++) if (g_sub_ops[i]) o += snprintf(ops + o, sizeof ops - (size_t)o, " %d:%u", i, g_sub_ops[i]);
        static int n; if (n++ < 3) VKLOG("[RED] vkEndCommandBuffer failed (%d): submission dropped; record ops (op:count)%s", er, ops);
        vk_bisect_result(0);
        if (s->nmain_sent) {   // parts already sent may still run: nothing more is submitted (the submission below only signals its fence)
            s->nmain_sent = s->nmain_used; s->up_used = 0; s->cb_err = 0; s->no_ts = 1; vk_ubd_land_drop();
        } else {
            vkResetCommandPool(V.dev, s->pool, 0); VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            s->main = s->mains[0]; s->nmain_used = 1; s->cb_err = 0; s->no_ts = 1;   // no timestamps: the slot's queries still hold an older submission's
            if (s->nup_used > 1) { s->up = s->ups[0]; s->nup_used = 1; }
            VKCHK(vkBeginCommandBuffer(s->main, &bi)); g_cmd_gen++; s->up_used = 0; vk_ubd_land_drop();
            pools_readback_barrier(s->main);
            VKCHK(vkEndCommandBuffer(s->main));
        }
    }
    else vk_bisect_result(1);
    {
        extern uint32_t g_sub_ops[64];
        uint32_t draws = g_sub_ops[25] + g_sub_ops[26] + g_sub_ops[48];
        uint32_t dispatches = g_sub_ops[27];
        uint32_t passes = (g_cur >= 0 && g_cur < NSLOT) ? (uint32_t)g_npass[g_cur] : 0u;
        pthread_mutex_lock(&g_diag_mu);
        V.cur_sub_diag.frame = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED);
        V.cur_sub_diag.fence = g_fence + 1;
        if (draws > V.cur_sub_diag.draws) V.cur_sub_diag.draws = draws;
        if (dispatches > V.cur_sub_diag.dispatches) V.cur_sub_diag.dispatches = dispatches;
        V.cur_sub_diag.passes = passes;
        V.last_sub_diag = V.cur_sub_diag;
        memset(&V.cur_sub_diag, 0, sizeof V.cur_sub_diag);
        pthread_mutex_unlock(&g_diag_mu);
        memset(g_sub_ops, 0, sizeof g_sub_ops);
    }
    if (s->up_used) { vk_post_transfer_barrier(s->up); VKCHK(vkEndCommandBuffer(s->up)); }
    // Defensive flush for any non-coherent scratch blocks before vkQueueSubmit (0 cost on HOST_COHERENT GPUs).
    if (s->nblk > 0 && vkFlushMappedMemoryRanges) {
        VkMappedMemoryRange fl[16]; uint32_t nfl = 0;
        uint64_t atom = V.props.limits.nonCoherentAtomSize ? V.props.limits.nonCoherentAtomSize : 1;
        for (int i = 0; i < s->nblk; i++) {
            scratch_blk *b = &s->blk[i];
            if (!b->noncoherent || !b->used) continue;
            VkDeviceSize sz = ((b->used + atom - 1) / atom) * atom;
            if (sz > b->size) sz = VK_WHOLE_SIZE;
            fl[nfl++] = (VkMappedMemoryRange){VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, b->m, 0, sz};
            if (nfl == 16) { VKCHK(vkFlushMappedMemoryRanges(V.dev, nfl, fl)); nfl = 0; }
        }
        if (nfl) VKCHK(vkFlushMappedMemoryRanges(V.dev, nfl, fl));
    }
    uint64_t f = ++g_fence;
    __atomic_store_n(&V.sub_seq, f, __ATOMIC_RELEASE);
    VkCommandBuffer cbs[1 + MAIN_CBS]; uint32_t ncb = 0; if (s->up_used) cbs[ncb++] = s->up; for (int i = s->nmain_sent; i < s->nmain_used; i++) cbs[ncb++] = s->mains[i];
    // GPU-only hardware submission chain wait (zero CPU fence_wait, zero GPU bubble):
    // Wait at VK_PIPELINE_STAGE_ALL_COMMANDS_BIT (covers EVERY execution stage: VERTEX_INPUT, VERTEX_SHADER,
    // GEOMETRY_SHADER, FRAGMENT_SHADER, COMPUTE_SHADER, TRANSFER, DRAW_INDIRECT, COLOR_ATTACHMENT_OUTPUT, and
    // EARLY/LATE_FRAGMENT_TESTS) without blocking TOP_OF_PIPE CSF frontend parsing. Guarantees Frame N NEVER
    // reads or overwrites shared p->buf or render targets until Frame N-1 completes 100%, even when s->up_used==0!
    uint64_t wait_val = f > 1 ? f - 1 : 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &f};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, ncb, cbs, 1, &g_timeline};
    VkFence fe = VK_NULL_HANDLE;
    VkSemaphore wait_sem = VK_NULL_HANDLE, sig_sem = VK_NULL_HANDLE;
    if (V.has_timeline) {
        if (wait_val && g_shadow_pools && s->nmain_sent == 0) {
            ts.waitSemaphoreValueCount = 1; ts.pWaitSemaphoreValues = &wait_val;
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_timeline; si.pWaitDstStageMask = &wait_stage;
        }
    } else {   // the ring's fence for f: free once the completion thread has passed f - NFRING (already so: slot_recycle waited for f - NSLOT)
        if (f > NFRING) mtl_wait(f - NFRING, ~0ull);
        fe = g_fring[f % NFRING]; VKCHK(vkResetFences(V.dev, 1, &fe));
        si.pNext = NULL; si.signalSemaphoreCount = 0;
        if (g_chain_pending) {
            wait_sem = g_chain_pending; g_chain_pending = VK_NULL_HANDLE;
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = &wait_sem; si.pWaitDstStageMask = &wait_stage;
        }
        if (g_shadow_pools) {
            sig_sem = g_chain_sem[f % NFRING];
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sig_sem;
            g_chain_pending = sig_sem;
        }
    }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0); vk_wd_enter(VK_WD_VK_SUBMIT, V.last_sub_diag.last_vs, V.last_sub_diag.last_fs); pthread_mutex_lock(&g_vk_queue_mu); VkResult r = vkQueueSubmit(V.q, 1, &si, fe); pthread_mutex_unlock(&g_vk_queue_mu); vk_wd_leave(); clock_gettime(CLOCK_MONOTONIC, &t1);
    { uint64_t sd = (uint64_t)((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec)); g_ns_submit += sd; vk_sub_timing_add(4, sd);
      if (sd > 1500000000ull) VKLOG("[RED] [vk] GPU submit stall %.1f ms (vendor=%#x dev=%s) — possible TDR/hang!", sd / 1e6, V.props.vendorID, V.props.deviceName); }
    if (r) { if (r == VK_ERROR_DEVICE_LOST) vk_record_device_lost("mtl_sub_commit", r); void vk_crumbs_report(void); vk_crumbs_report(); die("[vk] vkQueueSubmit: %d (device lost?)", r); }
    s->fence = f;
    g_last_sub_needs_rb = (!s->readback_ranges) || (s->needs_cpu_rb && !s->rb_staged);
    g_prev_rb_slot = s->rb_staged ? g_cur : -1;
    cpend *c = calloc(1, sizeof *c); c->fence = f; c->frame = __atomic_load_n(&g_cur_frame, __ATOMIC_RELAXED); c->n = s->npost; c->rslot = g_cur; c->slot = s->no_ts ? -1 : g_cur; s->no_ts = 0;
    c->npass = g_npass[g_cur]; memcpy(c->pmeta, g_pmeta[g_cur], sizeof(pass_meta) * (size_t)c->npass);
    if (s->npost) { c->ops = malloc(s->npost * sizeof *c->ops); memcpy(c->ops, s->post, s->npost * sizeof *c->ops); }
    pthread_mutex_lock(&g_cmu); if (g_ct) g_ct->next = c; else g_ch = c; g_ct = c; pthread_cond_signal(&g_ccv); pthread_mutex_unlock(&g_cmu);
    s->npost = 0; g_cur = -1;
    pthread_mutex_unlock(&g_sub_lock);
    return f;
}
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600ull * 1000000000ull ? 3600ull * 1000000000ull : timeout_ns);
    dl.tv_sec += (time_t)(ns / 1000000000ull); dl.tv_nsec = (long)(ns % 1000000000ull);
    pthread_mutex_lock(&g_done_mu);
    while (g_done < fence) if (pthread_cond_timedwait(&g_done_cv, &g_done_mu, &dl)) break;
    int ok = g_done >= fence; pthread_mutex_unlock(&g_done_mu);
    return ok;
}
void mtl_finish(void) { mtl_wait(g_fence, ~0ull); }
void mtl_drain(void) { mtl_finish(); vk_pipeline_cache_save(); }
