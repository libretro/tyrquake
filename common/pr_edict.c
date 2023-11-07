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
// sv_edict.c -- entity dictionary

#include <compat/strl.h>

#include "cmd.h"
#include "console.h"
#include "crc.h"
#include "pr_comp.h"
#include "progdefs.h"
#include "progs.h"
#include "server.h"
#include "world.h"
#include "zone.h"

#ifdef NQ_HACK
#include "host.h"
#include "quakedef.h"
#include "sys.h"

/* FIXME - quick hack to enable merging of NQ/QWSV shared code */
#define SV_Error Sys_Error
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
#include "qwsvdef.h"
#endif

#include <streams/file_stream.h>

/* forward declarations */
int rfprintf(RFILE * stream, const char * format, ...);

dprograms_t *progs;
dfunction_t *pr_functions;
char *pr_strings;
int pr_strings_size;
dstatement_t *pr_statements;
globalvars_t *pr_global_struct;
float *pr_globals;		// same as pr_global_struct
int pr_edict_size;		// in bytes

static ddef_t *pr_fielddefs;
static ddef_t *pr_globaldefs;

/*
 * These are the sizes of the types enumerated in etype_t (pr_comp.h)
 */
static int type_size[8] = {
    1,				// ev_void
    1,				// ev_string
    1,				// ev_float
    3,				// ev_vector
    1,				// ev_entity
    1,				// ev_field
    1,				// ev_function
    1				// ev_pointer
};

static qboolean ED_ParseEpair(void *base, ddef_t *key, const char *s);

#define	MAX_FIELD_LEN	64
#define GEFV_CACHESIZE	2

typedef struct {
    ddef_t *pcache;
    char field[MAX_FIELD_LEN];
} gefv_cache;

static gefv_cache gefvCache[GEFV_CACHESIZE] = { {NULL, ""}, {NULL, ""} };

#ifdef NQ_HACK
cvar_t nomonsters = { "nomonsters", "0" };
static cvar_t scratch1 = { "scratch1", "0" };
static cvar_t scratch2 = { "scratch2", "0" };
static cvar_t scratch3 = { "scratch3", "0" };
static cvar_t scratch4 = { "scratch4", "0" };
static cvar_t saved1 = { "saved1", "0", true };
static cvar_t saved2 = { "saved2", "0", true };
static cvar_t saved3 = { "saved3", "0", true };
static cvar_t saved4 = { "saved4", "0", true };
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
func_t SpectatorConnect;
func_t SpectatorThink;
func_t SpectatorDisconnect;
#endif

