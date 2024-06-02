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
// r_surf.c: surface-related refresh code

#include <stdint.h>

#include "quakedef.h"
#include "r_local.h"
#include "sys.h"

drawsurf_t r_drawsurf;

int lightleft, sourcesstep, blocksize, sourcetstep;
int lightdelta, lightdeltastep;
int lightright, lightleftstep, lightrightstep, blockdivshift;

int				lightlefta[3];
int				sourcesstep, blocksize, sourcetstep;
int				lightrighta[3];
int				lightleftstepa[3], lightrightstepa[3], blockdivshift;

unsigned blockdivmask;
void *prowdestbase;
unsigned char *pbasesource;
int surfrowbytes;		// used by ASM files
//unsigned *r_lightptr;
int *r_lightptr;

int r_stepback;
int r_lightwidth;
unsigned char *r_source, *r_sourcemax;

static int r_numhblocks;
int r_numvblocks;

void R_DrawSurfaceBlock8_mip0(void);
void R_DrawSurfaceBlock8_mip1(void);
void R_DrawSurfaceBlock8_mip2(void);
void R_DrawSurfaceBlock8_mip3(void);

void R_DrawSurfaceBlockRGB_mip0(void);
void R_DrawSurfaceBlockRGB_mip1(void);
void R_DrawSurfaceBlockRGB_mip2(void);
void R_DrawSurfaceBlockRGB_mip3(void);


static void (*surfmiptable[4]) (void) = {
    R_DrawSurfaceBlock8_mip0,
    R_DrawSurfaceBlock8_mip1,
    R_DrawSurfaceBlock8_mip2,
    R_DrawSurfaceBlock8_mip3
};

static void	(*surfmiptableRGB[4])(void) =
{
	R_DrawSurfaceBlockRGB_mip0,
	R_DrawSurfaceBlockRGB_mip1,
	R_DrawSurfaceBlockRGB_mip2,
	R_DrawSurfaceBlockRGB_mip3
};

//static unsigned blocklights[18 * 18 * 3];
int		blocklights[18*18*3]; // LordHavoc: .lit support (*3 for RGB)

// Leilei - macros to make colored lighting code look a little more bearable to sanity
// Macros for initiating the RGB light deltas.
#define MakeLightDelta() { light[0] =  lightrighta[0];	light[1] =  lightrighta[1];	light[2] =  lightrighta[2];};
#define PushLightDelta() { light[0] += lightdelta[0];	light[1] += lightdelta[1];	light[2] += lightdelta[2]; };
#define FinishLightDelta() { psource += sourcetstep; lightrighta[0] += lightrightstepa[0];lightlefta[0] += lightleftstepa[0];lightdelta[0] += lightdeltastep[0]; lightrighta[1] += lightrightstepa[1];lightlefta[1] += lightleftstepa[1];lightdelta[1] += lightdeltastep[1]; lightrighta[2] += lightrightstepa[2];lightlefta[2] += lightleftstepa[2];lightdelta[2] += lightdeltastep[2]; prowdest += surfrowbytes;}
#define MIPRGB(i) {  	if (psource[i] < host_fullbrights){ 	pix = psource[i]; pix24 = (unsigned char *)&d_8to24table[pix]; trans[0] = (pix24[0] * (light[0])) >> 17; trans[1] = (pix24[1] * (light[1])) >> 17; trans[2] = (pix24[2] * (light[2])) >> 17; if (trans[0] & ~63) trans[0] = 63; if (trans[1] & ~63) trans[1] = 63; if (trans[2] & ~63) trans[2] = 63; prowdest[i] = palmap2[trans[0]][trans[1]][trans[2]]; }	else prowdest[i] = psource[i];}
#define Mip0Stuff(i) { MakeLightDelta(); i(15); PushLightDelta(); i(14); PushLightDelta(); PushLightDelta(); i(13); PushLightDelta(); i(12); PushLightDelta(); i(11); PushLightDelta(); i(10); PushLightDelta(); i(9); PushLightDelta(); i(8); PushLightDelta(); i(7); PushLightDelta(); i(6); PushLightDelta(); i(5); PushLightDelta(); i(4); PushLightDelta(); i(3); PushLightDelta(); i(2); PushLightDelta(); i(1); PushLightDelta(); i(0);  FinishLightDelta();}
#define Mip1Stuff(i) { MakeLightDelta(); i(7); PushLightDelta(); i(6); PushLightDelta(); i(5); PushLightDelta(); i(4); PushLightDelta(); i(3); PushLightDelta(); i(2); PushLightDelta(); i(1); PushLightDelta(); i(0); FinishLightDelta();}
#define Mip2Stuff(i) { MakeLightDelta();i(3); PushLightDelta(); i(2); PushLightDelta(); i(1); PushLightDelta(); i(0); FinishLightDelta();}
#define Mip3Stuff(i) { MakeLightDelta(); i(1); PushLightDelta(); i(0); FinishLightDelta();}

