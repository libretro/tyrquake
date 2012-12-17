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
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "r_local.h"
#include "sbar.h"
#include "screen.h"
#include "sound.h"
#include "sys.h"
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

// only the refresh window will be updated unless these variables are flagged
int scr_copytop;
int scr_copyeverything;

float scr_con_current;
float scr_conlines;		// lines of console to display

cvar_t scr_viewsize = { "viewsize", "100", true };
cvar_t scr_fov = { "fov", "90" };	// 10 - 170
cvar_t scr_conspeed = { "scr_conspeed", "300" };
cvar_t scr_showram = { "showram", "1" };
cvar_t scr_showturtle = { "showturtle", "0" };
cvar_t scr_showpause = { "showpause", "1" };
static cvar_t show_fps = { "show_fps", "0" };	/* set for running times */

qboolean scr_initialized;	// ready to draw

qpic_t *scr_ram;
qpic_t *scr_net;
qpic_t *scr_turtle;

int scr_fullupdate;
int clearconsole;
int clearnotify;

vrect_t *pconupdate;
vrect_t scr_vrect;

qboolean scr_disabled_for_loading;
qboolean scr_skipupdate;
#ifdef NQ_HACK
qboolean scr_drawloading;
float scr_disabled_time;
#endif

qboolean scr_block_drawing;

void SCR_ScreenShot_f(void);

#ifdef QW_HACK
static float oldsbar;
cvar_t scr_allowsnap = { "scr_allowsnap", "1" };
void SCR_RSShot_f(void);
#endif

//=============================================================================

/*
==================
SCR_Init
==================
*/
void
SCR_Init(void)
{
    Cvar_RegisterVariable(&scr_fov);
    Cvar_RegisterVariable(&scr_viewsize);
    Cvar_RegisterVariable(&scr_conspeed);
    Cvar_RegisterVariable(&scr_showram);
    Cvar_RegisterVariable(&scr_showturtle);
    Cvar_RegisterVariable(&scr_showpause);
    Cvar_RegisterVariable(&scr_centertime);
    Cvar_RegisterVariable(&scr_printspeed);
    Cvar_RegisterVariable(&show_fps);

    Cmd_AddCommand("screenshot", SCR_ScreenShot_f);
    Cmd_AddCommand("sizeup", SCR_SizeUp_f);
    Cmd_AddCommand("sizedown", SCR_SizeDown_f);

#ifdef NQ_HACK
    scr_ram = Draw_PicFromWad("ram");
    scr_net = Draw_PicFromWad("net");
    scr_turtle = Draw_PicFromWad("turtle");
#endif
#ifdef QW_HACK
    scr_ram = W_GetLumpName("ram");
    scr_net = W_GetLumpName("net");
    scr_turtle = W_GetLumpName("turtle");

    Cvar_RegisterVariable(&scr_allowsnap);
    Cmd_AddCommand("snap", SCR_RSShot_f);
#endif

    scr_initialized = true;
}


/*
==============
SCR_DrawRam
==============
*/
void
SCR_DrawRam(void)
{
    if (!scr_showram.value)
	return;

    if (!r_cache_thrash)
	return;

    Draw_Pic(scr_vrect.x + 32, scr_vrect.y, scr_ram);
}


/*
==============
SCR_DrawTurtle
==============
*/
void
SCR_DrawTurtle(void)
{
    static int count;

    if (!scr_showturtle.value)
	return;

    if (host_frametime < 0.1) {
	count = 0;
	return;
    }

    count++;
    if (count < 3)
	return;

    Draw_Pic(scr_vrect.x, scr_vrect.y, scr_turtle);
}


/*
==============
SCR_DrawNet
==============
*/
void
SCR_DrawNet(void)
{
#ifdef NQ_HACK
    if (realtime - cl.last_received_message < 0.3)
	return;
#endif
#ifdef QW_HACK
    if (cls.netchan.outgoing_sequence - cls.netchan.incoming_acknowledged <
	UPDATE_BACKUP - 1)
	return;
#endif

    if (cls.demoplayback)
	return;

    Draw_Pic(scr_vrect.x + 64, scr_vrect.y, scr_net);
}


