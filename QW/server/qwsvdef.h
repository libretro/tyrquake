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

#ifndef SERVER_QWSVDEF_H
#define SERVER_QWSVDEF_H

// quakedef.h -- primary header for server

#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <setjmp.h>
#include <ctype.h>

#include "qtypes.h"
#include "cvar.h"

//=============================================================================

// the host system specifies the base of the directory tree, the
// command line parms passed to the program, and the amount of memory
// available for the program to use

typedef struct {
    const char *basedir;
    int argc;
    const char **argv;
    void *membase;
    int memsize;
} quakeparms_t;


//=============================================================================

//
// host
//
extern quakeparms_t host_parms;

extern qboolean host_initialized;	// true if into command execution
extern double host_frametime;
extern double realtime;		// not bounded in any way, changed at

										// start of every frame, never reset

void SV_Error(const char *fmt, ...)
    __attribute__((noreturn, format(printf,1,2)));
void SV_Init(quakeparms_t *parms);

#endif /* SERVER_QWSVDEF_H */
