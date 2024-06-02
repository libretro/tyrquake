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
// d_polyset.c: routines for drawing sets of polygons sharing the same
// texture (used for Alias models)

#include <stdint.h>

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

// TODO: put in span spilling to shrink list size
// !!! if this is changed, it must be changed in d_polysa.s too !!!
#define DPS_MAXSPANS MAXHEIGHT+1
			// 1 extra for spanpackage that marks end

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct {
    void *pdest;
    short *pz;
    int count;
    byte *ptex;
    int sfrac, tfrac, light, zi;
} spanpackage_t;

typedef struct {
    int isflattop;
    int numleftedges;
    int *pleftedgevert0;
    int *pleftedgevert1;
    int *pleftedgevert2;
    int numrightedges;
    int *prightedgevert0;
    int *prightedgevert1;
    int *prightedgevert2;
} edgetable;

int r_p0[6], r_p1[6], r_p2[6];

byte *d_pcolormap;

int d_xdenom;

static edgetable *pedgetable;
static edgetable edgetables[12] = {
    {0, 1, r_p0, r_p2, NULL, 2, r_p0, r_p1, r_p2},
    {0, 2, r_p1, r_p0, r_p2, 1, r_p1, r_p2, NULL},
    {1, 1, r_p0, r_p2, NULL, 1, r_p1, r_p2, NULL},
    {0, 1, r_p1, r_p0, NULL, 2, r_p1, r_p2, r_p0},
    {0, 2, r_p0, r_p2, r_p1, 1, r_p0, r_p1, NULL},
    {0, 1, r_p2, r_p1, NULL, 1, r_p2, r_p0, NULL},
    {0, 1, r_p2, r_p1, NULL, 2, r_p2, r_p0, r_p1},
    {0, 2, r_p2, r_p1, r_p0, 1, r_p2, r_p0, NULL},
    {0, 1, r_p1, r_p0, NULL, 1, r_p1, r_p2, NULL},
    {1, 1, r_p2, r_p1, NULL, 1, r_p0, r_p1, NULL},
    {1, 1, r_p1, r_p0, NULL, 1, r_p2, r_p0, NULL},
    {0, 1, r_p0, r_p2, NULL, 1, r_p0, r_p1, NULL},
};

/* FIXME: some of these can become statics */
int a_sstepxfrac, a_tstepxfrac, r_lstepx, a_ststepxwhole;
int r_sstepx, r_tstepx, r_lstepy, r_sstepy, r_tstepy;
int r_zistepx, r_zistepy;
int d_aspancount, d_countextrastep;

spanpackage_t *a_spans;
spanpackage_t *d_pedgespanpackage;
static int ystart;
byte *d_pdest, *d_ptex;
short *d_pz;
int d_sfrac, d_tfrac, d_light, d_zi;
int d_ptexextrastep, d_sfracextrastep;
int d_tfracextrastep, d_lightextrastep, d_pdestextrastep;
int d_lightbasestep, d_pdestbasestep, d_ptexbasestep;
int d_sfracbasestep, d_tfracbasestep;
int d_ziextrastep, d_zibasestep;
int d_pzextrastep, d_pzbasestep;

typedef struct {
    int quotient;
    int remainder;
} adivtab_t;

static adivtab_t adivtab[32 * 32] = {
#include "adivtab.h"
};

byte *skintable[MAX_LBM_HEIGHT];
static int skinwidth;
static byte *skinstart;

void D_PolysetDrawSpans8(spanpackage_t *pspanpackage);
void D_PolysetCalcGradients(int skinwidth);
void D_PolysetSetEdgeTable(void);
void D_RasterizeAliasPolySmooth(void);
void D_PolysetScanLeftEdge(int height);

static void D_DrawSubdiv(void);
static void D_DrawNonSubdiv(void);
static void D_PolysetRecursiveTriangle(int *p1, int *p2, int *p3);

/*
================
D_PolysetDraw
================
*/
void
D_PolysetDraw(void)
{
   spanpackage_t spans[CACHE_PAD_ARRAY(DPS_MAXSPANS + 1, spanpackage_t)];
   /* one extra because of cache line pretouching */

   a_spans = (spanpackage_t *)
      (((uintptr_t)&spans[0] + CACHE_SIZE - 1) & ~(uintptr_t)(CACHE_SIZE - 1));

   if (r_affinetridesc.drawtype)
      D_DrawSubdiv();
   else
      D_DrawNonSubdiv();
}

