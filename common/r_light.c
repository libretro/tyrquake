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
// r_light.c

#include "quakedef.h"
#include "r_local.h"

/*
==================
R_AnimateLight
==================
*/
void R_AnimateLight(void)
{
   int j, k;

   /* light animations
    * 'm' is normal light, 'a' is no light, 'z' is double bright */
   int i = (int)(cl.time * 10);

   for (j = 0; j < MAX_LIGHTSTYLES; j++)
   {
      if (!cl_lightstyle[j].length)
      {
         d_lightstylevalue[j] = 256;
         continue;
      }
      k = i % cl_lightstyle[j].length;
      k = cl_lightstyle[j].map[k] - 'a';
      k = k * 22;
      d_lightstylevalue[j] = k;
   }
}


/*
=============================================================================

DYNAMIC LIGHTS

=============================================================================
*/

/*
=============
R_MarkLights
=============
*/
void R_MarkLights (dlight_t *light, int num, mnode_t *node)  //qbism- adapted from MH tute - increased dlights
{
   mplane_t   *splitplane;
   float      dist;
#ifdef HAVE_FIXED_POINT
   int idist;
#endif
   msurface_t   *surf;
   int         i;

   if (node->contents < 0)
      return;

   splitplane = node->plane;
#ifdef HAVE_FIXED_POINT
   if (splitplane->type < 3)
      idist = light->iorigin[splitplane->type] - (splitplane->idist >> 16);
   else
      idist = (IIDotProduct (splitplane->inormal, light->iorigin) - splitplane->idist) >> 16;

   if (idist > light->iradius)
#else
   dist = DotProduct (light->origin, splitplane->normal) - splitplane->dist;

   if (dist > light->radius)
#endif
   {
      R_MarkLights (light, num, node->children[0]);
      return;
   }

#ifdef HAVE_FIXED_POINT
   if (idist < -light->iradius)
#else
   if (dist < -light->radius)
#endif
   {
      R_MarkLights (light, num, node->children[1]);
      return;
   }

   // mark the polygons
   surf = cl.worldmodel->surfaces + node->firstsurface;

   for (i = 0; i < node->numsurfaces; i++, surf++)
   {
      if (surf->dlightframe != r_framecount)
      {
         memset (surf->dlightbits, 0, sizeof (surf->dlightbits));
         surf->dlightframe = r_framecount;
      }

      surf->dlightbits[num >> 5] |= 1 << (num & 31);
   }

   R_MarkLights (light, num, node->children[0]);
   R_MarkLights (light, num, node->children[1]);
}


/*
=============
R_PushDlights
=============
*/
void R_PushDlights(mnode_t *headnode) /* qbism- from MH tute - increased dlights */
{
    int i;
    dlight_t *l = cl_dlights;

    for (i = 0; i < MAX_DLIGHTS; i++, l++)
    {
       if (l->die < cl.time)
          continue;
#ifdef HAVE_FIXED_POINT
       if (!l->iradius)
          continue;
#else
       if (l->radius <= 0)
          continue;
#endif

       R_MarkLights(l, i, headnode);
    }
}


/*
=============================================================================

LIGHT SAMPLING

=============================================================================
*/

#ifdef HAVE_FIXED_POINT
#define ISURFACE_CLIP_EPSILON   (2048L)

#define HULLCHECKSTATE_EMPTY 0
#define HULLCHECKSTATE_SOLID 1
#define HULLCHECKSTATE_DONE 2

