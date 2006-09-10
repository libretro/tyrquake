#
# A Makefile to run under MinGW on Win32
#
# Pretty crappy, but you can build all the targets if you clean in
# between. It's needed because various -DXXX flags change the object files and
# I'm still working on doing it properly... but slowly.
#

TYR_VERSION_MAJOR = 0
TYR_VERSION_MINOR = 50
TYR_VERSION_BUILD =

TYR_VERSION = $(TYR_VERSION_MAJOR).$(TYR_VERSION_MINOR)$(TYR_VERSION_BUILD)

# ============================================================================
# Choose the target here: (Yes, I know it sucks).
# ============================================================================

TARGET_APP = NQ
#TARGET_APP = QW
#TARGET_APP = QWSV

#TARGET_RENDER = SW
TARGET_RENDER = GL

#TARGET_OS = WIN32
TARGET_OS = LINUX

#DEBUG=y 	# Compile with debug info
#NO_X86_ASM=y	# Compile with no x86 asm

# ============================================================================

# EXPORT ALL VARIABLES
export

.PHONY:	default clean \
	nq-w32-sw-objs nq-w32-gl-objs qw-w32-sw-objs qw-w32-gl-objs \
	qwsv-w32-objs \
	nq-linux-sw-objs nq-linux-gl-objs qw-linux-sw-objs qw-linux-gl-objs \
	qwsv-linux-objs

# ============================================================================
# Helper functions
# ============================================================================

check_gcc = $(shell if $(CC) $(CFLAGS) $(1) -S -o /dev/null -xc /dev/null > /dev/null 2>&1; then echo "$(1)"; else echo "$(2)"; fi ;)

# ============================================================================

# FIXME - how to detect build env reliably...?
ifeq ($(OSTYPE),msys)
TOPDIR := $(shell pwd -W)
else
TOPDIR := $(shell pwd)
endif

# ----------------------------
# The two project directories
# ----------------------------

NQ_DIR = NQ
QW_DIR = QW

# ----------------------------
# Include dirs compiler flags
# ----------------------------
DX_INC    = $(TOPDIR)/dxsdk/sdk/inc
ST_INC    = $(TOPDIR)/scitech/include

WIN32_SW_INC = $(DX_INC) $(ST_INC)
WIN32_GL_INC = $(DX_INC)

QWSV_INC  = $(TOPDIR)/$(QW_DIR)/server $(TOPDIR)/$(QW_DIR)/client

WIN32_SW_IFLAGS := -idirafter $(DX_INC)
WIN32_SW_IFLAGS += -idirafter $(ST_INC)
WIN32_SW_IFLAGS += -I$(TOPDIR)/include

WIN32_GL_IFLAGS := -idirafter $(DX_INC)
WIN32_GL_IFLAGS += -I$(TOPDIR)/include

QWSV_WIN32_IFLAGS  = $(patsubst %,-idirafter %,$(QWSV_INC)) -I$(TOPDIR)/include
QWSV_LINUX_IFLAGS  = $(patsubst %,-idirafter %,$(QWSV_INC)) -I$(TOPDIR)/include

# ------------------------------
# define flags
# ------------------------------
DFLAGS := -DTYR_VERSION=$(TYR_VERSION)

# App specific hacks
ifeq ($(TARGET_APP),NQ)
DFLAGS += -DNQ_HACK
endif
ifeq ($(TARGET_APP),QW)
DFLAGS += -DQW_HACK
endif
ifeq ($(TARGET_APP),QWSV)
DFLAGS += -DQW_HACK -DSERVERONLY
endif

ifeq ($(TARGET_RENDER),GL)
DFLAGS += -DGLQUAKE
endif

ifeq ($(TARGET_OS),WIN32)
DFLAGS += -DWIN32 -D_WIN32
endif

ifdef DEBUG
DFLAGS += -DDEBUG
else
DFLAGS += -DNDEBUG
endif

ifdef NO_X86_ASM
DFLAGS += -U__i386__
endif

