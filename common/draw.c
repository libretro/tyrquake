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

// draw.c -- this is the only file outside the refresh that touches the
// vid buffer

#include "common.h"
#include "console.h"
#include "d_iface.h"
#include "quakedef.h"
#include "sys.h"
#include "vid.h"
#include "view.h"
#include "wad.h"
#include "zone.h"

#ifdef NQ_HACK
#include "sound.h"
#endif
#ifdef QW_HACK
#include "bothdefs.h"
#include "client.h"
#endif

typedef struct {
    vrect_t rect;
    int width;
    int height;
    const byte *ptexbytes;
    int rowbytes;
} rectdesc_t;

static rectdesc_t r_rectdesc;

byte *draw_chars;		// 8*8 graphic characters
const qpic_t *draw_disc;
extern byte *host_basepal;
static const qpic_t *draw_backtile;

extern int coloredlights;

//=============================================================================
/* Support Routines */

typedef struct cachepic_s {
    char name[MAX_QPATH];
    cache_user_t cache;
} cachepic_t;

#define	MAX_CACHED_PICS		128
static cachepic_t menu_cachepics[MAX_CACHED_PICS];
static int menu_numcachepics;


void *Draw_PicFromWad(const char *name)
{
    return (qpic_t*)W_GetLumpName(name);
}

/*
================
Draw_CachePic
================
*/
qpic_t *
Draw_CachePic(const char *path)
{
    cachepic_t *pic;
    int i;
    qpic_t *dat;

    for (pic = menu_cachepics, i = 0; i < menu_numcachepics; pic++, i++)
	if (!strcmp(path, pic->name))
	    break;

    if (i == menu_numcachepics) {
	if (menu_numcachepics == MAX_CACHED_PICS)
	    Sys_Error("menu_numcachepics == MAX_CACHED_PICS");
	menu_numcachepics++;
	strcpy(pic->name, path);
    }

    dat = (qpic_t*)Cache_Check(&pic->cache);

    if (dat)
	return dat;

//
// load the pic from disk
//
    COM_LoadCacheFile(path, &pic->cache);

    dat = (qpic_t *)pic->cache.data;
    if (!dat) {
	Sys_Error("%s: failed to load %s", __func__, path);
    }

    SwapPic(dat);

    return dat;
}



/*
===============
Draw_Init
===============
*/

extern void VID_SetPalette2(unsigned char *palette);

// Colored Lighting lookup tables
byte palmap2[64][64][64];	

/*
===============
BestColor
===============
*/
static byte BestColor(int r, int g, int b, int start, int stop)
{
	int	i;
	// let any color go to 0 as a last resort
	int bestdistortion = 256*256*4;
	int bestcolor      = 0;
	byte *pal          = host_basepal + start*3;
	for (i=start ; i<= stop ; i++)
	{
		int dr          = r - (int)pal[0];
		int dg          = g - (int)pal[1];
		int db          = b - (int)pal[2];
		int distortion  = dr * dr + dg * dg + db * db;

		pal            += 3;

		if (distortion < bestdistortion)
		{
			if (!distortion)
				return i; // perfect match

			bestdistortion = distortion;
			bestcolor      = i;
		}
	}

	return bestcolor;
}

static void Draw_Generate18BPPTable (void)
{
	int		r, g, b;

	// Make the 18-bit lookup table here
	for (r=0 ; r<256 ; r+=4)
	{
		for (g=0 ; g<256 ; g+=4)
		{
			for (b=0 ; b<256 ; b+=4)
			{
				int beastcolor = BestColor (r, g, b, 0, 254);
				palmap2[r>>2][g>>2][b>>2] = beastcolor;
			}
		}
	}
}

void Draw_Init(void)
{
    draw_chars = (byte*)W_GetLumpName("conchars");
    draw_disc = (const qpic_t*)W_GetLumpName("disc");
    draw_backtile = (const qpic_t*)W_GetLumpName("backtile");

    r_rectdesc.width = draw_backtile->width;
    r_rectdesc.height = draw_backtile->height;
    r_rectdesc.ptexbytes = draw_backtile->data;
    r_rectdesc.rowbytes = draw_backtile->width;

    if (coloredlights)
    {
       VID_SetPalette2 (host_basepal);
       Draw_Generate18BPPTable();
    }
}

