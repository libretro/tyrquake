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
// cl_parse.c  -- parse a message received from the server

#include "cdaudio.h"
#include "client.h"
#include "cmd.h"
#include "console.h"
#include "host.h"
#include "model.h"
#include "net.h"
#include "protocol.h"
#include "quakedef.h"
#include "sbar.h"
#include "screen.h"
#include "server.h"
#include "sound.h"
#include "bgmusic.h"
#include "sys.h"

#include <compat/strl.h>

//=============================================================================

/*
===============
CL_EntityNum

This error checks and tracks the total number of entities
===============
*/
static entity_t *
CL_EntityNum(int num)
{
    if (num >= cl.num_entities) {
	if (num >= MAX_EDICTS)
	    Host_Error("CL_EntityNum: %i is an invalid number", num);
	while (cl.num_entities <= num) {
	    cl_entities[cl.num_entities].colormap = vid.colormap;
	    cl.num_entities++;
	}
    }

    return &cl_entities[num];
}


static int CL_ReadSoundNum(int field_mask)
{
   switch (cl.protocol)
   {
      case PROTOCOL_VERSION_FITZ:
         if (field_mask & SND_FITZ_LARGESOUND)
            return (unsigned short)MSG_ReadShort();
	 /* fall-through */
      case PROTOCOL_VERSION_NQ:
      case PROTOCOL_VERSION_BJP:
         return MSG_ReadByte();
      case PROTOCOL_VERSION_BJP2:
      case PROTOCOL_VERSION_BJP3:
         return (unsigned short)MSG_ReadShort();
      default:
         Host_Error("%s: Unknown protocol version (%d)\n", __func__,
               cl.protocol);
   }

   return 0;
}

/*
==================
CL_ParseStartSoundPacket
==================
*/
void
CL_ParseStartSoundPacket(void)
{
   vec3_t pos;
   int channel, ent;
   int sound_num;
   int i;
   int field_mask    = MSG_ReadByte();
   int volume        = DEFAULT_SOUND_PACKET_VOLUME;
   float attenuation = DEFAULT_SOUND_PACKET_ATTENUATION;

   if (field_mask & SND_VOLUME)
      volume = MSG_ReadByte();

   if (field_mask & SND_ATTENUATION)
      attenuation = MSG_ReadByte() / 64.0;

   if (cl.protocol == PROTOCOL_VERSION_FITZ && (field_mask & SND_FITZ_LARGEENTITY))
   {
      ent = (unsigned short)MSG_ReadShort();
      channel = MSG_ReadByte();
   }
   else
   {
      channel = MSG_ReadShort();
      ent = channel >> 3;
      channel &= 7;
   }
   sound_num = CL_ReadSoundNum(field_mask);

   if (ent > MAX_EDICTS)
      Host_Error("CL_ParseStartSoundPacket: ent = %i", ent);

   for (i = 0; i < 3; i++)
      pos[i] = MSG_ReadCoord();

   S_StartSound(ent, channel, cl.sound_precache[sound_num], pos,
         volume / 255.0, attenuation);
}

/*
==================
CL_KeepaliveMessage

When the client is taking a long time to load stuff, send keepalive messages
so the server doesn't disconnect.
==================
*/
static void CL_KeepaliveMessage(void)
{
    float time;
    static float lastmsg;
    int ret;
    sizebuf_t old;
    byte olddata[8192];

    if (sv.active)
	return;			// no need if server is local
    if (cls.demoplayback)
	return;

    // read messages from server, should just be nops
    old = net_message;
    memcpy(olddata, net_message.data, net_message.cursize);

    do {
	ret = CL_GetMessage();
	switch (ret) {
	case 0:
	    break;		// nothing waiting
	case 1:
	    Host_Error("%s: received a message", __func__);
	case 2:
	    if (MSG_ReadByte() != svc_nop)
		Host_Error("%s: datagram wasn't a nop", __func__);
	    break;
	default:
	    Host_Error("%s: CL_GetMessage failed", __func__);
	}
    } while (ret);

    net_message = old;
    memcpy(net_message.data, olddata, net_message.cursize);

    // check time
    time = Sys_DoubleTime();
    if (time - lastmsg < 5)
	return;
    lastmsg = time;

    // write out a nop
    Con_Printf("--> client to server keepalive\n");

    MSG_WriteByte(&cls.message, clc_nop);
    NET_SendMessage(cls.netcon, &cls.message);
    SZ_Clear(&cls.message);
}