NQ_LINUX_SW_DFLAGS = $(DFLAGS) -DX11 -DELF -I$(TOPDIR)/include
QW_LINUX_SW_DFLAGS = $(DFLAGS) -DX11 -DELF -I$(TOPDIR)/include
NQ_LINUX_GL_DFLAGS = $(DFLAGS) -DX11 -DELF -I$(TOPDIR)/include
QW_LINUX_GL_DFLAGS = $(DFLAGS) -DX11 -DELF -DGL_EXT_SHARED -I$(TOPDIR)/include

# -------------------------
# Set up the various FLAGS
# -------------------------

# NQ - Normal Quake
ifeq ($(TARGET_APP),NQ)
ifeq ($(TARGET_RENDER),SW)
ifeq ($(TARGET_OS),WIN32)
CPPFLAGS = $(DFLAGS) $(WIN32_SW_IFLAGS) -I$(TOPDIR)/NQ
endif
ifeq ($(TARGET_OS),LINUX)
CPPFLAGS = $(NQ_LINUX_SW_DFLAGS) -I$(TOPDIR)/NQ
endif
endif
ifeq ($(TARGET_RENDER),GL)
ifeq ($(TARGET_OS),WIN32)
CPPFLAGS = $(DFLAGS) $(WIN32_GL_IFLAGS) -I$(TOPDIR)/NQ
endif
ifeq ($(TARGET_OS),LINUX)
CPPFLAGS = $(NQ_LINUX_GL_DFLAGS) $(NQ_LINUX_GL_IFLAGS) -I$(TOPDIR)/NQ
endif
endif
endif

# QW - QuakeWorld Client
ifeq ($(TARGET_APP),QW)
ifeq ($(TARGET_RENDER),SW)
ifeq ($(TARGET_OS),WIN32)
CPPFLAGS = $(DFLAGS) $(WIN32_SW_IFLAGS) -I$(TOPDIR)/$(QW_DIR)/client -I$(TOPDIR)/$(QW_DIR)/common
endif
ifeq ($(TARGET_OS),LINUX)
CPPFLAGS = $(QW_LINUX_SW_DFLAGS) -I$(TOPDIR)/$(QW_DIR)/client -I$(TOPDIR)/$(QW_DIR)/common
endif
endif
ifeq ($(TARGET_RENDER),GL)
ifeq ($(TARGET_OS),WIN32)
CPPFLAGS = $(DFLAGS) $(WIN32_GL_IFLAGS) -I$(TOPDIR)/$(QW_DIR)/client -I$(TOPDIR)/$(QW_DIR)/common
endif
ifeq ($(TARGET_OS),LINUX)
CPPFLAGS = $(QW_LINUX_GL_DFLAGS) $(QW_LINUX_GL_IFLAGS) -I$(TOPDIR)/$(QW_DIR)/client -I$(TOPDIR)/$(QW_DIR)/common
endif
endif
endif

# QWSV - QuakeWorld Server
ifeq ($(TARGET_APP),QWSV)
ifeq ($(TARGET_OS),WIN32)
CPPFLAGS = $(DFLAGS) $(QWSV_WIN32_IFLAGS)
endif
ifeq ($(TARGET_OS),LINUX)
CPPFLAGS = $(DFLAGS) $(QWSV_LINUX_IFLAGS)
endif
endif

# ------------------------------------------------------
# Define the default target based on TARGET_* variables
# ------------------------------------------------------
DT_PREFIX =tyr-
ifeq ($(TARGET_RENDER),GL)
  DT_RENDER =gl
else
  DT_RENDER =
endif
ifeq ($(TARGET_APP),NQ)
  DT_APP =quake
endif
ifeq ($(TARGET_APP),QW)
  DT_APP =qwcl
endif
ifeq ($(TARGET_APP),QWSV)
  DT_APP =qwsv
endif
ifeq ($(TARGET_OS),WIN32)
  DT_EXT =.exe
else
  DT_EXT =
endif

DEFAULT_TARGET = $(DT_PREFIX)$(DT_RENDER)$(DT_APP)$(DT_EXT)

