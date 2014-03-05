/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2012 Zhibin Wu, Simon Davie, Nico Kaiser
 * Copyright (C) 2012 QiSS ME Project Team
 * Copyright (C) 2012 Twisted, Sean Neeley
 * Copyright (C) 2012-2014 Tomasz Rostanski
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

#define LOG_TAG "CameraHAL"

//#define DUMP_PARAMS 1   /* dump parameteters after get/set operation */

#define MAX_CAMERAS_SUPPORTED 2
#define GRALLOC_USAGE_PMEM_PRIVATE_ADSP GRALLOC_USAGE_PRIVATE_0

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include <cutils/log.h>
#include <camera/CameraParameters.h>
#include <hardware/camera.h>
#include <binder/IMemory.h>
#include "QualcommCameraHardware.h"
#include <cutils/properties.h>

using android::sp;
//using android::Overlay;
using android::String8;
using android::IMemory;
using android::IMemoryHeap;
using android::CameraParameters;

using android::CameraInfo;
using android::HAL_getCameraInfo;
using android::HAL_getNumberOfCameras;
using android::HAL_openCameraHardware;
using android::QualcommCameraHardware;

static QualcommCameraHardware *gCameraHals[MAX_CAMERAS_SUPPORTED];
static unsigned int gCamerasOpen = 0;
//static android::Mutex gCameraDeviceLock;

static int camera_device_open(const hw_module_t* module, const char* name,
                              hw_device_t** device);
static int camera_device_close(hw_device_t* device);
static int camera_get_number_of_cameras(void);
static int camera_get_camera_info(int camera_id, struct camera_info *info);
int camera_get_number_of_cameras(void);

static struct hw_module_methods_t camera_module_methods = {
    open: camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
    common: {
        tag: HARDWARE_MODULE_TAG,
        version_major: 1,
        version_minor: 0,
        id: CAMERA_HARDWARE_MODULE_ID,
        name: "Tenderloin CameraHal Module",
        author: "Tomasz Rostanski",
        methods: &camera_module_methods,
        dso: NULL, /* remove compilation warnings */
        reserved: {0}, /* remove compilation warnings */
    },
    get_number_of_cameras: camera_get_number_of_cameras,
    get_camera_info: camera_get_camera_info,
};

typedef struct priv_camera_device {
    camera_device_t base;
    /* specific "private" data can go here (base.priv) */
    int cameraid;
    int preview_mode;
    int rotation;
    int released;
} priv_camera_device_t;


static struct {
    int type;
    const char *text;
} msg_map[] = {
    {0x0001, "CAMERA_MSG_ERROR"},
    {0x0002, "CAMERA_MSG_SHUTTER"},
    {0x0004, "CAMERA_MSG_FOCUS"},
    {0x0008, "CAMERA_MSG_ZOOM"},
    {0x0010, "CAMERA_MSG_PREVIEW_FRAME"},
    {0x0020, "CAMERA_MSG_VIDEO_FRAME"},
    {0x0040, "CAMERA_MSG_POSTVIEW_FRAME"},
    {0x0080, "CAMERA_MSG_RAW_IMAGE"},
    {0x0100, "CAMERA_MSG_COMPRESSED_IMAGE"},
    {0x0200, "CAMERA_MSG_RAW_IMAGE_NOTIFY"},
    {0x0400, "CAMERA_MSG_PREVIEW_METADATA"},
    {0x0000, "CAMERA_MSG_ALL_MSGS"}, //0xFFFF
    {0x0000, "NULL"},
};

static void dump_msg(const char *tag, int msg_type)
{
#if LOG_NDEBUG
    int i;
    for (i = 0; msg_map[i].type; i++) {
        if (msg_type & msg_map[i].type) {
            ALOGV("%s: %s", tag, msg_map[i].text);
        }
    }
#endif
}

static int read_mode_from_config(const char *entry)
{
    int mode = 0;
    FILE *fp = fopen("/data/misc/camera/config.txt", "r");
    if (fp) {
        char buff[80], *tmp;
        while (fgets(buff, sizeof(buff), fp) != NULL) {
            if ((tmp = strstr(buff, entry)) != NULL) {
                tmp += strlen(entry) + 1;
                mode = atoi(tmp);
                break;
            }
        }
        fclose(fp);
    }
    ALOGI("Returning param %s mode %d\n", entry, mode);
    return mode;
}