/*
================
Draw_Character

Draws one 8*8 graphics character with 0 being transparent.
It can be clipped to the top of the screen to allow the console to be
smoothly scrolled off.
================
*/
void Draw_Character(int x, int y, int num)
{
    byte *dest;
    byte *source;
    int drawline;
    int row, col;

    num &= 255;

    if (y <= -8)
	return;

    if (y > vid.height - 8 || x < 0 || x > vid.width - 8)
	return;
    if (num < 0 || num > 255)
	return;

    row = num >> 4;
    col = num & 15;
    source = draw_chars + (row << 10) + (col << 3);

    if (y < 0) {		// clipped
	drawline = 8 + y;
	source -= 128 * y;
	y = 0;
    } else
	drawline = 8;

    {
	dest = vid.conbuffer + y * vid.conrowbytes + x;

	while (drawline--)
	{
	    if (source[0])
		dest[0] = source[0];
	    if (source[1])
		dest[1] = source[1];
	    if (source[2])
		dest[2] = source[2];
	    if (source[3])
		dest[3] = source[3];
	    if (source[4])
		dest[4] = source[4];
	    if (source[5])
		dest[5] = source[5];
	    if (source[6])
		dest[6] = source[6];
	    if (source[7])
		dest[7] = source[7];
	    source += 128;
	    dest += vid.conrowbytes;
	}
    }
}

/*
================
Draw_String
================
*/
void Draw_String(int x, int y, char *str)
{
   while (*str)
   {
      Draw_Character(x, y, *str);
      str++;
      x += 8;
   }
}

/*
================
Draw_Alt_String
================
*/
void Draw_Alt_String(int x, int y, char *str)
{
   while (*str)
   {
      Draw_Character(x, y, (*str) | 0x80);
      str++;
      x += 8;
   }
}

static void Draw_Pixel(int x, int y, byte color)
{
      uint8_t *dest = vid.conbuffer + y * vid.conrowbytes + x;
      *dest = color;
}

void
Draw_Crosshair(void)
{
   int x, y;
   byte c = (byte)crosshaircolor.value;

   if (crosshair.value == 2)
   {
      x = scr_vrect.x + scr_vrect.width / 2 + cl_crossx.value;
      y = scr_vrect.y + scr_vrect.height / 2 + cl_crossy.value;
      Draw_Pixel(x - 1, y, c);
      Draw_Pixel(x - 3, y, c);
      Draw_Pixel(x + 1, y, c);
      Draw_Pixel(x + 3, y, c);
      Draw_Pixel(x, y - 1, c);
      Draw_Pixel(x, y - 3, c);
      Draw_Pixel(x, y + 1, c);
      Draw_Pixel(x, y + 3, c);
   }
   else if (crosshair.value)
      Draw_Character(scr_vrect.x + scr_vrect.width / 2 - 4 +
            cl_crossx.value,
            scr_vrect.y + scr_vrect.height / 2 - 4 +
            cl_crossy.value, '+');
}


/*
=============
Draw_Pic
=============
*/
void Draw_Pic(int x, int y, const qpic_t *pic)
{
   int v;
   uint8_t *dest;
   const byte *source;

   if (x < 0 || x + pic->width > vid.width ||
         y < 0 || y + pic->height > vid.height)
      Sys_Error("%s: bad coordinates", __func__);

   source = pic->data;

   dest = vid.buffer + y * vid.rowbytes + x;

   for (v = 0; v < pic->height; v++)
   {
      memcpy(dest, source, pic->width);
      dest   += vid.rowbytes;
      source += pic->width;
   }
}


