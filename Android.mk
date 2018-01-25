# Copyright 2006-2014 The Android Open Source Project

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= klogcat.cpp

LOCAL_SHARED_LIBRARIES := liblog libcutils

LOCAL_MODULE := klogcat

LOCAL_CFLAGS := -Werror

LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR_EXECUTABLES)

include $(BUILD_EXECUTABLE)