/*
==================
CL_ParseServerInfo
==================
*/
void
CL_ParseServerInfo(void)
{
    char *level;
    const char *mapname;
    int i;
    int nummodels, numsounds;
    char **model_precache = malloc(sizeof(char*) * MAX_MODELS);
    char **sound_precache = malloc(sizeof(char*) * MAX_SOUNDS);
    for (i = 0; i < MAX_MODELS; i++)
       model_precache[i] = malloc(sizeof(char*) * MAX_QPATH);
    for (i = 0; i < MAX_SOUNDS; i++)
       sound_precache[i] = malloc(sizeof(char*) * MAX_QPATH);

    /* wipe the client_state_t struct */
    CL_ClearState();

    /* parse protocol version number */
    i = MSG_ReadLong();
    if (!Protocol_Known(i))
    {
       Con_Printf("Server returned unknown protocol version %i\n", i);
       return;
    }
    cl.protocol = i;

    /* parse maxclients */
    cl.maxclients = MSG_ReadByte();
    if (cl.maxclients < 1 || cl.maxclients > MAX_SCOREBOARD)
    {
       Con_Printf("Bad maxclients (%u) from server\n", cl.maxclients);
       return;
    }
    cl.players = (player_info_t*)Hunk_AllocName(cl.maxclients * sizeof(*cl.players), "players");

    /* parse gametype */
    cl.gametype = MSG_ReadByte();

    /* parse signon message */
    level = cl.levelname;
    strlcpy(level, MSG_ReadString(), sizeof(cl.levelname));

    /* seperate the printfs so the server message can have a color */
    Con_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36"
	       "\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
    Con_Printf("%c%s\n", 2, level);
    Con_Printf("Using protocol %i\n", cl.protocol);

    // first we go through and touch all of the precache data that still
    // happens to be in the cache, so precaching something else doesn't
    // needlessly purge it

    /* precache models */
    memset(cl.model_precache, 0, sizeof(cl.model_precache));
    for (nummodels = 1;; nummodels++)
    {
       char *in = MSG_ReadString();
       if (!in[0])
          break;
       if (nummodels == max_models(cl.protocol))
       {
          Host_Error("Server sent too many model precaches (max = %d)",
                max_models(cl.protocol));
          return;
       }
       strcpy(model_precache[nummodels], in);
       Mod_TouchModel(in);
    }

    /* precache sounds */
    memset(cl.sound_precache, 0, sizeof(cl.sound_precache));
    for (numsounds = 1;; numsounds++)
    {
       char *in = MSG_ReadString();
       if (!in[0])
          break;
       if (numsounds == max_sounds(cl.protocol))
       {
          Host_Error("Server sent too many sound precaches (max = %d)",
                max_sounds(cl.protocol));
          return;
       }

       strcpy(sound_precache[numsounds], in);
       S_TouchSound(in);
    }

    /* copy the naked name of the map file to the cl structure */
    mapname = COM_SkipPath(model_precache[1]);
    strlcpy(cl.mapname, mapname, sizeof(cl.mapname));
    COM_StripExtension(cl.mapname);

    /* now we try to load everything else until a cache allocation fails */

    for (i = 1; i < nummodels; i++)
    {
       cl.model_precache[i] = Mod_ForName(model_precache[i], false);
       if (cl.model_precache[i] == NULL)
       {
          Con_Printf("Model %s not found\n", model_precache[i]);
          return;
       }
       CL_KeepaliveMessage();
    }

    S_BeginPrecaching();
    for (i = 1; i < numsounds; i++)
    {
       cl.sound_precache[i] = S_PrecacheSound(sound_precache[i]);
       CL_KeepaliveMessage();
    }
    S_EndPrecaching();


    /* local state */
    cl_entities[0].model = cl.worldmodel = cl.model_precache[1];

    R_NewMap();

    /* make sure nothing is hurt */
    Hunk_Check();		

    /* noclip is turned off at start */
    noclip_anglehack = false;

    for (i = 0; i < MAX_MODELS; i++)
       free(model_precache[i]);
    free(model_precache);
    for (i = 0; i < MAX_SOUNDS; i++)
       free(sound_precache[i]);
    free(sound_precache);
}