/*
=============
Draw_SubPic
=============
*/
void Draw_SubPic(int x, int y, const qpic_t *pic, int srcx, int srcy, int width,
	    int height)
{
   const byte *source;
   int v;

   if (x < 0 || x + width > vid.width ||
         y < 0 || y + height > vid.height)
      Sys_Error("%s: bad coordinates", __func__);

   source = pic->data + srcy * pic->width + srcx;

   {
      uint8_t *dest = vid.buffer + y * vid.rowbytes + x;

      for (v = 0; v < height; v++)
      {
         memcpy(dest, source, width);
         dest += vid.rowbytes;
         source += pic->width;
      }
   }
}


/*
=============
Draw_TransPic
=============
*/
void Draw_TransPic(int x, int y, const qpic_t *pic)
{
   byte *dest, tbyte;
   const byte *source;
   int v, u;

   if (x < 0 || (unsigned)(x + pic->width) > vid.width ||
         y < 0 || (unsigned)(y + pic->height) > vid.height)
      Sys_Error("%s: bad coordinates", __func__);

   source = pic->data;

   {
      dest = vid.buffer + y * vid.rowbytes + x;

      if (pic->width & 7) {	// general
         for (v = 0; v < pic->height; v++) {
            for (u = 0; u < pic->width; u++)
               if ((tbyte = source[u]) != TRANSPARENT_COLOR)
                  dest[u] = tbyte;

            dest += vid.rowbytes;
            source += pic->width;
         }
      } else {		// unwound
         for (v = 0; v < pic->height; v++) {
            for (u = 0; u < pic->width; u += 8) {
               if ((tbyte = source[u]) != TRANSPARENT_COLOR)
                  dest[u] = tbyte;
               if ((tbyte = source[u + 1]) != TRANSPARENT_COLOR)
                  dest[u + 1] = tbyte;
               if ((tbyte = source[u + 2]) != TRANSPARENT_COLOR)
                  dest[u + 2] = tbyte;
               if ((tbyte = source[u + 3]) != TRANSPARENT_COLOR)
                  dest[u + 3] = tbyte;
               if ((tbyte = source[u + 4]) != TRANSPARENT_COLOR)
                  dest[u + 4] = tbyte;
               if ((tbyte = source[u + 5]) != TRANSPARENT_COLOR)
                  dest[u + 5] = tbyte;
               if ((tbyte = source[u + 6]) != TRANSPARENT_COLOR)
                  dest[u + 6] = tbyte;
               if ((tbyte = source[u + 7]) != TRANSPARENT_COLOR)
                  dest[u + 7] = tbyte;
            }
            dest += vid.rowbytes;
            source += pic->width;
         }
      }
   }
}


/*
=============
Draw_TransPicTranslate
=============
*/
void Draw_TransPicTranslate(int x, int y, const qpic_t *pic, byte *translation)
{
   byte *dest, tbyte;
   const byte *source;
   int v, u;

   if (x < 0 || (unsigned)(x + pic->width) > vid.width ||
         y < 0 || (unsigned)(y + pic->height) > vid.height)
      Sys_Error("%s: bad coordinates", __func__);

   source = pic->data;

   {
      dest = vid.buffer + y * vid.rowbytes + x;

      if (pic->width & 7) {	// general
         for (v = 0; v < pic->height; v++) {
            for (u = 0; u < pic->width; u++)
               if ((tbyte = source[u]) != TRANSPARENT_COLOR)
                  dest[u] = translation[tbyte];

            dest += vid.rowbytes;
            source += pic->width;
         }
      } else {		// unwound
         for (v = 0; v < pic->height; v++) {
            for (u = 0; u < pic->width; u += 8) {
               if ((tbyte = source[u]) != TRANSPARENT_COLOR)
                  dest[u] = translation[tbyte];
               if ((tbyte = source[u + 1]) != TRANSPARENT_COLOR)
                  dest[u + 1] = translation[tbyte];
               if ((tbyte = source[u + 2]) != TRANSPARENT_COLOR)
                  dest[u + 2] = translation[tbyte];
               if ((tbyte = source[u + 3]) != TRANSPARENT_COLOR)
                  dest[u + 3] = translation[tbyte];
               if ((tbyte = source[u + 4]) != TRANSPARENT_COLOR)
                  dest[u + 4] = translation[tbyte];
               if ((tbyte = source[u + 5]) != TRANSPARENT_COLOR)
                  dest[u + 5] = translation[tbyte];
               if ((tbyte = source[u + 6]) != TRANSPARENT_COLOR)
                  dest[u + 6] = translation[tbyte];
               if ((tbyte = source[u + 7]) != TRANSPARENT_COLOR)
                  dest[u + 7] = translation[tbyte];
            }
            dest += vid.rowbytes;
            source += pic->width;
         }
      }
   }
}


