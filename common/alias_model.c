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

#include "common.h"
#include "crc.h"
#include "model.h"
#include "sys.h"

#include "r_local.h"

static aliashdr_t *pheader;

/* FIXME - get rid of these static limits by doing two passes? */

static stvert_t stverts[MAXALIASVERTS];
static mtriangle_t triangles[MAXALIASTRIS];

/* a pose is a single set of vertexes.  a frame may be */
/* an animating sequence of poses */
static const trivertx_t *poseverts[MAXALIASFRAMES];
static float poseintervals[MAXALIASFRAMES];
static int posenum;

#define MAXALIASSKINS 256

/* a skin may be an animating set 1 or more textures */
static float skinintervals[MAXALIASSKINS];
static byte *skindata[MAXALIASSKINS];
static int skinnum;

/*
=================
Mod_LoadAliasFrame
=================
*/
static void
Mod_LoadAliasFrame(const daliasframe_t *in, maliasframedesc_t *frame)
{
    int i;

    /* Defensive: ensure we have room in the per-load poseverts
     * pool before writing. */
    if (posenum >= MAXALIASFRAMES)
	Sys_Error("%s: posenum (%d) >= MAXALIASFRAMES",
                  __func__, posenum);

    frame->firstpose = posenum;
    frame->numposes = 1;

    for (i = 0; i < 3; i++) {
	/* these are byte values, so we don't have to worry about */
	/* endianness */
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    poseverts[posenum] = in->verts;
    poseintervals[posenum] = 999.0f; /* unused, but make problems obvious */
    posenum++;
}


/*
=================
Mod_LoadAliasGroup

returns a pointer to the memory location following this frame group
=================
*/
static daliasframetype_t *
Mod_LoadAliasGroup(const daliasgroup_t *in, maliasframedesc_t *frame,
                   const byte *bufend, const char *modname)
{
   int i, numframes;
   daliasframe_t *dframe;

#ifdef MSB_FIRST
   numframes = LittleLong(in->numframes);
#else
   numframes = (in->numframes);
#endif

   /* Defensive: numframes is file-controlled; without bounds
    * checking, a negative or absurdly large value either
    * underflows the in->intervals[numframes] pointer arithmetic
    * (causing OOB reads on subsequent loads) or overflows the
    * static poseverts[MAXALIASFRAMES] array. */
   if (numframes < 1 || numframes > MAXALIASFRAMES - posenum)
      Sys_Error("%s: bad numframes %d (posenum %d, max %d)",
                __func__, numframes, posenum, MAXALIASFRAMES);

   /* in->intervals[numframes] is the start of the per-subframe
    * data.  Bound the array within the file. */
   if ((const byte *)&in->intervals[numframes] > bufend)
      Sys_Error("model %s: group intervals past EOF (numframes %d)",
                modname, numframes);

   frame->firstpose = posenum;
   frame->numposes = numframes;

   for (i = 0; i < 3; i++) {
      /* these are byte values, so we don't have to worry about endianness */
      frame->bboxmin.v[i] = in->bboxmin.v[i];
      frame->bboxmax.v[i] = in->bboxmax.v[i];
   }

   dframe = (daliasframe_t *)&in->intervals[numframes];
   for (i = 0; i < numframes; i++) {
      /* dframe header + numverts trivertx_t must fit.
       * numverts was bounded against MAXALIASVERTS in
       * Mod_LoadAliasModel. */
      if ((const byte *)&dframe->verts[pheader->numverts] > bufend)
         Sys_Error("model %s: group subframe %d data past EOF",
                   modname, i);
      poseverts[posenum] = dframe->verts;
#ifdef MSB_FIRST
      poseintervals[posenum] = LittleFloat(in->intervals[i].interval);
#else
      poseintervals[posenum] = (in->intervals[i].interval);
#endif
      if (poseintervals[posenum] <= 0)
         Sys_Error("%s: interval <= 0", __func__);
      posenum++;
      dframe = (daliasframe_t *)&dframe->verts[pheader->numverts];
   }

   return (daliasframetype_t *)dframe;
}


/*
=================
Mod_LoadAliasSkinGroup
=================
*/
static void *
Mod_LoadAliasSkinGroup(void *pin, maliasskindesc_t *pskindesc, int skinsize,
                       const byte *bufend, const char *modname)
{
   daliasskininterval_t *pinskinintervals;
   byte *pdata;
   int i;
   int numframes;

   daliasskingroup_t *pinskingroup  = (daliasskingroup_t*)pin;

   /* daliasskingroup_t header (just the numskins int) must
    * fit before we read it. */
   if ((const byte *)pinskingroup + sizeof(daliasskingroup_t) > bufend)
      Sys_Error("%s: %s: skin group header past EOF",
                __func__, modname);

#ifdef MSB_FIRST
   numframes = LittleLong(pinskingroup->numskins);
#else
   numframes = (pinskingroup->numskins);
#endif

   /* Defensive: numskins is file-controlled.  Without bounds
    * checking, a negative or large value either skips the
    * loops below entirely (latent bug) or overflows the
    * static skinintervals[]/skindata[] arrays. */
   if (numframes < 1 || numframes > MAXALIASSKINS - skinnum)
      Sys_Error("%s: bad numframes %d (skinnum %d, max %d)",
                __func__, numframes, skinnum, MAXALIASSKINS);

   pskindesc->firstframe = skinnum;
   pskindesc->numframes  = numframes;
   pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);

   /* numframes daliasskininterval_t entries must fit. */
   if ((const byte *)&pinskinintervals[numframes] > bufend)
      Sys_Error("%s: %s: skin group intervals past EOF (numframes %d)",
                __func__, modname, numframes);

   for (i = 0; i < numframes; i++) {
#ifdef MSB_FIRST
      skinintervals[skinnum] = LittleFloat(pinskinintervals->interval);
#else
      skinintervals[skinnum] = (pinskinintervals->interval);
#endif
      if (skinintervals[skinnum] <= 0)
         Sys_Error("%s: interval <= 0", __func__);
      skinnum++;
      pinskinintervals++;
   }

   pdata = (byte *)pinskinintervals;
   /* numframes * skinsize bytes of pixel data must fit.
    * skinsize was already bounded against
    * MAX_LBM_HEIGHT * MAX_LBM_HEIGHT in Mod_LoadAliasModel,
    * so the multiplication can't overflow when widened to
    * size_t.  Store-side (LoadSkinData -> memcpy) reads
    * `skinsize` bytes from each skindata[i] pointer; without
    * the bound here, that memcpy would walk past the input
    * buffer. */
   {
      size_t total = (size_t)numframes * (size_t)skinsize;
      if ((const byte *)pdata > bufend
          || total > (size_t)(bufend - (const byte *)pdata))
         Sys_Error("%s: %s: skin group pixel data past EOF",
                   __func__, modname);
   }
   for (i = 0; i < numframes; i++)
   {
      skindata[pskindesc->firstframe + i] = pdata;
      pdata += skinsize;
   }

   return pdata;
}