/*
=================
ED_ClearEdict

Sets everything to NULL
=================
*/
static void
ED_ClearEdict(edict_t *e)
{
    memset(&e->v, 0, progs->entityfields * 4);
    e->free = false;
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *
ED_Alloc(void)
{
    int i;
    edict_t *e;

#ifdef NQ_HACK
    for (i = svs.maxclients + 1; i < sv.num_edicts; i++) {
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
    for (i = MAX_CLIENTS + 1; i < sv.num_edicts; i++) {
#endif
	e = EDICT_NUM(i);
	// the first couple seconds of server time can involve a lot of
	// freeing and allocating, so relax the replacement policy
	if (e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5)) {
	    ED_ClearEdict(e);
	    return e;
	}
    }

#ifdef NQ_HACK
    if (i == MAX_EDICTS)
	SV_Error("%s: no free edicts", __func__);
    sv.num_edicts++;
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
    if (i == MAX_EDICTS) {
	Con_Printf("WARNING: ED_Alloc: no free edicts\n");
	i--;			// step on whatever is the last edict
	e = EDICT_NUM(i);
	SV_UnlinkEdict(e);
    } else
	sv.num_edicts++;
#endif

    e = EDICT_NUM(i);
    ED_ClearEdict(e);

    return e;
}

/*
=================
ED_Free

Marks the edict as free
FIXME: walk all entities and NULL out references to this entity
=================
*/
void
ED_Free(edict_t *ed)
{
    SV_UnlinkEdict(ed);		// unlink from world bsp

    ed->free = true;
    ed->v.model = 0;
    ed->v.takedamage = 0;
    ed->v.modelindex = 0;
    ed->v.colormap = 0;
    ed->v.skin = 0;
    ed->v.frame = 0;
    VectorCopy(vec3_origin, ed->v.origin);
    VectorCopy(vec3_origin, ed->v.angles);
    ed->v.nextthink = -1;
    ed->v.solid = 0;

    ed->freetime = sv.time;
}

/*
============
ED_FieldAtOfs
============
*/
static ddef_t *
ED_FieldAtOfs(int ofs)
{
    ddef_t *def;
    int i;

    for (i = 0; i < progs->numfielddefs; i++) {
	def = &pr_fielddefs[i];
	if (def->ofs == ofs)
	    return def;
    }
    return NULL;
}

/*
============
ED_FindField
============
*/
static ddef_t *
ED_FindField(const char *name)
{
    ddef_t *def;
    int i;

    for (i = 0; i < progs->numfielddefs; i++) {
	def = &pr_fielddefs[i];
	if (!strcmp(PR_GetString(def->s_name), name))
	    return def;
    }
    return NULL;
}


/*
============
ED_FindGlobal
============
*/
static ddef_t *
ED_FindGlobal(const char *name)
{
    ddef_t *def;
    int i;

    for (i = 0; i < progs->numglobaldefs; i++) {
	def = &pr_globaldefs[i];
	if (!strcmp(PR_GetString(def->s_name), name))
	    return def;
    }
    return NULL;
}


/*
============
ED_FindFunction
============
*/
static dfunction_t *
ED_FindFunction(const char *name)
{
    dfunction_t *func;
    int i;

    for (i = 0; i < progs->numfunctions; i++) {
	func = &pr_functions[i];
	if (!strcmp(PR_GetString(func->s_name), name))
	    return func;
    }
    return NULL;
}

eval_t *
GetEdictFieldValue(edict_t *ed, const char *field)
{
    static int rep = 0;
    ddef_t *def = NULL;
    int i;

    for (i = 0; i < GEFV_CACHESIZE; i++) {
	if (!strcmp(field, gefvCache[i].field)) {
	    def = gefvCache[i].pcache;
	    goto Done;
	}
    }

    def = ED_FindField(field);

    if (strlen(field) < MAX_FIELD_LEN) {
	gefvCache[rep].pcache = def;
	strcpy(gefvCache[rep].field, field);
	rep ^= 1;
    }

  Done:
    if (!def)
	return NULL;

    return (eval_t *)((char *)&ed->v + def->ofs * 4);
}

/*
============
PR_UglyValueString

Returns a string describing *data in a type specific manner
Easier to parse than PR_ValueString
=============
*/
static char *
PR_UglyValueString(etype_t type, eval_t *val)
{
    static char line[128];
    ddef_t *def;
    dfunction_t *f;

    type &= ~DEF_SAVEGLOBAL;

    switch (type) {
    case ev_string:
	strlcpy(line, PR_GetString(val->string), sizeof(line));
	break;
    case ev_entity:
	snprintf(line, sizeof(line), "%i",
		 NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)));
	break;
    case ev_function:
	f = pr_functions + val->function;
	strlcpy(line, PR_GetString(f->s_name), sizeof(line));
	break;
    case ev_field:
	def = ED_FieldAtOfs(val->_int);
	strlcpy(line, PR_GetString(def->s_name), sizeof(line));
	break;
    case ev_void:
	strlcpy(line, "void", sizeof(line));
	break;
    case ev_float:
	snprintf(line, sizeof(line), "%f", val->_float);
	break;
    case ev_vector:
	snprintf(line, sizeof(line), "%f %f %f",
		 val->vector[0], val->vector[1], val->vector[2]);
	break;
    default:
	snprintf(line, sizeof(line), "bad type %i", type);
	break;
    }

    return line;
}

