/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2012 Zhibin Wu, Simon Davie, Nico Kaiser
 * Copyright (C) 2012 QiSS ME Project Team
 * Copyright (C) 2012 Twisted, Sean Neeley
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

#define MAX_CAMERAS_SUPPORTED 2
#define GRALLOC_USAGE_PMEM_PRIVATE_ADSP GRALLOC_USAGE_PRIVATE_0

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <cutils/log.h>
#include <ui/OverlayHtc.h>
#include <camera/CameraParameters.h>
#include <hardware/camera.h>
#include <binder/IMemory.h>
#include "CameraHardwareInterface.h"
#include <cutils/properties.h>

using android::sp;
using android::Overlay;
using android::String8;
using android::IMemory;
using android::IMemoryHeap;
using android::CameraParameters;

using android::CameraInfo;
using android::HAL_getCameraInfo;
using android::HAL_getNumberOfCameras;
using android::HAL_openCameraHardware;
using android::CameraHardwareInterface;

static sp<CameraHardwareInterface> gCameraHals[MAX_CAMERAS_SUPPORTED];
static unsigned int gCamerasOpen = 0;
//static android::Mutex gCameraDeviceLock;

static int camera_device_open(const hw_module_t* module, const char* name,
                              hw_device_t** device);
static int camera_device_close(hw_device_t* device);
static int camera_get_number_of_cameras(void);
static int camera_get_camera_info(int camera_id, struct camera_info *info);

static struct hw_module_methods_t camera_module_methods = {
open: camera_device_open
};