static int CL_ReadModelIndex(unsigned int bits)
{
   switch (cl.protocol)
   {
      case PROTOCOL_VERSION_FITZ:
         if (bits & B_FITZ_LARGEMODEL)
            return MSG_ReadShort();
	 /* fall-through */
      case PROTOCOL_VERSION_NQ:
         return MSG_ReadByte();
      case PROTOCOL_VERSION_BJP:
      case PROTOCOL_VERSION_BJP2:
      case PROTOCOL_VERSION_BJP3:
         return MSG_ReadShort();
      default:
         break;
   }

   Host_Error("%s: Unknown protocol version (%d)\n", __func__,
         cl.protocol);
   return 0; /* should never happen */
}

static int CL_ReadModelFrame(unsigned int bits)
{
   switch (cl.protocol)
   {
      case PROTOCOL_VERSION_FITZ:
         if (bits & B_FITZ_LARGEFRAME)
            return MSG_ReadShort();
	 /* fall-through */
      case PROTOCOL_VERSION_NQ:
      case PROTOCOL_VERSION_BJP:
      case PROTOCOL_VERSION_BJP2:
      case PROTOCOL_VERSION_BJP3:
         return MSG_ReadByte();
      default:
         break;
   }

   Host_Error("%s: Unknown protocol version (%d)\n", __func__,
         cl.protocol);
   return 0; /* should never happen */
}

/*
==================
CL_ParseUpdate

Parse an entity update message from the server
If an entities model or origin changes from frame to frame, it must be
relinked.  Other attributes can change without relinking.
==================
*/

