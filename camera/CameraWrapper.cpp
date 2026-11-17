/*
 * Copyright (C) 2012-2016, The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
* @file CameraWrapper.cpp
*
* This file wraps a vendor camera module.
*/

#define LOG_TAG "CameraWrapper"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <CameraParameters.h>
#include <cutils/log.h>
#include <hardware/camera.h>
#include <hardware/hardware.h>
#include <utils/String8.h>
#include <utils/threads.h>

static const char KEY_DIS[] = "dis";
static const char DIS_DISABLE[] = "disable";
static const char KEY_ZSL[] = "zsl";
static const char ON[] = "on";
static const char OFF[] = "off";
static const char KEY_PHASE_AF[] = "phase-af";
static const char KEY_DYNAMIC_RANGE_CONTROL[] = "dynamic-range-control";
static const char KEY_QC_RT_HDR[] = "rt-hdr";
static const char KEY_QC_VIDEO_HDR[] = "video-hdr";
static const char KEY_QC_VIDEO_HDR_VALUES[] = "video-hdr-values";

#define BACK_CAMERA_ID 0
#define FRONT_CAMERA_ID 1

using namespace android;
using android::hardware::camera::common::helper::CameraParameters;

static Mutex gCameraWrapperLock;
static camera_module_t *gVendorModule = 0;

static camera_notify_callback gUserNotifyCb = NULL;
static camera_data_callback gUserDataCb = NULL;
static camera_data_timestamp_callback gUserDataCbTimestamp = NULL;
static camera_request_memory gUserGetMemory = NULL;
static void *gUserCameraDevice = NULL;

static char **fixed_set_params = NULL;

static int camera_device_open(const hw_module_t *module, const char *name,
        hw_device_t **device);
static int camera_get_number_of_cameras(void);
static int camera_get_camera_info(int camera_id, struct camera_info *info);
static int camera_set_callbacks(const camera_module_callbacks_t *callbacks);
static int camera_set_torch_mode(const char *camera_id, bool enabled);

static struct hw_module_methods_t camera_module_methods = {
    .open = camera_device_open,
};

/*
 * Module API 2.4 (the devices stay HALv1) so that the provider offers
 * set_torch_mode: Android 15 has no HALv1 flash control in cameraserver.
 * The wrapper then owns the torch contract of a 2.4 module: torch off and
 * NOT_AVAILABLE while a camera is open, -EBUSY for torch requests meanwhile.
 */
camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
         .tag = HARDWARE_MODULE_TAG,
         .module_api_version = CAMERA_MODULE_API_VERSION_2_4,
         .hal_api_version = HARDWARE_HAL_API_VERSION,
         .id = CAMERA_HARDWARE_MODULE_ID,
         .name = "MSM8974 Camera Wrapper",
         .author = "The CyanogenMod Project",
         .methods = &camera_module_methods,
         .dso = NULL,
         .reserved = {0},
    },
    .get_number_of_cameras = camera_get_number_of_cameras,
    .get_camera_info = camera_get_camera_info,
    .set_callbacks = camera_set_callbacks,
    .get_vendor_tag_ops = NULL,
    .open_legacy = NULL,
    .set_torch_mode = camera_set_torch_mode,
    .init = NULL,
    .reserved = {0},
};

/*******************************************************************
 * Torch (rear LED through the max77804k torch-sec1 class device)
 *******************************************************************/

#define TORCH_CAMERA_ID "0"
#define TORCH_PATH "/sys/class/camera/flash/rear_flash"
/* Torch current code 0..15 (MAX77804K_TORCH_IOUT); 6 = board-8974.c default */
#define TORCH_ON_LEVEL "6"

static std::mutex gTorchLock;
static const camera_module_callbacks_t *gModuleCallbacks = NULL;
static bool gTorchOn = false;
static int gOpenCameras = 0;

/*
 * Torch status goes to the provider from one worker thread, in order: the
 * provider forwards it under its own callback lock straight into
 * cameraserver, which may be inside a call into this module (open while
 * enumerating, setTorchMode).
 */
