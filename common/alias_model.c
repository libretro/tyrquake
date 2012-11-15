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

// a pose is a single set of vertexes.  a frame may be
// an animating sequence of poses
static const trivertx_t *poseverts[MAXALIASFRAMES];
static int posenum;

/*
=================
Mod_LoadAliasFrame
=================
*/
static void
Mod_LoadAliasFrame(const daliasframe_t *in, maliasframedesc_t *frame)
{
    int i;

    strncpy(frame->name, in->name, sizeof(frame->name));
    frame->name[sizeof(frame->name) - 1] = 0;
    frame->firstpose = posenum;
    frame->numposes = 1;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about
	// endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    poseverts[posenum] = in->verts;
    posenum++;
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
    int i, numframes;
    daliasframe_t *dframe;

    numframes = LittleLong(in->numframes);
    frame->firstpose = posenum;
    frame->numposes = numframes;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    /*
     * FIXME? the on-disk format allows for one interval per frame, but here
     *        the entire frame group gets just one interval. Probably all the
     *        original quake art assets just use a constant interval.
     */
    frame->interval = LittleFloat(in->intervals[0].interval);
    dframe = (daliasframe_t *)&in->intervals[numframes];
    strncpy(frame->name, dframe->name, sizeof(frame->name));
    frame->name[sizeof(frame->name) - 1] = 0;
    for (i = 0; i < numframes; i++) {
	poseverts[posenum] = dframe->verts;
	posenum++;
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
    int i, j;
    mdl_t *pinmodel;
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
    trivertx_t *verts;

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
			    !strcmp(loadmodel->name, "progs/player.mdl") ?
			    pmodel_name : emodel_name, st, MAX_INFO_STRING);

	if (cls.state >= ca_connected) {
	    MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	    snprintf(st, sizeof(st), "setinfo %s %d",
		     !strcmp(loadmodel->name, "progs/player.mdl") ?
		     pmodel_name : emodel_name, (int)crc);
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
    size = sizeof(aliashdr_t) +
	LittleLong(pinmodel->numframes) * sizeof(pheader->frames[0]) +
	LittleLong(pinmodel->numverts) * sizeof(stvert_t) +
	LittleLong(pinmodel->numtris) * sizeof(mtriangle_t);

    pheader = Hunk_AllocName(size, loadname);
    mod->flags = LittleLong(pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
    pheader->numskins = LittleLong(pinmodel->numskins);
    pheader->skinwidth = LittleLong(pinmodel->skinwidth);
    pheader->skinheight = LittleLong(pinmodel->skinheight);

    if (pheader->skinheight > MAX_LBM_HEIGHT)
	Sys_Error("model %s has a skin taller than %d", mod->name,
		  MAX_LBM_HEIGHT);

    pheader->numverts = LittleLong(pinmodel->numverts);

    if (pheader->numverts <= 0)
	Sys_Error("model %s has no vertices", mod->name);

    if (pheader->numverts > MAXALIASVERTS)
	Sys_Error("model %s has too many vertices", mod->name);

    pheader->numtris = LittleLong(pinmodel->numtris);

    if (pheader->numtris <= 0)
	Sys_Error("model %s has no triangles", mod->name);

    pheader->numframes = LittleLong(pinmodel->numframes);
    pheader->size = LittleFloat(pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
    mod->synctype = LittleLong(pinmodel->synctype);
    mod->numframes = pheader->numframes;

    for (i = 0; i < 3; i++) {
	pheader->scale[i] = LittleFloat(pinmodel->scale[i]);
	pheader->scale_origin[i] = LittleFloat(pinmodel->scale_origin[i]);
    }

    numskins = pheader->numskins;
    numframes = pheader->numframes;

    if (pheader->skinwidth & 0x03)
	Sys_Error("%s: skinwidth not multiple of 4", __func__);

//
// load the skins
//
    skinsize = pheader->skinheight * pheader->skinwidth;

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
    pstverts = (stvert_t *)((byte *)&pheader[1] + pheader->numframes *
			    sizeof(pheader->frames[0]));
    pinstverts = (stvert_t *)pskintype;

    pheader->stverts = (byte *)pstverts - (byte *)pheader;

    for (i = 0; i < pheader->numverts; i++) {
	pstverts[i].onseam = LittleLong(pinstverts[i].onseam);
	// put s and t in 16.16 format
	pstverts[i].s = LittleLong(pinstverts[i].s) << 16;
	pstverts[i].t = LittleLong(pinstverts[i].t) << 16;
    }

//
// set up the triangles
//
    ptri = (mtriangle_t *)&pstverts[pheader->numverts];
    pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];

    pheader->triangles = (byte *)ptri - (byte *)pheader;

    for (i = 0; i < pheader->numtris; i++) {
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

    posenum = 0;
    pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];

    for (i = 0; i < numframes; i++) {
	if (LittleLong(pframetype->type) == ALIAS_SINGLE) {
	    frame = (daliasframe_t *)(pframetype + 1);
	    Mod_LoadAliasFrame(frame, &pheader->frames[i]);
	    pframetype = (daliasframetype_t *)&frame->verts[pheader->numverts];
	} else {
	    group = (daliasgroup_t *)(pframetype + 1);
	    pframetype = Mod_LoadAliasGroup(group, &pheader->frames[i],
					    pheader->numverts, loadname);
	}
    }
    pheader->numposes = posenum;
    mod->type = mod_alias;

// FIXME: do this right
    mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
    mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;

    /*
     * Allocate the verts, copy and setup offsets
     */
    pheader->poseverts = pheader->numverts;
    verts = Hunk_Alloc(pheader->numposes * pheader->poseverts * sizeof(*verts));
    pheader->posedata = (byte *)verts - (byte *)pheader;
    for (i = 0; i < pheader->numposes; i++)
	for (j = 0; j < pheader->poseverts; j++)
	    *verts++ = poseverts[i][j];

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