int			host_fullbrights;   // for preserving fullbrights in color operations


/*
===============
R_AddDynamicLights
===============
*/
static void R_AddDynamicLights(void)
{
   msurface_t *surf;
   int lnum;
   int sd, td;
   float dist, rad, minlight;
   vec3_t impact, local;
   int s, t;
   int i;
   int smax, tmax;
   mtexinfo_t *tex;

   surf = r_drawsurf.surf;
   smax = (surf->extents[0] >> 4) + 1;
   tmax = (surf->extents[1] >> 4) + 1;
   tex = surf->texinfo;

   for (lnum = 0; lnum < MAX_DLIGHTS; lnum++) {
      if (!(surf->dlightbits[lnum >> 5] & (1 << (lnum & 31)))) continue;  //qbism from MH

      rad = cl_dlights[lnum].radius;
      dist = DotProduct(cl_dlights[lnum].origin, surf->plane->normal) -
         surf->plane->dist;
      rad -= fabs(dist);
      minlight = cl_dlights[lnum].minlight;
      if (rad < minlight)
         continue;
      minlight = rad - minlight;

      for (i = 0; i < 3; i++) {
         impact[i] = cl_dlights[lnum].origin[i] -
            surf->plane->normal[i] * dist;
      }

      local[0] = DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3];
      local[1] = DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3];

      local[0] -= surf->texturemins[0];
      local[1] -= surf->texturemins[1];

      for (t = 0; t < tmax; t++) {
         td = local[1] - t * 16;
         if (td < 0)
            td = -td;
         for (s = 0; s < smax; s++) {
            sd = local[0] - s * 16;
            if (sd < 0)
               sd = -sd;
            if (sd > td)
               dist = sd + (td >> 1);
            else
               dist = td + (sd >> 1);
            if (dist < minlight)
               blocklights[t * smax + s] += (rad - dist) * 256;
         }
      }
   }
}

static void R_AddDynamicLightsRGB(void)
{
   msurface_t *surf;
   int lnum;
   int sd, td;
   float dist, rad, minlight;
   vec3_t impact, local;
   int s, t;
   int i;
   int smax, tmax;
   mtexinfo_t *tex;
   float		cred, cgreen, cblue, brightness;
   int *bl;

   surf = r_drawsurf.surf;
   smax = (surf->extents[0] >> 4) + 1;
   tmax = (surf->extents[1] >> 4) + 1;
   tex = surf->texinfo;

   for (lnum = 0; lnum < MAX_DLIGHTS; lnum++) {
      if (!(surf->dlightbits[lnum >> 5] & (1 << (lnum & 31)))) continue;  //qbism from MH

      rad = cl_dlights[lnum].radius;
      dist = DotProduct(cl_dlights[lnum].origin, surf->plane->normal) -
         surf->plane->dist;
      rad -= fabs(dist);
      minlight = cl_dlights[lnum].minlight;
      if (rad < minlight)
         continue;
      minlight = rad - minlight;

      for (i = 0; i < 3; i++) {
         impact[i] = cl_dlights[lnum].origin[i] -
            surf->plane->normal[i] * dist;
      }

      local[0] = DotProduct(impact, tex->vecs[0]) + tex->vecs[0][3];
      local[1] = DotProduct(impact, tex->vecs[1]) + tex->vecs[1][3];

      local[0] -= surf->texturemins[0];
      local[1] -= surf->texturemins[1];

      cred = cl_dlights[lnum].color[0] * 256.0f * 2;
      cgreen = cl_dlights[lnum].color[1] * 256.0f* 2;
      cblue = cl_dlights[lnum].color[2] * 256.0f* 2;

      bl = blocklights;

		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
				{
					brightness = rad - dist;
					bl[0] += (int) (brightness * cred);
					bl[1] += (int) (brightness * cgreen);
					bl[2] += (int) (brightness * cblue);
				}
				bl += 3;
			}
		}
 	}  
}


