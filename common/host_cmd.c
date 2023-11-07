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

#include "client.h"
#include "cmd.h"
#include "console.h"
#include "host.h"
#include "keys.h"
#include "menu.h"
#include "model.h"
#include "net.h"
#include "protocol.h"
#include "quakedef.h"
#include "screen.h"
#include "server.h"
#include "sys.h"
#include "world.h"
#include "zone.h"

#include <streams/file_stream.h>

/* forward declarations */
RFILE* rfopen(const char *path, const char *mode);
int rfscanf(RFILE * stream, const char * format, ...);
int rfeof(RFILE* stream);
int rfgetc(RFILE* stream);
int rfclose(RFILE* stream);
int rfprintf(RFILE * stream, const char * format, ...);
int64_t rfflush(RFILE * stream);

int current_skill;

/*
==================
Host_Quit_f
==================
*/

void
Host_Quit_f(void)
{
    if (key_dest != key_console && cls.state != ca_dedicated) {
	M_Menu_Quit_f();
	return;
    }
    CL_Disconnect();
    Host_ShutdownServer(false);
}

/*
==================
Host_God_f

Sets client to godmode
==================
*/
void
Host_God_f(void)
{
    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    if (pr_global_struct->deathmatch)
	return;

    sv_player->v.flags = (int)sv_player->v.flags ^ FL_GODMODE;
    if (!((int)sv_player->v.flags & FL_GODMODE))
	SV_ClientPrintf("godmode OFF\n");
    else
	SV_ClientPrintf("godmode ON\n");
}

void
Host_Notarget_f(void)
{
    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    if (pr_global_struct->deathmatch)
	return;

    sv_player->v.flags = (int)sv_player->v.flags ^ FL_NOTARGET;
    if (!((int)sv_player->v.flags & FL_NOTARGET))
	SV_ClientPrintf("notarget OFF\n");
    else
	SV_ClientPrintf("notarget ON\n");
}

qboolean noclip_anglehack;

void
Host_Noclip_f(void)
{
    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    if (pr_global_struct->deathmatch)
	return;

    if (sv_player->v.movetype != MOVETYPE_NOCLIP) {
	noclip_anglehack = true;
	sv_player->v.movetype = MOVETYPE_NOCLIP;
	SV_ClientPrintf("noclip ON\n");
    } else {
	noclip_anglehack = false;
	sv_player->v.movetype = MOVETYPE_WALK;
	SV_ClientPrintf("noclip OFF\n");
    }
}

/*
==================
Host_Fly_f

Sets client to flymode
==================
*/
void
Host_Fly_f(void)
{
    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    if (pr_global_struct->deathmatch)
	return;

    if (sv_player->v.movetype != MOVETYPE_FLY) {
	sv_player->v.movetype = MOVETYPE_FLY;
	SV_ClientPrintf("flymode ON\n");
    } else {
	sv_player->v.movetype = MOVETYPE_WALK;
	SV_ClientPrintf("flymode OFF\n");
    }
}


/*
==================
Host_Ping_f

==================
*/
void
Host_Ping_f(void)
{
    int i, j;
    float total;
    client_t *client;

    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    SV_ClientPrintf("Client ping times:\n");
    for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++) {
	if (!client->active)
	    continue;
	total = 0;
	for (j = 0; j < NUM_PING_TIMES; j++)
	    total += client->ping_times[j];
	total /= NUM_PING_TIMES;
	SV_ClientPrintf("%4i %s\n", (int)(total * 1000), client->name);
    }
}

/*
===============================================================================

SERVER TRANSITIONS

===============================================================================
*/


/*
======================
Host_Map_f

handle a
map <servername>
command from the console.  Active clients are kicked off.
======================
*/
void Host_Map_f(void)
{
   int i;
   char name[MAX_QPATH];

   if (cmd_source != src_command)
      return;

   if (Cmd_Argc() < 2)
   {
      /* no map name given */
      Con_Printf ("map <levelname>: start a new server\n");
      if (cls.state == ca_dedicated)
      {
         if (sv.active)
            Con_Printf ("Currently on: %s\n", sv.name);
         else
            Con_Printf ("Server not active\n");
      }
      else if (cls.state >= ca_connected)
         Con_Printf ("Currently on: %s ( %s )\n", cl.levelname, cl.mapname);
      return;
   }

   cls.demonum = -1;		// stop demo loop in case this fails

   CL_Disconnect();
   Host_ShutdownServer(false);

   key_dest = key_game;	// remove console or menu
   SCR_BeginLoadingPlaque();

   svs.serverflags = 0;	// haven't completed an episode yet
   strcpy(name, Cmd_Argv(1));

   SV_SpawnServer(name);

   if (!sv.active)
      return;

   if (cls.state != ca_dedicated) {
      strcpy(cls.spawnparms, "");

      for (i = 2; i < Cmd_Argc(); i++) {
         strcat(cls.spawnparms, Cmd_Argv(i));
         strcat(cls.spawnparms, " ");
      }
      Cmd_ExecuteString("connect local", src_command);
   }
}