/*******************************************************************
 * implementation of priv_camera_device_ops functions
 *******************************************************************/

void CameraHAL_FixupParams(android::CameraParameters &camParams)
{
    const char *fps_supported_ranges = "(15,31)";

    camParams.set(android::CameraParameters::KEY_VIDEO_FRAME_FORMAT,
                  android::CameraParameters::PIXEL_FORMAT_YUV420SP);

    if (!camParams.get(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE)) {
        camParams.set(CameraParameters::KEY_SUPPORTED_PREVIEW_FPS_RANGE,
                      fps_supported_ranges);
    }

    /* Disable auto focus on TouchPad */
    camParams.set(CameraParameters::KEY_SUPPORTED_FOCUS_MODES,
                  CameraParameters::FOCUS_MODE_INFINITY);

    camParams.set(CameraParameters::KEY_FOCUS_MODE,
                  CameraParameters::FOCUS_MODE_INFINITY);

    camParams.set(android::CameraParameters::KEY_MAX_SHARPNESS, "30");
    camParams.set(android::CameraParameters::KEY_MAX_CONTRAST, "10");
    camParams.set(android::CameraParameters::KEY_MAX_SATURATION, "10");
    camParams.set("num-snaps-per-shutter", "1");
}

int camera_set_preview_window(struct camera_device * device,
                              struct preview_stream_ops *window)
{
    priv_camera_device_t* dev = NULL;
    ALOGI("%s+++,device %p", __FUNCTION__,device);

    if(!device)
        return -EINVAL;

    dev = (priv_camera_device_t*) device;

    if (dev->released)
        return -EINVAL;

    return gCameraHals[dev->cameraid]->set_PreviewWindow((void *)window);
}

void camera_set_callbacks(struct camera_device * device,
                          camera_notify_callback notify_cb,
                          camera_data_callback data_cb,
                          camera_data_timestamp_callback data_cb_timestamp,
                          camera_request_memory get_memory,
                          void *user)
{
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++,device %p", __FUNCTION__,device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->setCallbacks(notify_cb, data_cb,
                                             data_cb_timestamp, get_memory, user);
    ALOGI("%s---", __FUNCTION__);
}

void camera_enable_msg_type(struct camera_device * device, int32_t msg_type)
{
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: type %i device %p", __FUNCTION__, msg_type,device);
    if (msg_type & CAMERA_MSG_RAW_IMAGE_NOTIFY) {
        msg_type &= ~CAMERA_MSG_RAW_IMAGE_NOTIFY;
        msg_type |= CAMERA_MSG_RAW_IMAGE;
    }

    dump_msg(__FUNCTION__, msg_type);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->enableMsgType(msg_type);
    ALOGI("%s---", __FUNCTION__);

}

void camera_disable_msg_type(struct camera_device * device, int32_t msg_type)
{
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: type %i device %p", __FUNCTION__, msg_type,device);
    dump_msg(__FUNCTION__, msg_type);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    /* The camera app disables the shutter too early which leads to crash.
     * Leaving it enabled. */
    if (msg_type == CAMERA_MSG_SHUTTER)
        return;

    gCameraHals[dev->cameraid]->disableMsgType(msg_type);
    ALOGI("%s---", __FUNCTION__);

}

int camera_msg_type_enabled(struct camera_device * device, int32_t msg_type)
{
    priv_camera_device_t* dev = NULL;
    int rv = -EINVAL;

    ALOGI("%s+++: type %i device %p", __FUNCTION__, msg_type,device);

    if(!device)
        return 0;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->msgTypeEnabled(msg_type);
    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_start_preview(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->startPreview();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

void camera_stop_preview(struct camera_device * device)
{
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->stopPreview();
    ALOGI("%s---", __FUNCTION__);
}

int camera_preview_enabled(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->previewEnabled();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);

    return rv;
}

int camera_store_meta_data_in_buffers(struct camera_device * device, int enable)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    //  TODO: meta data buffer not current supported
    //rv = gCameraHals[dev->cameraid]->storeMetaDataInBuffers(enable);
    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
    //return enable ? android::INVALID_OPERATION: android::OK;
}