static std::mutex gStatusLock;
static std::condition_variable gStatusCond;
static std::deque<int> gStatusQueue;
static bool gStatusThreadStarted = false;

static void torch_status_thread()
{
    for (;;) {
        int status;
        const camera_module_callbacks_t *callbacks;
        {
            std::unique_lock<std::mutex> lock(gStatusLock);
            gStatusCond.wait(lock, [] { return !gStatusQueue.empty(); });
            status = gStatusQueue.front();
            gStatusQueue.pop_front();
            callbacks = gModuleCallbacks;
        }
        if (callbacks && callbacks->torch_mode_status_change)
            callbacks->torch_mode_status_change(callbacks, TORCH_CAMERA_ID, status);
    }
}

static void torch_notify(int status)
{
    std::lock_guard<std::mutex> lock(gStatusLock);
    if (!gStatusThreadStarted) {
        std::thread(torch_status_thread).detach();
        gStatusThreadStarted = true;
    }
    gStatusQueue.push_back(status);
    gStatusCond.notify_one();
}

static int torch_write(bool on)
{
    int fd = open(TORCH_PATH, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;
        ALOGE("%s: open %s: %s", __FUNCTION__, TORCH_PATH, strerror(err));
        return -err;
    }
    const char *value = on ? TORCH_ON_LEVEL : "0";
    ssize_t len = strlen(value);
    int ret = write(fd, value, len) == len ? 0 : -errno;
    if (ret)
        ALOGE("%s: write %s to %s: %s", __FUNCTION__, value, TORCH_PATH, strerror(-ret));
    close(fd);
    return ret;
}

static int camera_set_callbacks(const camera_module_callbacks_t *callbacks)
{
    std::lock_guard<std::mutex> lock(gStatusLock);
    gModuleCallbacks = callbacks;
    return 0;
}

static int camera_set_torch_mode(const char *camera_id, bool enabled)
{
    if (!camera_id || strcmp(camera_id, TORCH_CAMERA_ID))
        return -ENOSYS;

    std::lock_guard<std::mutex> lock(gTorchLock);
    if (gOpenCameras > 0)
        return -EBUSY;

    int ret = torch_write(enabled);
    if (ret)
        return ret;

    gTorchOn = enabled;
    torch_notify(enabled ? TORCH_MODE_STATUS_AVAILABLE_ON : TORCH_MODE_STATUS_AVAILABLE_OFF);
    return 0;
}

/* A camera is about to open: the HAL owns the flash LED until it closes. */
static void torch_camera_opening()
{
    std::lock_guard<std::mutex> lock(gTorchLock);
    if (gOpenCameras++ > 0)
        return;
    if (gTorchOn) {
        torch_write(false);
        gTorchOn = false;
    }
    torch_notify(TORCH_MODE_STATUS_NOT_AVAILABLE);
}

static void torch_camera_closed()
{
    std::lock_guard<std::mutex> lock(gTorchLock);
    if (gOpenCameras > 0 && --gOpenCameras == 0)
        torch_notify(TORCH_MODE_STATUS_AVAILABLE_OFF);
}

typedef struct wrapper_camera_device {
    camera_device_t base;
    int id;
    camera_device_t *vendor;
} wrapper_camera_device_t;

#define VENDOR_CALL(device, func, ...) ({ \
    wrapper_camera_device_t *__wrapper_dev = (wrapper_camera_device_t*) device; \
    __wrapper_dev->vendor->ops->func(__wrapper_dev->vendor, ##__VA_ARGS__); \
})

#define CAMERA_ID(device) (((wrapper_camera_device_t *)device)->id)

static int check_vendor_module()
{
    int rv;
    ALOGV("%s", __FUNCTION__);

    if (gVendorModule)
        return 0;

    rv = hw_get_module_by_class("camera", "vendor",
            (const hw_module_t**)&gVendorModule);
    if (rv)
        ALOGE("Failed to open vendor camera module %d", rv);

    return rv;
}

