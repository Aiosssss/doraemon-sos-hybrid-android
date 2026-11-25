// Android port: the Vulkan backend behind the replay interface of nvn_mtl.h (what nvn_mtl.m implements on Metal).
// Files: gfx_vk_core.c (device, memory pools, submissions, completion), gfx_vk_tex.c (formats, textures, samplers, registration tables),
// gfx_vk_shader.c (shader bundle, modules, layouts), gfx_vk_exec.c (command replay: passes, pipelines, descriptors, draws, copies; one file a section, gfx_vk_exec_<section>.inc), gfx_vk_present.c (window, swapchain),
// gfx_vk_quirks.c (the GPU driver and its quirk table).
// Version 1 is correct-first: whole-pipeline barriers between passes, full pipelines with a small dynamic state set, descriptor sets per draw.
#pragma once
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"

#define VKLOG(...) LOG("[vk] " __VA_ARGS__)
#define VKRED_ONCE(id, ...) do { static int once_##id; if (!once_##id++) LOG("[RED] [vk] " __VA_ARGS__); } while (0)
#define VKCHK(x) do { VkResult r_ = (x); if (r_ < 0) die("[vk] %s failed: %d", #x, r_); } while (0)

// ---- Vulkan entry points (loaded from libvulkan.so) ----
#define VK_FNS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateAndroidSurfaceKHR) X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR)
#define VK_DEV_FNS(X) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkBindImageMemory) X(vkGetBufferMemoryRequirements) \
    X(vkGetImageMemoryRequirements) X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) X(vkMapMemory) X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkResetCommandBuffer) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkCreateSemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDestroyFence) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdFillBuffer) X(vkCmdUpdateBuffer) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResolveImage) \
    X(vkCreateRenderPass) X(vkCreateRenderPass2) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetDepthBias) X(vkCmdSetLineWidth) X(vkCmdDraw) X(vkCmdPushConstants) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateComputePipelines) X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) X(vkCreateDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreateBufferView) X(vkDestroyBufferView) X(vkGetAndroidHardwareBufferPropertiesANDROID) \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) \
    X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT) X(vkCmdDrawIndirectByteCountEXT) X(vkCmdDrawIndirectCount) X(vkCmdDrawIndexedIndirectCount) \
    X(vkGetRefreshCycleDurationGOOGLE) X(vkGetPastPresentationTimingGOOGLE)
#define VK_DECL(f) extern PFN_##f f;
VK_FNS(VK_DECL) VK_DEV_FNS(VK_DECL)

