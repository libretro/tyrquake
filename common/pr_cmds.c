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

#include <float.h>
#include <compat/strl.h>

#include "cmd.h"
#include "console.h"
#include "model.h"
#include "progs.h"
#include "server.h"
#include "world.h"

#ifdef NQ_HACK
#include "host.h"
#include "protocol.h"
#include "quakedef.h"
#include "sys.h"
/* FIXME - quick hack to enable merging of NQ/QWSV shared code */
#define SV_Error Host_Error
#endif
#ifdef QW_HACK
#include "qwsvdef.h"
#endif

#define	RETURN_EDICT(e) (((int *)pr_globals)[OFS_RETURN] = EDICT_TO_PROG(e))
#define	RETURN_STRING(s) (((int *)pr_globals)[OFS_RETURN] = PR_SetString(s))

/*
===============================================================================

						BUILT-IN FUNCTIONS

===============================================================================
*/

static const char *
PF_VarString(int first)
{
    static char out[512];
    const char *arg;
    int i, buflen, arglen;

    buflen = sizeof(out) - 1;
    out[0] = 0;
    for (i = first; i < pr_argc; i++) {
	arg = G_STRING(OFS_PARM0 + i * 3);
	arglen = strlen(arg);
	strncat(out, arg, buflen);
	buflen -= arglen;
	if (buflen < 0)
	    break;
    }
    return out;
}


/*
=================
PF_errror

This is a TERMINAL error, which will kill off the entire server.
Dumps self.

error(value)
=================
*/
static void
PF_error(void)
{
    const char *s = PF_VarString(0);
    Con_Printf("======SERVER ERROR in %s:\n%s\n",
	       PR_GetString(pr_xfunction->s_name), s);
    SV_Error("Program error");
}

/*
=================
PF_objerror

Dumps out self, then an error message.  The program is aborted and self is
removed, but the level can continue.

objerror(value)
=================
*/
static void
PF_objerror(void)
{
    const char *s;
    edict_t *ed;

    s = PF_VarString(0);
    Con_Printf("======OBJECT ERROR in %s:\n%s\n",
	       PR_GetString(pr_xfunction->s_name), s);
    ed = PROG_TO_EDICT(pr_global_struct->self);
    ED_Free(ed);

    SV_Error("Program error");
}



/*
==============
PF_makevectors

Writes new values for v_forward, v_up, and v_right based on angles
makevectors(vector)
==============
*/
static void
PF_makevectors(void)
{
    AngleVectors(G_VECTOR(OFS_PARM0), pr_global_struct->v_forward,
		 pr_global_struct->v_right, pr_global_struct->v_up);
}

/*
=================
PF_setorigin

This is the only valid way to move an object without using the physics of the
world (setting velocity and waiting).  Directly changing origin will not set
internal links correctly, so clipping would be messed up.  This should be
called when an object is spawned, and then only if it is teleported.

setorigin (entity, origin)
=================
*/
static void
PF_setorigin(void)
{
    edict_t *e;
    float *org;

    e = G_EDICT(OFS_PARM0);
    org = G_VECTOR(OFS_PARM1);
    VectorCopy(org, e->v.origin);
    SV_LinkEdict(e, false);
}

#ifdef NQ_HACK
static void
SetMinMaxSize(edict_t *e, float *min, float *max, qboolean rotate)
{
    float *angles;
    vec3_t rmin, rmax;
    float bounds[2][3];
    float xvector[2], yvector[2];
    float a;
    vec3_t base, transformed;
    int i, j, k, l;

    for (i = 0; i < 3; i++)
	if (min[i] > max[i])
	    PR_RunError("backwards mins/maxs");

    rotate = false;		// FIXME: implement rotation properly again

    if (!rotate) {
	VectorCopy(min, rmin);
	VectorCopy(max, rmax);
    } else {
	// find min / max for rotations
	angles = e->v.angles;

	a = angles[1] / 180 * M_PI;

	xvector[0] = cos(a);
	xvector[1] = sin(a);
	yvector[0] = -sin(a);
	yvector[1] = cos(a);

	VectorCopy(min, bounds[0]);
	VectorCopy(max, bounds[1]);

	rmin[0] = rmin[1] = rmin[2] = FLT_MAX;
	rmax[0] = rmax[1] = rmax[2] = -FLT_MAX;

	for (i = 0; i <= 1; i++) {
	    base[0] = bounds[i][0];
	    for (j = 0; j <= 1; j++) {
		base[1] = bounds[j][1];
		for (k = 0; k <= 1; k++) {
		    base[2] = bounds[k][2];

		    // transform the point
		    transformed[0] =
			xvector[0] * base[0] + yvector[0] * base[1];
		    transformed[1] =
			xvector[1] * base[0] + yvector[1] * base[1];
		    transformed[2] = base[2];

		    for (l = 0; l < 3; l++) {
			if (transformed[l] < rmin[l])
			    rmin[l] = transformed[l];
			if (transformed[l] > rmax[l])
			    rmax[l] = transformed[l];
		    }
		}
	    }
	}
    }

// set derived values
    VectorCopy(rmin, e->v.mins);
    VectorCopy(rmax, e->v.maxs);
    VectorSubtract(max, min, e->v.size);

    SV_LinkEdict(e, false);
}
#endif

/*
=================
PF_setsize

the size box is rotated by the current angle

setsize (entity, minvector, maxvector)
=================
*/
static void
PF_setsize(void)
{
    edict_t *e = G_EDICT(OFS_PARM0);
    float *min = G_VECTOR(OFS_PARM1);
    float *max = G_VECTOR(OFS_PARM2);
#ifdef NQ_HACK
    SetMinMaxSize(e, min, max, false);
#endif
#ifdef QW_HACK
    VectorCopy(min, e->v.mins);
    VectorCopy(max, e->v.maxs);
    VectorSubtract(max, min, e->v.size);
    SV_LinkEdict(e, false);
#endif
}


