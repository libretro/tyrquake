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
// view.c -- player eye positioning

#include "bspfile.h"
#include "client.h"
#include "cmd.h"
#include "console.h"
#include "cvar.h"
#include "draw.h"
#include "host.h"
#include "quakedef.h"
#include "screen.h"
#include "view.h"

/*
 * The view is allowed to move slightly from it's true position for bobbing,
 * but if it exceeds 8 pixels linear distance (spherical, not box), the list
 * of entities sent from the server may not include everything in the pvs,
 * especially when crossing a water boudnary.
 */

cvar_t scr_ofsx = { "scr_ofsx", "0", false };
cvar_t scr_ofsy = { "scr_ofsy", "0", false };
cvar_t scr_ofsz = { "scr_ofsz", "0", false };

cvar_t cl_rollspeed = { "cl_rollspeed", "200" };
cvar_t cl_rollangle = { "cl_rollangle", "2.0" };

cvar_t cl_bob = { "cl_bob", "0.02", false };
cvar_t cl_bobcycle = { "cl_bobcycle", "0.6", false };
cvar_t cl_bobup = { "cl_bobup", "0.5", false };

cvar_t v_kicktime = { "v_kicktime", "0.5", false };
cvar_t v_kickroll = { "v_kickroll", "0.6", false };
cvar_t v_kickpitch = { "v_kickpitch", "0.6", false };

cvar_t v_iyaw_cycle = { "v_iyaw_cycle", "2", false };
cvar_t v_iroll_cycle = { "v_iroll_cycle", "0.5", false };
cvar_t v_ipitch_cycle = { "v_ipitch_cycle", "1", false };
cvar_t v_iyaw_level = { "v_iyaw_level", "0.3", false };
cvar_t v_iroll_level = { "v_iroll_level", "0.1", false };
cvar_t v_ipitch_level = { "v_ipitch_level", "0.3", false };

cvar_t v_idlescale = { "v_idlescale", "0", false };

cvar_t crosshair = { "crosshair", "0", true };
cvar_t crosshaircolor = { "crosshaircolor", "79", true };
cvar_t cl_crossx = { "cl_crossx", "0", false };
cvar_t cl_crossy = { "cl_crossy", "0", false };

float v_dmg_time, v_dmg_roll, v_dmg_pitch;

/* BOF - Framerate-independent stair-step smoothing */
float   v_oldz, v_stepz;
float   v_steptime;
/* EOF - Framerate-independent stair-step smoothing */

static int old_health = 100;
static float old_velocity_z = 0.0;
extern void retro_set_rumble_damage(int damage);
extern void retro_set_rumble_touch(unsigned intensity, float duration);

/*
===============
V_CalcRoll

Used by view and sv_user
===============
*/
vec3_t forward, right, up;

void V_NewMap (void)
{
   v_oldz = v_stepz = 0;
   v_steptime = 0;
}

float V_CalcRoll(vec3_t angles, vec3_t velocity)
{
   float sign;
   float side;
   float value;

   AngleVectors(angles, forward, right, up);
   side = DotProduct(velocity, right);
   sign = side < 0 ? -1 : 1;
   side = fabs(side);

   value = cl_rollangle.value;
   //      if (cl.inwater)
   //              value *= 6;

   if (side < cl_rollspeed.value)
      side = side * value / cl_rollspeed.value;
   else
      side = value;

   return side * sign;

}


