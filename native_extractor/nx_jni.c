#define _FILE_OFFSET_BITS 64
#include "nx_extractor.h"
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define TAG "nxhybrid_extract"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static volatile int g_cancel_flag = 0;

typedef struct {
    JNIEnv *env;
    jobject cb_obj;
    jmethodID on_prog;
} jni_prog_ctx;

static void jni_progress_cb(void *user, const char *stage, uint64_t fdone, uint64_t ftotal,
                            uint64_t bdone, uint64_t btotal) {
    jni_prog_ctx *c = (jni_prog_ctx *)user;
    if (!c || !c->cb_obj || !c->on_prog) return;
    jstring jstage = (*c->env)->NewStringUTF(c->env, stage ? stage : "");
    (*c->env)->CallVoidMethod(c->env, c->cb_obj, c->on_prog, jstage,
                              (jlong)fdone, (jlong)ftotal, (jlong)bdone, (jlong)btotal);
    if ((*c->env)->ExceptionCheck(c->env)) (*c->env)->ExceptionClear(c->env);
    (*c->env)->DeleteLocalRef(c->env, jstage);
}

JNIEXPORT void JNICALL
Java_org_nx2a_doraemon_NativeBridge_nativeCancelExtract(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    g_cancel_flag = 1;
    ALOGI("cancel requested");
}

JNIEXPORT jlong JNICALL
Java_org_nx2a_doraemon_NativeBridge_nativeQueryFreeSpace(JNIEnv *env, jclass clazz, jstring jpath) {
    (void)clazz;
    if (!jpath) return 0;
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);
    uint64_t avail = path ? nx_query_free_space(path) : 0;
    if (path) (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (jlong)avail;
}

/*
 * Returns empty string "" on success, or human-readable error message on failure.
 * Caller passes raw POSIX fds obtained from ParcelFileDescriptor.detachFd() (or getFd()).
 */
JNIEXPORT jstring JNICALL
Java_org_nx2a_doraemon_NativeBridge_nativeExtractGame(
    JNIEnv *env, jclass clazz,
    jint keysFd, jint baseFd, jint updateFd, jintArray dlcFdsArr,
    jstring jPatchesDir, jstring jDestRoot, jlong minFreeBytes, jobject progressCb) {
    (void)clazz;
    g_cancel_flag = 0;

    const char *patches_dir = jPatchesDir ? (*env)->GetStringUTFChars(env, jPatchesDir, NULL) : NULL;
    const char *dest_root   = jDestRoot   ? (*env)->GetStringUTFChars(env, jDestRoot, NULL)   : NULL;

    int dlc_fds[32];
    int dlc_count = 0;
    if (dlcFdsArr) {
        jsize n = (*env)->GetArrayLength(env, dlcFdsArr);
        if (n > 32) n = 32;
        jint *elems = (*env)->GetIntArrayElements(env, dlcFdsArr, NULL);
        if (elems) {
            for (jsize i = 0; i < n; i++) dlc_fds[i] = (int)elems[i];
            dlc_count = (int)n;
            (*env)->ReleaseIntArrayElements(env, dlcFdsArr, elems, JNI_ABORT);
        }
    }

    jni_prog_ctx jctx = { .env = env, .cb_obj = progressCb, .on_prog = NULL };
    if (progressCb) {
        jclass c = (*env)->GetObjectClass(env, progressCb);
        if (c) {
            jctx.on_prog = (*env)->GetMethodID(env, c, "onProgress", "(Ljava/lang/String;JJJJ)V");
            if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); jctx.on_prog = NULL; }
            (*env)->DeleteLocalRef(env, c);
        }
    }

    nx_extract_params p = {
        .keys_fd = (int)keysFd,
        .base_fd = (int)baseFd,
        .update_fd = (int)updateFd,
        .dlc_fds = dlc_fds,
        .dlc_count = dlc_count,
        .patches_dir = patches_dir,
        .dest_root = dest_root,
        .min_free_bytes = minFreeBytes > 0 ? (uint64_t)minFreeBytes : NX_DEFAULT_MIN_FREE,
        .progress_cb = jctx.on_prog ? jni_progress_cb : NULL,
        .progress_user = &jctx,
        .cancel_flag = &g_cancel_flag
    };

    nx_extract_result res;
    char err[512] = "";
    ALOGI("starting native extraction (dlcs=%d, dest=%s)", dlc_count, dest_root ? dest_root : "(null)");
    int rc = nx_extract_hybrid_game(&p, &res, err, sizeof err);

    if (patches_dir) (*env)->ReleaseStringUTFChars(env, jPatchesDir, patches_dir);
    if (dest_root)   (*env)->ReleaseStringUTFChars(env, jDestRoot, dest_root);

    if (rc == 0) {
        ALOGI("extraction complete: %llu RomFS files, %d AOC", (unsigned long long)res.romfs_files, res.aoc_count);
        return (*env)->NewStringUTF(env, "");
    }
    ALOGE("extraction failed (%d): %s", rc, err);
    return (*env)->NewStringUTF(env, err[0] ? err : "unknown native extraction error");
}