/*
===============
Mod_LoadAllSkins
===============
*/
static void *
Mod_LoadAllSkins(const model_loader_t *loader, const model_t *loadmodel,
		 int numskins, daliasskintype_t *pskintype,
		 const byte *bufend)
{
   int i, skinsize;
   maliasskindesc_t *pskindesc;
   float *pskinintervals;
   byte *pskindata;

   if (numskins < 1)
      Sys_Error("%s: Invalid # of skins: %d", __func__, numskins);
   if (pheader->skinwidth & 0x03)
      Sys_Error("%s: skinwidth not multiple of 4", __func__);

   skinsize = pheader->skinwidth * pheader->skinheight;
   pskindesc = (maliasskindesc_t*)Hunk_Alloc(numskins * sizeof(maliasskindesc_t));
   pheader->skindesc = (byte *)pskindesc - (byte *)pheader;

   skinnum = 0;
   for (i = 0; i < numskins; i++)
   {
      aliasskintype_t skintype;

      /* daliasskintype_t (just the type int) must fit
       * before we read pskintype->type. */
      if ((const byte *)pskintype + sizeof(daliasskintype_t) > bufend)
         Sys_Error("%s: %s: skin %d type past EOF",
                   __func__, loadmodel->name, i);

#ifdef MSB_FIRST
      skintype = (aliasskintype_t)LittleLong(pskintype->type);
#else
      skintype = (aliasskintype_t)(pskintype->type);
#endif
      if (skintype == ALIAS_SKIN_SINGLE)
      {
         /* (pskintype + 1) + skinsize must fit. */
         if ((const byte *)(pskintype + 1) > bufend
             || (size_t)skinsize > (size_t)(bufend - (const byte *)(pskintype + 1)))
            Sys_Error("%s: %s: skin %d pixel data past EOF",
                      __func__, loadmodel->name, i);
         pskindesc[i].firstframe = skinnum;
         pskindesc[i].numframes = 1;
         skindata[skinnum] = (byte *)(pskintype + 1);
         skinintervals[skinnum] = 999.0f;
         skinnum++;
         pskintype = (daliasskintype_t *)((byte *)(pskintype + 1) + skinsize);
      }
      else
      {
         pskintype = (daliasskintype_t*)Mod_LoadAliasSkinGroup(pskintype + 1, pskindesc + i,
               skinsize, bufend, loadmodel->name);
      }
   }

   pskinintervals = (float*)Hunk_Alloc(skinnum * sizeof(float));
   pheader->skinintervals = (byte *)pskinintervals - (byte *)pheader;
   memcpy(pskinintervals, skinintervals, skinnum * sizeof(float));

   /* Hand off saving the skin data to the loader */
   pskindata = (byte*)loader->LoadSkinData(loadmodel->name, pheader, skinnum, skindata);
   pheader->skindata = (byte *)pskindata - (byte *)pheader;

   return pskintype;
}