camera_module_t HAL_MODULE_INFO_SYM = {
common: {
tag: HARDWARE_MODULE_TAG,
version_major: 1,
version_minor: 0,
id: CAMERA_HARDWARE_MODULE_ID,
name: "7x30 CameraHal Module",
author: "Zhibin Wu",
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
    /* new world */
    preview_stream_ops *window;
    camera_notify_callback notify_callback;
    camera_data_callback data_callback;
    camera_data_timestamp_callback data_timestamp_callback;
    camera_request_memory request_memory;
    void *user;
    /* old world*/
    int preview_width;
    int preview_height;
    sp<Overlay> overlay;
    gralloc_module_t const *gralloc;
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
    int i;
    for (i = 0; msg_map[i].type; i++) {
        if (msg_type & msg_map[i].type) {
            LOGI("%s: %s", tag, msg_map[i].text);
        }
    }
}

/*******************************************************************
 * overlay hook
 *******************************************************************/

static void wrap_set_fd_hook(void *data, int fd)
{
    priv_camera_device_t* dev = NULL;
    //LOGI("%s+++: data %p", __FUNCTION__, data);

    if(!data)
        return;

    dev = (priv_camera_device_t*) data;
    //LOGI("%s---: fd %i", __FUNCTION__, fd);
}

static void wrap_set_crop_hook(void *data,
                               uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h)
{
    priv_camera_device_t* dev = NULL;
    LOGI("%s+++: %p", __FUNCTION__,data);

    if(!data)
        return;

    dev = (priv_camera_device_t*) data;
    LOGI("%s---: %i %i %i %i", __FUNCTION__, x, y, w, h);
}
//QiSS ME for preview
static void wrap_queue_buffer_hook(void *data, void* buffer)
{
    sp<IMemoryHeap> heap;
    priv_camera_device_t* dev = NULL;
    preview_stream_ops* window = NULL;
    //LOGI("%s+++: %p", __FUNCTION__,data);

    if(!data)
        return;

    dev = (priv_camera_device_t*) data;

    window = dev->window;

	//QiSS ME fix video preview crash
    if(window == 0)
		return;

    heap =  gCameraHals[dev->cameraid]->getPreviewHeap();
    if(heap == 0)
		return;

    int offset = (int)buffer;
    char *frame = (char *)(heap->base()) + offset;

    //LOGI("%s: base:%p offset:%i frame:%p", __FUNCTION__,
    //     heap->base(), offset, frame);

    int stride;
    void *vaddr;
    buffer_handle_t *buf_handle;

    int width = dev->preview_width;
    int height = dev->preview_height;
    if (0 != window->dequeue_buffer(window, &buf_handle, &stride)) {
        LOGE("%s: could not dequeue gralloc buffer", __FUNCTION__);
        goto skipframe;
    }
    if (0 == dev->gralloc->lock(dev->gralloc, *buf_handle,
                                GRALLOC_USAGE_SW_WRITE_MASK,
                                0, 0, width, height, &vaddr)) {
        // the code below assumes YUV, not RGB
        memcpy(vaddr, frame, width * height * 3 / 2);
        //LOGI("%s: copy frame to gralloc buffer", __FUNCTION__);
    } else {
        LOGE("%s: could not lock gralloc buffer", __FUNCTION__);
        goto skipframe;
    }

    dev->gralloc->unlock(dev->gralloc, *buf_handle);

    if (0 != window->enqueue_buffer(window, buf_handle)) {
        LOGE("%s: could not dequeue gralloc buffer", __FUNCTION__);
        goto skipframe;
    }

skipframe:

#ifdef DUMP_PREVIEW_FRAMES
    static int frameCnt = 0;
    int written;
    if (frameCnt >= 100 && frameCnt <= 109 ) {
        char path[128];
        snprintf(path, sizeof(path), "/data/%d_preview.yuv", frameCnt);
        int file_fd = open(path, O_RDWR | O_CREAT, 0666);
        LOGI("dumping preview frame %d", frameCnt);
        if (file_fd < 0) {
            LOGE("cannot open file:%s (error:%i)\n", path, file_fd);
        }
        else
        {
            LOGV("dumping data");
            written = write(file_fd, (char *)frame,
                            dev->preview_frame_size);
            if(written < 0)
                LOGE("error in data write");
        }
        close(file_fd);
    }
    frameCnt++;
#endif
    LOGI("%s---: ", __FUNCTION__);

    return;
}

/*******************************************************************
 * camera interface callback
 *******************************************************************/

static camera_memory_t *wrap_memory_data(priv_camera_device_t *dev,
                                         const sp<IMemory>& dataPtr)
{
    void *data;
    size_t size;
    ssize_t offset;
    sp<IMemoryHeap> heap;
    camera_memory_t *mem;

    LOGI("%s+++,dev->request_memory %p", __FUNCTION__,dev->request_memory);

    if (!dev->request_memory)
        return NULL;

    heap = dataPtr->getMemory(&offset, &size);
    data = (void *)((char *)(heap->base()) + offset);

    LOGI("%s: data: %p size: %i", __FUNCTION__, data, size);

    LOGI(" offset:0x%x ",  offset);

    //#define DUMP_CAPTURE_JPEG
#ifdef DUMP_CAPTURE_JPEG
    static int frameCnt = 0;
    int written;
    char path[128];
    snprintf(path, sizeof(path), "/data/%d_capture.jpg", frameCnt);
    int file_fd = open(path, O_RDWR | O_CREAT, 0666);
    LOGI("dumping capture jpeg %d", frameCnt);
    if (file_fd < 0) {
        LOGE("cannot open file:%s (error:%i)\n", path, file_fd);
    }
    else
    {
        LOGV("dumping jpeg");
        written = write(file_fd, (char *)data,
                        size);
        if(written < 0)
            LOGE("error in data write");
    }
    close(file_fd);
    frameCnt++;
#endif

    mem = dev->request_memory(-1, size, 1, dev->user);

    LOGI(" mem:%p,mem->data%p ",  mem,mem->data);

    memcpy(mem->data, data, size);
    LOGI("%s---", __FUNCTION__);

    return mem;
}

static void wrap_notify_callback(int32_t msg_type, int32_t ext1,
                                 int32_t ext2, void* user)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: type %i user %p", __FUNCTION__, msg_type,user);
    dump_msg(__FUNCTION__, msg_type);

    if(!user)
        return;

    dev = (priv_camera_device_t*) user;

    if (dev->notify_callback)
        dev->notify_callback(msg_type, ext1, ext2, dev->user);
    LOGI("%s---", __FUNCTION__);

}
//QiSS ME for capture
static void wrap_data_callback(int32_t msg_type, const sp<IMemory>& dataPtr,
                               void* user)
{
    camera_memory_t *data = NULL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: type %i user %p", __FUNCTION__, msg_type,user);
    dump_msg(__FUNCTION__, msg_type);

    if(!user)
        return;

    dev = (priv_camera_device_t*) user;

    if(msg_type ==CAMERA_MSG_RAW_IMAGE)
    {
    	gCameraHals[dev->cameraid]->disableMsgType(CAMERA_MSG_RAW_IMAGE);
        return;
    }

    data = wrap_memory_data(dev, dataPtr);

    if (dev->data_callback)
        dev->data_callback(msg_type, data, 0, NULL, dev->user);
	LOGI("%s---", __FUNCTION__);

}
//QiSS ME for record