#ifdef HEXEN2
void D_PolysetDrawT3 (void)
{
   spanpackage_t spans[CACHE_PAD_ARRAY(DPS_MAXSPANS + 1, spanpackage_t)];
   /* one extra because of cache line pretouching */

   a_spans = (spanpackage_t *)
      (((long)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
		D_DrawSubdivT3 ();
	else
		D_DrawNonSubdiv ();
}

void D_PolysetDrawT (void)
{
   spanpackage_t spans[CACHE_PAD_ARRAY(DPS_MAXSPANS + 1, spanpackage_t)];
   /* one extra because of cache line pretouching */

   a_spans = (spanpackage_t *)
      (((long)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
		D_DrawSubdivT ();
	else
		D_DrawNonSubdiv ();
}

void D_PolysetDrawT2 (void)
{
   spanpackage_t spans[CACHE_PAD_ARRAY(DPS_MAXSPANS + 1, spanpackage_t)];
   /* one extra because of cache line pretouching */

   a_spans = (spanpackage_t *)
      (((long)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
		D_DrawSubdivT2 ();
	else
		D_DrawNonSubdiv ();
}

void D_PolysetDrawT5 (void)
{
   spanpackage_t spans[CACHE_PAD_ARRAY(DPS_MAXSPANS + 1, spanpackage_t)];
   /* one extra because of cache line pretouching */

   a_spans = (spanpackage_t *)
      (((long)&spans[0] + CACHE_SIZE - 1) & ~(CACHE_SIZE - 1));

	if (r_affinetridesc.drawtype)
		D_DrawSubdivT5 ();
	else
		D_DrawNonSubdiv ();
}
#endif


/*
================
D_PolysetDrawFinalVerts

TODO/FIXME - Needs updates/specialization for Hexen 2
================
*/
void
D_PolysetDrawFinalVerts(finalvert_t *fv, int numverts)
{
   int i, z;
   int16_t *zbuf;

   for (i = 0; i < numverts; i++, fv++)
   {
      /* valid triangle coordinates for filling can include the bottom and
       * right clip edges, due to the fill rule; these shouldn't be drawn */
      if ((fv->v[0] < r_refdef.vrectright) &&
            (fv->v[1] < r_refdef.vrectbottom))
      {
         /* Baker - in very rare occassions, these may have 
          * negative values and can result in a memory access violation */
         if (fv->v[0] >=0 && fv->v[1] >=0)
         {
            z = fv->v[5] >> 16;
            zbuf = zspantable[fv->v[1]] + fv->v[0];
            if (z >= *zbuf)
            {
               int pix;

               *zbuf = z;
               pix = skintable[fv->v[3] >> 16][fv->v[2] >> 16];
               pix = ((byte *)acolormap)[pix + (fv->v[4] & 0xFF00)];
               d_viewbuffer[d_scantable[fv->v[1]] + fv->v[0]] = pix;
            }
         }
      }
   }
}


/*
================
D_DrawSubdiv

TODO/FIXME - Needs updates/specialization for Hexen 2
================
*/
static void D_DrawSubdiv(void)
{
   int i;
   finalvert_t *pfv  = r_affinetridesc.pfinalverts;
   mtriangle_t *ptri = r_affinetridesc.ptriangles;
   int lnumtriangles = r_affinetridesc.numtriangles;

   for (i = 0; i < lnumtriangles; i++)
   {
      finalvert_t *index0 = pfv + ptri[i].vertindex[0];
      finalvert_t *index1 = pfv + ptri[i].vertindex[1];
      finalvert_t *index2 = pfv + ptri[i].vertindex[2];

      if (((index0->v[1] - index1->v[1]) *
               (index0->v[0] - index2->v[0]) -
               (index0->v[0] - index1->v[0]) *
               (index0->v[1] - index2->v[1])) >= 0)
         continue;

      d_pcolormap = &((byte *)acolormap)[index0->v[4] & 0xFF00];

      if (ptri[i].facesfront)
         D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);
      else
      {
         int s0 = index0->v[2];
         int s1 = index1->v[2];
         int s2 = index2->v[2];

         if (index0->flags & ALIAS_ONSEAM)
            index0->v[2] += r_affinetridesc.seamfixupX16;
         if (index1->flags & ALIAS_ONSEAM)
            index1->v[2] += r_affinetridesc.seamfixupX16;
         if (index2->flags & ALIAS_ONSEAM)
            index2->v[2] += r_affinetridesc.seamfixupX16;

         D_PolysetRecursiveTriangle(index0->v, index1->v, index2->v);

         index0->v[2] = s0;
         index1->v[2] = s1;
         index2->v[2] = s2;
      }
   }
}


/*
================
D_DrawNonSubdiv

TODO/FIXME - Needs updates/specialization for Hexen 2
================
*/
static void D_DrawNonSubdiv(void)
{
   int i;

   finalvert_t *pfv = r_affinetridesc.pfinalverts;
   mtriangle_t *ptri = r_affinetridesc.ptriangles;
   int lnumtriangles = r_affinetridesc.numtriangles;

   for (i = 0; i < lnumtriangles; i++, ptri++)
   {
      finalvert_t *index0 = pfv + ptri->vertindex[0];
      finalvert_t *index1 = pfv + ptri->vertindex[1];
      finalvert_t *index2 = pfv + ptri->vertindex[2];

      d_xdenom = (index0->v[1] - index1->v[1]) *
         (index0->v[0] - index2->v[0]) -
         (index0->v[0] - index1->v[0]) * (index0->v[1] - index2->v[1]);

      if (d_xdenom >= 0) {
         continue;
      }

      r_p0[0] = index0->v[0];	// u
      r_p0[1] = index0->v[1];	// v
      r_p0[2] = index0->v[2];	// s
      r_p0[3] = index0->v[3];	// t
      r_p0[4] = index0->v[4];	// light
      r_p0[5] = index0->v[5];	// iz

      r_p1[0] = index1->v[0];
      r_p1[1] = index1->v[1];
      r_p1[2] = index1->v[2];
      r_p1[3] = index1->v[3];
      r_p1[4] = index1->v[4];
      r_p1[5] = index1->v[5];

      r_p2[0] = index2->v[0];
      r_p2[1] = index2->v[1];
      r_p2[2] = index2->v[2];
      r_p2[3] = index2->v[3];
      r_p2[4] = index2->v[4];
      r_p2[5] = index2->v[5];

      if (!ptri->facesfront) {
         if (index0->flags & ALIAS_ONSEAM)
            r_p0[2] += r_affinetridesc.seamfixupX16;
         if (index1->flags & ALIAS_ONSEAM)
            r_p1[2] += r_affinetridesc.seamfixupX16;
         if (index2->flags & ALIAS_ONSEAM)
            r_p2[2] += r_affinetridesc.seamfixupX16;
      }

      D_PolysetSetEdgeTable();
      D_RasterizeAliasPolySmooth();
   }
}


/*
================
D_PolysetRecursiveTriangle

TODO/FIXME - Needs updates/specialization for Hexen 2
================
*/
static void D_PolysetRecursiveTriangle(int *lp1, int *lp2, int *lp3)
{
   int *temp;
   int newobj[6];
   int z;
   int16_t *zbuf;

   int d = lp2[0] - lp1[0];
   if (d < -1 || d > 1)
      goto split;
   d = lp2[1] - lp1[1];
   if (d < -1 || d > 1)
      goto split;

   d = lp3[0] - lp2[0];
   if (d < -1 || d > 1)
      goto split2;
   d = lp3[1] - lp2[1];
   if (d < -1 || d > 1)
      goto split2;

   d = lp1[0] - lp3[0];
   if (d < -1 || d > 1)
      goto split3;
   d = lp1[1] - lp3[1];
   if (d < -1 || d > 1) {
split3:
      temp = lp1;
      lp1 = lp3;
      lp3 = lp2;
      lp2 = temp;

      goto split;
   }

   return;			// entire tri is filled

split2:
   temp = lp1;
   lp1 = lp2;
   lp2 = lp3;
   lp3 = temp;

split:
   // split this edge
   newobj[0] = (lp1[0] + lp2[0]) >> 1;
   newobj[1] = (lp1[1] + lp2[1]) >> 1;
   newobj[2] = (lp1[2] + lp2[2]) >> 1;
   newobj[3] = (lp1[3] + lp2[3]) >> 1;
   newobj[5] = (lp1[5] + lp2[5]) >> 1;

   // draw the point if splitting a leading edge
   if (lp2[1] > lp1[1])
      goto nodraw;
   if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0]))
      goto nodraw;


   z = newobj[5] >> 16;
   zbuf = zspantable[newobj[1]] + newobj[0];
   if (z >= *zbuf) {
      int pix;

      *zbuf = z;
      pix = d_pcolormap[skintable[newobj[3] >> 16][newobj[2] >> 16]];
      d_viewbuffer[d_scantable[newobj[1]] + newobj[0]] = pix;
   }

nodraw:
   // recursively continue
   D_PolysetRecursiveTriangle(lp3, lp1, newobj);
   D_PolysetRecursiveTriangle(lp3, newobj, lp2);
}

/*
================
D_PolysetUpdateTables
================
*/
void D_PolysetUpdateTables(void)
{
   if (  r_affinetridesc.skinwidth != skinwidth ||
         r_affinetridesc.pskin     != skinstart)
   {
      byte *s;
      int i;
      skinwidth = r_affinetridesc.skinwidth;
      skinstart = (byte*)r_affinetridesc.pskin;
      s = skinstart;

      for (i = 0; i < MAX_LBM_HEIGHT; i++, s += skinwidth)
         skintable[i] = s;
   }
}

/*
===================
D_PolysetScanLeftEdge
====================
*/
void D_PolysetScanLeftEdge(int height)
{
   do
   {
      d_pedgespanpackage->pdest = d_pdest;
      d_pedgespanpackage->pz    = d_pz;
      d_pedgespanpackage->count = d_aspancount;
      d_pedgespanpackage->ptex  = d_ptex;

      d_pedgespanpackage->sfrac = d_sfrac;
      d_pedgespanpackage->tfrac = d_tfrac;

      /* FIXME: need to clamp l, s, t, at both ends? */
      d_pedgespanpackage->light = d_light;
      d_pedgespanpackage->zi    = d_zi;

      d_pedgespanpackage++;

      errorterm += erroradjustup;
      if (errorterm >= 0) {
         d_pdest += d_pdestextrastep;
         d_pz += d_pzextrastep;
         d_aspancount += d_countextrastep;
         d_ptex += d_ptexextrastep;
         d_sfrac += d_sfracextrastep;
         d_ptex += d_sfrac >> 16;

         d_sfrac &= 0xFFFF;
         d_tfrac += d_tfracextrastep;
         if (d_tfrac & 0x10000) {
            d_ptex += r_affinetridesc.skinwidth;
            d_tfrac &= 0xFFFF;
         }
         d_light += d_lightextrastep;
         d_zi += d_ziextrastep;
         errorterm -= erroradjustdown;
      } else {
         d_pdest += d_pdestbasestep;
         d_pz += d_pzbasestep;
         d_aspancount += ubasestep;
         d_ptex += d_ptexbasestep;
         d_sfrac += d_sfracbasestep;
         d_ptex += d_sfrac >> 16;
         d_sfrac &= 0xFFFF;
         d_tfrac += d_tfracbasestep;
         if (d_tfrac & 0x10000) {
            d_ptex += r_affinetridesc.skinwidth;
            d_tfrac &= 0xFFFF;
         }
         d_light += d_lightbasestep;
         d_zi += d_zibasestep;
      }
   } while (--height);
}

/*
===================
D_PolysetSetUpForLineScan
====================
*/
static void D_PolysetSetUpForLineScan(fixed8_t startvertu, fixed8_t startvertv,
			  fixed8_t endvertu, fixed8_t endvertv)
{
   int tm = endvertu - startvertu;
   int tn = endvertv - startvertv;

   errorterm = -1;

   if (((tm <= 16) && (tm >= -15)) && ((tn <= 16) && (tn >= -15)))
   {
      adivtab_t *ptemp = &adivtab[((tm + 15) << 5) + (tn + 15)];
      ubasestep = ptemp->quotient;
      erroradjustup = ptemp->remainder;
      erroradjustdown = tn;
   }
   else
   {
      double dm = (double)tm;
      double dn = (double)tn;

      FloorDivMod(dm, dn, &ubasestep, &erroradjustup);

      erroradjustdown = dn;
   }
}

/*
================
D_PolysetCalcGradients
================
*/
void D_PolysetCalcGradients(int skinwidth)
{
   static float
      xstepdenominv
      ,   ystepdenominv
      ,   t0
      ,   t1
      ,   p01_minus_p21
      ,   p11_minus_p21
      ,   p00_minus_p20
      ,   p10_minus_p20
      ;

   xstepdenominv = 1.0 / (float)d_xdenom;
   ystepdenominv = -xstepdenominv;

   // mankrip - optimization
   p00_minus_p20 = (r_p0[0] - r_p2[0]) * ystepdenominv;
   p01_minus_p21 = (r_p0[1] - r_p2[1]) * xstepdenominv;
   p10_minus_p20 = (r_p1[0] - r_p2[0]) * ystepdenominv;
   p11_minus_p21 = (r_p1[1] - r_p2[1]) * xstepdenominv;

   t0 = r_p0[2] - r_p2[2];
   t1 = r_p1[2] - r_p2[2];
   r_sstepx  = (int)      (t1 * p01_minus_p21 - t0 * p11_minus_p21);
   r_sstepy  = (int)      (t1 * p00_minus_p20 - t0 * p10_minus_p20);

   t0 = r_p0[3] - r_p2[3];
   t1 = r_p1[3] - r_p2[3];
   r_tstepx  = (int)      (t1 * p01_minus_p21 - t0 * p11_minus_p21);
   r_tstepy  = (int)      (t1 * p00_minus_p20 - t0 * p10_minus_p20);

   // ceil () for light so positive steps are exaggerated, negative steps diminished,
   // pushing us away from underflow toward overflow.
   // Underflow is very visible, overflow is very unlikely, because of ambient lighting
   t0 = r_p0[4] - r_p2[4];
   t1 = r_p1[4] - r_p2[4];
   r_lstepx  = (int) ceil (t1 * p01_minus_p21 - t0 * p11_minus_p21);
   r_lstepy  = (int) ceil (t1 * p00_minus_p20 - t0 * p10_minus_p20);

   t0 = r_p0[5] - r_p2[5];
   t1 = r_p1[5] - r_p2[5];
   r_zistepx = (int)      (t1 * p01_minus_p21 - t0 * p11_minus_p21);
   r_zistepy = (int)      (t1 * p00_minus_p20 - t0 * p10_minus_p20);

   a_sstepxfrac = r_sstepx & 0xFFFF;
   a_tstepxfrac = r_tstepx & 0xFFFF;

   a_ststepxwhole = skinwidth * (r_tstepx >> 16) + (r_sstepx >> 16);
}


/*
================
D_PolysetDrawSpans8
================
*/
void D_PolysetDrawSpansRGB(spanpackage_t *pspanpackage);

void D_PolysetDrawSpans8(spanpackage_t *pspanpackage)
{
   byte *lpdest;
   byte *lptex;
   int lsfrac, ltfrac;
   int llight;
   int lzi;
   int16_t *lpz;

   do
   {
      int lcount = d_aspancount - pspanpackage->count;

      errorterm += erroradjustup;
      if (errorterm >= 0)
      {
         d_aspancount += d_countextrastep;
         errorterm -= erroradjustdown;
      }
      else
         d_aspancount += ubasestep;

      if (lcount > 0)
      {
         lpdest = (byte*)pspanpackage->pdest;
         lptex = pspanpackage->ptex;
         lpz = pspanpackage->pz;
         lsfrac = pspanpackage->sfrac;
         ltfrac = pspanpackage->tfrac;
         llight = pspanpackage->light;
         lzi = pspanpackage->zi;

         do
         {
            if ((lzi >> 16) >= *lpz) {
               *lpdest = ((byte *)acolormap)[*lptex + (llight & 0xFF00)];
               *lpz = lzi >> 16;
            }
            lpdest++;
            lzi += r_zistepx;
            lpz++;
            llight += r_lstepx;
            lptex += a_ststepxwhole;
            lsfrac += a_sstepxfrac;
            lptex += lsfrac >> 16;
            lsfrac &= 0xFFFF;
            ltfrac += a_tstepxfrac;
            if (ltfrac & 0x10000) {
               lptex += r_affinetridesc.skinwidth;
               ltfrac &= 0xFFFF;
            }
         } while (--lcount);
      }

      pspanpackage++;
   } while (pspanpackage->count != -999999);
}

// leilei - quickly hacked colored lighting on models
extern vec3_t lightcolor; // for colored lighting
extern	int			host_fullbrights;   // for preserving fullbrights in color operations

void D_PolysetDrawSpansRGB(spanpackage_t *pspanpackage)
{
   byte *lpdest;
   byte *lptex;
   byte ah;
   vec3_t lc;
   unsigned trans[3];
   unsigned char *pix24;	// leilei - colored lighting
   int lsfrac, ltfrac;
   int llight;
   int lzi;
   short *lpz;
   // normalize
   //VectorNormalize(lightcolor);

   do
   {
      int lcount = d_aspancount - pspanpackage->count;

      errorterm += erroradjustup;
      if (errorterm >= 0)
      {
         d_aspancount += d_countextrastep;
         errorterm -= erroradjustdown;
      }
      else
         d_aspancount += ubasestep;

      if (lcount > 0)
      {
         lpdest = (byte*)pspanpackage->pdest;
         lptex = pspanpackage->ptex;
         lpz = pspanpackage->pz;
         lsfrac = pspanpackage->sfrac;
         ltfrac = pspanpackage->tfrac;
         llight = pspanpackage->light;
         lzi = pspanpackage->zi;

         do
         {
            if ((lzi >> 16) >= *lpz) {
				// leilei - gross simple hack. it goes like this
				// lpdest = the skin......
				//	TIMES
				// Colored lighting color
				//      AND THEN
				// colormap is blended on it
		if (*lptex < host_fullbrights)
		{
			int seven;
			ah = ((byte *)acolormap)[*lptex + (0 & 0xFF00)];
			pix24 = (unsigned char *)&d_8to24table[ah];

			//lc[0] *= 1; 
			//lc[1] *= 1;
			//lc[2] -= (llight & 0x0000);
			for (seven=0;seven<3;seven++)
			//lc[seven] =  (llight  & 0xFF00) / 255;

			lc[seven] =  (lightcolor[seven] / 1024);

			//	lc[seven] = (16384 - (llight & 0xFF00)) * (lightcolor[seven]);

			//lc[seven] = (16384 - llight & 0xFF00) * lightcolor[seven];

	//		trans[0] = (pix24[0] * (lc[0]<<6 )) >> 15;
	//		trans[1] = (pix24[1] * (lc[1]<<6 )) >> 15;
	//		trans[2] = (pix24[2] * (lc[2]<<6 )) >> 15;


			trans[0] = (pix24[0] * lc[0]);
			trans[1] = (pix24[1] * lc[1]);
			trans[2] = (pix24[2] * lc[2]);
			

			//if (trans[0] & ~63) trans[0] = 63; if (trans[1] & ~63) trans[1] = 63; if (trans[2] & ~63) trans[2] = 63;

			//ah = palmap2 [(int)trans[0]] [(int)trans[1]] [(int)trans[2]];
	        //        *lpdest = ((byte *)acolormap)[ah + (llight & 0xFF00)];
		         //*lpdest = ((byte *)acolormap)[ah];
		
			*lpdest = palmap2 [trans[0]] [trans[1]] [trans[2]];

		        // *lpdest = palmap2 [trans[0] >> 17] [trans[1] >> 17] [trans[2] >> 17];

		}
		else
		{
		*lpdest = *lptex; // go directly to the color
		}
               *lpz = lzi >> 16;
            }
            lpdest++;
            lzi += r_zistepx;
            lpz++;
            llight += r_lstepx;
            lptex += a_ststepxwhole;
            lsfrac += a_sstepxfrac;
            lptex += lsfrac >> 16;
            lsfrac &= 0xFFFF;
            ltfrac += a_tstepxfrac;
            if (ltfrac & 0x10000) {
               lptex += r_affinetridesc.skinwidth;
               ltfrac &= 0xFFFF;
            }
         } while (--lcount);
      }

      pspanpackage++;
   } while (pspanpackage->count != -999999);
}

/*
================
D_RasterizeAliasPolySmooth
================
*/

extern int coloredlights;

void D_RasterizeAliasPolySmooth(void)
{
   int working_lstepx, originalcount;

   int *plefttop = pedgetable->pleftedgevert0;
   int *prighttop = pedgetable->prightedgevert0;

   int *pleftbottom = pedgetable->pleftedgevert1;
   int *prightbottom = pedgetable->prightedgevert1;

   int initialleftheight = pleftbottom[1] - plefttop[1];
   int initialrightheight = prightbottom[1] - prighttop[1];

   //
   // set the s, t, and light gradients, which are consistent across the triangle
   // because being a triangle, things are affine
#ifdef HEXEN2
   if ((currententity->model->flags & EF_SPECIAL_TRANS))
      D_PolysetCalcGradients (r_affinetridesc.skinwidth);
   else if (currententity->drawflags & DRF_TRANSLUCENT)
      D_PolysetCalcGradients (r_affinetridesc.skinwidth);
   else if ((currententity->model->flags & EF_TRANSPARENT))
      D_PolysetCalcGradients (r_affinetridesc.skinwidth);
   else if ((currententity->model->flags & EF_HOLEY))
      D_PolysetCalcGradients (r_affinetridesc.skinwidth);
   else
#endif
      D_PolysetCalcGradients(r_affinetridesc.skinwidth);

   //
   // rasterize the polygon
   //

   //
   // scan out the top (and possibly only) part of the left edge
   //
   D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
         pleftbottom[0], pleftbottom[1]);

   d_pedgespanpackage = a_spans;

   ystart = plefttop[1];
   d_aspancount = plefttop[0] - prighttop[0];

   d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
      (plefttop[3] >> 16) * r_affinetridesc.skinwidth;
   d_sfrac = plefttop[2] & 0xFFFF;
   d_tfrac = plefttop[3] & 0xFFFF;
   d_pzbasestep = d_zwidth + ubasestep;
   d_pzextrastep = d_pzbasestep + 1;
   d_light = plefttop[4];
   d_zi = plefttop[5];

   d_pdestbasestep = screenwidth + ubasestep;
   d_pdestextrastep = d_pdestbasestep + 1;
   d_pdest = (byte *)d_viewbuffer + ystart * screenwidth + plefttop[0];
   d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

   /* TODO: can reuse partial expressions here */

   /*
    * for negative steps in x along left edge, bias toward overflow rather
    * than underflow (sort of turning the floor () we did in the gradient
    * calcs into ceil (), but plus a little bit)
    */
   if (ubasestep < 0)
      working_lstepx = r_lstepx - 1;
   else
      working_lstepx = r_lstepx;

   d_countextrastep = ubasestep + 1;
   d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
      ((r_tstepy + r_tstepx * ubasestep) >> 16) * r_affinetridesc.skinwidth;
   d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
   d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
   d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
   d_zibasestep = r_zistepy + r_zistepx * ubasestep;

   d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
      ((r_tstepy + r_tstepx * d_countextrastep) >> 16) *
      r_affinetridesc.skinwidth;
   d_sfracextrastep = (r_sstepy + r_sstepx * d_countextrastep) & 0xFFFF;
   d_tfracextrastep = (r_tstepy + r_tstepx * d_countextrastep) & 0xFFFF;
   d_lightextrastep = d_lightbasestep + working_lstepx;
   d_ziextrastep = d_zibasestep + r_zistepx;

   D_PolysetScanLeftEdge(initialleftheight);

   //
   // scan out the bottom part of the left edge, if it exists
   //
   if (pedgetable->numleftedges == 2) {
      int height;

      plefttop = pleftbottom;
      pleftbottom = pedgetable->pleftedgevert2;

      D_PolysetSetUpForLineScan(plefttop[0], plefttop[1],
            pleftbottom[0], pleftbottom[1]);

      height = pleftbottom[1] - plefttop[1];

      // TODO: make this a function; modularize this function in general

      ystart = plefttop[1];
      d_aspancount = plefttop[0] - prighttop[0];
      d_ptex = (byte *)r_affinetridesc.pskin + (plefttop[2] >> 16) +
         (plefttop[3] >> 16) * r_affinetridesc.skinwidth;
      d_sfrac = 0;
      d_tfrac = 0;
      d_light = plefttop[4];
      d_zi = plefttop[5];

      d_pdestbasestep = screenwidth + ubasestep;
      d_pdestextrastep = d_pdestbasestep + 1;
      d_pdest = (byte *)d_viewbuffer + ystart * screenwidth + plefttop[0];
      d_pzbasestep = d_zwidth + ubasestep;
      d_pzextrastep = d_pzbasestep + 1;
      d_pz = d_pzbuffer + ystart * d_zwidth + plefttop[0];

      if (ubasestep < 0)
         working_lstepx = r_lstepx - 1;
      else
         working_lstepx = r_lstepx;

      d_countextrastep = ubasestep + 1;
      d_ptexbasestep = ((r_sstepy + r_sstepx * ubasestep) >> 16) +
         ((r_tstepy + r_tstepx * ubasestep) >> 16) *
         r_affinetridesc.skinwidth;
      d_sfracbasestep = (r_sstepy + r_sstepx * ubasestep) & 0xFFFF;
      d_tfracbasestep = (r_tstepy + r_tstepx * ubasestep) & 0xFFFF;
      d_lightbasestep = r_lstepy + working_lstepx * ubasestep;
      d_zibasestep = r_zistepy + r_zistepx * ubasestep;

      d_ptexextrastep = ((r_sstepy + r_sstepx * d_countextrastep) >> 16) +
         ((r_tstepy + r_tstepx * d_countextrastep) >> 16) *
         r_affinetridesc.skinwidth;
      d_sfracextrastep = (r_sstepy + r_sstepx * d_countextrastep) & 0xFFFF;
      d_tfracextrastep = (r_tstepy + r_tstepx * d_countextrastep) & 0xFFFF;
      d_lightextrastep = d_lightbasestep + working_lstepx;
      d_ziextrastep = d_zibasestep + r_zistepx;

      D_PolysetScanLeftEdge(height);
   }
   // scan out the top (and possibly only) part of the right edge, updating the
   // count field
   d_pedgespanpackage = a_spans;

   D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
         prightbottom[0], prightbottom[1]);
   d_aspancount = 0;
   d_countextrastep = ubasestep + 1;
   originalcount = a_spans[initialrightheight].count;
   a_spans[initialrightheight].count = -999999;	// mark end of the spanpackages

   if (coloredlights)
      D_PolysetDrawSpansRGB(a_spans);
   else
      D_PolysetDrawSpans8(a_spans);


   // scan out the bottom part of the right edge, if it exists
   if (pedgetable->numrightedges == 2) {
      int height;
      spanpackage_t *pstart;

      pstart = a_spans + initialrightheight;
      pstart->count = originalcount;

      d_aspancount = prightbottom[0] - prighttop[0];

      prighttop = prightbottom;
      prightbottom = pedgetable->prightedgevert2;

      height = prightbottom[1] - prighttop[1];

      D_PolysetSetUpForLineScan(prighttop[0], prighttop[1],
            prightbottom[0], prightbottom[1]);

      d_countextrastep = ubasestep + 1;
      a_spans[initialrightheight + height].count = -999999;
      // mark end of the spanpackages

      if (coloredlights)
         D_PolysetDrawSpansRGB(pstart);
      else
         D_PolysetDrawSpans8(pstart);
   }
}


/*
================
D_PolysetSetEdgeTable
================
*/
void D_PolysetSetEdgeTable(void)
{
   int edgetableindex = 0;		// assume the vertices are already in
   //  top to bottom order

   // determine which edges are right & left, and the order in which
   // to rasterize them
   if (r_p0[1] >= r_p1[1]) {
      if (r_p0[1] == r_p1[1]) {
         if (r_p0[1] < r_p2[1])
            pedgetable = &edgetables[2];
         else
            pedgetable = &edgetables[5];

         return;
      } else {
         edgetableindex = 1;
      }
   }

   if (r_p0[1] == r_p2[1]) {
      if (edgetableindex)
         pedgetable = &edgetables[8];
      else
         pedgetable = &edgetables[9];

      return;
   } else if (r_p1[1] == r_p2[1]) {
      if (edgetableindex)
         pedgetable = &edgetables[10];
      else
         pedgetable = &edgetables[11];

      return;
   }

   if (r_p0[1] > r_p2[1])
      edgetableindex += 2;

   if (r_p1[1] > r_p2[1])
      edgetableindex += 4;

   pedgetable = &edgetables[edgetableindex];
}
