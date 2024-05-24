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
#include "console.h"
#include "model.h"
#include "sys.h"
#include "zone.h"

#include "d_iface.h"
#include "render.h"

/*
=================
Mod_LoadSpriteFrame
=================
*/
static void * Mod_LoadSpriteFrame(void *pin, mspriteframe_t **ppframe,
      int framenum)
{
   int origin[2];
   dspriteframe_t *pinframe = (dspriteframe_t *)pin;

#ifdef MSB_FIRST
   int width = LittleLong(pinframe->width);
   int height = LittleLong(pinframe->height);
#else
   int width = (pinframe->width);
   int height = (pinframe->height);
#endif
   int numpixels = width * height;
   int size = sizeof(mspriteframe_t) + R_SpriteDataSize(numpixels);
   mspriteframe_t *pspriteframe = (mspriteframe_t*)Hunk_Alloc(size);

   memset(pspriteframe, 0, size);
   *ppframe = pspriteframe;

   pspriteframe->width = width;
   pspriteframe->height = height;
#ifdef MSB_FIRST
   origin[0] = LittleLong(pinframe->origin[0]);
   origin[1] = LittleLong(pinframe->origin[1]);
#else
   origin[0] = (pinframe->origin[0]);
   origin[1] = (pinframe->origin[1]);
#endif

   pspriteframe->up = origin[1];
   pspriteframe->down = origin[1] - height;
   pspriteframe->left = origin[0];
   pspriteframe->right = width + origin[0];

   /* Let the renderer process the pixel data as needed */
   R_SpriteDataStore(pspriteframe, framenum, (byte *)(pinframe + 1));

   return (byte *)pinframe + sizeof(dspriteframe_t) + numpixels;
}


/*
=================
Mod_LoadSpriteGroup
=================
*/
static void * Mod_LoadSpriteGroup(void *pin, mspriteframe_t **ppframe,
		    int framenum)
{
   int i;
   dspriteinterval_t *pin_intervals;
   float *poutintervals;
   void *ptemp;

   dspritegroup_t *pingroup = (dspritegroup_t *)pin;
#ifdef MSB_FIRST
   int numframes = LittleLong(pingroup->numframes);
#else
   int numframes = (pingroup->numframes);
#endif

   mspritegroup_t *pspritegroup = (mspritegroup_t*)Hunk_Alloc(sizeof(*pspritegroup) +
         numframes * sizeof(pspritegroup->frames[0]));

   pspritegroup->numframes = numframes;
   *ppframe = (mspriteframe_t *)pspritegroup;
   pin_intervals = (dspriteinterval_t *)(pingroup + 1);
   poutintervals = (float*)Hunk_Alloc(numframes * sizeof(float));
   pspritegroup->intervals = poutintervals;

   for (i = 0; i < numframes; i++) {
#ifdef MSB_FIRST
      *poutintervals = LittleFloat(pin_intervals->interval);
#else
      *poutintervals = (pin_intervals->interval);
#endif
      if (*poutintervals <= 0.0)
         Sys_Error("%s: interval <= 0", __func__);

      poutintervals++;
      pin_intervals++;
   }

   ptemp = (void *)pin_intervals;

   for (i = 0; i < numframes; i++) {
      ptemp = Mod_LoadSpriteFrame(ptemp, &pspritegroup->frames[i], framenum * 100 + i);
   }

   return ptemp;
}


/*
=================
Mod_LoadSpriteModel
=================
*/
void Mod_LoadSpriteModel(model_t *mod, void *buffer)
{
   int i;
   msprite_t *psprite;
   int numframes;
   int size;
   dspriteframetype_t *pframetype;
   dsprite_t *pin = (dsprite_t *)buffer;

#ifdef MSB_FIRST
   int version = LittleLong(pin->version);
#else
   int version = (pin->version);
#endif
   if (version != SPRITE_VERSION)
      Sys_Error("%s: %s has wrong version number (%i should be %i)",
            __func__, mod->name, version, SPRITE_VERSION);

#ifdef MSB_FIRST
   numframes = LittleLong(pin->numframes);
#else
   numframes = (pin->numframes);
#endif
   size = sizeof(*psprite) + numframes * sizeof(psprite->frames[0]);
   psprite = (msprite_t*)Hunk_Alloc(size);
   mod->cache.data = psprite;

#ifdef MSB_FIRST
   psprite->type = LittleLong(pin->type);
   psprite->maxwidth = LittleLong(pin->width);
   psprite->maxheight = LittleLong(pin->height);
   psprite->beamlength = LittleFloat(pin->beamlength);
   mod->synctype = (synctype_t)LittleLong(pin->synctype);
#else
   psprite->type = (pin->type);
   psprite->maxwidth = (pin->width);
   psprite->maxheight = (pin->height);
   psprite->beamlength = (pin->beamlength);
   mod->synctype = (synctype_t)(pin->synctype);
#endif
   psprite->numframes = numframes;

   mod->mins[0] = mod->mins[1] = -psprite->maxwidth / 2;
   mod->maxs[0] = mod->maxs[1] = psprite->maxwidth / 2;
   mod->mins[2] = -psprite->maxheight / 2;
   mod->maxs[2] = psprite->maxheight / 2;

   //
   // load the frames
   //
   if (numframes < 1)
      Sys_Error("%s: Invalid # of frames: %d", __func__, numframes);

   mod->numframes = numframes;
   mod->flags = 0;

   pframetype = (dspriteframetype_t *)(pin + 1);

   for (i = 0; i < numframes; i++) {
      spriteframetype_t frametype;

#ifdef MSB_FIRST
      frametype = (spriteframetype_t)LittleLong(pframetype->type);
#else
      frametype = (spriteframetype_t)(pframetype->type);
#endif
      psprite->frames[i].type = frametype;

      if (frametype == SPR_SINGLE) {
         pframetype = (dspriteframetype_t *)
            Mod_LoadSpriteFrame(pframetype + 1,
                  &psprite->frames[i].frameptr, i);
      } else {
         pframetype = (dspriteframetype_t *)
            Mod_LoadSpriteGroup(pframetype + 1,
                  &psprite->frames[i].frameptr, i);
      }
   }

   mod->type = mod_sprite;
}

/*
==================
Mod_GetSpriteFrame
==================
*/
mspriteframe_t *Mod_GetSpriteFrame(const entity_t *e, msprite_t *psprite, float time)
{
   mspriteframe_t *pspriteframe;
   int i;
   int frame = e->frame;

   if ((frame >= psprite->numframes) || (frame < 0))
   {
      Con_Printf("R_DrawSprite: no such frame %d\n", frame);
      frame = 0;
   }

   if (psprite->frames[frame].type == SPR_SINGLE)
      pspriteframe = psprite->frames[frame].frameptr;
   else
   {
      mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
      float *pintervals            = pspritegroup->intervals;
      int numframes                = pspritegroup->numframes;
      float fullinterval           = pintervals[numframes - 1];

      // when loading in Mod_LoadSpriteGroup, we guaranteed all interval
      // values are positive, so we don't have to worry about division by 0
      float targettime             = time - ((int)(time / fullinterval)) * fullinterval;

      for (i = 0; i < (numframes - 1); i++)
      {
         if (pintervals[i] > targettime)
            break;
      }
      pspriteframe = pspritegroup->frames[i];
   }

   return pspriteframe;
}
