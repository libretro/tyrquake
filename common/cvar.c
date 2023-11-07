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
// cvar.c -- dynamic variable tracking

#include "cmd.h"
#include "common.h"
#include "console.h"
#include "cvar.h"
#include "shell.h"
#include "zone.h"

#ifdef NQ_HACK
#include "server.h"
#include "quakedef.h"
#include "host.h"
#endif

#ifdef SERVERONLY
#include "qwsvdef.h"
#include "server.h"
#else
#ifdef QW_HACK
#include "quakedef.h"
#include "client.h"
#endif
#endif

#define cvar_entry(ptr) container_of(ptr, struct cvar_s, stree)
DECLARE_STREE_ROOT(cvar_tree);

/*
============
Cvar_FindVar
============
*/
cvar_t *
Cvar_FindVar(const char *var_name)
{
    struct cvar_s *ret = NULL;
    struct stree_node *n;

    n = STree_Find(&cvar_tree, var_name);
    if (n)
	ret = cvar_entry(n);

    return ret;
}

/*
 * Return a string tree with all possible argument completions of the given
 * buffer for the given cvar.
 */
struct stree_root *
Cvar_ArgCompletions(const char *name, const char *buf)
{
    cvar_t *cvar;
    struct stree_root *root = NULL;

    cvar = Cvar_FindVar(name);
    if (cvar && cvar->completion)
	root = cvar->completion(buf);

    return root;
}

/*
 * Call the argument completion function for cvar "name".
 * Returned result should be Z_Free'd after use.
 */
char *
Cvar_ArgComplete(const char *name, const char *buf)
{
    char *result = NULL;
    struct stree_root *root;

    root = Cvar_ArgCompletions(name, buf);
    if (root) {
	result = STree_MaxMatch(root, buf);
	Z_Free(root);
    }

    return result;
}


#ifdef NQ_HACK
/*
 * For NQ/net_dgrm.c, command == CCREQ_RULE_INFO case
 */
cvar_t *
Cvar_NextServerVar(const char *var_name)
{
   cvar_t *ret = NULL;
   cvar_t *var;
   struct stree_node *n;

   if (var_name[0] == '\0')
      var_name = NULL;

   if (var_name)
   {
      STree_ForEach_Init__(&cvar_tree, &n);
      STree_ForEach_After__(&cvar_tree, &n, var_name);
      for (; STree_WalkLeft__(&cvar_tree, &n) ; STree_WalkRight__(&n)) {
         var = cvar_entry(n);
         if (var->server) {
            ret = var;
            STree_ForEach_Cleanup__(&cvar_tree);
            return ret;
         }
      }
   }
   else
   {
      STree_ForEach_After_NullStr(&cvar_tree, n) {
         var = cvar_entry(n);
         if (var->server) {
            ret = var;
            STree_ForEach_Cleanup__(&cvar_tree);
            return ret;
         }
      }
   }

   return ret;
}
#endif

/*
============
Cvar_VariableValue
============
*/
float
Cvar_VariableValue(const char *var_name)
{
    cvar_t *var;

    var = Cvar_FindVar(var_name);
    if (!var)
	return 0;
    return Q_atof(var->string);
}


/*
============
Cvar_VariableString
============
*/
const char *
Cvar_VariableString(const char *var_name)
{
    cvar_t *var;

    var = Cvar_FindVar(var_name);

    return var ? var->string : "";
}


/*
============
Cvar_Set
============
*/
void
Cvar_Set(const char *var_name, const char *value)
{
    cvar_t *var;
    char *newstring;
    qboolean changed;

    var = Cvar_FindVar(var_name);
    if (!var) {
	/* there is an error in C code if this happens */
	Con_Printf("Cvar_Set: variable %s not found\n", var_name);
	return;
    }

    if (var->flags & CVAR_OBSOLETE) {
	Con_Printf("%s is obsolete.\n", var_name);
	return;
    }

    changed = strcmp(var->string, value);

#ifdef SERVERONLY
    if (var->info) {
	Info_SetValueForKey(svs.info, var_name, value, MAX_SERVERINFO_STRING);
	SV_SendServerInfoChange(var_name, value);
//              SV_BroadcastCommand ("fullserverinfo \"%s\"\n", svs.info);
    }
#else
#ifdef QW_HACK
    if (var->info) {
	Info_SetValueForKey(cls.userinfo, var_name, value, MAX_INFO_STRING);
	if (cls.state >= ca_connected) {
	    MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	    MSG_WriteStringf(&cls.netchan.message, "setinfo \"%s\" \"%s\"\n",
			     var_name, value);
	}
    }
#endif
#endif

    Z_Free(var->string);	// free the old value string

    newstring = (char*)Z_Malloc(strlen(value) + 1);
    strcpy(newstring, value);
    var->string = newstring;
    var->value = Q_atof(var->string);

#ifdef NQ_HACK
    if (var->server && changed) {
	if (sv.active)
	    SV_BroadcastPrintf("\"%s\" changed to \"%s\"\n", var->name,
			       var->string);
    }
#endif

    if (changed && var->callback)
	var->callback(var);

#ifdef NQ_HACK
    // Don't allow deathmatch and coop at the same time...
    if ((var->value != 0) && (!strcmp(var->name, deathmatch.name)))
	Cvar_Set("coop", "0");
    if ((var->value != 0) && (!strcmp(var->name, coop.name)))
	Cvar_Set("deathmatch", "0");
#endif
}

/*
============
Cvar_SetValue
============
*/
void
Cvar_SetValue(const char *var_name, float value)
{
    char val[32];

    snprintf(val, sizeof(val), "%f", value);
    Cvar_Set(var_name, val);
}


/*
============
Cvar_RegisterVariable

Adds a freestanding variable to the variable list.
============
*/
void Cvar_RegisterVariable(cvar_t *variable)
{
   char value[512];		// FIXME - magic numbers...

   /* first check to see if it has allready been defined */
   if (Cvar_FindVar(variable->name))
   {
      Con_Printf("Can't register variable %s, allready defined\n",
            variable->name);
      return;
   }
   /* check for overlap with a command */
   if (Cmd_Exists(variable->name)) {
      Con_Printf("Cvar_RegisterVariable: %s is a command\n",
            variable->name);
      return;
   }

   variable->stree.string = variable->name;
   STree_Insert(&cvar_tree, &variable->stree);

   // copy the value off, because future sets will Z_Free it
   strncpy(value, variable->string, 511);
   value[511] = '\0';
   variable->string = (const char*)Z_Malloc(1);

   if (!(variable->flags & CVAR_CALLBACK))
     variable->callback = NULL;

   /*
    * FIXME (BARF) - readonly cvars need to be initialised
    */
   /* set it through the function to be consistant */
   Cvar_Set(variable->name, value);
}

/*
============
Cvar_SetCallback

Set a callback function to the var
============
*/
void Cvar_SetCallback (cvar_t *var, cvar_callback func)
{
	var->callback = func;
	if (func)
		var->flags |= CVAR_CALLBACK;
	else	var->flags &= ~CVAR_CALLBACK;
}
