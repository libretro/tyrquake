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

#include <string.h>

#include "client.h"
#include "cmd.h"
#include "common.h"
#include "console.h"
#include "draw.h"
#include "keys.h"
#include "menu.h"
#include "quakedef.h"
#include "sbar.h"
#include "screen.h"
#include "sound.h"
#include "sys.h"
#include "view.h"

#include "d_iface.h"
#include "r_local.h"

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

static qboolean scr_initialized;	/* ready to draw */

// only the refresh window will be updated unless these variables are flagged
int scr_copytop;
int scr_copyeverything;

float scr_con_current;
static float scr_conlines;		/* lines of console to display */

int scr_fullupdate;
static int clearconsole;
int clearnotify;

vrect_t scr_vrect;

qboolean scr_disabled_for_loading;
qboolean scr_block_drawing;

static cvar_t scr_centertime = { "scr_centertime", "2" };
static cvar_t scr_printspeed = { "scr_printspeed", "8" };

cvar_t scr_viewsize = { "viewsize", "100", true };
cvar_t scr_fov = { "fov", "90" };	// 10 - 170
static cvar_t scr_conspeed = { "scr_conspeed", "300" };
static vrect_t *pconupdate;
qboolean scr_skipupdate;

static const qpic_t *scr_ram;
static const qpic_t *scr_net;

static char scr_centerstring[1024];
static float scr_centertime_start;	// for slow victory printing
float scr_centertime_off;
static int scr_center_lines;
static int scr_erase_lines;
static int scr_erase_center;

#ifdef NQ_HACK
static qboolean scr_drawloading;
static float scr_disabled_time;
#endif
#ifdef QW_HACK
static float oldsbar;
static cvar_t scr_allowsnap = { "scr_allowsnap", "1" };
#endif


//=============================================================================

/*
==============
SCR_DrawNet
==============
*/
static void
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

//=============================================================================

/*
==================
SCR_SetUpToDrawConsole
==================
*/
static void SCR_SetUpToDrawConsole(void)
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
static void SCR_DrawConsole(void)
{
   if (scr_con_current)
   {
      scr_copyeverything = 1;
      Con_DrawConsole(scr_con_current);
      clearconsole = 0;
   }
   else
   {
      if (key_dest == key_game || key_dest == key_message)
         Con_DrawNotify();	// only draw notify in game
   }
}

/*
===============================================================================

CENTER PRINTING

===============================================================================
*/

/*
==============
SCR_CenterPrint

Called for important messages that should stay in the center of the screen
for a few moments
==============
*/
void
SCR_CenterPrint(const char *str)
{
   strncpy(scr_centerstring, str, sizeof(scr_centerstring));
   scr_centerstring[sizeof(scr_centerstring) - 1] = 0;
   scr_centertime_off = scr_centertime.value;
   scr_centertime_start = cl.time;

   /* count the number of lines for centering */
   scr_center_lines = 1;
   while (*str)
   {
      if (*str == '\n')
         scr_center_lines++;
      str++;
   }
}

static void
SCR_EraseCenterString(void)
{
    int y, height;

    if (scr_erase_center++ > vid.numpages) {
	scr_erase_lines = 0;
	return;
    }

    if (scr_center_lines <= 4)
	y = vid.height * 0.35;
    else
	y = 48;

    /* Make sure we don't draw off the bottom of the screen*/
    height = qmin(8 * scr_erase_lines, ((int)vid.height) - y - 1);

    scr_copytop = 1;
    Draw_TileClear(0, y, vid.width, height);
}

