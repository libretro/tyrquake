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
#include "common.h"
#include "console.h"
#include "pmove.h"
#include "quakedef.h"
#include "sys.h"
#include "zone.h"

static void CL_FinishTimeDemo(void);

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

    fclose(cls.demofile);
    cls.demofile = NULL;
    cls.state = ca_disconnected;
    cls.demoplayback = false;

    if (cls.timedemo)
	CL_FinishTimeDemo();
}

#define DEM_CMD		0
#define DEM_READ	1
#define DEM_SET		2

/*
====================
CL_GetDemoMessage

  FIXME...
====================
*/
qboolean CL_GetDemoMessage(void)
{
    int r, i, j;
    float f;
    float demotime;
    byte c;
    usercmd_t *pcmd;

    // read the time from the packet
    fread(&demotime, sizeof(demotime), 1, cls.demofile);
    demotime = LittleFloat(demotime);

    // decide if it is time to grab the next message
    if (cls.timedemo)
    {
	if (cls.td_lastframe < 0)
	    cls.td_lastframe = demotime;
	else if (demotime > cls.td_lastframe)
	{
	    cls.td_lastframe = demotime;
	    // rewind back to time
	    fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
		  SEEK_SET);
	    return 0;		// allready read this frame's message
	}
	if (!cls.td_starttime && cls.state == ca_active)
	{
	    cls.td_starttime = Sys_DoubleTime();
	    cls.td_startframe = host_framecount;
	}
	realtime = demotime;	// warp
    } else if (!cl.paused && cls.state >= ca_onserver) {	// allways grab until fully connected
	if (realtime + 1.0 < demotime) {
	    // too far back
	    realtime = demotime - 1.0;
	    // rewind back to time
	    fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
		  SEEK_SET);
	    return 0;
	} else if (realtime < demotime) {
	    // rewind back to time
	    fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
		  SEEK_SET);
	    return 0;		// don't need another message yet
	}
    } else
	realtime = demotime;	// we're warping

    if (cls.state < ca_demostart)
	Host_Error("CL_GetDemoMessage: cls.state != ca_active");

    // get the msg type
    fread(&c, sizeof(c), 1, cls.demofile);

    switch (c) {
    case DEM_CMD:
	// user sent input
	i = cls.netchan.outgoing_sequence & UPDATE_MASK;
	pcmd = &cl.frames[i].cmd;
	r = fread(pcmd, sizeof(*pcmd), 1, cls.demofile);
	if (r != 1) {
	    CL_StopPlayback();
	    return 0;
	}
	// byte order stuff
	for (j = 0; j < 3; j++)
	    pcmd->angles[j] = LittleFloat(pcmd->angles[j]);
	pcmd->forwardmove = LittleShort(pcmd->forwardmove);
	pcmd->sidemove = LittleShort(pcmd->sidemove);
	pcmd->upmove = LittleShort(pcmd->upmove);
	cl.frames[i].senttime = demotime;
	cl.frames[i].receivedtime = -1;	// we haven't gotten a reply yet
	cls.netchan.outgoing_sequence++;
	for (i = 0; i < 3; i++) {
	    fread(&f, 4, 1, cls.demofile);
	    cl.viewangles[i] = LittleFloat(f);
	}
	break;

    case DEM_READ:
	// get the next message
	fread(&net_message.cursize, 4, 1, cls.demofile);
	net_message.cursize = LittleLong(net_message.cursize);
	//Con_Printf("read: %ld bytes\n", net_message.cursize);
	if (net_message.cursize > MAX_MSGLEN)
	    Sys_Error("Demo message > MAX_MSGLEN");
	r = fread(net_message.data, net_message.cursize, 1, cls.demofile);
	if (r != 1) {
	    CL_StopPlayback();
	    return 0;
	}
	break;

    case DEM_SET:
	fread(&i, 4, 1, cls.demofile);
	cls.netchan.outgoing_sequence = LittleLong(i);
	fread(&i, 4, 1, cls.demofile);
	cls.netchan.incoming_sequence = LittleLong(i);
	break;

    default:
	Con_Printf("Corrupted demo.\n");
	CL_StopPlayback();
	return 0;
    }

    return 1;
}

/*
====================
CL_GetMessage

Handles recording and playback of demos, on top of NET_ code
====================
*/
qboolean
CL_GetMessage(void)
{
    if (cls.demoplayback)
	return CL_GetDemoMessage();

    if (!NET_GetPacket())
	return false;

    return true;
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

    if (Cmd_Argc() != 2) {
	Con_Printf("play <demoname> : plays a demo\n");
	return;
    }
    // disconnect from server
    CL_Disconnect();

    // open the demo file
    strcpy(name, Cmd_Argv(1));
    COM_DefaultExtension(name, ".qwd");

    Con_Printf("Playing demo from %s.\n", name);
    COM_FOpenFile(name, &cls.demofile);
    if (!cls.demofile) {
	Con_Printf("ERROR: couldn't open.\n");
	cls.demonum = -1;	// stop demo loop
	return;
    }

    cls.demoplayback = true;
    cls.state = ca_demostart;
    Netchan_Setup(&cls.netchan, net_from, 0);
    realtime = 0;
}

struct stree_root *CL_Demo_Arg_f(const char *arg)
{
    struct stree_root *root = Z_Malloc(sizeof(struct stree_root));
    if (root)
    {
	*root = STREE_ROOT;
	STree_AllocInit();
	COM_ScanDir(root, "", arg, ".qwd", true);
    }

    return root;
}

/*
====================
CL_FinishTimeDemo

====================
*/
static void
CL_FinishTimeDemo(void)
{
    int frames;
    float time;

    cls.timedemo = false;

    // the first frame didn't count
    frames = (host_framecount - cls.td_startframe) - 1;
    time   = Sys_DoubleTime() - cls.td_starttime;
    if (!time)
	time = 1;
    Con_Printf("%i frames %5.1f seconds %5.1f fps\n", frames, time,
	       frames / time);
}

/*
====================
CL_TimeDemo_f

timedemo [demoname]
====================
*/
void CL_TimeDemo_f(void)
{
    if (Cmd_Argc() != 2) {
	Con_Printf("timedemo <demoname> : gets demo speeds\n");
	return;
    }

    CL_PlayDemo_f();

    if (cls.state != ca_demostart)
	return;

    // cls.td_starttime will be grabbed at the second frame of the demo, so
    // all the loading time doesn't get counted

    cls.timedemo = true;
    cls.td_starttime = 0;
    cls.td_startframe = host_framecount;
    cls.td_lastframe = -1;	// get a new message this frame
}
