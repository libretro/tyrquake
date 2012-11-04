#
# TyrQuake Makefile (tested under Linux and MinGW/Msys)
#
# By default, all executables will be built and placed in the ./bin
# subdirectory. If you want to build just one, type e.g. "make bin/tyr-quake".
# 

TYR_VERSION_MAJOR = 0
TYR_VERSION_MINOR = 62
TYR_VERSION_BUILD =

TYR_VERSION = $(TYR_VERSION_MAJOR).$(TYR_VERSION_MINOR)$(TYR_VERSION_BUILD)

# ============================================================================
# User configurable options here:
# ============================================================================

BUILD_DIR        ?= build
BIN_DIR          ?= bin
DEBUG            ?= N# Compile with debug info
OPTIMIZED_CFLAGS ?= Y# Enable compiler optimisations (if DEBUG != Y)
USE_X86_ASM      ?= $(I386_GUESS)
USE_SDL          ?= N# New (experimental) SDL video implementation for Win32
LOCALBASE        ?= /usr/local
QBASEDIR         ?= .# Default basedir for quake data files (Linux/BSD only)
TARGET_OS        ?= $(HOST_OS)
TARGET_UNIX      ?= $(if $(filter UNIX,$(TARGET_OS)),$(HOST_UNIX),)

# ============================================================================

.PHONY:	default clean

# ============================================================================

# ---------------------------------------
# Attempt detection of the build host OS
# ---------------------------------------

SYSNAME := $(shell uname -s)
TOPDIR := $(shell pwd)

ifneq (,$(findstring MINGW32,$(SYSNAME)))
HOST_OS = WIN32
else
ifneq (,$(findstring $(SYSNAME),FreeBSD NetBSD OpenBSD))
HOST_OS = UNIX
HOST_UNIX = bsd
else
ifneq (,$(findstring $(SYSNAME),Darwin))
HOST_OS = UNIX
HOST_UNIX = darwin
else
ifneq (,$(findstring $(SYSNAME),Linux))
HOST_OS = UNIX
HOST_UNIX = linux
else
$(error OS type not detected.)
endif
endif
endif
endif

# --------------------------------------------------------------------
# Setup driver options, choosing sensible defaults based on target OS
# --------------------------------------------------------------------

ifeq ($(TARGET_OS),UNIX)
EXT =
VID_TARGET ?= x11
IN_TARGET ?= x11
ifeq ($(TARGET_UNIX),darwin)
CD_TARGET ?= null
SND_TARGET ?= null
USE_XF86DGA ?= N
endif
ifeq ($(TARGET_UNIX),bsd)
CD_TARGET ?= bsd
SND_TARGET ?= linux
USE_XF86DGA ?= Y
endif
ifeq ($(TARGET_UNIX),linux)
CD_TARGET ?= linux
SND_TARGET ?= linux
USE_XF86DGA ?= Y
endif
endif

ifeq ($(TARGET_OS),WIN32)
EXT = .exe
CD_TARGET ?= win
SND_TARGET ?= win
ifeq ($(USE_SDL),Y)
VID_TARGET ?= sdl
IN_TARGET ?= sdl
else
VID_TARGET ?= win
IN_TARGET ?= win
endif
endif

# --------------------------------------------------------------
# Executable file extension and possible cross-compiler options
# --------------------------------------------------------------

ifeq ($(TARGET_OS),WIN32)
EXT = .exe
ifneq ($(HOST_OS),WIN32)
TARGET ?= $(MINGW_CROSS_GUESS)
CC = $(TARGET)-gcc
STRIP = $(TARGET)-strip
WINDRES = $(TARGET)-windres
endif
else
EXT =
endif

# ============================================================================
# Helper functions
# ============================================================================

# ---------------------------------------------------
# Remove duplicates from a list, preserving ordering
# ---------------------------------------------------
# (I wonder if Make optimises the tail recursion here?)

filter-dups = $(if $(1),$(firstword $(1)) $(call filter-dups,$(filter-out $(firstword $(1)),$(1))),)

# ----------------------------------------------
# Try to guess the location of X11 includes/libs
# ----------------------------------------------