/*
=================
PF_setmodel

setmodel(entity, model)
Also sets size, mins, and maxs for inline bmodels
=================
*/
static void
PF_setmodel(void)
{
    int i;
    model_t *mod;
    const char **check;
    edict_t *e    = G_EDICT(OFS_PARM0);
    const char *m = G_STRING(OFS_PARM1);

    /* check to see if model was properly precached */
    for (i = 0, check = sv.model_precache; *check; i++, check++)
	if (!strcmp(*check, m))
	    break;

    if (!*check)
	PR_RunError("no precache: %s\n", m);

    e->v.model = PR_SetString(m);
    e->v.modelindex = i;

#ifdef NQ_HACK
    mod = sv.models[(int)e->v.modelindex];
    if (mod)
	SetMinMaxSize(e, mod->mins, mod->maxs, true);
    else
	SetMinMaxSize(e, vec3_origin, vec3_origin, true);
#endif
#ifdef QW_HACK
    /* if it is an inline model, get the size information for it */
    if (m[0] == '*') {
	mod = Mod_ForName(m, true);
	VectorCopy(mod->mins, e->v.mins);
	VectorCopy(mod->maxs, e->v.maxs);
	VectorSubtract(mod->maxs, mod->mins, e->v.size);
	SV_LinkEdict(e, false);
    }
#endif
}

/*
=================
PF_bprint

broadcast print to everyone on server

bprint(value)
=================
*/
static void
PF_bprint(void)
{
#ifdef NQ_HACK
    const char *s = PF_VarString(0);
#endif
#ifdef QW_HACK
    int level     = G_FLOAT(OFS_PARM0);
    const char *s = PF_VarString(1);
#endif
#ifdef NQ_HACK
    SV_BroadcastPrintf("%s", s);
#endif
#ifdef QW_HACK
    SV_BroadcastPrintf(level, "%s", s);
#endif
}

/*
=================
PF_sprint

single print to a specific client

sprint(clientent, value)
=================
*/
static void
PF_sprint(void)
{
#ifdef NQ_HACK
    client_t *client;
    int entnum    = G_EDICTNUM(OFS_PARM0);
    const char *s = PF_VarString(1);

    if (entnum < 1 || entnum > svs.maxclients) {
	Con_Printf("tried to sprint to a non-client\n");
	return;
    }

    client = &svs.clients[entnum - 1];
    MSG_WriteChar(&client->message, svc_print);
    MSG_WriteString(&client->message, s);
#endif
#ifdef QW_HACK
    client_t *client;
    int entnum    = G_EDICTNUM(OFS_PARM0);
    int level     = G_FLOAT(OFS_PARM1);
    const char *s = PF_VarString(2);

    if (entnum < 1 || entnum > MAX_CLIENTS) {
	Con_Printf("tried to sprint to a non-client\n");
	return;
    }

    client = &svs.clients[entnum - 1];
    SV_ClientPrintf(client, level, "%s", s);
#endif
}