/*
===============
V_CalcBob

===============
*/
float V_CalcBob(void)
{
   float bob;
   float cycle;

   /* Avoid divide-by-zero, don't bob */
   if (!cl_bobcycle.value)
      return 0.0f;

   cycle = cl.time - (int)(cl.time / cl_bobcycle.value) * cl_bobcycle.value;
   cycle /= cl_bobcycle.value;
   if (cycle < cl_bobup.value)
      cycle = M_PI * cycle / cl_bobup.value;
   else
      cycle =
         M_PI + M_PI * (cycle - cl_bobup.value) / (1.0 - cl_bobup.value);

   // bob is proportional to velocity in the xy plane
   // (don't count Z, or jumping messes it up)

   bob =
      sqrt(cl.velocity[0] * cl.velocity[0] +
            cl.velocity[1] * cl.velocity[1]) * cl_bob.value;
   //Con_Printf ("speed: %5.1f\n", Length(cl.velocity));
   bob = bob * 0.3 + bob * 0.7 * sin(cycle);
   if (bob > 4)
      bob = 4;
   else if (bob < -7)
      bob = -7;

   /* Check for a sudden stop in downwards motion
    * > Means we've just 'touched' the ground, so
    *   trigger a weak touch-type rumble */
   if ((old_velocity_z < 0.0) &&
       (cl.velocity[2] == 0.0))
      retro_set_rumble_touch(6, 120.0f);
   old_velocity_z = cl.velocity[2];

   return bob;
}


//=============================================================================


cvar_t v_centermove = { "v_centermove", "0.15", false };
cvar_t v_centerspeed = { "v_centerspeed", "500" };


void V_StartPitchDrift(void)
{
   if (cl.laststop == cl.time)
      return; // something else is keeping it from drifting
   if (cl.nodrift || !cl.pitchvel)
   {
      cl.pitchvel = v_centerspeed.value;
      cl.nodrift = false;
      cl.driftmove = 0;
   }
}

void V_StopPitchDrift(void)
{
   cl.laststop = cl.time;
   cl.nodrift = true;
   cl.pitchvel = 0;
}

/*
===============
V_DriftPitch

Moves the client pitch angle towards cl.idealpitch sent by the server.

If the user is adjusting pitch manually, either with lookup/lookdown,
mlook and mouse, or klook and keyboard, pitch drifting is constantly stopped.

Drifting is enabled when the center view key is hit, mlook is released and
lookspring is non 0, or when
===============
*/
void V_DriftPitch(void)
{
   float delta, move;

   if (noclip_anglehack || !cl.onground || cls.demoplayback) {
      cl.driftmove = 0;
      cl.pitchvel = 0;
      return;
   }
   // don't count small mouse motion
   if (cl.nodrift) {
      if (fabs(cl.cmd.forwardmove) < cl_forwardspeed.value)
         cl.driftmove = 0;
      else
         cl.driftmove += host_frametime;

      if (cl.driftmove > v_centermove.value) {
         if (lookspring.value)
            V_StartPitchDrift();
      }
      return;
   }

   delta = cl.idealpitch - cl.viewangles[PITCH];

   if (!delta) {
      cl.pitchvel = 0;
      return;
   }

   move = host_frametime * cl.pitchvel;
   cl.pitchvel += host_frametime * v_centerspeed.value;

   if (delta > 0) {
      if (move > delta) {
         cl.pitchvel = 0;
         move = delta;
      }
      cl.viewangles[PITCH] += move;
   } else if (delta < 0) {
      if (move > -delta) {
         cl.pitchvel = 0;
         move = -delta;
      }
      cl.viewangles[PITCH] -= move;
   }
}





/*
==============================================================================

				PALETTE FLASHES

==============================================================================
*/


cshift_t cshift_empty = { {130, 80, 50}, 0 };
cshift_t cshift_water = { {130, 80, 50}, 128 };
cshift_t cshift_slime = { {0, 25, 5}, 150 };
cshift_t cshift_lava = { {255, 80, 0}, 150 };

cvar_t v_gamma = { "gamma", "0.95", true };

byte gammatable[256];		// palette is sent through this

void BuildGammaTable(float g)
{
   int i;

   if (g == 1.0)
   {
      for (i = 0; i < 256; i++)
         gammatable[i] = i;
      return;
   }

   for (i = 0; i < 256; i++)
   {
      int inf = 255 * powf((i + 0.5) / 255.5, g) + 0.5;
      if (inf < 0)
         inf = 0;
      if (inf > 255)
         inf = 255;
      gammatable[i] = inf;
   }
}

/*
=================
V_CheckGamma
=================
*/
qboolean V_CheckGamma(void)
{
   static float oldgammavalue;

   if (v_gamma.value == oldgammavalue)
      return false;
   oldgammavalue = v_gamma.value;

   BuildGammaTable(v_gamma.value);
   vid.recalc_refdef = 1;	// force a surface cache flush

   return true;
}



