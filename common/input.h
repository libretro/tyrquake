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

#ifndef INPUT_H
#define INPUT_H

// input.h -- external (non-keyboard) input devices

#include "qtypes.h"
#include "cvar.h"

#ifdef NQ_HACK
#include "client.h"
#endif

#ifdef QW_HACK
#include "protocol.h"
#endif

// FIXME - windows only?
extern cvar_t _windowed_mouse;

void IN_Init(void);
void IN_Shutdown(void);

void IN_Move(usercmd_t *cmd);
// add additional movement on top of the keyboard move cmd

#endif /* INPUT_H */
