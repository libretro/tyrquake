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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include <float.h>
#include <stdint.h>

#include "common.h"
#include "console.h"
#include "model.h"

#ifdef GLQUAKE
#include "glquake.h"
#endif

#ifdef SERVERONLY
#include "qwsvdef.h"
/* A dummy texture to point to. FIXME - should server care about textures? */
static texture_t r_notexture_mip_qwsv;
#else
#include "quakedef.h"
#include "render.h"
#include "sys.h"
#ifdef QW_HACK
#include "crc.h"
#endif
/* FIXME - quick hack to enable merging of NQ/QWSV shared code */
#define SV_Error Sys_Error
#endif

static model_t *loadmodel;
static char loadname[MAX_QPATH];	/* for hunk tags */

static void Mod_LoadBrushModel(model_t *mod, void *buffer, unsigned long size);
static model_t *Mod_LoadModel(model_t *mod, qboolean crash);

static byte mod_novis[MAX_MAP_LEAFS / 8];

#define MAX_MOD_KNOWN 512
static model_t mod_known[MAX_MOD_KNOWN];
static int mod_numknown;

#ifdef GLQUAKE
cvar_t gl_subdivide_size = { "gl_subdivide_size", "128", true };
#ifdef QW_HACK
byte player_8bit_texels[320 * 200];
#endif
#endif

static const model_loader_t *mod_loader;

