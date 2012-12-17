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

#ifdef QW_HACK
static float oldsbar;
cvar_t scr_allowsnap = { "scr_allowsnap", "1" };
#endif

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

#ifdef NQ_HACK
    if (cls.state == ca_dedicated)
	return;			// stdout only
#endif

    if (!scr_initialized || !con_initialized)
	return;			// not initialized yet

    scr_copytop = 0;
    scr_copyeverything = 0;

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