static void wrap_data_callback_timestamp(nsecs_t timestamp, int32_t msg_type,
                                         const sp<IMemory>& dataPtr, void* user)
{
    priv_camera_device_t* dev = NULL;
    camera_memory_t *data = NULL;

    LOGI("%s+++: type %i user %p", __FUNCTION__, msg_type,user);
    dump_msg(__FUNCTION__, msg_type);

    if(!user)
        return;

    dev = (priv_camera_device_t*) user;

    data = wrap_memory_data(dev, dataPtr);

    if (dev->data_timestamp_callback)
        dev->data_timestamp_callback(timestamp,msg_type, data, 0, dev->user);

    gCameraHals[dev->cameraid]->releaseRecordingFrame(dataPtr);//QiSS ME need release or record will stop

    if ( NULL != data ) {
        data->release(data);
    }

	LOGI("%s---", __FUNCTION__);

}

/*******************************************************************
 * implementation of priv_camera_device_ops functions
 *******************************************************************/

void CameraHAL_FixupParams(android::CameraParameters &camParams)
{
    const char *preferred_size = "640x480";
    const char *preview_frame_rates  = "30,27,24,15";
    const char *preferred_rate = "30";

    camParams.set(android::CameraParameters::KEY_VIDEO_FRAME_FORMAT,
                  android::CameraParameters::PIXEL_FORMAT_YUV420SP);

    camParams.set(CameraParameters::KEY_PREFERRED_PREVIEW_SIZE_FOR_VIDEO,
                  preferred_size);

    camParams.set(android::CameraParameters::KEY_MAX_SHARPNESS, "30");
    camParams.set(android::CameraParameters::KEY_MAX_CONTRAST, "10");
    camParams.set(android::CameraParameters::KEY_MAX_SATURATION, "10");
    camParams.set("num-snaps-per-shutter", "1");

    if (!camParams.get(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES)) {
        camParams.set(android::CameraParameters::KEY_SUPPORTED_PREVIEW_FRAME_RATES,
                      preview_frame_rates);
    }

    if (!camParams.get(android::CameraParameters::KEY_PREVIEW_FRAME_RATE)) {
        camParams.set(CameraParameters::KEY_PREVIEW_FRAME_RATE, preferred_rate);
    }
}

int camera_set_preview_window(struct camera_device * device,
                              struct preview_stream_ops *window)
{
    int rv = -EINVAL;
    int min_bufs = -1;
    int kBufferCount = 4;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++,device %p", __FUNCTION__,device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    dev->window = window;

    if (!window) {
        LOGI("%s---: window is NULL", __FUNCTION__);
        return 0;
    }