#define CHAR_WIDTH	8
#define CHAR_HEIGHT	8

static void
Draw_ScaledCharToConback(const qpic_t *conback, int num, byte *dest)
{
    int y;
    int drawlines = conback->height * CHAR_HEIGHT / 200;
    int drawwidth = conback->width * CHAR_WIDTH / 320;
    int row       = num >> 4;
    int col       = num & 15;
    byte *source  = draw_chars + (row << 10) + (col << 3);
    int fstep     = 320 * 0x10000 / conback->width;

    for (y = 0; y < drawlines; y++, dest += conback->width)
    {
	int x;
	int f = 0;
	byte *src = source + (y * CHAR_HEIGHT / drawlines) * 128;
	for (x = 0; x < drawwidth; x++, f += fstep)
	{
	    if (src[f >> 16])
		dest[x] = 0x60 + src[f >> 16];
	}
    }
}

/*
 * Draw_ConbackString
 *
 * This function draws a string to a very specific location on the console
 * background. The position is such that for a 320x200 background, the text
 * will be 6 pixels from the bottom and 11 pixels from the right. For other
 * sizes, the positioning is scaled so as to make it appear the same size and
 * at the same location.
 */
static void Draw_ConbackString(qpic_t *cb, const char *str)
{
    int x;
    size_t len = strlen(str);
    int row    = cb->height - ((CHAR_HEIGHT + 6) * cb->height / 200);
    int col    = cb->width - ((11 + CHAR_WIDTH * len) * cb->width / 320);
    byte *dest = cb->data + cb->width * row + col;
    for (x = 0; x < len; x++)
	Draw_ScaledCharToConback(cb, str[x], dest + (x * CHAR_WIDTH *
						     cb->width / 320));
}


/*
================
Draw_ConsoleBackground

================
*/
void
Draw_ConsoleBackground(int lines)
{
    int x, y, v;
    const byte *src;
    byte *dest;
    int f, fstep;
    qpic_t *conback = Draw_CachePic("gfx/conback.lmp");

    /* hack the version number directly into the pic */
    Draw_ConbackString(conback, stringify(TYR_VERSION));

    /* draw the pic */
    {
	dest = vid.conbuffer;

	for (y = 0; y < lines; y++, dest += vid.conrowbytes) {
	    v = (vid.conheight - lines + y) * conback->height / vid.conheight;
	    src = conback->data + v * conback->width;
	    if (vid.conwidth == conback->width)
		memcpy(dest, src, vid.conwidth);
	    else {
		f = 0;
		fstep = conback->width * 0x10000 / vid.conwidth;
		for (x = 0; x < vid.conwidth; x += 4) {
		    dest[x] = src[f >> 16];
		    f += fstep;
		    dest[x + 1] = src[f >> 16];
		    f += fstep;
		    dest[x + 2] = src[f >> 16];
		    f += fstep;
		    dest[x + 3] = src[f >> 16];
		    f += fstep;
		}
	    }
	}
    }
}


