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
#include "d_iface.h"
#include "model.h"
#include "r_local.h"
#include "sys.h"

static aliashdr_t *pheader;

/*
=================
Mod_LoadAliasFrame
=================
*/
static void
Mod_LoadAliasFrame(const daliasframe_t *in, int *pframeindex, int numv,
		   trivertx_t *pbboxmin, trivertx_t *pbboxmax,
		   char *name, const char *loadname)
{
    trivertx_t *pframe;
    int i, j;

    strcpy(name, in->name);

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about
	// endianness
	pbboxmin->v[i] = in->bboxmin.v[i];
	pbboxmax->v[i] = in->bboxmax.v[i];
    }

    pframe = Hunk_AllocName(numv * sizeof(*pframe), loadname);
    *pframeindex = (byte *)pframe - (byte *)pheader;

    for (i = 0; i < numv; i++) {
	// these are all byte values, so no need to deal with endianness
	pframe[i].lightnormalindex = in->verts[i].lightnormalindex;
	for (j = 0; j < 3; j++)
	    pframe[i].v[j] = in->verts[i].v[j];
    }
}

/*
=================
Mod_LoadAliasGroupFrame
=================
*/
static void
Mod_LoadAliasGroupFrame(const daliasframe_t *in, int *pframeindex, int numv,
			const char *loadname)
{
    trivertx_t *pframe;
    int i, j;

    pframe = Hunk_AllocName(numv * sizeof(*pframe), loadname);
    *pframeindex = (byte *)pframe - (byte *)pheader;

    for (i = 0; i < numv; i++) {
	// these are all byte values, so no need to deal with endianness
	pframe[i].lightnormalindex = in->verts[i].lightnormalindex;
	for (j = 0; j < 3; j++)
	    pframe[i].v[j] = in->verts[i].v[j];
    }
}

/*
=================
Mod_LoadAliasGroup

returns a pointer to the memory location following this frame group
=================
*/
static daliasframetype_t *
Mod_LoadAliasGroup(const daliasgroup_t *in, maliasframedesc_t *frame, int numv,
		   const char *loadname)
{
    maliasgroup_t *paliasgroup;
    int i, numframes;
    float *poutintervals;
    daliasframe_t *dframe;

    numframes = LittleLong(in->numframes);
    paliasgroup = Hunk_AllocName(sizeof(maliasgroup_t) +
				 numframes * sizeof(paliasgroup->frames[0]),
				 loadname);
    paliasgroup->numframes = numframes;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    frame->frame = (byte *)paliasgroup - (byte *)pheader;
    poutintervals = Hunk_AllocName(numframes * sizeof(float), loadname);
    paliasgroup->intervals = (byte *)poutintervals - (byte *)pheader;

    for (i = 0; i < numframes; i++) {
	*poutintervals = LittleFloat(in->intervals[i].interval);
	if (*poutintervals <= 0.0)
	    Sys_Error("%s: interval <= 0", __func__);

	poutintervals++;
    }

    dframe = (daliasframe_t *)&in->intervals[numframes];
    strcpy(frame->name, dframe->name);
    for (i = 0; i < numframes; i++) {
	Mod_LoadAliasGroupFrame(dframe, &paliasgroup->frames[i], numv,
				loadname);
	dframe = (daliasframe_t *)&dframe->verts[numv];
    }

    return (daliasframetype_t *)dframe;
}


/*
=================
Mod_LoadAliasSkin
=================
*/
static void *
Mod_LoadAliasSkin(void *pin, int *pskinindex, int skinsize,
		  const char *loadname)
{
    int i;
    byte *pskin, *pinskin;
    unsigned short *pusskin;

    pskin = Hunk_AllocName(skinsize * r_pixbytes, loadname);
    pinskin = (byte *)pin;
    *pskinindex = (byte *)pskin - (byte *)pheader;

    if (r_pixbytes == 1) {
	memcpy(pskin, pinskin, skinsize);
    } else if (r_pixbytes == 2) {
	pusskin = (unsigned short *)pskin;

	for (i = 0; i < skinsize; i++)
	    pusskin[i] = d_8to16table[pinskin[i]];
    } else {
	Sys_Error("%s: driver set invalid r_pixbytes: %d", __func__,
		  r_pixbytes);
    }

    pinskin += skinsize;

    return ((void *)pinskin);
}


