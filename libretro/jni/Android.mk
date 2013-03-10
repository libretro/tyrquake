LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

APP_DIR := ../../src

LOCAL_MODULE    := retro

ifeq ($(TARGET_ARCH),arm)
LOCAL_CFLAGS += -DANDROID_ARM
LOCAL_ARM_MODE := arm
endif

ifeq ($(TARGET_ARCH),x86)
LOCAL_CFLAGS +=  -DANDROID_X86
endif

ifeq ($(TARGET_ARCH),mips)
LOCAL_CFLAGS += -DANDROID_MIPS -D__mips__ -D__MIPSEL__
endif

BASE_DIR=../..


LOCAL_SRC_FILES    += $(BASE_DIR)/NQ/cl_input.c \
				 $(BASE_DIR)/common/cd_common.c \
				 $(BASE_DIR)/common/alias_model.c \
				 $(BASE_DIR)/NQ/chase.c \
				 $(BASE_DIR)/NQ/cl_demo.c \
				 $(BASE_DIR)/NQ/cl_main.c \
				 $(BASE_DIR)/NQ/cl_parse.c \
				 $(BASE_DIR)/NQ/cl_tent.c \
				 $(BASE_DIR)/common/common.c \
				 $(BASE_DIR)/common/cmd.c \
				 $(BASE_DIR)/common/crc.c \
				 $(BASE_DIR)/common/console.c \
				 $(BASE_DIR)/common/cvar.c \
				 $(BASE_DIR)/common/d_edge.c \
				 $(BASE_DIR)/common/d_fill.c \
				 $(BASE_DIR)/common/d_init.c \
				 $(BASE_DIR)/common/d_part.c \
				 $(BASE_DIR)/common/d_modech.c \
				 $(BASE_DIR)/common/d_polyse.c \
				 $(BASE_DIR)/common/d_scan.c \
				 $(BASE_DIR)/common/d_sky.c \
				 $(BASE_DIR)/common/d_sprite.c \
				 $(BASE_DIR)/common/d_surf.c \
				 $(BASE_DIR)/common/d_vars.c \
				 $(BASE_DIR)/common/draw.c \
				 $(BASE_DIR)/NQ/host.c \
				 $(BASE_DIR)/NQ/host_cmd.c \
				 $(BASE_DIR)/common/keys.c \
				 $(BASE_DIR)/common/mathlib.c \
				 $(BASE_DIR)/NQ/menu.c \
				 $(BASE_DIR)/common/model.c \
				 $(BASE_DIR)/NQ/net_common.c \
				 $(BASE_DIR)/NQ/net_loop.c \
				 $(BASE_DIR)/NQ/net_main.c \
				 $(BASE_DIR)/common/nonintel.c \
				 $(BASE_DIR)/common/pr_cmds.c \
				 $(BASE_DIR)/common/pr_exec.c \
				 $(BASE_DIR)/common/pr_edict.c \
				 $(BASE_DIR)/common/r_aclip.c \
				 $(BASE_DIR)/common/r_alias.c \
				 $(BASE_DIR)/common/r_bsp.c \
				 $(BASE_DIR)/common/r_draw.c \
				 $(BASE_DIR)/common/r_edge.c \
				 $(BASE_DIR)/common/r_efrag.c \
				 $(BASE_DIR)/common/r_light.c \
				 $(BASE_DIR)/common/r_main.c \
				 $(BASE_DIR)/common/r_misc.c \
				 $(BASE_DIR)/common/r_model.c \
				 $(BASE_DIR)/common/r_part.c \
				 $(BASE_DIR)/common/r_sky.c \
				 $(BASE_DIR)/common/r_sprite.c \
				 $(BASE_DIR)/common/r_vars.c \
				 $(BASE_DIR)/common/r_surf.c \
				 $(BASE_DIR)/common/rb_tree.c \
				 $(BASE_DIR)/NQ/sbar.c \
				 $(BASE_DIR)/common/screen.c \
				 $(BASE_DIR)/common/shell.c \
				 $(BASE_DIR)/common/snd_dma.c \
				 $(BASE_DIR)/common/snd_mem.c \
				 $(BASE_DIR)/common/snd_mix.c \
				 $(BASE_DIR)/common/sprite_model.c \
				 $(BASE_DIR)/NQ/sv_main.c \
				 $(BASE_DIR)/common/sv_move.c \
				 $(BASE_DIR)/NQ/sv_phys.c \
				 $(BASE_DIR)/NQ/sv_user.c \
				 $(BASE_DIR)/common/sys_libretro.c \
				 $(BASE_DIR)/NQ/view.c \
				 $(BASE_DIR)/common/wad.c \
				 $(BASE_DIR)/common/zone.c \
				 $(BASE_DIR)/common/world.c \
             $(BASE_DIR)/NQ/net_none.c

LOCAL_C_INCLUDES = $(BASE_DIR)/include $(BASE_DIR)/NQ

LOCAL_CFLAGS += -O3 -std=gnu99 -ffast-math -funroll-loops -DINLINE=inline -DNQ_HACK -DHAVE_STRINGS -DHAVE_INTTYPES_H -DQBASEDIR=. -DTYR_VERSION=0.62 -D__LIBRETRO__ -DFRONTEND_SUPPORTS_RGB565 -DANDROID -DLSB_FIRST

include $(BUILD_SHARED_LIBRARY)
