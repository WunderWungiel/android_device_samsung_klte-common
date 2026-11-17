/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Run the process with target SDK 22, the way TARGET_PROCESS_SDK_VERSION_OVERRIDE
 * used to (LineageOS bionic dropped it).
 *
 * mm-qcamera-daemon dlopen()s libmmcamera_faceproc.so, which has text
 * relocations. On 32-bit ARM the linker still accepts those, but only while the
 * process targets SDK < 23. Without the faceproc module the daemon cannot link
 * any preview stream, so the camera does not work at all.
 *
 * Linked into the blob as DT_NEEDED (blob_fixup): the constructor runs before
 * main(), well before the daemon loads its modules.
 */

#define LOG_TAG "libshim_sdk22"

#include <dlfcn.h>
#include <log/log.h>

#define SHIM_TARGET_SDK 22

typedef void (*set_target_sdk_t)(int);

__attribute__((constructor)) static void shim_set_target_sdk(void)
{
    set_target_sdk_t set_sdk = NULL;

    void *handle = dlopen("libdl_android.so", RTLD_NOW);
    if (handle)
        set_sdk = (set_target_sdk_t)dlsym(handle, "android_set_application_target_sdk_version");
    if (!set_sdk)
        set_sdk = (set_target_sdk_t)dlsym(RTLD_DEFAULT,
                "__loader_android_set_application_target_sdk_version");

    if (!set_sdk) {
        ALOGE("cannot find the target SDK setter (%s)", dlerror());
        return;
    }

    set_sdk(SHIM_TARGET_SDK);
    ALOGI("target SDK set to %d", SHIM_TARGET_SDK);
}