/*
=============
ED_Write

For savegames
=============
*/
void ED_Write(RFILE *f, edict_t *ed)
{
   int i;

   rfprintf(f, "{\n");

   if (ed->free)
   {
      rfprintf(f, "}\n");
      return;
   }

   for (i = 1; i < progs->numfielddefs; i++)
   {
      int j, type, *v;
      ddef_t        *d = &pr_fielddefs[i];
      const char *name = PR_GetString(d->s_name);
      if (name[strlen(name) - 2] == '_')
         continue;		// skip _x, _y, _z vars

      v = (int *)((char *)&ed->v + d->ofs * 4);

      // if the value is still all 0, skip the field
      type = d->type & ~DEF_SAVEGLOBAL;
      for (j = 0; j < type_size[type]; j++)
         if (v[j])
            break;
      if (j == type_size[type])
         continue;

      rfprintf(f, "\"%s\" ", name);
      rfprintf(f, "\"%s\"\n", PR_UglyValueString((etype_t)d->type, (eval_t *)v));
   }

   rfprintf(f, "}\n");
}

/*
==============================================================================

ARCHIVING GLOBALS

FIXME: need to tag constants, doesn't really work
==============================================================================
*/

/*
=============
ED_WriteGlobals
=============
*/
void
ED_WriteGlobals(RFILE *f)
{
    int i;

    rfprintf(f, "{\n");
    for (i = 0; i < progs->numglobaldefs; i++)
    {
        const char *name;
	ddef_t *def = &pr_globaldefs[i];
	int type = def->type;
	if (!(def->type & DEF_SAVEGLOBAL))
	    continue;
	type &= ~DEF_SAVEGLOBAL;

	if (type != ev_string && type != ev_float && type != ev_entity)
	    continue;

	name = PR_GetString(def->s_name);
	rfprintf(f, "\"%s\" ", name);
	rfprintf(f, "\"%s\"\n",
		PR_UglyValueString((etype_t)type, (eval_t *)&pr_globals[def->ofs]));
    }
    rfprintf(f, "}\n");
}

/*
=============
ED_ParseGlobals
=============
*/
void
ED_ParseGlobals(const char *data)
{
    char keyname[64];
    ddef_t *key;

    while (1) {
	// parse key
	data = COM_Parse(data);
	if (com_token[0] == '}')
	    break;
	if (!data)
	    SV_Error("%s: EOF without closing brace", __func__);

	strcpy(keyname, com_token);

	// parse value
	data = COM_Parse(data);
	if (!data)
	    SV_Error("%s: EOF without closing brace", __func__);

	if (com_token[0] == '}')
	    SV_Error("%s: closing brace without data", __func__);

	key = ED_FindGlobal(keyname);
	if (!key) {
	    Con_Printf("'%s' is not a global\n", keyname);
	    continue;
	}

	if (!ED_ParseEpair((void *)pr_globals, key, com_token))
#ifdef NQ_HACK
	    Host_Error("%s: parse error", __func__);
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
	    SV_Error("%s: parse error", __func__);
#endif
    }
}

//============================================================================


/*
=============
ED_NewString
=============
*/
static char *
ED_NewString(const char *string)
{
    int i;
    int l        = strlen(string) + 1;
    char *newobj = (char*)Hunk_Alloc(l);
    char *new_p  = newobj;

    for (i = 0; i < l; i++) {
	if (string[i] == '\\' && i < l - 1) {
	    i++;
	    if (string[i] == 'n')
		*new_p++ = '\n';
	    else
		*new_p++ = '\\';
	} else
	    *new_p++ = string[i];
    }

    return newobj;
}