# --------------
# Library stuff
# --------------
WIN_LIBDIR = C:/mingw-1.1/lib
NQ_ST_LIBDIR = scitech/lib/win32/vc
QW_ST_LIBDIR = scitech/lib/win32/vc

NQ_W32_COMMON_LIBS = wsock32 winmm dxguid
NQ_W32_SW_LIBS = mgllt ddraw
NQ_W32_GL_LIBS = opengl32 comctl32

LINUX_X11_LIBDIR = /usr/X11R6/lib
NQ_LINUX_COMMON_LIBS = m X11 Xext Xxf86dga Xxf86vm
NQ_LINUX_GL_LIBS = GL

NQ_W32_SW_LFLAGS = -mwindows $(patsubst %,-l%,$(NQ_W32_SW_LIBS) $(NQ_W32_COMMON_LIBS))
NQ_W32_GL_LFLAGS = -mwindows $(patsubst %,-l%,$(NQ_W32_GL_LIBS) $(NQ_W32_COMMON_LIBS))
NQ_LINUX_SW_LFLAGS = $(patsubst %,-l%,$(NQ_LINUX_COMMON_LIBS))
NQ_LINUX_GL_LFLAGS = $(patsubst %,-l%,$(NQ_LINUX_COMMON_LIBS) $(NQ_LINUX_GL_LIBS))

# ---------------------------------------
# Define some build variables
# ---------------------------------------

CFLAGS := -Wall -Wno-trigraphs

# Enable this if you're getting pedantic again...
#ifeq ($(TARGET_OS),LINUX)
#CFLAGS += -Werror
#endif

ifdef DEBUG
CFLAGS += -g
STRIP_CMD = @echo "** Debug build - not stripping"
else
# Note that "-fomit-frame-pointer" seems to screw some things up
# (at least on MinGW)
CFLAGS += -O2
CFLAGS += $(call check_gcc,-fweb,)
CFLAGS += $(call check_gcc,-frename-registers,)
CFLAGS += $(call check_gcc,-mtune=i686,-mcpu=i686)
STRIP_CMD = strip
endif

# -------------------------------------
# Got to build something by default...
# -------------------------------------

default:	$(DEFAULT_TARGET)

# ---------------------------
# Tweak the implicit ruleset
# ---------------------------

%.o:	%.S
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

%.o:	%.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $^

# ----------------------------------------------------------------------------
# Normal Quake (NQ)
# ----------------------------------------------------------------------------

# Objects common to all versions of NQ, sources are c code
NQ_COMMON_C_OBJS = \
	chase.o		\
	cl_demo.o	\
	cl_input.o	\
	cl_main.o	\
	cl_parse.o	\
	cl_tent.o	\
	cmd.o		\
	common.o	\
	console.o	\
	crc.o		\
	cvar.o		\
	host.o		\
	host_cmd.o	\
	keys.o		\
	mathlib.o	\
	menu.o		\
	net_dgrm.o	\
	net_loop.o	\
	net_main.o	\
	net_vcr.o	\
	pr_cmds.o	\
	pr_edict.o	\
	pr_exec.o	\
	r_part.o	\
	\
	rb_tree.o	\
	\
	sbar.o		\
	\
	shell.o		\
	\
	snd_dma.o	\
	snd_mem.o	\
	snd_mix.o	\
	sv_main.o	\
	sv_move.o	\
	sv_phys.o	\
	sv_user.o	\
	view.o		\
	wad.o		\
	world.o		\
	zone.o

NQ_COMMON_ASM_OBJS = \
	math.o		\
	snd_mixa.o	\
	worlda.o

# Used in both SW and GL versions of NQ on the Win32 platform
NQ_W32_C_OBJS = \
	cd_win.o	\
	conproc.o	\
	in_win.o	\
	net_win.o	\
	net_wins.o	\
	net_wipx.o	\
	snd_win.o	\
	sys_win.o

NQ_W32_ASM_OBJS = \
	sys_wina.o

