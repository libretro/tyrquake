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

// screen.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"
#include "cmd.h"
#include "console.h"
#include "draw.h"
#include "glquake.h"
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "sbar.h"
#include "screen.h"
#include "sound.h"
#include "sys.h"
#include "vid.h"
#include "view.h"
#include "wad.h"

#ifdef NQ_HACK
#include "host.h"
#endif

/*

background clear
rendering
turtle/net/ram icons
sbar
centerprint / slow centerprint
notify lines
intermission / finale overlay
loading plaque
console
menu

required background clears
required update regions

syncronous draw mode or async
One off screen buffer, with updates either copied or xblited
Need to double buffer?

async draw will require the refresh area to be cleared, because it will be
xblited, but sync draw can just ignore it.

sync
draw

CenterPrint();
SlowPrint();
Screen_Update();
Con_Printf();

net
turn off messages option

the refresh is always rendered, unless the console is full screen

console is:
	notify lines
	half
	full
*/

int glx, gly, glwidth, glheight;

// only the refresh window will be updated unless these variables are flagged
int scr_copytop;
int scr_copyeverything;

float scr_con_current;
float scr_conlines;		// lines of console to display

#ifdef QW_HACK
static float oldsbar;
cvar_t scr_allowsnap = { "scr_allowsnap", "1" };
#endif

qboolean scr_initialized;	// ready to draw

qpic_t *scr_ram;
qpic_t *scr_net;
qpic_t *scr_turtle;

int scr_fullupdate;
int clearconsole;
int clearnotify;
int sb_lines;

vrect_t scr_vrect;

qboolean scr_disabled_for_loading;
#ifdef NQ_HACK
qboolean scr_drawloading;
float scr_disabled_time;
#endif

qboolean scr_block_drawing;

void SCR_ScreenShot_f(void);
#ifdef QW_HACK
void SCR_RSShot_f(void);
#endif

//=============================================================================

/*
==================
SCR_SetUpToDrawConsole
==================
*/
void
SCR_SetUpToDrawConsole(void)
{
    Con_CheckResize();

#ifdef NQ_HACK
    if (scr_drawloading)
	return;			// never a console with loading plaque
#endif

// decide on the height of the console
#ifdef NQ_HACK
    con_forcedup = !cl.worldmodel || cls.state != ca_active;
#endif
#ifdef QW_HACK
    con_forcedup = cls.state != ca_active;
#endif

    if (con_forcedup) {
	scr_conlines = vid.height;	// full screen
	scr_con_current = scr_conlines;
    } else if (key_dest == key_console)
	scr_conlines = vid.height / 2;	// half screen
    else
	scr_conlines = 0;	// none visible

    if (scr_conlines < scr_con_current) {
	scr_con_current -= scr_conspeed.value * host_frametime;
	if (scr_conlines > scr_con_current)
	    scr_con_current = scr_conlines;

    } else if (scr_conlines > scr_con_current) {
	scr_con_current += scr_conspeed.value * host_frametime;
	if (scr_conlines < scr_con_current)
	    scr_con_current = scr_conlines;
    }

    if (clearconsole++ < vid.numpages)
	Sbar_Changed();
    else if (clearnotify++ >= vid.numpages)
	con_notifylines = 0;
}


/*
==================
SCR_DrawConsole
==================
*/
void
SCR_DrawConsole(void)
{
    if (scr_con_current) {
	scr_copyeverything = 1;
	Con_DrawConsole(scr_con_current);
	clearconsole = 0;
    } else {
	if (key_dest == key_game || key_dest == key_message)
	    Con_DrawNotify();	// only draw notify in game
    }
}


/*
==============================================================================

				SCREEN SHOTS

==============================================================================
*/

typedef struct _TargaHeader {
    unsigned char id_length, colormap_type, image_type;
    unsigned short colormap_index, colormap_length;
    unsigned char colormap_size;
    unsigned short x_origin, y_origin, width, height;
    unsigned char pixel_size, attributes;
} TargaHeader;