static struct stree_root * Host_Map_Arg_f(const char *arg)
{
   struct stree_root *root = (struct stree_root*)
      Z_Malloc(sizeof(struct stree_root));

   if (root)
   {
      root->entries = 0;
      root->maxlen  = 0;
      root->minlen  = -1;
      root->stack   = NULL;

      STree_AllocInit();
      COM_ScanDir(root, "maps", arg, ".bsp", true);
   }
   return root;
}

/*
==================
Host_Changelevel_f

Goes to a new map, taking all clients along
==================
*/
void Host_Changelevel_f(void)
{
   char level[MAX_QPATH];

   if (Cmd_Argc() != 2)
   {
      Con_Printf
         ("changelevel <levelname> : continue game on a new level\n");
      return;
   }

   if (!sv.active || cls.demoplayback)
   {
      Con_Printf("Only the server may changelevel\n");
      return;
   }
   SV_SaveSpawnparms();
   strcpy(level, Cmd_Argv(1));
   SV_SpawnServer(level);
}

/*
==================
Host_Restart_f

Restarts the current server for a dead player
==================
*/
void Host_Restart_f(void)
{
   char mapname[MAX_QPATH];

   if (cls.demoplayback || !sv.active)
      return;

   if (cmd_source != src_command)
      return;
   strcpy(mapname, sv.name);	// must copy out, because it gets cleared
   // in sv_spawnserver
   SV_SpawnServer(mapname);
}

/*
==================
Host_Reconnect_f

This command causes the client to wait for the signon messages again.
This is sent just before a server changes levels
==================
*/
void Host_Reconnect_f(void)
{
   SCR_BeginLoadingPlaque();
   cls.signon = 0;		// need new connection messages

   // FIXME - this check is just paranoia until I understand it better
   if (cls.state < ca_connected)
      Host_Error("Host_Reconnect_f: cls.state < ca_connected");

   cls.state = ca_connected;
}

/*
=====================
Host_Connect_f

User command to connect to server
=====================
*/
void Host_Connect_f(void)
{
   char name[MAX_QPATH];

   cls.demonum = -1;		// stop demo loop in case this fails
   if (cls.demoplayback)
   {
      CL_StopPlayback();
      CL_Disconnect();
   }
   strcpy(name, Cmd_Argv(1));
   CL_EstablishConnection(name);
   Host_Reconnect_f();
}


/*
===============================================================================

LOAD / SAVE GAME

===============================================================================
*/

#define	SAVEGAME_VERSION	5

/*
===============
Host_SavegameComment

Writes a SAVEGAME_COMMENT_LENGTH character comment describing the current
===============
*/
static void Host_SavegameComment(char *text)
{
   int i;
   char kills[20];

   for (i = 0; i < SAVEGAME_COMMENT_LENGTH; i++)
      text[i] = ' ';
   memcpy(text, cl.levelname, strlen(cl.levelname));
   sprintf(kills, "kills:%3i/%3i", cl.stats[STAT_MONSTERS],
         cl.stats[STAT_TOTALMONSTERS]);
   memcpy(text + 22, kills, strlen(kills));
   // convert space to _ to make stdio happy
   for (i = 0; i < SAVEGAME_COMMENT_LENGTH; i++)
      if (text[i] == ' ')
         text[i] = '_';
   text[SAVEGAME_COMMENT_LENGTH] = '\0';
}