#define KEY_VIDEO_HFR_VALUES "video-hfr-values"

// nv12-venus is needed for blobs, but
// framework has no idea what it is
#define PIXEL_FORMAT_NV12_VENUS "nv12-venus"

static bool is_4k_video(CameraParameters &params) {
    int video_width, video_height;
    params.getVideoSize(&video_width, &video_height);
    ALOGV("%s : VideoSize is %x", __FUNCTION__, video_width * video_height);
    return video_width * video_height == 3840 * 2160;
}

static char *camera_fixup_getparams(int __attribute__((unused)) id,
    const char *settings)
{
    CameraParameters params;
    params.unflatten(String8(settings));

    ALOGV("%s: original parameters:", __FUNCTION__);
    params.dump();

    const char *recordHint = params.get(CameraParameters::KEY_RECORDING_HINT);
    bool videoMode = recordHint ? !strcmp(recordHint, "true") : false;

    //Hide nv12-venus from Android.
    if (strcmp (params.getPreviewFormat(), PIXEL_FORMAT_NV12_VENUS) == 0)
          params.setPreviewFormat(params.PIXEL_FORMAT_YUV420SP);

    const char *videoSizeValues = params.get(
            CameraParameters::KEY_SUPPORTED_VIDEO_SIZES);
    if (videoSizeValues) {
        char videoSizes[strlen(videoSizeValues) + 10 + 1];
        sprintf(videoSizes, "3840x2160,%s", videoSizeValues);
        params.set(CameraParameters::KEY_SUPPORTED_VIDEO_SIZES,
                videoSizes);
    }

    /* If the vendor has HFR values but doesn't also expose that
     * this can be turned off, fixup the params to tell the Camera
     * that it really is okay to turn it off.
     */
    const char *hfrModeValues = params.get(KEY_VIDEO_HFR_VALUES);
    if (hfrModeValues && !strstr(hfrModeValues, "off")) {
        char hfrModes[strlen(hfrModeValues) + 4 + 1];
        sprintf(hfrModes, "%s,off", hfrModeValues);
        params.set(KEY_VIDEO_HFR_VALUES, hfrModes);
    }

    /* Enforce video-snapshot-supported to true */
    if (videoMode) {
        params.set(CameraParameters::KEY_VIDEO_SNAPSHOT_SUPPORTED, "true");
    }

    /* advertise video HDR mode support */
    if (id == BACK_CAMERA_ID)
        params.set(KEY_QC_VIDEO_HDR_VALUES, "on,off");

    ALOGV("%s: Fixed parameters:", __FUNCTION__);
    params.dump();

    String8 strParams = params.flatten();
    char *ret = strdup(strParams.c_str());

    return ret;
}

static char *camera_fixup_setparams(int id, const char *settings)
{
    CameraParameters params;
    params.unflatten(String8(settings));

    ALOGV("%s: original parameters:", __FUNCTION__);
    params.dump();

    bool wasTorch = false;
    if (fixed_set_params[id]) {
        /* When torch mode is switched off, it is important not to set ZSL, to
           avoid a segmentation violation in libcameraservice.so. Hence, check
           if the last call to setparams enabled torch mode */
        CameraParameters old_params;
        old_params.unflatten(String8(fixed_set_params[id]));

        const char *old_flashMode = old_params.get(CameraParameters::KEY_FLASH_MODE);
        wasTorch = old_flashMode && !strcmp(old_flashMode, CameraParameters::FLASH_MODE_TORCH);
    }

    const char *recordingHint = params.get(CameraParameters::KEY_RECORDING_HINT);
    bool isVideo = recordingHint && !strcmp(recordingHint, "true");
    const char *flashMode = params.get(CameraParameters::KEY_FLASH_MODE);
    bool isTorch = flashMode && !strcmp(flashMode, CameraParameters::FLASH_MODE_TORCH);

    if (!isTorch && !wasTorch) {
        if (isVideo) {
            params.set(KEY_DIS, DIS_DISABLE);
            params.set(KEY_ZSL, OFF);
        } else {
            params.set(KEY_ZSL, ON);
        }
    }

    if (id == BACK_CAMERA_ID) {
        params.set(KEY_PHASE_AF, ON);
        params.set(KEY_DYNAMIC_RANGE_CONTROL, ON);
        if (isVideo) {
            /* need to translate video-hdr to rt-hdr */
            const char *vhdr = params.get(KEY_QC_VIDEO_HDR);
            params.set(KEY_QC_RT_HDR, vhdr && !strcmp(vhdr, "on") ? ON : OFF);
        }
    }

    ALOGV("%s: Fixed parameters:", __FUNCTION__);
    params.dump();

    String8 strParams = params.flatten();
    if (fixed_set_params[id])
        free(fixed_set_params[id]);
    fixed_set_params[id] = strdup(strParams.c_str());
    char *ret = fixed_set_params[id];

    return ret;
}

