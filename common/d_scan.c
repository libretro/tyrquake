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
// d_scan.c
//
// Portable C scan-level rasterization code, all pixel depths.

#include <stdint.h>

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

unsigned char *r_turb_pbase, *r_turb_pdest;
fixed16_t r_turb_s, r_turb_t, r_turb_sstep, r_turb_tstep;
int *r_turb_turb;
int r_turb_spancount;

void D_DrawTurbulent8Span(void);

/*
=============
D_WarpScreen

// this performs a slight compression of the screen at the same time as
// the sine warp, to keep the edges from wrapping
=============
*/
void
D_WarpScreen(void)
{
   int u, v;
   byte *dest;
   int *turb;
   byte **row;
   int *column;

   int w = r_refdef.vrect.width;
   int h = r_refdef.vrect.height;

   float wratio = w / (float)scr_vrect.width;
   float hratio = h / (float)scr_vrect.height;

   // FIXME - use Zmalloc or similar?
   // FIXME - rowptr and column are constant for same vidmode?
   // FIXME - do they cycle?
   byte **rowptr = (byte**)malloc((scr_vrect.height + TURB_SCREEN_AMP * 2) * sizeof(byte *));

   for (v = 0; v < scr_vrect.height + TURB_SCREEN_AMP * 2; v++)
   {
      rowptr[v] = d_viewbuffer + (r_refdef.vrect.y * screenwidth) +
         (screenwidth * (int)((float)v * hratio * h /
                              (h + TURB_SCREEN_AMP * 2)));
   }

   column = (int*)malloc((scr_vrect.width + TURB_SCREEN_AMP * 2) * sizeof(int));
   for (u = 0; u < scr_vrect.width + TURB_SCREEN_AMP * 2; u++)
   {
      column[u] = r_refdef.vrect.x +
         (int)((float)u * wratio * w / (w + TURB_SCREEN_AMP * 2));
   }

   turb = intsintable + ((int)(cl.time * TURB_SPEED) & (TURB_CYCLE - 1));
   dest = vid.buffer + scr_vrect.y * vid.rowbytes + scr_vrect.x;

   for (v = 0; v < scr_vrect.height; v++, dest += vid.rowbytes)
   {
      int *col = &column[turb[v & (TURB_CYCLE - 1)]];
      row = &rowptr[v];
      for (u = 0; u < scr_vrect.width; u += 4)
      {
         dest[u + 0] = row[turb[(u + 0) & (TURB_CYCLE - 1)]][col[u + 0]];
         dest[u + 1] = row[turb[(u + 1) & (TURB_CYCLE - 1)]][col[u + 1]];
         dest[u + 2] = row[turb[(u + 2) & (TURB_CYCLE - 1)]][col[u + 2]];
         dest[u + 3] = row[turb[(u + 3) & (TURB_CYCLE - 1)]][col[u + 3]];
      }
   }

   free(rowptr);
   free(column);
}

/*
=============
D_DrawTurbulent8Span
=============
*/
void
D_DrawTurbulent8Span(void)
{
   do
   {
      int tturb;
      int sturb = r_turb_s + r_turb_turb[(r_turb_t >> 16) & (TURB_CYCLE - 1)];
      sturb = (sturb >> 16) & (TURB_TEX_SIZE - 1);
      tturb = r_turb_t + r_turb_turb[(r_turb_s >> 16) & (TURB_CYCLE - 1)];
      tturb = (tturb >> 16) & (TURB_TEX_SIZE - 1);
      *r_turb_pdest++ = *(r_turb_pbase + (tturb * TURB_TEX_SIZE) + sturb);
      r_turb_s += r_turb_sstep;
      r_turb_t += r_turb_tstep;
   } while (--r_turb_spancount > 0);
}