# Used in both SW and GL versions on NQ on the Linux platform
NQ_LINUX_C_OBJS = \
	cd_linux.o	\
	net_udp.o	\
	net_bsd.o	\
	snd_linux.o	\
	sys_linux.o	\
	x11_core.o	\
	in_x11.o

NQ_LINUX_ASM_OBJS = \
	sys_dosa.o

# Objects only used in software rendering versions of NQ
NQ_SW_C_OBJS = \
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
	r_efrag.o	\
	r_light.o	\
	r_main.o	\
	r_misc.o	\
	r_sky.o		\
	r_sprite.o	\
	r_surf.o	\
	r_vars.o	\
	screen.o

NQ_SW_ASM_OBJS = \
	d_draw.o	\
	d_draw16.o	\
	d_parta.o	\
	d_polysa.o	\
	d_scana.o	\
	d_spr8.o	\
	d_varsa.o	\
	r_aclipa.o	\
	r_aliasa.o	\
	r_drawa.o	\
	r_edgea.o	\
	r_varsa.o	\
	surf16.o	\
	surf8.o

# Objects only used in software rendering versions of NQ on the Win32 Platform
NQ_W32_SW_C_OBJS = \
	vid_win.o

NQ_W32_SW_ASM_OBJS = \
	dosasm.o

# Objects only used in software rendering versions of NQ on the Linux Platform
NQ_LINUX_SW_C_OBJS = \
	vid_x.o

NQ_LINUX_SW_AMS_OBJS =

# Objects only used in OpenGL rendering versions of NQ
NQ_GL_C_OBJS = \
	drawhulls.o	\
	gl_draw.o	\
	gl_mesh.o	\
	gl_model.o	\
	gl_refrag.o	\
	gl_rlight.o	\
	gl_rmain.o	\
	gl_rmisc.o	\
	gl_rsurf.o	\
	gl_screen.o	\
	gl_warp.o

NQ_GL_ASM_OBJS =

# Objects only used in OpenGL rendering versions of NQ on the Win32 Platform
NQ_W32_GL_C_OBJS = \
	gl_vidnt.o

NQ_W32_GL_ASM_OBJS =

# Objects only used in OpenGL rendering versions of NQ on the Linux Platform
NQ_LINUX_GL_C_OBJS = \
	gl_vidlinuxglx.o

NQ_LINUX_GL_ASM_OBJS =

# Misc objects that don't seem to get used...
NQ_OTHER_ASM_OBJS = \
	d_copy.o	\
	sys_dosa.o

NQ_OTHER_C_OBJS = 

# =========================================================================== #

# Build the list of object files for each particular target
# (*sigh* - something has to be done about this makefile...)

NQ_W32_SW_OBJS := $(NQ_COMMON_C_OBJS)
NQ_W32_SW_OBJS += $(NQ_SW_C_OBJS)
NQ_W32_SW_OBJS += $(NQ_W32_C_OBJS)
NQ_W32_SW_OBJS += $(NQ_W32_SW_C_OBJS)
NQ_W32_SW_OBJS += winquake.res
ifdef NO_X86_ASM
NQ_W32_SW_OBJS += nonintel.o
else
NQ_W32_SW_OBJS += $(NQ_COMMON_ASM_OBJS)
NQ_W32_SW_OBJS += $(NQ_SW_ASM_OBJS)
NQ_W32_SW_OBJS += $(NQ_W32_ASM_OBJS)
NQ_W32_SW_OBJS += $(NQ_W32_SW_ASM_OBJS)
endif

NQ_W32_GL_OBJS := $(NQ_COMMON_C_OBJS)
NQ_W32_GL_OBJS += $(NQ_GL_C_OBJS)
NQ_W32_GL_OBJS += $(NQ_W32_C_OBJS)
NQ_W32_GL_OBJS += $(NQ_W32_GL_C_OBJS)
NQ_W32_GL_OBJS += winquake.res
ifndef NO_X86_ASM
NQ_W32_GL_OBJS += $(NQ_COMMON_ASM_OBJS)
NQ_W32_GL_OBJS += $(NQ_GL_ASM_OBJS)
NQ_W32_GL_OBJS += $(NQ_W32_ASM_OBJS)
NQ_W32_GL_OBJS += $(NQ_W32_GL_ASM_OBJS)
endif