/*
=================
Mod_LoadAliasModel
=================
*/
void
Mod_LoadAliasModel(const model_loader_t *loader, model_t *mod, void *buffer,
		   const model_t *loadmodel)
{
   byte *container;
   int i, j, pad;
   mdl_t *pinmodel;
   stvert_t *pinstverts;
   dtriangle_t *pintriangles;
   int version, numframes;
   int size;
   daliasframetype_t *pframetype;
   daliasframe_t *frame;
   daliasgroup_t *group;
   daliasskintype_t *pskintype;
   int start, end, total;
   float *intervals;
   const byte *bufend;

   start = Hunk_LowMark();

   pinmodel = (mdl_t *)buffer;
   /* bufend is the one-past-end pointer for `buffer`, the
    * com_filesize bytes loaded from disk.  Used below to
    * bound the running pointer cursor as we walk through
    * the variable-size sections (skins, frames).  Without
    * these checks a hostile .mdl whose header lies about
    * numverts/numframes/numtris can make a pointer step
    * past the end of the loaded buffer; subsequent reads
    * (Mod_LoadAliasFrame's bbox copies, the verts pointer
    * stored into poseverts[] for SW_LoadMeshData's per-pose
    * memcpy) walk into adjacent host memory.  com_filesize
    * is the file's actual size as set by COM_LoadStackFile
    * / Mod_LoadModel before this is called.  We require
    * the header itself to fit before we even read it. */
   if (com_filesize < (int)sizeof(mdl_t))
      Sys_Error("model %s truncated (filesize %d < %d)",
                mod->name, com_filesize, (int)sizeof(mdl_t));
   bufend = (const byte *)buffer + com_filesize;

#ifdef MSB_FIRST
   version = LittleLong(pinmodel->version);
#else
   version = (pinmodel->version);
#endif
   if (version != ALIAS_VERSION)
      Sys_Error("%s has wrong version number (%i should be %i)",
            mod->name, version, ALIAS_VERSION);

   /* allocate space for a working header, plus all the data except the frames, */
   /* skin and group info */
   pad = loader->Aliashdr_Padding();
#ifdef MSB_FIRST
   numframes = LittleLong(pinmodel->numframes);
#else
   numframes = (pinmodel->numframes);
#endif

   /* numframes is also bounded against MAXALIASFRAMES below
    * after the working header is allocated, but the size
    * computation
    *   size = pad + sizeof(aliashdr_t) + numframes * sizeof(...)
    * happens here first.  Without an early bound, a hostile
    * .mdl with numframes near INT_MAX makes the multiplication
    * overflow into a tiny positive (or wrapped-negative on
    * Hunk_Alloc -- still rejected by d0f7e07's allocator
    * bounds, but we want to fail with a useful message that
    * names the model). */
   if (numframes < 1 || numframes > MAXALIASFRAMES)
      Sys_Error("model %s has invalid numframes %d (max %d)",
                mod->name, numframes, MAXALIASFRAMES);

   size = pad + sizeof(aliashdr_t) +
      numframes * sizeof(pheader->frames[0]);

   container = (byte*)Hunk_Alloc(size);
   pheader = (aliashdr_t *)(container + pad);

#ifdef MSB_FIRST
   mod->flags = LittleLong(pinmodel->flags);

   /* endian-adjust and copy the data, starting with the alias model header */
   pheader->numskins = LittleLong(pinmodel->numskins);
   pheader->skinwidth = LittleLong(pinmodel->skinwidth);
   pheader->skinheight = LittleLong(pinmodel->skinheight);
#else
   mod->flags = (pinmodel->flags);

   pheader->numskins = (pinmodel->numskins);
   pheader->skinwidth = (pinmodel->skinwidth);
   pheader->skinheight = (pinmodel->skinheight);
#endif


   if (pheader->skinheight > MAX_LBM_HEIGHT)
      Sys_Error("model %s has a skin taller than %d", mod->name,
            MAX_LBM_HEIGHT);

   /* Defensive: skinwidth was previously unchecked, leaving
    * skinwidth*skinheight in Mod_LoadAllSkins vulnerable to
    * integer overflow on a hostile or corrupt .mdl file.
    * MAX_LBM_HEIGHT is also a sane width cap (textures don't
    * need to be wider than they're tall in this engine). */
   if (pheader->skinwidth <= 0 || pheader->skinwidth > MAX_LBM_HEIGHT)
      Sys_Error("model %s has invalid skinwidth %d", mod->name,
            pheader->skinwidth);

   /* numskins is later passed unbounded to Mod_LoadAllSkins,
    * which writes into skindata[MAXALIASSKINS] indexed by
    * file-controlled values.  Cap here to prevent OOB writes
    * into adjacent globals from a malicious model. */
   if (pheader->numskins <= 0)
      Sys_Error("model %s has no skins", mod->name);
   if (pheader->numskins > MAXALIASSKINS)
      Sys_Error("model %s has too many skins (%d > %d)",
            mod->name, pheader->numskins, MAXALIASSKINS);

#ifdef MSB_FIRST
   pheader->numverts = LittleLong(pinmodel->numverts);
#else
   pheader->numverts = (pinmodel->numverts);
#endif

   if (pheader->numverts <= 0)
      Sys_Error("model %s has no vertices", mod->name);

   if (pheader->numverts > MAXALIASVERTS)
      Sys_Error("model %s has too many vertices", mod->name);

#ifdef MSB_FIRST
   pheader->numtris = LittleLong(pinmodel->numtris);
#else
   pheader->numtris = (pinmodel->numtris);
#endif

   if (pheader->numtris <= 0)
      Sys_Error("model %s has no triangles", mod->name);

   if (pheader->numtris > MAXALIASTRIS)
      Sys_Error("model %s has too many triangles (%d > %d)",
            mod->name, pheader->numtris, MAXALIASTRIS);

#ifdef MSB_FIRST
   pheader->size = LittleFloat(pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
   mod->synctype = (synctype_t)LittleLong(pinmodel->synctype);
#else
   pheader->size = (pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
   mod->synctype = (synctype_t)(pinmodel->synctype);
#endif
   /* numframes was already validated and byte-swapped at the
    * top of this function. */
   pheader->numframes = numframes;
   mod->numframes = pheader->numframes;

   for (i = 0; i < 3; i++) {
#ifdef MSB_FIRST
      pheader->scale[i] = LittleFloat(pinmodel->scale[i]);
      pheader->scale_origin[i] = LittleFloat(pinmodel->scale_origin[i]);
#else
      pheader->scale[i] = (pinmodel->scale[i]);
      pheader->scale_origin[i] = (pinmodel->scale_origin[i]);
#endif
   }

   /* load the skins */
   pskintype = (daliasskintype_t *)&pinmodel[1];
   if ((const byte *)pskintype > bufend)
      Sys_Error("model %s: skintype past EOF", mod->name);
   pskintype = (daliasskintype_t *)Mod_LoadAllSkins(loader, loadmodel, pheader->numskins,
         pskintype, bufend);
   if ((const byte *)pskintype > bufend)
      Sys_Error("model %s: skin data past EOF", mod->name);

   /* set base s and t vertices */
   pinstverts = (stvert_t *)pskintype;
   /* numverts was bounded against MAXALIASVERTS above, so
    * numverts * sizeof(stvert_t) cannot overflow; compare
    * the resulting one-past-end pointer against bufend. */
   if ((const byte *)&pinstverts[pheader->numverts] > bufend)
      Sys_Error("model %s: stverts past EOF (numverts %d)",
                mod->name, pheader->numverts);
   for (i = 0; i < pheader->numverts; i++) {
#ifdef MSB_FIRST
      stverts[i].onseam = LittleLong(pinstverts[i].onseam);
      stverts[i].s = LittleLong(pinstverts[i].s);
      stverts[i].t = LittleLong(pinstverts[i].t);
#else
      stverts[i].onseam = (pinstverts[i].onseam);
      stverts[i].s = (pinstverts[i].s);
      stverts[i].t = (pinstverts[i].t);
#endif
   }

   /* set up the triangles */
   pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];
   if ((const byte *)&pintriangles[pheader->numtris] > bufend)
      Sys_Error("model %s: triangles past EOF (numtris %d)",
                mod->name, pheader->numtris);
   for (i = 0; i < pheader->numtris; i++)
   {
#ifdef MSB_FIRST
      triangles[i].facesfront = LittleLong(pintriangles[i].facesfront);
#else
      triangles[i].facesfront = (pintriangles[i].facesfront);
#endif
      for (j = 0; j < 3; j++)
      {
#ifdef MSB_FIRST
         triangles[i].vertindex[j] = LittleLong(pintriangles[i].vertindex[j]);
#else
         triangles[i].vertindex[j] = (pintriangles[i].vertindex[j]);
#endif
         if (triangles[i].vertindex[j] < 0 ||
               triangles[i].vertindex[j] >= pheader->numverts)
            Sys_Error("%s: invalid vertex index (%d of %d) in %s\n",
                  __func__, triangles[i].vertindex[j],
                  pheader->numverts, mod->name);
      }
   }

   /* load the frames */
   numframes = pheader->numframes;
   if (numframes < 1)
      Sys_Error("%s: Invalid # of frames: %d", __func__, numframes);

   posenum = 0;
   pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];
   /* bound the initial frame pointer; the loop body checks
    * each subsequent step before dereferencing */
   if ((const byte *)pframetype + sizeof(daliasframetype_t) > bufend)
      Sys_Error("model %s: frametype past EOF", mod->name);

   for (i = 0; i < numframes; i++)
   {
      /* per-iteration: pframetype must have room for at
       * least the type field; the inner branches check
       * the larger sub-headers (daliasframe_t /
       * daliasgroup_t) before walking past them. */
      if ((const byte *)pframetype + sizeof(daliasframetype_t) > bufend)
         Sys_Error("model %s: frame %d header past EOF", mod->name, i);
#ifdef MSB_FIRST
      if (LittleLong(pframetype->type) == ALIAS_SINGLE)
#else
         if ((pframetype->type) == ALIAS_SINGLE)
#endif
         {
            frame = (daliasframe_t *)(pframetype + 1);
            /* daliasframe_t header + numverts * trivertx_t
             * must all fit.  numverts was bounded against
             * MAXALIASVERTS so the multiplication is safe. */
            if ((const byte *)&frame->verts[pheader->numverts] > bufend)
               Sys_Error("model %s: frame %d data past EOF",
                         mod->name, i);
            Mod_LoadAliasFrame(frame, &pheader->frames[i]);
            pframetype = (daliasframetype_t *)&frame->verts[pheader->numverts];
         } else {
            group = (daliasgroup_t *)(pframetype + 1);
            /* daliasgroup_t header itself must fit before
             * we read group->numframes inside
             * Mod_LoadAliasGroup; that function then
             * walks group->intervals[numframes] and
             * subsequent daliasframe_t verts.  The full
             * extent depends on the file-supplied
             * numframes, so we re-check inside the helper
             * before each step. */
            if ((const byte *)group + sizeof(daliasgroup_t) > bufend)
               Sys_Error("model %s: frame %d group header past EOF",
                         mod->name, i);
            pframetype = Mod_LoadAliasGroup(group, &pheader->frames[i], bufend, mod->name);
            if ((const byte *)pframetype > bufend)
               Sys_Error("model %s: frame %d group walk past EOF",
                         mod->name, i);
         }
   }
   pheader->numposes = posenum;
   mod->type = mod_alias;

   /* FIXME: do this right */
   mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
   mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;

   /* Save the frame intervals */
   intervals = (float*)Hunk_Alloc(pheader->numposes * sizeof(float));
   pheader->poseintervals = (byte *)intervals - (byte *)pheader;
   for (i = 0; i < pheader->numposes; i++)
      intervals[i] = poseintervals[i];

   /* Save the mesh data (verts, stverts, triangles) */
   loader->LoadMeshData(loadmodel, pheader, triangles, stverts, poseverts);

   /* move the complete, relocatable alias model to the cache */
   end = Hunk_LowMark();
   total = end - start;

   Cache_AllocPadded(&mod->cache, pad, total - pad);
   if (!mod->cache.data)
      return;

   memcpy((byte *)mod->cache.data - pad, container, total);

   Hunk_FreeToLowMark(start);
}