# $(1) - header file to search for
# $(2) - library name to search for
# $(3) - list of directories to search in
IGNORE_DIRS = /usr $(LOCALBASE)
find-localbase = $(shell \
	for DIR in $(IGNORE_DIRS); do \
            if [ -e $$DIR/include/$(1) ] && \
		[ -e $$DIR/lib/lib$(2).a ] || \
		[ -e $$DIR/lib/lib$(2).la ] || \
		[ -e $$DIR/lib/lib$(2).dylib ]; then exit 0; fi; \
	done; \
	for DIR in $(3); do \
            if [ -e $$DIR/include/$(1) ] && \
		[ -e $$DIR/lib/lib$(2).a ] || \
		[ -e $$DIR/lib/lib$(2).la ] || \
		[ -e $$DIR/lib/lib$(2).dylib ]; then echo $$DIR; exit 0; fi; \
	done )

X11DIRS = /usr/X11R7 /usr/local/X11R7 /usr/X11R6 /usr/local/X11R6 /opt/X11 /opt/local
X11BASE_GUESS := $(call find-localbase,X11/Xlib.h,X11,$(X11DIRS))
X11BASE ?= $(X11BASE_GUESS)

SDLDIRS = /opt/local
SDLBASE_GUESS := $(call find-localbase,SDL/SDL.h,SDL,$(SDLDIRS))
SDLBASE ?= $(SDLBASE_GUESS)

# ------------------------------------------------------------------------
# Try to guess the MinGW cross compiler executables
# - I've seen i386-mingw32msvc, i586-mingw32msvc (Debian) and now
#   i486-mingw32 (Arch).
# ------------------------------------------------------------------------

MINGW_CROSS_GUESS := $(shell \
	if which i486-mingw32-gcc > /dev/null 2>&1; then \
		echo i486-mingw32; \
	elif which i586-mingw32msvc-gcc > /dev/null 2>&1; then \
		echo i586-mingw32msvc; \
	else \
		echo i386-mingw32msvc; \
	fi)

# --------------------------------
# GCC version and option checking
# --------------------------------

cc-version = $(shell sh $(TOPDIR)/scripts/gcc-version \
              $(if $(1), $(1), $(CC)))

cc-option = $(shell if $(CC) $(CFLAGS) -Werror $(1) -S -o /dev/null -xc /dev/null \
             > /dev/null 2>&1; then echo "$(1)"; else echo "$(2)"; fi ;)

cc-i386 = $(if $(subst __i386,,$(shell echo __i386 | $(CC) -E -xc - | tail -n 1)),Y,N)

GCC_VERSION := $(call cc-version,)
I386_GUESS  := $(call cc-i386)

# -------------------------
# Special include/lib dirs
# -------------------------
DX_INC    = $(TOPDIR)/dxsdk/sdk/inc
ST_INC    = $(TOPDIR)/scitech/include
ST_LIBDIR = scitech/lib/win32/vc

# ---------------------------------------
# Define some build variables
# ---------------------------------------

STRIP   ?= strip
WINDRES ?= windres

CFLAGS ?=
CFLAGS := $(CFLAGS) -Wall -Wno-trigraphs

ifeq ($(DEBUG),Y)
CFLAGS += -g
else
ifeq ($(OPTIMIZED_CFLAGS),Y)
CFLAGS += -O2
# -funit-at-a-time is buggy for MinGW GCC > 3.2
# I'm assuming it's fixed for MinGW GCC >= 4.0 when that comes about
CFLAGS += $(shell if [ $(GCC_VERSION) -lt 0400 ] ;\
		then echo $(call cc-option,-fno-unit-at-a-time,); fi ;)
CFLAGS += $(call cc-option,-fweb,)
CFLAGS += $(call cc-option,-frename-registers,)
CFLAGS += $(call cc-option,-ffast-math,)
endif
endif

# --------------------------------------------------------------------------
#  Each binary needs to build it's own object files in separate directories
#  due to the {NQ,QW}_HACK ifdefs still present in the common files.
# --------------------------------------------------------------------------

# (sw = software renderer, gl = OpenGL renderer, sv = server)
NQSWDIR	= $(BUILD_DIR)/nqsw
NQGLDIR	= $(BUILD_DIR)/nqgl
QWSWDIR	= $(BUILD_DIR)/qwsw
QWGLDIR	= $(BUILD_DIR)/qwgl
QWSVDIR	= $(BUILD_DIR)/qwsv