// ---- device ----
typedef struct {
    uint64_t frame, fence;
    uint32_t draws, passes, dispatches;
    uint64_t last_vs, last_fs, last_gs, last_cs;
    struct { uint64_t vs, fs, gs; } new_pipes[8];
    uint32_t n_new_pipes;
} vk_sub_diag_t;
typedef struct {
    VkInstance inst; VkPhysicalDevice pd; VkDevice dev; VkQueue q; uint32_t qf;
    VkPhysicalDeviceProperties props; VkPhysicalDeviceMemoryProperties mem;
    VkPipelineCache pcache;
    int has_bc, has_astc, has_gs, has_gs_pointsize, has_depth_clip_ctl, has_depth_bias_clamp, has_custom_border, has_robust2, has_xfb, has_imageless, has_dtiming, has_dynrender, has_timeline, has_draw_count, robust_buf;
    int use_classic_fb;    // Vulkan 1.0/1.1 fallback (e.g. Adreno 650/618 1.1.128 without dynamicRendering or imagelessFramebuffer): classic vkCreateRenderPass + cached VkFramebuffer
    int has_divisor;       // vertexAttributeInstanceRateDivisor (VK_EXT_vertex_attribute_divisor or the KHR one): streams stepping per N instances (build_pipeline)
    int has_nonzero_first_inst; // VK_KHR_vertex_attribute_divisor supportsNonZeroFirstInstance (1 on EXT or when KHR reports true; 0 -> emulate via per-instance VBO offsets)
    float max_aniso;
    // subgroups (vk_init): VkPhysicalDeviceSubgroupProperties' size, stages, operations and quadOperationsInAllStages; VkPhysicalDeviceSubgroupSizeControlProperties'
    // range and maxComputeWorkgroupSubgroups; DM_VK_SUBGROUP_PIN's required size (sg_pin) and the stages it is given to (vk_sg_pin)
    uint32_t sg_size, sg_stages, sg_ops, sg_quad_all, sg_min, sg_max, sg_maxwgs, sg_pin_stages; VkPipelineShaderStageRequiredSubgroupSizeCreateInfo sg_pin;
    VkDeviceSize tbuf_align_uniform, tbuf_align_storage;
    int tbuf_single_uniform, tbuf_single_storage;
    // Submission sequencing, non-blocking pool retire tracking, and last-submitted diagnostic snapshot (hybrid36):
    uint64_t sub_seq, completed_seq;
    int dev_lost_streak;
    vk_sub_diag_t cur_sub_diag, last_sub_diag;
} vkdev_t;
extern vkdev_t V;
void vk_diag_note_draw(uint64_t vs, uint64_t fs, uint64_t gs);
void vk_diag_note_dispatch(uint64_t cs);
void vk_diag_note_new_pipe(uint64_t vs, uint64_t fs, uint64_t gs);
void vk_pool_retire_drain(void);
const void *vk_sg_pin(VkShaderStageFlagBits stage, const uint32_t *local);   // DM_VK_SUBGROUP_PIN: a game pipeline stage's pNext, NULL when the driver picks its size (local: a compute shader's workgroup size)
// gfx_vk_subgroup.c (no Vulkan): the module as subgroups of one invocation when it uses a subgroup class not in ops (its stage's; 0 when the device has none there).
// NULL when there is nothing the stage lacks (why empty) or it uses something not covered (why names it); the module then stays as it is
uint32_t *spv_one_lane(const uint32_t *w, uint32_t nw, uint32_t ops, uint32_t *onw, char *why, size_t nwhy);
uint32_t *spv_gs_split(const uint32_t *w, uint32_t nw, int mode, uint32_t *onw, char *why, size_t nwhy);
uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid);
void vk_init(void);   // once, from mtl_init or the first use