static void
SCR_DrawCenterString(void)
{
    char *start;
    int l;
    int j;
    int x, y;
    int remaining;

    scr_copytop = 1;
    if (scr_center_lines > scr_erase_lines)
	scr_erase_lines = scr_center_lines;

    scr_centertime_off -= host_frametime;

    if (scr_centertime_off <= 0 && !cl.intermission)
	return;
    if (key_dest != key_game)
	return;

// the finale prints the characters one at a time
    if (cl.intermission)
	remaining = scr_printspeed.value * (cl.time - scr_centertime_start);
    else
	remaining = 9999;

    scr_erase_center = 0;
    start = scr_centerstring;

    if (scr_center_lines <= 4)
	y = vid.height * 0.35;
    else
	y = 48;

    do {
	// scan the width of the line
	for (l = 0; l < 40; l++)
	    if (start[l] == '\n' || !start[l])
		break;
	x = (vid.width - l * 8) / 2;
	for (j = 0; j < l; j++, x += 8) {
	    Draw_Character(x, y, start[j]);
	    if (!remaining--)
		return;
	}

	y += 8;

	while (*start && *start != '\n')
	    start++;

	if (!*start)
	    break;
	start++;		// skip the \n
    } while (1);
}

//=============================================================================

static const char *scr_notifystring;
static qboolean scr_drawdialog;

static void
SCR_DrawNotifyString(void)
{
    const char *start;
    int l;
    int j;
    int x, y;

    start = scr_notifystring;

    y = vid.height * 0.35;

    do {
	// scan the width of the line
	for (l = 0; l < 40; l++)
	    if (start[l] == '\n' || !start[l])
		break;
	x = (vid.width - l * 8) / 2;
	for (j = 0; j < l; j++, x += 8)
	    Draw_Character(x, y, start[j]);

	y += 8;

	while (*start && *start != '\n')
	    start++;

	if (!*start)
	    break;
	start++;		// skip the \n
    } while (1);
}


/*
==================
SCR_ModalMessage

Displays a text string in the center of the screen and waits for a Y or N
keypress.
==================
*/
int
SCR_ModalMessage(const char *text)
{
#ifdef NQ_HACK
    if (cls.state == ca_dedicated)
	return true;
#endif

    scr_notifystring = text;

// draw a fresh screen
    scr_fullupdate = 0;
    scr_drawdialog = true;
    SCR_UpdateScreen();
    scr_drawdialog = false;

    S_ClearBuffer();		// so dma doesn't loop current sound

    do {
	key_count = -1;		// wait for a key down and up
	Sys_SendKeyEvents();
    } while (key_lastpress != 'y' && key_lastpress != 'n'
	     && key_lastpress != K_ESCAPE);

    scr_fullupdate = 0;
    SCR_UpdateScreen();

    return key_lastpress == 'y';
}

//============================================================================

/*
====================
CalcFov
====================
*/
static float
CalcFov(float fov_x, float width, float height)
{
    float a;
    float x;

    if (fov_x < 1 || fov_x > 179)
	Sys_Error("Bad fov: %f", fov_x);

    x = width / tan(fov_x / 360 * M_PI);
    a = atan(height / x);
    a = a * 360 / M_PI;

    return a;
}


/*
=================
SCR_CalcRefdef

Must be called whenever vid changes
Internal use only
=================
*/
static void SCR_CalcRefdef(void)
{
   vrect_t vrect;
   float size;

   scr_fullupdate = 0;		// force a background redraw
   vid.recalc_refdef = 0;

   // force the status bar to redraw
   Sbar_Changed();

   //========================================

   // bound viewsize
   if (scr_viewsize.value < 30)
      Cvar_Set("viewsize", "30");
   if (scr_viewsize.value > 120)
      Cvar_Set("viewsize", "120");

   // bound field of view
   if (scr_fov.value < 10)
      Cvar_Set("fov", "10");
   if (scr_fov.value > 170)
      Cvar_Set("fov", "170");

   // intermission is always full screen
   if (cl.intermission)
      size = 120;
   else
      size = scr_viewsize.value;

   if (size >= 120)
      sb_lines = 0;		// no status bar at all
   else if (size >= 110)
      sb_lines = 24;		// no inventory
   else
      sb_lines = 24 + 16 + 8;

   // these calculations mirror those in R_Init() for r_refdef, but take no
   // account of water warping
   vrect.x = 0;
   vrect.y = 0;
   vrect.width = vid.width;
   vrect.height = vid.height;

   R_SetVrect(&vrect, &scr_vrect, sb_lines);

   r_refdef.fov_x = scr_fov.value;
   r_refdef.fov_y =
      CalcFov(r_refdef.fov_x, r_refdef.vrect.width, r_refdef.vrect.height);

   // guard against going from one mode to another that's less than half the
   // vertical resolution
   if (scr_con_current > vid.height)
      scr_con_current = vid.height;

   // notify the refresh of the change
   R_ViewChanged(&vrect, sb_lines, vid.aspect);
}