/*
===============
Host_Savegame_f
===============
*/
void Host_Savegame_f(void)
{
   char name[256];
   RFILE *f;
   int i;
   char comment[SAVEGAME_COMMENT_LENGTH + 1];
#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

   if (cmd_source != src_command)
      return;

   if (!sv.active) {
      Con_Printf("Not playing a local game.\n");
      return;
   }

   if (cl.intermission) {
      Con_Printf("Can't save in intermission.\n");
      return;
   }

   if (svs.maxclients != 1) {
      Con_Printf("Can't save multiplayer games.\n");
      return;
   }

   if (Cmd_Argc() != 2) {
      Con_Printf("save <savename> : save a game\n");
      return;
   }

   if (strstr(Cmd_Argv(1), "..")) {
      Con_Printf("Relative pathnames are not allowed.\n");
      return;
   }

   for (i = 0; i < svs.maxclients; i++) {
      if (svs.clients[i].active && (svs.clients[i].edict->v.health <= 0)) {
         Con_Printf("Can't savegame with a dead player\n");
         return;
      }
   }

   sprintf(name, "%s%c%s", com_savedir, slash, Cmd_Argv(1));
   COM_DefaultExtension(name, ".sav");

   Con_Printf("Saving game to %s...\n", name);
   f = rfopen(name, "w");
   if (!f) {
      Con_Printf("ERROR: couldn't open.\n");
      return;
   }

   rfprintf(f, "%i\n", SAVEGAME_VERSION);
   Host_SavegameComment(comment);
   rfprintf(f, "%s\n", comment);
   for (i = 0; i < NUM_SPAWN_PARMS; i++)
      rfprintf(f, "%f\n", svs.clients->spawn_parms[i]);
   rfprintf(f, "%d\n", current_skill);
   rfprintf(f, "%s\n", sv.name);
   rfprintf(f, "%f\n", sv.time);

   // write the light styles

   for (i = 0; i < MAX_LIGHTSTYLES; i++) {
      if (sv.lightstyles[i])
         rfprintf(f, "%s\n", sv.lightstyles[i]);
      else
         rfprintf(f, "m\n");
   }


   ED_WriteGlobals(f);
   for (i = 0; i < sv.num_edicts; i++) {
      ED_Write(f, EDICT_NUM(i));
      rfflush(f);
   }
   rfclose(f);
   Con_Printf("done.\n");
}


/*
===============
Host_Loadgame_f
===============
*/
void Host_Loadgame_f(void)
{
   char name[MAX_OSPATH];
   RFILE *f;
   char mapname[MAX_QPATH];
   float time, tfloat;
   char str[32768];
   char *lightstyle;
   const char *start;
   int i, r;
   edict_t *ent;
   int entnum;
   int version;
   float spawn_parms[NUM_SPAWN_PARMS];
#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

   if (cmd_source != src_command)
      return;

   if (Cmd_Argc() != 2) {
      Con_Printf("load <savename> : load a game\n");
      return;
   }

   cls.demonum = -1;		// stop demo loop in case this fails

   sprintf(name, "%s%c%s", com_savedir, slash, Cmd_Argv(1));
   COM_DefaultExtension(name, ".sav");

   // we can't call SCR_BeginLoadingPlaque, because too much stack space has
   // been used.  The menu calls it before stuffing loadgame command
   //      SCR_BeginLoadingPlaque();

   Con_Printf("Loading game from %s...\n", name);
   f = rfopen(name, "r");
   if (!f) {
      Con_Printf("ERROR: couldn't open.\n");
      return;
   }

   rfscanf(f, "%i\n", &version);
   if (version != SAVEGAME_VERSION)
   {
      rfclose(f);
      Con_Printf("Savegame is version %i, not %i\n", version,
            SAVEGAME_VERSION);
      return;
   }
   rfscanf(f, "%s\n", str);
   for (i = 0; i < NUM_SPAWN_PARMS; i++)
      rfscanf(f, "%f\n", &spawn_parms[i]);

   /*
    * This silliness is so we can load 1.06 save files, which have float
    * skill values
    */
   rfscanf(f, "%f\n", &tfloat);
   current_skill = (int)(tfloat + 0.1);
   Cvar_SetValue("skill", (float)current_skill);

   rfscanf(f, "%s\n", mapname);
   rfscanf(f, "%f\n", &time);

   CL_Disconnect_f();

   SV_SpawnServer(mapname);

   if (!sv.active) {
      Con_Printf("Couldn't load map\n");
      rfclose(f);
      return;
   }
   sv.paused = true;		// pause until all clients connect
   sv.loadgame = true;

   // load the light styles

   for (i = 0; i < MAX_LIGHTSTYLES; i++)
   {
      rfscanf(f, "%s\n", str);
      lightstyle = (char*)Hunk_Alloc(strlen(str) + 1);
      strcpy(lightstyle, str);
      sv.lightstyles[i] = lightstyle;
   }

   // load the edicts out of the savegame file
   entnum = -1;		// -1 is the globals
   while (!rfeof(f)) {
      for (i = 0; i < sizeof(str) - 1; i++) {
         r = rfgetc(f);
         if (r == EOF || !r)
            break;
         str[i] = r;
         if (r == '}') {
            i++;
            break;
         }
      }
      if (i == sizeof(str) - 1)
         Sys_Error("Loadgame buffer overflow");
      str[i] = 0;
      start = COM_Parse(str);
      if (!com_token[0])
         break;		// end of file
      if (strcmp(com_token, "{"))
         Sys_Error("First token isn't a brace");

      if (entnum == -1) {	// parse the global vars
         ED_ParseGlobals(start);
      } else {		// parse an edict

         ent = EDICT_NUM(entnum);
         memset(&ent->v, 0, progs->entityfields * 4);
         ent->free = false;
         ED_ParseEdict(start, ent);

         // link it into the bsp tree
         if (!ent->free)
            SV_LinkEdict(ent, false);
      }

      entnum++;
   }

   sv.num_edicts = entnum;
   sv.time = time;

   rfclose(f);

   for (i = 0; i < NUM_SPAWN_PARMS; i++)
      svs.clients->spawn_parms[i] = spawn_parms[i];

   if (cls.state != ca_dedicated) {
      CL_EstablishConnection("local");
      Host_Reconnect_f();
   }
}