/*
=============
Turbulent8
=============
*/
void
Turbulent8(espan_t *pspan)
{
   fixed16_t snext, tnext;
   float sdivz16stepu, tdivz16stepu, zi16stepu;

   r_turb_turb = sintable + ((int)(cl.time * TURB_SPEED) & (TURB_CYCLE - 1));

   r_turb_sstep = 0;		// keep compiler happy
   r_turb_tstep = 0;		// ditto

   r_turb_pbase = (unsigned char *)cacheblock;

   sdivz16stepu = d_sdivzstepu * 16;
   tdivz16stepu = d_tdivzstepu * 16;
   zi16stepu = d_zistepu * 16;

   do
   {
      // calculate the initial s/z, t/z, 1/z, s, and t and clamp
      int count = pspan->count;
      float  du = (float)pspan->u;
      float  dv = (float)pspan->v;
      float sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
      float tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;
      float zi    = d_ziorigin + dv * d_zistepv + du * d_zistepu;
      float z     = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

      r_turb_pdest = (unsigned char *)((byte *)d_viewbuffer +
            (screenwidth * pspan->v) + pspan->u);
      r_turb_s     = (int)(sdivz * z) + sadjust;

      if (r_turb_s > bbextents)
         r_turb_s = bbextents;
      else if (r_turb_s < 0)
         r_turb_s = 0;

      r_turb_t = (int)(tdivz * z) + tadjust;
      if (r_turb_t > bbextentt)
         r_turb_t = bbextentt;
      else if (r_turb_t < 0)
         r_turb_t = 0;

      do
      {
         // calculate s and t at the far end of the span
         if (count >= 16)
            r_turb_spancount = 16;
         else
            r_turb_spancount = count;

         count -= r_turb_spancount;

         if (count)
         {
            // calculate s/z, t/z, zi->fixed s and t at far end of span,
            // calculate s and t steps across span by shifting
            sdivz += sdivz16stepu;
            tdivz += tdivz16stepu;
            zi += zi16stepu;
            z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext < 16)
               snext = 16;	// prevent round-off error on <0 steps from
            //  from causing overstepping & running off the
            //  edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 16)
               tnext = 16;	// guard against round-off error on <0 steps

            r_turb_sstep = (snext - r_turb_s) >> 4;
            r_turb_tstep = (tnext - r_turb_t) >> 4;
         }
         else
         {
            // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
            // can't step off polygon), clamp, calculate s and t steps across
            // span by division, biasing steps low so we don't run off the
            // texture
            float spancountminus1 = (float)(r_turb_spancount - 1);
            sdivz += d_sdivzstepu * spancountminus1;
            tdivz += d_tdivzstepu * spancountminus1;
            zi += d_zistepu * spancountminus1;
            z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext < 16)
               snext = 16;	// prevent round-off error on <0 steps from
            //  from causing overstepping & running off the
            //  edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 16)
               tnext = 16;	// guard against round-off error on <0 steps

            if (r_turb_spancount > 1) {
               r_turb_sstep =
                  (snext - r_turb_s) / (r_turb_spancount - 1);
               r_turb_tstep =
                  (tnext - r_turb_t) / (r_turb_spancount - 1);
            }
         }

         r_turb_s = r_turb_s & ((TURB_CYCLE << 16) - 1);
         r_turb_t = r_turb_t & ((TURB_CYCLE << 16) - 1);

         D_DrawTurbulent8Span();

         r_turb_s = snext;
         r_turb_t = tnext;

      } while (count > 0);

   } while ((pspan = pspan->pnext) != NULL);
}

/*
   =============
   D_DrawSpans16

FIXME: actually make this subdivide by 16 instead of 8!!!  qb:  OK!!!!
=============
*/

            /*==============================================
            //unrolled- mh, MK, qbism
            //============================================*/

   static int          count, spancount;
   static byte         *pbase, *pdest;
   static fixed16_t    s, t, snext, tnext, sstep, tstep;
   static float        sdivz, tdivz, zi, z, du, dv, spancountminus1;
   static float        sdivzstepu, tdivzstepu, zistepu;

   int dither_kernel[2][2][2] =
{
   {
      {16384,0},
      {49152,32768}
   }
   ,
      {
         {32768,49152},
         {0,16384}
      }
};