/*******************************************************************
 * Implementation of camera_device_ops functions
 *******************************************************************/
static char *camera_get_parameters(struct camera_device *device);
static int camera_set_parameters(struct camera_device *device, const char *params);

static int camera_set_preview_window(struct camera_device *device,
        struct preview_stream_ops *window)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, set_preview_window, window);
}

void camera_notify_cb(int32_t msg_type, int32_t ext1, int32_t ext2, void *user) {
    gUserNotifyCb(msg_type, ext1, ext2, gUserCameraDevice);
}

void camera_data_cb(int32_t msg_type, const camera_memory_t *data, unsigned int index,
        camera_frame_metadata_t *metadata, void *user) {
    gUserDataCb(msg_type, data, index, metadata, gUserCameraDevice);
}

void camera_data_cb_timestamp(nsecs_t timestamp, int32_t msg_type,
        const camera_memory_t *data, unsigned index, void *user) {
    gUserDataCbTimestamp(timestamp, msg_type, data, index, gUserCameraDevice);
}

camera_memory_t* camera_get_memory(int fd, size_t buf_size,
        uint_t num_bufs, void *user) {
    return gUserGetMemory(fd, buf_size, num_bufs, gUserCameraDevice);
}

static void camera_set_callbacks(struct camera_device *device,
        camera_notify_callback notify_cb,
        camera_data_callback data_cb,
        camera_data_timestamp_callback data_cb_timestamp,
        camera_request_memory get_memory,
        void *user)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    gUserNotifyCb = notify_cb;
    gUserDataCb = data_cb;
    gUserDataCbTimestamp = data_cb_timestamp;
    gUserGetMemory = get_memory;
    gUserCameraDevice = user;

    VENDOR_CALL(device, set_callbacks, camera_notify_cb, camera_data_cb,
            camera_data_cb_timestamp, camera_get_memory, user);
}

static void camera_enable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, enable_msg_type, msg_type);
}

static void camera_disable_msg_type(struct camera_device *device,
        int32_t msg_type)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, disable_msg_type, msg_type);
}

static int camera_msg_type_enabled(struct camera_device *device,
        int32_t msg_type)
{
    if (!device)
        return 0;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, msg_type_enabled, msg_type);
}

static int camera_start_preview(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, start_preview);
}

static void camera_stop_preview(struct camera_device *device)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, stop_preview);
}

static int camera_preview_enabled(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, preview_enabled);
}

static int camera_store_meta_data_in_buffers(struct camera_device *device,
        int enable)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, store_meta_data_in_buffers, enable);
}

static int camera_start_recording(struct camera_device *device)
{
    if (!device)
        return EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    CameraParameters parameters;
    parameters.unflatten(String8(camera_get_parameters(device)));

    if (is_4k_video(parameters)) {
        ALOGV("%s : UHD detected, switching preview-format to nv12-venus", __FUNCTION__);
        parameters.setPreviewFormat(PIXEL_FORMAT_NV12_VENUS);
        camera_set_parameters(device, strdup(parameters.flatten().c_str()));
    }

    return VENDOR_CALL(device, start_recording);
}