    if (!dev->gralloc) {
        if (hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                          (const hw_module_t **)&(dev->gralloc))) {
            LOGE("%s: Fail on loading gralloc HAL", __FUNCTION__);
        }
    }

    if (window->get_min_undequeued_buffer_count(window, &min_bufs)) {
        LOGE("%s---: could not retrieve min undequeued buffer count", __FUNCTION__);
        return -1;
    }

    LOGI("%s: bufs:%i", __FUNCTION__, min_bufs);

    if (min_bufs >= kBufferCount) {
        LOGE("%s: min undequeued buffer count %i is too high (expecting at most %i)",
             __FUNCTION__, min_bufs, kBufferCount - 1);
    }

    LOGI("%s: setting buffer count to %i", __FUNCTION__, kBufferCount);
    if (window->set_buffer_count(window, kBufferCount)) {
        LOGE("%s---: could not set buffer count", __FUNCTION__);
        return -1;
    }

    int preview_width;
    int preview_height;
    CameraParameters params(gCameraHals[dev->cameraid]->getParameters());
    params.getPreviewSize(&preview_width, &preview_height);
    int hal_pixel_format = HAL_PIXEL_FORMAT_YCrCb_420_SP;

    const char *str_preview_format = params.getPreviewFormat();
    LOGI("%s: preview format %s", __FUNCTION__, str_preview_format);

    //Enable panorama without camera application "hacks"
    //if (window->set_usage(window, GRALLOC_USAGE_SW_WRITE_MASK)) {
    //    LOGE("%s---: could not set usage on gralloc buffer", __FUNCTION__);
    //    return -1;
    //}

    window->set_usage(window, GRALLOC_USAGE_PMEM_PRIVATE_ADSP | GRALLOC_USAGE_SW_READ_OFTEN);

    if (window->set_buffers_geometry(window, preview_width,
                                     preview_height, hal_pixel_format)) {
        LOGE("%s---: could not set buffers geometry to %s",
             __FUNCTION__, str_preview_format);
        return -1;
    }

    dev->preview_width = preview_width;
    dev->preview_height = preview_height;

    dev->overlay =  new Overlay(wrap_set_fd_hook,
                                wrap_set_crop_hook,
                                wrap_queue_buffer_hook,
                                (void *)dev);
    gCameraHals[dev->cameraid]->setOverlay(dev->overlay);
    LOGI("%s---,rv %d", __FUNCTION__,rv);

    return rv;
}

void camera_set_callbacks(struct camera_device * device,
                          camera_notify_callback notify_cb,
                          camera_data_callback data_cb,
                          camera_data_timestamp_callback data_cb_timestamp,
                          camera_request_memory get_memory,
                          void *user)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++,device %p", __FUNCTION__,device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    dev->notify_callback = notify_cb;
    dev->data_callback = data_cb;
    dev->data_timestamp_callback = data_cb_timestamp;
    dev->request_memory = get_memory;
    dev->user = user;

    gCameraHals[dev->cameraid]->setCallbacks(wrap_notify_callback, wrap_data_callback,
                                             wrap_data_callback_timestamp, (void *)dev);

    LOGI("%s---", __FUNCTION__);

}

void camera_enable_msg_type(struct camera_device * device, int32_t msg_type)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: type %i device %p", __FUNCTION__, msg_type,device);
    if (msg_type & CAMERA_MSG_RAW_IMAGE_NOTIFY) {
        msg_type &= ~CAMERA_MSG_RAW_IMAGE_NOTIFY;
        msg_type |= CAMERA_MSG_RAW_IMAGE;
	}

    dump_msg(__FUNCTION__, msg_type);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->enableMsgType(msg_type);
    LOGI("%s---", __FUNCTION__);

}

void camera_disable_msg_type(struct camera_device * device, int32_t msg_type)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: type %i device %p", __FUNCTION__, msg_type,device);
    dump_msg(__FUNCTION__, msg_type);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->disableMsgType(msg_type);
    LOGI("%s---", __FUNCTION__);

}

int camera_msg_type_enabled(struct camera_device * device, int32_t msg_type)
{
    priv_camera_device_t* dev = NULL;
    int rv = -EINVAL;

    LOGI("%s+++: type %i device %p", __FUNCTION__, msg_type,device);

    if(!device)
        return 0;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->msgTypeEnabled(msg_type);
    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_start_preview(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->startPreview();
    LOGI("%s--- rv %d", __FUNCTION__,rv);

    return rv;
}

void camera_stop_preview(struct camera_device * device)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->stopPreview();
    LOGI("%s---", __FUNCTION__);
}