void
CL_ParseUpdate(unsigned int bits)
{
   int i;
   model_t *model;
   int modnum;
   qboolean forcelink = false;
   entity_t *ent;
   int num;

   if (cls.state == ca_firstupdate) {
      // first update is the final signon stage
      cls.signon = SIGNONS;
      CL_SignonReply();
   }

   if (bits & U_MOREBITS) {
      i = MSG_ReadByte();
      bits |= (i << 8);
   }

   if (cl.protocol == PROTOCOL_VERSION_FITZ) {
      if (bits & U_FITZ_EXTEND1)
         bits |= MSG_ReadByte() << 16;
      if (bits & U_FITZ_EXTEND2)
         bits |= MSG_ReadByte() << 24;
   }

   if (bits & U_LONGENTITY)
      num = MSG_ReadShort();
   else
      num = MSG_ReadByte();

   ent = CL_EntityNum(num);

   if (ent->msgtime != cl.mtime[1])
      forcelink = true;	// no previous frame to lerp from

   ent->msgtime = cl.mtime[0];

   if (bits & U_MODEL) {
      modnum = CL_ReadModelIndex(0);
      if (modnum >= max_models(cl.protocol))
         Host_Error("CL_ParseModel: bad modnum");
   } else
      modnum = ent->baseline.modelindex;

   if (bits & U_FRAME)
      ent->frame = MSG_ReadByte();
   else
      ent->frame = ent->baseline.frame;

   /* ANIMATION LERPING INFO */
   if (ent->currentframe != ent->frame) {
      /* TODO: invalidate things when they fall off the
         currententities list or haven't been updated for a while */
      ent->previousframe = ent->currentframe;
      ent->previousframetime = ent->currentframetime;
      ent->currentframe = ent->frame;
      ent->currentframetime = cl.time;
   }

   if (bits & U_COLORMAP)
      i = MSG_ReadByte();
   else
      i = ent->baseline.colormap;
   if (!i)
      ent->colormap = vid.colormap;
   else {
      if (i > cl.maxclients)
         Sys_Error("i >= cl.maxclients");
      ent->colormap = cl.players[i - 1].translations;
   }

   if (bits & U_SKIN)
      ent->skinnum = MSG_ReadByte();
   else
      ent->skinnum = ent->baseline.skinnum;

   if (bits & U_EFFECTS)
      ent->effects = MSG_ReadByte();
   else
      ent->effects = ent->baseline.effects;

   // shift the known values for interpolation
   VectorCopy(ent->msg_origins[0], ent->msg_origins[1]);
   VectorCopy(ent->msg_angles[0], ent->msg_angles[1]);

   if (bits & U_ORIGIN1)
      ent->msg_origins[0][0] = MSG_ReadCoord();
   else
      ent->msg_origins[0][0] = ent->baseline.origin[0];
   if (bits & U_ANGLE1)
      ent->msg_angles[0][0] = MSG_ReadAngle();
   else
      ent->msg_angles[0][0] = ent->baseline.angles[0];

   if (bits & U_ORIGIN2)
      ent->msg_origins[0][1] = MSG_ReadCoord();
   else
      ent->msg_origins[0][1] = ent->baseline.origin[1];
   if (bits & U_ANGLE2)
      ent->msg_angles[0][1] = MSG_ReadAngle();
   else
      ent->msg_angles[0][1] = ent->baseline.angles[1];

   if (bits & U_ORIGIN3)
      ent->msg_origins[0][2] = MSG_ReadCoord();
   else
      ent->msg_origins[0][2] = ent->baseline.origin[2];
   if (bits & U_ANGLE3)
      ent->msg_angles[0][2] = MSG_ReadAngle();
   else
      ent->msg_angles[0][2] = ent->baseline.angles[2];

   if (cl.protocol == PROTOCOL_VERSION_FITZ) {
      if (bits & U_FITZ_ALPHA) {
         MSG_ReadByte(); // FIXME - TODO
      }
      if (bits & U_FITZ_FRAME2)
         ent->frame = (ent->frame & 0xFF) | (MSG_ReadByte() << 8);
      if (bits & U_FITZ_MODEL2)
         modnum = (modnum & 0xFF)| (MSG_ReadByte() << 8);
      if (bits & U_FITZ_LERPFINISH) {
         MSG_ReadByte(); // FIXME - TODO
      }
   }

   model = cl.model_precache[modnum];
   if (model != ent->model) {
      ent->model = model;
      // automatic animation (torches, etc) can be either all together
      // or randomized
      if (model) {
         if (model->synctype == ST_RAND)
            ent->syncbase = (float)(rand() & 0x7fff) / 0x7fff;
         else
            ent->syncbase = 0.0;
      } else
         forcelink = true;	// hack to make null model players work
   }

   /* MOVEMENT LERP INFO - could I just extend baseline instead? */
   if (!VectorCompare(ent->msg_origins[0], ent->currentorigin)) {
      if (ent->currentorigintime) {
         VectorCopy(ent->currentorigin, ent->previousorigin);
         ent->previousorigintime = ent->currentorigintime;
      } else {
         VectorCopy(ent->msg_origins[0], ent->previousorigin);
         ent->previousorigintime = cl.mtime[0];
      }
      VectorCopy(ent->msg_origins[0], ent->currentorigin);
      ent->currentorigintime = cl.mtime[0];
   }
   if (!VectorCompare(ent->msg_angles[0], ent->currentangles)) {
      if (ent->currentanglestime) {
         VectorCopy(ent->currentangles, ent->previousangles);
         ent->previousanglestime = ent->currentanglestime;
      } else {
         VectorCopy(ent->msg_angles[0], ent->previousangles);
         ent->previousanglestime = cl.mtime[0];
      }
      VectorCopy(ent->msg_angles[0], ent->currentangles);
      ent->currentanglestime = cl.mtime[0];
   }

   if (bits & U_NOLERP)
      ent->forcelink = true;

   if (forcelink) {		// didn't have an update last message
      VectorCopy(ent->msg_origins[0], ent->msg_origins[1]);
      VectorCopy(ent->msg_origins[0], ent->origin);
      VectorCopy(ent->msg_angles[0], ent->msg_angles[1]);
      VectorCopy(ent->msg_angles[0], ent->angles);
      ent->forcelink = true;
   }
}