//============================================================================

/*
======================
Host_Name_f
======================
*/
void Host_Name_f(void)
{
   char new_name[16];

   if (Cmd_Argc() == 1) {
      Con_Printf("\"name\" is \"%s\"\n", cl_name.string);
      return;
   }
   if (Cmd_Argc() == 2)
      strncpy(new_name, Cmd_Argv(1), sizeof(new_name));
   else
      strncpy(new_name, Cmd_Args(), sizeof(new_name));
   new_name[sizeof(new_name) - 1] = 0;

   if (cmd_source == src_command) {
      if (strcmp(cl_name.string, new_name) == 0)
         return;
      Cvar_Set("_cl_name", new_name);
      if (cls.state >= ca_connected)
         Cmd_ForwardToServer();
      return;
   }

   if (host_client->name[0] && strcmp(host_client->name, "unconnected"))
      if (strcmp(host_client->name, new_name) != 0)
         Con_Printf("%s renamed to %s\n", host_client->name, new_name);
   strcpy(host_client->name, new_name);
   host_client->edict->v.netname = PR_SetString(host_client->name);

   // send notification to all clients

   MSG_WriteByte(&sv.reliable_datagram, svc_updatename);
   MSG_WriteByte(&sv.reliable_datagram, host_client - svs.clients);
   MSG_WriteString(&sv.reliable_datagram, host_client->name);
}

void
Host_Say(qboolean teamonly)
{
    client_t *client;
    client_t *save;
    int i;
    size_t len, space, p_len;
    const char *p;
    char text[64];
    qboolean fromServer = false;

    if (cmd_source == src_command) {
	if (cls.state == ca_dedicated) {
	    fromServer = true;
	    teamonly = false;
	} else {
	    Cmd_ForwardToServer();
	    return;
	}
    }

    if (Cmd_Argc() < 2)
	return;

    save = host_client;

// turn on color set 1
    if (!fromServer)
	sprintf(text, "%c%s: ", 1, save->name);
    else
	sprintf(text, "%c<%s> ", 1, hostname.string);

    len   = strlen(text);
    space = sizeof(text) - len - 2; // -2 for \n and null terminator
    p     = Cmd_Args();
    p_len = strlen(p);
    if (*p == '"') {
	/* remove quotes */
	strncat(text, p + 1, qmin(p_len - 2, space));
	text[len + qmin(p_len - 2, space)] = 0;
    } else {
	strncat(text, p, space);
	text[len + qmin(p_len, space)] = 0;
    }
    strcat(text, "\n");

    for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++) {
	if (!client || !client->active || !client->spawned)
	    continue;
	if (teamplay.value && teamonly
	    && client->edict->v.team != save->edict->v.team)
	    continue;
	host_client = client;
	SV_ClientPrintf("%s", text);
    }
    host_client = save;
}


void
Host_Say_f(void)
{
    Host_Say(false);
}