APPS =	tyr-quake$(EXT) tyr-glquake$(EXT) \
	tyr-qwcl$(EXT) tyr-glqwcl$(EXT) \
	tyr-qwsv$(EXT)

default:	all

all:	$(patsubst %,$(BIN_DIR)/%,$(APPS))

# To make warnings more obvious, be less verbose as default
# Use 'make V=1' to see the full commands
ifdef V
  quiet =
else
  quiet = quiet_
endif

quiet_cmd_mkdir = '  MKDIR   $(@D)'
      cmd_mkdir = mkdir -p $(@D)

define do_mkdir
	@if [ ! -d $(@D) ]; then \
		echo $($(quiet)cmd_mkdir); \
		$(cmd_mkdir); \
	fi
endef

# cmd_fixdep => Turn all pre-requisites into targets with no commands, to
# prevent errors when moving files around between builds (e.g. from NQ or QW
# dirs to the common dir.)
cmd_fixdep = \
	cp $(@D)/.$(@F).d $(@D)/.$(@F).d.tmp ; \
	sed -e 's/\#.*//' -e 's/^[^:]*: *//' -e 's/ *\\$$//' -e '/^$$/ d' \
	    -e 's/$$/ :/' < $(@D)/.$(@F).d.tmp >> $(@D)/.$(@F).d ; \
	rm -f $(@D)/.$(@F).d.tmp

cmd_cc_dep_c = \
	$(CC) -MM -MT $@ $(CPPFLAGS) -o $(@D)/.$(@F).d $< ; \
	$(cmd_fixdep)