int camera_preview_enabled(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->previewEnabled();

    LOGI("%s--- rv %d", __FUNCTION__,rv);

    return rv;
}

int camera_store_meta_data_in_buffers(struct camera_device * device, int enable)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    //  TODO: meta data buffer not current supported
    //rv = gCameraHals[dev->cameraid]->storeMetaDataInBuffers(enable);
    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
    //return enable ? android::INVALID_OPERATION: android::OK;
}

int camera_start_recording(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->startRecording();
    LOGI("%s--- rv %d", __FUNCTION__,rv);

    return rv;
}

void camera_stop_recording(struct camera_device * device)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->stopRecording();

    //QiSS ME force start preview when recording stop
    gCameraHals[dev->cameraid]->startPreview();
    LOGI("%s---", __FUNCTION__);
}

int camera_recording_enabled(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->recordingEnabled();
    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

void camera_release_recording_frame(struct camera_device * device,
                                    const void *opaque)
{
    priv_camera_device_t* dev = NULL;
    //camera_memory_t *data = (camera_memory_t *)(&opaque);


    //LOGI("%s+++: device %p,opaque %p,data %p", __FUNCTION__, device,opaque,data);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;
    /*
     if ( NULL != data ) {
     data->release(data);
     }
     */
    //gCameraHals[dev->cameraid]->releaseRecordingFrame(opaque);

    LOGI("%s---", __FUNCTION__);
}

int camera_auto_focus(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->autoFocus();

    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_cancel_auto_focus(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->cancelAutoFocus();
    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_take_picture(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->takePicture();

    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_cancel_picture(struct camera_device * device)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->cancelPicture();

    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

int camera_set_parameters(struct camera_device * device, const char *params)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;
    CameraParameters camParams;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    String8 params_str8(params);
    camParams.unflatten(params_str8);
#if 0
    camParams.dump();
#endif

    rv = gCameraHals[dev->cameraid]->setParameters(camParams);

#if 0
    camParams.dump();
#endif
    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

char* camera_get_parameters(struct camera_device * device)
{
    char* params = NULL;
    priv_camera_device_t* dev = NULL;
    String8 params_str8;
    CameraParameters camParams;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return NULL;

    dev = (priv_camera_device_t*) device;

    camParams = gCameraHals[dev->cameraid]->getParameters();
#if 0
    camParams.dump();
#endif

    CameraHAL_FixupParams(camParams);

#ifdef HTC_FFC
    if (dev->cameraid == 1) {
#ifdef REVERSE_FFC
        /* Change default parameters for the front camera */
        camParams.set("front-camera-mode", "reverse"); // default is "mirror"
#endif
    } else {
        camParams.set("front-camera-mode", "mirror");
    }
#endif
    camParams.set("orientation", "landscape");

    params_str8 = camParams.flatten();
    params = (char*) malloc(sizeof(char) * (params_str8.length()+1));
    strcpy(params, params_str8.string());

#if 0
    camParams.dump();
#endif
    LOGI("%s---", __FUNCTION__);
    return params;
}

static void camera_put_parameters(struct camera_device *device, char *parms)
{
    LOGI("%s+++", __FUNCTION__);
    free(parms);
    LOGI("%s---", __FUNCTION__);
}

int camera_send_command(struct camera_device * device,
                        int32_t cmd, int32_t arg1, int32_t arg2)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;

    LOGI("%s: cmd %i,device %p", __FUNCTION__, cmd,device);

    if(!device)
        return rv;

    dev = (priv_camera_device_t*) device;

    rv = gCameraHals[dev->cameraid]->sendCommand(cmd, arg1, arg2);
    LOGI("%s--- rv %d", __FUNCTION__,rv);
    return rv;
}

void camera_release(struct camera_device * device)
{
    priv_camera_device_t* dev = NULL;

    LOGI("%s+++: device %p", __FUNCTION__, device);

    if(!device)
        return;

    dev = (priv_camera_device_t*) device;

    gCameraHals[dev->cameraid]->release();
    LOGI("%s---", __FUNCTION__);
}

int camera_dump(struct camera_device * device, int fd)
{
    int rv = -EINVAL;
    priv_camera_device_t* dev = NULL;
    LOGI("%s", __FUNCTION__);

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

    LOGI("%s+++: device %p", __FUNCTION__, device);

    //android::Mutex::Autolock lock(gCameraDeviceLock);

    if (!device) {
        ret = -EINVAL;
        goto done;
    }

    dev = (priv_camera_device_t*) device;

    if (dev) {
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
    LOGI("%s--- ret %d", __FUNCTION__,ret);

    return ret;
}

/*******************************************************************
 * implementation of camera_module functions
 *******************************************************************/

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
    sp<CameraHardwareInterface> camera = NULL;

    //android::Mutex::Autolock lock(gCameraDeviceLock);


    LOGI("camera_device open+++");

    if (name != NULL) {
        cameraid = atoi(name);

        num_cameras = HAL_getNumberOfCameras();

        if(cameraid > num_cameras)
        {
            LOGE("camera service provided cameraid out of bounds, "
                 "cameraid = %d, num supported = %d",
                 cameraid, num_cameras);
            rv = -EINVAL;
            goto fail;
        }

        if(gCamerasOpen >= MAX_CAMERAS_SUPPORTED)
        {
            LOGE("maximum number of cameras already open");
            rv = -ENOMEM;
            goto fail;
        }

        priv_camera_device = (priv_camera_device_t*)malloc(sizeof(*priv_camera_device));
        if(!priv_camera_device)
        {
            LOGE("camera_device allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

        camera_ops = (camera_device_ops_t*)malloc(sizeof(*camera_ops));
        if(!camera_ops)
        {
            LOGE("camera_ops allocation fail");
            rv = -ENOMEM;
            goto fail;
        }

#ifdef HTC_FFC
#define HTC_SWITCH_CAMERA_FILE_PATH "/sys/android_camera2/htcwc"

        char htc_buffer[16];
        int htc_fd;

        if (access(HTC_SWITCH_CAMERA_FILE_PATH, W_OK) == 0) {
            LOGI("Switching to HTC Camera: %d", cameraid);
            snprintf(htc_buffer, sizeof(htc_buffer), "%d", cameraid);
            htc_fd = open(HTC_SWITCH_CAMERA_FILE_PATH, O_WRONLY);
            write(htc_fd, htc_buffer, strlen(htc_buffer));
            close(htc_fd);
        }
#endif

        memset(priv_camera_device, 0, sizeof(*priv_camera_device));
        memset(camera_ops, 0, sizeof(*camera_ops));

        priv_camera_device->base.common.tag = HARDWARE_DEVICE_TAG;
        priv_camera_device->base.common.version = 0;
        priv_camera_device->base.common.module = (hw_module_t *)(module);
        priv_camera_device->base.common.close = camera_device_close;
        priv_camera_device->base.ops = camera_ops;

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
            LOGE("Couldn't create instance of CameraHal class");
            rv = -ENOMEM;
            goto fail;
        }

        gCameraHals[cameraid] = camera;
        gCamerasOpen++;
    }
    LOGI("%s---ok rv %d", __FUNCTION__,rv);

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
    LOGI("%s--- fail rv %d", __FUNCTION__,rv);

    return rv;
}

int camera_get_number_of_cameras(void)
{
    int num_cameras = HAL_getNumberOfCameras();

    LOGI("%s: number:%i", __FUNCTION__, num_cameras);

    return num_cameras;
}

int camera_get_camera_info(int camera_id, struct camera_info *info)
{
    int rv = 0;

    CameraInfo cameraInfo;

    android::HAL_getCameraInfo(camera_id, &cameraInfo);

    info->facing = cameraInfo.facing;
    //info->orientation = cameraInfo.orientation;
    if(info->facing == 1) {
        info->orientation = 270;
    } else {
        info->orientation = 90;
    }

    LOGI("%s: id:%i faceing:%i orientation: %i", __FUNCTION__,camera_id, info->facing, info->orientation);

    return rv;
}