#define SOLID(i) pdest[i] = pbase[(s >> 16) + (t >> 16) * cachewidth]
#define DITHERED_SOLID(i) pdest[i] = pbase[idiths + iditht * cachewidth]
#define DITHERED_SOLID_B(i) pdest[i] = pbase[idiths_b + iditht_b * cachewidth]

#define DITHERED_SOLID_B_UPDATE() \
   idiths_b = (s + dither_val_s_b) >> 16; iditht_b = (t + dither_val_t_b) >> 16; \
idiths_b = idiths_b ? ((idiths_b) - 1) : idiths_b; \
iditht_b = iditht_b ? ((iditht_b) - 1) : iditht_b

#define DITHERED_SOLID_UPDATE() \
   idiths = (s + dither_val_s) >> 16; iditht = (t + dither_val_t) >> 16; \
idiths = idiths ? ((idiths) - 1) : idiths; \
iditht = iditht ? ((iditht) - 1) : iditht

//qbism: pointer to pbase and macroize idea from mankrip
#define WRITEPDEST(i) { pdest[i] = *(pbase + (s >> 16) + (t >> 16) * cachewidth); s+=sstep; t+=tstep;}

extern surfcache_t		*pcurrentcache;

void D_DrawSpans16Qb(espan_t *pspan) //qb: up it from 8 to 16.  This + unroll = big speed gain!
{
   sstep = 0;   // keep compiler happy
   tstep = 0;   // ditto

   pbase = (byte *)cacheblock;
   sdivzstepu = d_sdivzstepu * 16;
   tdivzstepu = d_tdivzstepu * 16;
   zistepu = d_zistepu * 16;

   do
   {
      pdest = (byte *)((byte *)d_viewbuffer + (screenwidth * pspan->v) + pspan->u);
      count = pspan->count >> 4;

      spancount = pspan->count % 16;

      // calculate the initial s/z, t/z, 1/z, s, and t and clamp
      du = (float)pspan->u;
      dv = (float)pspan->v;

      sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
      tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
      zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
      z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point

      s = (int)(sdivz * z) + sadjust;
      if (s < 0) s = 0;
      else if (s > bbextents) s = bbextents;

      t = (int)(tdivz * z) + tadjust;
      if (t < 0) t = 0;
      else if (t > bbextentt) t = bbextentt;

      while (count-- > 0) // Manoel Kasimier
      {
         sdivz += sdivzstepu;
         tdivz += tdivzstepu;
         zi += zistepu;
         z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point

         snext = (int)(sdivz * z) + sadjust;
         if (snext < 16) snext = 16;
         else if (snext > bbextents) snext = bbextents;

         tnext = (int)(tdivz * z) + tadjust;
         if (tnext < 16) tnext = 16;
         else if (tnext > bbextentt) tnext = bbextentt;

         sstep = (snext - s) >> 4;
         tstep = (tnext - t) >> 4;
         pdest += 16;

         WRITEPDEST(-16);
         WRITEPDEST(-15);
         WRITEPDEST(-14);
         WRITEPDEST(-13);
         WRITEPDEST(-12);
         WRITEPDEST(-11);
         WRITEPDEST(-10);
         WRITEPDEST(-9);
         WRITEPDEST(-8);
         WRITEPDEST(-7);
         WRITEPDEST(-6);
         WRITEPDEST(-5);
         WRITEPDEST(-4);
         WRITEPDEST(-3);
         WRITEPDEST(-2);
         WRITEPDEST(-1);

         s = snext;
         t = tnext;
      }
      if (spancount > 0)
      {
         spancountminus1 = (float)(spancount - 1);
         sdivz += d_sdivzstepu * spancountminus1;
         tdivz += d_tdivzstepu * spancountminus1;
         zi += d_zistepu * spancountminus1;
         z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point

         snext = (int)(sdivz * z) + sadjust;
         if (snext < 16) snext = 16;
         else if (snext > bbextents) snext = bbextents;

         tnext = (int)(tdivz * z) + tadjust;
         if (tnext < 16) tnext = 16;
         else if (tnext > bbextentt) tnext = bbextentt;

         if (spancount > 1)
         {
            sstep = (snext - s) / (spancount - 1);
            tstep = (tnext - t) / (spancount - 1);
         }

         pdest += spancount;

         switch (spancount)
         {
            case 16:
               WRITEPDEST(-16);
            case 15:
               WRITEPDEST(-15);
            case 14:
               WRITEPDEST(-14);
            case 13:
               WRITEPDEST(-13);
            case 12:
               WRITEPDEST(-12);
            case 11:
               WRITEPDEST(-11);
            case 10:
               WRITEPDEST(-10);
            case  9:
               WRITEPDEST(-9);
            case  8:
               WRITEPDEST(-8);
            case  7:
               WRITEPDEST(-7);
            case  6:
               WRITEPDEST(-6);
            case  5:
               WRITEPDEST(-5);
            case  4:
               WRITEPDEST(-4);
            case  3:
               WRITEPDEST(-3);
            case  2:
               WRITEPDEST(-2);
            case  1:
               WRITEPDEST(-1);
               break;
         }
      }
   } while ((pspan = pspan->pnext) != NULL);
}