NQ_LINUX_SW_OBJS := $(NQ_COMMON_C_OBJS)
NQ_LINUX_SW_OBJS += $(NQ_SW_C_OBJS)
NQ_LINUX_SW_OBJS += $(NQ_LINUX_C_OBJS)
NQ_LINUX_SW_OBJS += $(NQ_LINUX_SW_C_OBJS)
ifdef NO_X86_ASM
NQ_LINUX_SW_OBJS += nonintel.o
else
NQ_LINUX_SW_OBJS += $(NQ_COMMON_ASM_OBJS)
NQ_LINUX_SW_OBJS += $(NQ_SW_ASM_OBJS)
NQ_LINUX_SW_OBJS += $(NQ_LINUX_ASM_OBJS)
NQ_LINUX_SW_OBJS += $(NQ_LINUX_SW_ASM_OBJS)
endif

NQ_LINUX_GL_OBJS := $(NQ_COMMON_C_OBJS)
NQ_LINUX_GL_OBJS += $(NQ_GL_C_OBJS)
NQ_LINUX_GL_OBJS += $(NQ_LINUX_C_OBJS)
NQ_LINUX_GL_OBJS += $(NQ_LINUX_GL_C_OBJS)
ifndef NO_X86_ASM
NQ_LINUX_GL_OBJS += $(NQ_COMMON_ASM_OBJS)
NQ_LINUX_GL_OBJS += $(NQ_GL_ASM_OBJS)
NQ_LINUX_GL_OBJS += $(NQ_LINUX_ASM_OBJS)
NQ_LINUX_GL_OBJS += $(NQ_LINUX_GL_ASM_OBJS)
endif

# ------------------------
# Now, the build rules...
# ------------------------

# Win32
nq-w32-sw-objs:
	$(MAKE) -C $(NQ_DIR) quake-sw-win32

nq-w32-gl-objs:
	$(MAKE) -C $(NQ_DIR) quake-gl-win32

tyr-quake.exe:	nq-w32-sw-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(patsubst %,$(NQ_DIR)/%,$(NQ_W32_SW_OBJS)) -L$(WIN_LIBDIR) -L$(NQ_ST_LIBDIR) $(NQ_W32_SW_LFLAGS)
	$(STRIP_CMD) $@

tyr-glquake.exe:	nq-w32-gl-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(patsubst %,$(NQ_DIR)/%,$(NQ_W32_GL_OBJS)) -L$(WIN_LIBDIR) -L$(NQ_ST_LIBDIR) $(NQ_W32_GL_LFLAGS)
	$(STRIP_CMD) $@

# Linux
nq-linux-sw-objs:
	$(MAKE) -C $(NQ_DIR) quake-sw-linux

nq-linux-gl-objs:
	$(MAKE) -C $(NQ_DIR) quake-gl-linux

tyr-quake:	nq-linux-sw-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-quake $(patsubst %,$(NQ_DIR)/%,$(NQ_LINUX_SW_OBJS)) -L$(LINUX_X11_LIBDIR) $(NQ_LINUX_SW_LFLAGS)
	$(STRIP_CMD) $@

tyr-glquake:	nq-linux-gl-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-glquake $(patsubst %,$(NQ_DIR)/%,$(NQ_LINUX_GL_OBJS)) -L$(LINUX_X11_LIBDIR) $(NQ_LINUX_GL_LFLAGS)
	$(STRIP_CMD) $@


# ----------------------------------------------------------------------------
# QuakeWorld (QW) - Client
# ----------------------------------------------------------------------------

QW_SV_SHARED_C_OBJS = \
	cmd.o		\
	common.o	\
	crc.o		\
	cvar.o		\
	mathlib.o	\
	md4.o		\
	net_chan.o	\
	pmove.o		\
	pmovetst.o	\
	rb_tree.o	\
	shell.o		\
	zone.o

