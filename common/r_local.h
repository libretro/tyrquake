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

#ifndef R_LOCAL_H
#define R_LOCAL_H

// r_local.h -- private refresh defs

#include "client.h"
#include "model.h"
#include "r_shared.h"

#define ALIAS_BASE_SIZE_RATIO	(1.0 / 11.0)
				// normalizing factor so player model works out
				// to about 1 pixel per triangle

#define BMODEL_FULLY_CLIPPED	(0x10)
				// value returned by R_BmodelCheckBBox ()
				// if bbox is trivially rejected

//===========================================================================
// viewmodel lighting

typedef struct {
    int ambientlight;
    int shadelight;
    float *plightvec;
} alight_t;

//===========================================================================
// clipped bmodel edges

typedef struct bedge_s {
    mvertex_t *v[2];
    struct bedge_s *pnext;
} bedge_t;

typedef struct {
    float fv[3];		// viewspace x, y
} auxvert_t;

//===========================================================================

extern cvar_t r_graphheight;
extern cvar_t r_clearcolor;
extern cvar_t r_waterwarp;

#define XCENTERING	(1.0 / 2.0)
#define YCENTERING	(1.0 / 2.0)

#define CLIP_EPSILON		0.001

#define BACKFACE_EPSILON	0.01

//===========================================================================

#define	DIST_NOT_SET	98765

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct clipplane_s {
    mplane_t plane;
    struct clipplane_s *next;
    byte leftedge;
    byte rightedge;
    byte reserved[2];
} clipplane_t;

extern clipplane_t view_clipplanes[4];

//=============================================================================

void R_RenderWorld(void);

//=============================================================================

extern mplane_t screenedge[4];
extern vec3_t r_origin;
extern vec3_t r_entorigin;
extern int r_visframecount;

//=============================================================================

//
// current entity info
//
extern qboolean insubmodel;

void R_DrawSprite(const entity_t *e);
void R_RenderFace(const entity_t *e, msurface_t *fa, int clipflags);
void R_RenderBmodelFace(const entity_t *e, bedge_t *pedges, msurface_t *psurf);
void R_TransformPlane(mplane_t *p, float *normal, float *dist);
void R_TransformFrustum(void);
void R_SetSkyFrame(void);
void R_DrawSurfaceBlock16(void);
void R_DrawSurfaceBlock8(void);

void R_GenSkyTile(void *pdest);
void R_GenSkyTile16(void *pdest);
void R_DrawSubmodelPolygons(const entity_t *e, model_t *pmodel, int clipflags);
void R_DrawSolidClippedSubmodelPolygons(const entity_t *e, model_t *pmodel);

void R_AddPolygonEdges(emitpoint_t *pverts, int numverts, int miplevel);
surf_t *R_GetSurf(void);
void R_AliasDrawModel(entity_t *e, alight_t *plighting);
void R_BeginEdgeFrame(void);
void R_ScanEdges(void);

extern void R_Surf8Start(void);
extern void R_Surf8End(void);
extern void R_Surf16Start(void);
extern void R_Surf16End(void);
extern void R_EdgeCodeStart(void);
extern void R_EdgeCodeEnd(void);

extern void R_RotateBmodel(const entity_t *e);

// !!! if this is changed, it must be changed in asm_draw.h too !!!
#define	NEAR_CLIP	0.01

extern int ubasestep, errorterm, erroradjustup, erroradjustdown;

extern fixed16_t sadjust, tadjust;
extern fixed16_t bbextents, bbextentt;

#define MAXBVERTINDEXES	1000	// new clipped vertices when clipping bmodels
				// to the world BSP
extern mvertex_t *r_ptverts, *r_ptvertsmax;

extern vec3_t sbaseaxis[3], tbaseaxis[3];

extern int r_currentkey;
extern int r_currentbkey;

//=========================================================
// Alias models
//=========================================================

#define MAXALIASVERTS		2048	// TODO: tune this
#define ALIAS_Z_CLIP_PLANE	5

extern int numverts;
extern int a_skinwidth;
extern int numtriangles;
extern float leftclip, topclip, rightclip, bottomclip;
extern int r_acliptype;
extern float r_avertexnormals[][3];

qboolean R_AliasCheckBBox(entity_t *e);

//=========================================================
// turbulence stuff

#define	TURB_SURF_AMP	8*0x10000
#define	TURB_SCREEN_AMP	3
#define	TURB_SPEED	20

//=========================================================
// particle stuff

void R_DrawParticles(void);
void R_InitParticles(void);
void R_ClearParticles(void);

void R_PushDlights (struct mnode_s *headnode); //qbism - moved from render.h

extern edge_t *auxedges;
extern int r_numallocatededges;
extern edge_t *r_edges, *edge_p, *edge_max;

extern edge_t *newedges[MAXHEIGHT];
extern edge_t *removeedges[MAXHEIGHT];

extern int screenwidth;

// FIXME: make stack vars when debugging done
extern edge_t edge_head;
extern edge_t edge_tail;
extern edge_t edge_aftertail;

extern float aliasxscale, aliasyscale, aliasxcenter, aliasycenter;
extern float r_aliastransition, r_resfudge;

extern mvertex_t *r_pcurrentvertbase;
extern int r_maxvalidedgeoffset;

void R_AliasClipTriangle(mtriangle_t *ptri, finalvert_t *pfinalverts, auxvert_t *pauxverts);
void R_AliasProjectFinalVert(finalvert_t *fv, auxvert_t *av);
void R_Alias_clip_top(finalvert_t *pfv0, finalvert_t *pfv1, finalvert_t *out);
void R_Alias_clip_bottom(finalvert_t *pfv0, finalvert_t *pfv1, finalvert_t *out);
void R_Alias_clip_left(finalvert_t *pfv0, finalvert_t *pfv1, finalvert_t *out);
void R_Alias_clip_right(finalvert_t *pfv0, finalvert_t *pfv1, finalvert_t *out);

extern int r_maxsurfsseen, r_maxedgesseen;
extern cshift_t cshift_water;
extern qboolean r_dowarpold, r_viewchanged;

extern mleaf_t *r_viewleaf, *r_oldviewleaf;

extern vec3_t r_emins, r_emaxs;
extern mnode_t *r_pefragtopnode;
extern int r_clipflags;

void R_StoreEfrags(efrag_t **ppefrag);
void R_AnimateLight(void);
int R_LightPoint(vec3_t p);
void R_SetupFrame(void);
void R_cshift_f(void);
void R_EmitEdge(mvertex_t *pv0, mvertex_t *pv1);
void R_ClipEdge(mvertex_t *pv0, mvertex_t *pv1, clipplane_t *clip);
void R_SplitEntityOnNode2(mnode_t *node);

void R_DrawSurfaceBlockRGB_mip0(void);
void R_DrawSurfaceBlockRGB_mip1(void);
void R_DrawSurfaceBlockRGB_mip2(void);
void R_DrawSurfaceBlockRGB_mip3(void);

#endif /* R_LOCAL_H */