int camera_start_recording(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->startRecording();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

void camera_stop_recording(struct camera_device * device)
{
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->stopRecording();

    ALOGI("%s---", __FUNCTION__);
}

int camera_recording_enabled(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->recordingEnabled();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

void camera_release_recording_frame(struct camera_device * device,
                                    const void *opaque)
{
    priv_camera_device_t* dev = NULL;

    ALOGV("%s+++: device %p, opaque %p", __FUNCTION__, device, opaque);

    if (!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->releaseRecordingFrame(opaque);

    ALOGV("%s---", __FUNCTION__);
}

int camera_auto_focus(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->autoFocus();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_cancel_auto_focus(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->cancelAutoFocus();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_take_picture(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->enableMsgType(CAMERA_MSG_SHUTTER |
        CAMERA_MSG_POSTVIEW_FRAME |
        CAMERA_MSG_RAW_IMAGE |
        CAMERA_MSG_COMPRESSED_IMAGE);

    rv = gCameraHals[dev->cameraid]->takePicture();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_cancel_picture(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->cancelPicture();

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_set_parameters(struct camera_device * device, const char *params)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;
    CameraParameters camParams;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    String8 params_str8(params);
    camParams.unflatten(params_str8);

#ifdef DUMP_PARAMS
    camParams.dump();
#endif

    /* Add timestamp */
    char str[20] = { 0 };
    const time_t date = time(NULL) + 1;
    if (strftime(str, sizeof(str), "%Y-%m-%d %H.%M.%S", localtime(&date)) > 0)
        camParams.set(CameraParameters::KEY_EXIF_DATETIME, str);

    rv = gCameraHals[dev->cameraid]->setParameters(camParams);

#ifdef DUMP_PARAMS
    camParams.dump();
#endif

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

char* camera_get_parameters(struct camera_device * device)
{
    char* params = NULL;
    priv_camera_device_t* dev = NULL;
    String8 params_str8;
    CameraParameters camParams;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return NULL;

    dev = (priv_camera_device_t*) device;

    camParams = gCameraHals[dev->cameraid]->getParameters();

#ifdef DUMP_PARAMS
    camParams.dump();
#endif

    CameraHAL_FixupParams(camParams);

    if (dev->rotation != 0) {
        camParams.set("rotation", dev->rotation);
    }

    params_str8 = camParams.flatten();
    params = (char*) malloc(sizeof(char) * (params_str8.length()+1));
    strcpy(params, params_str8.string());

#ifdef DUMP_PARAMS
    camParams.dump();
#endif

    ALOGI("%s---", __FUNCTION__);
    return params;
}

static void camera_put_parameters(struct camera_device *device, char *parms)
{
    ALOGI("%s+++", __FUNCTION__);
    free(parms);
    ALOGI("%s---", __FUNCTION__);
}

int camera_send_command(struct camera_device * device,
                        int32_t cmd, int32_t arg1, int32_t arg2)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s: cmd %i, arg1: %i arg2: %i, device %p", __FUNCTION__,
        cmd, arg1, arg2, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->sendCommand(cmd, arg1, arg2);

    ALOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

void camera_release(struct camera_device * device)
{
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->release();
    dev->released = true;
    ALOGI("%s---", __FUNCTION__);
}

int camera_dump(struct camera_device * device, int fd)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;
    ALOGI("%s", __FUNCTION__);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    // rv = gCameraHals[dev->cameraid]->dump(fd);
    return rv;
}

extern "C" void heaptracker_free_leaked_memory(void);

int camera_device_close(hw_device_t* device)
{
    int ret = 0;
    priv_camera_device_t* dev = NULL;

    ALOGI("%s+++: device %p", __FUNCTION__, device);

    //android::Mutex::Autolock lock(gCameraDeviceLock);

    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    dev = (priv_camera_device_t*) device;

    if (dev) {
        if (!dev->released) {
            gCameraHals[dev->cameraid]->release();
            dev->released = true;
        }
        delete(gCameraHals[dev->cameraid]);
        gCameraHals[dev->cameraid] = NULL;
        gCamerasOpen--;

        if (dev->base.ops) {
            free(dev->base.ops);
        }
        free(dev);
    }
done:
#ifdef HEAPTRACKER
    heaptracker_free_leaked_memory();
#endif
    ALOGI("%s--- ret %d", __FUNCTION__,ret);

    return ret;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

/* Ugly stuff - ignore SIGFPE */
void sigfpe_handle(int s)
{
    ALOGV("Received SIGFPE. Ignoring\n");
}

/* open device handle to one of the cameras
 *
 * assume camera service will keep singleton of each camera
 * so this function will always only be called once per camera instance
 */

int camera_device_open(const hw_module_t* module, const char* name,
                       hw_device_t** device)
{
    int rv = 0;
    int cameraid;
    int num_cameras = 0;
    priv_camera_device_t* priv_camera_device = NULL;
    camera_device_ops_t* camera_ops = NULL;
    QualcommCameraHardware *camera = NULL;

    //android::Mutex::Autolock lock(gCameraDeviceLock);

    /* add SIGFPE handler */
    signal(SIGFPE, sigfpe_handle);

    ALOGI("camera_device open+++");

    if (name != NULL) {
        cameraid = atoi(name);

        num_cameras = camera_get_number_of_cameras();

        if(cameraid > num_cameras)
        {
            ALOGE("camera service provided cameraid out of bounds, "
                 "cameraid = %d, num supported = %d",
                 cameraid, num_cameras);
            rv = -EINVAL;
            goto fail;
        }

        if(gCamerasOpen >= MAX_CAMERAS_SUPPORTED)
        {
            ALOGE("maximum number of cameras already open");
            rv = -ENOMEM;
            goto fail;
        }

        priv_camera_device = (priv_camera_device_t*)malloc(sizeof(*priv_camera_device));
        if(!priv_camera_device)
        {
            ALOGE("camera_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        camera_ops = (camera_device_ops_t*)malloc(sizeof(*camera_ops));
        if(!camera_ops)
        {
            ALOGE("camera_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        memset(priv_camera_device, 0, sizeof(*priv_camera_device));
        memset(camera_ops, 0, sizeof(*camera_ops));

        priv_camera_device->base.common.tag = HARDWARE_DEVICE_TAG;
        priv_camera_device->base.common.version = 0;
        priv_camera_device->base.common.module = (hw_module_t *)(module);
        priv_camera_device->base.common.close = camera_device_close;
        priv_camera_device->base.ops = camera_ops;

        priv_camera_device->preview_mode = read_mode_from_config("preview_mode");
        priv_camera_device->rotation = read_mode_from_config("rotation_mode");
        priv_camera_device->released = false;

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

        *device = &priv_camera_device->base.common;

        // -------- specific stuff --------

        priv_camera_device->cameraid = cameraid;

        camera = HAL_openCameraHardware(cameraid);
        if(camera == NULL)
        {
            ALOGE("Couldn't create instance of CameraHal class");
            rv = -ENOMEM;
            goto fail;
        }

        gCameraHals[cameraid] = camera;
        gCamerasOpen++;
    }
    ALOGI("%s---ok rv %d", __FUNCTION__,rv);

    return rv;

fail:
    if(priv_camera_device) {
        free(priv_camera_device);
        priv_camera_device = NULL;
    }
    if(camera_ops) {
        free(camera_ops);
        camera_ops = NULL;
    }
    *device = NULL;
    ALOGI("%s--- fail rv %d", __FUNCTION__,rv);

    return rv;
}

int camera_get_number_of_cameras(void)
{
    int num_cameras = HAL_getNumberOfCameras();
    ALOGI("%s: number:%i", __FUNCTION__, num_cameras);
    return num_cameras;
}

int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    int rv = 0;
    CameraInfo cameraInfo;

    android::HAL_getCameraInfo(camera_id, &cameraInfo);
    info->facing = cameraInfo.facing;
    info->orientation = cameraInfo.orientation;

    if (read_mode_from_config("preview_mode") == 1) {
        info->facing = CAMERA_FACING_BACK;
    }

    ALOGI("%s: id:%i faceing:%i orientation: %i", __FUNCTION__,camera_id, info->facing, info->orientation);
    return rv;
}