/*
=============
ED_ParseEval

Can parse either fields or globals
returns false if error
=============
*/
static qboolean
ED_ParseEpair(void *base, ddef_t *key, const char *s)
{
    int i;
    char string[128];
    ddef_t *def;
    char *v, *w;
    dfunction_t *func;
    void *d = (void *)((int *)base + key->ofs);

    switch (key->type & ~DEF_SAVEGLOBAL) {
    case ev_string:
	*(string_t *)d = PR_SetString(ED_NewString(s));
	break;

    case ev_float:
	*(float *)d = atof(s);
	break;

    case ev_vector:
	strcpy(string, s);
	v = string;
	w = string;
	for (i = 0; i < 3; i++) {
	    while (*v && *v != ' ')
		v++;
	    *v = 0;
	    ((float *)d)[i] = atof(w);
	    w = v = v + 1;
	}
	break;

    case ev_entity:
	*(int *)d = EDICT_TO_PROG(EDICT_NUM(atoi(s)));
	break;

    case ev_field:
	def = ED_FindField(s);
	if (!def) {
	    Con_Printf("Can't find field %s\n", s);
	    return false;
	}
	*(int *)d = G_INT(def->ofs);
	break;

    case ev_function:
	func = ED_FindFunction(s);
	if (!func) {
	    Con_Printf("Can't find function %s\n", s);
	    return false;
	}
	*(func_t *)d = func - pr_functions;
	break;

    default:
	break;
    }
    return true;
}

/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
Used for initial level load and for savegames.
====================
*/
const char *
ED_ParseEdict(const char *data, edict_t *ent)
{
    int n;
    ddef_t *key;
    qboolean anglehack;
    char keyname[256];
    qboolean init = false;

    // clear it
    if (ent != sv.edicts)	// hack
	memset(&ent->v, 0, progs->entityfields * 4);

    // go through all the dictionary pairs
    while (1) {
	// parse key
	data = COM_Parse(data);
	if (com_token[0] == '}')
	    break;
	if (!data)
	    SV_Error("%s: EOF without closing brace", __func__);

	// anglehack is to allow QuakeEd to write single scalar angles
	// and allow them to be turned into vectors. (FIXME...)
	if (!strcmp(com_token, "angle")) {
	    strcpy(com_token, "angles");
	    anglehack = true;
	} else
	    anglehack = false;

	// FIXME: change light to _light to get rid of this hack
	if (!strcmp(com_token, "light"))
	    strcpy(com_token, "light_lev");	// hack for single light def

	strcpy(keyname, com_token);

	// another hack to fix keynames with trailing spaces
	n = strlen(keyname);
	while (n && keyname[n - 1] == ' ') {
	    keyname[n - 1] = 0;
	    n--;
	}

	// parse value
	data = COM_Parse(data);
	if (!data)
	    SV_Error("%s: EOF without closing brace", __func__);

	if (com_token[0] == '}')
	    SV_Error("%s: closing brace without data", __func__);

	init = true;

	// keynames with a leading underscore are used for utility comments,
	// and are immediately discarded by quake
	if (keyname[0] == '_')
	    continue;

	key = ED_FindField(keyname);
	if (!key) {
	    Con_Printf("'%s' is not a field\n", keyname);
	    continue;
	}

	if (anglehack) {
	    char temp[32];

	    strcpy(temp, com_token);
	    sprintf(com_token, "0 %s 0", temp);
	}

	if (!ED_ParseEpair((void *)&ent->v, key, com_token))
#ifdef NQ_HACK
	    Host_Error("%s: parse error", __func__);
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
	    SV_Error("%s: parse error", __func__);
#endif
    }

    if (!init)
	ent->free = true;

    return data;
}


/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.

Used for both fresh maps and savegame loads.  A fresh map would also need
to call ED_CallSpawnFunctions () to let the objects initialize themselves.
================
*/
void
ED_LoadFromFile(const char *data)
{
    dfunction_t *func;
    edict_t *ent = NULL;
    int inhibit  = 0;

    pr_global_struct->time = sv.time;

    // parse ents
    while (1)
    {
        // parse the opening brace
	data = COM_Parse(data);
	if (!data)
	    break;
	if (com_token[0] != '{')
	    SV_Error("%s: found %s when expecting {", __func__, com_token);

	if (!ent)
	    ent = EDICT_NUM(0);
	else
	    ent = ED_Alloc();
	data = ED_ParseEdict(data, ent);

// remove things from different skill levels or deathmatch
#ifdef NQ_HACK
	if (deathmatch.value) {
#endif
	    if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH)) {
		ED_Free(ent);
		inhibit++;
		continue;
	    }
#ifdef NQ_HACK
	} else
	    if ((current_skill == 0
		 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_EASY))
		|| (current_skill == 1
		    && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM))
		|| (current_skill >= 2
		    && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_HARD))) {
	    ED_Free(ent);
	    inhibit++;
	    continue;
	}