quiet_cmd_cc_o_c = '  CC      $@'
      cmd_cc_o_c = $(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

define do_cc_o_c
	@$(do_mkdir);
	@$(cmd_cc_dep_c);
	@echo $($(quiet)cmd_cc_o_c);
	@$(cmd_cc_o_c);
endef

cmd_cc_dep_rc = \
	$(CC) -x c-header -MM -MT $@ $(CPPFLAGS) -o $(@D)/.$(@F).d $< ; \
	$(cmd_fixdep)

quiet_cmd_windres_res_rc = '  WINDRES $@'
      cmd_windres_res_rc = $(WINDRES) -I $(<D) -i $< -O coff -o $@

define do_windres_res_rc
	@$(do_mkdir);
	@$(cmd_cc_dep_rc);
	@echo $($(quiet)cmd_windres_res_rc);
	@$(cmd_windres_res_rc);
endef

quiet_cmd_cc_link = '  LINK    $@'
      cmd_cc_link = $(CC) -o $@ $^ $(1)

define do_cc_link
	@$(do_mkdir);
	@echo $($(quiet)cmd_cc_link);
	@$(call cmd_cc_link,$(1))
endef

quiet_cmd_strip = '  STRIP   $(1)'
      cmd_strip = $(STRIP) $(1)

ifeq ($(DEBUG),Y)
do_strip=
else
ifeq ($(STRIP),)
do_strip=
else
define do_strip
	@echo $(call $(quiet)cmd_strip,$(1));
	@$(call cmd_strip,$(1));
endef
endif
endif

DEPFILES = \
	$(wildcard $(NQSWDIR)/.*.d) \
	$(wildcard $(NQGLDIR)/.*.d) \
	$(wildcard $(QWSWDIR)/.*.d) \
	$(wildcard $(QWGLDIR)/.*.d) \
	$(wildcard $(QWSVDIR)/.*.d)

ifneq ($(DEPFILES),)
-include $(DEPFILES)
endif

$(NQSWDIR)/%.o:	CPPFLAGS = $(ALL_NQSW_CPPFLAGS)
$(NQGLDIR)/%.o:	CPPFLAGS = $(ALL_NQGL_CPPFLAGS)
$(QWSWDIR)/%.o:	CPPFLAGS = $(ALL_QWSW_CPPFLAGS)
$(QWGLDIR)/%.o:	CPPFLAGS = $(ALL_QWGL_CPPFLAGS)
$(QWSVDIR)/%.o:	CPPFLAGS = $(ALL_QWSV_CPPFLAGS)

$(NQSWDIR)/%.o:		common/%.S	; $(do_cc_o_c)
$(NQSWDIR)/%.o:		NQ/%.S		; $(do_cc_o_c)
$(NQSWDIR)/%.o:		common/%.c	; $(do_cc_o_c)
$(NQSWDIR)/%.o:		NQ/%.c		; $(do_cc_o_c)
$(NQSWDIR)/%.res:	common/%.rc	; $(do_windres_res_rc)
$(NQSWDIR)/%.res:	NQ/%.rc		; $(do_windres_res_rc)

$(NQGLDIR)/%.o:		common/%.S	; $(do_cc_o_c)
$(NQGLDIR)/%.o:		NQ/%.S		; $(do_cc_o_c)
$(NQGLDIR)/%.o:		common/%.c	; $(do_cc_o_c)
$(NQGLDIR)/%.o:		NQ/%.c		; $(do_cc_o_c)
$(NQGLDIR)/%.res:	common/%.rc	; $(do_windres_res_rc)
$(NQGLDIR)/%.res:	NQ/%.rc		; $(do_windres_res_rc)

$(QWSWDIR)/%.o:		common/%.S	; $(do_cc_o_c)
$(QWSWDIR)/%.o:		QW/client/%.S	; $(do_cc_o_c)
$(QWSWDIR)/%.o:		QW/common/%.S	; $(do_cc_o_c)
$(QWSWDIR)/%.o:		common/%.c	; $(do_cc_o_c)
$(QWSWDIR)/%.o:		QW/client/%.c	; $(do_cc_o_c)
$(QWSWDIR)/%.o:		QW/common/%.c	; $(do_cc_o_c)
$(QWSWDIR)/%.res:	common/%.rc	; $(do_windres_res_rc)
$(QWSWDIR)/%.res:	QW/client/%.rc	; $(do_windres_res_rc)

$(QWGLDIR)/%.o:		common/%.S	; $(do_cc_o_c)
$(QWGLDIR)/%.o:		QW/client/%.S	; $(do_cc_o_c)
$(QWGLDIR)/%.o:		QW/common/%.S	; $(do_cc_o_c)
$(QWGLDIR)/%.o:		common/%.c	; $(do_cc_o_c)
$(QWGLDIR)/%.o:		QW/client/%.c	; $(do_cc_o_c)
$(QWGLDIR)/%.o:		QW/common/%.c	; $(do_cc_o_c)
$(QWGLDIR)/%.res:	common/%.rc	; $(do_windres_res_rc)
$(QWGLDIR)/%.res:	QW/client/%.rc	; $(do_windres_res_rc)

$(QWSVDIR)/%.o:		QW/server/%.S	; $(do_cc_o_c)
$(QWSVDIR)/%.o:		QW/common/%.S	; $(do_cc_o_c)
$(QWSVDIR)/%.o:		common/%.S	; $(do_cc_o_c)
$(QWSVDIR)/%.o:		QW/server/%.c	; $(do_cc_o_c)
$(QWSVDIR)/%.o:		QW/common/%.c	; $(do_cc_o_c)
$(QWSVDIR)/%.o:		common/%.c	; $(do_cc_o_c)

# ----------------------------------
# Output the build Options: Output
# ----------------------------------
$(info Compile Options:)
$(info .        DEBUG = $(DEBUG))
$(info .    TARGET_OS = $(TARGET_OS))
$(info .  TARGET_UNIX = $(TARGET_UNIX))
$(info .  USE_X86_ASM = $(USE_X86_ASM))
$(info .    CD_TARGET = $(CD_TARGET))
$(info .   SND_TARGET = $(SND_TARGET))
$(info .   VID_TARGET = $(VID_TARGET))
$(info .    IN_TARGET = $(IN_TARGET))
$(info .  USE_XF86DGA = $(USE_XF86DGA))

# ============================================================================
# Object Files, libraries and options
# ============================================================================
#
# Provide a set of makefile variables to which we can attach lists of object
# files, libraries to link against, preprocessor and linker options, etc. The
# prefixes tell us to which targets the variables apply:
#
# Shared lists:
#  COMMON_ - objects common to all five targets
#  CL_     - objects common to the four client targets (NQ, QW, SW & GL)
#  SV_     - objects common to the three server targets (NQ SW&GL, QWSV)
#  NQCL_   - objects common to the NQ client targets (SW & GL)
#  QWCL_   - objects common to the QW client targets (SW & GL)
#  QW_     - objects common to the QW targets (CL & SV)
#  SW_     - objects common to the software rendering clients
#  GL_     - objects common to the OpenGL rendering clients
#
# Target specific lists:
#  NQSW_
#  NQGL_
#  QWSW_
#  QWGL_
#  QWSV_
#
# The suffix describes where the list is used
#  _OBJS     - list of object files used as dependency and for linker
#  _LIBS     - list of libs to pass to the linker
#  _CPPFLAGS - C preprocessor flags, e.g. include directories or defines
#  _LFLAGS   - linker flags, e.g. library directories
#
# Then we have configuration options which will alter the content of the lists:
# - VID_TARGET  - video driver
# - IN_TARGET   - input driver (usually tied to video driver)
# - CD_TARGET   - cd audio driver
# - SND_TARGET  - sound driver
# - USE_X86_ASM - conditionally replace C code with assembly
#
#
# 1. Set up lists of object files which aren't affected by config options
# 2. Go through the various config options and append to the appropriate lists
#
#
# TODO: Think about different groupings?
#       e.g. SW_RENDER_OBJS, GL_RENDER_OBJS, NET_DRIVER_OBJS...
#
# --------------------------------------------------------------------------
# Object File Lists - Static wrt. config options
# --------------------------------------------------------------------------

COMMON_OBJS := \
	cmd.o		\
	common.o	\
	crc.o		\
	cvar.o		\
	mathlib.o	\
	rb_tree.o	\
	shell.o		\
	zone.o

CL_OBJS := \
	cd_common.o	\
	cl_demo.o	\
	cl_input.o	\
	cl_main.o	\
	cl_parse.o	\
	cl_tent.o	\
	console.o	\
	keys.o		\
	menu.o		\
	r_efrag.o	\
	r_part.o	\
	sbar.o		\
	snd_dma.o	\
	snd_mem.o	\
	snd_mix.o	\
	view.o		\
	wad.o

SV_OBJS := \
	pr_cmds.o	\
	pr_edict.o	\
	pr_exec.o	\
	sv_main.o	\
	sv_move.o	\
	sv_phys.o	\
	sv_user.o

NQCL_OBJS := \
	chase.o		\
	host.o		\
	host_cmd.o	\
	net_dgrm.o	\
	net_loop.o	\
	net_main.o	\
	world.o

QWCL_OBJS := \
	cl_cam.o	\
	cl_ents.o	\
	cl_pred.o	\
	skin.o

QW_OBJS := \
	md4.o		\
	net_chan.o	\
	pmove.o		\
	pmovetst.o

SW_OBJS := \
	d_edge.o	\
	d_fill.o	\
	d_init.o	\
	d_modech.o	\
	d_part.o	\
	d_polyse.o	\
	d_scan.o	\
	d_sky.o		\
	d_sprite.o	\
	d_surf.o	\
	d_vars.o	\
	d_zpoint.o	\
	draw.o		\
	model.o		\
	r_aclip.o	\
	r_alias.o	\
	r_bsp.o		\
	r_draw.o	\
	r_edge.o	\
	r_light.o	\
	r_main.o	\
	r_misc.o	\
	r_sky.o		\
	r_sprite.o	\
	r_surf.o	\
	r_vars.o	\
	screen.o

GL_OBJS := \
	drawhulls.o	\
	gl_draw.o	\
	gl_mesh.o	\
	gl_model.o	\
	gl_rlight.o	\
	gl_rmain.o	\
	gl_rmisc.o	\
	gl_rsurf.o	\
	gl_screen.o	\
	gl_warp.o

NQSW_OBJS :=

NQGL_OBJS :=

QWSW_OBJS :=

QWGL_OBJS := \
	gl_ngraph.o

QWSV_OBJS := \
	sv_ccmds.o	\
	sv_ents.o	\
	sv_init.o	\
	sv_nchan.o	\
	sv_send.o	\
	world.o

# ----------------------------------------------------------------------------
# Start off the CPPFLAGS, config independent stuff
# ----------------------------------------------------------------------------

# Defines
COMMON_CPPFLAGS += -DTYR_VERSION=$(TYR_VERSION) -DQBASEDIR="$(QBASEDIR)"
NQCL_CPPFLAGS   += -DNQ_HACK
QW_CPPFLAGS     += -DQW_HACK
QWSV_CPPFLAGS   += -DSERVERONLY
GL_CPPFLAGS     += -DGLQUAKE
ifeq ($(DEBUG),Y)
COMMON_CPPFLAGS += -DDEBUG
else
COMMON_CPPFLAGS += -DNDEBUG
endif

# Includes
COMMON_CPPFLAGS += -iquote $(TOPDIR)/include
ifneq ($(LOCALBASE),)
COMMON_CPPFLAGS += -idirafter $(LOCALBASE)/include
COMMON_LFLAGS += -L$(LOCALBASE)/lib
endif
NQCL_CPPFLAGS   += -iquote $(TOPDIR)/NQ
QW_CPPFLAGS     += -iquote $(TOPDIR)/QW/client
QWSV_CPPFLAGS   += -iquote $(TOPDIR)/QW/server

# ----------------------------------------------------------------------------
# Add objects depending whether using x86 assembly
# ----------------------------------------------------------------------------

ifeq ($(USE_X86_ASM),Y)
COMMON_CPPFLAGS += -DUSE_X86_ASM
CL_OBJS   += math.o snd_mixa.o sys_wina.o
NQCL_OBJS += worlda.o
SW_OBJS   += d_draw.o d_draw16.o d_parta.o d_polysa.o d_scana.o d_spr8.o \
	     d_varsa.o r_aclipa.o r_aliasa.o r_drawa.o r_edgea.o r_varsa.o \
	     surf8.o surf16.o
else
SW_OBJS += nonintel.o
endif

# ----------------------------------------------------------------------------
# Quick sanity check to make sure the lists have no overlap
# ----------------------------------------------------------------------------
dups-only = $(if $(1),$(if $(filter $(firstword $(1)),$(wordlist 2,$(words $(1)),$(1))),$(firstword $(1)),) $(call dups-only,$(filter-out $(firstword $(1)),$(1))),)
ALL_OBJS := $(COMMON_OBJS) $(CL_OBJS) $(SV_OBJS) $(NQCL_OBJS) $(QWCL_OBJS) \
	    $(QW_OBJS) $(SW_OBJS) $(GL_OBJS) $(NQSW_OBJS) $(NQGL_OBJS) \
	    $(QWSW_OBJS) $(QWGL_OBJS) $(QWSV_OBJS))
