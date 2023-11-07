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
// vid.h -- video driver defs

#ifndef VID_H
#define VID_H

#include "qtypes.h"

#define VID_CBITS	6
#define VID_GRADES	(1 << VID_CBITS)

// a pixel can be one, two, or four bytes
typedef byte pixel_t;

typedef struct vrect_s {
    int x, y, width, height;
    struct vrect_s *pnext;
} vrect_t;

typedef struct {
    pixel_t *buffer;		// invisible buffer
    pixel_t *colormap;		// 256 * VID_GRADES size
    unsigned short *colormap16;	// 256 * VID_GRADES size
    int fullbright;		// index of first fullbright color
    int rowbytes;		// may be > width if displayed in a window
    int width;
    int height;
    float aspect;		// width / height -- < 0 is taller than wide
    int numpages;
    int recalc_refdef;		// if true, recalc vid-based stuff
    pixel_t *conbuffer;
    int conrowbytes;
    int conwidth;
    int conheight;
    int maxwarpwidth;
    int maxwarpheight;
    pixel_t *direct;		// direct drawing to framebuffer, if not NULL
} viddef_t;

extern viddef_t vid;		// global video state
extern unsigned short d_8to16table[256];
extern void (*vid_menudrawfn) (void);
extern void (*vid_menukeyfn) (int key);

extern	unsigned 	d_8to24table[256];

void VID_SetPalette(unsigned char *palette);

// called for bonus and pain flashes, and for underwater color changes

extern unsigned short ramps[3][256];
extern void (*VID_SetGammaRamp)(unsigned short ramp[3][256]);

// called to set hardware gamma (if available - primarily for OpenGL renderer)

void VID_Init(unsigned char *palette);

// Called at startup to set up translation tables, takes 256 8 bit RGB values
// the palette data will go away after the call, so it must be copied off if
// the video driver will need it again

void VID_Shutdown(void);

// Called at shutdown

void VID_Update(vrect_t *rects);

#endif /* VID_H */
