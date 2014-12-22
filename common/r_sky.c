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
// r_sky.c

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"


int iskyspeed = 8;
int iskyspeed2 = 2;
float skyspeed, skyspeed2;

float skytime;

byte *r_skysource;

int r_skydirect;		// not used?

/*
=============
R_InitSky

A sky texture is 256*128, with the right side being a masked overlay
==============
*/
void R_InitSky (texture_t *mt)
{

   skyoverlay = (byte *)mt + mt->offsets[0]; // Manoel Kasimier - smooth sky
   skyunderlay = skyoverlay+128; // Manoel Kasimier - smooth sky
}

/*
=============
R_SetSkyFrame
==============
*/
void
R_SetSkyFrame(void)
{
    int g, s1, s2;
    float temp;

    skyspeed = iskyspeed;
    skyspeed2 = iskyspeed2;

    g = GreatestCommonDivisor(iskyspeed, iskyspeed2);
    s1 = iskyspeed / g;
    s2 = iskyspeed2 / g;
    temp = SKYSIZE * s1 * s2;

    skytime = cl.time - ((int)(cl.time / temp) * temp);
}