int RecursiveLightPoint_f (mnode_t      *node, int p1f, int p2f, int* p1, int* p2)
{
   mnode_t *split;
   mplane_t        *plane;
   int             t[2];
   int             frac, frac2;
   int             mid[3], mid2[3];
   int                     side, ret, onplane;
   int             midf, midf2, adjf;
   mtexinfo_t      *tex;
   byte            *lightmap;
   unsigned        scale;
   int                     i,maps;
   bmodel_t        *model;
   int *u,*v;
   int x,y,w,h,c,size,xs,ys,ss,tt;
   msurface_t      *surf;
   int pt[3];
   int                     r, ds, dt;


   split = 0;
   //find the split node
   do {
      //found a leaf
      if(node->contents)
      {
         //do something???
         //putParticleLine(p1,p2,12*16);
         if (node->contents != CONTENTS_SOLID)
         {
            return -1;              // empty
         }
         else
         {
            return -2;              //solid
         }
      }

      //
      // find the point distances
      //
      //node = hull->clipnodes + num;
      plane = node->plane;

      if (plane->type < 3)
      {
         t[0] = p1[plane->type] - plane->idist;
         t[1] = p2[plane->type] - plane->idist;
      }
      else
      {
         t[0] = IDotProduct (plane->inormal, p1) - plane->idist;
         t[1] = IDotProduct (plane->inormal, p2) - plane->idist;
      }
      //both in front
      if (t[0] >= 0 && t[1] >= 0)
      {
         node = node->children[0];
         continue;
      }

      //both behind
      if (t[0] < 0 && t[1] < 0)
      {
         node = node->children[1];
         continue;
      }

      split = node;
      //we have a split
      break;
   } while(1);

   // put the crosspoint SURFACE_CLIP_EPSILON pixels on the near side
   onplane = 0;
   if ( t[0] < t[1] ) {
      //idist = 1.0/(t[0]-t[1]);
      side = 1;
      //frac = frac2 = t[0]*idist;
      frac = frac2 = idiv64(t[0],t[0]-t[1]);
      adjf = idiv64(ISURFACE_CLIP_EPSILON,(t[1]-t[0]));
   } else if (t[0] > t[1]) {
      //idist = 1.0/(t[0]-t[1]);
      side = 0;
      //frac = frac2 = t[0]*idist;
      frac = frac2 = idiv64(t[0],t[0]-t[1]);
      adjf = idiv64(ISURFACE_CLIP_EPSILON,(t[0]-t[1]));
   } else {
      side = 0;
      frac = 1<<16;
      frac2 = 0;
      //adjf = adj[0] = adj[1] = adj[2] = 0.0f;
      adjf = 0;
      //this is a point
      //do something special???
   }

   frac -= adjf;

   // move up to the node
   if ( frac < 0 ) {
      frac = 0;
   }
   if ( frac > (1<<16) ) {
      frac = (1<<16);
   }

   midf = p1f + imul64((p2f - p1f),frac);

   mid[0] = p1[0] + imul64((frac),(p2[0] - p1[0]));
   mid[1] = p1[1] + imul64((frac),(p2[1] - p1[1]));
   mid[2] = p1[2] + imul64((frac),(p2[2] - p1[2]));

   ret = RecursiveLightPoint_f( node->children[side], p1f, midf, p1, mid );
   if(ret != -1)
      return ret;


   frac2 += adjf;

   // go past the node
   if ( frac2 < 0 ) {
      frac2 = 0;
   }
   if ( frac2 > (1<<16) ) {
      frac2 = (1<<16);
   }

   midf2 = p1f + imul64((p2f - p1f),frac2);

   mid2[0] = p1[0] + imul64((frac2),(p2[0] - p1[0]));
   mid2[1] = p1[1] + imul64((frac2),(p2[1] - p1[1]));
   mid2[2] = p1[2] + imul64((frac2),(p2[2] - p1[2]));


   ret = RecursiveLightPoint_f( node->children[side^1], midf, p2f, mid, p2 );
   //if(ret != 1)
   if(ret != -2)
      return ret;
   /*      
           if (!side)
           {
           VectorCopy (plane->normal, trace->plane.normal);
           trace->plane.dist = plane->dist;
           trace->ifraction = midf;
           IVectorCopy (mid, trace->iendpos);
           }
           else
           {
           VectorSubtract (vec3_origin, plane->normal, trace->plane.normal);
           trace->plane.dist = -plane->dist;
           trace->ifraction = midf;
           IVectorCopy (mid, trace->iendpos);
           }*/

      pt[0] = mid[0]>>14;
   pt[1] = mid[1]>>14;
   pt[2] = mid[2]>>14;

   model = (bmodel_t *)cl.worldmodel->cache.data;
   surf = model->surfaces + node->firstsurface;
   for (i=0 ; i<node->numsurfaces ; i++, surf++)
   {
      if (surf->flags & SURF_DRAWTILED)
         continue;       // no lightmaps

      tex = surf->texinfo;
      ss = surf->texturemins[0];
      tt = surf->texturemins[1];
      ss <<= 4;
      tt <<= 4;
      xs = (tex->texture->width<<16)/tex->texture->ds.width;
      ys = (tex->texture->height<<16)/tex->texture->ds.height;
      u = tex->ivecs[0];
      v = tex->ivecs[1];

      x = CALC_COORD(pt,u);
      y = CALC_COORD(pt,v);
      x = (x-ss)>>4;
      y = (y-tt)>>4;
      x = (x * xs)>>16;//20;
      y = (y * ys)>>16;//20;

      /*              s = DotProduct (mid, tex->vecs[0]) + tex->vecs[0][3];
                      t = DotProduct (mid, tex->vecs[1]) + tex->vecs[1][3];;

                      if (s < surf->texturemins[0] ||
                      t < surf->texturemins[1])
                      continue;

                      ds = s - surf->texturemins[0];
                      dt = t - surf->texturemins[1];

                      if ( ds > surf->extents[0] || dt > surf->extents[1] )
                      continue;

*/              
      if(x < 0 || y < 0)
      {
         continue;
      }
      if ( x > surf->extents[0] || y > surf->extents[1] )
      {
         continue;
      }
      if (!surf->samples)
         return 0;

      ds = x >> 4;
      dt = y >> 4;

      lightmap = surf->samples;
      r = 0;
      if (lightmap)
      {

         lightmap += dt * ((surf->extents[0]>>4)+1) + ds;

         for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
               maps++)
         {
            scale = d_lightstylevalue[surf->styles[maps]];
            r += *lightmap * scale;
            lightmap += ((surf->extents[0]>>4)+1) *
               ((surf->extents[1]>>4)+1);
         }

         r >>= 8;
      }

      return r;
   }

   return -3;
}
#endif