/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
static void R_BuildLightMap(void)
{
   int i;
   unsigned scale;
   int maps;
   msurface_t *surf = r_drawsurf.surf;
   int smax = (surf->extents[0] >> 4) + 1;
   int tmax = (surf->extents[1] >> 4) + 1;
   int size = smax * tmax;
   byte *lightmap = surf->samples;

   if (r_fullbright.value || !cl.worldmodel->lightdata)
   {
      for (i = 0; i < size; i++)
         blocklights[i] = 0;
      return;
   }

   /* clear to ambient */
   for (i = 0; i < size; i++)
      blocklights[i] = r_refdef.ambientlight << 8;


   // add all the lightmaps
   if (lightmap)
      for (maps = 0; maps < MAXLIGHTMAPS && surf->styles[maps] != 255;
            maps++)
      {
         scale = r_drawsurf.lightadj[maps];	// 8.8 fraction
         for (i = 0; i < size; i++)
            blocklights[i] += lightmap[i] * scale;
         lightmap += size;	// skip to next lightmap
      }
   // add all the dynamic lights
   if (surf->dlightframe == r_framecount)
      R_AddDynamicLights();

   // bound, invert, and shift
   for (i = 0; i < size; i++)
   {
      int t = (255 * 256 - (int)blocklights[i]) >> (8 - VID_CBITS);

      if (t < (1 << 6))
         t = (1 << 6);

      blocklights[i] = t;
   }
}


void R_BuildLightMapRGB (void)
{
	int			smax, tmax;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;
	int r;
	int sample;
	int	shifted;
		shifted = 0;
		sample = 65536;

	surf = r_drawsurf.surf;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax*3;
	lightmap = surf->samples;

	if (/* r_fullbright.value || */ !cl.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 0;
		return;
	}

// clear to ambient
	for (i=0 ; i<size ; i++)
		blocklights[i] = r_refdef.ambientlight<<8;


// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fraction		
			for (i=0 ; i<size ; i+=3)
			{
				blocklights[i]		+= lightmap[i] * scale;
				blocklights[i+1]	+= lightmap[i+1] * scale;
				blocklights[i+2]	+= lightmap[i+2] * scale;
			}
			lightmap += size;	// skip to next lightmap
		}

// add all the dynamic lights
			 if (surf->dlightframe == r_framecount)
					R_AddDynamicLightsRGB ();

		 			 

	
// bound, invert, and shift
/*
	{
	int t, re;
	for (i=0 ; i<size ; i++)
		{
			
			t = blocklights[i] / 2;
			if (t < 1024)
				t = 1024;
			if (t > sample)
				t = sample;
			t = t >> (8 - VID_CBITS);
	
			if (t < (1 << 6))
				t = (1 << 6);
			
	
			
			r = t;
			blocklights[i] = r;
		
		}
	}
*/
		for (i=0 ; i<size ; i++)
	{
		r = blocklights[i] >> shifted;
		blocklights[i] = (r < 256) ? 256 : (r > sample) ? sample : r;	// leilei - made min 256 to rid visual artifacts and gain speed
	}

	
}



/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation(const entity_t *e, texture_t *base)
{
   int reletive;
   int count;

   if (e->frame)
   {
      if (base->alternate_anims)
         base = base->alternate_anims;
   }

   if (!base->anim_total)
      return base;

   reletive = (int)(cl.time * 10) % base->anim_total;

   count = 0;
   while (base->anim_min > reletive || base->anim_max <= reletive) {
      base = base->anim_next;
      if (!base)
         Sys_Error("%s: broken cycle", __func__);
      if (++count > 100)
         Sys_Error("%s: infinite cycle", __func__);
   }

   return base;
}

/*
===============
R_DrawSurface
===============
*/

extern int coloredlights;