// ---- memory pools (gfx_vk_core.c): the guest's pool memory is GPU memory (AHardwareBuffer mapped over it) ----
typedef struct { uint64_t lo, hi; VkBuffer buf; VkDeviceMemory mem; void *ahb; int zero_copy, noncoherent; uint8_t *shadow, *baseline, *hot, *gpu_blk, *no_hot; uint32_t *cpu_blk_gen; uint64_t serial, base, end; int flags, synced, baseline_complete, baseline_fresh, cached, cpf_reused, gpu_wrote_ever; uint32_t sync_gen; uint64_t stamp, stamp0; const void *span; uint64_t boff; void *slab; uint32_t slot; int used, lazy; uint32_t lazy_n; uint64_t lazy_at, left; int has_inner; } vkpool_t;   // used: vk_pool_of has found it (DM_VK_MEMLOG); lazy: 1, no buffer until the GPU first uses it (pool_back), 2, that failed for good; lazy_n, lazy_at: failed tries, no try before (pool_ns); left: a length from lo that covers the pages a failed try left on a buffer it dropped (copied back when the pool is removed, unless a later try covered them); has_inner: another (smaller) pool was made over memory inside it, so an address in it may be that pool's (sticky; pool_find answers it only from the locked search); boff: [lo, hi) is at this offset of buf (a slab slot; 0 otherwise); serial: unique per pool made (caches of views over a pool's buffer key on it); [base, end): the game's pool, of which [lo, hi) is one chunk (ZC_CHUNK); flags: the NVN pool's (vk_pool_set_flags); baseline: last synchronized GPU-copy contents; baseline_complete: refreshed by a full completion merge, consumed by the next sync; baseline_fresh: newly calloc()'d all-zero baseline (sparse init skips zero-block memset); hot: per-64KB adaptive dirty cooldown; gpu_blk: per-64KB oversized GPU-written CPU_UNCACHED marker; no_hot: per-64KB permanent ban from hot_blocks (GPU-written or CPU-backed VkImage overlap); cpu_blk_gen: per-64KB monotonic CPU-write generation; synced, stamp, stamp0, span: a copied pool's incremental sync
vkpool_t *vk_pool_of(uint64_t addr, uint64_t len);   // the pool containing [addr, addr+len), NULL if none
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);   // GPU buffer + offset of a pool address range
void vk_pool_ban_hot(const void *addr, size_t len);   // ban 64KB blocks overlapping [addr, addr+len) from hot_blocks (packaged/linear VkImage storage)
// Pools that could not be made zero-copy keep a GPU copy refreshed before each submission (vk_pools_sync, called from mtl_sub_begin)
void vk_pools_sync(VkCommandBuffer cb);
int vk_pool_is_copied(const void *addr, size_t len);
int vk_disable_msaa(void);   // DM_VK_DISABLE_MSAA: 1 disables MSAA (experimental; default 0 native MSAA on all GPUs)
int vk_no_a2c(void);         // DM_VK_NO_A2C: 1 disables alphaToCoverageEnable (default on Mali via VQ_NO_A2C; 0 on Adreno)
int vk_pool_safe_sync(void);   // DM_VK_POOL_SAFE_SYNC: 2 (default) Serial Safe Sync + Safe Hot16 + Deferred Wait; 1 = Legacy Staged; 0 = Legacy
int vk_deferred_wait(void);    // DM_VK_DEFERRED_WAIT (default 1): safe CPU+GPU pipelining via per-slot s->up staging in safe_sync=2
int vk_safe1_hot_blocks(void);   // DM_VK_SAFE1_HOT_BLOCKS (default 1): 0 disables hot blocks in safe_sync=1 while keeping overlap=1
int vk_safe1_rb_staged(void);    // DM_VK_SAFE1_RB_STAGED (default 1): 0 disables async rb_staged in safe_sync=1 while keeping overlap=1
int vk_safe1_diff1k(void);       // DM_VK_SAFE1_DIFF1K (default 1): 0 uploads full dirty 64KB blocks via s->up + stamp0 while keeping overlap=1
int vk_inline_resolve(void);     // DM_VK_INLINE_RESOLVE (default 1): fuse post-MSAA Downsample into inline TBDR pResolveAttachments + STORE_OP_DONT_CARE
void vk_pipeline_cache_save_async(void);   // background VkPipelineCache save (called from onPause / burst)
int vk_pool_overlap(void);     // DM_VK_POOL_OVERLAP: 1 (default on Mali) overlaps CPU frame N with GPU frame N-1 via staged s->up DMA when prior sub needs no CPU readback
int vk_ubd_copyback(void);     // DM_VK_UBD_COPYBACK: 1 (default) sync-mirrors vk_ubd_land to copied-pool guest RAM + baseline (keeps overlap=1); 2 = legacy async pool_merge_back; 0 = off
int vk_dump_3d_frame(void);    // DM_VK_DUMP_3D_FRAME: 1 dumps exactly 1 3D gameplay frame render graph (dispatches/passes/post-process/instanced draws)
int vk_strict_barriers(void);   // DM_VK_STRICT_BARRIERS: 1 keeps VkRenderPass2, disables clear folding & split submits, adds post-pass barriers
int vk_emu_first_instance(void);   // DM_VK_EMU_FIRST_INSTANCE: 1 (default) emulates non-zero firstInstance when driver lacks it; 0 disables for A/B
void vk_dump_3d_frame_present(void);   // gfx_vk_exec.c: called from mtl_present() to arm/finish the 1-shot 3D frame dump
void vk_gpu_wrote_ssbo(uint64_t a, uint64_t n);   // gfx_vk_core.c: GPU shader SSBO write (CPU readback only for CPU_CACHED pools)
int vk_gpu_pending(uint64_t a, uint64_t n);       // gfx_vk_core.c: 1 if an incomplete GPU write overlaps [a, a+n)
void vk_sub_timing_add(int idx, uint64_t ns);   // 0=pool_upload, 1=pool_readback, 2=dirty_rearm, 3=replay, 4=vk_submit, 5=fence_wait, 6=present
void vk_sub_timing_snapshot(uint64_t out[7]);
int vk_mali_legacy_compiler(void);   // ARM Mali Vulkan 1.1 or driver < 40.0 (e.g. Mali-G57 r32p1): serialize pipeline compile & skip bg warm
int vk_mali_legacy_r32(void);        // ARM Mali driver <= 32.1.0 (0x08001000, e.g. Mali-G76/G57/G52 r32p1/r26p0): 100% non-blocking async pipe + bad-shader guard
int vk_mali_bifrost_legacy(void);    // ARM Mali Bifrost driver < 32.0 (e.g. Mali-G52 r26p0 = 0x06800000): disable order_keep & force pass barriers
int vk_mali_old_driver(void);        // ARM Mali driver < 49.0 (e.g. Mali-G615 44.1.0, G57 32.1.0, G52 26.0.0)
int vk_mali_low_end(void);           // ARM Mali 2-core / entry tier (Mali-G52/G57/G68, Adreno 610..642, subgroups <= 8, or driver < 40.0)
int vk_gpu_ultra_low_end(void);      // Ultra-low-end GPUs (Mali-G52/G51/G31, PowerVR, Adreno 610/612/5xx and below) -> default 30 FPS Stabil in auto mode
int vk_gpu_tier(void);               // 0=Flagship/Upper-Mid, 1=Mid-Range (0.75x), 2=Low-End (0.67x), 3=Ultra Low-End (0.55x)
int vk_low_cost_pp(void);            // DM_VK_LOW_COST_PP: auto (1 on Tier 2-3 low-end) | 1 | 0
int vk_samsung_safe(void);           // DM_VK_SAMSUNG_SAFE: auto (1 on vendorID 0x144D Xclipse) | 1 | 0
int vk_shadow_rate(void);            // DM_VK_SHADOW_RATE: 1=60Hz full, 2=30Hz half-rate (auto on Tier >=2), 0=off (Turbo)
float vk_shadow_scale(void);         // DM_VK_SHADOW_SCALE: auto (0.5 on Tier 1-3, 1.0 Flagship) | 0.5 | 1.0
void vk_classic_fb_drop_view(VkImageView v);   // invalidate & defer-free cached classic VkFramebuffers referencing v
enum {
    VK_WD_IDLE = 0, VK_WD_ACQUIRE_IMAGE, VK_WD_POOL_UPLOAD, VK_WD_REPLAY_DRAW,
    VK_WD_PIPELINE_COMPILE, VK_WD_VK_SUBMIT, VK_WD_FENCE_WAIT, VK_WD_PRESENT, VK_WD_POOL_READBACK
};
void vk_wd_enter(int phase, uint64_t vs, uint64_t fs);
void vk_wd_leave(void);
void vk_bad_pipes_init(void);
int vk_bad_pipe_check(uint64_t vs, uint64_t fs);
void vk_bad_pipe_add(uint64_t vs, uint64_t fs, const char *reason);
void vk_pipe_worker_begin(uint64_t vs, uint64_t fs);
void vk_pipe_worker_end(void);
int vk_low_ram_tier(void);           // 0 = >=5.5 GiB, 1 = 4 GB Low-RAM (Anti-LMK), 2 = <=3.5 GB Ultra Low-RAM (DM_LOW_RAM=0|1|2 override)
float vk_render_scale(void);         // DM_VK_RENDER_SCALE: 1.0 on high-end >=6 GB, 0.67 on tier 1 / Mali-G52..G68, 0.55 on tier 2
int vk_tex_downscale(void);          // DM_VK_TEX_DOWNSCALE: 1 on high-end >=6 GB, 2 on tier 1 / Mali-G52..G68, 4 on tier 2
void vk_pipe_compile_lock(void);     // serializes driver pipeline compiler calls across replay + async worker
int vk_pipe_compile_trylock(void);   // non-blocking trylock (returns 1 if acquired, 0 if busy)
void vk_pipe_compile_unlock(void);
void vk_present_extent(int *w_out, int *h_out);   // active swapchain window extent (for 1:1 touch scaling)
void vk_record_device_lost(const char *where, VkResult r);   // logs full context + writes last_launch_error.txt & .gpu_device_lost recovery marker

