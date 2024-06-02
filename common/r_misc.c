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
// r_misc.c

#include "console.h"
#include "draw.h"
#include "menu.h"
#include "quakedef.h"
#include "r_local.h"
#include "render.h"
#include "sbar.h"
#ifdef NQ_HACK
#include "host.h"
#include "server.h"
#endif
#include "sys.h"

/*
===============
R_CheckVariables
===============
*/
static void
R_CheckVariables(void)
{
    // FIXME - do it right (cvar callback)
    static float oldbright;

    if (r_fullbright.value != oldbright) {
	oldbright = r_fullbright.value;
	D_FlushCaches();	// so all lighting changes
    }
}

/*
============
Show

Debugging use
============
*/
void
Show(void)
{
    vrect_t vr;

    vr.x = vr.y = 0;
    vr.width = vid.width;
    vr.height = vid.height;
    vr.pnext = NULL;
    VID_Update(&vr);
}

/*
=============
R_PrintAliasStats
=============
*/
void
R_PrintAliasStats(void)
{
    Con_Printf("%3i polygon model drawn\n", r_amodels_drawn);
}

void
WarpPalette(void)
{
    int i, j;
    byte newpalette[768];
    int basecolor[3];

    basecolor[0] = 130;
    basecolor[1] = 80;
    basecolor[2] = 50;

// pull the colors halfway to bright brown
    for (i = 0; i < 256; i++) {
	for (j = 0; j < 3; j++) {
	    newpalette[i * 3 + j] =
		(host_basepal[i * 3 + j] + basecolor[j]) / 2;
	}
    }

    VID_ShiftPalette(newpalette);
}

/*
===================
R_TransformFrustum
===================
*/
void
R_TransformFrustum(void)
{
    int i;
    vec3_t v, v2;
    mplane_t *plane;

#ifdef NQ_HACK
    if (r_lockfrustum.value)
	return;
#endif

    for (i = 0; i < 4; i++) {
	v[0] = screenedge[i].normal[2];
	v[1] = -screenedge[i].normal[0];
	v[2] = screenedge[i].normal[1];

	v2[0] = v[1] * vright[0] + v[2] * vup[0] + v[0] * vpn[0];
	v2[1] = v[1] * vright[1] + v[2] * vup[1] + v[0] * vpn[1];
	v2[2] = v[1] * vright[2] + v[2] * vup[2] + v[0] * vpn[2];

	plane = &view_clipplanes[i].plane;
	VectorCopy(v2, plane->normal);
	plane->dist = DotProduct(modelorg, v2);
	plane->signbits = SignbitsForPlane(plane);
    }
}

/*
================
TransformVector
================
*/
void
TransformVector(vec3_t in, vec3_t out)
{
    out[0] = DotProduct(in, vright);
    out[1] = DotProduct(in, vup);
    out[2] = DotProduct(in, vpn);
}

/*
================
R_TransformPlane
================
*/
void
R_TransformPlane(mplane_t *p, float *normal, float *dist)
{
    float d;

    d = DotProduct(r_origin, p->normal);
    *dist = p->dist - d;
// TODO: when we have rotating entities, this will need to use the view matrix
    TransformVector(p->normal, normal);
}

/*
===============
R_SetupFrame
===============
*/
void
R_SetupFrame(void)
{
    int edgecount;
    vrect_t vrect;
    float w, h;

// don't allow cheats in multiplayer
#ifdef NQ_HACK
    if (cl.maxclients > 1) {
	Cvar_Set("r_draworder", "0");
	Cvar_Set("r_fullbright", "0");
	Cvar_Set("r_ambient", "0");
    }
#endif
#ifdef QW_HACK
    r_draworder.value = 0;
    r_fullbright.value = 0;
    r_ambient.value = 0;
#endif

    if (r_numsurfs.value) {
	if ((surface_p - surfaces) > r_maxsurfsseen)
	    r_maxsurfsseen = surface_p - surfaces;

	Con_Printf("Used %d of %d surfs; %d max\n",
		   (int)(surface_p - surfaces),
		   (int)(surf_max - surfaces), r_maxsurfsseen);
    }

    if (r_numedges.value) {
	edgecount = edge_p - r_edges;

	if (edgecount > r_maxedgesseen)
	    r_maxedgesseen = edgecount;

	Con_Printf("Used %d of %d edges; %d max\n", edgecount,
		   r_numallocatededges, r_maxedgesseen);
    }

    r_refdef.ambientlight = r_ambient.value;

    if (r_refdef.ambientlight < 0)
	r_refdef.ambientlight = 0;

#ifdef NQ_HACK
    if (!sv.active)
	r_draworder.value = 0;	// don't let cheaters look behind walls
#endif
#ifdef QW_HACK
    r_draworder.value = 0;	// don't let cheaters look behind walls
#endif

    R_CheckVariables();

    R_AnimateLight();

    r_framecount++;

    // build the transformation matrix for the given view angles
    VectorCopy(r_refdef.vieworg, modelorg);
    VectorCopy(r_refdef.vieworg, r_origin);

    AngleVectors(r_refdef.viewangles, vpn, vright, vup);

    // current viewleaf
    r_oldviewleaf = r_viewleaf;
    r_viewleaf = Mod_PointInLeaf(cl.worldmodel, r_origin);

    r_dowarpold = r_dowarp;
    r_dowarp = r_waterwarp.value && (r_viewleaf->contents <= CONTENTS_WATER);

    if ((r_dowarp != r_dowarpold) || r_viewchanged) {
	if (r_dowarp) {
	    if ((vid.width <= vid.maxwarpwidth) &&
		(vid.height <= vid.maxwarpheight)) {
		vrect.x = 0;
		vrect.y = 0;
		vrect.width = vid.width;
		vrect.height = vid.height;

		R_ViewChanged(&vrect, sb_lines, vid.aspect);
	    } else {
		w = vid.width;
		h = vid.height;

		if (w > vid.maxwarpwidth) {
		    h *= (float)vid.maxwarpwidth / w;
		    w = vid.maxwarpwidth;
		}

		if (h > vid.maxwarpheight) {
		    h = vid.maxwarpheight;
		    w *= (float)vid.maxwarpheight / h;
		}

		vrect.x = 0;
		vrect.y = 0;
		vrect.width = (int)w;
		vrect.height = (int)h;

		R_ViewChanged(&vrect,
			      (int)((float)sb_lines *
				    (h / (float)vid.height)),
			      vid.aspect * (h / w) * ((float)vid.width /
						      (float)vid.height));
	    }
	} else {
	    vrect.x = 0;
	    vrect.y = 0;
	    vrect.width = vid.width;
	    vrect.height = vid.height;

	    R_ViewChanged(&vrect, sb_lines, vid.aspect);
	}

	r_viewchanged = false;
    }
// start off with just the four screen edge clip planes
    R_TransformFrustum();

// save base values
    VectorCopy(vpn, base_vpn);
    VectorCopy(vright, base_vright);
    VectorCopy(vup, base_vup);
    VectorCopy(modelorg, base_modelorg);

    R_SetSkyFrame();

    // clear frame counts
    r_drawnpolycount = 0;
    r_amodels_drawn = 0;

    D_SetupFrame();
}