QW_COMMON_C_OBJS = \
	$(QW_SV_SHARED_C_OBJS) \
	cl_cam.o	\
	cl_demo.o	\
	cl_ents.o	\
	cl_input.o	\
	cl_main.o	\
	cl_parse.o	\
	cl_pred.o	\
	cl_tent.o	\
	console.o	\
	keys.o		\
	menu.o		\
	r_part.o	\
	sbar.o		\
	skin.o		\
	snd_dma.o	\
	snd_mem.o	\
	snd_mix.o	\
	view.o		\
	wad.o

QW_COMMON_ASM_OBJS = \
	math.o		\
	snd_mixa.o

QW_W32_C_OBJS = \
	cd_win.o	\
	in_win.o	\
	net_wins.o	\
	snd_win.o	\
	sys_win.o

QW_W32_ASM_OBJS = \
	sys_wina.o

QW_LINUX_SV_SHARED_C_OBJS = \
	net_udp.o

QW_LINUX_C_OBJS = \
	$(QW_LINUX_SV_SHARED_C_OBJS) \
	cd_linux.o	\
	snd_linux.o	\
	sys_linux.o	\
	in_x11.o	\
	x11_core.o

QW_LINUX_ASM_OBJS = \
	sys_dosa.o

QW_SW_C_OBJS = \
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
	r_efrag.o	\
	r_light.o	\
	r_main.o	\
	r_misc.o	\
	r_sky.o		\
	r_sprite.o	\
	r_surf.o	\
	r_vars.o	\
	screen.o

QW_SW_ASM_OBJS = \
	d_draw.o	\
	d_draw16.o	\
	d_parta.o	\
	d_polysa.o	\
	d_scana.o	\
	d_spr8.o	\
	d_varsa.o	\
	r_aclipa.o	\
	r_aliasa.o	\
	r_drawa.o	\
	r_edgea.o	\
	r_varsa.o	\
	surf16.o	\
	surf8.o

QW_W32_SW_C_OBJS = \
	vid_win.o

QW_W32_SW_ASM_OBJS =

QW_LINUX_SW_C_OBJS = \
	vid_x.o

QW_LINUX_SW_ASM_OBJS =

QW_GL_C_OBJS = \
	drawhulls.o	\
	gl_draw.o	\
	gl_mesh.o	\
	gl_model.o	\
	gl_ngraph.o	\
	gl_refrag.o	\
	gl_rlight.o	\
	gl_rmain.o	\
	gl_rmisc.o	\
	gl_rsurf.o	\
	gl_screen.o	\
	gl_warp.o

QW_GL_ASM_OBJS =

QW_W32_GL_C_OBJS = \
	gl_vidnt.o

QW_W32_GL_ASM_OBJS =

QW_LINUX_GL_C_OBJS = \
	gl_vidlinuxglx.o

QW_LINUX_GL_ASM_OBJS =

# ========================================================================== #

# Build the list of object files for each particular target
# (*sigh* - something has to be done about this makefile...)

QW_W32_SW_OBJS := $(QW_COMMON_C_OBJS)
QW_W32_SW_OBJS += $(QW_SW_C_OBJS)
QW_W32_SW_OBJS += $(QW_W32_C_OBJS)
QW_W32_SW_OBJS += $(QW_W32_SW_C_OBJS)
QW_W32_SW_OBJS += winquake.res
ifdef NO_X86_ASM
QW_W32_SW_OBJS += nonintel.o
else
QW_W32_SW_OBJS += $(QW_COMMON_ASM_OBJS)
QW_W32_SW_OBJS += $(QW_SW_ASM_OBJS)
QW_W32_SW_OBJS += $(QW_W32_ASM_OBJS)
QW_W32_SW_OBJS += $(QW_W32_SW_ASM_OBJS)
endif