int RecursiveLightPoint(mnode_t *node, vec3_t start, vec3_t end)
{
   int r;
   float front, back, frac;
   int side;
   mplane_t *plane;
   vec3_t mid;
   msurface_t *surf;
   int s, t, ds, dt;
   int i;
   mtexinfo_t *tex;
   byte *lightmap;
   unsigned scale;
   int maps;

   if (node->contents < 0)
      return -1;		// didn't hit anything

   // calculate mid point

   plane = node->plane;
   switch (plane->type) {
      case PLANE_X:
      case PLANE_Y:
      case PLANE_Z:
         front = start[plane->type - PLANE_X] - plane->dist;
         back = end[plane->type - PLANE_X] - plane->dist;
         break;
      default:
         front = DotProduct(start, plane->normal) - plane->dist;
         back = DotProduct(end, plane->normal) - plane->dist;
         break;
   }
   side = front < 0;

   /* FIXME - tail recursion => optimize */
   if ((back < 0) == side)
      return RecursiveLightPoint(node->children[side], start, end);

   frac = front / (front - back);
   mid[0] = start[0] + (end[0] - start[0]) * frac;
   mid[1] = start[1] + (end[1] - start[1]) * frac;
   mid[2] = start[2] + (end[2] - start[2]) * frac;

   // go down front side
   r = RecursiveLightPoint(node->children[side], start, mid);
   if (r >= 0)
      return r;		// hit something

   if ((back < 0) == side)
      return -1;		// didn't hit anuthing

   // check for impact on this node

   surf = cl.worldmodel->surfaces + node->firstsurface;
   for (i = 0; i < node->numsurfaces; i++, surf++) {
      if (surf->flags & SURF_DRAWTILED)
         continue;		// no lightmaps

      tex = surf->texinfo;

      s = DotProduct(mid, tex->vecs[0]) + tex->vecs[0][3];
      t = DotProduct(mid, tex->vecs[1]) + tex->vecs[1][3];;

      if (s < surf->texturemins[0] || t < surf->texturemins[1])
         continue;

      ds = s - surf->texturemins[0];
      dt = t - surf->texturemins[1];

      if (ds > surf->extents[0] || dt > surf->extents[1])
         continue;

      if (!surf->samples)
         return 0;

      ds >>= 4;
      dt >>= 4;

      /* FIXME: does this account properly for dynamic lights? e.g. rocket */
      lightmap = surf->samples;
      r = 0;
      if (lightmap) {
         lightmap += dt * ((surf->extents[0] >> 4) + 1) + ds;
         for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255;
               maps++) {
            scale = d_lightstylevalue[surf->styles[maps]];
            r += *lightmap * scale;
            lightmap += ((surf->extents[0] >> 4) + 1) *
               ((surf->extents[1] >> 4) + 1);
         }
         r >>= 8;
      }

      return r;
   }

   /* FIXME - tail recursion => optimize */
   /* go down back side */
   return RecursiveLightPoint(node->children[!side], mid, end);
}

/*
 * FIXME - check what the callers do, but I don't think this will check the
 * light value of a bmodel below the point. Models could easily be standing on
 * a func_plat or similar...
 */
int R_LightPoint(vec3_t p)
{
   vec3_t end;
#ifdef HAVE_FIXED_POINT
   int end2[3], p2[3];
   int i, r2;
#endif
   int r;

   if (!cl.worldmodel->lightdata)
      return 255;

   end[0] = p[0];
   end[1] = p[1];
   end[2] = p[2] - (8192 + 2); /* Max distance + error margin */

#ifdef HAVE_FIXED_POINT
   for (i = 0; i < 3; i++)
   {
      p2[i] = p[i]*(1<<16);
      end2[i] = end[i]*(1<<16);
   }

   r = RecursiveLightPoint_f (model->nodes, 0,(1<<16), p2, end2);

   if (r < 0)
#else
   r = RecursiveLightPoint(cl.worldmodel->nodes, p, end);

   if (r == -1)
#endif
      r = 0;

   if (r < r_refdef.ambientlight)
      r = r_refdef.ambientlight;

   return r;
}
