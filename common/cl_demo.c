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
#include "net.h"
#include "protocol.h"
#include "quakedef.h"
#include "sys.h"
#include "zone.h"

#include <streams/file_stream.h>

/* forward declarations */
int rfclose(RFILE* stream);
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int rfgetc(RFILE* stream);

/*
==============================================================================

DEMO CODE

When a demo is playing back, all NET_SendMessages are skipped, and
NET_GetMessages are read from the demo file.

Whenever cl.time gets past the last received message, another message is
read from the demo file.
==============================================================================
*/

/*
==============
CL_StopPlayback

Called when a demo file runs out, or the user starts a game
==============
*/
void
CL_StopPlayback(void)
{
    if (!cls.demoplayback)
	return;

    rfclose(cls.demofile);
    cls.demoplayback = false;
    cls.demofile     = NULL;
    cls.state        = ca_disconnected;
}

/*
====================
CL_GetMessage

Handles recording and playback of demos, on top of NET_ code
====================
*/
int
CL_GetMessage(void)
{
   int r;
#ifdef MSB_FIRST
   float f;
#endif

   if (cls.demoplayback)
   {
      int i;

      // decide if it is time to grab the next message
      // allways grab until fully connected
      if (cls.state == ca_active)
      {
         if (cl.time <= cl.mtime[0])
         {
            // don't need another message yet
            return 0;
         }
      }
      // get the next message
      rfread(&net_message.cursize, 4, 1, cls.demofile);
      VectorCopy(cl.mviewangles[0], cl.mviewangles[1]);

      for (i = 0; i < 3; i++)
#ifdef MSB_FIRST
      {
         rfread(&f, 4, 1, cls.demofile);
         cl.mviewangles[0][i] = LittleFloat(f);
      }

      net_message.cursize = LittleLong(net_message.cursize);
#else
      r = rfread (&cl.mviewangles[0][i], 4, 1, cls.demofile);
#endif

      if (net_message.cursize > MAX_MSGLEN)
         Sys_Error("Demo message > MAX_MSGLEN");
      r = rfread(net_message.data, net_message.cursize, 1, cls.demofile);
      if (r != 1) {
         CL_StopPlayback();
         return 0;
      }

      return 1;
   }

   while (1)
   {
      r = NET_GetMessage(cls.netcon);

      if (r != 1 && r != 2)
         return r;

      // discard nop keepalive message
      if (net_message.cursize == 1 && net_message.data[0] == svc_nop)
         Con_Printf("<-- server to client keepalive\n");
      else
         break;
   }

   return r;
}

/*
====================
CL_PlayDemo_f

play [demoname]
====================
*/
void
CL_PlayDemo_f(void)
{
    char name[256];
    int c;
    qboolean neg = false;

    if (cmd_source != src_command)
	return;

    if (Cmd_Argc() != 2) {
	Con_Printf("play <demoname> : plays a demo\n");
	return;
    }
//
// disconnect from server
//
    CL_Disconnect();

//
// open the demo file
//
    strcpy(name, Cmd_Argv(1));
    COM_DefaultExtension(name, ".dem");

    Con_Printf("Playing demo from %s.\n", name);
    COM_FOpenFile(name, &cls.demofile);
    if (!cls.demofile) {
	Con_Printf("ERROR: couldn't open.\n");
	cls.demonum = -1;	// stop demo loop
	return;
    }

    cls.demoplayback = true;
    cls.state = ca_connected;
    cls.forcetrack = 0;

    while ((c = rfgetc(cls.demofile)) != '\n')
	if (c == '-')
	    neg = true;
	else
	    cls.forcetrack = cls.forcetrack * 10 + (c - '0');

    if (neg)
	cls.forcetrack = -cls.forcetrack;
}

struct stree_root *
CL_Demo_Arg_f(const char *arg)
{
    struct stree_root *root;

    root = (struct stree_root*)Z_Malloc(sizeof(struct stree_root));
    if (root)
    {
    root->entries = 0;
    root->maxlen = 0;
    root->minlen = -1;
    root->stack = NULL;
	STree_AllocInit();
	COM_ScanDir(root, "", arg, ".dem", true);
    }

    return root;
}