/*
===============
V_ParseDamage
===============
*/
void V_ParseDamage(void)
{
   vec3_t from;
   int i;
   vec3_t forward, right, up;
   const entity_t *ent;
   float side;
   float count;
   int armor = MSG_ReadByte();
   int blood = MSG_ReadByte();

   for (i = 0; i < 3; i++)
      from[i] = MSG_ReadCoord();

   count = blood * 0.5 + armor * 0.5;
   if (count < 10)
      count = 10;

   cl.faceanimtime = cl.time + 0.2;	// but sbar face into pain frame

   cl.cshifts[CSHIFT_DAMAGE].percent += 3 * count;
   if (cl.cshifts[CSHIFT_DAMAGE].percent < 0)
      cl.cshifts[CSHIFT_DAMAGE].percent = 0;
   if (cl.cshifts[CSHIFT_DAMAGE].percent > 150)
      cl.cshifts[CSHIFT_DAMAGE].percent = 150;

   if (armor > blood) {
      cl.cshifts[CSHIFT_DAMAGE].destcolor[0] = 200;
      cl.cshifts[CSHIFT_DAMAGE].destcolor[1] = 100;
      cl.cshifts[CSHIFT_DAMAGE].destcolor[2] = 100;
   } else if (armor) {
      cl.cshifts[CSHIFT_DAMAGE].destcolor[0] = 220;
      cl.cshifts[CSHIFT_DAMAGE].destcolor[1] = 50;
      cl.cshifts[CSHIFT_DAMAGE].destcolor[2] = 50;
   } else {
      cl.cshifts[CSHIFT_DAMAGE].destcolor[0] = 255;
      cl.cshifts[CSHIFT_DAMAGE].destcolor[1] = 0;
      cl.cshifts[CSHIFT_DAMAGE].destcolor[2] = 0;
   }

   /* calculate view angle kicks */
   ent = &cl_entities[cl.viewentity];

   VectorSubtract(from, ent->origin, from);
   VectorNormalize(from);

   AngleVectors(ent->angles, forward, right, up);

   side = DotProduct(from, right);
   v_dmg_roll = count * side * v_kickroll.value;

   side = DotProduct(from, forward);
   v_dmg_pitch = count * side * v_kickpitch.value;

   v_dmg_time = v_kicktime.value;

   /* BOF - Frame-rate independent damage and bonus shifts */
   cl.cshifts[CSHIFT_DAMAGE].initialpct = cl.cshifts[CSHIFT_DAMAGE].percent;
   cl.cshifts[CSHIFT_DAMAGE].time = cl.time;
   /* EOF - Frame-rate independent damage and bonus shifts */
}


/*
==================
V_cshift_f
==================
*/
void V_cshift_f(void)
{
    cshift_empty.destcolor[0] = atoi(Cmd_Argv(1));
    cshift_empty.destcolor[1] = atoi(Cmd_Argv(2));
    cshift_empty.destcolor[2] = atoi(Cmd_Argv(3));
    cshift_empty.percent = atoi(Cmd_Argv(4));
}


/*
==================
V_BonusFlash_f

When you run over an item, the server sends this command
==================
*/
void V_BonusFlash_f(void)
{
    cl.cshifts[CSHIFT_BONUS].destcolor[0] = 215;
    cl.cshifts[CSHIFT_BONUS].destcolor[1] = 186;
    cl.cshifts[CSHIFT_BONUS].destcolor[2] = 69;
    cl.cshifts[CSHIFT_BONUS].percent = 50;

    /* BOF - Frame-rate independent damage and bonus shifts */
    cl.cshifts[CSHIFT_BONUS].initialpct = 50;
    cl.cshifts[CSHIFT_BONUS].time = cl.time;
    /* EOF - Frame-rate independent damage and bonus shifts */

    /* We have touched an item - trigger a moderate
     * touch-type rumble */
    retro_set_rumble_touch(9, 140.0f);
}

