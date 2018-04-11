LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/..

USE_CODEC_FLAC   := 1
USE_CODEC_VORBIS := 1

include $(CORE_DIR)/Makefile.common

COREFLAGS := -ffast-math -funroll-loops -DINLINE=inline -DNQ_HACK -DQBASEDIR=. -DTYR_VERSION=0.62 -D__LIBRETRO__ -DANDROID $(INCFLAGS)
COREFLAGS += -DUSE_CODEC_WAVE -DUSE_CODEC_VORBIS -DUSE_CODEC_FLAC

GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
  COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

include $(CLEAR_VARS)
LOCAL_MODULE    := retro
LOCAL_SRC_FILES := $(SOURCES_C)
LOCAL_CFLAGS    := -std=gnu99 $(COREFLAGS)
LOCAL_LDFLAGS   := -Wl,-version-script=$(CORE_DIR)/common/libretro-link.T

ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
  LOCAL_ARM_NEON := true
endif

include $(BUILD_SHARED_LIBRARY)
