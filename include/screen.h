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

#ifndef SCREEN_H
#define SCREEN_H

#include "qtypes.h"
#include "cvar.h"
#include "vid.h"

// screen.h

void SCR_Init(void);
void SCR_UpdateScreen(void);
void SCR_UpdateWholeScreen(void);
void SCR_SizeUp(void);
void SCR_SizeDown(void);
void SCR_BringDownConsole(void);
void SCR_CenterPrint(const char *str);
void SCR_BeginLoadingPlaque(void);
void SCR_EndLoadingPlaque(void);
int SCR_ModalMessage(const char *text);

extern qboolean scr_drawdialog;
void SCR_DrawCenterString(void);
void SCR_DrawNotifyString(void);
void SCR_EraseCenterString(void);
void SCR_CalcRefdef(void); /* internal use only */
void SCR_SizeUp_f(void);
void SCR_SizeDown_f(void);
void SCR_ScreenShot_f(void);
#ifdef QW_HACK
void SCR_RSShot_f(void);
extern cvar_t scr_allowsnap;
#endif

void SCR_DrawRam(void);
void SCR_DrawTurtle(void);
void SCR_DrawNet(void);
void SCR_DrawFPS(void);
void SCR_DrawPause(void);
void SCR_SetUpToDrawConsole(void);
void SCR_DrawConsole(void);

extern qboolean scr_initialized;
extern float scr_con_current;
extern float scr_conlines;	// lines of console to display

extern float scr_centertime_off;
extern int scr_fullupdate;	// set to 0 to force full redraw

extern int clearnotify;		// set to 0 whenever notify text is drawn
extern int clearconsole;
extern qboolean scr_disabled_for_loading;
extern qboolean scr_skipupdate;
extern qboolean scr_block_drawing;
extern qboolean scr_drawloading;

extern cvar_t scr_centertime;
extern cvar_t scr_printspeed;
extern cvar_t scr_viewsize;
extern cvar_t scr_fov;
extern cvar_t scr_conspeed;
#ifdef GLQUAKE
extern cvar_t gl_triplebuffer;
#endif

extern vrect_t scr_vrect;

// only the refresh window will be updated unless these variables are flagged
extern int scr_copytop;
extern int scr_copyeverything;

#endif /* SCREEN_H */