/*
=============
V_SetContentsColor

Underwater, lava, etc each has a color shift
=============
*/
void V_SetContentsColor(int contents)
{
   switch (contents)
   {
      case CONTENTS_EMPTY:
      case CONTENTS_SOLID:
         cl.cshifts[CSHIFT_CONTENTS] = cshift_empty;
         break;
      case CONTENTS_LAVA:
         cl.cshifts[CSHIFT_CONTENTS] = cshift_lava;
         break;
      case CONTENTS_SLIME:
         cl.cshifts[CSHIFT_CONTENTS] = cshift_slime;
         break;
      default:
         cl.cshifts[CSHIFT_CONTENTS] = cshift_water;
   }
}

/*
=============
V_CalcPowerupCshift
=============
*/
void V_CalcPowerupCshift(void)
{
   if (cl.stats[STAT_ITEMS] & IT_QUAD) {
      cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 0;
      cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 0;
      cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 255;
      cl.cshifts[CSHIFT_POWERUP].percent = 30;
   } else if (cl.stats[STAT_ITEMS] & IT_SUIT) {
      cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 0;
      cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 255;
      cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 0;
      cl.cshifts[CSHIFT_POWERUP].percent = 20;
   } else if (cl.stats[STAT_ITEMS] & IT_INVISIBILITY) {
      cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 100;
      cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 100;
      cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 100;
      cl.cshifts[CSHIFT_POWERUP].percent = 100;
   } else if (cl.stats[STAT_ITEMS] & IT_INVULNERABILITY) {
      cl.cshifts[CSHIFT_POWERUP].destcolor[0] = 255;
      cl.cshifts[CSHIFT_POWERUP].destcolor[1] = 255;
      cl.cshifts[CSHIFT_POWERUP].destcolor[2] = 0;
      cl.cshifts[CSHIFT_POWERUP].percent = 30;
   } else
      cl.cshifts[CSHIFT_POWERUP].percent = 0;
}

void V_DropCShift (cshift_t *cs, float droprate)
{
   if (cs->time < 0)
      cs->percent = 0;
   else if ((cs->percent = cs->initialpct - (cl.time - cs->time) * droprate) <= 0)
   {
      cs->percent = 0;
      cs->time = -1;
   }
}

/*
=============
V_UpdatePalette
=============
*/
void V_UpdatePalette(void)
{
   int i, j;
   qboolean newobj;
   byte *basepal, *newpal;
   byte pal[768];
   qboolean force;

   V_CalcPowerupCshift();

   newobj = false;

   for (i = 0; i < NUM_CSHIFTS; i++)
   {
      if (cl.cshifts[i].percent != cl.prev_cshifts[i].percent)
      {
         newobj = true;
         cl.prev_cshifts[i].percent = cl.cshifts[i].percent;
      }
      for (j = 0; j < 3; j++)
         if (cl.cshifts[i].destcolor[j] != cl.prev_cshifts[i].destcolor[j])
         {
            newobj = true;
            cl.prev_cshifts[i].destcolor[j] = cl.cshifts[i].destcolor[j];
         }
   }

   // drop the damage and bonus values
   V_DropCShift (&cl.cshifts[CSHIFT_DAMAGE], 150);
   V_DropCShift (&cl.cshifts[CSHIFT_BONUS], 100);

   force = V_CheckGamma();
   if (!newobj && !force)
      return;

   basepal = host_basepal;
   newpal  = pal;

   for (i = 0; i < 256; i++)
   {
      int r = basepal[0];
      int g = basepal[1];
      int b = basepal[2];
      basepal += 3;

      for (j = 0; j < NUM_CSHIFTS; j++)
      {
         r += (cl.cshifts[j].percent *
               (cl.cshifts[j].destcolor[0] - r)) >> 8;
         g += (cl.cshifts[j].percent *
               (cl.cshifts[j].destcolor[1] - g)) >> 8;
         b += (cl.cshifts[j].percent *
               (cl.cshifts[j].destcolor[2] - b)) >> 8;
      }

      newpal[0] = gammatable[r];
      newpal[1] = gammatable[g];
      newpal[2] = gammatable[b];
      newpal += 3;
   }

   VID_SetPalette(pal);
}