/*
==================
SCR_ScreenShot_f
==================
*/
void
SCR_ScreenShot_f(void)
{
    byte *buffer;
    char tganame[80];
    char checkname[MAX_OSPATH];
    int i, c, temp;

//
// find a file name to save it to
//
    strcpy(tganame, "quake00.tga");

    for (i = 0; i <= 99; i++) {
	tganame[5] = i / 10 + '0';
	tganame[6] = i % 10 + '0';
	sprintf(checkname, "%s/%s", com_gamedir, tganame);
	if (Sys_FileTime(checkname) == -1)
	    break;		// file doesn't exist
    }
    if (i == 100) {
	Con_Printf("%s: Couldn't create a TGA file\n", __func__);
	return;
    }

    /* Construct the TGA header */
    buffer = malloc(glwidth * glheight * 3 + 18);
    memset(buffer, 0, 18);
    buffer[2] = 2;		// uncompressed type
    buffer[12] = glwidth & 255;
    buffer[13] = glwidth >> 8;
    buffer[14] = glheight & 255;
    buffer[15] = glheight >> 8;
    buffer[16] = 24;		// pixel size

    glReadPixels(glx, gly, glwidth, glheight, GL_RGB, GL_UNSIGNED_BYTE,
		 buffer + 18);

    // swap rgb to bgr
    c = 18 + glwidth * glheight * 3;
    for (i = 18; i < c; i += 3) {
	temp = buffer[i];
	buffer[i] = buffer[i + 2];
	buffer[i + 2] = temp;
    }
    COM_WriteFile(tganame, buffer, glwidth * glheight * 3 + 18);

    free(buffer);
    Con_Printf("Wrote %s\n", tganame);
}

//=============================================================================

#ifdef NQ_HACK
/*
===============
SCR_BeginLoadingPlaque

================
*/
void
SCR_BeginLoadingPlaque(void)
{
    S_StopAllSounds(true);

    if (cls.state != ca_active)
	return;

// redraw with no console and the loading plaque
    Con_ClearNotify();
    scr_centertime_off = 0;
    scr_con_current = 0;

    scr_drawloading = true;
    scr_fullupdate = 0;
    Sbar_Changed();
    SCR_UpdateScreen();
    scr_drawloading = false;

    scr_disabled_for_loading = true;
    scr_disabled_time = realtime;
    scr_fullupdate = 0;
}

/*
==============
SCR_DrawLoading
==============
*/
void
SCR_DrawLoading(void)
{
    qpic_t *pic;

    if (!scr_drawloading)
	return;

    pic = Draw_CachePic("gfx/loading.lmp");
    Draw_Pic((vid.width - pic->width) / 2,
	     (vid.height - 48 - pic->height) / 2, pic);
}

/*
===============
SCR_EndLoadingPlaque

================
*/
void
SCR_EndLoadingPlaque(void)
{
    scr_disabled_for_loading = false;
    scr_fullupdate = 0;
    Con_ClearNotify();
}
#endif /* NQ_HACK */

//=============================================================================

#ifdef QW_HACK
/*
==============
WritePCXfile
==============
*/
static void
WritePCXfile(char *filename, byte *data, int width, int height,
	     int rowbytes, byte *palette, qboolean upload)
{
    int i, j, length;
    pcx_t *pcx;
    byte *pack;

    pcx = Hunk_TempAlloc(width * height * 2 + 1000);
    if (pcx == NULL) {
	Con_Printf("SCR_ScreenShot_f: not enough memory\n");
	return;
    }

    pcx->manufacturer = 0x0a;	// PCX id
    pcx->version = 5;		// 256 color
    pcx->encoding = 1;		// uncompressed
    pcx->bits_per_pixel = 8;	// 256 color
    pcx->xmin = 0;
    pcx->ymin = 0;
    pcx->xmax = LittleShort((short)(width - 1));
    pcx->ymax = LittleShort((short)(height - 1));
    pcx->hres = LittleShort((short)width);
    pcx->vres = LittleShort((short)height);
    memset(pcx->palette, 0, sizeof(pcx->palette));
    pcx->color_planes = 1;	// chunky image
    pcx->bytes_per_line = LittleShort((short)width);
    pcx->palette_type = LittleShort(1);	// not a grey scale
    memset(pcx->filler, 0, sizeof(pcx->filler));

    // pack the image
    pack = &pcx->data;

    // The GL buffer addressing is bottom to top?
    data += rowbytes * (height - 1);
    for (i = 0; i < height; i++) {
	for (j = 0; j < width; j++) {
	    if ((*data & 0xc0) != 0xc0) {
		*pack++ = *data++;
	    } else {
		*pack++ = 0xc1;
		*pack++ = *data++;
	    }
	}
	data += rowbytes - width;
	data -= rowbytes * 2;
    }

    // write the palette
    *pack++ = 0x0c;		// palette ID byte
    for (i = 0; i < 768; i++)
	*pack++ = *palette++;

    // write output file
    length = pack - (byte *)pcx;

    if (upload)
	CL_StartUpload((void *)pcx, length);
    else
	COM_WriteFile(filename, pcx, length);
}