void R_DrawSurface(void)
{
   unsigned char *basetptr;
   int smax, tmax, twidth;
   int u;
   int soffset, basetoffset, texwidth;
   int horzblockstep;
   unsigned char *pcolumndest;
   void (*pblockdrawer) (void);
   texture_t *mt;

   // calculate the lightings
   
   if (coloredlights)
      R_BuildLightMapRGB();
   else
      R_BuildLightMap();


   surfrowbytes = r_drawsurf.rowbytes;

   mt = r_drawsurf.texture;

   r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];

   // the fractional light values should range from 0 to (VID_GRADES - 1) << 16
   // from a source range of 0 - 255

   texwidth = mt->width >> r_drawsurf.surfmip;

   blocksize = 16 >> r_drawsurf.surfmip;
   blockdivshift = 4 - r_drawsurf.surfmip;
   blockdivmask = (1 << blockdivshift) - 1;

   r_lightwidth = (r_drawsurf.surf->extents[0] >> 4) + 1;

   r_numhblocks = r_drawsurf.surfwidth >> blockdivshift;
   r_numvblocks = r_drawsurf.surfheight >> blockdivshift;

   //==============================

   if (coloredlights)
	   pblockdrawer = surfmiptableRGB[r_drawsurf.surfmip]; // 18-bit lookups
   else
	   pblockdrawer = surfmiptable[r_drawsurf.surfmip];

   // TODO: only needs to be set when there is a display settings change
   horzblockstep = blocksize;

   smax = mt->width >> r_drawsurf.surfmip;
   twidth = texwidth;
   tmax = mt->height >> r_drawsurf.surfmip;
   sourcetstep = texwidth;
   r_stepback = tmax * twidth;

   r_sourcemax = r_source + (tmax * smax);

   soffset = r_drawsurf.surf->texturemins[0];
   basetoffset = r_drawsurf.surf->texturemins[1];

   // << 16 components are to guarantee positive values for %
   soffset = ((soffset >> r_drawsurf.surfmip) + (smax << 16)) % smax;
   basetptr = &r_source[((((basetoffset >> r_drawsurf.surfmip)
               + (tmax << 16)) % tmax) * twidth)];

   pcolumndest = r_drawsurf.surfdat;
	//horzblockstep *= 2;	// CAUSES CRASH

   for (u = 0; u < r_numhblocks; u++) {
      if (coloredlights)
         r_lightptr = blocklights + u * 3;
      else
         r_lightptr = blocklights + u;

      prowdestbase = pcolumndest;

      pbasesource = basetptr + soffset;

      (*pblockdrawer) ();

      soffset = soffset + blocksize;
      if (soffset >= smax)
         soffset = 0;

      pcolumndest += horzblockstep;
   }
}


//=============================================================================

// 18-bit version
// Unrolled 