/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
static void
SCR_SizeUp_f(void)
{
    Cvar_SetValue("viewsize", scr_viewsize.value + 10);
    vid.recalc_refdef = 1;
}


/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
static void SCR_SizeDown_f(void)
{
   Cvar_SetValue("viewsize", scr_viewsize.value - 10);
   vid.recalc_refdef = 1;
}

#ifdef NQ_HACK
/*
===============
SCR_BeginLoadingPlaque

================
*/
void SCR_BeginLoadingPlaque(void)
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
static void SCR_DrawLoading(void)
{
   const qpic_t *pic;

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
void SCR_EndLoadingPlaque(void)
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

   if (scr_skipupdate)
      return;
   if (scr_block_drawing)
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

#ifdef NQ_HACK
   if (cls.state == ca_dedicated)
      return;			// stdout only
#endif

   if (!scr_initialized || !con_initialized)
      return;			// not initialized yet

   scr_copytop = 0;
   scr_copyeverything = 0;

   /*
    * Check for vid setting changes
    */
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

   /*
    * do 3D refresh drawing, and then update the screen
    */

   if (scr_fullupdate++ < vid.numpages) {
      /* clear the entire screen */
      scr_copyeverything = 1;
      Draw_TileClear(0, 0, vid.width, vid.height);
      Sbar_Changed();
   }
   pconupdate = NULL;
   SCR_SetUpToDrawConsole();
   SCR_EraseCenterString();

   V_RenderView();

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
#if defined(NQ_HACK) /* FIXME? */
   } else if (cl.intermission == 3 && key_dest == key_game) {
      SCR_DrawCenterString();
#endif
   } else {
      SCR_DrawNet();
      SCR_DrawCenterString();
      Sbar_Draw();
      SCR_DrawConsole();
      M_Draw();
   }

   if (pconupdate)
      D_UpdateRects(pconupdate);

   V_UpdatePalette();

   /*
    * update one of three areas
    */
   if (scr_copyeverything)
   {
      vrect.x = 0;
      vrect.y = 0;
      vrect.width = vid.width;
      vrect.height = vid.height;
   }
   else if (scr_copytop)
   {
      vrect.x = 0;
      vrect.y = 0;
      vrect.width = vid.width;
      vrect.height = vid.height - sb_lines;
   }
   else
   {
      vrect.x = scr_vrect.x;
      vrect.y = scr_vrect.y;
      vrect.width = scr_vrect.width;
      vrect.height = scr_vrect.height;
   }
   vrect.pnext = 0;
   VID_Update(&vrect);
}

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
    Cvar_RegisterVariable(&scr_centertime);
    Cvar_RegisterVariable(&scr_printspeed);

    Cmd_AddCommand("sizeup", SCR_SizeUp_f);
    Cmd_AddCommand("sizedown", SCR_SizeDown_f);

#ifdef NQ_HACK
    scr_ram = (qpic_t*)Draw_PicFromWad("ram");
    scr_net = (qpic_t*)Draw_PicFromWad("net");
#endif
#ifdef QW_HACK
    scr_ram = W_GetLumpName("ram");
    scr_net = W_GetLumpName("net");

    Cvar_RegisterVariable(&scr_allowsnap);
#endif

    scr_initialized = true;
}