MSG_DUP = WARNING: Duplicate words detected in group
DUPS := $(strip $(call dups-only,$(ALL_OBJS)))
DUMMY := $(if $(DUPS),$(warning $(MSG_DUP): $(DUPS)),)

# ----------------------------------------------------------------------------
# FIXME - Bit of an ugly hack here because I know there is a duplicate model.c
#         file for QWSV which isn't the same thing as the one used for the sw
#         clients. Probably the clients should be refactored to have alias and
#         bsp models in separate modules so the bsp model code can be shared.
# ----------------------------------------------------------------------------
QWSV_OBJS += model.o

# ----------------------------------------------------------------------------
# Target OS Options
# ----------------------------------------------------------------------------

ifeq ($(TARGET_OS),WIN32)
COMMON_OBJS += net_wins.o sys_win.o
CL_OBJS     += winquake.res
NQCL_OBJS   += conproc.o net_win.o
COMMON_LIBS += wsock32 winmm dxguid
GL_LIBS     += opengl32
ifeq ($(DEBUG),Y)
CL_LFLAGS += -mconsole
else
CL_LFLAGS += -mwindows
endif
QWSV_LFLAGS += -mconsole
endif
ifeq ($(TARGET_OS),UNIX)
COMMON_CPPFLAGS += -DELF
COMMON_OBJS += net_udp.o
COMMON_LIBS += m
CL_OBJS     += sys_linux.o
NQCL_OBJS   += net_bsd.o
QWSV_OBJS   += sys_unix.o
GL_LIBS     += GL
endif

