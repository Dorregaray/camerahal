
LOCAL_PATH:= $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/hw
LOCAL_MODULE := camera.$(TARGET_BOARD_PLATFORM)

LOCAL_MODULE_TAGS := optional

LOCAL_SRC_FILES := cameraHAL.cpp
LOCAL_C_INCLUDES := $(TOP)/frameworks/base/include

LOCAL_SHARED_LIBRARIES := liblog libutils libcutils
LOCAL_SHARED_LIBRARIES += libui libhardware libcamera_client
LOCAL_SHARED_LIBRARIES += libcamera
LOCAL_PRELINK_MODULE := false

# building for ICS
LOCAL_CFLAGS += -DANDROID_ICS

ifeq ($(BOARD_HAVE_HTC_FFC), true)
    LOCAL_CFLAGS += -DHTC_FFC
endif
ifeq ($(BOARD_USE_REVERSE_FFC), true)
    LOCAL_CFLAGS += -DREVERSE_FFC
endif
ifeq ($(BOARD_CAMERA_USE_ENCODEDATA),true)
    LOCAL_CFLAGS += -DUSE_ENCODEDATA
endif
ifeq ($(BOARD_CAMERA_USE_GETBUFFERINFO),true)
    LOCAL_CFLAGS += -DUSE_GETBUFFERINFO
endif
ifeq ($(BOARD_FIRST_CAMERA_FRONT_FACING),true)
    LOCAL_CFLAGS += -DFIRST_CAMERA_FACING=CAMERA_FACING_FRONT -DFIRST_CAMERA_ORIENTATION=90
endif
ifeq ($(BOARD_USE_FROYO_LIBCAMERA), true)
    LOCAL_CFLAGS += -DBOARD_USE_FROYO_LIBCAMERA
endif

include $(BUILD_SHARED_LIBRARY)