/*
=================
Mod_LoadAliasSkinGroup
=================
*/
static void *
Mod_LoadAliasSkinGroup(void *pin, int *pskinindex, int skinsize,
		       const char *loadname)
{
    daliasskingroup_t *pinskingroup;
    maliasskingroup_t *paliasskingroup;
    int i, numskins;
    daliasskininterval_t *pinskinintervals;
    float *poutskinintervals;
    void *ptemp;

    pinskingroup = (daliasskingroup_t *)pin;

    numskins = LittleLong(pinskingroup->numskins);

    paliasskingroup = Hunk_AllocName(sizeof(maliasskingroup_t) + numskins *
				     sizeof(paliasskingroup->skindescs[0]),
				     loadname);

    paliasskingroup->numskins = numskins;

    *pskinindex = (byte *)paliasskingroup - (byte *)pheader;
    pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);
    poutskinintervals = Hunk_AllocName(numskins * sizeof(float), loadname);

    paliasskingroup->intervals = (byte *)poutskinintervals - (byte *)pheader;

    for (i = 0; i < numskins; i++) {
	*poutskinintervals = LittleFloat(pinskinintervals->interval);
	if (*poutskinintervals <= 0)
	    Sys_Error("%s: interval <= 0", __func__);

	poutskinintervals++;
	pinskinintervals++;
    }

    ptemp = (void *)pinskinintervals;

    for (i = 0; i < numskins; i++) {
	ptemp = Mod_LoadAliasSkin(ptemp,
				  &paliasskingroup->skindescs[i].skin,
				  skinsize, loadname);
    }

    return ptemp;
}