QW_W32_GL_OBJS := $(QW_COMMON_C_OBJS)
QW_W32_GL_OBJS += $(QW_GL_C_OBJS)
QW_W32_GL_OBJS += $(QW_W32_C_OBJS)
QW_W32_GL_OBJS += $(QW_W32_GL_C_OBJS)
QW_W32_GL_OBJS += winquake.res
ifndef NO_X86_ASM
QW_W32_GL_OBJS += $(QW_COMMON_ASM_OBJS)
QW_W32_GL_OBJS += $(QW_GL_ASM_OBJS)
QW_W32_GL_OBJS += $(QW_W32_ASM_OBJS)
QW_W32_GL_OBJS += $(QW_W32_GL_ASM_OBJS)
endif

QW_LINUX_SW_OBJS := $(QW_COMMON_C_OBJS)
QW_LINUX_SW_OBJS += $(QW_SW_C_OBJS)
QW_LINUX_SW_OBJS += $(QW_LINUX_C_OBJS)
QW_LINUX_SW_OBJS += $(QW_LINUX_SW_C_OBJS)
ifdef NO_X86_ASM
QW_LINUX_SW_OBJS += nonintel.o
else
QW_LINUX_SW_OBJS += $(QW_COMMON_ASM_OBJS)
QW_LINUX_SW_OBJS += $(QW_SW_ASM_OBJS)
QW_LINUX_SW_OBJS += $(QW_LINUX_ASM_OBJS)
QW_LINUX_SW_OBJS += $(QW_LINUX_SW_ASM_OBJS)
endif

QW_LINUX_GL_OBJS := $(QW_COMMON_C_OBJS)
QW_LINUX_GL_OBJS += $(QW_GL_C_OBJS)
QW_LINUX_GL_OBJS += $(QW_LINUX_C_OBJS)
QW_LINUX_GL_OBJS += $(QW_LINUX_GL_C_OBJS)
ifndef NO_X86_ASM
QW_LINUX_GL_OBJS += $(QW_COMMON_ASM_OBJS)
QW_LINUX_GL_OBJS += $(QW_GL_ASM_OBJS)
QW_LINUX_GL_OBJS += $(QW_LINUX_ASM_OBJS)
QW_LINUX_GL_OBJS += $(QW_LINUX_GL_ASM_OBJS)
endif

# ---------
# QW Libs
# ---------
QW_W32_COMMON_LIBS = wsock32 dxguid winmm
QW_W32_SW_LIBS = mgllt
QW_W32_GL_LIBS = opengl32 comctl32

QW_LINUX_COMMON_LIBS = m X11 Xext Xxf86dga Xxf86vm
QW_LINUX_GL_LIBS = GL

QW_W32_SW_LFLAGS = -mwindows $(patsubst %,-l%,$(QW_W32_SW_LIBS) $(QW_W32_COMMON_LIBS))
QW_W32_GL_LFLAGS = -mwindows $(patsubst %,-l%,$(QW_W32_GL_LIBS) $(QW_W32_COMMON_LIBS))
QW_LINUX_SW_LFLAGS = $(patsubst %,-l%,$(QW_LINUX_COMMON_LIBS))
QW_LINUX_GL_LFLAGS = $(patsubst %,-l%,$(QW_LINUX_COMMON_LIBS) $(QW_LINUX_GL_LIBS))

# ---------------------
# build rules
# --------------------

# Win32
qw-w32-sw-objs:
	$(MAKE) -C $(QW_DIR)/client qwcl-sw-win32

qw-w32-gl-objs:
	$(MAKE) -C $(QW_DIR)/client qwcl-gl-win32

tyr-qwcl.exe:	qw-w32-sw-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-qwcl.exe $(patsubst %,$(QW_DIR)/client/%,$(QW_W32_SW_OBJS)) -L$(WIN_LIBDIR) -L$(QW_ST_LIBDIR) $(QW_W32_SW_LFLAGS)
	$(STRIP_CMD) $@

tyr-glqwcl.exe:	qw-w32-gl-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-glqwcl.exe $(patsubst %,$(QW_DIR)/client/%,$(QW_W32_GL_OBJS)) -L$(WIN_LIBDIR) $(QW_W32_GL_LFLAGS)
	$(STRIP_CMD) $@

# Linux
qw-linux-sw-objs:
	$(MAKE) -C $(QW_DIR)/client qwcl-sw-linux

