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

#include "glquake.h"
#include "quakedef.h"
#include "vid.h"
#include "sys.h"

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
unsigned char d_15to8table[65536];

viddef_t vid;

void GL_BeginRendering(int *x, int *y, int *width, int *height) {}
void GL_EndRendering(void) {}
qboolean VID_Is8bit(void) { return false; }
qboolean VID_IsFullScreen(void) { return false; }
void VID_LockBuffer(void) {}
void VID_UnlockBuffer(void) {}


static void VID_SetNullGammaRamp(unsigned short ramp[3][256]) {}
void (*VID_SetGammaRamp)(unsigned short ramp[3][256]) = VID_SetNullGammaRamp;

float gldepthmin, gldepthmax;
int texture_mode;
qboolean gl_mtexable;
cvar_t gl_ztrick = { "gl_ztrick", "1" };

void VID_SetPalette(unsigned char *palette) {}
void VID_ShiftPalette(unsigned char *palette) {}
void VID_Init(unsigned char *palette) {}
void VID_Shutdown(void) {}
void VID_Update(vrect_t *rects) {}
void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height) {}
void D_EndDirectRect(int x, int y, int width, int height) {}

/*
 * FIXME!!
 *
 * Move stuff around or create abstractions so these hacks aren't needed
 */

#ifndef _WIN32
void Sys_SendKeyEvents(void) {}
#endif

#ifdef _WIN32
#include <windows.h>

qboolean DDActive;
qboolean scr_skipupdate;
HWND mainwindow;
BINDTEXFUNCPTR bindTexFunc;
void VID_ForceLockState(int lk) {}
int VID_ForceUnlockedAndReturnState(void) { return 0; }
void VID_SetDefaultMode(void) {}
qboolean window_visible(void) { return true; }
#endif