static void camera_stop_recording(struct camera_device *device)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, stop_recording);
}

static int camera_recording_enabled(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, recording_enabled);
}

static void camera_release_recording_frame(struct camera_device *device,
        const void *opaque)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, release_recording_frame, opaque);
}

static int camera_auto_focus(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, auto_focus);
}

static int camera_cancel_auto_focus(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, cancel_auto_focus);
}

static int camera_take_picture(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, take_picture);
}

static int camera_cancel_picture(struct camera_device *device)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, cancel_picture);
}

static int camera_set_parameters(struct camera_device *device,
        const char *params)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    char *tmp = camera_fixup_setparams(CAMERA_ID(device), params);

    return VENDOR_CALL(device, set_parameters, tmp);
}

static char *camera_get_parameters(struct camera_device *device)
{
    if (!device)
        return NULL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    char *params = VENDOR_CALL(device, get_parameters);

    char *tmp = camera_fixup_getparams(CAMERA_ID(device), params);

    VENDOR_CALL(device, put_parameters, params);
    params = tmp;

    return params;
}

static void camera_put_parameters(__unused struct camera_device *device,
        char *params)
{
    if (params)
        free(params);
}

static int camera_send_command(struct camera_device *device,
        int32_t cmd, int32_t arg1, int32_t arg2)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, send_command, cmd, arg1, arg2);
}

static void camera_release(struct camera_device *device)
{
    if (!device)
        return;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    VENDOR_CALL(device, release);
}

static int camera_dump(struct camera_device *device, int fd)
{
    if (!device)
        return -EINVAL;

    ALOGV("%s->%08X->%08X", __FUNCTION__, (uintptr_t)device,
            (uintptr_t)(((wrapper_camera_device_t*)device)->vendor));

    return VENDOR_CALL(device, dump, fd);
}

extern "C" void heaptracker_free_leaked_memory(void);

static int camera_device_close(hw_device_t *device)
{
    int ret = 0;
    wrapper_camera_device_t *wrapper_dev = NULL;

    ALOGV("%s", __FUNCTION__);

    Mutex::Autolock lock(gCameraWrapperLock);

    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    for (int i = 0; i < camera_get_number_of_cameras(); i++) {
        if (fixed_set_params[i])
            free(fixed_set_params[i]);
    }

    wrapper_dev = (wrapper_camera_device_t*) device;

    wrapper_dev->vendor->common.close((hw_device_t*)wrapper_dev->vendor);
    if (wrapper_dev->base.ops)
        free(wrapper_dev->base.ops);
    free(wrapper_dev);
    torch_camera_closed();

done:
#ifdef HEAPTRACKER
    heaptracker_free_leaked_memory();
#endif
    return ret;
}

/*******************************************************************
 * Implementation of camera_module functions
 *******************************************************************/