// ---- submissions (gfx_vk_core.c) ----
typedef struct vksub vksub_t;
VkCommandBuffer vk_cmd(void);          // the current submission's main command buffer (recording)
int vk_readback_ranges(void);
void vk_readback_unknown(void);   // force full cached-pool merge for this submission
int vk_sub_open(void);                 // a submission is being recorded
VkCommandBuffer vk_upload_cmd(void);   // the current submission's upload command buffer (runs before the main one)
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);   // host-visible memory that lives until this submission completes
void vk_defer_free_buffer(VkBuffer b, VkDeviceMemory m);   // destroyed once the GPU has passed the current submission
void vk_defer_free_image(VkImage i, VkDeviceMemory m);
VkDeviceMemory vk_image_memory(const VkMemoryRequirements *req, uint32_t type, VkDeviceSize *off, void **sub);   // NULL on failure; *sub: a slot of a shared block, or NULL (memory of its own)
void vk_defer_free_image_sub(VkImage i, void *sub);
void vk_defer_free_view(VkImageView v);
void vk_defer_free_bview(VkBufferView v);   // a texel buffer view, destroyed when this submission completes
void vk_defer_free_framebuffer(VkFramebuffer fb);   // temporary framebuffer (e.g. MSAA clear pass), destroyed when this submission completes
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout l);    // from this submission's descriptor pools
int vk_sub_slot_index(void);     // current submission slot index (0..NSLOT-1), or -1
uint64_t vk_sub_dp_epoch(void);  // descriptor-pool epoch of current slot (only advances on vkResetDescriptorPool)
uint64_t vk_fence_done(void);   // the highest submission fence the GPU has completed
VkSemaphore vk_timeline_semaphore(void);   // hardware timeline semaphore signaled at each submission fence (VK_NULL_HANDLE if !V.has_timeline)