#endif

//
// immediately call spawn function
//
	if (!ent->v.classname) {
	    Con_Printf("No classname for:\n");
	    ED_Free(ent);
	    continue;
	}
	// look for the spawn function
	func = ED_FindFunction(PR_GetString(ent->v.classname));

	if (!func) {
	    Con_Printf("No spawn function for:\n");
	    ED_Free(ent);
	    continue;
	}

	pr_global_struct->self = EDICT_TO_PROG(ent);
	PR_ExecuteProgram(func - pr_functions);
#if defined(QW_HACK) && defined(SERVERONLY)
	SV_FlushSignon();
#endif
    }
}


/*
===============
PR_LoadProgs
===============
*/
void
PR_LoadProgs(void)
{
   int i;
#if defined(QW_HACK) && defined(SERVERONLY)
   char num[32];
   dfunction_t *f;
#endif

   // flush the non-C variable lookup cache
   for (i = 0; i < GEFV_CACHESIZE; i++)
      gefvCache[i].field[0] = 0;

#ifdef NQ_HACK
   progs = (dprograms_t*)COM_LoadHunkFile("progs.dat");
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
   progs = COM_LoadHunkFile("qwprogs.dat");
   if (!progs)
      progs = COM_LoadHunkFile("progs.dat");
#endif
   if (!progs)
      SV_Error("%s: couldn't load progs.dat", __func__);

#if defined(QW_HACK) && defined(SERVERONLY)
   // add prog crc to the serverinfo
   sprintf(num, "%i", CRC_Block((byte *)progs, com_filesize));
   Info_SetValueForStarKey(svs.info, "*progs", num, MAX_SERVERINFO_STRING);
#endif

#ifdef MSB_FIRST
   // byte swap the header
   for (i = 0; i < sizeof(*progs) / 4; i++)
      ((int *)progs)[i] = LittleLong(((int *)progs)[i]);
#endif

   if (progs->version != PROG_VERSION)
      SV_Error("progs.dat has wrong version number (%i should be %i)",
            progs->version, PROG_VERSION);
   if (progs->crc != PROGHEADER_CRC)
#ifdef NQ_HACK
      SV_Error("progs.dat system vars have been modified, "
            "progdefs.h is out of date");
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
   SV_Error("You must have the progs.dat from QuakeWorld installed");
#endif

   pr_functions = (dfunction_t *)((byte *)progs + progs->ofs_functions);
   pr_strings = (char *)progs + progs->ofs_strings;
   pr_strings_size = progs->strings_size;
   if (progs->ofs_strings + pr_strings_size >= com_filesize)
#ifdef NQ_HACK
      Host_Error("progs.dat strings extend past end of file\n");
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
   SV_Error("progs.dat strings extend past end of file\n");
#endif
   PR_InitStringTable();

   pr_globaldefs = (ddef_t *)((byte *)progs + progs->ofs_globaldefs);
   pr_fielddefs = (ddef_t *)((byte *)progs + progs->ofs_fielddefs);
   pr_statements = (dstatement_t *)((byte *)progs + progs->ofs_statements);

   pr_global_struct = (globalvars_t *)((byte *)progs + progs->ofs_globals);
   pr_globals = (float *)pr_global_struct;

   pr_edict_size =
      progs->entityfields * 4 + sizeof(edict_t) - sizeof(entvars_t);

#ifdef MSB_FIRST
   // byte swap the lumps
   for (i = 0; i < progs->numstatements; i++)
   {
      pr_statements[i].op = LittleShort(pr_statements[i].op);
      pr_statements[i].a = LittleShort(pr_statements[i].a);
      pr_statements[i].b = LittleShort(pr_statements[i].b);
      pr_statements[i].c = LittleShort(pr_statements[i].c);
   }

   for (i = 0; i < progs->numfunctions; i++) {
      pr_functions[i].first_statement =
         LittleLong(pr_functions[i].first_statement);
      pr_functions[i].parm_start = LittleLong(pr_functions[i].parm_start);
      pr_functions[i].s_name = LittleLong(pr_functions[i].s_name);
      pr_functions[i].s_file = LittleLong(pr_functions[i].s_file);
      pr_functions[i].numparms = LittleLong(pr_functions[i].numparms);
      pr_functions[i].locals = LittleLong(pr_functions[i].locals);
   }

   for (i = 0; i < progs->numglobaldefs; i++) {
      pr_globaldefs[i].type = LittleShort(pr_globaldefs[i].type);
      pr_globaldefs[i].ofs = LittleShort(pr_globaldefs[i].ofs);
      pr_globaldefs[i].s_name = LittleLong(pr_globaldefs[i].s_name);
   }
#endif

   for (i = 0; i < progs->numfielddefs; i++) {
#ifdef MSB_FIRST
      pr_fielddefs[i].type = LittleShort(pr_fielddefs[i].type);
#endif
      if (pr_fielddefs[i].type & DEF_SAVEGLOBAL)
         SV_Error("%s: pr_fielddefs[i].type & DEF_SAVEGLOBAL", __func__);
#ifdef MSB_FIRST
      pr_fielddefs[i].ofs    = LittleShort(pr_fielddefs[i].ofs);
      pr_fielddefs[i].s_name = LittleLong(pr_fielddefs[i].s_name);
#endif
   }

#ifdef MSB_FIRST
   for (i = 0; i < progs->numglobals; i++)
      ((int *)pr_globals)[i] = LittleLong(((int *)pr_globals)[i]);
#endif

#if defined(QW_HACK) && defined(SERVERONLY)
   // Zoid, find the spectator functions
   SpectatorConnect = SpectatorThink = SpectatorDisconnect = 0;

   if ((f = ED_FindFunction("SpectatorConnect")) != NULL)
      SpectatorConnect = (func_t)(f - pr_functions);
   if ((f = ED_FindFunction("SpectatorThink")) != NULL)
      SpectatorThink = (func_t)(f - pr_functions);
   if ((f = ED_FindFunction("SpectatorDisconnect")) != NULL)
      SpectatorDisconnect = (func_t)(f - pr_functions);
#endif
}