void
Host_Say_Team_f(void)
{
    Host_Say(true);
}


void Host_Tell_f(void)
{
   size_t p_len;
   client_t *client;
   client_t *save;
   int i, len, space;
   const char *p;
   char text[64];

   if (cmd_source == src_command) {
      Cmd_ForwardToServer();
      return;
   }

   if (Cmd_Argc() < 3)
      return;

   strcpy(text, host_client->name);
   strcat(text, ": ");

   len   = strlen(text);
   space = sizeof(text) - len - 2; // -2 for \n and null terminator
   p     = Cmd_Args();
   p_len = strlen(p);
   if (*p == '"') {
      /* remove quotes */
      strncat(text, p + 1, qmin((int)p_len - 2, space));
      text[len + qmin((int)p_len - 2, space)] = 0;
   } else {
      strncat(text, p, space);
      text[len + qmin((int)p_len, space)] = 0;
   }
   strcat(text, "\n");

   save = host_client;
   for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++) {
      if (!client->active || !client->spawned)
         continue;
      if (strcasecmp(client->name, Cmd_Argv(1)))
         continue;
      host_client = client;
      SV_ClientPrintf("%s", text);
      break;
   }
   host_client = save;
}


/*
==================
Host_Color_f
==================
*/
void
Host_Color_f(void)
{
    int top, bottom;
    int playercolor;

    if (Cmd_Argc() == 1) {
	Con_Printf("\"color\" is \"%i %i\"\n", ((int)cl_color.value) >> 4,
		   ((int)cl_color.value) & 0x0f);
	Con_Printf("color <0-13> [0-13]\n");
	return;
    }

    if (Cmd_Argc() == 2)
	top = bottom = atoi(Cmd_Argv(1));
    else {
	top = atoi(Cmd_Argv(1));
	bottom = atoi(Cmd_Argv(2));
    }

    top &= 15;
    if (top > 13)
	top = 13;
    bottom &= 15;
    if (bottom > 13)
	bottom = 13;

    playercolor = top * 16 + bottom;

    if (cmd_source == src_command) {
	Cvar_SetValue("_cl_color", playercolor);
	if (cls.state >= ca_connected)
	    Cmd_ForwardToServer();
	return;
    }

    host_client->colors = playercolor;
    host_client->edict->v.team = bottom + 1;

// send notification to all clients
    MSG_WriteByte(&sv.reliable_datagram, svc_updatecolors);
    MSG_WriteByte(&sv.reliable_datagram, host_client - svs.clients);
    MSG_WriteByte(&sv.reliable_datagram, host_client->colors);
}

/*
==================
Host_Kill_f
==================
*/
void
Host_Kill_f(void)
{
    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    if (sv_player->v.health <= 0) {
	SV_ClientPrintf("Can't suicide -- allready dead!\n");
	return;
    }

    pr_global_struct->time = sv.time;
    pr_global_struct->self = EDICT_TO_PROG(sv_player);
    PR_ExecuteProgram(pr_global_struct->ClientKill);
}


/*
==================
Host_Pause_f
==================
*/
void
Host_Pause_f(void)
{

    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }
    if (!pausable.value)
	SV_ClientPrintf("Pause not allowed.\n");
    else {
	sv.paused ^= 1;

	if (sv.paused) {
	    SV_BroadcastPrintf("%s paused the game\n",
			       PR_GetString(sv_player->v.netname));
	} else {
	    SV_BroadcastPrintf("%s unpaused the game\n",
			       PR_GetString(sv_player->v.netname));
	}

	// send notification to all clients
	MSG_WriteByte(&sv.reliable_datagram, svc_setpause);
	MSG_WriteByte(&sv.reliable_datagram, sv.paused);
    }
}

//===========================================================================


/*
==================
Host_PreSpawn_f
==================
*/
void
Host_PreSpawn_f(void)
{
    if (cmd_source == src_command) {
	Con_Printf("prespawn is not valid from the console\n");
	return;
    }

    if (host_client->spawned) {
	Con_Printf("prespawn not valid -- allready spawned\n");
	return;
    }

    SZ_Write(&host_client->message, sv.signon.data, sv.signon.cursize);
    MSG_WriteByte(&host_client->message, svc_signonnum);
    MSG_WriteByte(&host_client->message, 2);
    host_client->sendsignon = true;
}