qw-linux-gl-objs:
	$(MAKE) -C $(QW_DIR)/client qwcl-gl-linux

tyr-qwcl:	qw-linux-sw-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-qwcl $(patsubst %,$(QW_DIR)/client/%,$(QW_LINUX_SW_OBJS)) -L$(LINUX_X11_LIBDIR) $(QW_LINUX_SW_LFLAGS)
	$(STRIP_CMD) $@

tyr-glqwcl:	qw-linux-gl-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-glqwcl $(patsubst %,$(QW_DIR)/client/%,$(QW_LINUX_GL_OBJS)) -L$(LINUX_X11_LIBDIR) $(QW_LINUX_GL_LFLAGS)
	$(STRIP_CMD) $@

UNUSED_OBJS	= cd_audio.o

# --------------------------------------------------------------------------
# QuakeWorld (QW) - Server
# --------------------------------------------------------------------------

QWSV_SHARED_OBJS = \
	cmd.o		\
	common.o	\
	crc.o		\
	cvar.o		\
	mathlib.o	\
	md4.o		\
	net_chan.o	\
	pmove.o		\
	pmovetst.o	\
	rb_tree.o	\
	shell.o		\
	zone.o

QWSV_W32_SHARED_OBJS = \
	net_wins.o

QWSV_LINUX_SHARED_OBJS = \
	net_udp.o

QWSV_ONLY_OBJS = \
	model.o		\
	pr_cmds.o	\
	pr_edict.o	\
	pr_exec.o	\
	sv_ccmds.o	\
	sv_ents.o	\
	sv_init.o	\
	sv_main.o	\
	sv_move.o	\
	sv_nchan.o	\
	sv_phys.o	\
	sv_send.o	\
	sv_user.o	\
	world.o

QWSV_W32_ONLY_OBJS = \
	sys_win.o

QWSV_LINUX_ONLY_OBJS = \
	sys_unix.o

QWSV_W32_OBJS = \
	$(QWSV_SHARED_OBJS) 	\
	$(QWSV_W32_SHARED_OBJS)	\
	$(QWSV_ONLY_OBJS)	\
	$(QWSV_W32_ONLY_OBJS)

QWSV_LINUX_OBJS = \
	$(QWSV_SHARED_OBJS) 		\
	$(QWSV_LINUX_SHARED_OBJS)	\
	$(QWSV_ONLY_OBJS)		\
	$(QWSV_LINUX_ONLY_OBJS)

# ----------------
# QWSV Libs
# ----------------
QWSV_W32_LIBS = wsock32 winmm
QWSV_W32_LFLAGS = -mconsole $(patsubst %,-l%,$(QWSV_W32_LIBS))
QWSV_LINUX_LIBS = m
QWSV_LINUX_LFLAGS = $(patsubst %,-l%,$(QWSV_LINUX_LIBS))

# -------------
# Build rules
# -------------

# Win32
qwsv-w32-objs:
	$(MAKE) -C $(QW_DIR)/server qwsv-win32

tyr-qwsv.exe:	qwsv-w32-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-qwsv.exe $(patsubst %,$(QW_DIR)/server/%,$(QWSV_W32_OBJS)) -L$(WIN_LIBDIR) $(QWSV_W32_LFLAGS)
	$(STRIP_CMD) $@

# Linux
qwsv-linux-objs:
	$(MAKE) -C $(QW_DIR)/server qwsv-linux

tyr-qwsv:	qwsv-linux-objs
	$(CC) $(CPPFLAGS) $(CFLAGS) -o tyr-qwsv $(patsubst %,$(QW_DIR)/server/%,$(QWSV_LINUX_OBJS)) $(QWSV_LINUX_LFLAGS)
	$(STRIP_CMD) $@

# ----------------------------------------------------------------------------
# Very basic clean target (can't use xargs on MSYS)
# ----------------------------------------------------------------------------

# Main clean function...
clean:
	@rm -f $(shell find . \( \
		-name '*~' -o -name '#*#' -o -name '*.o' -o -name '*.res' \
	\) -print)