/*
==================
CL_ParseBaseline
==================
*/
static void
CL_ParseBaseline(entity_t *ent, unsigned int bits)
{
   int i;

   ent->baseline.modelindex = CL_ReadModelIndex(bits);
   ent->baseline.frame      = CL_ReadModelFrame(bits);
   ent->baseline.colormap   = MSG_ReadByte();
   ent->baseline.skinnum    = MSG_ReadByte();
   for (i = 0; i < 3; i++)
   {
      ent->baseline.origin[i] = MSG_ReadCoord();
      ent->baseline.angles[i] = MSG_ReadAngle();
   }

   if (cl.protocol == PROTOCOL_VERSION_FITZ && (bits & B_FITZ_ALPHA))
      MSG_ReadByte(); // FIXME - TODO
}


/*
==================
CL_ParseClientdata

Server information pertaining to this client only
==================
*/
void
CL_ParseClientdata(void)
{
    int i, j;
    unsigned int bits = (unsigned short)MSG_ReadShort();

    if (bits & SU_FITZ_EXTEND1)
	bits |= MSG_ReadByte() << 16;
    if (bits & SU_FITZ_EXTEND2)
	bits |= MSG_ReadByte() << 24;

    if (bits & SU_VIEWHEIGHT)
	cl.viewheight = MSG_ReadChar();
    else
	cl.viewheight = DEFAULT_VIEWHEIGHT;

    if (bits & SU_IDEALPITCH)
	cl.idealpitch = MSG_ReadChar();
    else
	cl.idealpitch = 0;

    VectorCopy(cl.mvelocity[0], cl.mvelocity[1]);
    for (i = 0; i < 3; i++) {
	if (bits & (SU_PUNCH1 << i))
	    cl.punchangle[i] = MSG_ReadChar();
	else
	    cl.punchangle[i] = 0;
	if (bits & (SU_VELOCITY1 << i))
	    cl.mvelocity[0][i] = MSG_ReadChar() * 16;
	else
	    cl.mvelocity[0][i] = 0;
    }

// [always sent]        if (bits & SU_ITEMS)
    i = MSG_ReadLong();

    if (cl.stats[STAT_ITEMS] != i) {	// set flash times
	Sbar_Changed();
	for (j = 0; j < 32; j++)
	    if ((i & (1 << j)) && !(cl.stats[STAT_ITEMS] & (1 << j)))
		cl.item_gettime[j] = cl.time;
	cl.stats[STAT_ITEMS] = i;
    }

    cl.onground = (bits & SU_ONGROUND) != 0;
    cl.inwater = (bits & SU_INWATER) != 0;

    if (bits & SU_WEAPONFRAME)
	cl.stats[STAT_WEAPONFRAME] = MSG_ReadByte();
    else
	cl.stats[STAT_WEAPONFRAME] = 0;

    if (bits & SU_ARMOR)
	i = MSG_ReadByte();
    else
	i = 0;
    if (cl.stats[STAT_ARMOR] != i) {
	cl.stats[STAT_ARMOR] = i;
	Sbar_Changed();
    }

    if (bits & SU_WEAPON)
	i = CL_ReadModelIndex(0);
    else
	i = 0;
    if (cl.stats[STAT_WEAPON] != i) {
	cl.stats[STAT_WEAPON] = i;
	Sbar_Changed();
    }

    i = MSG_ReadShort();
    if (cl.stats[STAT_HEALTH] != i) {
	cl.stats[STAT_HEALTH] = i;
	Sbar_Changed();
    }

    i = MSG_ReadByte();
    if (cl.stats[STAT_AMMO] != i) {
	cl.stats[STAT_AMMO] = i;
	Sbar_Changed();
    }

    for (i = 0; i < 4; i++) {
	j = MSG_ReadByte();
	if (cl.stats[STAT_SHELLS + i] != j) {
	    cl.stats[STAT_SHELLS + i] = j;
	    Sbar_Changed();
	}
    }

    i = MSG_ReadByte();

    if (standard_quake) {
	if (cl.stats[STAT_ACTIVEWEAPON] != i) {
	    cl.stats[STAT_ACTIVEWEAPON] = i;
	    Sbar_Changed();
	}
    } else {
	if (cl.stats[STAT_ACTIVEWEAPON] != (1 << i)) {
	    cl.stats[STAT_ACTIVEWEAPON] = (1 << i);
	    Sbar_Changed();
	}
    }

    /* FITZ Protocol */
    if (bits & SU_FITZ_WEAPON2)
	cl.stats[STAT_WEAPON] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_ARMOR2)
	cl.stats[STAT_ARMOR] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_AMMO2)
	cl.stats[STAT_AMMO] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_SHELLS2)
	cl.stats[STAT_SHELLS] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_NAILS2)
	cl.stats[STAT_NAILS] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_ROCKETS2)
	cl.stats[STAT_ROCKETS] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_CELLS2)
	cl.stats[STAT_CELLS] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_WEAPONFRAME2)
	cl.stats[STAT_WEAPONFRAME] |= MSG_ReadByte() << 8;
    if (bits & SU_FITZ_WEAPONALPHA)
	MSG_ReadByte(); // FIXME - TODO
}