void D_DrawSpans16QbDither (espan_t *pspan) //qbism up it from 8 to 16. This + unroll = big speed gain!
{
   int spancount;
   uint8_t *pbase;
   fixed16_t snext, tnext, sstep, tstep;
   float sdivzstepu, tdivzstepu, zistepu;

   // mipmaps shouldn't be dithered
   if (pcurrentcache->mipscale < 1.0f)
   {
      D_DrawSpans16Qb(pspan);
      return;
   }

   sstep = 0; // keep compiler happy
   tstep = 0; // ditto

   pbase      = (uint8_t*)cacheblock;
   sdivzstepu = d_sdivzstepu * 16;
   tdivzstepu = d_tdivzstepu * 16;
   zistepu    = d_zistepu * 16;

   do
   {
      fixed16_t t;
      int count = pspan->count;
      // calculate the initial s/z, t/z, 1/z, s, and t and clamp
      float du = (float)pspan->u;
      float dv = (float)pspan->v;

      float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
      float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
      float zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
      float z = (float)0x10000 / zi; // prescale to 16.16 fixed-point

      fixed16_t s = (int)(sdivz * z) + sadjust;

      uint8_t *pdest = (uint8_t*)((byte *)d_viewbuffer +
            (screenwidth * pspan->v) + pspan->u);

      if (s > bbextents)
         s = bbextents;
      else if (s < 0)
         s = 0;

      t = (int)(tdivz * z) + tadjust;
      if (t > bbextentt)
         t = bbextentt;
      else if (t < 0)
         t = 0;

      do
      {
         // calculate s and t at the far end of the span
         if (count >= 16)
            spancount = 16;
         else
            spancount = count;

         count -= spancount;

         if (count)
         {
            // calculate s/z, t/z, zi->fixed s and t at far end of span,
            // calculate s and t steps across span by shifting
            sdivz += sdivzstepu;
            tdivz += tdivzstepu;
            zi += zistepu;
            z = (float)0x10000 / zi; // prescale to 16.16 fixed-point

            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext <= 16)
               snext = 16; // prevent round-off error on <0 steps from
            // from causing overstepping & running off the
            // edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 16)
               tnext = 16; // guard against round-off error on <0 steps

            sstep = (snext - s) >> 4;
            tstep = (tnext - t) >> 4;
         }
         else
         {
            // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
            // can't step off polygon), clamp, calculate s and t steps across
            // span by division, biasing steps low so we don't run off the
            // texture
            float spancountminus1 = (float)(spancount - 1);
            sdivz += d_sdivzstepu * spancountminus1;
            tdivz += d_tdivzstepu * spancountminus1;
            zi += d_zistepu * spancountminus1;
            z = (float)0x10000 / zi; // prescale to 16.16 fixed-point
            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext < 16)
               snext = 16; // prevent round-off error on <0 steps from
            // from causing overstepping & running off the
            // edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 16)
               tnext = 16; // guard against round-off error on <0 steps

            if (spancount > 1)
            {
               sstep = (snext - s) / (spancount - 1);
               tstep = (tnext - t) / (spancount - 1);
            }
         }

         {
            int X = (pspan->u + spancount) & 1;
            int Y = (pspan->v) & 1;
            int dither_val_s = dither_kernel[X][Y][0];
            int dither_val_t = dither_kernel[X][Y][1];
            int dither_val_s_b = dither_kernel[!X][Y][0];
            int dither_val_t_b = dither_kernel[!X][Y][1];
            int idiths, iditht, idiths_b, iditht_b;

            DITHERED_SOLID_UPDATE();
            DITHERED_SOLID_B_UPDATE();

            //qbism- Duff's Device loop unroll per mh.
            pdest += spancount;
            switch (spancount)
            {
               case 16: DITHERED_SOLID(-16); s += sstep; t += tstep;
                        DITHERED_SOLID_B_UPDATE();
               case 15: DITHERED_SOLID_B(-15); s += sstep; t += tstep;
                        DITHERED_SOLID_UPDATE();
               case 14: DITHERED_SOLID(-14); s += sstep; t += tstep;
                        DITHERED_SOLID_B_UPDATE();
               case 13: DITHERED_SOLID_B(-13); s += sstep; t += tstep;
                        DITHERED_SOLID_UPDATE();
               case 12: DITHERED_SOLID(-12); s += sstep; t += tstep;
                        DITHERED_SOLID_B_UPDATE();
               case 11: DITHERED_SOLID_B(-11); s += sstep; t += tstep;
                        DITHERED_SOLID_UPDATE();
               case 10: DITHERED_SOLID(-10); s += sstep; t += tstep;
                        DITHERED_SOLID_B_UPDATE();
               case 9: DITHERED_SOLID_B(-9); s += sstep; t += tstep;
                       DITHERED_SOLID_UPDATE();
               case 8: DITHERED_SOLID(-8); s += sstep; t += tstep;
                       DITHERED_SOLID_B_UPDATE();
               case 7: DITHERED_SOLID_B(-7); s += sstep; t += tstep;
                       DITHERED_SOLID_UPDATE();
               case 6: DITHERED_SOLID(-6); s += sstep; t += tstep;
                       DITHERED_SOLID_B_UPDATE();
               case 5: DITHERED_SOLID_B(-5); s += sstep; t += tstep;
                       DITHERED_SOLID_UPDATE();
               case 4: DITHERED_SOLID(-4); s += sstep; t += tstep;
                       DITHERED_SOLID_B_UPDATE();
               case 3: DITHERED_SOLID_B(-3); s += sstep; t += tstep;
                       DITHERED_SOLID_UPDATE();
               case 2: DITHERED_SOLID(-2); s += sstep; t += tstep;
                       DITHERED_SOLID_B_UPDATE();
               case 1: DITHERED_SOLID_B(-1); s += sstep; t += tstep;
                       DITHERED_SOLID_UPDATE();
            }
         }

         s = snext;
         t = tnext;

      } while (count > 0);

   } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans8