// ---- formats and textures (gfx_vk_tex.c) ----
// dec: a BC format the device lacks, decoded on the CPU (gfx_vk_tex.c bc_decode) into vk, dbpp bytes a texel; bw, bh and bpb stay the guest's blocks
typedef struct { VkFormat vk; VkComponentMapping swz; uint8_t kind; uint8_t depth, stencil, compressed, is_int, is_sint, bw, bh, bpb, dec, dbpp; } vkfmt_t;
const vkfmt_t *vk_fmt(int nvn_format);   // NULL when the format has no Vulkan counterpart on this device
void vk_bc_decode(const vkfmt_t *f, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint32_t z, uint8_t *dst);   // (f->dec) gfx_vk_tex.c bc_decode
typedef struct vkimg vkimg_t;
static inline int vk_img_valid(const vkimg_t *im) { return im != NULL && (uintptr_t)im > 4096u; }
struct vkimg {
    VkImageUsageFlags usage; VkImageCreateFlags cflags;   // as created (an imageless framebuffer names them)
    VkImage img; VkDeviceMemory mem; void *sub; VkFormat fmt; VkImageViewType vtype; VkImageAspectFlags aspect;
    uint32_t w, h, d, layers, levels, samples; int target, nvn_format;
    uint32_t orig_w, orig_h; uint8_t ds_shift, no_scale, is_shadow, shadow_rendered_once, shadow_cleared_off, ever_sampled, pp_zeroed, depth_ever_loaded_across_sub; uint32_t depth_clear_frames; uint64_t depth_last_sub, vram_bytes;   // guest dims before render scale / BC downscale shift / unscale lock / shadow RT scale+rate tracking / ever sampled or copy-read / low-cost PP zeroed / proven 1x 3D depth clear tracking / actual VRAM accounting
    VkImageLayout layout;           // version 1 keeps every image in GENERAL after its first use
    uint8_t family_formats, family_closed;   // logical family members already shared; packaged metadata prevents adding new formats
    int packaged;                   // filled from guest memory (deswizzled upload), re-uploaded when its pages change
    uint64_t pkg_stamp; const void *pkg_span; uint64_t pkg_seen;
    uint64_t pkg_hash;              // hash of the guest bytes it was last uploaded from: pages written around it with its own bytes unchanged keep what the GPU wrote (gfx_vk_tex.c vk_tex)
    const uint8_t *pkg_mem; size_t pkg_len; uint64_t pkg_used; int pkg_released;   // its storage, the present it was last drawn at, protection given up (vk_tex_sweep)
    uint64_t pkg_vgen; int pkg_sparse;   // a texture of a sparse virtual pool (read through its page table, nvn_sparse_copy): the mapping generation at its upload
    uint64_t used;                  // the present it was last looked up at (vk_tex: evict_overlaps)
    int looked;                     // looked up inside a submission once (vk_tex: its first use, when its content is decided; a lookup outside one records nothing)
    uint64_t rt_last, rt_frames;     // as an attachment: 1 + the present it was last drawn in, and in how many presents in a row up to it (gfx_vk_exec.c async_pipe)
    uint64_t fdz_frame;             // depth images: 1 + the present at which a fragment shader that replaces depth last wrote it (gfx_vk_exec.c fdz)
    int gpu_w;                      // the GPU wrote it: an attachment, a storage image, a copy or clear destination (gfx_vk_exec.c); lin_copy keeps that until the CPU writes its storage
    uint64_t lin_stamp; const void *lin_span; const uint8_t *lin_mem; size_t lin_len;   // lin_copy: the storage watched since the GPU wrote it
    uint64_t lin_gen;               // linear textures: the upload command buffer (g_up_gen) that last copied it from its pool (vk_tex: lin_copy)
    uint64_t depth_sub;   // the submission (gfx_vk_exec.c g_set_epoch) whose passes last used it as their depth attachment
    uint64_t depth_cut;   // and the command buffer within it (gfx_vk_exec.c g_cuts)
    int fixed_fmt;   // created with a format list (DM_VK_UBWC): views only in its format or its sRGB twin
    VkFormat fmt2;   // with fixed_fmt: the other format of its list (its sRGB twin), VK_FORMAT_UNDEFINED when the list is its format alone
    VkFormat fmt_cls[2];   // with fixed_fmt, an image flag and an uncompressed colour format: the uint and the sint format of its texel size, in its list (storage views: vk_tex_view_as)
    VkImageView full;              // a view of the whole image as created
    VkBufferView tbuf;              // buffer textures: a texel buffer view of the pool
    uint64_t last_use;              // submission fence of the last use
    vkimg_t *bc_draw;               // a block-compressed image drawn into through an uncompressed view: the image in that format, its size in blocks, that the passes
                                    // draw into in its place (gfx_vk_tex.c vk_tex_view_as), copied into it at each pass's end (gfx_vk_exec.c bc_draw_back)
    vkimg_t *bc_of;                 // such a stand-in: the block-compressed image it is drawn for
    uint32_t bc_lv, bc_ly, bc_nly;  // and the level and the layers (depth slices) of its last attachment view
    int bc_tried;                   // a block-compressed image: a stand-in was tried for it (one attempt: one log line, no image made at every pass)
    uint64_t pool; int64_t off, size; int flags;   // its storage (pool, offset, layout size) and NVN texture flags: images over the same memory find each other
    struct vkimg *ovl[8]; int novl;   // the other images over some of the same memory (a texture of another description over it: on the Switch it sees these texels)
    uint64_t wseq, rseen;             // when it was last rendered to (gfx_vk_tex.c g_wseq); the newest overlapping write already reported
    struct vkimg *fb_copy;            // lazy same-size snapshot image when a pass samples its own active colour attachment (self-feedback)
};
vkimg_t *vk_tex(const tex_t *t);                                 // the image of a texture object (created, uploaded or refreshed as needed)
int vk_tex_unscale_cb(VkCommandBuffer cb, vkimg_t *im, const tex_t *t);   // recreate render-scaled image at 1:1 orig_w x orig_h, preserving GPU-rendered pixels via blit when cb != NULL
int vk_tex_unscale(vkimg_t *im, const tex_t *t);                 // recreate a render-scaled image at 1:1 native orig_w x orig_h (for CopyBufferToTexture / pkg_upload)
vkimg_t *vk_tex_feedback_snapshot(VkCommandBuffer cb, vkimg_t *im);   // copy im -> im->fb_copy for Vulkan-spec-valid self-feedback sampling
VkImageView vk_tex_view_of_img(vkimg_t *im, const tex_t *t, const view_t *vw, int dim, int arrayed);
void vk_lrz_forget(vkimg_t *im);                                 // an image is being dropped: the pass code forgets it (gfx_vk_exec.c)
int vk_window_bottom_up(void);                                   // the game draws in lower-left origin mode: its window image is stored bottom-up (gfx_vk_exec.c)
VkImageView vk_tex_view(const tex_t *t, const view_t *v, int dim, int arrayed, int as_attachment, vkimg_t **img_out);   // a view for sampling / storage / rendering
VkImageView vk_tex_view_as(const tex_t *t, const view_t *v, int dim, int arrayed, int as_attachment, vkimg_t **img_out, VkFormat want);   // the same, in the format want (a storage view of the shader's integer class; 0: the texture's own)
VkFormat vk_int_fmt(uint32_t bytes, int sint);   // the uint (or sint) format of a texel size (1, 2, 4, 8, 16 bytes) that can be stored into; VK_FORMAT_UNDEFINED when none (gfx_vk_tex.c)
void vk_watch(const vkimg_t *im, const char *what);   // DM_VK_WATCH (gfx_vk_exec.c)
vkimg_t *vk_tex_by_id(int tid, const view_t **view_out, const tex_t **tex_out);   // registered texture ID → image (NULL if none)
VkSampler vk_samp_by_id(int sid);
extern __thread VkFormat vk_view_fmt; extern __thread uint32_t vk_view_layers; extern __thread VkImageUsageFlags vk_view_usage;   // the format, layer count and inherited usage of the view vk_tex_view last returned on this thread
VkImageView vk_dummy_view(int dim, int arrayed, int ms, int storage, int is_int);
VkSampler vk_dummy_sampler(void);
VkBufferView vk_dummy_tbuf(int storage);
void vk_end_pass_land(void);   // vk_end_pass, then the deferred uniform updates land (before copies, dispatches, the submission's end)
void vk_img_barrier_all(VkCommandBuffer cb);   // version 1: one global memory barrier (all images stay in GENERAL)
void vk_compute_barrier(VkCommandBuffer cb);   // explicit COMPUTE_SHADER write -> graphics/vertex/indirect/transfer read+write barrier
void vk_tex_prepare(VkCommandBuffer cb, vkimg_t *im);   // first use: transition from UNDEFINED to GENERAL (and upload a packaged texture)
void vk_tex_mark_written(vkimg_t *im);                  // advance alias wseq and mark gpu_w on storage-image / copy / clear writes
void vk_clear_msaa_subresource(VkCommandBuffer cb, vkimg_t *im, uint32_t mip, uint32_t base_layer, uint32_t layer_count, const VkClearColorValue *cc, const VkClearDepthStencilValue *ds);