/*
=====================
CL_NewTranslation
=====================
*/
void
CL_NewTranslation(int slot)
{
   int i, j;
   int top, bottom;
   byte *dest, *source;

   if (slot > cl.maxclients)
      Sys_Error("%s: slot > cl.maxclients", __func__);
   dest = cl.players[slot].translations;
   source = vid.colormap;
   memcpy(dest, vid.colormap, sizeof(cl.players[slot].translations));
   top = cl.players[slot].topcolor;
   bottom = cl.players[slot].bottomcolor;

   for (i = 0; i < VID_GRADES; i++, dest += 256, source += 256)
   {
      if (top < 128) // the artists made some backwards ranges.  sigh.
         memcpy(dest + TOP_RANGE, source + top, 16);
      else
         for (j = 0; j < 16; j++)
            dest[TOP_RANGE + j] = source[top + 15 - j];

      if (bottom < 128)
         memcpy(dest + BOTTOM_RANGE, source + bottom, 16);
      else
         for (j = 0; j < 16; j++)
            dest[BOTTOM_RANGE + j] = source[bottom + 15 - j];
   }
}

/*
=====================
CL_ParseStatic
=====================
*/
void
CL_ParseStatic(unsigned int bits)
{
    entity_t *ent;
    int i = cl.num_statics;
    if (i >= MAX_STATIC_ENTITIES)
	Host_Error("Too many static entities");
    ent = &cl_static_entities[i];
    cl.num_statics++;
    CL_ParseBaseline(ent, bits);

// copy it to the current state
    ent->model = cl.model_precache[ent->baseline.modelindex];
    ent->frame = ent->baseline.frame;
    ent->colormap = vid.colormap;
    ent->skinnum = ent->baseline.skinnum;
    ent->effects = ent->baseline.effects;

    /* Initilise frames for model lerp */
    ent->currentframe = ent->baseline.frame;
    ent->previousframe = ent->baseline.frame;
    ent->currentframetime = cl.time;
    ent->previousframetime = cl.time;

    /* Initialise movelerp data */
    ent->previousorigintime = cl.time;
    ent->currentorigintime = cl.time;
    VectorCopy(ent->baseline.origin, ent->previousorigin);
    VectorCopy(ent->baseline.origin, ent->currentorigin);
    VectorCopy(ent->baseline.angles, ent->previousangles);
    VectorCopy(ent->baseline.angles, ent->currentangles);

    VectorCopy(ent->baseline.origin, ent->origin);
    VectorCopy(ent->baseline.angles, ent->angles);
    R_AddEfrags(ent);
}


static int CL_ReadSoundNum_Static(void)
{
   switch (cl.protocol)
   {
      case PROTOCOL_VERSION_NQ:
      case PROTOCOL_VERSION_BJP:
      case PROTOCOL_VERSION_BJP3:
      case PROTOCOL_VERSION_FITZ:
         return MSG_ReadByte();
      case PROTOCOL_VERSION_BJP2:
         return MSG_ReadShort();
      default:
         break;
   }

   Host_Error("%s: Unknown protocol version (%d)\n", __func__,
         cl.protocol);
   return 0; /* should never happen */
}