/*
=================
PF_centerprint

single print to a specific client

centerprint(clientent, value)
=================
*/
static void
PF_centerprint(void)
{
    client_t *client;
    int entnum    = G_EDICTNUM(OFS_PARM0);
    const char *s = PF_VarString(1);

#ifdef NQ_HACK
    if (entnum < 1 || entnum > svs.maxclients) {
#endif
#ifdef QW_HACK
    if (entnum < 1 || entnum > MAX_CLIENTS) {
#endif
	Con_Printf("tried to sprint to a non-client\n");
	return;
    }

    client = &svs.clients[entnum - 1];
#ifdef NQ_HACK
    MSG_WriteChar(&client->message, svc_centerprint);
    MSG_WriteString(&client->message, s);
#endif
#ifdef QW_HACK
    ClientReliableWrite_Begin(client, svc_centerprint, 2 + strlen(s));
    ClientReliableWrite_String(client, s);
#endif
}


/*
=================
PF_normalize

vector normalize(vector)
=================
*/
static void
PF_normalize(void)
{
    vec3_t newvalue;
    float *value1 = G_VECTOR(OFS_PARM0);
    float newval  =
	value1[0] * value1[0] + value1[1] * value1[1] + value1[2] * value1[2];
    newval = sqrt(newval);

    if (newval == 0)
	newvalue[0] = newvalue[1] = newvalue[2] = 0;
    else {
	newval = 1 / newval;
	newvalue[0] = value1[0] * newval;
	newvalue[1] = value1[1] * newval;
	newvalue[2] = value1[2] * newval;
    }

    VectorCopy(newvalue, G_VECTOR(OFS_RETURN));
}

/*
=================
PF_vlen

scalar vlen(vector)
=================
*/
static void
PF_vlen(void)
{
    float *value1 = G_VECTOR(OFS_PARM0);
    float newobj  =
	value1[0] * value1[0] + value1[1] * value1[1] + value1[2] * value1[2];
    newobj = sqrt(newobj);

    G_FLOAT(OFS_RETURN) = newobj;
}

/*
=================
PF_vectoyaw

float vectoyaw(vector)
=================
*/
static void
PF_vectoyaw(void)
{
    float yaw;
    float *value1 = G_VECTOR(OFS_PARM0);

    if (value1[1] == 0 && value1[0] == 0)
	yaw = 0;
    else {
	yaw = (int)(atan2(value1[1], value1[0]) * 180 / M_PI);
	if (yaw < 0)
	    yaw += 360;
    }

    G_FLOAT(OFS_RETURN) = yaw;
}


/*
=================
PF_vectoangles

vector vectoangles(vector)
=================
*/
static void
PF_vectoangles(void)
{
    float forward;
    float yaw, pitch;
    float *value1 = G_VECTOR(OFS_PARM0);

    if (value1[1] == 0 && value1[0] == 0) {
	yaw = 0;
	if (value1[2] > 0)
	    pitch = 90;
	else
	    pitch = 270;
    } else {
	yaw = (int)(atan2(value1[1], value1[0]) * 180 / M_PI);
	if (yaw < 0)
	    yaw += 360;

	forward = sqrt(value1[0] * value1[0] + value1[1] * value1[1]);
	pitch = (int)(atan2(value1[2], forward) * 180 / M_PI);
	if (pitch < 0)
	    pitch += 360;
    }

    G_FLOAT(OFS_RETURN + 0) = pitch;
    G_FLOAT(OFS_RETURN + 1) = yaw;
    G_FLOAT(OFS_RETURN + 2) = 0;
}

/*
=================
PF_Random

Returns a number from 0<= num < 1

random()
=================
*/
static void
PF_random(void)
{
    float num = (rand() & 0x7fff) / ((float)0x7fff);

    G_FLOAT(OFS_RETURN) = num;
}

#ifdef NQ_HACK
/*
=================
PF_particle

particle(origin, color, count)
=================
*/
static void
PF_particle(void)
{
    float *org  = G_VECTOR(OFS_PARM0);
    float *dir  = G_VECTOR(OFS_PARM1);
    float color = G_FLOAT(OFS_PARM2);
    float count = G_FLOAT(OFS_PARM3);
    SV_StartParticle(org, dir, color, count);
}

static void
PF_WriteSoundNum_Static(sizebuf_t *sb, int c)
{
    switch (sv.protocol) {
    case PROTOCOL_VERSION_NQ:
    case PROTOCOL_VERSION_BJP:
    case PROTOCOL_VERSION_BJP3:
	MSG_WriteByte(sb, c);
	break;
    case PROTOCOL_VERSION_BJP2:
	MSG_WriteShort(sb, c);
	break;
    case PROTOCOL_VERSION_FITZ:
	if (c > 255)
	    MSG_WriteShort(sb, c);
	else
	    MSG_WriteByte(sb, c);
	break;
    default:
	Host_Error("%s: Unknown protocol version (%d)\n", __func__,
		   sv.protocol);
    }
}
#endif

/*
=================
PF_ambientsound

=================
*/
static void
PF_ambientsound(void)
{
    int i, soundnum;
    const char **check;
    float *pos        = G_VECTOR(OFS_PARM0);
    const char *samp  = G_STRING(OFS_PARM1);
    float vol         = G_FLOAT(OFS_PARM2);
    float attenuation = G_FLOAT(OFS_PARM3);

    // check to see if samp was properly precached
    for (soundnum = 0, check = sv.sound_precache; *check; check++, soundnum++)
	if (!strcmp(*check, samp))
	    break;

    if (!*check) {
	Con_Printf("no precache: %s\n", samp);
	return;
    }

// add an svc_spawnambient command to the level signon packet
#ifdef NQ_HACK
    if (sv.protocol == PROTOCOL_VERSION_FITZ && soundnum > 255)
	MSG_WriteByte(&sv.signon, svc_fitz_spawnstaticsound2);
    else
	MSG_WriteByte(&sv.signon, svc_spawnstaticsound);
#endif
#ifdef QW_HACK
    MSG_WriteByte(&sv.signon, svc_spawnstaticsound);
#endif
    for (i = 0; i < 3; i++)
	MSG_WriteCoord(&sv.signon, pos[i]);

#ifdef NQ_HACK
    PF_WriteSoundNum_Static(&sv.signon, soundnum);
#endif
#ifdef QW_HACK
    MSG_WriteByte(&sv.signon, soundnum);
#endif
    MSG_WriteByte(&sv.signon, vol * 255);
    MSG_WriteByte(&sv.signon, attenuation * 64);
}

/*
=================
PF_sound

Each entity can have eight independant sound sources, like voice,
weapon, feet, etc.

Channel 0 is an auto-allocate channel, the others override anything
allready running on that entity/channel pair.

An attenuation of 0 will play full volume everywhere in the level.
Larger attenuations will drop off.

=================
*/
static void
PF_sound(void)
{
    edict_t *entity    = G_EDICT(OFS_PARM0);
    int channel        = G_FLOAT(OFS_PARM1);
    const char *sample = G_STRING(OFS_PARM2);
    int volume         = G_FLOAT(OFS_PARM3) * 255;
    float attenuation  = G_FLOAT(OFS_PARM4);

#ifdef NQ_HACK
    if (volume < 0 || volume > 255)
	Sys_Error("%s: volume = %i", __func__, volume);

    if (attenuation < 0 || attenuation > 4)
	Sys_Error("%s: attenuation = %f", __func__, attenuation);

    if (channel < 0 || channel > 7)
	Sys_Error("%s: channel = %i", __func__, channel);
#endif

    SV_StartSound(entity, channel, sample, volume, attenuation);
}

/*
=================
PF_break

break()
=================
*/
static void PF_break(void)
{
    Con_Printf("break statement\n");
    abort(); /* dump to debugger */
}

/*
=================
PF_traceline

Used for use tracing and shot targeting
Traces are blocked by bbox and exact bsp entityes, and also slide box entities
if the tryents flag is set.

traceline (vector1, vector2, tryents)
=================
*/
static void
PF_traceline(void)
{
    float *v1      = G_VECTOR(OFS_PARM0);
    float *v2      = G_VECTOR(OFS_PARM1);
    int nomonsters = G_FLOAT(OFS_PARM2);
    edict_t *ent   = G_EDICT(OFS_PARM3);
    trace_t trace  = SV_Move(v1, vec3_origin, vec3_origin, v2, nomonsters, ent);

    pr_global_struct->trace_allsolid = trace.allsolid;
    pr_global_struct->trace_startsolid = trace.startsolid;
    pr_global_struct->trace_fraction = trace.fraction;
    pr_global_struct->trace_inwater = trace.inwater;
    pr_global_struct->trace_inopen = trace.inopen;
    VectorCopy(trace.endpos, pr_global_struct->trace_endpos);
    VectorCopy(trace.plane.normal, pr_global_struct->trace_plane_normal);
    pr_global_struct->trace_plane_dist = trace.plane.dist;
    if (trace.ent)
	pr_global_struct->trace_ent = EDICT_TO_PROG(trace.ent);
    else
	pr_global_struct->trace_ent = EDICT_TO_PROG(sv.edicts);
}

//============================================================================

static int
PF_newcheckclient(int check)
{
    int entnum;
    edict_t *ent;
    vec3_t org;

// cycle to the next one
#ifdef NQ_HACK
    check = qclamp(check, 1, svs.maxclients);
    entnum = (check == svs.maxclients) ? 1 : check + 1;
#endif
#ifdef QW_HACK
    check = qclamp(check, 1, MAX_CLIENTS);
    entnum = (check == MAX_CLIENTS) ? 1 : check + 1;
#endif

    for (;; entnum++) {
#ifdef NQ_HACK
	if (entnum == svs.maxclients + 1)
	    entnum = 1;
#endif
#ifdef QW_HACK
	if (entnum == MAX_CLIENTS + 1)
	    entnum = 1;
#endif

	ent = EDICT_NUM(entnum);

	if (entnum == check)
	    break;		// didn't find anything else

	if (ent->free)
	    continue;
	if (ent->v.health <= 0)
	    continue;
	if ((int)ent->v.flags & FL_NOTARGET)
	    continue;

	// anything that is a client, or has a client as an enemy
	break;
    }

// get the current leaf for the entity
    VectorAdd(ent->v.origin, ent->v.view_ofs, org);
    sv.checkleaf = Mod_PointInLeaf(sv.worldmodel, org);

    return entnum;
}

/*
=================
PF_checkclient

Returns a client (or object that has a client enemy) that would be a
valid target.

If there are more than one valid options, they are cycled each frame

If (self.origin + self.viewofs) is not in the PVS of the current target,
it is not returned at all.

name checkclient ()
=================
*/
#define	MAX_CHECK	16
static int c_invis, c_notvis;
static void
PF_checkclient(void)
{
    edict_t *ent, *self;
    mleaf_t *leaf;
    int leafnum;
    const leafbits_t *checkpvs;
    vec3_t view;

// find a new check if on a new frame
    if (sv.time - sv.lastchecktime >= 0.1) {
	sv.lastcheck = PF_newcheckclient(sv.lastcheck);
	sv.lastchecktime = sv.time;
    }
// return check if it might be visible
    ent = EDICT_NUM(sv.lastcheck);
    if (ent->free || ent->v.health <= 0) {
	RETURN_EDICT(sv.edicts);
	return;
    }
// if current entity can't possibly see the check entity, return 0
    checkpvs = Mod_LeafPVS(sv.worldmodel, sv.checkleaf);
    self = PROG_TO_EDICT(pr_global_struct->self);
    VectorAdd(self->v.origin, self->v.view_ofs, view);
    leaf = Mod_PointInLeaf(sv.worldmodel, view);
    leafnum = (leaf - sv.worldmodel->leafs) - 1;
    if (leafnum < 0 || !Mod_TestLeafBit(checkpvs, leafnum)) {
	c_notvis++;
	RETURN_EDICT(sv.edicts);
	return;
    }
// might be able to see it
    c_invis++;
    RETURN_EDICT(ent);
}

//============================================================================


/*
=================
PF_stuffcmd

Sends text over to the client's execution buffer

stuffcmd (clientent, value)
=================
*/
static void
PF_stuffcmd(void)
{
    const char *str;
    client_t *client;
    int entnum = G_EDICTNUM(OFS_PARM0);
#ifdef NQ_HACK
    if (entnum < 1 || entnum > svs.maxclients)
	PR_RunError("Parm 0 not a client");
#endif
#ifdef QW_HACK
    if (entnum < 1 || entnum > MAX_CLIENTS)
	PR_RunError("Parm 0 not a client");
#endif
    str = G_STRING(OFS_PARM1);

#ifdef NQ_HACK
    client = host_client;
    host_client = &svs.clients[entnum - 1];
    Host_ClientCommands("%s", str);
    host_client = client;
#endif
#ifdef QW_HACK
    client = &svs.clients[entnum - 1];
    if (strcmp(str, "disconnect\n") == 0) {
	// so long and thanks for all the fish
	client->drop = true;
	return;
    }
    ClientReliableWrite_Begin(client, svc_stufftext, 2 + strlen(str));
    ClientReliableWrite_String(client, str);
#endif
}

/*
=================
PF_localcmd

Sends text over to the client's execution buffer

localcmd (string)
=================
*/
static void
PF_localcmd(void)
{
    Cbuf_AddText("%s", G_STRING(OFS_PARM0));
}

/*
=================
PF_cvar

float cvar (string)
=================
*/
static void
PF_cvar(void)
{
    const char *var     = G_STRING(OFS_PARM0);

    G_FLOAT(OFS_RETURN) = Cvar_VariableValue(var);
}

/*
=================
PF_cvar_set

float cvar (string)
=================
*/
static void
PF_cvar_set(void)
{
    const char *var = G_STRING(OFS_PARM0);
    const char *val = G_STRING(OFS_PARM1);

    Cvar_Set(var, val);
}

/*
=================
PF_findradius

Returns a chain of entities that have origins within a spherical area

findradius (origin, radius)
=================
*/
static void
PF_findradius(void)
{
    vec3_t eorg;
    int i, j;
    edict_t *chain = (edict_t *)sv.edicts;
    float *org     = G_VECTOR(OFS_PARM0);
    float rad      = G_FLOAT(OFS_PARM1);
    edict_t *ent   = NEXT_EDICT(sv.edicts);

    for (i = 1; i < sv.num_edicts; i++, ent = NEXT_EDICT(ent)) {
	if (ent->free)
	    continue;
	if (ent->v.solid == SOLID_NOT)
	    continue;
	for (j = 0; j < 3; j++)
	    eorg[j] =
		org[j] - (ent->v.origin[j] +
			  (ent->v.mins[j] + ent->v.maxs[j]) * 0.5);
	if (Length(eorg) > rad)
	    continue;

	ent->v.chain = EDICT_TO_PROG(chain);
	chain = ent;
    }

    RETURN_EDICT(chain);
}


/*
=========
PF_dprint
=========
*/
static void
PF_dprint(void)
{
}

static char pr_string_temp[128];

static void
PF_ftos(void)
{
    float v = G_FLOAT(OFS_PARM0);

    if (v == (int)v)
	sprintf(pr_string_temp, "%d", (int)v);
    else
	sprintf(pr_string_temp, "%5.1f", v);
    G_INT(OFS_RETURN) = PR_SetString(pr_string_temp);
}

static void
PF_fabs(void)
{
    float v = G_FLOAT(OFS_PARM0);
    G_FLOAT(OFS_RETURN) = fabs(v);
}

static void
PF_vtos(void)
{
    sprintf(pr_string_temp, "'%5.1f %5.1f %5.1f'", G_VECTOR(OFS_PARM0)[0],
	    G_VECTOR(OFS_PARM0)[1], G_VECTOR(OFS_PARM0)[2]);
    G_INT(OFS_RETURN) = PR_SetString(pr_string_temp);
}

static void
PF_Spawn(void)
{
    edict_t *ed = ED_Alloc();
    RETURN_EDICT(ed);
}

static void
PF_Remove(void)
{
    edict_t *ed = G_EDICT(OFS_PARM0);
    ED_Free(ed);
}


// entity (entity start, .string field, string match) find = #5;
static void
PF_Find(void)
{
    const char *t;
    int e         = G_EDICTNUM(OFS_PARM0);
    int f         = G_INT(OFS_PARM1);
    const char *s = G_STRING(OFS_PARM2);
    if (!s)
	PR_RunError("%s: bad search string", __func__);

    for (e++; e < sv.num_edicts; e++)
    {
	edict_t *ed = EDICT_NUM(e);
	if (ed->free)
	    continue;
	t = E_STRING(ed, f);
	if (!t)
	    continue;
	if (!strcmp(t, s)) {
	    RETURN_EDICT(ed);
	    return;
	}
    }

    RETURN_EDICT(sv.edicts);
}

static void
PR_CheckEmptyString(const char *s)
{
    if (s[0] <= ' ')
	PR_RunError("%s: Bad string", __func__);
}

static void
PF_precache_file(void)
{
    // precache_file is only used to copy files with qcc, it does nothing
    G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
}

static void
PF_precache_sound(void)
{
    const char *s;
    int i;

    if (sv.state != ss_loading)
	PR_RunError("%s: Precache can only be done in spawn functions",
		    __func__);

    s = G_STRING(OFS_PARM0);
    G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
    PR_CheckEmptyString(s);

#ifdef NQ_HACK
    for (i = 0; i < max_sounds(sv.protocol); i++) {
#endif
#ifdef QW_HACK
    for (i = 0; i < MAX_SOUNDS; i++) {
#endif
	if (!sv.sound_precache[i]) {
	    sv.sound_precache[i] = s;
	    return;
	}
	if (!strcmp(sv.sound_precache[i], s))
	    return;
    }
#ifdef NQ_HACK
    PR_RunError("%s: overflow (max = %d)", __func__, max_sounds(sv.protocol));
#endif
#ifdef QW_HACK
    PR_RunError("%s: overflow (max = %d)", __func__, MAX_SOUNDS);
#endif
}

static void
PF_precache_model(void)
{
    const char *s;
    int i;

    if (sv.state != ss_loading)
	PR_RunError("%s: Precache can only be done in spawn functions",
		    __func__);

    s = G_STRING(OFS_PARM0);
    G_INT(OFS_RETURN) = G_INT(OFS_PARM0);
    PR_CheckEmptyString(s);

#ifdef NQ_HACK
    for (i = 0; i < max_models(sv.protocol); i++) {
#endif
#ifdef QW_HACK
    for (i = 0; i < MAX_MODELS; i++) {
#endif
	if (!sv.model_precache[i]) {
	    sv.model_precache[i] = s;
#ifdef NQ_HACK
	    sv.models[i] = Mod_ForName(s, true);
#endif
	    return;
	}
	if (!strcmp(sv.model_precache[i], s))
	    return;
    }
#ifdef NQ_HACK
    PR_RunError("%s: overflow (max = %d)", __func__, max_models(sv.protocol));
#endif
#ifdef QW_HACK
    PR_RunError("%s: overflow (max = %d)", __func__, MAX_MODELS);
#endif
}


static void
PF_coredump(void)
{
}

static void
PF_traceon(void)
{
}

static void
PF_traceoff(void)
{
}

static void
PF_eprint(void)
{
}

/*
===============
PF_walkmove

float(float yaw, float dist) walkmove
===============
*/
static void
PF_walkmove(void)
{
    vec3_t move;
    dfunction_t *oldf;
    int oldself;
    edict_t *ent = PROG_TO_EDICT(pr_global_struct->self);
    float yaw    = G_FLOAT(OFS_PARM0);
    float dist   = G_FLOAT(OFS_PARM1);

    if (!((int)ent->v.flags & (FL_ONGROUND | FL_FLY | FL_SWIM))) {
	G_FLOAT(OFS_RETURN) = 0;
	return;
    }

    yaw = yaw * M_PI * 2 / 360;

    move[0] = cos(yaw) * dist;
    move[1] = sin(yaw) * dist;
    move[2] = 0;

    // save program state, because SV_movestep may call other progs
    oldf    = pr_xfunction;
    oldself = pr_global_struct->self;

    G_FLOAT(OFS_RETURN) = SV_movestep(ent, move, true);


    // restore program state
    pr_xfunction           = oldf;
    pr_global_struct->self = oldself;
}

/*
===============
PF_droptofloor

void() droptofloor
===============
*/
static void
PF_droptofloor(void)
{
    vec3_t end;
    trace_t trace;
    edict_t *ent = PROG_TO_EDICT(pr_global_struct->self);

    VectorCopy(ent->v.origin, end);
    end[2] -= 256;

    trace = SV_Move(ent->v.origin, ent->v.mins, ent->v.maxs, end, false, ent);

    if (trace.fraction == 1 || trace.allsolid)
	G_FLOAT(OFS_RETURN) = 0;
    else {
	VectorCopy(trace.endpos, ent->v.origin);
	SV_LinkEdict(ent, false);
	ent->v.flags = (int)ent->v.flags | FL_ONGROUND;
	ent->v.groundentity = EDICT_TO_PROG(trace.ent);
	G_FLOAT(OFS_RETURN) = 1;
    }
}

/*
===============
PF_lightstyle

void(float style, string value) lightstyle
===============
*/
static void
PF_lightstyle(void)
{
    int i;
    client_t *client;
    int style       = G_FLOAT(OFS_PARM0);
    const char *val = G_STRING(OFS_PARM1);

    // change the string in sv
    sv.lightstyles[style] = val;

    // send message to all clients on this server
    if (sv.state != ss_active)
	return;

#ifdef NQ_HACK
    for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++)
	if (client->active || client->spawned) {
	    MSG_WriteChar(&client->message, svc_lightstyle);
	    MSG_WriteChar(&client->message, style);
	    MSG_WriteString(&client->message, val);
	}
#endif
#ifdef QW_HACK
    for (i = 0, client = svs.clients; i < MAX_CLIENTS; i++, client++)
	if (client->state == cs_spawned) {
	    ClientReliableWrite_Begin(client, svc_lightstyle,
				      strlen(val) + 3);
	    ClientReliableWrite_Char(client, style);
	    ClientReliableWrite_String(client, val);
	}
#endif
}

static void
PF_rint(void)
{
    float f = G_FLOAT(OFS_PARM0);
    if (f > 0)
	G_FLOAT(OFS_RETURN) = (int)(f + 0.5);
    else
	G_FLOAT(OFS_RETURN) = (int)(f - 0.5);
}

static void
PF_floor(void)
{
    G_FLOAT(OFS_RETURN) = floor(G_FLOAT(OFS_PARM0));
}

static void
PF_ceil(void)
{
    G_FLOAT(OFS_RETURN) = ceil(G_FLOAT(OFS_PARM0));
}


/*
=============
PF_checkbottom
=============
*/
static void
PF_checkbottom(void)
{
    edict_t *ent = G_EDICT(OFS_PARM0);

    G_FLOAT(OFS_RETURN) = SV_CheckBottom(ent);
}

/*
=============
PF_pointcontents
=============
*/
static void
PF_pointcontents(void)
{
    float *v = G_VECTOR(OFS_PARM0);

    G_FLOAT(OFS_RETURN) = SV_PointContents(v);
}

/*
=============
PF_nextent

entity nextent(entity)
=============
*/
static void
PF_nextent(void)
{
    edict_t *ent;
    int i = G_EDICTNUM(OFS_PARM0);

    while (1) {
	i++;
	if (i == sv.num_edicts) {
	    RETURN_EDICT(sv.edicts);
	    return;
	}
	ent = EDICT_NUM(i);
	if (!ent->free) {
	    RETURN_EDICT(ent);
	    return;
	}
    }
}

/*
=============
PF_aim

Pick a vector for the player to shoot along
vector aim(entity, missilespeed)
=============
*/
#ifdef NQ_HACK
cvar_t sv_aim = { "sv_aim", "0.93" };
#endif
#ifdef QW_HACK
cvar_t sv_aim = { "sv_aim", "2" };
#endif

static void
PF_aim(void)
{
    edict_t *check, *bestent;
    vec3_t start, dir, end, bestdir;
    int i, j;
    trace_t tr;
    float dist, bestdist;
    /* NOTE: missilespeed parameter is ignored */
    //float speed;
#ifdef QW_HACK
    char *noaim;
#endif
    edict_t *ent = G_EDICT(OFS_PARM0);

    VectorCopy(ent->v.origin, start);
    start[2] += 20;

#ifdef QW_HACK
// noaim option
    i = NUM_FOR_EDICT(ent);
    if (i > 0 && i < MAX_CLIENTS) {
	noaim = Info_ValueForKey(svs.clients[i - 1].userinfo, "noaim");
	if (atoi(noaim) > 0) {
	    VectorCopy(pr_global_struct->v_forward, G_VECTOR(OFS_RETURN));
	    return;
	}
    }
#endif

// try sending a trace straight
    VectorCopy(pr_global_struct->v_forward, dir);
    VectorMA(start, 2048, dir, end);
    tr = SV_Move(start, vec3_origin, vec3_origin, end, false, ent);
    if (tr.ent && tr.ent->v.takedamage == DAMAGE_AIM
	&& (!teamplay.value || ent->v.team <= 0
	    || ent->v.team != tr.ent->v.team)) {
	VectorCopy(pr_global_struct->v_forward, G_VECTOR(OFS_RETURN));
	return;
    }
// try all possible entities
    VectorCopy(dir, bestdir);
    bestdist = sv_aim.value;
    bestent = NULL;

    check = NEXT_EDICT(sv.edicts);
    for (i = 1; i < sv.num_edicts; i++, check = NEXT_EDICT(check)) {
	if (check->v.takedamage != DAMAGE_AIM)
	    continue;
	if (check == ent)
	    continue;
	if (teamplay.value && ent->v.team > 0 && ent->v.team == check->v.team)
	    continue;		// don't aim at teammate
	for (j = 0; j < 3; j++)
	    end[j] = check->v.origin[j]
		+ 0.5 * (check->v.mins[j] + check->v.maxs[j]);
	VectorSubtract(end, start, dir);
	VectorNormalize(dir);
	dist = DotProduct(dir, pr_global_struct->v_forward);
	if (dist < bestdist)
	    continue;		// to far to turn
	tr = SV_Move(start, vec3_origin, vec3_origin, end, false, ent);
	if (tr.ent == check) {	// can shoot at this one
	    bestdist = dist;
	    bestent = check;
	}
    }

    if (bestent) {
	VectorSubtract(bestent->v.origin, ent->v.origin, dir);
	dist = DotProduct(dir, pr_global_struct->v_forward);
	VectorScale(pr_global_struct->v_forward, dist, end);
	end[2] = dir[2];
	VectorNormalize(end);
	VectorCopy(end, G_VECTOR(OFS_RETURN));
    } else {
	VectorCopy(bestdir, G_VECTOR(OFS_RETURN));
    }
}

/*
==============
PF_changeyaw

This was a major timewaster in progs, so it was converted to C
==============
*/
void
PF_changeyaw(void)
{
    float move;
    edict_t *ent = PROG_TO_EDICT(pr_global_struct->self);
    float current = anglemod(ent->v.angles[1]);
    float ideal = ent->v.ideal_yaw;
    float speed = ent->v.yaw_speed;

    if (current == ideal)
	return;
    move = ideal - current;
    if (ideal > current) {
	if (move >= 180)
	    move = move - 360;
    } else {
	if (move <= -180)
	    move = move + 360;
    }
    if (move > 0) {
	if (move > speed)
	    move = speed;
    } else {
	if (move < -speed)
	    move = -speed;
    }

    ent->v.angles[1] = anglemod(current + move);
}

/*
===============================================================================

MESSAGE WRITING

===============================================================================
*/

#define MSG_BROADCAST	0	/* unreliable to all            */
#define MSG_ONE		1	/* reliable to one (msg_entity) */
#define MSG_ALL		2	/* reliable to all              */
#define MSG_INIT	3	/* write to the init string     */
#ifdef QW_HACK
#define MSG_MULTICAST	4	/* for multicast()              */
#endif

static sizebuf_t *
WriteDest(void)
{
#ifdef NQ_HACK
    int entnum;
    edict_t *ent;
#endif
    int dest = G_FLOAT(OFS_PARM0);
    switch (dest) {
    case MSG_BROADCAST:
	return &sv.datagram;

    case MSG_ONE:
#ifdef NQ_HACK
	ent = PROG_TO_EDICT(pr_global_struct->msg_entity);
	entnum = NUM_FOR_EDICT(ent);
	if (entnum < 1 || entnum > svs.maxclients)
	    PR_RunError("%s: not a client", __func__);
	return &svs.clients[entnum - 1].message;
#endif
#ifdef QW_HACK
	SV_Error("%s: Shouldn't be at MSG_ONE", __func__);
#endif

    case MSG_ALL:
	return &sv.reliable_datagram;

    case MSG_INIT:
#ifdef QW_HACK
	if (sv.state != ss_loading)
	    PR_RunError("%s: MSG_INIT can only be written in spawn functions",
			__func__);
#endif
	return &sv.signon;

#ifdef QW_HACK
    case MSG_MULTICAST:
	return &sv.multicast;
#endif

    default:
	PR_RunError("%s: bad destination", __func__);
	break;
    }

    return NULL;
}

#ifdef QW_HACK
static client_t *
Write_GetClient(void)
{
    edict_t *ent = PROG_TO_EDICT(pr_global_struct->msg_entity);
    int entnum   = NUM_FOR_EDICT(ent);
    if (entnum < 1 || entnum > MAX_CLIENTS)
	PR_RunError("%s: not a client", __func__);
    return &svs.clients[entnum - 1];
}
#endif

static void
PF_WriteByte(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 1);
	ClientReliableWrite_Byte(client, G_FLOAT(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteByte(WriteDest(), G_FLOAT(OFS_PARM1));
}

static void
PF_WriteChar(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 1);
	ClientReliableWrite_Char(client, G_FLOAT(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteChar(WriteDest(), G_FLOAT(OFS_PARM1));
}

static void
PF_WriteShort(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 2);
	ClientReliableWrite_Short(client, G_FLOAT(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteShort(WriteDest(), G_FLOAT(OFS_PARM1));
}

static void
PF_WriteLong(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 4);
	ClientReliableWrite_Long(client, G_FLOAT(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteLong(WriteDest(), G_FLOAT(OFS_PARM1));
}

static void
PF_WriteAngle(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 1);
	ClientReliableWrite_Angle(client, G_FLOAT(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteAngle(WriteDest(), G_FLOAT(OFS_PARM1));
}

static void
PF_WriteCoord(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 2);
	ClientReliableWrite_Coord(client, G_FLOAT(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteCoord(WriteDest(), G_FLOAT(OFS_PARM1));
}

static void
PF_WriteString(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 1 + strlen(G_STRING(OFS_PARM1)));
	ClientReliableWrite_String(client, G_STRING(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteString(WriteDest(), G_STRING(OFS_PARM1));
}

static void
PF_WriteEntity(void)
{
#ifdef QW_HACK
    if (G_FLOAT(OFS_PARM0) == MSG_ONE) {
	client_t *client = Write_GetClient();
	ClientReliableCheckBlock(client, 2);
	ClientReliableWrite_Short(client, G_EDICTNUM(OFS_PARM1));
	return;
    }
#endif
    MSG_WriteShort(WriteDest(), G_EDICTNUM(OFS_PARM1));
}

//=============================================================================

static void
PF_makestatic(void)
{
    int i;
#ifdef NQ_HACK
    unsigned int bits;
#endif
    edict_t *ent = G_EDICT(OFS_PARM0);

#ifdef NQ_HACK
    bits = 0;
    if (sv.protocol == PROTOCOL_VERSION_FITZ) {
	if (SV_ModelIndex(PR_GetString(ent->v.model)) & 0xff00)
	    bits |= B_FITZ_LARGEMODEL;
	if ((int)ent->v.frame & 0xff00)
	    bits |= B_FITZ_LARGEFRAME;
#if 0
	if (ent->alpha != ENTALPHA_DEFAULT)
	    bits |= B_FITZ_ALPHA;
#endif
    }

    if (bits) {
	MSG_WriteByte(&sv.signon, svc_fitz_spawnstatic2);
	MSG_WriteByte(&sv.signon, bits);
    } else {
	MSG_WriteByte(&sv.signon, svc_spawnstatic);
    }
    SV_WriteModelIndex(&sv.signon, SV_ModelIndex(PR_GetString(ent->v.model)), bits);
#endif
#ifdef QW_HACK
    MSG_WriteByte(&sv.signon, svc_spawnstatic);
    MSG_WriteByte(&sv.signon, SV_ModelIndex(PR_GetString(ent->v.model)));
#endif

    MSG_WriteByte(&sv.signon, ent->v.frame);
    MSG_WriteByte(&sv.signon, ent->v.colormap);
    MSG_WriteByte(&sv.signon, ent->v.skin);

    for (i = 0; i < 3; i++) {
	MSG_WriteCoord(&sv.signon, ent->v.origin[i]);
	MSG_WriteAngle(&sv.signon, ent->v.angles[i]);
    }

#if 0
    if (bits & B_ALPHA)
	MSG_WriteByte(&sv.signon, ent->alpha);
#endif

    /* throw the entity away now */
    ED_Free(ent);
}

//=============================================================================

/*
==============
PF_setspawnparms
==============
*/
static void
PF_setspawnparms(void)
{
    client_t *client;
    edict_t *ent = G_EDICT(OFS_PARM0);
    int i        = NUM_FOR_EDICT(ent);
#ifdef NQ_HACK
    if (i < 1 || i > svs.maxclients)
#endif
#ifdef QW_HACK
    if (i < 1 || i > MAX_CLIENTS)
#endif
	PR_RunError("%s: Entity is not a client", __func__);

    // copy spawn parms out of the client_t
    client = svs.clients + (i - 1);

    for (i = 0; i < NUM_SPAWN_PARMS; i++)
	(&pr_global_struct->parm1)[i] = client->spawn_parms[i];
}

/*
==============
PF_changelevel
==============
*/
static void
PF_changelevel(void)
{
#ifdef NQ_HACK
    /* make sure we don't issue two changelevels */
    if (svs.changelevel_issued)
	return;
    svs.changelevel_issued = true;

    Cbuf_AddText("changelevel %s\n", G_STRING(OFS_PARM0));
#endif
#ifdef QW_HACK
    static int last_spawncount;

    /* make sure we don't issue two changelevels */
    if (svs.spawncount == last_spawncount)
	return;
    last_spawncount = svs.spawncount;

    Cbuf_AddText("map %s\n", G_STRING(OFS_PARM0));
#endif
}

#ifdef QW_HACK
/*
==============
PF_infokey

string(entity e, string key) infokey
==============
*/
static void
PF_infokey(void)
{
    static char buf[256]; /* only needs to fit IP or ping */
    const char *value;
    edict_t *e = G_EDICT(OFS_PARM0);
    int e1 = NUM_FOR_EDICT(e);
    const char *key = G_STRING(OFS_PARM1);

    if (e1 == 0) {
	if ((value = Info_ValueForKey(svs.info, key)) == NULL || !*value)
	    value = Info_ValueForKey(localinfo, key);
    } else if (e1 <= MAX_CLIENTS) {
	if (!strcmp(key, "ip")) {
	    netadr_t addr = svs.clients[e1 - 1].netchan.remote_address;
	    strlcpy(buf, NET_BaseAdrToString(addr), sizeof(buf));
	    value = buf;
	} else if (!strcmp(key, "ping")) {
	    int ping = SV_CalcPing(&svs.clients[e1 - 1]);
	    snprintf(buf, sizeof(buf), "%d", ping);
	    value = buf;
	} else
	    value = Info_ValueForKey(svs.clients[e1 - 1].userinfo, key);
    } else
	value = "";

    RETURN_STRING(value);
}

/*
==============
PF_stof

float(string s) stof
==============
*/
static void
PF_stof(void)
{
    const char *s = G_STRING(OFS_PARM0);

    G_FLOAT(OFS_RETURN) = atof(s);
}


/*
==============
PF_multicast

void(vector where, float set) multicast
==============
*/
static void
PF_multicast(void)
{
    float *o = G_VECTOR(OFS_PARM0);
    int to   = G_FLOAT(OFS_PARM1);

    SV_Multicast(o, to);
}
#endif /* QW_HACK */

static void
PF_Fixme(void)
{
    PR_RunError("unimplemented builtin");
}


builtin_t pr_builtin[] = {
    PF_Fixme,
    PF_makevectors,	// void(entity e) makevectors                    = #1;
    PF_setorigin,	// void(entity e, vector o) setorigin            = #2;
    PF_setmodel,	// void(entity e, string m) setmodel             = #3;
    PF_setsize,		// void(entity e, vector min, vector max) setsize = #4;
    PF_Fixme,		// void(entity e, vector min, vector max) setabssize = #5;
    PF_break,		// void() break                                  = #6;
    PF_random,		// float() random                                = #7;
    PF_sound,		// void(entity e, float chan, string samp) sound = #8;
    PF_normalize,	// vector(vector v) normalize                    = #9;
    PF_error,		// void(string e) error                          = #10;
    PF_objerror,	// void(string e) objerror                       = #11;
    PF_vlen,		// float(vector v) vlen                          = #12;
    PF_vectoyaw,	// float(vector v) vectoyaw                      = #13;
    PF_Spawn,		// entity() spawn                                = #14;
    PF_Remove,		// void(entity e) remove                         = #15;
    PF_traceline,	// float(vector v1, vector v2, float tryents) traceline = #16;
    PF_checkclient,	// entity() clientlist                           = #17;
    PF_Find,		// entity(entity start, .string fld, string match) find = #18;
    PF_precache_sound,	// void(string s) precache_sound                 = #19;
    PF_precache_model,	// void(string s) precache_model                 = #20;
    PF_stuffcmd,	// void(entity client, string s)stuffcmd         = #21;
    PF_findradius,	// entity(vector org, float rad) findradius      = #22;
    PF_bprint,		// void(string s) bprint                         = #23;
    PF_sprint,		// void(entity client, string s) sprint          = #24;
    PF_dprint,		// void(string s) dprint                         = #25;
    PF_ftos,		// void(string s) ftos                           = #26;
    PF_vtos,		// void(string s) vtos                           = #27;
    PF_coredump,
    PF_traceon,
    PF_traceoff,
    PF_eprint,		// void(entity e) debug print an entire entity
    PF_walkmove,	// float(float yaw, float dist) walkmove
    PF_Fixme,		// float(float yaw, float dist) walkmove
    PF_droptofloor,
    PF_lightstyle,
    PF_rint,
    PF_floor,
    PF_ceil,
    PF_Fixme,
    PF_checkbottom,
    PF_pointcontents,
    PF_Fixme,
    PF_fabs,
    PF_aim,
    PF_cvar,
    PF_localcmd,
    PF_nextent,
#ifdef NQ_HACK
    PF_particle,
#endif
#ifdef QW_HACK
    PF_Fixme,
#endif
    PF_changeyaw,
    PF_Fixme,
    PF_vectoangles,

    PF_WriteByte,
    PF_WriteChar,
    PF_WriteShort,
    PF_WriteLong,
    PF_WriteCoord,
    PF_WriteAngle,
    PF_WriteString,
    PF_WriteEntity,

    PF_Fixme,
    PF_Fixme,
    PF_Fixme,
    PF_Fixme,
    PF_Fixme,
    PF_Fixme,
    PF_Fixme,

    SV_MoveToGoal,
    PF_precache_file,
    PF_makestatic,

    PF_changelevel,
    PF_Fixme,

    PF_cvar_set,
    PF_centerprint,

    PF_ambientsound,

    PF_precache_model,
    PF_precache_sound,	/* precache_sound2 is different only for qcc */
    PF_precache_file,

    PF_setspawnparms,

#ifdef QW_HACK
    PF_logfrag,
    PF_infokey,
    PF_stof,
    PF_multicast,
#endif
};

builtin_t *pr_builtins = pr_builtin;
int pr_numbuiltins = sizeof(pr_builtin) / sizeof(pr_builtin[0]);