/*
==============================================================================

				VIEW RENDERING

==============================================================================
*/

float angledelta(float a)
{
   a = anglemod(a);
   if (a > 180)
      a -= 360;
   return a;
}

/*
==================
CalcGunAngle
==================
*/
void CalcGunAngle(void)
{
   float move;
   static float oldyaw = 0;
   static float oldpitch = 0;
   float yaw   = r_refdef.viewangles[YAW];
   float pitch = -r_refdef.viewangles[PITCH];

   yaw = angledelta(yaw - r_refdef.viewangles[YAW]) * 0.4;

   if (yaw > 10)
      yaw = 10;
   if (yaw < -10)
      yaw = -10;
   pitch = angledelta(-pitch - r_refdef.viewangles[PITCH]) * 0.4;
   if (pitch > 10)
      pitch = 10;
   if (pitch < -10)
      pitch = -10;
   move = host_frametime * 20;
   if (yaw > oldyaw)
   {
      if (oldyaw + move < yaw)
         yaw = oldyaw + move;
   }
   else
   {
      if (oldyaw - move > yaw)
         yaw = oldyaw - move;
   }

   if (pitch > oldpitch) {
      if (oldpitch + move < pitch)
         pitch = oldpitch + move;
   } else {
      if (oldpitch - move > pitch)
         pitch = oldpitch - move;
   }

   oldyaw = yaw;
   oldpitch = pitch;

   cl.viewent.angles[YAW] = r_refdef.viewangles[YAW] + yaw;
   cl.viewent.angles[PITCH] = -(r_refdef.viewangles[PITCH] + pitch);

   cl.viewent.angles[ROLL] -=
      v_idlescale.value * sin(cl.time * v_iroll_cycle.value) *
      v_iroll_level.value;
   cl.viewent.angles[PITCH] -=
      v_idlescale.value * sin(cl.time * v_ipitch_cycle.value) *
      v_ipitch_level.value;
   cl.viewent.angles[YAW] -=
      v_idlescale.value * sin(cl.time * v_iyaw_cycle.value) *
      v_iyaw_level.value;
}

/*
==============
V_BoundOffsets
==============
*/
void V_BoundOffsets(void)
{
   const entity_t *ent = &cl_entities[cl.viewentity];

   // absolutely bound refresh reletive to entity clipping hull
   // so the view can never be inside a solid wall

   if (r_refdef.vieworg[0] < ent->origin[0] - 14)
      r_refdef.vieworg[0] = ent->origin[0] - 14;
   else if (r_refdef.vieworg[0] > ent->origin[0] + 14)
      r_refdef.vieworg[0] = ent->origin[0] + 14;
   if (r_refdef.vieworg[1] < ent->origin[1] - 14)
      r_refdef.vieworg[1] = ent->origin[1] - 14;
   else if (r_refdef.vieworg[1] > ent->origin[1] + 14)
      r_refdef.vieworg[1] = ent->origin[1] + 14;
   if (r_refdef.vieworg[2] < ent->origin[2] - 22)
      r_refdef.vieworg[2] = ent->origin[2] - 22;
   else if (r_refdef.vieworg[2] > ent->origin[2] + 30)
      r_refdef.vieworg[2] = ent->origin[2] + 30;
}

/*
==============
V_AddIdle

Idle swaying
==============
*/
void V_AddIdle(void)
{
   r_refdef.viewangles[ROLL] +=
      v_idlescale.value * sin(cl.time * v_iroll_cycle.value) *
      v_iroll_level.value;
   r_refdef.viewangles[PITCH] +=
      v_idlescale.value * sin(cl.time * v_ipitch_cycle.value) *
      v_ipitch_level.value;
   r_refdef.viewangles[YAW] +=
      v_idlescale.value * sin(cl.time * v_iyaw_cycle.value) *
      v_iyaw_level.value;
}