/*
Find closest color in the palette for named color
*/
int
MipColor(int r, int g, int b)
{
    int i;
    float dist;
    int best;
    float bestdist;
    int r1, g1, b1;
    static int lr = -1, lg = -1, lb = -1;
    static int lastbest;

    if (r == lr && g == lg && b == lb)
	return lastbest;

    bestdist = 256 * 256 * 3;

    best = 0;			// FIXME - Uninitialised? Zero ok?
    for (i = 0; i < 256; i++) {
	r1 = host_basepal[i * 3] - r;
	g1 = host_basepal[i * 3 + 1] - g;
	b1 = host_basepal[i * 3 + 2] - b;
	dist = r1 * r1 + g1 * g1 + b1 * b1;
	if (dist < bestdist) {
	    bestdist = dist;
	    best = i;
	}
    }
    lr = r;
    lg = g;
    lb = b;
    lastbest = best;
    return best;
}


void
SCR_DrawCharToSnap(int num, byte *dest, int width)
{
    int row, col;
    byte *source;
    int drawline;
    int x;

    row = num >> 4;
    col = num & 15;
    source = draw_chars + (row << 10) + (col << 3);

    drawline = 8;

    while (drawline--) {
	for (x = 0; x < 8; x++)
	    if (source[x])
		dest[x] = source[x];
	    else
		dest[x] = 98;
	source += 128;
	dest -= width;
    }

}


void
SCR_DrawStringToSnap(const char *s, byte *buf, int x, int y, int width)
{
    byte *dest;
    const unsigned char *p;

    dest = buf + ((y * width) + x);

    p = (const unsigned char *)s;
    while (*p) {
	SCR_DrawCharToSnap(*p++, dest, width);
	dest += 8;
    }
}


/*
==================
SCR_RSShot_f
==================
*/
void
SCR_RSShot_f(void)
{
    int x, y;
    unsigned char *src, *dest;
    char pcxname[80];
    unsigned char *newbuf;
    int w, h;
    int dx, dy, dex, dey, nx;
    int r, b, g;
    int count;
    float fracw, frach;
    char st[80];
    time_t now;

    if (CL_IsUploading())
	return;			// already one pending

    if (cls.state < ca_onserver)
	return;			// gotta be connected

    Con_Printf("Remote screen shot requested.\n");

#if 0
//
// find a file name to save it to
//
    strcpy(pcxname, "mquake00.pcx");

    for (i = 0; i <= 99; i++) {
	pcxname[6] = i / 10 + '0';
	pcxname[7] = i % 10 + '0';
	sprintf(checkname, "%s/%s", com_gamedir, pcxname);
	if (Sys_FileTime(checkname) == -1)
	    break;		// file doesn't exist
    }
    if (i == 100) {
	Con_Printf("SCR_ScreenShot_f: Couldn't create a PCX");
	return;
    }
#endif

//
// save the pcx file
//
    newbuf = malloc(glheight * glwidth * 3);

    glReadPixels(glx, gly, glwidth, glheight, GL_RGB, GL_UNSIGNED_BYTE,
		 newbuf);

    w = (vid.width < RSSHOT_WIDTH) ? glwidth : RSSHOT_WIDTH;
    h = (vid.height < RSSHOT_HEIGHT) ? glheight : RSSHOT_HEIGHT;

    fracw = (float)glwidth / (float)w;
    frach = (float)glheight / (float)h;

    for (y = 0; y < h; y++) {
	dest = newbuf + (w * 3 * y);

	for (x = 0; x < w; x++) {
	    r = g = b = 0;

	    dx = x * fracw;
	    dex = (x + 1) * fracw;
	    if (dex == dx)
		dex++;		// at least one
	    dy = y * frach;
	    dey = (y + 1) * frach;
	    if (dey == dy)
		dey++;		// at least one

	    count = 0;
	    for ( /* */ ; dy < dey; dy++) {
		src = newbuf + (glwidth * 3 * dy) + dx * 3;
		for (nx = dx; nx < dex; nx++) {
		    r += *src++;
		    g += *src++;
		    b += *src++;
		    count++;
		}
	    }
	    r /= count;
	    g /= count;
	    b /= count;
	    *dest++ = r;
	    *dest++ = b;
	    *dest++ = g;
	}
    }

    // convert to eight bit
    for (y = 0; y < h; y++) {
	src = newbuf + (w * 3 * y);
	dest = newbuf + (w * y);

	for (x = 0; x < w; x++) {
	    *dest++ = MipColor(src[0], src[1], src[2]);
	    src += 3;
	}
    }

    time(&now);
    strcpy(st, ctime(&now));
    st[strlen(st) - 1] = 0;
    SCR_DrawStringToSnap(st, newbuf, w - strlen(st) * 8, h - 1, w);

    strncpy(st, cls.servername, sizeof(st));
    st[sizeof(st) - 1] = 0;
    SCR_DrawStringToSnap(st, newbuf, w - strlen(st) * 8, h - 11, w);

    strncpy(st, name.string, sizeof(st));
    st[sizeof(st) - 1] = 0;
    SCR_DrawStringToSnap(st, newbuf, w - strlen(st) * 8, h - 21, w);

    WritePCXfile(pcxname, newbuf, w, h, w, host_basepal, true);

    free(newbuf);

    Con_Printf("Wrote %s\n", pcxname);
}
#endif /* QW_HACK */