/*
===============
PR_Init
===============
*/
void
PR_Init(void)
{
#ifdef NQ_HACK
    Cvar_RegisterVariable(&nomonsters);
    Cvar_RegisterVariable(&scratch1);
    Cvar_RegisterVariable(&scratch2);
    Cvar_RegisterVariable(&scratch3);
    Cvar_RegisterVariable(&scratch4);
    Cvar_RegisterVariable(&saved1);
    Cvar_RegisterVariable(&saved2);
    Cvar_RegisterVariable(&saved3);
    Cvar_RegisterVariable(&saved4);
#endif
}

edict_t *
EDICT_NUM(int n)
{
#ifdef NQ_HACK
    if (n < 0 || n >= sv.max_edicts)
#endif
#if defined(QW_HACK) && defined(SERVERONLY)
    if (n < 0 || n >= MAX_EDICTS)
#endif
	SV_Error("%s: bad number %i", __func__, n);
    return (edict_t *)((byte *)sv.edicts + (n) * pr_edict_size);
}

int
NUM_FOR_EDICT(const edict_t *e)
{
    int b = ((byte *)e - (byte *)sv.edicts) / pr_edict_size;
    if (b < 0 || b >= sv.num_edicts)
	SV_Error("%s: bad pointer", __func__);
    return b;
}
