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
// gl_mesh.c: triangle model functions

#include "common.h"
#include "console.h"
#include "glquake.h"
#include "model.h"
#include "quakedef.h"
#include "sys.h"

/*
=================================================================

ALIAS MODEL DISPLAY LIST GENERATION

=================================================================
*/

static int used[8192];

// the command list holds counts and s/t values that are valid for
// every frame
union {
    int i;
    float f;
} commands[8192];
static int numcommands;

// all frames will have their vertexes rearranged and expanded
// so they are in the order expected by the command list
static int vertexorder[8192];
static int numorder;

static int allverts, alltris;

static int stripverts[128];
static int striptris[128];
static int stripcount;

/*
================
StripLength
================
*/
static int
StripLength(aliashdr_t *hdr, int starttri, int startv, const mtriangle_t *tris)
{
    const mtriangle_t *last, *check;
    int m1, m2;
    int j;
    int k;

    used[starttri] = 2;

    last = &tris[starttri];

    stripverts[0] = last->vertindex[(startv) % 3];
    stripverts[1] = last->vertindex[(startv + 1) % 3];
    stripverts[2] = last->vertindex[(startv + 2) % 3];

    striptris[0] = starttri;
    stripcount = 1;

    m1 = last->vertindex[(startv + 2) % 3];
    m2 = last->vertindex[(startv + 1) % 3];

    // look for a matching triangle
  nexttri:
    for (j = starttri + 1, check = &tris[starttri + 1];
	 j < hdr->numtris; j++, check++) {
	if (check->facesfront != last->facesfront)
	    continue;
	for (k = 0; k < 3; k++) {
	    if (check->vertindex[k] != m1)
		continue;
	    if (check->vertindex[(k + 1) % 3] != m2)
		continue;

	    // this is the next part of the fan

	    // if we can't use this triangle, this tristrip is done
	    if (used[j])
		goto done;

	    // the new edge
	    if (stripcount & 1)
		m2 = check->vertindex[(k + 2) % 3];
	    else
		m1 = check->vertindex[(k + 2) % 3];

	    stripverts[stripcount + 2] = check->vertindex[(k + 2) % 3];
	    striptris[stripcount] = j;
	    stripcount++;

	    used[j] = 2;
	    goto nexttri;
	}
    }
  done:

    // clear the temp used flags
    for (j = starttri + 1; j < hdr->numtris; j++)
	if (used[j] == 2)
	    used[j] = 0;

    return stripcount;
}

/*
===========
FanLength
===========
*/
static int
FanLength(aliashdr_t *hdr, int starttri, int startv, const mtriangle_t *tris)
{
    const mtriangle_t *last, *check;
    int m1, m2;
    int j;
    int k;

    used[starttri] = 2;

    last = &tris[starttri];

    stripverts[0] = last->vertindex[(startv) % 3];
    stripverts[1] = last->vertindex[(startv + 1) % 3];
    stripverts[2] = last->vertindex[(startv + 2) % 3];

    striptris[0] = starttri;
    stripcount = 1;

    m1 = last->vertindex[(startv + 0) % 3];
    m2 = last->vertindex[(startv + 2) % 3];


    // look for a matching triangle
  nexttri:
    for (j = starttri + 1, check = &tris[starttri + 1];
	 j < hdr->numtris; j++, check++) {
	if (check->facesfront != last->facesfront)
	    continue;
	for (k = 0; k < 3; k++) {
	    if (check->vertindex[k] != m1)
		continue;
	    if (check->vertindex[(k + 1) % 3] != m2)
		continue;

	    // this is the next part of the fan

	    // if we can't use this triangle, this tristrip is done
	    if (used[j])
		goto done;

	    // the new edge
	    m2 = check->vertindex[(k + 2) % 3];

	    stripverts[stripcount + 2] = m2;
	    striptris[stripcount] = j;
	    stripcount++;

	    used[j] = 2;
	    goto nexttri;
	}
    }
  done:

    // clear the temp used flags
    for (j = starttri + 1; j < hdr->numtris; j++)
	if (used[j] == 2)
	    used[j] = 0;

    return stripcount;
}


