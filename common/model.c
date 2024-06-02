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

#include "cmd.h"
#include "common.h"
#include "console.h"
#include "model.h"

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

static void Mod_LoadBrushModel(model_t *mod, void *buffer, unsigned long size);
static model_t *Mod_LoadModel(model_t *mod, qboolean crash);

#define MAX_MOD_KNOWN 512
static model_t mod_known[MAX_MOD_KNOWN];
static int mod_numknown;

static const model_loader_t *mod_loader;

static void PVSCache_f(void);

// leilei HACK

int coloredlights = 0; // to debug the colored lights as we have no menu option yet. 


/*
===============
Mod_Init
===============
*/
void
Mod_Init(const model_loader_t *loader)
{
    Cmd_AddCommand("pvscache", PVSCache_f);
    mod_loader = loader;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t * Mod_PointInLeaf(const model_t *model, const vec3_t point)
{
   mnode_t *node;

   if (!model || !model->nodes)
      SV_Error("%s: bad model", __func__);

   node = model->nodes;

   while (1)
   {
      float dist;
      mplane_t *plane;
      if (node->contents < 0)
         return (mleaf_t *)node;
      plane = node->plane;
      dist = DotProduct(point, plane->normal) - plane->dist;
      if (dist > 0)
         node = node->children[0];
      else
         node = node->children[1];
   }

   return NULL;		// never reached
}

void
Mod_AddLeafBits(leafbits_t *dst, const leafbits_t *src)
{
    int i, leafblocks;
    const leafblock_t *srcblock;
    leafblock_t *dstblock;

    if (src->numleafs != dst->numleafs)
	SV_Error("%s: src->numleafs (%d) != dst->numleafs (%d)",
		 __func__, src->numleafs, dst->numleafs);

    srcblock = src->bits;
    dstblock = dst->bits;
    leafblocks = (src->numleafs + LEAFMASK) >> LEAFSHIFT;
    for (i = 0; i < leafblocks; i++)
	*dstblock++ |= *srcblock++;
}

#ifdef SERVERONLY
int
Mod_CountLeafBits(const leafbits_t *leafbits)
{
    int i, leafblocks, count;
    leafblock_t block;

    count = 0;
    leafblocks = (leafbits->numleafs + LEAFMASK) >> LEAFSHIFT;
    for (i = 0; i < leafblocks; i++) {
	block = leafbits->bits[i];
	while (block) {
	    count++;
	    block &= (block - 1); /* remove least significant bit */
	}
    }

    return count;
};
#endif

/*
 * Simple LRU cache for decompressed vis data
 */
typedef struct {
    const model_t *model;
    const mleaf_t *leaf;
    leafbits_t *leafbits;
} pvscache_t;
static pvscache_t pvscache[2];
static leafbits_t *fatpvs;
static int pvscache_numleafs;
static int pvscache_bytes;
static int pvscache_blocks;

static int c_cachehit, c_cachemiss;

#define PVSCACHE_SIZE ARRAY_SIZE(pvscache)

static void
Mod_InitPVSCache(int numleafs)
{
    int i;
    int memsize;
    byte *leafmem;

    pvscache_numleafs = numleafs;
    pvscache_bytes = ((numleafs + LEAFMASK) & ~LEAFMASK) >> 3;
    pvscache_blocks = pvscache_bytes / sizeof(leafblock_t);
    memsize = Mod_LeafbitsSize(numleafs);
    fatpvs = (leafbits_t*)Hunk_Alloc(memsize);

    memset(pvscache, 0, sizeof(pvscache));
    leafmem = (byte*)Hunk_Alloc(PVSCACHE_SIZE * memsize);
    for (i = 0; i < PVSCACHE_SIZE; i++)
	pvscache[i].leafbits = (leafbits_t *)(leafmem + i * memsize);
}

/*
===================
Mod_DecompressVis
===================
*/

static void
Mod_DecompressVis(const byte *in, const model_t *model, leafbits_t *dest)
{
    leafblock_t *out;
    int num_out;
    int shift;
    int count;

    dest->numleafs = model->numleafs;
    out = dest->bits;

    if (!in) {
	/* no vis info, so make all visible */
	memset(out, 0xff, pvscache_bytes);
	return;
    }

    memset(out, 0, pvscache_bytes);
    num_out = 0;
    shift = 0;
    do {
	if (*in) {
	    *out |= (leafblock_t)*in++ << shift;
	    shift += 8;
	    num_out += 8;
	    if (shift == (1 << LEAFSHIFT)) {
		shift = 0;
		out++;
	    }
	    continue;
	}

	/* Run of zeros - skip over */
	count = in[1];
	in += 2;
	out += count / sizeof(leafblock_t);
	shift += (count % sizeof(leafblock_t)) << 3;
	num_out += count << 3;
	if (shift >= (1 << LEAFSHIFT)) {
	    shift -= (1 << LEAFSHIFT);
	    out++;
	}
    } while (num_out < dest->numleafs);
}

const leafbits_t *
Mod_LeafPVS(const model_t *model, const mleaf_t *leaf)
{
    int slot;
    pvscache_t tmp;

    for (slot = 0; slot < PVSCACHE_SIZE; slot++)
	if (pvscache[slot].model == model && pvscache[slot].leaf == leaf) {
	    c_cachehit++;
	    break;
	}

    if (slot) {
	if (slot == PVSCACHE_SIZE) {
	    slot--;
	    tmp.model = model;
	    tmp.leaf = leaf;
	    tmp.leafbits = pvscache[slot].leafbits;
	    if (leaf == model->leafs) {
		/* return set with everything visible */
		tmp.leafbits->numleafs = model->numleafs;
		memset(tmp.leafbits->bits, 0xff, pvscache_bytes);
	    } else {
		Mod_DecompressVis(leaf->compressed_vis, model, tmp.leafbits);
	    }
	    c_cachemiss++;
	} else {
	    tmp = pvscache[slot];
	}
	memmove(pvscache + 1, pvscache, slot * sizeof(pvscache_t));
	pvscache[0] = tmp;
    }

    return pvscache[0].leafbits;
}

static void
PVSCache_f(void)
{
    Con_Printf("PVSCache: %7d hits %7d misses\n", c_cachehit, c_cachemiss);
}

static void Mod_AddToFatPVS(const model_t *model, const vec3_t point, const mnode_t *node)
{
   while (1)
   {
      float d;
      mplane_t *plane;

      /* if this is a leaf, accumulate the pvs bits */
      if (node->contents < 0)
      {
         if (node->contents != CONTENTS_SOLID)
         {
            const leafbits_t *pvs = Mod_LeafPVS(model, (const mleaf_t *)node);
            Mod_AddLeafBits(fatpvs, pvs);
         }
         return;
      }

      plane = node->plane;
      d = DotProduct(point, plane->normal) - plane->dist;

      if (d > 8)
         node = node->children[0];
      else if (d < -8)
         node = node->children[1];
      else
      {			// go down both
         Mod_AddToFatPVS(model, point, node->children[0]);
         node = node->children[1];
      }
   }
}

/*
=============
Mod_FatPVS

Calculates a PVS that is the inclusive or of all leafs within 8 pixels of the
given point.

The FatPVS must include a small area around the client to allow head bobbing
or other small motion on the client side.  Otherwise, a bob might cause an
entity that should be visible to not show up, especially when the bob
crosses a waterline.
=============
*/
const leafbits_t *
Mod_FatPVS(const model_t *model, const vec3_t point)
{
    fatpvs->numleafs = model->numleafs;
    memset(fatpvs->bits, 0, pvscache_bytes);
    Mod_AddToFatPVS(model, point, model->nodes);

    return fatpvs;
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

    fatpvs = NULL;
    memset(pvscache, 0, sizeof(pvscache));
    pvscache_numleafs = 0;
    pvscache_bytes = pvscache_blocks = 0;
    c_cachehit = c_cachemiss = 0;
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

    // load the file
    buf = (unsigned int*)COM_LoadStackFile(mod->name, stackbuf, sizeof(stackbuf), &size);
    if (!buf) {
	if (crash)
	    SV_Error("%s: %s not found", __func__, mod->name);
	return NULL;
    }

    // allocate a new model
    loadmodel = mod;

//
// fill it in
//

// call the apropriate loader
    mod->needload = false;

    switch (LittleLong(*(unsigned *)buf))
    {
#ifndef SERVERONLY
       case IDPOLYHEADER:
          Mod_LoadAliasModel(mod_loader, mod, buf, loadmodel);
          break;

       case IDSPRITEHEADER:
          Mod_LoadSpriteModel(mod, buf);
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
   int i, j, pixels, num, max, altmax = 0;
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

#ifdef MSB_FIRST
   m->nummiptex = LittleLong(m->nummiptex);
#endif

   loadmodel->numtextures = m->nummiptex;
   loadmodel->textures = (texture_t**)Hunk_Alloc(m->nummiptex * sizeof(*loadmodel->textures));

   for (i = 0; i < m->nummiptex; i++)
   {
#ifdef MSB_FIRST
      m->dataofs[i] = LittleLong(m->dataofs[i]);
#endif
      if (m->dataofs[i] == -1)
         continue;
      mt = (miptex_t *)((byte *)m + m->dataofs[i]);
#ifdef MSB_FIRST
      mt->width = (uint32_t)LittleLong(mt->width);
      mt->height = (uint32_t)LittleLong(mt->height);
      for (j = 0; j < MIPLEVELS; j++)
         mt->offsets[j] = (uint32_t)LittleLong(mt->offsets[j]);
#endif

      if ((mt->width & 15) || (mt->height & 15))
         SV_Error("Texture %s is not 16 aligned", mt->name);
      pixels = mt->width * mt->height / 64 * 85;
      tx = (texture_t*)Hunk_Alloc(sizeof(texture_t) + pixels);
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
	int		i;
	byte	*data;
	char	litname[1024];
	byte 	*lightmapfile;

	if (!l->filelen) {
		loadmodel->lightdata = NULL;
		return;
	}

	if (coloredlights)	// if colored lights are enabled, look for a lit file to load
	{
		strcpy(litname, loadmodel->name);
		COM_StripExtension(litname);
		COM_DefaultExtension(litname, ".lit");
		lightmapfile = COM_LoadHunkFile(litname);
		if (lightmapfile)
		{
			data = lightmapfile;	
			if (data[0] == 'Q' && data[1] == 'L' && data[2] == 'I' && data[3] == 'T')
			{
				i = LittleLong(((int *)data)[1]);
				if (i == 1)
				{
					loadmodel->lightdata = data + 8;	
					return;
				}
				else
					Con_Printf("Unknown .LIT file version (%d)\n", i);
			}
			else
				Con_Printf("Corrupt .LIT file (old version?), ignoring\n");

		}
		else
		{
		//expand the mono lighting to 24 bit
			int i;
			byte *dest, *src = mod_base + l->fileofs;
			loadmodel->lightdata = Hunk_Alloc( l->filelen*3);
			dest = loadmodel->lightdata;
			for (i = 0; i<l->filelen; i++)
			{
				dest[0] = *src;
				dest[1] = *src;
				dest[2] = *src;

				src++;
				dest+=3;
		
			}
				
	
		}
	}
	else		// mono lights
	{
	    loadmodel->lightdata = (byte*)Hunk_Alloc(l->filelen);
	    memcpy(loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
	}
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
    loadmodel->visdata = (byte*)Hunk_Alloc(l->filelen);
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
    loadmodel->entities = (char*)Hunk_Alloc(l->filelen);
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

   in = (dvertex_t*)(void *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mvertex_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->vertexes = out;
   loadmodel->numvertexes = count;

   for (i = 0; i < count; i++, in++, out++)
   {
#ifdef MSB_FIRST
      out->position[0] = LittleFloat(in->point[0]);
      out->position[1] = LittleFloat(in->point[1]);
      out->position[2] = LittleFloat(in->point[2]);
#else
      out->position[0] = (in->point[0]);
      out->position[1] = (in->point[1]);
      out->position[2] = (in->point[2]);
#endif
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

   in = (dmodel_t*)(void *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (dmodel_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->submodels = out;
   loadmodel->numsubmodels = count;

   for (i = 0; i < count; i++, in++, out++)
   {
      for (j = 0; j < 3; j++)
      {	// spread the mins / maxs by a pixel
#ifdef MSB_FIRST
         out->mins[j]   = LittleFloat(in->mins[j]) - 1;
         out->maxs[j]   = LittleFloat(in->maxs[j]) + 1;
         out->origin[j] = LittleFloat(in->origin[j]);
#else
         out->mins[j]   = (in->mins[j]) - 1;
         out->maxs[j]   = (in->maxs[j]) + 1;
         out->origin[j] = (in->origin[j]);
#endif
      }
      for (j = 0; j < MAX_MAP_HULLS; j++)
      {
#ifdef MSB_FIRST
         out->headnode[j] = LittleLong(in->headnode[j]);
#else
         out->headnode[j] = (in->headnode[j]);
#endif
      }
#ifdef MSB_FIRST
      out->visleafs  = LittleLong(in->visleafs);
      out->firstface = LittleLong(in->firstface);
      out->numfaces  = LittleLong(in->numfaces);
#else
      out->visleafs  =  (in->visleafs);
      out->firstface = (in->firstface);
      out->numfaces  = (in->numfaces);
#endif
   }
}

/*
=================
Mod_LoadEdges
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadEdges_BSP29(lump_t *l)
{
   bsp29_dedge_t *in;
   medge_t *out;
   int i, count;

   in = (bsp29_dedge_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (medge_t*)Hunk_Alloc((count + 1) * sizeof(*out));

   loadmodel->edges = out;
   loadmodel->numedges = count;

   for (i = 0; i < count; i++, in++, out++)
   {
#ifdef MSB_FIRST
      out->v[0] = (uint16_t)LittleShort(in->v[0]);
      out->v[1] = (uint16_t)LittleShort(in->v[1]);
#else
      out->v[0] = (uint16_t)(in->v[0]);
      out->v[1] = (uint16_t)(in->v[1]);
#endif
   }
}

static void
Mod_LoadEdges_BSP2(lump_t *l)
{
   bsp2_dedge_t *in;
   medge_t *out;
   int i, count;

   in = (bsp2_dedge_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (medge_t*)Hunk_Alloc((count + 1) * sizeof(*out));

   loadmodel->edges = out;
   loadmodel->numedges = count;

   for (i = 0; i < count; i++, in++, out++) {
#ifdef MSB_FIRST
      out->v[0] = (uint32_t)LittleLong(in->v[0]);
      out->v[1] = (uint32_t)LittleLong(in->v[1]);
#else
      out->v[0] = (uint32_t)(in->v[0]);
      out->v[1] = (uint32_t)(in->v[1]);
#endif
   }
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void Mod_LoadTexinfo(lump_t *l)
{
   texinfo_t *in;
   mtexinfo_t *out;
   int i, j, count;
   int miptex;
   float len1, len2;

   in = (texinfo_t*)(void *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mtexinfo_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->texinfo = out;
   loadmodel->numtexinfo = count;

   for (i = 0; i < count; i++, in++, out++)
   {
      for (j = 0; j < 4; j++)
      {
#ifdef MSB_FIRST
         out->vecs[0][j] = LittleFloat(in->vecs[0][j]);
         out->vecs[1][j] = LittleFloat(in->vecs[1][j]);
#else
         out->vecs[0][j] = (in->vecs[0][j]);
         out->vecs[1][j] = (in->vecs[1][j]);
#endif
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

#ifdef MSB_FIRST
      miptex     = LittleLong(in->miptex);
      out->flags = LittleLong(in->flags);
#else
      miptex     = (in->miptex);
      out->flags = (in->flags);
#endif

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

/*
	 * The (long double) casts below are important: The original code was
	 * written for x87 floating-point which uses 80-bit floats for
	 * intermediate calculations. But if you compile it without the casts
	 * for modern x86_64, the compiler will round each intermediate result
	 * to a 32-bit float, which introduces extra rounding error.
	 *
	 * This becomes a problem if the rounding error causes the light
	 * utilities and the engine to disagree about the lightmap size for
	 * some surfaces.
	 *
	 * Casting to (long double) keeps the intermediate values at at least
	 * 64 bits of precision, probably 128.
	 */

	for (j = 0; j < 2; j++) {
       val =
		(long double)v->position[0] * tex->vecs[j][0] +
		(long double)v->position[1] * tex->vecs[j][1] +
		(long double)v->position[2] * tex->vecs[j][2] +
		                                   tex->vecs[j][3];
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

static void
CalcSurfaceBounds(msurface_t *surf)
{
    int i, j, edgenum;
    medge_t *edge;
    mvertex_t *v;

    surf->mins[0] = surf->mins[1] = surf->mins[2] = FLT_MAX;
    surf->maxs[0] = surf->maxs[1] = surf->maxs[2] = -FLT_MAX;

    for (i = 0; i < surf->numedges; i++) {
	edgenum = loadmodel->surfedges[surf->firstedge + i];
	if (edgenum >= 0) {
	    edge = &loadmodel->edges[edgenum];
	    v = &loadmodel->vertexes[edge->v[0]];
	} else {
	    edge = &loadmodel->edges[-edgenum];
	    v = &loadmodel->vertexes[edge->v[1]];
	}

	for (j = 0; j < 3; j++) {
	    if (surf->mins[j] > v->position[j])
		surf->mins[j] = v->position[j];
	    if (surf->maxs[j] < v->position[j])
		surf->maxs[j] = v->position[j];
	}
    }
}

/*
=================
Mod_LoadFaces
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadFaces_BSP29(lump_t *l)
{
   bsp29_dface_t *in;
   msurface_t *out;
   int i, count, surfnum;
   int planenum, side;

   in = (bsp29_dface_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (msurface_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->surfaces = out;
   loadmodel->numsurfaces = count;

   for (surfnum = 0; surfnum < count; surfnum++, in++, out++)
   {
#ifdef MSB_FIRST
      out->firstedge = LittleLong(in->firstedge);
      out->numedges  = LittleShort(in->numedges);
#else
      out->firstedge = (in->firstedge);
      out->numedges  = (in->numedges);
#endif
      out->flags = 0;

      /* FIXME - Also check numedges doesn't overflow edges */
      if (out->numedges <= 0)
         SV_Error("%s: bmodel %s has surface with no edges", __func__,
               loadmodel->name);

#ifdef MSB_FIRST
      planenum = LittleShort(in->planenum);
      side     = LittleShort(in->side);
#else
      planenum = (in->planenum);
      side     = (in->side);
#endif
      if (side)
         out->flags |= SURF_PLANEBACK;

      out->plane = loadmodel->planes + planenum;
#ifdef MSB_FIRST
      out->texinfo = loadmodel->texinfo + LittleShort(in->texinfo);
#else
      out->texinfo = &loadmodel->texinfo[in->texinfo];
#endif

      CalcSurfaceExtents(out);
      CalcSurfaceBounds(out);

      // lighting info

      for (i = 0; i < MAXLIGHTMAPS; i++)
         out->styles[i] = in->styles[i];
#ifdef MSB_FIRST
      i = LittleLong(in->lightofs);
#else
      i = (in->lightofs);
#endif
      if (coloredlights)
      {
         if (i == -1)
            out->samples = NULL;
         out->samples = loadmodel->lightdata + i * 3;
      }
      else
      {
         if (i == -1)
            out->samples = NULL;
         else
            out->samples = loadmodel->lightdata + i;
      }

      /* set the surface drawing flags */
      if (!strncmp(out->texinfo->texture->name, "sky", 3)) {
         out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
      } else if (!strncmp(out->texinfo->texture->name, "*", 1)) {
         out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
         for (i = 0; i < 2; i++) {
            out->extents[i] = 16384;
            out->texturemins[i] = -8192;
         }
      }
   }
}

static void Mod_LoadFaces_BSP2(lump_t *l)
{
   msurface_t *out;
   int i, count, surfnum;
   int planenum, side;
   bsp2_dface_t *in = (bsp2_dface_t *)(mod_base + l->fileofs);

   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);

   count = l->filelen / sizeof(*in);
   out = (msurface_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->surfaces = out;
   loadmodel->numsurfaces = count;

   for (surfnum = 0; surfnum < count; surfnum++, in++, out++)
   {
#ifdef MSB_FIRST
      out->firstedge = LittleLong(in->firstedge);
      out->numedges  = LittleLong(in->numedges);
#else
      out->firstedge = (in->firstedge);
      out->numedges  = (in->numedges);
#endif
      out->flags     = 0;

#ifdef MSB_FIRST
      planenum       = LittleLong(in->planenum);
      side           = LittleLong(in->side);
#else
      planenum       = (in->planenum);
      side           = (in->side);
#endif
      if (side)
         out->flags |= SURF_PLANEBACK;

      out->plane = loadmodel->planes + planenum;
#ifdef MSB_FIRST
      out->texinfo = loadmodel->texinfo + LittleLong(in->texinfo);
#else
      out->texinfo = &loadmodel->texinfo[in->texinfo];
#endif

      CalcSurfaceExtents(out);
      CalcSurfaceBounds(out);

      // lighting info

      for (i = 0; i < MAXLIGHTMAPS; i++)
         out->styles[i] = in->styles[i];
#ifdef MSB_FIRST
      i = LittleLong(in->lightofs);
#else
      i = (in->lightofs);
#endif
      if (i == -1)
         out->samples = NULL;
      else
         out->samples = loadmodel->lightdata + i;

      /* set the surface drawing flags */
      if (!strncmp(out->texinfo->texture->name, "sky", 3))
         out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
      else if (!strncmp(out->texinfo->texture->name, "*", 1))
      {
         out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
         for (i = 0; i < 2; i++)
         {
            out->extents[i] = 16384;
            out->texturemins[i] = -8192;
         }
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
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadNodes_BSP29(lump_t *l)
{
   int i, j, count, p;
   bsp29_dnode_t *in;
   mnode_t *out;

   in = (bsp29_dnode_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mnode_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->nodes = out;
   loadmodel->numnodes = count;

   for (i = 0; i < count; i++, in++, out++) {
      for (j = 0; j < 3; j++) {
#ifdef MSB_FIRST
         out->mins[j] = LittleShort(in->mins[j]);
         out->maxs[j] = LittleShort(in->maxs[j]);
#else
         out->mins[j] = (in->mins[j]);
         out->maxs[j] = (in->maxs[j]);
#endif
      }

#ifdef MSB_FIRST
      p = LittleLong(in->planenum);
#else
      p = (in->planenum);
#endif
      out->plane = loadmodel->planes + p;

#ifdef MSB_FIRST
      out->firstsurface = (uint16_t)LittleShort(in->firstface);
      out->numsurfaces = (uint16_t)LittleShort(in->numfaces);
#else
      out->firstsurface = (uint16_t)(in->firstface);
      out->numsurfaces = (uint16_t)(in->numfaces);
#endif

      for (j = 0; j < 2; j++)
      {
#ifdef MSB_FIRST
         p = LittleShort(in->children[j]);
#else
         p = (in->children[j]);
#endif
         if (p >= 0)
            out->children[j] = loadmodel->nodes + p;
         else
            out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
      }
   }

   Mod_SetParent(loadmodel->nodes, NULL);	// sets nodes and leafs
}

static void Mod_LoadNodes_BSP2(lump_t *l)
{
   int i, count;
   mnode_t *out;
   bsp2_dnode_t *in = (bsp2_dnode_t *)(mod_base + l->fileofs);

   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);

   count = l->filelen / sizeof(*in);
   out   = (mnode_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->nodes    = out;
   loadmodel->numnodes = count;

   for (i = 0; i < count; i++, in++, out++)
   {
      int j, p;

      for (j = 0; j < 3; j++)
      {
#ifdef MSB_FIRST
         out->mins[j] = LittleShort(in->mins[j]);
         out->maxs[j] = LittleShort(in->maxs[j]);
#else
         out->mins[j] = (in->mins[j]);
         out->maxs[j] = (in->maxs[j]);
#endif
      }

#ifdef MSB_FIRST
      p = LittleLong(in->planenum);
#else
      p = (in->planenum);
#endif
      out->plane = loadmodel->planes + p;

#ifdef MSB_FIRST
      out->firstsurface = (uint32_t)LittleLong(in->firstface);
      out->numsurfaces = (uint32_t)LittleLong(in->numfaces);
#else
      out->firstsurface = (uint32_t)(in->firstface);
      out->numsurfaces = (uint32_t)(in->numfaces);
#endif

      for (j = 0; j < 2; j++)
      {
#ifdef MSB_FIRST
         p = LittleLong(in->children[j]);
#else
         p = (in->children[j]);
#endif
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
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadLeafs_BSP29(lump_t *l)
{
   bsp29_dleaf_t *in;
   mleaf_t *out;
   int i, j, count, p;

   in = (bsp29_dleaf_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mleaf_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->leafs = out;
   loadmodel->numleafs = count;

   for (i = 0; i < count; i++, in++, out++)
   {
      for (j = 0; j < 3; j++)
      {
#ifdef MSB_FIRST
         out->mins[j] = LittleShort(in->mins[j]);
         out->maxs[j] = LittleShort(in->maxs[j]);
#else
         out->mins[j] = (in->mins[j]);
         out->maxs[j] = (in->maxs[j]);
#endif
      }

#ifdef MSB_FIRST
      p = LittleLong(in->contents);
#else
      p = (in->contents);
#endif
      out->contents = p;

#ifdef MSB_FIRST
      out->firstmarksurface = loadmodel->marksurfaces +
         (uint16_t)LittleShort(in->firstmarksurface);
      out->nummarksurfaces = (uint16_t)LittleShort(in->nummarksurfaces);

      p = LittleLong(in->visofs);
#else
      out->firstmarksurface = &loadmodel->marksurfaces[in->firstmarksurface];
      out->nummarksurfaces = (uint16_t)(in->nummarksurfaces);

      p = (in->visofs);
#endif
      if (p == -1)
         out->compressed_vis = NULL;
      else
         out->compressed_vis = loadmodel->visdata + p;
      out->efrags = NULL;

      for (j = 0; j < 4; j++)
         out->ambient_sound_level[j] = in->ambient_level[j];
   }
}

static void
Mod_LoadLeafs_BSP2(lump_t *l)
{
   bsp2_dleaf_t *in;
   mleaf_t *out;
   int i, j, count, p;

   in = (bsp2_dleaf_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mleaf_t*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->leafs = out;
   loadmodel->numleafs = count;

   for (i = 0; i < count; i++, in++, out++) {
      for (j = 0; j < 3; j++) {
#ifdef MSB_FIRST
         out->mins[j] = LittleShort(in->mins[j]);
         out->maxs[j] = LittleShort(in->maxs[j]);
#else
         out->mins[j] = (in->mins[j]);
         out->maxs[j] = (in->maxs[j]);
#endif
      }

#ifdef MSB_FIRST
      p = LittleLong(in->contents);
#else
      p = (in->contents);
#endif
      out->contents = p;

#ifdef MSB_FIRST
      out->firstmarksurface = loadmodel->marksurfaces +
         (uint32_t)LittleLong(in->firstmarksurface);
      out->nummarksurfaces = (uint32_t)LittleLong(in->nummarksurfaces);

      p = LittleLong(in->visofs);
#else
      out->firstmarksurface = &loadmodel->marksurfaces[in->firstmarksurface];
      out->nummarksurfaces = (uint32_t)(in->nummarksurfaces);

      p = (in->visofs);
#endif

      if (p == -1)
         out->compressed_vis = NULL;
      else
         out->compressed_vis = loadmodel->visdata + p;
      out->efrags = NULL;

      for (j = 0; j < 4; j++)
         out->ambient_sound_level[j] = in->ambient_level[j];
   }
}

/*
=================
Mod_LoadClipnodes
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadClipnodes_BSP29(lump_t *l)
{
   bsp29_dclipnode_t *in;
   mclipnode_t *out;
   int i, j, count;
   hull_t *hull;

   in = (bsp29_dclipnode_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mclipnode_t*)Hunk_Alloc(count * sizeof(*out));

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

   for (i = 0; i < count; i++, out++, in++)
   {
#ifdef MSB_FIRST
      out->planenum = LittleLong(in->planenum);
#else
      out->planenum = (in->planenum);
#endif
      for (j = 0; j < 2; j++) {
#ifdef MSB_FIRST
         out->children[j] = (uint16_t)LittleShort(in->children[j]);
#else
         out->children[j] = (uint16_t)(in->children[j]);
#endif
         if (out->children[j] > 0xfff0)
            out->children[j] -= 0x10000;
         if (out->children[j] >= count)
            SV_Error("%s: bad clipnode child number", __func__);
      }
   }
}

static void
Mod_LoadClipnodes_BSP2(lump_t *l)
{
   bsp2_dclipnode_t *in;
   mclipnode_t *out;
   int i, j, count;
   hull_t *hull;

   in = (bsp2_dclipnode_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (mclipnode_t*)Hunk_Alloc(count * sizeof(*out));

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
#ifdef MSB_FIRST
      out->planenum = LittleLong(in->planenum);
#else
      out->planenum = (in->planenum);
#endif
      for (j = 0; j < 2; j++) {
#ifdef MSB_FIRST
         out->children[j] = LittleLong(in->children[j]);
#else
         out->children[j] = (in->children[j]);
#endif
         if (out->children[j] >= count)
            SV_Error("%s: bad clipnode child number", __func__);
      }
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
    mclipnode_t *out;
    int i, j, count;
    hull_t *hull;

    hull = &loadmodel->hulls[0];

    in = loadmodel->nodes;
    count = loadmodel->numnodes;
    out = (mclipnode_t*)Hunk_Alloc(count * sizeof(*out));

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
 => Two versions for the different BSP file formats
=================
*/
static void
Mod_LoadMarksurfaces_BSP29(lump_t *l)
{
   int i, j, count;
   uint16_t *in;
   msurface_t **out;

   in = (uint16_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (msurface_t**)Hunk_Alloc(count * sizeof(*out));

   loadmodel->marksurfaces = out;
   loadmodel->nummarksurfaces = count;

   for (i = 0; i < count; i++)
   {
#ifdef MSB_FIRST
      j = (uint16_t)LittleShort(in[i]);
#else
      j = (uint16_t)(in[i]);
#endif
      if (j >= loadmodel->numsurfaces)
         SV_Error("%s: bad surface number", __func__);
      out[i] = loadmodel->surfaces + j;
   }
}

static void
Mod_LoadMarksurfaces_BSP2(lump_t *l)
{
   int i, j, count;
   uint32_t *in;
   msurface_t **out;

   in = (uint32_t *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (msurface_t**)Hunk_Alloc(count * sizeof(*out));

   loadmodel->marksurfaces = out;
   loadmodel->nummarksurfaces = count;

   for (i = 0; i < count; i++) {
#ifdef MSB_FIRST
      j = (uint32_t)LittleLong(in[i]);
#else
      j = (uint32_t)(in[i]);
#endif
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

   in = (int*)(void *)(mod_base + l->fileofs);
   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);
   count = l->filelen / sizeof(*in);
   out = (int*)Hunk_Alloc(count * sizeof(*out));

   loadmodel->surfedges = out;
   loadmodel->numsurfedges = count;

   for (i = 0; i < count; i++)
#ifdef MSB_FIRST
      out[i] = LittleLong(in[i]);
#else
      out[i] = (in[i]);
#endif
}

/*
=================
Mod_LoadPlanes
=================
*/
static void Mod_LoadPlanes(lump_t *l)
{
   int i, j, count;
   mplane_t *out;
   dplane_t *in = (dplane_t*)(void *)(mod_base + l->fileofs);

   if (l->filelen % sizeof(*in))
      SV_Error("%s: funny lump size in %s", __func__, loadmodel->name);

   count = l->filelen / sizeof(*in);
   out   = (mplane_t*)
      Hunk_Alloc(count * 2 * sizeof(*out));

   loadmodel->planes    = out;
   loadmodel->numplanes = count;

   for (i = 0; i < count; i++, in++, out++)
   {
      int bits = 0;
      for (j = 0; j < 3; j++)
      {
#ifdef MSB_FIRST
         out->normal[j] = LittleFloat(in->normal[j]);
#else
         out->normal[j] = (in->normal[j]);
#endif
         if (out->normal[j] < 0)
            bits |= 1 << j;
      }

#ifdef MSB_FIRST
      out->dist = LittleFloat(in->dist);
      out->type = LittleLong(in->type);
#else
      out->dist = (in->dist);
      out->type = (in->type);
#endif
      out->signbits = bits;
   }
}

/*
=================
RadiusFromBounds
=================
*/
static float RadiusFromBounds(vec3_t mins, vec3_t maxs)
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
static void Mod_LoadBrushModel(model_t *mod, void *buffer, unsigned long size)
{
   int i, j;
   dheader_t *header;
   dmodel_t *bm;

   loadmodel->type = mod_brush;
   header = (dheader_t *)buffer;

#ifdef MSB_FIRST
   /* swap all the header entries */
   header->version = LittleLong(header->version);
   for (i = 0; i < HEADER_LUMPS; i++) {
      header->lumps[i].fileofs = LittleLong(header->lumps[i].fileofs);
      header->lumps[i].filelen = LittleLong(header->lumps[i].filelen);
   }
#endif

   if (header->version != BSPVERSION && header->version != BSP2VERSION)
      SV_Error("%s: %s has wrong version number (%i should be %i or %i)",
            __func__, mod->name, header->version, BSPVERSION, BSP2VERSION);

   mod_base = (byte *)header;

   /*
    * Check the lump extents
    * FIXME - do this more generally... cleanly...?
    */
   for (i = 0; i < HEADER_LUMPS; ++i)
   {
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
      for (j = 0; j < HEADER_LUMPS; ++j)
      {
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
#ifdef MSB_FIRST
   mod->checksum  = LittleLong(mod->checksum);
   mod->checksum2 = LittleLong(mod->checksum2);
#endif
#endif

   /* load into heap */
   Mod_LoadVertexes(&header->lumps[LUMP_VERTEXES]);
   if (header->version == BSPVERSION)
      Mod_LoadEdges_BSP29(&header->lumps[LUMP_EDGES]);
   else
      Mod_LoadEdges_BSP2(&header->lumps[LUMP_EDGES]);
   Mod_LoadSurfedges(&header->lumps[LUMP_SURFEDGES]);
   Mod_LoadTextures(&header->lumps[LUMP_TEXTURES]);
   Mod_LoadLighting(&header->lumps[LUMP_LIGHTING]);
   Mod_LoadPlanes(&header->lumps[LUMP_PLANES]);
   Mod_LoadTexinfo(&header->lumps[LUMP_TEXINFO]);
   if (header->version == BSPVERSION) {
      Mod_LoadFaces_BSP29(&header->lumps[LUMP_FACES]);
      Mod_LoadMarksurfaces_BSP29(&header->lumps[LUMP_MARKSURFACES]);
   } else {
      Mod_LoadFaces_BSP2(&header->lumps[LUMP_FACES]);
      Mod_LoadMarksurfaces_BSP2(&header->lumps[LUMP_MARKSURFACES]);
   }
   Mod_LoadVisibility(&header->lumps[LUMP_VISIBILITY]);
   if (header->version == BSPVERSION) {
      Mod_LoadLeafs_BSP29(&header->lumps[LUMP_LEAFS]);
      Mod_LoadNodes_BSP29(&header->lumps[LUMP_NODES]);
      Mod_LoadClipnodes_BSP29(&header->lumps[LUMP_CLIPNODES]);
   } else {
      Mod_LoadLeafs_BSP2(&header->lumps[LUMP_LEAFS]);
      Mod_LoadNodes_BSP2(&header->lumps[LUMP_NODES]);
      Mod_LoadClipnodes_BSP2(&header->lumps[LUMP_CLIPNODES]);
   }
   Mod_LoadEntities(&header->lumps[LUMP_ENTITIES]);
   Mod_LoadSubmodels(&header->lumps[LUMP_MODELS]);

   Mod_MakeHull0();

   mod->numframes = 2;		// regular and alternate animation
   mod->flags = 0;

   /*
    * Create space for the decompressed vis data
    * - We assume the main map is the first BSP file loaded (should be)
    * - If any other model has more leafs, then we may be in trouble...
    */
         if (mod->numleafs > pvscache_numleafs) {
            if (pvscache[0].leafbits)
               SV_Error("%s: %d allocated for visdata, but model %s has %d leafs",
                     __func__, pvscache_numleafs, loadmodel->name, mod->numleafs);
            Mod_InitPVSCache(mod->numleafs);
         }

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
void *Mod_Extradata(model_t *mod)
{
   void *r = Cache_Check(&mod->cache);
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
void Mod_Print(void)
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
void Mod_TouchModel(char *name)
{
   model_t *mod = Mod_FindName(name);

   if (!mod->needload)
   {
      if (mod->type == mod_alias)
         Cache_Check(&mod->cache);
   }
}

#endif /* !SERVERONLY */