/*
===============
Mod_Init
===============
*/
void
Mod_Init(const model_loader_t *loader)
{
#ifdef GLQUAKE
    Cvar_RegisterVariable(&gl_subdivide_size);
#endif
    memset(mod_novis, 0xff, sizeof(mod_novis));
    mod_loader = loader;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *
Mod_PointInLeaf(vec3_t p, model_t *model)
{
    mnode_t *node;
    float d;
    mplane_t *plane;

    if (!model || !model->nodes)
	SV_Error("%s: bad model", __func__);

    node = model->nodes;
    while (1) {
	if (node->contents < 0)
	    return (mleaf_t *)node;
	plane = node->plane;
	d = DotProduct(p, plane->normal) - plane->dist;
	if (d > 0)
	    node = node->children[0];
	else
	    node = node->children[1];
    }

    return NULL;		// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
static byte *
Mod_DecompressVis(byte *in, model_t *model)
{
    static byte decompressed[MAX_MAP_LEAFS / 8];
    int c;
    byte *out;
    int row;

    row = (model->numleafs + 7) >> 3;
    out = decompressed;

    if (!in) {			// no vis info, so make all visible
	while (row) {
	    *out++ = 0xff;
	    row--;
	}
	return decompressed;
    }

    do {
	if (*in) {
	    *out++ = *in++;
	    continue;
	}

	c = in[1];
	in += 2;
	while (c) {
	    *out++ = 0;
	    c--;
	}
    } while (out - decompressed < row);

    return decompressed;
}

byte *
Mod_LeafPVS(mleaf_t *leaf, model_t *model)
{
    if (leaf == model->leafs)
	return mod_novis;
    return Mod_DecompressVis(leaf->compressed_vis, model);
}

/*
===================
Mod_ClearAll
===================
*/
void
Mod_ClearAll(void)
{
    int i;
    model_t *mod;

    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++) {
	if (mod->type != mod_alias)
	    mod->needload = true;
	/*
	 * FIXME: sprites use the cache data pointer for their own purposes,
	 *        bypassing the Cache_Alloc/Free functions.
	 */
	if (mod->type == mod_sprite)
	    mod->cache.data = NULL;
    }
}

/*
==================
Mod_FindName

==================
*/
static model_t *
Mod_FindName(const char *name)
{
    int i;
    model_t *mod;

    if (!name[0])
	SV_Error("%s: NULL name", __func__);

//
// search the currently loaded models
//
    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	if (!strcmp(mod->name, name))
	    break;

    if (i == mod_numknown) {
	if (mod_numknown == MAX_MOD_KNOWN)
	    SV_Error("mod_numknown == MAX_MOD_KNOWN");
	strncpy(mod->name, name, MAX_QPATH - 1);
	mod->name[MAX_QPATH - 1] = 0;
	mod->needload = true;
	mod_numknown++;
    }

    return mod;
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *
Mod_LoadModel(model_t *mod, qboolean crash)
{
    unsigned *buf;
    byte stackbuf[1024];	// avoid dirtying the cache heap
    unsigned long size;

    if (!mod->needload) {
	if (mod->type == mod_alias) {
	    if (Cache_Check(&mod->cache))
		return mod;
	} else
	    return mod;		// not cached at all
    }
//
// load the file
//
    buf = (unsigned *)COM_LoadStackFile(mod->name, stackbuf, sizeof(stackbuf),
					&size);
    if (!buf) {
	if (crash)
	    SV_Error("%s: %s not found", __func__, mod->name);
	return NULL;
    }
//
// allocate a new model
//
    COM_FileBase(mod->name, loadname, sizeof(loadname));

    loadmodel = mod;

//
// fill it in
//

// call the apropriate loader
    mod->needload = false;

    switch (LittleLong(*(unsigned *)buf)) {
#ifndef SERVERONLY
    case IDPOLYHEADER:
	Mod_LoadAliasModel(mod_loader, mod, buf, loadmodel, loadname);
	break;

    case IDSPRITEHEADER:
	Mod_LoadSpriteModel(mod, buf, loadname);
	break;
#endif
    default:
	Mod_LoadBrushModel(mod, buf, size);
	break;
    }

    return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *
Mod_ForName(const char *name, qboolean crash)
{
    model_t *mod;

    mod = Mod_FindName(name);

    return Mod_LoadModel(mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static byte *mod_base;


/*
=================
Mod_LoadTextures
=================
*/
static void
Mod_LoadTextures(lump_t *l)
{
    int i, j, pixels, num, max, altmax;
    miptex_t *mt;
    texture_t *tx, *tx2;
    texture_t *anims[10];
    texture_t *altanims[10];
    dmiptexlump_t *m;

    if (!l->filelen) {
	loadmodel->textures = NULL;
	return;
    }
    m = (dmiptexlump_t *)(mod_base + l->fileofs);

    m->nummiptex = LittleLong(m->nummiptex);

    loadmodel->numtextures = m->nummiptex;
    loadmodel->textures =
	Hunk_AllocName(m->nummiptex * sizeof(*loadmodel->textures), loadname);

    for (i = 0; i < m->nummiptex; i++) {
	m->dataofs[i] = LittleLong(m->dataofs[i]);
	if (m->dataofs[i] == -1)
	    continue;
	mt = (miptex_t *)((byte *)m + m->dataofs[i]);
	mt->width = (uint32_t)LittleLong(mt->width);
	mt->height = (uint32_t)LittleLong(mt->height);
	for (j = 0; j < MIPLEVELS; j++)
	    mt->offsets[j] = (uint32_t)LittleLong(mt->offsets[j]);

	if ((mt->width & 15) || (mt->height & 15))
	    SV_Error("Texture %s is not 16 aligned", mt->name);
	pixels = mt->width * mt->height / 64 * 85;
	tx = Hunk_AllocName(sizeof(texture_t) + pixels, loadname);
	loadmodel->textures[i] = tx;

	memcpy(tx->name, mt->name, sizeof(tx->name));
	tx->width = mt->width;
	tx->height = mt->height;
	for (j = 0; j < MIPLEVELS; j++)
	    tx->offsets[j] =
		mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t);
	// the pixels immediately follow the structures
	memcpy(tx + 1, mt + 1, pixels);

#ifndef SERVERONLY
	if (!strncmp(mt->name, "sky", 3))
	    R_InitSky(tx);
#ifdef GLQUAKE
	else {
	    texture_mode = GL_LINEAR_MIPMAP_NEAREST;	//_LINEAR;
	    tx->gl_texturenum =
		GL_LoadTexture(mt->name, tx->width, tx->height,
			       (byte *)(tx + 1), true, false);
	    texture_mode = GL_LINEAR;
	}
#endif
#endif
    }

//
// sequence the animations
//
    for (i = 0; i < m->nummiptex; i++) {
	tx = loadmodel->textures[i];
	if (!tx || tx->name[0] != '+')
	    continue;
	if (tx->anim_next)
	    continue;		// allready sequenced

	// find the number of frames in the animation
	memset(anims, 0, sizeof(anims));
	memset(altanims, 0, sizeof(altanims));

	max = tx->name[1];
	if (max >= 'a' && max <= 'z')
	    max -= 'a' - 'A';
	if (max >= '0' && max <= '9') {
	    max -= '0';
	    altmax = 0;
	    anims[max] = tx;
	    max++;
	} else if (max >= 'A' && max <= 'J') {
	    altmax = max - 'A';
	    max = 0;
	    altanims[altmax] = tx;
	    altmax++;
	} else
	    SV_Error("Bad animating texture %s", tx->name);

	for (j = i + 1; j < m->nummiptex; j++) {
	    tx2 = loadmodel->textures[j];
	    if (!tx2 || tx2->name[0] != '+')
		continue;
	    if (strcmp(tx2->name + 2, tx->name + 2))
		continue;

	    num = tx2->name[1];
	    if (num >= 'a' && num <= 'z')
		num -= 'a' - 'A';
	    if (num >= '0' && num <= '9') {
		num -= '0';
		anims[num] = tx2;
		if (num + 1 > max)
		    max = num + 1;
	    } else if (num >= 'A' && num <= 'J') {
		num = num - 'A';
		altanims[num] = tx2;
		if (num + 1 > altmax)
		    altmax = num + 1;
	    } else
		SV_Error("Bad animating texture %s", tx->name);
	}

#define	ANIM_CYCLE	2
	// link them all together
	for (j = 0; j < max; j++) {
	    tx2 = anims[j];
	    if (!tx2)
		SV_Error("Missing frame %i of %s", j, tx->name);
	    tx2->anim_total = max * ANIM_CYCLE;
	    tx2->anim_min = j * ANIM_CYCLE;
	    tx2->anim_max = (j + 1) * ANIM_CYCLE;
	    tx2->anim_next = anims[(j + 1) % max];
	    if (altmax)
		tx2->alternate_anims = altanims[0];
	}
	for (j = 0; j < altmax; j++) {
	    tx2 = altanims[j];
	    if (!tx2)
		SV_Error("Missing frame %i of %s", j, tx->name);
	    tx2->anim_total = altmax * ANIM_CYCLE;
	    tx2->anim_min = j * ANIM_CYCLE;
	    tx2->anim_max = (j + 1) * ANIM_CYCLE;
	    tx2->anim_next = altanims[(j + 1) % altmax];
	    if (max)
		tx2->alternate_anims = anims[0];
	}
    }
}

/*
=================
Mod_LoadLighting
=================
*/
static void
Mod_LoadLighting(lump_t *l)
{
    if (!l->filelen) {
	loadmodel->lightdata = NULL;
	return;
    }
    loadmodel->lightdata = Hunk_AllocName(l->filelen, loadname);
    memcpy(loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
static void
Mod_LoadVisibility(lump_t *l)
{
    if (!l->filelen) {
	loadmodel->visdata = NULL;
	return;
    }
    loadmodel->visdata = Hunk_AllocName(l->filelen, loadname);
    memcpy(loadmodel->visdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadEntities
=================
*/
static void
Mod_LoadEntities(lump_t *l)
{
    if (!l->filelen) {
	loadmodel->entities = NULL;
	return;
    }
    loadmodel->entities = Hunk_AllocName(l->filelen, loadname);
    memcpy(loadmodel->entities, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVertexes
=================
*/
static void
Mod_LoadVertexes(lump_t *l)
{
    dvertex_t *in;
    mvertex_t *out;
    int i, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->vertexes = out;
    loadmodel->numvertexes = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->position[0] = LittleFloat(in->point[0]);
	out->position[1] = LittleFloat(in->point[1]);
	out->position[2] = LittleFloat(in->point[2]);
    }
}

/*
=================
Mod_LoadSubmodels
=================
*/
static void
Mod_LoadSubmodels(lump_t *l)
{
    dmodel_t *in;
    dmodel_t *out;
    int i, j, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->submodels = out;
    loadmodel->numsubmodels = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {	// spread the mins / maxs by a pixel
	    out->mins[j] = LittleFloat(in->mins[j]) - 1;
	    out->maxs[j] = LittleFloat(in->maxs[j]) + 1;
	    out->origin[j] = LittleFloat(in->origin[j]);
	}
	for (j = 0; j < MAX_MAP_HULLS; j++)
	    out->headnode[j] = LittleLong(in->headnode[j]);
	out->visleafs = LittleLong(in->visleafs);
	out->firstface = LittleLong(in->firstface);
	out->numfaces = LittleLong(in->numfaces);
    }
}

/*
=================
Mod_LoadEdges
=================
*/
static void
Mod_LoadEdges(lump_t *l)
{
    dedge_t *in;
    medge_t *out;
    int i, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName((count + 1) * sizeof(*out), loadname);

    loadmodel->edges = out;
    loadmodel->numedges = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->v[0] = (uint16_t)LittleShort(in->v[0]);
	out->v[1] = (uint16_t)LittleShort(in->v[1]);
    }
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void
Mod_LoadTexinfo(lump_t *l)
{
    texinfo_t *in;
    mtexinfo_t *out;
    int i, j, count;
    int miptex;
    float len1, len2;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->texinfo = out;
    loadmodel->numtexinfo = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 4; j++) {
	    out->vecs[0][j] = LittleFloat(in->vecs[0][j]);
	    out->vecs[1][j] = LittleFloat(in->vecs[1][j]);
	}
	len1 = Length(out->vecs[0]);
	len2 = Length(out->vecs[1]);
	len1 = (len1 + len2) / 2;
	if (len1 < 0.32)
	    out->mipadjust = 4;
	else if (len1 < 0.49)
	    out->mipadjust = 3;
	else if (len1 < 0.99)
	    out->mipadjust = 2;
	else
	    out->mipadjust = 1;

	miptex = LittleLong(in->miptex);
	out->flags = LittleLong(in->flags);

	if (!loadmodel->textures) {
#ifndef SERVERONLY
	    out->texture = r_notexture_mip;	// checkerboard texture
#else
	    out->texture = &r_notexture_mip_qwsv;	// checkerboard texture
#endif
	    out->flags = 0;
	} else {
	    if (miptex >= loadmodel->numtextures)
		SV_Error("miptex >= loadmodel->numtextures");
	    out->texture = loadmodel->textures[miptex];
	    if (!out->texture) {
#ifndef SERVERONLY
		out->texture = r_notexture_mip;	// texture not found
#else
		out->texture = &r_notexture_mip_qwsv;	// texture not found
#endif
		out->flags = 0;
	    }
	}
    }
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void
CalcSurfaceExtents(msurface_t *s)
{
    float mins[2], maxs[2], val;
    int i, j, e;
    mvertex_t *v;
    mtexinfo_t *tex;
    int bmins[2], bmaxs[2];

    mins[0] = mins[1] = FLT_MAX;
    maxs[0] = maxs[1] = -FLT_MAX;

    tex = s->texinfo;

    for (i = 0; i < s->numedges; i++) {
	e = loadmodel->surfedges[s->firstedge + i];
	if (e >= 0)
	    v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
	else
	    v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

	for (j = 0; j < 2; j++) {
	    val = v->position[0] * tex->vecs[j][0] +
		v->position[1] * tex->vecs[j][1] +
		v->position[2] * tex->vecs[j][2] + tex->vecs[j][3];
	    if (val < mins[j])
		mins[j] = val;
	    if (val > maxs[j])
		maxs[j] = val;
	}
    }

    for (i = 0; i < 2; i++) {
	bmins[i] = floor(mins[i] / 16);
	bmaxs[i] = ceil(maxs[i] / 16);

	s->texturemins[i] = bmins[i] * 16;
	s->extents[i] = (bmaxs[i] - bmins[i]) * 16;
	if (!(tex->flags & TEX_SPECIAL) && s->extents[i] > 256)
	    SV_Error("Bad surface extents");
    }
}


/*
=================
Mod_LoadFaces
=================
*/
static void
Mod_LoadFaces(lump_t *l)
{
    dface_t *in;
    msurface_t *out;
    int i, count, surfnum;
    int planenum, side;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->surfaces = out;
    loadmodel->numsurfaces = count;

    for (surfnum = 0; surfnum < count; surfnum++, in++, out++) {
	out->firstedge = LittleLong(in->firstedge);
	out->numedges = LittleShort(in->numedges);
	out->flags = 0;

	planenum = LittleShort(in->planenum);
	side = LittleShort(in->side);
	if (side)
	    out->flags |= SURF_PLANEBACK;

	out->plane = loadmodel->planes + planenum;

	out->texinfo = loadmodel->texinfo + LittleShort(in->texinfo);

	CalcSurfaceExtents(out);

	// lighting info

	for (i = 0; i < MAXLIGHTMAPS; i++)
	    out->styles[i] = in->styles[i];
	i = LittleLong(in->lightofs);
	if (i == -1)
	    out->samples = NULL;
	else
	    out->samples = loadmodel->lightdata + i;

	/* set the surface drawing flags */
	if (!strncmp(out->texinfo->texture->name, "sky", 3)) {
	    out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
#ifdef GLQUAKE
	    GL_SubdivideSurface(loadmodel, out);
#endif
	} else if (!strncmp(out->texinfo->texture->name, "*", 1)) {
	    out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
	    for (i = 0; i < 2; i++) {
		out->extents[i] = 16384;
		out->texturemins[i] = -8192;
	    }
#ifdef GLQUAKE
	    GL_SubdivideSurface(loadmodel, out);
#endif
	}
    }
}


/*
=================
Mod_SetParent
=================
*/
static void
Mod_SetParent(mnode_t *node, mnode_t *parent)
{
    node->parent = parent;
    if (node->contents < 0)
	return;
    Mod_SetParent(node->children[0], node);
    Mod_SetParent(node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
static void
Mod_LoadNodes(lump_t *l)
{
    int i, j, count, p;
    dnode_t *in;
    mnode_t *out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->nodes = out;
    loadmodel->numnodes = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->minmaxs[j] = LittleShort(in->mins[j]);
	    out->minmaxs[3 + j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->planenum);
	out->plane = loadmodel->planes + p;

	out->firstsurface = (uint16_t)LittleShort(in->firstface);
	out->numsurfaces = (uint16_t)LittleShort(in->numfaces);

	for (j = 0; j < 2; j++) {
	    p = LittleShort(in->children[j]);
	    if (p >= 0)
		out->children[j] = loadmodel->nodes + p;
	    else
		out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
	}
    }

    Mod_SetParent(loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
static void
Mod_LoadLeafs(lump_t *l)
{
    dleaf_t *in;
    mleaf_t *out;
    int i, j, count, p;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);

    /* FIXME - fail gracefully */
    if (count > MAX_MAP_LEAFS)
	SV_Error("%s: model->numleafs > MAX_MAP_LEAFS\n", __func__);

    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->leafs = out;
    loadmodel->numleafs = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->minmaxs[j] = LittleShort(in->mins[j]);
	    out->minmaxs[3 + j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->contents);
	out->contents = p;

	out->firstmarksurface = loadmodel->marksurfaces +
	    (uint16_t)LittleShort(in->firstmarksurface);
	out->nummarksurfaces = (uint16_t)LittleShort(in->nummarksurfaces);

	p = LittleLong(in->visofs);
	if (p == -1)
	    out->compressed_vis = NULL;
	else
	    out->compressed_vis = loadmodel->visdata + p;
	out->efrags = NULL;

	for (j = 0; j < 4; j++)
	    out->ambient_sound_level[j] = in->ambient_level[j];

#ifdef GLQUAKE
	// FIXME - gl underwater warp
	// this warping is ugly, these ifdefs are ugly - get rid of it all?
	if (out->contents != CONTENTS_EMPTY) {
	    for (j = 0; j < out->nummarksurfaces; j++)
		out->firstmarksurface[j]->flags |= SURF_UNDERWATER;
	}

#ifdef QW_HACK
	{
	    char s[80];
	    snprintf(s, sizeof(s), "maps/%s.bsp",
		     Info_ValueForKey(cl.serverinfo, "map"));
	    s[sizeof(s) - 1] = 0;
	    if (strcmp(s, loadmodel->name)) {
#endif
		for (j = 0; j < out->nummarksurfaces; j++)
		    out->firstmarksurface[j]->flags |= SURF_DONTWARP;
#ifdef QW_HACK
	    }
	}
#endif
#endif
    }
}

/*
=================
Mod_LoadClipnodes
=================
*/
static void
Mod_LoadClipnodes(lump_t *l)
{
    dclipnode_t *in, *out;
    int i, count;
    hull_t *hull;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->clipnodes = out;
    loadmodel->numclipnodes = count;

    hull = &loadmodel->hulls[1];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = loadmodel->planes;
    hull->clip_mins[0] = -16;
    hull->clip_mins[1] = -16;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 16;
    hull->clip_maxs[1] = 16;
    hull->clip_maxs[2] = 32;

    hull = &loadmodel->hulls[2];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = loadmodel->planes;
    hull->clip_mins[0] = -32;
    hull->clip_mins[1] = -32;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 32;
    hull->clip_maxs[1] = 32;
    hull->clip_maxs[2] = 64;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = LittleLong(in->planenum);
	out->children[0] = LittleShort(in->children[0]);
	out->children[1] = LittleShort(in->children[1]);
    }
}

/*
=================
Mod_MakeHull0

Duplicate the drawing hull structure as a clipping hull
=================
*/
static void
Mod_MakeHull0(void)
{
    mnode_t *in, *child;
    dclipnode_t *out;
    int i, j, count;
    hull_t *hull;

    hull = &loadmodel->hulls[0];

    in = loadmodel->nodes;
    count = loadmodel->numnodes;
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = loadmodel->planes;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = in->plane - loadmodel->planes;
	for (j = 0; j < 2; j++) {
	    child = in->children[j];
	    if (child->contents < 0)
		out->children[j] = child->contents;
	    else
		out->children[j] = child - loadmodel->nodes;
	}
    }
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
static void
Mod_LoadMarksurfaces(lump_t *l)
{
    int i, j, count;
    unsigned short *in;
    msurface_t **out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->marksurfaces = out;
    loadmodel->nummarksurfaces = count;

    for (i = 0; i < count; i++) {
	j = (uint16_t)LittleShort(in[i]);
	if (j >= loadmodel->numsurfaces)
	    SV_Error("%s: bad surface number", __func__);
	out[i] = loadmodel->surfaces + j;
    }
}

/*
=================
Mod_LoadSurfedges
=================
*/
static void
Mod_LoadSurfedges(lump_t *l)
{
    int i, count;
    int *in, *out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->surfedges = out;
    loadmodel->numsurfedges = count;

    for (i = 0; i < count; i++)
	out[i] = LittleLong(in[i]);
}

/*
=================
Mod_LoadPlanes
=================
*/
static void
Mod_LoadPlanes(lump_t *l)
{
    int i, j;
    mplane_t *out;
    dplane_t *in;
    int count;
    int bits;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * 2 * sizeof(*out), loadname);

    loadmodel->planes = out;
    loadmodel->numplanes = count;

    for (i = 0; i < count; i++, in++, out++) {
	bits = 0;
	for (j = 0; j < 3; j++) {
	    out->normal[j] = LittleFloat(in->normal[j]);
	    if (out->normal[j] < 0)
		bits |= 1 << j;
	}

	out->dist = LittleFloat(in->dist);
	out->type = LittleLong(in->type);
	out->signbits = bits;
    }
}

/*
=================
RadiusFromBounds
=================
*/
static float
RadiusFromBounds(vec3_t mins, vec3_t maxs)
{
    int i;
    vec3_t corner;

    for (i = 0; i < 3; i++)
	corner[i] = qmax(fabs(mins[i]), fabs(maxs[i]));

    return Length(corner);
}

/*
=================
Mod_LoadBrushModel
=================
*/
static void
Mod_LoadBrushModel(model_t *mod, void *buffer, unsigned long size)
{
    int i, j;
    dheader_t *header;
    dmodel_t *bm;

    loadmodel->type = mod_brush;
    header = (dheader_t *)buffer;

    /* swap all the header entries */
    header->version = LittleLong(header->version);
    for (i = 0; i < HEADER_LUMPS; i++) {
	header->lumps[i].fileofs = LittleLong(header->lumps[i].fileofs);
	header->lumps[i].filelen = LittleLong(header->lumps[i].filelen);
    }

    if (header->version != BSPVERSION)
	SV_Error("%s: %s has wrong version number (%i should be %i)",
		 __func__, mod->name, header->version, BSPVERSION);

    mod_base = (byte *)header;

    /*
     * Check the lump extents
     * FIXME - do this more generally... cleanly...?
     */
    for (i = 0; i < HEADER_LUMPS; ++i) {
	int b1 = header->lumps[i].fileofs;
	int e1 = b1 + header->lumps[i].filelen;

	/*
	 * Sanity checks
	 * - begin and end >= 0 (end might overflow).
	 * - end > begin (again, overflow reqd.)
	 * - end < size of file.
	 */
	if (b1 > e1 || e1 > size || b1 < 0 || e1 < 0)
	    SV_Error("%s: bad lump extents in %s", __func__,
		     loadmodel->name);

	/* Now, check that it doesn't overlap any other lumps */
	for (j = 0; j < HEADER_LUMPS; ++j) {
	    int b2 = header->lumps[j].fileofs;
	    int e2 = b2 + header->lumps[j].filelen;

	    if ((b1 < b2 && e1 > b2) || (b2 < b1 && e2 > b1))
		SV_Error("%s: overlapping lumps in %s", __func__,
			 loadmodel->name);
	}
    }

#ifdef QW_HACK
    mod->checksum = 0;
    mod->checksum2 = 0;

// checksum all of the map, except for entities
    for (i = 0; i < HEADER_LUMPS; i++) {
	const lump_t *l = &header->lumps[i];
	unsigned int checksum;

	if (i == LUMP_ENTITIES)
	    continue;
	checksum = Com_BlockChecksum(mod_base + l->fileofs, l->filelen);
	mod->checksum ^= checksum;
	if (i == LUMP_VISIBILITY || i == LUMP_LEAFS || i == LUMP_NODES)
	    continue;
	mod->checksum2 ^= checksum;
    }
    mod->checksum = LittleLong(mod->checksum);
    mod->checksum2 = LittleLong(mod->checksum2);
#endif

    /* load into heap */
    Mod_LoadVertexes(&header->lumps[LUMP_VERTEXES]);
    Mod_LoadEdges(&header->lumps[LUMP_EDGES]);
    Mod_LoadSurfedges(&header->lumps[LUMP_SURFEDGES]);
    Mod_LoadTextures(&header->lumps[LUMP_TEXTURES]);
    Mod_LoadLighting(&header->lumps[LUMP_LIGHTING]);
    Mod_LoadPlanes(&header->lumps[LUMP_PLANES]);
    Mod_LoadTexinfo(&header->lumps[LUMP_TEXINFO]);
    Mod_LoadFaces(&header->lumps[LUMP_FACES]);
    Mod_LoadMarksurfaces(&header->lumps[LUMP_MARKSURFACES]);
    Mod_LoadVisibility(&header->lumps[LUMP_VISIBILITY]);
    Mod_LoadLeafs(&header->lumps[LUMP_LEAFS]);
    Mod_LoadNodes(&header->lumps[LUMP_NODES]);
    Mod_LoadClipnodes(&header->lumps[LUMP_CLIPNODES]);
    Mod_LoadEntities(&header->lumps[LUMP_ENTITIES]);
    Mod_LoadSubmodels(&header->lumps[LUMP_MODELS]);

    Mod_MakeHull0();

    mod->numframes = 2;		// regular and alternate animation
    mod->flags = 0;

//
// set up the submodels (FIXME: this is confusing)
//
    for (i = 0; i < mod->numsubmodels; i++) {
	bm = &mod->submodels[i];

	mod->hulls[0].firstclipnode = bm->headnode[0];
	for (j = 1; j < MAX_MAP_HULLS; j++) {
	    mod->hulls[j].firstclipnode = bm->headnode[j];
	    mod->hulls[j].lastclipnode = mod->numclipnodes - 1;
	}

	mod->firstmodelsurface = bm->firstface;
	mod->nummodelsurfaces = bm->numfaces;

	VectorCopy(bm->maxs, mod->maxs);
	VectorCopy(bm->mins, mod->mins);

	mod->radius = RadiusFromBounds(mod->mins, mod->maxs);
	mod->numleafs = bm->visleafs;

	/* duplicate the basic information */
	if (i < mod->numsubmodels - 1) {
	    char name[10];

	    snprintf(name, sizeof(name), "*%i", i + 1);
	    loadmodel = Mod_FindName(name);
	    *loadmodel = *mod;
	    strcpy(loadmodel->name, name);
	    mod = loadmodel;
	}
    }
}

/*
 * =========================================================================
 *                          CLIENT ONLY FUNCTIONS
 * =========================================================================
 */
#ifndef SERVERONLY

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *
Mod_Extradata(model_t *mod)
{
    void *r;

    r = Cache_Check(&mod->cache);
    if (r)
	return r;

    Mod_LoadModel(mod, true);

    if (!mod->cache.data)
	Sys_Error("%s: caching failed", __func__);
    return mod->cache.data;
}

/*
================
Mod_Print
================
*/
void
Mod_Print(void)
{
    int i;
    model_t *mod;

    Con_Printf("Cached models:\n");
    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	Con_Printf("%*p : %s\n", (int)sizeof(void *) * 2 + 2,
		   mod->cache.data, mod->name);
}

/*
==================
Mod_TouchModel

==================
*/
void
Mod_TouchModel(char *name)
{
    model_t *mod;

    mod = Mod_FindName(name);

    if (!mod->needload) {
	if (mod->type == mod_alias)
	    Cache_Check(&mod->cache);
    }
}

#endif /* !SERVERONLY */