void
SCR_DrawFPS(void)
{
    static double lastframetime;
    static int lastfps;
    double t;
    int x, y;
    char st[80];

    if (!show_fps.value)
	return;

    t = Sys_DoubleTime();
    if ((t - lastframetime) >= 1.0) {
	lastfps = fps_count;
	fps_count = 0;
	lastframetime = t;
    }

    sprintf(st, "%3d FPS", lastfps);
    x = vid.width - strlen(st) * 8 - 8;
    y = vid.height - sb_lines - 8;
    Draw_String(x, y, st);
}


/*
==============
DrawPause
==============
*/
void
SCR_DrawPause(void)
{
    qpic_t *pic;

    if (!scr_showpause.value)	// turn off for screenshots
	return;

    if (!cl.paused)
	return;

    pic = Draw_CachePic("gfx/pause.lmp");
    Draw_Pic((vid.width - pic->width) / 2,
	     (vid.height - 48 - pic->height) / 2, pic);
}

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

    if (clearconsole++ < vid.numpages) {
	scr_copytop = 1;
	Draw_TileClear(0, (int)scr_con_current, vid.width,
		       vid.height - (int)scr_con_current);
	Sbar_Changed();
    } else if (clearnotify++ < vid.numpages) {
	scr_copytop = 1;
	Draw_TileClear(0, 0, vid.width, con_notifylines);
    } else
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


/*
==============
WritePCXfile
==============
*/
static void
WritePCXfile(const char *filename, const byte *data, int width, int height,
	     int rowbytes, const byte *palette, qboolean upload)
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
    }

    // write the palette
    *pack++ = 0x0c;		// palette ID byte
    for (i = 0; i < 768; i++)
	*pack++ = *palette++;

    // write output file
    length = pack - (byte *)pcx;

#ifdef QW_HACK
    if (upload) {
	CL_StartUpload((void *)pcx, length);
	return;
    }
#endif

    COM_WriteFile(filename, pcx, length);
}


/*
==================
SCR_ScreenShot_f
==================
*/
void
SCR_ScreenShot_f(void)
{
    int i;
    char pcxname[80];
    char checkname[MAX_OSPATH];

//
// find a file name to save it to
//
    strcpy(pcxname, "quake00.pcx");

    for (i = 0; i <= 99; i++) {
	pcxname[5] = i / 10 + '0';
	pcxname[6] = i % 10 + '0';
	sprintf(checkname, "%s/%s", com_gamedir, pcxname);
	if (Sys_FileTime(checkname) == -1)
	    break;		// file doesn't exist
    }
    if (i == 100) {
	Con_Printf("%s: Couldn't create a PCX file\n", __func__);
	return;
    }
//
// save the pcx file
//
    D_EnableBackBufferAccess();	// enable direct drawing of console to back
    //  buffer

    WritePCXfile(pcxname, vid.buffer, vid.width, vid.height, vid.rowbytes,
		 host_basepal, false);

    D_DisableBackBufferAccess();	// for adapters that can't stay mapped in
    //  for linear writes all the time

    Con_Printf("Wrote %s\n", pcxname);
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
	dest += width;
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

    if (!scr_allowsnap.value) {
	MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	MSG_WriteString(&cls.netchan.message, "snap\n");
	Con_Printf("Refusing remote screen shot request.\n");
	return;
    }

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
    D_EnableBackBufferAccess();	// enable direct drawing of console to back
    //  buffer

    w = (vid.width < RSSHOT_WIDTH) ? vid.width : RSSHOT_WIDTH;
    h = (vid.height < RSSHOT_HEIGHT) ? vid.height : RSSHOT_HEIGHT;

    fracw = (float)vid.width / (float)w;
    frach = (float)vid.height / (float)h;

    newbuf = malloc(w * h);

    for (y = 0; y < h; y++) {
	dest = newbuf + (w * y);

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
		src = vid.buffer + (vid.rowbytes * dy) + dx;
		for (nx = dx; nx < dex; nx++) {
		    r += host_basepal[*src * 3];
		    g += host_basepal[*src * 3 + 1];
		    b += host_basepal[*src * 3 + 2];
		    src++;
		    count++;
		}
	    }
	    r /= count;
	    g /= count;
	    b /= count;
	    *dest++ = MipColor(r, g, b);
	}
    }

    time(&now);
    strcpy(st, ctime(&now));
    st[strlen(st) - 1] = 0;
    SCR_DrawStringToSnap(st, newbuf, w - strlen(st) * 8, 0, w);

    strncpy(st, cls.servername, sizeof(st));
    st[sizeof(st) - 1] = 0;
    SCR_DrawStringToSnap(st, newbuf, w - strlen(st) * 8, 10, w);

    strncpy(st, name.string, sizeof(st));
    st[sizeof(st) - 1] = 0;
    SCR_DrawStringToSnap(st, newbuf, w - strlen(st) * 8, 20, w);

    WritePCXfile(pcxname, newbuf, w, h, w, host_basepal, true);

    free(newbuf);

    D_DisableBackBufferAccess();	// for adapters that can't stay mapped in
    //  for linear writes all the time