/*
==============
R_DrawRect8
==============
*/
static void R_DrawRect8(vrect_t *prect, int rowbytes, const byte *psrc, int transparent)
{
    byte t;
    int i, j;
    byte *pdest = vid.buffer + (prect->y * vid.rowbytes) + prect->x;
    int srcdelta = rowbytes - prect->width;
    int destdelta = vid.rowbytes - prect->width;

    if (transparent)
    {
	for (i = 0; i < prect->height; i++)
	{
	    for (j = 0; j < prect->width; j++)
	    {
		t = *psrc;
		if (t != TRANSPARENT_COLOR)
		    *pdest = t;

		psrc++;
		pdest++;
	    }

	    psrc += srcdelta;
	    pdest += destdelta;
	}
    }
    else
    {
	for (i = 0; i < prect->height; i++)
	{
	    memcpy(pdest, psrc, prect->width);
	    psrc += rowbytes;
	    pdest += vid.rowbytes;
	}
    }
}

/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void
Draw_TileClear(int x, int y, int w, int h)
{
    int width, height, tileoffsetx, tileoffsety;
    const byte *psrc;
    vrect_t vr;

    if (x < 0 || (unsigned)(x + w) > vid.width ||
	y < 0 || (unsigned)(y + h) > vid.height)
	Sys_Error("%s: bad coordinates", __func__);

    r_rectdesc.rect.x = x;
    r_rectdesc.rect.y = y;
    r_rectdesc.rect.width = w;
    r_rectdesc.rect.height = h;

    vr.y = r_rectdesc.rect.y;
    height = r_rectdesc.rect.height;

    tileoffsety = vr.y % r_rectdesc.height;

    while (height > 0) {
	vr.x = r_rectdesc.rect.x;
	width = r_rectdesc.rect.width;

	if (tileoffsety != 0)
	    vr.height = r_rectdesc.height - tileoffsety;
	else
	    vr.height = r_rectdesc.height;

	if (vr.height > height)
	    vr.height = height;

	tileoffsetx = vr.x % r_rectdesc.width;

	while (width > 0) {
	    if (tileoffsetx != 0)
		vr.width = r_rectdesc.width - tileoffsetx;
	    else
		vr.width = r_rectdesc.width;

	    if (vr.width > width)
		vr.width = width;

	    psrc = r_rectdesc.ptexbytes +
		(tileoffsety * r_rectdesc.rowbytes) + tileoffsetx;

	    R_DrawRect8(&vr, r_rectdesc.rowbytes, psrc, 0);

	    vr.x += vr.width;
	    width -= vr.width;
	    tileoffsetx = 0;	// only the left tile can be left-clipped
	}

	vr.y += vr.height;
	height -= vr.height;
	tileoffsety = 0;	// only the top tile can be top-clipped
    }
}


/*
=============
Draw_Fill

Fills a box of pixels with a single color
=============
*/
void Draw_Fill(int x, int y, int w, int h, int c)
{
    byte *dest;
    int u, v;

    if (x < 0 || x + w > vid.width || y < 0 || y + h > vid.height)
    {
	Con_Printf("Bad Draw_Fill(%d, %d, %d, %d, %c)\n", x, y, w, h, c);
	return;
    }

    {
	dest = vid.buffer + y * vid.rowbytes + x;
	for (v = 0; v < h; v++, dest += vid.rowbytes)
	    for (u = 0; u < w; u++)
		dest[u] = c;
    }
}

//=============================================================================

/*
================
Draw_FadeScreen

================
*/
void Draw_FadeScreen(void)
{
   int x, y;

   for (y = 0; y < vid.height; y++)
   {
      byte *pbuf = (byte *)(vid.buffer + vid.rowbytes * y);
      int t      = (y & 1) << 1;

      for (x = 0; x < vid.width; x++) {
         if ((x & 3) != t)
            pbuf[x] = 0;
      }
   }
}

//=============================================================================

/*
================
Draw_BeginDisc

Draws the little blue disc in the corner of the screen.
Call before beginning any disc IO.
================
*/
void Draw_BeginDisc(void)
{
    D_BeginDirectRect(vid.width - 24, 0, draw_disc->data, 24, 24);
}

/*
================
Draw_EndDisc

Erases the disc icon.
Call after completing any disc IO
================
*/
void Draw_EndDisc(void)
{
    D_EndDirectRect(vid.width - 24, 0, 24, 24);
}