void R_DrawSurfaceBlockRGB_mip0()
{
	unsigned int				v, i; 
	unsigned int light[3];
	unsigned int lightdelta[3], lightdeltastep[3];
	unsigned char	pix, *psource, *prowdest;
	unsigned char *pix24;
	unsigned trans[3];
	psource = pbasesource;
	prowdest = prowdestbase;


	for (v=0 ; v<r_numvblocks ; v++)
	{
		lightlefta[0] = r_lightptr[0];
		lightrighta[0] = r_lightptr[3];
		lightlefta[1] = r_lightptr[0+1];
		lightrighta[1] = r_lightptr[3+1];
		lightlefta[2] = r_lightptr[0+2];
		lightrighta[2] = r_lightptr[3+2];

		lightdelta[0] = (lightlefta[0] - lightrighta[0])  >> 4; 
		lightdelta[1] = (lightlefta[1] - lightrighta[1])  >> 4;  
		lightdelta[2] = (lightlefta[2] - lightrighta[2])  >> 4; 


		r_lightptr += r_lightwidth * 3;

		lightleftstepa[0] = (r_lightptr[0] - lightlefta[0]) >> 4;
		lightrightstepa[0] = (r_lightptr[3] - lightrighta[0]) >> 4;

		lightleftstepa[1] = (r_lightptr[0+1] - lightlefta[1]) >> 4;
		lightrightstepa[1] = (r_lightptr[3+1] - lightrighta[1]) >> 4;

		lightleftstepa[2] = (r_lightptr[0+2] - lightlefta[2]) >> 4;
		lightrightstepa[2] = (r_lightptr[3+2] - lightrighta[2]) >> 4;

		lightdeltastep[0] = (lightleftstepa[0] - lightrightstepa[0]) >> 4;
		lightdeltastep[1] = (lightleftstepa[1] - lightrightstepa[1]) >> 4;
		lightdeltastep[2] = (lightleftstepa[2] - lightrightstepa[2]) >> 4;


		for (i=0 ; i<16 ; i++)
		{
			Mip0Stuff(MIPRGB);
		
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
		
	}
}




void R_DrawSurfaceBlockRGB_mip1()
{
	unsigned int				v, i; 
	unsigned int light[3];
	unsigned int lightdelta[3], lightdeltastep[3];
	unsigned char	pix, *psource, *prowdest;
	unsigned char *pix24;
	unsigned trans[3];
	psource = pbasesource;
	
	prowdest = prowdestbase;

	
	for (v=0 ; v<r_numvblocks ; v++)
	{
		lightlefta[0] = r_lightptr[0];
		lightrighta[0] = r_lightptr[3];
		lightlefta[1] = r_lightptr[0+1];
		lightrighta[1] = r_lightptr[3+1];
		lightlefta[2] = r_lightptr[0+2];
		lightrighta[2] = r_lightptr[3+2];

		lightdelta[0] = (lightlefta[0] - lightrighta[0])  >> 3; 
		lightdelta[1] = (lightlefta[1] - lightrighta[1])  >> 3;  
		lightdelta[2] = (lightlefta[2] - lightrighta[2])  >> 3; 


		r_lightptr += r_lightwidth * 3;

		lightleftstepa[0] = (r_lightptr[0] - lightlefta[0]) >> 3;
		lightrightstepa[0] = (r_lightptr[3] - lightrighta[0]) >> 3;

		lightleftstepa[1] = (r_lightptr[0+1] - lightlefta[1]) >> 3;
		lightrightstepa[1] = (r_lightptr[3+1] - lightrighta[1]) >> 3;

		lightleftstepa[2] = (r_lightptr[0+2] - lightlefta[2]) >> 3;
		lightrightstepa[2] = (r_lightptr[3+2] - lightrighta[2]) >> 3;

		lightdeltastep[0] = (lightleftstepa[0] - lightrightstepa[0]) >> 3;
		lightdeltastep[1] = (lightleftstepa[1] - lightrightstepa[1]) >> 3;
		lightdeltastep[2] = (lightleftstepa[2] - lightrightstepa[2]) >> 3;

		for (i=0 ; i<8 ; i++)
		{
			Mip1Stuff(MIPRGB);


		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
		
	}
}



void R_DrawSurfaceBlockRGB_mip2()
{
	unsigned int				v, i; 
	unsigned int light[3];
	unsigned int lightdelta[3], lightdeltastep[3];
	unsigned char	pix, *psource, *prowdest;
	unsigned char *pix24;
	unsigned trans[3];
	psource = pbasesource;
	
	prowdest = prowdestbase;

	
	for (v=0 ; v<r_numvblocks ; v++)
	{
		lightlefta[0] = r_lightptr[0];
		lightrighta[0] = r_lightptr[3];
		lightlefta[1] = r_lightptr[0+1];
		lightrighta[1] = r_lightptr[3+1];
		lightlefta[2] = r_lightptr[0+2];
		lightrighta[2] = r_lightptr[3+2];

		lightdelta[0] = (lightlefta[0] - lightrighta[0])  >> 2; 
		lightdelta[1] = (lightlefta[1] - lightrighta[1])  >> 2;  
		lightdelta[2] = (lightlefta[2] - lightrighta[2])  >> 2; 


		r_lightptr += r_lightwidth * 3;

		lightleftstepa[0] = (r_lightptr[0] - lightlefta[0]) >> 2;
		lightrightstepa[0] = (r_lightptr[3] - lightrighta[0]) >> 2;

		lightleftstepa[1] = (r_lightptr[0+1] - lightlefta[1]) >> 2;
		lightrightstepa[1] = (r_lightptr[3+1] - lightrighta[1]) >> 2;

		lightleftstepa[2] = (r_lightptr[0+2] - lightlefta[2]) >> 2;
		lightrightstepa[2] = (r_lightptr[3+2] - lightrighta[2]) >> 2;

		lightdeltastep[0] = (lightleftstepa[0] - lightrightstepa[0]) >> 2;
		lightdeltastep[1] = (lightleftstepa[1] - lightrightstepa[1]) >> 2;
		lightdeltastep[2] = (lightleftstepa[2] - lightrightstepa[2]) >> 2;

		for (i=0 ; i<4 ; i++)
		{
			Mip2Stuff(MIPRGB);


		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
		
	}
}


void R_DrawSurfaceBlockRGB_mip3()
{
	unsigned int				v, i; 
	unsigned int light[3];
	unsigned int lightdelta[3], lightdeltastep[3];
	unsigned char	pix, *psource, *prowdest;
	unsigned char *pix24;
	unsigned trans[3];
	psource = pbasesource;
	
	prowdest = prowdestbase;

	
	for (v=0 ; v<r_numvblocks ; v++)
	{
		lightlefta[0] = r_lightptr[0];
		lightrighta[0] = r_lightptr[3];
		lightlefta[1] = r_lightptr[0+1];
		lightrighta[1] = r_lightptr[3+1];
		lightlefta[2] = r_lightptr[0+2];
		lightrighta[2] = r_lightptr[3+2];

		lightdelta[0] = (lightlefta[0] - lightrighta[0])  >> 1; 
		lightdelta[1] = (lightlefta[1] - lightrighta[1])  >> 1;  
		lightdelta[2] = (lightlefta[2] - lightrighta[2])  >> 1; 


		r_lightptr += r_lightwidth * 3;

		lightleftstepa[0] = (r_lightptr[0] - lightlefta[0]) >> 1;
		lightrightstepa[0] = (r_lightptr[3] - lightrighta[0]) >> 1;

		lightleftstepa[1] = (r_lightptr[0+1] - lightlefta[1]) >> 1;
		lightrightstepa[1] = (r_lightptr[3+1] - lightrighta[1]) >> 1;

		lightleftstepa[2] = (r_lightptr[0+2] - lightlefta[2]) >> 1;
		lightrightstepa[2] = (r_lightptr[3+2] - lightrighta[2]) >> 1;

		lightdeltastep[0] = (lightleftstepa[0] - lightrightstepa[0]) >> 1;
		lightdeltastep[1] = (lightleftstepa[1] - lightrightstepa[1]) >> 1;
		lightdeltastep[2] = (lightleftstepa[2] - lightrightstepa[2]) >> 1;

		for (i=0 ; i<2 ; i++)
		{
			Mip3Stuff(MIPRGB);

	
		}

		if (psource >= r_sourcemax)
			psource -= r_stepback;
		
	}
}




/*
================
R_DrawSurfaceBlock8_mip0
================
*/
void R_DrawSurfaceBlock8_mip0(void)
{
   int v, i, b, lightstep, lighttemp, light;
   unsigned char pix;
   unsigned char *psource = pbasesource;
   unsigned char *prowdest = (unsigned char*)prowdestbase;

   for (v = 0; v < r_numvblocks; v++)
   {
      // FIXME: make these locals?
      // FIXME: use delta rather than both right and left, like ASM?
      lightleft = r_lightptr[0];
      lightright = r_lightptr[1];
      r_lightptr += r_lightwidth;
      lightleftstep = (r_lightptr[0] - lightleft) >> 4;
      lightrightstep = (r_lightptr[1] - lightright) >> 4;

      for (i = 0; i < 16; i++) {
         lighttemp = lightleft - lightright;
         lightstep = lighttemp >> 4;

         light = lightright;

         for (b = 15; b >= 0; b--)
         {
            pix = psource[b];
            prowdest[b] = ((unsigned char *)vid.colormap)
               [(light & 0xFF00) + pix];
            light += lightstep;
         }

         psource += sourcetstep;
         lightright += lightrightstep;
         lightleft += lightleftstep;
         prowdest += surfrowbytes;
      }

      if (psource >= r_sourcemax)
         psource -= r_stepback;
   }
}


/*
================
R_DrawSurfaceBlock8_mip1
================
*/
void R_DrawSurfaceBlock8_mip1(void)
{
   int v, i, b, lightstep, lighttemp, light;
   unsigned char pix;
   unsigned char *psource = pbasesource;
   unsigned char *prowdest = (unsigned char*)prowdestbase;

   for (v = 0; v < r_numvblocks; v++)
   {
      // FIXME: make these locals?
      // FIXME: use delta rather than both right and left, like ASM?
      lightleft = r_lightptr[0];
      lightright = r_lightptr[1];
      r_lightptr += r_lightwidth;
      lightleftstep = (r_lightptr[0] - lightleft) >> 3;
      lightrightstep = (r_lightptr[1] - lightright) >> 3;

      for (i = 0; i < 8; i++) {
         lighttemp = lightleft - lightright;
         lightstep = lighttemp >> 3;

         light = lightright;

         for (b = 7; b >= 0; b--) {
            pix = psource[b];
            prowdest[b] = ((unsigned char *)vid.colormap)
               [(light & 0xFF00) + pix];
            light += lightstep;
         }

         psource += sourcetstep;
         lightright += lightrightstep;
         lightleft += lightleftstep;
         prowdest += surfrowbytes;
      }

      if (psource >= r_sourcemax)
         psource -= r_stepback;
   }
}

/*
================
R_DrawSurfaceBlock8_mip2
================
*/
void R_DrawSurfaceBlock8_mip2(void)
{
   int v, i, b, lightstep, lighttemp, light;
   unsigned char pix;

   unsigned char *psource = pbasesource;
   unsigned char *prowdest = (unsigned char*)prowdestbase;

   for (v = 0; v < r_numvblocks; v++)
   {
      // FIXME: make these locals?
      // FIXME: use delta rather than both right and left, like ASM?
      lightleft = r_lightptr[0];
      lightright = r_lightptr[1];
      r_lightptr += r_lightwidth;
      lightleftstep = (r_lightptr[0] - lightleft) >> 2;
      lightrightstep = (r_lightptr[1] - lightright) >> 2;

      for (i = 0; i < 4; i++) {
         lighttemp = lightleft - lightright;
         lightstep = lighttemp >> 2;

         light = lightright;

         for (b = 3; b >= 0; b--) {
            pix = psource[b];
            prowdest[b] = ((unsigned char *)vid.colormap)
               [(light & 0xFF00) + pix];
            light += lightstep;
         }

         psource += sourcetstep;
         lightright += lightrightstep;
         lightleft += lightleftstep;
         prowdest += surfrowbytes;
      }

      if (psource >= r_sourcemax)
         psource -= r_stepback;
   }
}


/*
================
R_DrawSurfaceBlock8_mip3
================
*/
void R_DrawSurfaceBlock8_mip3(void)
{
   int v, i, b, lightstep, lighttemp, light;
   unsigned char pix;
   unsigned char *psource = pbasesource;
   unsigned char *prowdest = (unsigned char*)prowdestbase;

   for (v = 0; v < r_numvblocks; v++)
   {
      // FIXME: make these locals?
      // FIXME: use delta rather than both right and left, like ASM?
      lightleft = r_lightptr[0];
      lightright = r_lightptr[1];
      r_lightptr += r_lightwidth;
      lightleftstep = (r_lightptr[0] - lightleft) >> 1;
      lightrightstep = (r_lightptr[1] - lightright) >> 1;

      for (i = 0; i < 2; i++)
      {
         lighttemp = lightleft - lightright;
         lightstep = lighttemp >> 1;

         light = lightright;

         for (b = 1; b >= 0; b--)
         {
            pix = psource[b];
            prowdest[b] = ((unsigned char *)vid.colormap)
               [(light & 0xFF00) + pix];
            light += lightstep;
         }

         psource += sourcetstep;
         lightright += lightrightstep;
         lightleft += lightleftstep;
         prowdest += surfrowbytes;
      }

      if (psource >= r_sourcemax)
         psource -= r_stepback;
   }
}


/*
================
R_DrawSurfaceBlock16

FIXME: make this work
================
*/
void R_DrawSurfaceBlock16(void)
{
   int k;
   unsigned short *prowdest = (unsigned short *)prowdestbase;

   for (k = 0; k < blocksize; k++)
   {
      int b;
      unsigned char *psource = pbasesource;
      int lighttemp = lightright - lightleft;
      int lightstep = lighttemp >> blockdivshift;
      int light = lightleft;
      unsigned short *pdest = prowdest;

      for (b = 0; b < blocksize; b++)
      {
         unsigned char pix = *psource;
         *pdest = vid.colormap16[(light & 0xFF00) + pix];
         psource += sourcesstep;
         pdest++;
         light += lightstep;
      }

      pbasesource += sourcetstep;
      lightright += lightrightstep;
      lightleft += lightleftstep;
      prowdest = (unsigned short *)((uintptr_t)prowdest + surfrowbytes);
   }

   prowdestbase = prowdest;
}