/*
==============
V_CalcViewRoll

Roll is induced by movement and damage
==============
*/
void V_CalcViewRoll(void)
{
   float side = V_CalcRoll(cl_entities[cl.viewentity].angles, cl.velocity);

   r_refdef.viewangles[ROLL] += side;

   if (v_dmg_time > 0) {
      r_refdef.viewangles[ROLL] +=
         v_dmg_time / v_kicktime.value * v_dmg_roll;
      r_refdef.viewangles[PITCH] +=
         v_dmg_time / v_kicktime.value * v_dmg_pitch;
      v_dmg_time -= host_frametime;

      if (old_health > cl.stats[STAT_HEALTH])
         retro_set_rumble_damage(old_health - cl.stats[STAT_HEALTH]);

      old_health = cl.stats[STAT_HEALTH];
   }
   else
      retro_set_rumble_damage(0);

   if (cl.stats[STAT_HEALTH] <= 0) {
      r_refdef.viewangles[ROLL] = 80;	// dead view angle
      return;
   }

}


/*
==================
V_CalcIntermissionRefdef

==================
*/
void V_CalcIntermissionRefdef(void)
{
   float old;

   /* ent is the player model (visible when out of body) */
   entity_t *ent = &cl_entities[cl.viewentity];
   // view is the weapon model (only visible from inside body)
   entity_t *view = &cl.viewent;

   VectorCopy(ent->origin, r_refdef.vieworg);
   VectorCopy(ent->angles, r_refdef.viewangles);
   view->model = NULL;

   // allways idle in intermission
   old = v_idlescale.value;
   v_idlescale.value = 1;
   V_AddIdle();
   v_idlescale.value = old;
}

/*
==================
V_CalcRefdef

==================
*/
void V_CalcRefdef(void)
{
   entity_t *ent, *view;
   int i;
   vec3_t forward, right, up;
   vec3_t angles;
   float bob;

   V_DriftPitch();

   // ent is the player model (visible when out of body)
   ent = &cl_entities[cl.viewentity];
   // view is the weapon model (only visible from inside body)
   view = &cl.viewent;

   // transform the view offset by the model's matrix to get the offset from
   // model origin for the view
   ent->angles[YAW] = cl.viewangles[YAW];	// the model should face
   // the view dir
   ent->angles[PITCH] = -cl.viewangles[PITCH];	// the model should face
   // the view dir

   bob = V_CalcBob();

   // refresh position
   VectorCopy(ent->origin, r_refdef.vieworg);
   r_refdef.vieworg[2] += cl.viewheight + bob;

   // never let it sit exactly on a node line, because a water plane can
   // dissapear when viewed with the eye exactly on it.
   // the server protocol only specifies to 1/16 pixel, so add 1/32 in each axis
   r_refdef.vieworg[0] += 1.0 / 32;
   r_refdef.vieworg[1] += 1.0 / 32;
   r_refdef.vieworg[2] += 1.0 / 32;

   VectorCopy(cl.viewangles, r_refdef.viewangles);
   V_CalcViewRoll();
   V_AddIdle();

   // offsets
   angles[PITCH] = -ent->angles[PITCH];	// because entity pitches are
   //  actually backward
   angles[YAW] = ent->angles[YAW];
   angles[ROLL] = ent->angles[ROLL];

   AngleVectors(angles, forward, right, up);

   for (i = 0; i < 3; i++)
   {
      r_refdef.vieworg[i] += scr_ofsx.value * forward[i]
      + scr_ofsy.value * right[i]
      + scr_ofsz.value * up[i];
   }

   V_BoundOffsets();

   // set up gun position
   VectorCopy(cl.viewangles, view->angles);

   CalcGunAngle();

   VectorCopy(ent->origin, view->origin);
   view->origin[2] += cl.viewheight;

   for (i = 0; i < 3; i++) {
      view->origin[i] += forward[i] * bob * 0.4;
      //              view->origin[i] += right[i]*bob*0.4;
      //              view->origin[i] += up[i]*bob*0.8;
   }
   view->origin[2] += bob;

   // fudge position around to keep amount of weapon visible
   // roughly equal with different FOV
   if (scr_viewsize.value == 110)
      view->origin[2] += 1;
   else if (scr_viewsize.value == 100)
      view->origin[2] += 2;
   else if (scr_viewsize.value == 90)
      view->origin[2] += 1;
   else if (scr_viewsize.value == 80)
      view->origin[2] += 0.5;

   view->model = cl.model_precache[cl.stats[STAT_WEAPON]];
   view->frame = cl.stats[STAT_WEAPONFRAME];
   view->colormap = vid.colormap;

   // set up the refresh position
   VectorAdd(r_refdef.viewangles, cl.punchangle, r_refdef.viewangles);

   // smooth out stair step ups
   if (cl.onground && ent->origin[2] - v_stepz > 0)
   {
      v_stepz = v_oldz + (cl.time - v_steptime) * 80; // BJP Quake used 160 here

      if (v_stepz > ent->origin[2])
      {
         v_steptime = cl.time;
         v_stepz = v_oldz = ent->origin[2];
      }

      if (ent->origin[2] - v_stepz > 12)
      {
         v_steptime = cl.time;
         v_stepz = v_oldz = ent->origin[2] - 12;
      }

      r_refdef.vieworg[2] += v_stepz - ent->origin[2];
      view->origin[2] += v_stepz - ent->origin[2];
   }
   else
   {
      v_oldz = v_stepz = ent->origin[2];
      v_steptime = cl.time;
   }

   if (chase_active.value)
      Chase_Update();
}