//=============================================================================

void
SCR_TileClear(void)
{
    if (r_refdef.vrect.x > 0) {
	// left
	Draw_TileClear(0, 0, r_refdef.vrect.x, vid.height - sb_lines);
	// right
	Draw_TileClear(r_refdef.vrect.x + r_refdef.vrect.width, 0,
		       vid.width - r_refdef.vrect.x + r_refdef.vrect.width,
		       vid.height - sb_lines);
    }
    if (r_refdef.vrect.y > 0) {
	// top
	Draw_TileClear(r_refdef.vrect.x, 0,
		       r_refdef.vrect.x + r_refdef.vrect.width,
		       r_refdef.vrect.y);
	// bottom
	Draw_TileClear(r_refdef.vrect.x,
		       r_refdef.vrect.y + r_refdef.vrect.height,
		       r_refdef.vrect.width,
		       vid.height - sb_lines -
		       (r_refdef.vrect.height + r_refdef.vrect.y));
    }
}

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.

WARNING: be very careful calling this from elsewhere, because the refresh
needs almost the entire 256k of stack space!
==================
*/
void
SCR_UpdateScreen(void)
{
    static float old_viewsize, old_fov;

    if (scr_block_drawing)
	return;

    vid.numpages = 2 + gl_triplebuffer.value;

#ifdef NQ_HACK
    if (scr_disabled_for_loading) {
	/*
	 * FIXME - this really needs to be fixed properly.
	 * Simply starting a new game and typing "changelevel foo" will hang
	 * the engine for 5s (was 60s!) if foo.bsp does not exist.
	 */
	if (realtime - scr_disabled_time > 5) {
	    scr_disabled_for_loading = false;
	    Con_Printf("load failed.\n");
	} else
	    return;
    }
#endif
#ifdef QW_HACK
    if (scr_disabled_for_loading)
	return;
#endif

    if (!scr_initialized || !con_initialized)
	return;			// not initialized yet

    scr_copytop = 0;
    scr_copyeverything = 0;

#ifdef QW_HACK
    if (oldsbar != cl_sbar.value) {
	oldsbar = cl_sbar.value;
	vid.recalc_refdef = true;
    }
#endif

    GL_BeginRendering(&glx, &gly, &glwidth, &glheight);

    //
    // determine size of refresh window
    //
    if (old_fov != scr_fov.value) {
	old_fov = scr_fov.value;
	vid.recalc_refdef = true;
    }

    if (old_viewsize != scr_viewsize.value) {
	old_viewsize = scr_viewsize.value;
	vid.recalc_refdef = true;
    }

    if (vid.recalc_refdef)
	SCR_CalcRefdef();

//
// do 3D refresh drawing, and then update the screen
//
    SCR_SetUpToDrawConsole();

    V_RenderView();

    GL_Set2D();

    //
    // draw any areas not covered by the refresh
    //
    SCR_TileClear();

#ifdef QW_HACK
    if (r_netgraph.value)
	R_NetGraph();
#endif

    if (scr_drawdialog) {
	Sbar_Draw();
	Draw_FadeScreen();
	SCR_DrawNotifyString();
	scr_copyeverything = true;
#ifdef NQ_HACK
    } else if (scr_drawloading) {
	SCR_DrawLoading();
	Sbar_Draw();
#endif
    } else if (cl.intermission == 1 && key_dest == key_game) {
	Sbar_IntermissionOverlay();
    } else if (cl.intermission == 2 && key_dest == key_game) {
	Sbar_FinaleOverlay();
	SCR_DrawCenterString();
    } else {
#ifdef NQ_HACK
	if (crosshair.value) {
	    //Draw_Crosshair();
	    Draw_Character(scr_vrect.x + scr_vrect.width / 2,
			   scr_vrect.y + scr_vrect.height / 2, '+');
	}
#endif
#ifdef QW_HACK
	if (crosshair.value)
	    Draw_Crosshair();
#endif
	SCR_DrawRam();
	SCR_DrawNet();
	SCR_DrawFPS();
	SCR_DrawTurtle();
	SCR_DrawPause();
	SCR_DrawCenterString();
	Sbar_Draw();
	SCR_DrawConsole();
	M_Draw();
    }

    V_UpdatePalette();

    GL_EndRendering();
}