/*
==================
Host_Spawn_f
==================
*/
void
Host_Spawn_f(void)
{
    int i;
    client_t *client;
    edict_t *ent;

    if (cmd_source == src_command) {
	Con_Printf("spawn is not valid from the console\n");
	return;
    }

    if (host_client->spawned) {
	Con_Printf("Spawn not valid -- allready spawned\n");
	return;
    }
// run the entrance script
    if (sv.loadgame) {		// loaded games are fully inited allready
	// if this is the last client to be connected, unpause
	sv.paused = false;
    } else {
	// set up the edict
	ent = host_client->edict;

	memset(&ent->v, 0, progs->entityfields * 4);
	ent->v.colormap = NUM_FOR_EDICT(ent);
	ent->v.team = (host_client->colors & 15) + 1;
	ent->v.netname = PR_SetString(host_client->name);

	// copy spawn parms out of the client_t

	for (i = 0; i < NUM_SPAWN_PARMS; i++)
	    (&pr_global_struct->parm1)[i] = host_client->spawn_parms[i];

	// call the spawn function

	pr_global_struct->time = sv.time;
	pr_global_struct->self = EDICT_TO_PROG(sv_player);
	PR_ExecuteProgram(pr_global_struct->ClientConnect);

	PR_ExecuteProgram(pr_global_struct->PutClientInServer);
    }


// send all current names, colors, and frag counts
    SZ_Clear(&host_client->message);

// send time of update
    MSG_WriteByte(&host_client->message, svc_time);
    MSG_WriteFloat(&host_client->message, sv.time);

    for (i = 0, client = svs.clients; i < svs.maxclients; i++, client++) {
	MSG_WriteByte(&host_client->message, svc_updatename);
	MSG_WriteByte(&host_client->message, i);
	MSG_WriteString(&host_client->message, client->name);
	MSG_WriteByte(&host_client->message, svc_updatefrags);
	MSG_WriteByte(&host_client->message, i);
	MSG_WriteShort(&host_client->message, client->old_frags);
	MSG_WriteByte(&host_client->message, svc_updatecolors);
	MSG_WriteByte(&host_client->message, i);
	MSG_WriteByte(&host_client->message, client->colors);
    }

// send all current light styles
    for (i = 0; i < MAX_LIGHTSTYLES; i++) {
	MSG_WriteByte(&host_client->message, svc_lightstyle);
	MSG_WriteByte(&host_client->message, (char)i);
	MSG_WriteString(&host_client->message, sv.lightstyles[i]);
    }

//
// send some stats
//
    MSG_WriteByte(&host_client->message, svc_updatestat);
    MSG_WriteByte(&host_client->message, STAT_TOTALSECRETS);
    MSG_WriteLong(&host_client->message, pr_global_struct->total_secrets);

    MSG_WriteByte(&host_client->message, svc_updatestat);
    MSG_WriteByte(&host_client->message, STAT_TOTALMONSTERS);
    MSG_WriteLong(&host_client->message, pr_global_struct->total_monsters);

    MSG_WriteByte(&host_client->message, svc_updatestat);
    MSG_WriteByte(&host_client->message, STAT_SECRETS);
    MSG_WriteLong(&host_client->message, pr_global_struct->found_secrets);

    MSG_WriteByte(&host_client->message, svc_updatestat);
    MSG_WriteByte(&host_client->message, STAT_MONSTERS);
    MSG_WriteLong(&host_client->message, pr_global_struct->killed_monsters);


//
// send a fixangle
// Never send a roll angle, because savegames can catch the server
// in a state where it is expecting the client to correct the angle
// and it won't happen if the game was just loaded, so you wind up
// with a permanent head tilt
    ent = EDICT_NUM(1 + (host_client - svs.clients));
    MSG_WriteByte(&host_client->message, svc_setangle);
    for (i = 0; i < 2; i++)
	MSG_WriteAngle(&host_client->message, ent->v.angles[i]);
    MSG_WriteAngle(&host_client->message, 0);

    SV_WriteClientdataToMessage(sv_player, &host_client->message);

    MSG_WriteByte(&host_client->message, svc_signonnum);
    MSG_WriteByte(&host_client->message, 3);
    host_client->sendsignon = true;
}

/*
==================
Host_Begin_f
==================
*/
void
Host_Begin_f(void)
{
    if (cmd_source == src_command) {
	Con_Printf("begin is not valid from the console\n");
	return;
    }

    host_client->spawned = true;
}