/*
==================
V_RenderView

The player's clipping box goes from (-16 -16 -24) to (16 16 32) from
the entity origin, so any view position inside that will be valid
==================
*/
void V_RenderView(void)
{
   if (con_forcedup)
      return;

   // don't allow cheats in multiplayer
   if (cl.maxclients > 1)
   {
      Cvar_Set("scr_ofsx", "0");
      Cvar_Set("scr_ofsy", "0");
      Cvar_Set("scr_ofsz", "0");
   }

   if (cl.intermission) // intermission / finale rendering
      V_CalcIntermissionRefdef();
   else
   {
      if (!cl.paused /* && (sv.maxclients > 1 || key_dest == key_game) */ )
         V_CalcRefdef();
   }

   R_RenderView();

   if (crosshair.value)
      Draw_Crosshair();
}

//============================================================================

/*
=============
V_Init
=============
*/
void V_Init(void)
{
   Cmd_AddCommand("v_cshift", V_cshift_f);
   Cmd_AddCommand("bf", V_BonusFlash_f);
   Cmd_AddCommand("centerview", V_StartPitchDrift);

   Cvar_RegisterVariable(&v_centermove);
   Cvar_RegisterVariable(&v_centerspeed);

   Cvar_RegisterVariable(&v_iyaw_cycle);
   Cvar_RegisterVariable(&v_iroll_cycle);
   Cvar_RegisterVariable(&v_ipitch_cycle);
   Cvar_RegisterVariable(&v_iyaw_level);
   Cvar_RegisterVariable(&v_iroll_level);
   Cvar_RegisterVariable(&v_ipitch_level);

   Cvar_RegisterVariable(&v_idlescale);
   Cvar_RegisterVariable(&crosshair);
   Cvar_RegisterVariable(&crosshaircolor);
   Cvar_RegisterVariable(&cl_crossx);
   Cvar_RegisterVariable(&cl_crossy);

   Cvar_RegisterVariable(&scr_ofsx);
   Cvar_RegisterVariable(&scr_ofsy);
   Cvar_RegisterVariable(&scr_ofsz);
   Cvar_RegisterVariable(&cl_rollspeed);
   Cvar_RegisterVariable(&cl_rollangle);
   Cvar_RegisterVariable(&cl_bob);
   Cvar_RegisterVariable(&cl_bobcycle);
   Cvar_RegisterVariable(&cl_bobup);

   Cvar_RegisterVariable(&v_kicktime);
   Cvar_RegisterVariable(&v_kickroll);
   Cvar_RegisterVariable(&v_kickpitch);

   BuildGammaTable(1.0);	// no gamma yet
   Cvar_RegisterVariable(&v_gamma);
}