/*
 * Open device handle to one of the cameras
 *
 * Assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

static int camera_device_open(const hw_module_t *module, const char *name,
        hw_device_t **device)
{
    int rv = 0;
    int num_cameras = 0;
    int camera_id;
    bool torch_released = false;
    bool vendor_opened = false;
    wrapper_camera_device_t *camera_device = NULL;
    camera_device_ops_t *camera_ops = NULL;

    Mutex::Autolock lock(gCameraWrapperLock);

    ALOGV("%s", __FUNCTION__);

    if (name != NULL) {
        if (check_vendor_module())
            return -EINVAL;

        camera_id = atoi(name);
        num_cameras = gVendorModule->get_number_of_cameras();

        fixed_set_params = (char **) malloc(sizeof(char *) *num_cameras);
        if (!fixed_set_params) {
            ALOGE("Parameter memory allocation fail");
            rv = -ENOMEM;
            goto fail;
        }
        memset(fixed_set_params, 0, sizeof(char *) * num_cameras);

        if (camera_id > num_cameras) {
            ALOGE("Camera service provided camera_id out of bounds, "
                    "camera_id = %d, num supported = %d",
                    camera_id, num_cameras);
            rv = -EINVAL;
            goto fail;
        }

        camera_device = (wrapper_camera_device_t*)
                malloc(sizeof(*camera_device));
        if (!camera_device) {
            ALOGE("camera_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }
        memset(camera_device, 0, sizeof(*camera_device));
        camera_device->id = camera_id;

        torch_camera_opening();
        torch_released = true;

        rv = gVendorModule->common.methods->open(
                (const hw_module_t*)gVendorModule, name,
                (hw_device_t**)&(camera_device->vendor));
        if (rv) {
            ALOGE("Vendor camera open fail");
            goto fail;
        }
        vendor_opened = true;
        ALOGV("%s: Got vendor camera device 0x%08X",
                __FUNCTION__, (uintptr_t)(camera_device->vendor));

        camera_ops = (camera_device_ops_t*)malloc(sizeof(*camera_ops));
        if (!camera_ops) {
            ALOGE("camera_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        memset(camera_ops, 0, sizeof(*camera_ops));

        camera_device->base.common.tag = HARDWARE_DEVICE_TAG;
        camera_device->base.common.version = CAMERA_DEVICE_API_VERSION_1_0;
        camera_device->base.common.module = (hw_module_t *)(module);
        camera_device->base.common.close = camera_device_close;
        camera_device->base.ops = camera_ops;

        camera_ops->set_preview_window = camera_set_preview_window;
        camera_ops->set_callbacks = camera_set_callbacks;
        camera_ops->enable_msg_type = camera_enable_msg_type;
        camera_ops->disable_msg_type = camera_disable_msg_type;
        camera_ops->msg_type_enabled = camera_msg_type_enabled;
        camera_ops->start_preview = camera_start_preview;
        camera_ops->stop_preview = camera_stop_preview;
        camera_ops->preview_enabled = camera_preview_enabled;
        camera_ops->store_meta_data_in_buffers = camera_store_meta_data_in_buffers;
        camera_ops->start_recording = camera_start_recording;
        camera_ops->stop_recording = camera_stop_recording;
        camera_ops->recording_enabled = camera_recording_enabled;
        camera_ops->release_recording_frame = camera_release_recording_frame;
        camera_ops->auto_focus = camera_auto_focus;
        camera_ops->cancel_auto_focus = camera_cancel_auto_focus;
        camera_ops->take_picture = camera_take_picture;
        camera_ops->cancel_picture = camera_cancel_picture;
        camera_ops->set_parameters = camera_set_parameters;
        camera_ops->get_parameters = camera_get_parameters;
        camera_ops->put_parameters = camera_put_parameters;
        camera_ops->send_command = camera_send_command;
        camera_ops->release = camera_release;
        camera_ops->dump = camera_dump;

        *device = &camera_device->base.common;
    }

    return rv;

fail:
    if (camera_device) {
        if (vendor_opened)
            camera_device->vendor->common.close(
                    (hw_device_t*)camera_device->vendor);
        free(camera_device);
        camera_device = NULL;
    }
    if (camera_ops) {
        free(camera_ops);
        camera_ops = NULL;
    }
    if (torch_released)
        torch_camera_closed();
    *device = NULL;
    return rv;
}

static int camera_get_number_of_cameras(void)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    return gVendorModule->get_number_of_cameras();
}

static int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    ALOGV("%s", __FUNCTION__);
    if (check_vendor_module())
        return 0;
    if (!info)
        return -EINVAL;

    /* The vendor module is API 1.0: it fills facing and orientation only. */
    memset(info, 0, sizeof(*info));
    int rv = gVendorModule->get_camera_info(camera_id, info);

    /* Fields that a 2.x module must provide */
    info->device_version = CAMERA_DEVICE_API_VERSION_1_0;
    info->static_camera_characteristics = NULL;
    /* One ISP pipeline: each camera takes the whole budget, no explicit conflicts. */
    info->resource_cost = 100;
    info->conflicting_devices = NULL;
    info->conflicting_devices_length = 0;
    return rv;
}