//===========================================================================


/*
==================
Host_Kick_f

Kicks a user off of the server
==================
*/
void
Host_Kick_f(void)
{
    const char *who;
    const char *message = NULL;
    client_t *save;
    int i;
    qboolean byNumber = false;

    if (cmd_source == src_command) {
	if (!sv.active) {
	    Cmd_ForwardToServer();
	    return;
	}
    } else if (pr_global_struct->deathmatch)
	return;

    save = host_client;

    if (Cmd_Argc() > 2 && strcmp(Cmd_Argv(1), "#") == 0) {
	i = Q_atof(Cmd_Argv(2)) - 1;
	if (i < 0 || i >= svs.maxclients)
	    return;
	if (!svs.clients[i].active)
	    return;
	host_client = &svs.clients[i];
	byNumber = true;
    } else {
	for (i = 0, host_client = svs.clients; i < svs.maxclients;
	     i++, host_client++) {
	    if (!host_client->active)
		continue;
	    if (strcasecmp(host_client->name, Cmd_Argv(1)) == 0)
		break;
	}
    }

    if (i < svs.maxclients) {
	if (cmd_source == src_command)
	    if (cls.state == ca_dedicated)
		who = "Console";
	    else
		who = cl_name.string;
	else
	    who = save->name;

	// can't kick yourself!
	if (host_client == save)
	    return;

	if (Cmd_Argc() > 2) {
	    message = COM_Parse(Cmd_Args());
	    if (byNumber) {
		message++;	// skip the #
		while (*message == ' ')	// skip white space
		    message++;
		message += strlen(Cmd_Argv(2));	// skip the number
	    }
	    while (*message && *message == ' ')
		message++;
	}
	if (message)
	    SV_ClientPrintf("Kicked by %s: %s\n", who, message);
	else
	    SV_ClientPrintf("Kicked by %s\n", who);
	SV_DropClient(false);
    }

    host_client = save;
}

/*
===============================================================================

DEBUGGING TOOLS

===============================================================================
*/

/*
==================
Host_Give_f
==================
*/
void
Host_Give_f(void)
{
    const char *t;
    int v;
    eval_t *val;

    if (cmd_source == src_command) {
	Cmd_ForwardToServer();
	return;
    }

    if (pr_global_struct->deathmatch)
	return;

    t = Cmd_Argv(1);
    v = atoi(Cmd_Argv(2));

    switch (t[0]) {
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
	// MED 01/04/97 added hipnotic give stuff
	if (hipnotic) {
	    if (t[0] == '6') {
		if (t[1] == 'a')
		    sv_player->v.items =
			(int)sv_player->v.items | HIT_PROXIMITY_GUN;
		else
		    sv_player->v.items =
			(int)sv_player->v.items | IT_GRENADE_LAUNCHER;
	    } else if (t[0] == '9')
		sv_player->v.items =
		    (int)sv_player->v.items | HIT_LASER_CANNON;
	    else if (t[0] == '0')
		sv_player->v.items = (int)sv_player->v.items | HIT_MJOLNIR;
	    else if (t[0] >= '2')
		sv_player->v.items =
		    (int)sv_player->v.items | (IT_SHOTGUN << (t[0] - '2'));
	} else {
	    if (t[0] >= '2')
		sv_player->v.items =
		    (int)sv_player->v.items | (IT_SHOTGUN << (t[0] - '2'));
	}
	break;

    case 's':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_shells1");
	    if (val)
		val->_float = v;
	}

	sv_player->v.ammo_shells = v;
	break;
    case 'n':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_nails1");
	    if (val) {
		val->_float = v;
		if (sv_player->v.weapon <= IT_LIGHTNING)
		    sv_player->v.ammo_nails = v;
	    }
	} else {
	    sv_player->v.ammo_nails = v;
	}
	break;
    case 'l':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_lava_nails");
	    if (val) {
		val->_float = v;
		if (sv_player->v.weapon > IT_LIGHTNING)
		    sv_player->v.ammo_nails = v;
	    }
	}
	break;
    case 'r':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_rockets1");
	    if (val) {
		val->_float = v;
		if (sv_player->v.weapon <= IT_LIGHTNING)
		    sv_player->v.ammo_rockets = v;
	    }
	} else {
	    sv_player->v.ammo_rockets = v;
	}
	break;
    case 'm':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_multi_rockets");
	    if (val) {
		val->_float = v;
		if (sv_player->v.weapon > IT_LIGHTNING)
		    sv_player->v.ammo_rockets = v;
	    }
	}
	break;
    case 'h':
	sv_player->v.health = v;
	break;
    case 'c':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_cells1");
	    if (val) {
		val->_float = v;
		if (sv_player->v.weapon <= IT_LIGHTNING)
		    sv_player->v.ammo_cells = v;
	    }
	} else {
	    sv_player->v.ammo_cells = v;
	}
	break;
    case 'p':
	if (rogue) {
	    val = GetEdictFieldValue(sv_player, "ammo_plasma");
	    if (val) {
		val->_float = v;
		if (sv_player->v.weapon > IT_LIGHTNING)
		    sv_player->v.ammo_cells = v;
	    }
	}
	break;
    }
}