# ----------------------------------------------------------------------------
# Driver Options
#   NOTE: there is some duplication that may happen here, e.g. adding common
#         libs/objs multiple times. We will strip out duplicates later.
# ----------------------------------------------------------------------------

# ----------------
# 1. CD driver
# ----------------

ifeq ($(CD_TARGET),null)
CL_OBJS += cd_null.o
endif
ifeq ($(CD_TARGET),linux)
CL_OBJS += cd_linux.o
endif
ifeq ($(CD_TARGET),bsd)
CL_OBJS += cd_bsd.o
endif
ifeq ($(CD_TARGET),win)
CL_OBJS += cd_win.o
endif

# ----------------
# 2. Sound driver
# ----------------

ifeq ($(SND_TARGET),null)
CL_OBJS += snd_null.o
endif
ifeq ($(SND_TARGET),win)
CL_CPPFLAGS += -idirafter $(DX_INC)
CL_OBJS += snd_win.o
# FIXME - direct sound libs?
endif
ifeq ($(SND_TARGET),linux)
CL_OBJS += snd_linux.o
endif

# ----------------
# 3. Video driver
# ----------------

ifeq ($(VID_TARGET),x11)
CL_CPPFLAGS += -DX11
CL_OBJS += x11_core.o
SW_OBJS += vid_x.o
GL_OBJS += gl_vidlinuxglx.o
CL_LIBS += X11 Xext Xxf86vm
ifeq ($(USE_XF86DGA),Y)
CL_CPPFLAGS += -DUSE_XF86DGA
CL_LIBS += Xxf86dga
endif
ifneq ($(X11BASE),)
CL_CPPFLAGS += -idirafter $(X11BASE)/include
CL_LFLAGS += -L$(X11BASE)/lib
endif
endif
ifeq ($(VID_TARGET),win)
CL_CPPFLAGS += -idirafter $(DX_INC)
SW_CPPFLAGS += -idirafter $(ST_INC)
SW_OBJS += vid_win.o
GL_OBJS += gl_vidnt.o
SW_LIBS += mgllt gdi32 # gdi32 needs to come after mgllt
GL_LIBS += gdi32
GL_LIBS += comctl32
SW_LFLAGS += -L$(ST_LIBDIR)
endif
ifeq ($(VID_TARGET),sdl)
SW_OBJS += vid_sdl.o
GL_OBJS += vid_sgl.o
CL_LIBS += SDL
ifneq ($(SDLBASE),)
CL_CPPFLAGS += -idirafter $(SDLBASE)/include
CL_LFLAGS += -L$(SDLBASE)/lib
endif
endif