/*
================
BuildTris

Generate a list of trifans or strips
for the model, which holds for all frames
================
*/
static void
BuildTris(aliashdr_t *hdr, const mtriangle_t *tris, const stvert_t *stverts)
{
    int i, j, k;
    int startv;
    float s, t;
    int len, bestlen, besttype;
    int bestverts[1024];
    int besttris[1024];
    int type;

    //
    // build tristrips
    //
    numorder = 0;
    numcommands = 0;
    memset(used, 0, sizeof(used));
    for (i = 0; i < hdr->numtris; i++) {
	// pick an unused triangle and start the trifan
	if (used[i])
	    continue;

	bestlen = 0;
	besttype = 0;
	for (type = 0; type < 2; type++) {
	    for (startv = 0; startv < 3; startv++) {
		if (type == 1)
		    len = StripLength(hdr, i, startv, tris);
		else
		    len = FanLength(hdr, i, startv, tris);
		if (len > bestlen) {
		    besttype = type;
		    bestlen = len;
		    for (j = 0; j < bestlen + 2; j++)
			bestverts[j] = stripverts[j];
		    for (j = 0; j < bestlen; j++)
			besttris[j] = striptris[j];
		}
	    }
	}

	// mark the tris on the best strip as used
	for (j = 0; j < bestlen; j++)
	    used[besttris[j]] = 1;

	if (besttype == 1)
	    commands[numcommands++].i = (bestlen + 2);
	else
	    commands[numcommands++].i = -(bestlen + 2);

	for (j = 0; j < bestlen + 2; j++) {
	    // emit a vertex into the reorder buffer
	    k = bestverts[j];
	    vertexorder[numorder++] = k;

	    // emit s/t coords into the commands stream
	    s = stverts[k].s;
	    t = stverts[k].t;
	    if (!tris[besttris[0]].facesfront && stverts[k].onseam)
		s += hdr->skinwidth / 2;	// on back side
	    s = (s + 0.5) / hdr->skinwidth;
	    t = (t + 0.5) / hdr->skinheight;

	    commands[numcommands++].f = s;
	    commands[numcommands++].f = t;
	}
    }

    commands[numcommands++].i = 0;	// end of list marker

    Con_DPrintf("%3i tri %3i vert %3i cmd\n", hdr->numtris, numorder,
		numcommands);

    allverts += numorder;
    alltris += hdr->numtris;
}


/*
================
GL_MakeAliasModelDisplayLists
================
*/
void
GL_LoadMeshData(const model_t *model, aliashdr_t *hdr, const mtriangle_t *tris,
		const stvert_t *stverts, const trivertx_t **poseverts)
{
    int i, j;
    int *cmds;
    trivertx_t *verts;
    char cache[MAX_QPATH];
    FILE *f;

    //
    // look for a cached version
    //
    sprintf(cache, "%s/glquake/", com_gamedir);
    COM_StripExtension(model->name + strlen("progs/"), cache + strlen(cache));
    strcat(cache, ".ms2");

    f = fopen(cache, "rb");
    if (f) {
	fread(&numcommands, 4, 1, f);
	fread(&numorder, 4, 1, f);
	fread(&commands, numcommands * sizeof(commands[0]), 1, f);
	fread(&vertexorder, numorder * sizeof(vertexorder[0]), 1, f);
	fclose(f);
    } else {
	//
	// build it from scratch
	//
	Con_DPrintf("meshing %s...\n", model->name);
	BuildTris(hdr, tris, stverts);	/* trifans or lists */

	//
	// save out the cached version
	//
	f = fopen(cache, "wb");
	if (!f) {
	    // Maybe the directory wasn't present, try again
	    char gldir[MAX_OSPATH];

	    sprintf(gldir, "%s/glquake", com_gamedir);
	    Sys_mkdir(gldir);
	    f = fopen(cache, "wb");
	}

	if (f) {
	    fwrite(&numcommands, 4, 1, f);
	    fwrite(&numorder, 4, 1, f);
	    fwrite(&commands, numcommands * sizeof(commands[0]), 1, f);
	    fwrite(&vertexorder, numorder * sizeof(vertexorder[0]), 1, f);
	    fclose(f);
	}
    }


    // save the data out

    hdr->numverts = numorder;

    cmds = Hunk_Alloc(numcommands * 4);
    GL_Aliashdr(hdr)->commands = (byte *)cmds - (byte *)hdr;
    memcpy(cmds, commands, numcommands * 4);

    verts = Hunk_Alloc(hdr->numposes * hdr->numverts * sizeof(trivertx_t));
    hdr->posedata = (byte *)verts - (byte *)hdr;
    for (i = 0; i < hdr->numposes; i++)
	for (j = 0; j < numorder; j++)
	    *verts++ = poseverts[i][vertexorder[j]];
}
