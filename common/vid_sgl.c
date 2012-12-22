/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifdef APPLE_OPENGL
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "SDL.h"

#include "console.h"
#include "glquake.h"
#include "input.h"
#include "quakedef.h"
#include "sdl_common.h"
#include "sys.h"
#include "vid.h"

#ifdef NQ_HACK
#include "host.h"
#endif

#define WARP_WIDTH 320
#define WARP_HEIGHT 200

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
unsigned char d_15to8table[65536];

viddef_t vid;

qboolean VID_IsFullScreen(void) { return false; }
void VID_LockBuffer(void) {}
void VID_UnlockBuffer(void) {}

void (*VID_SetGammaRamp)(unsigned short ramp[3][256]) = NULL;

float gldepthmin, gldepthmax;
qboolean gl_mtexable;
cvar_t gl_ztrick = { "gl_ztrick", "1" };

void VID_Update(vrect_t *rects) {}
void D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height) {}
void D_EndDirectRect(int x, int y, int width, int height) {}

/*
 * FIXME!!
 *
 * Move stuff around or create abstractions so these hacks aren't needed
 */

#ifndef _WIN32
void Sys_SendKeyEvents(void)
{
    IN_ProcessEvents();
}
#endif

#ifdef _WIN32
#include <windows.h>

qboolean DDActive;
qboolean scr_skipupdate;
HWND mainwindow;
void VID_ForceLockState(int lk) {}
int VID_ForceUnlockedAndReturnState(void) { return 0; }
void VID_SetDefaultMode(void) {}
qboolean window_visible(void) { return true; }
#endif

int texture_mode = GL_LINEAR;

static SDL_GLContext gl_context;

const char *gl_vendor;
const char *gl_renderer;
const char *gl_version;
const char *gl_extensions;
static int gl_num_texture_units;

static qboolean
VID_GL_CheckExtn(const char *extn)
{
    const char *found;
    const int len = strlen(extn);
    char nextc;

    found = strstr(gl_extensions, extn);
    while (found) {
	nextc = found[len];
	if (nextc == ' ' || !nextc)
	    return true;
	found = strstr(found + len, extn);
    }

    return false;
}

static void
VID_InitGL(void)
{
    gl_vendor = (const char *)glGetString(GL_VENDOR);
    gl_renderer = (const char *)glGetString(GL_RENDERER);
    gl_version = (const char *)glGetString(GL_VERSION);
    gl_extensions = (const char *)glGetString(GL_EXTENSIONS);

    printf("GL_VENDOR: %s\n", gl_vendor);
    printf("GL_RENDERER: %s\n", gl_renderer);
    printf("GL_VERSION: %s\n", gl_version);
    printf("GL_EXTENSIONS: %s\n", gl_extensions);

    gl_mtexable = false;
    if (!COM_CheckParm("-nomtex") && VID_GL_CheckExtn("GL_ARB_multitexture")) {
	qglMultiTexCoord2fARB = SDL_GL_GetProcAddress("glMultiTexCoord2fARB");
	qglActiveTextureARB = SDL_GL_GetProcAddress("glActiveTextureARB");

	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &gl_num_texture_units);
	if (gl_num_texture_units >= 2 &&
	    qglMultiTexCoord2fARB && qglActiveTextureARB)
	    gl_mtexable = true;
	Con_Printf("ARB multitexture extension enabled\n"
		   "-> %i texture units available\n",
		   gl_num_texture_units);
    }

    glClearColor(0.5, 0.5, 0.5, 0);
    glCullFace(GL_FRONT);
    glEnable(GL_TEXTURE_2D);

    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.666);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glShadeModel(GL_FLAT);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
}

static int
VID_SetMode(int modenum, unsigned char *palette)
{
    Uint32 flags;
    int w = 640;
    int h = 480;
    int err;

    if (gl_context)
	SDL_GL_DeleteContext(gl_context);
    if (sdl_window)
	SDL_DestroyWindow(sdl_window);

    flags = SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL;
    sdl_window = SDL_CreateWindow("TyrQuake",
				  SDL_WINDOWPOS_UNDEFINED,
				  SDL_WINDOWPOS_UNDEFINED,
				  w, h, flags);
    if (!sdl_window)
	Sys_Error("%s: Unable to create window: %s", __func__, SDL_GetError());

    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context)
	Sys_Error("%s: Unable to create OpenGL context: %s",
		  __func__, SDL_GetError());

    err = SDL_GL_MakeCurrent(sdl_window, gl_context);
    if (err)
	Sys_Error("%s: SDL_GL_MakeCurrent() failed: %s",
		  __func__, SDL_GetError());

    VID_InitGL();

    vid.numpages = 1;
    vid.width = vid.conwidth = w;
    vid.height = vid.conheight = h;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    return true;
}

void
VID_Init(unsigned char *palette)
{
    int err;

    Q_SDL_InitOnce();
    err = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (err < 0)
	Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());

    VID_SetMode(0, palette);

    VID_SetPalette(palette);
}

void
VID_Shutdown(void)
{
    if (sdl_window)
	SDL_DestroyWindow(sdl_window);
}

void
GL_BeginRendering(int *x, int *y, int *width, int *height)
{
    *x = *y = 0;
    *width = vid.width;
    *height = vid.height;
}

void
GL_EndRendering(void)
{
    glFlush();
    SDL_GL_SwapWindow(sdl_window);
}

void
VID_SetPalette(unsigned char *palette)
{
    unsigned i, r, g, b, pixel;

    switch (gl_solid_format) {
    case GL_RGB:
    case GL_RGBA:
	for (i = 0; i < 256; i++) {
	    r = palette[0];
	    g = palette[1];
	    b = palette[2];
	    palette += 3;
	    pixel = (0xff << 24) | (r << 0) | (g << 8) | (b << 16);
	    d_8to24table[i] = LittleLong(pixel);
	}
	d_8to24table[255] &= LittleLong(0xffffff);	// 255 is transparent
	break;
    default:
	Sys_Error("%s: unsupported texture format (%d)", __func__,
		  gl_solid_format);
    }
}

void
VID_ShiftPalette(unsigned char *palette)
{
    /* Done via gl_polyblend instead */
    //VID_SetPalette(palette);
}