/*
===============================================================================

DEMO LOOP CONTROL

===============================================================================
*/


/*
==================
Host_Startdemos_f
==================
*/
void
Host_Startdemos_f(void)
{
    int i, c;

    if (cls.state == ca_dedicated) {
	if (!sv.active)
	    Cbuf_AddText("map start\n");
	return;
    }

    c = Cmd_Argc() - 1;
    if (c > MAX_DEMOS) {
	Con_Printf("Max %i demos in demoloop\n", MAX_DEMOS);
	c = MAX_DEMOS;
    }
    Con_Printf("%i demo(s) in loop\n", c);

    for (i = 1; i < c + 1; i++)
	strncpy(cls.demos[i - 1], Cmd_Argv(i), sizeof(cls.demos[0]) - 1);

    if (!sv.active && cls.demonum != -1 && !cls.demoplayback) {
	cls.demonum = 0;
	CL_NextDemo();
    } else
	cls.demonum = -1;
}


/*
==================
Host_Demos_f

Return to looping demos
==================
*/
void
Host_Demos_f(void)
{
    if (cls.state == ca_dedicated)
	return;
    if (cls.demonum == -1)
	cls.demonum = 1;
    CL_Disconnect_f();
    CL_NextDemo();
}

/*
==================
Host_Stopdemo_f

Return to looping demos
==================
*/
void
Host_Stopdemo_f(void)
{
    if (cls.state == ca_dedicated)
	return;
    if (!cls.demoplayback)
	return;
    CL_StopPlayback();
    CL_Disconnect();
}

//=============================================================================

/*
==================
Host_InitCommands
==================
*/
void
Host_InitCommands(void)
{
    Cmd_AddCommand("quit", Host_Quit_f);
    Cmd_AddCommand("god", Host_God_f);
    Cmd_AddCommand("notarget", Host_Notarget_f);
    Cmd_AddCommand("fly", Host_Fly_f);
    Cmd_AddCommand("restart", Host_Restart_f);

    Cmd_AddCommand("map", Host_Map_f);
    Cmd_AddCommand("changelevel", Host_Changelevel_f);
    Cmd_SetCompletion("map", Host_Map_Arg_f);
    Cmd_SetCompletion("changelevel", Host_Map_Arg_f);

    Cmd_AddCommand("connect", Host_Connect_f);
    Cmd_AddCommand("reconnect", Host_Reconnect_f);
    Cmd_AddCommand("name", Host_Name_f);
    Cmd_AddCommand("noclip", Host_Noclip_f);

    Cmd_AddCommand("say", Host_Say_f);
    Cmd_AddCommand("say_team", Host_Say_Team_f);
    Cmd_AddCommand("tell", Host_Tell_f);
    Cmd_AddCommand("color", Host_Color_f);
    Cmd_AddCommand("kill", Host_Kill_f);
    Cmd_AddCommand("pause", Host_Pause_f);
    Cmd_AddCommand("spawn", Host_Spawn_f);
    Cmd_AddCommand("begin", Host_Begin_f);
    Cmd_AddCommand("prespawn", Host_PreSpawn_f);
    Cmd_AddCommand("kick", Host_Kick_f);
    Cmd_AddCommand("ping", Host_Ping_f);
    Cmd_AddCommand("load", Host_Loadgame_f);
    Cmd_AddCommand("save", Host_Savegame_f);
    Cmd_AddCommand("give", Host_Give_f);

    Cmd_AddCommand("startdemos", Host_Startdemos_f);
    Cmd_AddCommand("demos", Host_Demos_f);
    Cmd_AddCommand("stopdemo", Host_Stopdemo_f);
}