=============
*/
void
D_DrawSpans8(espan_t *pspan)
{
   fixed16_t sstep = 0;			// keep compiler happy
   fixed16_t tstep = 0;			// ditto

   unsigned char *pbase = (unsigned char *)cacheblock;
   float sdivz8stepu    = d_sdivzstepu * 8;
   float tdivz8stepu    = d_tdivzstepu * 8;
   float zi8stepu       = d_zistepu * 8;

   do
   {
      fixed16_t t;
      uint8_t *pdest = (uint8_t*)((byte *)d_viewbuffer +
            (screenwidth * pspan->v) + pspan->u);

      int count = pspan->count;

      // calculate the initial s/z, t/z, 1/z, s, and t and clamp
      float du = (float)pspan->u;
      float dv = (float)pspan->v;

      float sdivz = d_sdivzorigin + dv * d_sdivzstepv + du * d_sdivzstepu;
      float tdivz = d_tdivzorigin + dv * d_tdivzstepv + du * d_tdivzstepu;
      float zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
      float z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

      fixed16_t s = (int)(sdivz * z) + sadjust;
      if (s > bbextents)
         s = bbextents;
      else if (s < 0)
         s = 0;

      t = (int)(tdivz * z) + tadjust;
      if (t > bbextentt)
         t = bbextentt;
      else if (t < 0)
         t = 0;

      do
      {
         fixed16_t snext, tnext;
         int spancount = count;
         /* calculate s and t at the far end of the span */
         if (count >= 8)
            spancount = 8;

         count -= spancount;

         if (count)
         {
            // calculate s/z, t/z, zi->fixed s and t at far end of span,
            // calculate s and t steps across span by shifting
            sdivz += sdivz8stepu;
            tdivz += tdivz8stepu;
            zi += zi8stepu;
            z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point

            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext < 8)
               snext = 8;	// prevent round-off error on <0 steps from
            //  from causing overstepping & running off the
            //  edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 8)
               tnext = 8;	// guard against round-off error on <0 steps

            sstep = (snext - s) >> 3;
            tstep = (tnext - t) >> 3;
         }
         else
         {
            // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
            // can't step off polygon), clamp, calculate s and t steps across
            // span by division, biasing steps low so we don't run off the
            // texture
            float spancountminus1 = (float)(spancount - 1);
            sdivz += d_sdivzstepu * spancountminus1;
            tdivz += d_tdivzstepu * spancountminus1;
            zi += d_zistepu * spancountminus1;
            z = (float)0x10000 / zi;	// prescale to 16.16 fixed-point
            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext < 8)
               snext = 8;	// prevent round-off error on <0 steps from
            //  from causing overstepping & running off the
            //  edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 8)
               tnext = 8;	// guard against round-off error on <0 steps

            if (spancount > 1) {
               sstep = (snext - s) / (spancount - 1);
               tstep = (tnext - t) / (spancount - 1);
            }
         }

         do
         {
            *pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
            s += sstep;
            t += tstep;
         } while (--spancount > 0);

         s = snext;
         t = tnext;

      } while (count);

   } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawSpans
=============
*/

void D_DrawSpans16 (espan_t *pspan) //qbism up it from 8 to 16.  This + unroll = big speed gain!
{
   fixed16_t sstep = 0;   // keep compiler happy
   fixed16_t tstep = 0;   // ditto

   uint8_t *pbase = (uint8_t*)cacheblock;

   float sdivzstepu = d_sdivzstepu * 16;
   float tdivzstepu = d_tdivzstepu * 16;
   float zistepu    = d_zistepu * 16;

   do
   {
      fixed16_t t;
      uint8_t *pdest = (uint8_t*)((byte *)d_viewbuffer +
            (screenwidth * pspan->v) + pspan->u);

      int count = pspan->count;

      // calculate the initial s/z, t/z, 1/z, s, and t and clamp
      float du = (float)pspan->u;
      float dv = (float)pspan->v;

      float sdivz = d_sdivzorigin + dv*d_sdivzstepv + du*d_sdivzstepu;
      float tdivz = d_tdivzorigin + dv*d_tdivzstepv + du*d_tdivzstepu;
      float zi = d_ziorigin + dv*d_zistepv + du*d_zistepu;
      float z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point
      fixed16_t s = (int)(sdivz * z) + sadjust;

      if (s > bbextents)
         s = bbextents;
      else if (s < 0)
         s = 0;

      t = (int)(tdivz * z) + tadjust;
      if (t > bbextentt)
         t = bbextentt;
      else if (t < 0)
         t = 0;

      do
      {
         fixed16_t snext, tnext;
         int spancount = count;

         // calculate s and t at the far end of the span
         if (count >= 16)
            spancount = 16;

         count -= spancount;

         if (count)
         {
            // calculate s/z, t/z, zi->fixed s and t at far end of span,
            // calculate s and t steps across span by shifting
            sdivz += sdivzstepu;
            tdivz += tdivzstepu;
            zi += zistepu;
            z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point

            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext <= 16)
               snext = 16;   // prevent round-off error on <0 steps from
            //  from causing overstepping & running off the
            //  edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 16)
               tnext = 16;   // guard against round-off error on <0 steps

            sstep = (snext - s) >> 4;
            tstep = (tnext - t) >> 4;
         }
         else
         {
            // calculate s/z, t/z, zi->fixed s and t at last pixel in span (so
            // can't step off polygon), clamp, calculate s and t steps across
            // span by division, biasing steps low so we don't run off the
            // texture
            float spancountminus1 = (float)(spancount - 1);
            sdivz += d_sdivzstepu * spancountminus1;
            tdivz += d_tdivzstepu * spancountminus1;
            zi += d_zistepu * spancountminus1;
            z = (float)0x10000 / zi;   // prescale to 16.16 fixed-point
            snext = (int)(sdivz * z) + sadjust;
            if (snext > bbextents)
               snext = bbextents;
            else if (snext < 16)
               snext = 16;   // prevent round-off error on <0 steps from
            //  from causing overstepping & running off the
            //  edge of the texture

            tnext = (int)(tdivz * z) + tadjust;
            if (tnext > bbextentt)
               tnext = bbextentt;
            else if (tnext < 16)
               tnext = 16;   // guard against round-off error on <0 steps

            if (spancount > 1)
            {
               sstep = (snext - s) / (spancount - 1);
               tstep = (tnext - t) / (spancount - 1);
            }
         }

         do {
            *pdest++ = *(pbase + (s >> 16) + (t >> 16) * cachewidth);
            s += sstep;
            t += tstep;
         } while (--spancount > 0);

         s = snext;
         t = tnext;

      } while (count > 0);

   } while ((pspan = pspan->pnext) != NULL);
}