# ----------------
# 4. Input driver
# ----------------
# TODO: is it worth allowing input and video to be specified separately?
#       they can be pretty tightly bound...

ifeq ($(IN_TARGET),x11)
CL_OBJS += x11_core.o in_x11.o
CL_LIBS += X11
ifneq ($(X11BASE),)
CL_LFLAGS += -L$(X11BASE)/lib
endif
endif
ifeq ($(IN_TARGET),win)
CL_OBJS += in_win.o
endif
ifeq ($(IN_TARGET),sdl)
CL_OBJS += in_sdl.o
endif

# ----------------------------------------------------------------------------
# Combining the lists
# ----------------------------------------------------------------------------

nqsw-list = $(COMMON_$(1)) $(CL_$(1)) $(SV_$(1)) $(NQCL_$(1)) $(SW_$(1)) $(NQSW_$(1))
nqgl-list = $(COMMON_$(1)) $(CL_$(1)) $(SV_$(1)) $(NQCL_$(1)) $(GL_$(1)) $(NQGL_$(1))
qwsw-list = $(COMMON_$(1)) $(CL_$(1)) $(QW_$(1)) $(QWCL_$(1)) $(SW_$(1)) $(QWSW_$(1))
qwgl-list = $(COMMON_$(1)) $(CL_$(1)) $(QW_$(1)) $(QWCL_$(1)) $(GL_$(1)) $(QWGL_$(1))
qwsv-list = $(COMMON_$(1)) $(SV_$(1)) $(QW_$(1)) $(QWSV_$(1))