/*
===================
CL_ParseStaticSound
===================
*/
static void
CL_ParseStaticSound(void)
{
    vec3_t org;
    int sound_num, vol, atten;
    int i;

    for (i = 0; i < 3; i++)
	org[i] = MSG_ReadCoord();
    sound_num  = CL_ReadSoundNum_Static();
    vol        = MSG_ReadByte();
    atten      = MSG_ReadByte();

    S_StaticSound(cl.sound_precache[sound_num], org, vol, atten);
}

/* FITZ protocol */
static void
CL_ParseFitzStaticSound2(void)
{
    vec3_t org;
    int sound_num, vol, atten;
    int i;

    for (i = 0; i < 3; i++)
	org[i] = MSG_ReadCoord();
    sound_num  = MSG_ReadShort();
    vol        = MSG_ReadByte();
    atten      = MSG_ReadByte();

    S_StaticSound(cl.sound_precache[sound_num], org, vol, atten);
}

/*
=====================
CL_ParseServerMessage
=====================
*/
void
CL_ParseServerMessage(void)
{
   char *s;
   int i;
   unsigned int bits;
   byte colors;

   cl.onground = false;	// unless the server says otherwise
   // parse the message
   MSG_BeginReading();

   while (1)
   {
      int cmd;

      if (msg_badread)
         Host_Error("%s: Bad server message", __func__);

      cmd = MSG_ReadByte();

      if (cmd == -1) 
         return;		// end of message
      // if the high bit of the command byte is set, it is a fast update
      if (cmd & 128) {
         CL_ParseUpdate(cmd & 127);
         continue;
      }

      // other commands
      switch (cmd) {
         case svc_nop:
            break;

         case svc_time:
            cl.mtime[1] = cl.mtime[0];
            cl.mtime[0] = MSG_ReadFloat();
            break;

         case svc_clientdata:
            CL_ParseClientdata();
            break;

         case svc_version:
            i = MSG_ReadLong();
            if (!Protocol_Known(i))
               Host_Error("%s: Server returned unknown protocol version %i",
                     __func__, i);
            cl.protocol = i;
            break;

         case svc_disconnect:
            Host_EndGame("Server disconnected\n");

         case svc_print:
            Con_Printf("%s", MSG_ReadString());
            break;

         case svc_centerprint:
            SCR_CenterPrint(MSG_ReadString());
            break;

         case svc_stufftext:
            Cbuf_AddText("%s", MSG_ReadString());
            break;

         case svc_damage:
            V_ParseDamage();
            break;

         case svc_serverinfo:
            CL_ParseServerInfo();
            vid.recalc_refdef = true;	// leave intermission full screen
            break;

         case svc_setangle:
            for (i = 0; i < 3; i++)
               cl.viewangles[i] = MSG_ReadAngle();
            break;

         case svc_setview:
            cl.viewentity = MSG_ReadShort();
            break;

         case svc_lightstyle:
            i = MSG_ReadByte();
            if (i >= MAX_LIGHTSTYLES)
               Sys_Error("svc_lightstyle > MAX_LIGHTSTYLES");
            s = MSG_ReadString();
            strlcpy(cl_lightstyle[i].map, s, MAX_STYLESTRING);
            cl_lightstyle[i].length = strlen(cl_lightstyle[i].map);
            break;

         case svc_sound:
            CL_ParseStartSoundPacket();
            break;

         case svc_stopsound:
            i = MSG_ReadShort();
            S_StopSound(i >> 3, i & 7);
            break;

         case svc_updatename:
            Sbar_Changed();
            i = MSG_ReadByte();
            if (i >= cl.maxclients)
               Host_Error("%s: svc_updatename > MAX_SCOREBOARD", __func__);
            s = MSG_ReadString();
            strlcpy(cl.players[i].name, s, MAX_SCOREBOARDNAME);
            break;

         case svc_updatefrags:
            Sbar_Changed();
            i = MSG_ReadByte();
            if (i >= cl.maxclients)
               Host_Error("%s: svc_updatefrags > MAX_SCOREBOARD", __func__);
            cl.players[i].frags = MSG_ReadShort();
            break;

         case svc_updatecolors:
            Sbar_Changed();
            i = MSG_ReadByte();
            if (i >= cl.maxclients)
               Host_Error("%s: svc_updatecolors > MAX_SCOREBOARD", __func__);
            colors = MSG_ReadByte();
            cl.players[i].topcolor = (colors & 0xf0) >> 4;
            cl.players[i].bottomcolor = colors & 0x0f;
            CL_NewTranslation(i);
            break;

         case svc_particle:
            R_ParseParticleEffect();
            break;

         case svc_spawnbaseline:
            i = MSG_ReadShort();
            // must use CL_EntityNum() to force cl.num_entities up
            CL_ParseBaseline(CL_EntityNum(i), 0);
            break;

         case svc_fitz_spawnbaseline2:
            /* FIXME - check here that protocol is FITZ? => Host_Error() */
            i = MSG_ReadShort();
            bits = MSG_ReadByte();
            // must use CL_EntityNum() to force cl.num_entities up
            CL_ParseBaseline(CL_EntityNum(i), bits);
            break;

         case svc_spawnstatic:
            CL_ParseStatic(0);
            break;

         case svc_fitz_spawnstatic2:
            /* FIXME - check here that protocol is FITZ? => Host_Error() */
            bits = MSG_ReadByte();
            CL_ParseStatic(bits);
            break;

         case svc_temp_entity:
            CL_ParseTEnt();
            break;

         case svc_setpause:
            cl.paused = MSG_ReadByte();
            if (cl.paused)
            {
               CDAudio_Pause();
               BGM_Pause();
            }
            else
            {
               CDAudio_Resume();
               BGM_Resume();
            }
            break;

         case svc_signonnum:
            i = MSG_ReadByte();
            if (i <= cls.signon)
               Host_Error("Received signon %i when at %i", i, cls.signon);
            cls.signon = i;
            CL_SignonReply();
            break;

         case svc_killedmonster:
            cl.stats[STAT_MONSTERS]++;
            break;

         case svc_foundsecret:
            cl.stats[STAT_SECRETS]++;
            break;

         case svc_updatestat:
            i = MSG_ReadByte();
            if (i < 0 || i >= MAX_CL_STATS)
               Sys_Error("svc_updatestat: %i is invalid", i);
            cl.stats[i] = MSG_ReadLong();
            break;

         case svc_spawnstaticsound:
            CL_ParseStaticSound();
            break;

         case svc_fitz_spawnstaticsound2:
            /* FIXME - check here that protocol is FITZ? => Host_Error() */
            CL_ParseFitzStaticSound2();
            break;

         case svc_cdtrack:
            cl.cdtrack = MSG_ReadByte();
            cl.looptrack = MSG_ReadByte();
            if ((cls.demoplayback)
                  && (cls.forcetrack != -1))
               BGM_PlayCDtrack ((byte)cls.forcetrack, true);
			else
               BGM_PlayCDtrack ((byte)cl.cdtrack, true);
            break;

         case svc_intermission:
            cl.intermission = 1;
            cl.completed_time = cl.time;
            vid.recalc_refdef = true;	// go to full screen
            break;

         case svc_finale:
            cl.intermission = 2;
            cl.completed_time = cl.time;
            vid.recalc_refdef = true;	// go to full screen
            SCR_CenterPrint(MSG_ReadString());
            break;

         case svc_cutscene:
            cl.intermission = 3;
            cl.completed_time = cl.time;
            vid.recalc_refdef = true;	// go to full screen
            SCR_CenterPrint(MSG_ReadString());
            break;

         case svc_sellscreen:
            Cmd_ExecuteString("help", src_command);
            break;

            /* Various FITZ protocol messages - FIXME - !protocol => Host_Error */
         case svc_fitz_skybox:
            MSG_ReadString(); // FIXME - TODO
            break;

         case svc_fitz_bf:
            Cmd_ExecuteString("bf", src_command);
            break;

         case svc_fitz_fog:
            /* FIXME - TODO */
            MSG_ReadByte(); // density
            MSG_ReadByte(); // red
            MSG_ReadByte(); // green
            MSG_ReadByte(); // blue
            MSG_ReadShort(); // time
            break;

         default:
            Host_Error("%s: Illegible server message", __func__);
      }
   }
}