/*
=============
D_DrawZSpans
=============
*/
void
D_DrawZSpans(espan_t *pspan)
{
   // FIXME: check for clamping/range problems
   // we count on FP exceptions being turned off to avoid range problems
   int izistep = (int)(d_zistepu * 0x8000 * 0x10000);

   do
   {
      int doublecount;
      int16_t *pdest = d_pzbuffer + (d_zwidth * pspan->v) + pspan->u;

      int count = pspan->count;

      // calculate the initial 1/z
      float du = (float)pspan->u;
      float dv = (float)pspan->v;

      double zi = d_ziorigin + dv * d_zistepv + du * d_zistepu;
      // we count on FP exceptions being turned off to avoid range problems
      int   izi = (int)(zi * 0x8000 * 0x10000);

      if ((int32_t)pdest & 0x02)
      {
         *pdest++ = (short)(izi >> 16);
         izi += izistep;
         count--;
      }

      if ((doublecount = count >> 1) > 0)
      {
         do
         {
#ifdef MSB_FIRST
            unsigned ltemp = izi & 0xFFFF0000;
            izi += izistep;
            ltemp |= izi >> 16;
#else
            unsigned ltemp = izi >> 16;
            izi += izistep;
            ltemp |= izi & 0xFFFF0000;
#endif
            izi += izistep;
            *(int *)pdest = ltemp;
            pdest += 2;
         } while (--doublecount > 0);
      }

      if (count & 1)
         *pdest = (short)(izi >> 16);

   } while ((pspan = pspan->pnext));
}
