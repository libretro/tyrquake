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
// Quake is a trademark of Id Software, Inc., (c) 1996 Id Software, Inc. All
// rights reserved.

#include <string.h>

#include "cdaudio.h"
#include "cmd.h"
#include "common.h"
#include "console.h"
#include "sound.h"

// FIXME - transitional hacks
extern qboolean cdValid;
extern qboolean enabled;
extern qboolean playing;
extern qboolean wasPlaying;
extern qboolean playLooping;
extern byte remap[];
extern byte playTrack;
extern byte maxTrack;
void CDAudio_Eject(void);
void CDAudio_CloseDoor(void);
int CDAudio_GetAudioDiskInfo(void);


static void
CD_f(void)
{
    char *command;
    int ret;
    int n;

    if (Cmd_Argc() < 2)
	return;

    command = Cmd_Argv(1);

    if (strcasecmp(command, "on") == 0) {
	enabled = true;
	return;
    }

    if (strcasecmp(command, "off") == 0) {
	if (playing)
	    CDAudio_Stop();
	enabled = false;
	return;
    }

    if (strcasecmp(command, "reset") == 0) {
	enabled = true;
	if (playing)
	    CDAudio_Stop();
	for (n = 0; n < 100; n++)
	    remap[n] = n;
	CDAudio_GetAudioDiskInfo();
	return;
    }

    if (strcasecmp(command, "remap") == 0) {
	ret = Cmd_Argc() - 2;
	if (ret <= 0) {
	    for (n = 1; n < 100; n++)
		if (remap[n] != n)
		    Con_Printf("  %u -> %u\n", n, remap[n]);
	    return;
	}
	for (n = 1; n <= ret; n++)
	    remap[n] = Q_atoi(Cmd_Argv(n + 1));
	return;
    }

    if (strcasecmp(command, "close") == 0) {
	CDAudio_CloseDoor();
	return;
    }

    if (!cdValid) {
	CDAudio_GetAudioDiskInfo();
	if (!cdValid) {
	    Con_Printf("No CD in player.\n");
	    return;
	}
    }

    if (strcasecmp(command, "play") == 0) {
	CDAudio_Play((byte)Q_atoi(Cmd_Argv(2)), false);
	return;
    }

    if (strcasecmp(command, "loop") == 0) {
	CDAudio_Play((byte)Q_atoi(Cmd_Argv(2)), true);
	return;
    }

    if (strcasecmp(command, "stop") == 0) {
	CDAudio_Stop();
	return;
    }

    if (strcasecmp(command, "pause") == 0) {
	CDAudio_Pause();
	return;
    }

    if (strcasecmp(command, "resume") == 0) {
	CDAudio_Resume();
	return;
    }

    if (strcasecmp(command, "eject") == 0) {
	if (playing)
	    CDAudio_Stop();
	CDAudio_Eject();
	cdValid = false;
	return;
    }

    if (strcasecmp(command, "info") == 0) {
	Con_Printf("%u tracks\n", maxTrack);
	if (playing)
	    Con_Printf("Currently %s track %u\n",
		       playLooping ? "looping" : "playing", playTrack);
	else if (wasPlaying)
	    Con_Printf("Paused %s track %u\n",
		       playLooping ? "looping" : "playing", playTrack);
	Con_Printf("Volume is %f\n", bgmvolume.value);
	return;
    }
}

int
CDAudio_Init_Common(void)
{
    Cmd_AddCommand("cd", CD_f);

    return 1;
}