/*
=================
Mod_LoadAliasModel
=================
*/
void
Mod_LoadAliasModel(model_t *mod, void *buffer, const model_t *loadmodel,
		   const char *loadname)
{
    int i;
    mdl_t *pmodel, *pinmodel;
    stvert_t *pstverts, *pinstverts;
    mtriangle_t *ptri;
    dtriangle_t *pintriangles;
    int version, numframes, numskins;
    int size;
    daliasframetype_t *pframetype;
    daliasframe_t *frame;
    daliasgroup_t *group;
    daliasskintype_t *pskintype;
    maliasskindesc_t *pskindesc;
    int skinsize;
    int start, end, total;

#ifdef QW_HACK
    if (!strcmp(loadmodel->name, "progs/player.mdl") ||
	!strcmp(loadmodel->name, "progs/eyes.mdl")) {
	unsigned short crc;
	byte *p;
	int len;
	char st[40];

	CRC_Init(&crc);
	for (len = com_filesize, p = buffer; len; len--, p++)
	    CRC_ProcessByte(&crc, *p);

	snprintf(st, sizeof(st), "%d", (int)crc);
	Info_SetValueForKey(cls.userinfo,
			    !strcmp(loadmodel->name,
				    "progs/player.mdl") ? pmodel_name :
			    emodel_name, st, MAX_INFO_STRING);

	if (cls.state >= ca_connected) {
	    MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	    snprintf(st, sizeof(st), "setinfo %s %d",
		     !strcmp(loadmodel->name,
			     "progs/player.mdl") ? pmodel_name : emodel_name,
		     (int)crc);
	    SZ_Print(&cls.netchan.message, st);
	}
    }
#endif

    start = Hunk_LowMark();

    pinmodel = (mdl_t *)buffer;

    version = LittleLong(pinmodel->version);
    if (version != ALIAS_VERSION)
	Sys_Error("%s has wrong version number (%i should be %i)",
		  mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
    size = sizeof(aliashdr_t) + LittleLong(pinmodel->numframes) *
	sizeof(pheader->frames[0]) +
	sizeof(mdl_t) +
	LittleLong(pinmodel->numverts) * sizeof(stvert_t) +
	LittleLong(pinmodel->numtris) * sizeof(mtriangle_t);

    pheader = Hunk_AllocName(size, loadname);
    pmodel = (mdl_t *)((byte *)&pheader[1] + LittleLong(pinmodel->numframes) *
		       sizeof(pheader->frames[0]));
    mod->flags = LittleLong(pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
    pmodel->boundingradius = LittleFloat(pinmodel->boundingradius);
    pmodel->numskins = LittleLong(pinmodel->numskins);
    pmodel->skinwidth = LittleLong(pinmodel->skinwidth);
    pmodel->skinheight = LittleLong(pinmodel->skinheight);

    if (pmodel->skinheight > MAX_LBM_HEIGHT)
	Sys_Error("model %s has a skin taller than %d", mod->name,
		  MAX_LBM_HEIGHT);

    pmodel->numverts = LittleLong(pinmodel->numverts);

    if (pmodel->numverts <= 0)
	Sys_Error("model %s has no vertices", mod->name);

    if (pmodel->numverts > MAXALIASVERTS)
	Sys_Error("model %s has too many vertices", mod->name);

    pmodel->numtris = LittleLong(pinmodel->numtris);

    if (pmodel->numtris <= 0)
	Sys_Error("model %s has no triangles", mod->name);

    pmodel->numframes = LittleLong(pinmodel->numframes);
    pmodel->size = LittleFloat(pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
    mod->synctype = LittleLong(pinmodel->synctype);
    mod->numframes = pmodel->numframes;

    for (i = 0; i < 3; i++) {
	pmodel->scale[i] = LittleFloat(pinmodel->scale[i]);
	pmodel->scale_origin[i] = LittleFloat(pinmodel->scale_origin[i]);
	pmodel->eyeposition[i] = LittleFloat(pinmodel->eyeposition[i]);
    }

    numskins = pmodel->numskins;
    numframes = pmodel->numframes;

    if (pmodel->skinwidth & 0x03)
	Sys_Error("%s: skinwidth not multiple of 4", __func__);

    pheader->model = (byte *)pmodel - (byte *)pheader;

//
// load the skins
//
    skinsize = pmodel->skinheight * pmodel->skinwidth;

    if (numskins < 1)
	Sys_Error("%s: Invalid # of skins: %d", __func__, numskins);

    pskintype = (daliasskintype_t *)&pinmodel[1];

    pskindesc = Hunk_AllocName(numskins * sizeof(maliasskindesc_t), loadname);

    pheader->skindesc = (byte *)pskindesc - (byte *)pheader;

    for (i = 0; i < numskins; i++) {
	aliasskintype_t skintype;

	skintype = LittleLong(pskintype->type);
	pskindesc[i].type = skintype;

	if (skintype == ALIAS_SKIN_SINGLE) {
	    pskintype = (daliasskintype_t *)
		Mod_LoadAliasSkin(pskintype + 1,
				  &pskindesc[i].skin, skinsize, loadname);
	} else {
	    pskintype = (daliasskintype_t *)
		Mod_LoadAliasSkinGroup(pskintype + 1,
				       &pskindesc[i].skin, skinsize, loadname);
	}
    }

//
// set base s and t vertices
//
    pstverts = (stvert_t *)&pmodel[1];
    pinstverts = (stvert_t *)pskintype;

    pheader->stverts = (byte *)pstverts - (byte *)pheader;

    for (i = 0; i < pmodel->numverts; i++) {
	pstverts[i].onseam = LittleLong(pinstverts[i].onseam);
	// put s and t in 16.16 format
	pstverts[i].s = LittleLong(pinstverts[i].s) << 16;
	pstverts[i].t = LittleLong(pinstverts[i].t) << 16;
    }

//
// set up the triangles
//
    ptri = (mtriangle_t *)&pstverts[pmodel->numverts];
    pintriangles = (dtriangle_t *)&pinstverts[pmodel->numverts];

    pheader->triangles = (byte *)ptri - (byte *)pheader;

    for (i = 0; i < pmodel->numtris; i++) {
	int j;

	ptri[i].facesfront = LittleLong(pintriangles[i].facesfront);

	for (j = 0; j < 3; j++) {
	    ptri[i].vertindex[j] = LittleLong(pintriangles[i].vertindex[j]);
	}
    }

//
// load the frames
//
    if (numframes < 1)
	Sys_Error("%s: Invalid # of frames: %d", __func__, numframes);

    pframetype = (daliasframetype_t *)&pintriangles[pmodel->numtris];

    for (i = 0; i < numframes; i++) {
	aliasframetype_t frametype;

	frametype = LittleLong(pframetype->type);
	pheader->frames[i].type = frametype;

	if (frametype == ALIAS_SINGLE) {
	    frame = (daliasframe_t *)(pframetype + 1);
	    Mod_LoadAliasFrame(frame, &pheader->frames[i].frame,
			       pmodel->numverts, &pheader->frames[i].bboxmin,
			       &pheader->frames[i].bboxmax,
			       pheader->frames[i].name, loadname);
	    pframetype = (daliasframetype_t *)&frame->verts[pmodel->numverts];
	} else {
	    group = (daliasgroup_t *)(pframetype + 1);
	    pframetype = Mod_LoadAliasGroup(group, &pheader->frames[i],
					    pmodel->numverts, loadname);
	}
    }

    mod->type = mod_alias;

// FIXME: do this right
    mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
    mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;

//
// move the complete, relocatable alias model to the cache
//
    end = Hunk_LowMark();
    total = end - start;

    Cache_Alloc(&mod->cache, total, loadname);
    if (!mod->cache.data)
	return;
    memcpy(mod->cache.data, pheader, total);

    Hunk_FreeToLowMark(start);
}