/*
 * Real Vulkan Hardware Capability Probe (M3: integrated from nx2android-mali diagnostic-app).
 * Queries VkPhysicalDeviceProperties, BC1-BC7 sampled format features, ASTC_LDR, and vendor path.
 * Returns a JSON string for the Hybrid Launcher Diagnostics Panel.
 */
JNIEXPORT jstring JNICALL
Java_org_nx2a_doraemon_NativeBridge_nativeProbeVulkan(JNIEnv *env, jclass clazz) {
    (void)clazz;
    char out[1024];
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        snprintf(out, sizeof out, "{\"available\":false,\"error\":\"libvulkan.so unavailable\"}");
        return (*env)->NewStringUTF(env, out);
    }
    PFN_vkGetInstanceProcAddr p_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance p_ci = p_gipa ? (PFN_vkCreateInstance)p_gipa(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    if (!p_ci) {
        dlclose(lib);
        snprintf(out, sizeof out, "{\"available\":false,\"error\":\"vkCreateInstance missing\"}");
        return (*env)->NewStringUTF(env, out);
    }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "DoraemonProbe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    if (p_ci(&ici, NULL, &inst) != VK_SUCCESS || !inst) {
        dlclose(lib);
        snprintf(out, sizeof out, "{\"available\":false,\"error\":\"vkCreateInstance failed\"}");
        return (*env)->NewStringUTF(env, out);
    }
    PFN_vkEnumeratePhysicalDevices p_epd = (PFN_vkEnumeratePhysicalDevices)p_gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties p_prop = (PFN_vkGetPhysicalDeviceProperties)p_gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFeatures p_feat = (PFN_vkGetPhysicalDeviceFeatures)p_gipa(inst, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFormatProperties p_fmt = (PFN_vkGetPhysicalDeviceFormatProperties)p_gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkDestroyInstance p_di = (PFN_vkDestroyInstance)p_gipa(inst, "vkDestroyInstance");

    uint32_t ndev = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (p_epd && p_epd(inst, &ndev, &pd) >= 0 && ndev >= 1 && pd) {
        VkPhysicalDeviceProperties props; memset(&props, 0, sizeof props);
        VkPhysicalDeviceFeatures feats; memset(&feats, 0, sizeof feats);
        p_prop(pd, &props);
        p_feat(pd, &feats);
        VkFormatProperties fp_bc7; memset(&fp_bc7, 0, sizeof fp_bc7);
        if (p_fmt) p_fmt(pd, VK_FORMAT_BC7_UNORM_BLOCK, &fp_bc7);
        int bc7_sampled = (feats.textureCompressionBC && (fp_bc7.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ? 1 : 0;
        const char *vendor_tag =
            props.vendorID == 0x13B5 ? "ARM Mali (Unified M1-M4 Path: VQ_NO_ZEROCOPY + BC Cache)" :
            props.vendorID == 0x5143 ? "Qualcomm Adreno (Native BCn + ZeroCopy Path)" :
            props.vendorID == 0x144D ? "Samsung Xclipse" :
            props.vendorID == 0x1010 ? "Imagination PowerVR" : "Other GPU";
        /* Escape quotes in deviceName just in case */
        char safe_name[256];
        size_t j = 0;
        for (size_t i = 0; props.deviceName[i] && j + 2 < sizeof safe_name; i++) {
            char ch = props.deviceName[i];
            safe_name[j++] = (ch == '"' || ch == '\\') ? ' ' : ch;
        }
        safe_name[j] = '\0';
        snprintf(out, sizeof out,
            "{\"available\":true,\"gpu_name\":\"%s\",\"vendor_id\":\"0x%04X\",\"device_id\":\"0x%04X\","
            "\"vulkan_api\":\"%u.%u.%u\",\"driver_version\":\"0x%X\","
            "\"bc_feature\":%s,\"bc7_sampled\":%s,\"astc_ldr\":%s,\"mali_quirk\":%s,\"pipeline_path\":\"%s\"}",
            safe_name, props.vendorID, props.deviceID,
            VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
            props.driverVersion,
            feats.textureCompressionBC ? "true" : "false",
            bc7_sampled ? "true" : "false",
            feats.textureCompressionASTC_LDR ? "true" : "false",
            props.vendorID == 0x13B5 ? "true" : "false",
            vendor_tag);
    } else {
        snprintf(out, sizeof out, "{\"available\":false,\"error\":\"no Vulkan physical device\"}");
    }
    if (p_di) p_di(inst, NULL);
    dlclose(lib);
    return (*env)->NewStringUTF(env, out);
}