//      Con_Printf ("Wrote %s\n", pcxname);
    Con_Printf("Sending shot to server...\n");
}
#endif /* QW_HACK */

//=============================================================================

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
    vrect_t vrect;

    if (scr_skipupdate || scr_block_drawing)
	return;

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

#ifdef _WIN32
    {				// don't suck up any cpu if minimized
	if (!window_visible())
	    return;
    }
#endif

    scr_copytop = 0;
    scr_copyeverything = 0;

#ifdef NQ_HACK
    if (cls.state == ca_dedicated)
	return;			// stdout only
#endif

    if (!scr_initialized || !con_initialized)
	return;			// not initialized yet

//
// check for vid changes
//
    if (old_fov != scr_fov.value) {
	old_fov = scr_fov.value;
	vid.recalc_refdef = true;
    }

    if (old_viewsize != scr_viewsize.value) {
	old_viewsize = scr_viewsize.value;
	vid.recalc_refdef = true;
    }

#ifdef QW_HACK
    if (oldsbar != cl_sbar.value) {
	oldsbar = cl_sbar.value;
	vid.recalc_refdef = true;
    }
#endif

    if (vid.recalc_refdef)
	SCR_CalcRefdef();

//
// do 3D refresh drawing, and then update the screen
//
    D_EnableBackBufferAccess();	// of all overlay stuff if drawing directly

    if (scr_fullupdate++ < vid.numpages) {	// clear the entire screen
	scr_copyeverything = 1;
	Draw_TileClear(0, 0, vid.width, vid.height);
	Sbar_Changed();
    }

    pconupdate = NULL;

    SCR_SetUpToDrawConsole();
    SCR_EraseCenterString();

    D_DisableBackBufferAccess();	// for adapters that can't stay mapped in
    //  for linear writes all the time

    VID_LockBuffer();
    V_RenderView();
    VID_UnlockBuffer();

    D_EnableBackBufferAccess();	// of all overlay stuff if drawing directly

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
#ifdef NQ_HACK
    } else if (cl.intermission == 3 && key_dest == key_game) {
	SCR_DrawCenterString();
#endif
    } else {
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

    D_DisableBackBufferAccess();	// for adapters that can't stay mapped in
    //  for linear writes all the time
    if (pconupdate) {
	D_UpdateRects(pconupdate);
    }

    V_UpdatePalette();

//
// update one of three areas
//
    if (scr_copyeverything) {
	vrect.x = 0;
	vrect.y = 0;
	vrect.width = vid.width;
	vrect.height = vid.height;
	vrect.pnext = 0;

	VID_Update(&vrect);
    } else if (scr_copytop) {
	vrect.x = 0;
	vrect.y = 0;
	vrect.width = vid.width;
	vrect.height = vid.height - sb_lines;
	vrect.pnext = 0;

	VID_Update(&vrect);
    } else {
	vrect.x = scr_vrect.x;
	vrect.y = scr_vrect.y;
	vrect.width = scr_vrect.width;
	vrect.height = scr_vrect.height;
	vrect.pnext = 0;

	VID_Update(&vrect);
    }
}


/*
==================
SCR_UpdateWholeScreen
==================
*/
void
SCR_UpdateWholeScreen(void)
{
    scr_fullupdate = 0;
    SCR_UpdateScreen();
}