// ---- shaders (gfx_vk_shader.c) ----
enum { RK_SUPPORT, RK_CBUF, RK_C1, RK_DRV, RK_SBUF, RK_UBO_PTR, RK_SBUF_UBO, RK_TEX, RK_TBUF, RK_IMG, RK_IMG_TBUF, RK_TEX_C2A, RK_TEX_UBO, RK_TBUF_UBO, RK_IMG_UBO, RK_TEXD, RK_SAMPD, RK_UNKNOWN = 255 };
enum { RD_UBO, RD_SSBO, RD_SAMPLED, RD_STORAGE, RD_TBUF, RD_STORAGE_TBUF, RD_IMAGE, RD_SAMPLER };
enum { RS_NONE, RS_TEX, RS_SLOT, RS_ID, RS_UBO };
typedef struct __attribute__((packed)) { uint8_t kind, desc, dim, flags; uint16_t binding, count; int16_t reg; uint8_t samp, fmt; int32_t off, samp_arg; } dmsv_bind;
_Static_assert(sizeof(dmsv_bind) == 20, "dmsv_bind");
typedef struct {
    uint64_t key; int stage;          // NVN stage: 0 vertex 1 fragment 2 geometry 3 tess control 4 tess eval 5 compute
    VkShaderModule mod;
    VkDescriptorSetLayout dsl; int set;   // the set this stage's bindings were moved to (vertex 0, fragment 1, geometry 2, compute 0)
    int nbind; const dmsv_bind *b;
    const uint8_t *c1; uint32_t c1_len; VkBuffer c1_buf; VkDeviceSize c1_off;
    uint32_t local[3];
    uint8_t dyn[96]; int ndyn;          // bindings of uniform buffers bound with a dynamic offset (gfx_vk_exec.c stage_set), and their count
    int xfb; uint32_t xfb_stride[4];    // a vertex shader that captures transform feedback (its SPIR-V has the Xfb execution mode), and each buffer's stride
    uint32_t *spv, nspv;                // its SPIR-V as the module was made from it, for the variants below
    struct { uint64_t um, im; uint32_t xo; int fd; VkShaderModule mod; uint32_t vi; } var[8]; int nvar;   // variants whose sampled bindings (bit = index in b) of an integer view read uint (um) / int (im) texels,
                                                                                    // that declare the vec4 outputs xo (bit = location) the next stage reads, and (fd) that write gl_FragDepth = gl_FragCoord.z
    int depth_repl;                     // a fragment shader that writes gl_FragDepth (DepthReplacing / DepthGreater / DepthLess)
    uint8_t has_discard;                // a fragment shader containing OpKill / OpDemoteToHelperInvocation / OpTerminateInvocation
    uint32_t in_locs, out_locs;         // the vec4 (or per-vertex vec4) inputs and outputs it declares, by location
    int setmemo;                        // gfx_vk_exec.c stage_set: 1 its set may be memoized per stage, 2 not (it reads handles or addresses from uniform buffer memory), 0 not known yet
    int wmem;                           // gfx_vk_exec.c shader_wmem: 1 it writes memory (stores to storage buffers or images, atomics), 2 not, 0 not known yet
    uint64_t wmem_bind_mask;            // bit b->binding set if SPIR-V Binding (<64) is written by this shader (~0ull if unknown/indirect)
    int gs_split; VkShaderModule gs_mod[2]; // 1: effects/raster modules ready, 2: cannot safely separate, 0: not checked
} vkshader_t;
vkshader_t *vk_shader(uint64_t key, int stage);   // NULL when the bundle has no such shader
vkshader_t *vk_shader_gs_topo(vkshader_t *gs, VkPrimitiveTopology topo);   // the geometry shader translated for the draw's input primitive (shader_vk.py gs_variant_key); gs when there is none
VkShaderModule vk_shader_module(vkshader_t *s, uint64_t um, uint64_t im, uint32_t xo, int fd, uint32_t vi);   // the module, or its variant (integer views; extra outputs; fragment depth written)
int vk_shader_gs_split_ready(vkshader_t *s); // both modules exist before either pass may be recorded
void vk_shaders_open(const char *path);
VkBuffer vk_support_buffer(void);
VkBuffer vk_zero_buffer(void);
VkBuffer vk_default_attribute_buffer(void);

// ---- replay (gfx_vk_exec.c) ----
void vk_exec_reset(void);
void vk_exec_resume(void);   // new internal segment, retaining conditional state
extern int g_vk_cpu_ready;   // live diagnostic switch; default on
void vk_cpu_ready(uint64_t addr, uint64_t size);
int vk_ubd_overlaps(uint64_t addr, uint64_t size);
void vk_sub_resume(void);   // per submission
void vk_end_pass(void);     // close an open render pass

// ---- presentation (gfx_vk_present.c) ----
void vk_present_init(void);
uint64_t vk_present(const tex_t *t, double period, const int crop[4]);
enum { PT_DISPATCH = 0xd15 };   // vk_pass_ts: a dispatch timed as a pass (its format field)


// Mark driver cache updates, including same-sized payload changes.
void vk_pipeline_cache_changed(void);

#include "gfx_vk_quirks.h"   // the GPU driver quirk table (vk_quirk)