ALL_NQSW_CPPFLAGS := $(call nqsw-list,CPPFLAGS)
ALL_NQGL_CPPFLAGS := $(call nqgl-list,CPPFLAGS)
ALL_QWSW_CPPFLAGS := $(call qwsw-list,CPPFLAGS)
ALL_QWGL_CPPFLAGS := $(call qwgl-list,CPPFLAGS)
ALL_QWSV_CPPFLAGS := $(call qwsv-list,CPPFLAGS)

ALL_NQSW_OBJS := $(sort $(call nqsw-list,OBJS))
ALL_NQGL_OBJS := $(sort $(call nqgl-list,OBJS))
ALL_QWSW_OBJS := $(sort $(call qwsw-list,OBJS))
ALL_QWGL_OBJS := $(sort $(call qwgl-list,OBJS))
ALL_QWSV_OBJS := $(sort $(call qwsv-list,OBJS))

ALL_NQSW_LIBS := $(call filter-dups,$(call nqsw-list,LIBS))
ALL_NQGL_LIBS := $(call filter-dups,$(call nqgl-list,LIBS))
ALL_QWSW_LIBS := $(call filter-dups,$(call qwsw-list,LIBS))
ALL_QWGL_LIBS := $(call filter-dups,$(call qwgl-list,LIBS))
ALL_QWSV_LIBS := $(call filter-dups,$(call qwsv-list,LIBS))

ALL_NQSW_LFLAGS := $(call filter-dups,$(call nqsw-list,LFLAGS))
ALL_NQGL_LFLAGS := $(call filter-dups,$(call nqgl-list,LFLAGS))
ALL_QWSW_LFLAGS := $(call filter-dups,$(call qwsw-list,LFLAGS))
ALL_QWGL_LFLAGS := $(call filter-dups,$(call qwgl-list,LFLAGS))
ALL_QWSV_LFLAGS := $(call filter-dups,$(call qwsv-list,LFLAGS))

ALL_NQSW_LFLAGS += $(patsubst %,-l%,$(ALL_NQSW_LIBS))
ALL_NQGL_LFLAGS += $(patsubst %,-l%,$(ALL_NQGL_LIBS))
ALL_QWSW_LFLAGS += $(patsubst %,-l%,$(ALL_QWSW_LIBS))
ALL_QWGL_LFLAGS += $(patsubst %,-l%,$(ALL_QWGL_LIBS))
ALL_QWSV_LFLAGS += $(patsubst %,-l%,$(ALL_QWSV_LIBS))

# ============================================================================
# Build Rules
# ============================================================================

$(BIN_DIR)/tyr-quake$(EXT):	$(patsubst %,$(NQSWDIR)/%,$(ALL_NQSW_OBJS))
	$(call do_cc_link,$(ALL_NQSW_LFLAGS))
	$(call do_strip,$@)

$(BIN_DIR)/tyr-glquake$(EXT):	$(patsubst %,$(NQGLDIR)/%,$(ALL_NQGL_OBJS))
	$(call do_cc_link,$(ALL_NQGL_LFLAGS))
	$(call do_strip,$@)

$(BIN_DIR)/tyr-qwcl$(EXT):	$(patsubst %,$(QWSWDIR)/%,$(ALL_QWSW_OBJS))
	$(call do_cc_link,$(ALL_QWSW_LFLAGS))
	$(call do_strip,$@)

$(BIN_DIR)/tyr-glqwcl$(EXT):	$(patsubst %,$(QWGLDIR)/%,$(ALL_QWGL_OBJS))
	$(call do_cc_link,$(ALL_QWGL_LFLAGS))
	$(call do_strip,$@)

$(BIN_DIR)/tyr-qwsv$(EXT):	$(patsubst %,$(QWSVDIR)/%,$(ALL_QWSV_OBJS))
	$(call do_cc_link,$(ALL_QWSV_LFLAGS))
	$(call do_strip,$@)

# ----------------------------------------------------------------------------
# Very basic clean target (can't use xargs on MSYS)
# ----------------------------------------------------------------------------

# Main clean function...
clean:
	@rm -rf $(BUILD_DIR)
	@rm -rf $(BIN_DIR)
	@rm -f $(shell find . \( \
		-name '*~' -o -name '#*#' -o -name '*.o' -o -name '*.res' \
	\) -print)
