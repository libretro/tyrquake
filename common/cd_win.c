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

#include <windows.h>

#include "cdaudio.h"
#include "cmd.h"
#include "common.h"
#include "console.h"
#include "quakedef.h"
#include "sound.h"
#include "winquake.h"

#ifdef NQ_HACK
#include "client.h"
#endif

// FIXME - transitional hacks
qboolean cdValid = false;
qboolean playing = false;
qboolean enabled = false;
qboolean playLooping = false;
static float cdvolume;
byte playTrack;
byte maxTrack;

static UINT wDeviceID;


void
CDDrv_Eject(void)
{
    DWORD dwReturn;

    dwReturn = mciSendCommand(wDeviceID, MCI_SET, MCI_SET_DOOR_OPEN,
			      (DWORD)NULL);
    if (dwReturn)
	Con_DPrintf("MCI_SET_DOOR_OPEN failed (%i)\n", dwReturn);
}


void
CDDrv_CloseDoor(void)
{
    DWORD dwReturn;

    dwReturn = mciSendCommand(wDeviceID, MCI_SET, MCI_SET_DOOR_CLOSED,
			      (DWORD)NULL);
    if (dwReturn)
	Con_DPrintf("MCI_SET_DOOR_CLOSED failed (%i)\n", dwReturn);
}


int
CDAudio_GetAudioDiskInfo(void)
{
    DWORD dwReturn;
    MCI_STATUS_PARMS mciStatusParms;

    cdValid = false;

    mciStatusParms.dwItem = MCI_STATUS_READY;
    dwReturn =
	mciSendCommand(wDeviceID, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT,
		       (DWORD)(LPVOID)&mciStatusParms);
    if (dwReturn) {
	Con_DPrintf("CDAudio: drive ready test - get status failed\n");
	return -1;
    }
    if (!mciStatusParms.dwReturn) {
	Con_DPrintf("CDAudio: drive not ready\n");
	return -1;
    }

    mciStatusParms.dwItem = MCI_STATUS_NUMBER_OF_TRACKS;
    dwReturn =
	mciSendCommand(wDeviceID, MCI_STATUS, MCI_STATUS_ITEM | MCI_WAIT,
		       (DWORD)(LPVOID)&mciStatusParms);
    if (dwReturn) {
	Con_DPrintf("CDAudio: get tracks - status failed\n");
	return -1;
    }
    if (mciStatusParms.dwReturn < 1) {
	Con_DPrintf("CDAudio: no music tracks\n");
	return -1;
    }

    cdValid = true;
    maxTrack = mciStatusParms.dwReturn;

    return 0;
}


int
CDDrv_IsAudioTrack(byte track)
{
    int ret = 1;
    DWORD dwReturn;
    MCI_STATUS_PARMS mciStatusParms;

    mciStatusParms.dwItem = MCI_CDA_STATUS_TYPE_TRACK;
    mciStatusParms.dwTrack = track;
    dwReturn =
	mciSendCommand(wDeviceID, MCI_STATUS,
		       MCI_STATUS_ITEM | MCI_TRACK | MCI_WAIT,
		       (DWORD)(LPVOID)&mciStatusParms);
    if (dwReturn) {
	ret = 0;
	Con_DPrintf("MCI_STATUS failed (%i)\n", dwReturn);
    } else  if (mciStatusParms.dwReturn != MCI_CDA_TRACK_AUDIO)
	ret = 0;

    return ret;
}

int
CDDrv_PlayTrack(byte track)
{
    DWORD dwReturn;
    MCI_STATUS_PARMS mciStatusParms;
    MCI_PLAY_PARMS mciPlayParms;

    mciStatusParms.dwItem = MCI_STATUS_LENGTH;
    mciStatusParms.dwTrack = track;
    dwReturn =
	mciSendCommand(wDeviceID, MCI_STATUS,
		       MCI_STATUS_ITEM | MCI_TRACK | MCI_WAIT,
		       (DWORD)(LPVOID)&mciStatusParms);
    if (dwReturn) {
	Con_DPrintf("MCI_STATUS failed (%i)\n", dwReturn);
	return 1;
    }

    mciPlayParms.dwFrom = MCI_MAKE_TMSF(track, 0, 0, 0);
    mciPlayParms.dwTo = (mciStatusParms.dwReturn << 8) | track;
    mciPlayParms.dwCallback = (DWORD)mainwindow;
    dwReturn =
	mciSendCommand(wDeviceID, MCI_PLAY, MCI_NOTIFY | MCI_FROM | MCI_TO,
		       (DWORD)(LPVOID)&mciPlayParms);
    if (dwReturn) {
	Con_DPrintf("CDAudio: MCI_PLAY failed (%i)\n", dwReturn);
	return 1;
    }

    return 0;
}


void
CDDrv_Stop(void)
{
    DWORD dwReturn;

    dwReturn = mciSendCommand(wDeviceID, MCI_STOP, 0, (DWORD)NULL);
    if (dwReturn)
	Con_DPrintf("MCI_STOP failed (%i)", dwReturn);
}


void
CDDrv_Pause(void)
{
    DWORD dwReturn;
    MCI_GENERIC_PARMS mciGenericParms;

    mciGenericParms.dwCallback = (DWORD)mainwindow;
    dwReturn = mciSendCommand(wDeviceID, MCI_PAUSE, 0,
			      (DWORD)(LPVOID)&mciGenericParms);
    if (dwReturn)
	Con_DPrintf("MCI_PAUSE failed (%i)", dwReturn);
}


void
CDDrv_Resume(void)
{
    DWORD dwReturn;
    MCI_PLAY_PARMS mciPlayParms;

    mciPlayParms.dwFrom = MCI_MAKE_TMSF(playTrack, 0, 0, 0);
    mciPlayParms.dwTo = MCI_MAKE_TMSF(playTrack + 1, 0, 0, 0);
    mciPlayParms.dwCallback = (DWORD)mainwindow;
    dwReturn =
	mciSendCommand(wDeviceID, MCI_PLAY, MCI_TO | MCI_NOTIFY,
		       (DWORD)(LPVOID)&mciPlayParms);
    if (dwReturn) {
	Con_DPrintf("CDAudio: MCI_PLAY failed (%i)\n", dwReturn);
	return;
    }
}


LONG
CDAudio_MessageHandler(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    if (lParam != wDeviceID)
	return 1;

    switch (wParam) {
    case MCI_NOTIFY_SUCCESSFUL:
	if (playing) {
	    playing = false;
	    if (playLooping)
		CDAudio_Play(playTrack, true);
	}
	break;

    case MCI_NOTIFY_ABORTED:
    case MCI_NOTIFY_SUPERSEDED:
	break;

    case MCI_NOTIFY_FAILURE:
	Con_DPrintf("MCI_NOTIFY_FAILURE\n");
	CDAudio_Stop();
	cdValid = false;
	break;

    default:
	Con_DPrintf("Unexpected MM_MCINOTIFY type (%i)\n", wParam);
	return 1;
    }

    return 0;
}


void
CDAudio_Update(void)
{
    if (!enabled)
	return;

    if (bgmvolume.value != cdvolume) {
	if (cdvolume) {
	    Cvar_SetValue("bgmvolume", 0.0);
	    cdvolume = bgmvolume.value;
	    CDAudio_Pause();
	} else {
	    Cvar_SetValue("bgmvolume", 1.0);
	    cdvolume = bgmvolume.value;
	    CDAudio_Resume();
	}
    }
}


int
CDDrv_InitDevice(void)
{
    DWORD dwReturn;
    MCI_OPEN_PARMS mciOpenParms;
    MCI_SET_PARMS mciSetParms;

    mciOpenParms.lpstrDeviceType = "cdaudio";
    dwReturn = mciSendCommand(0, MCI_OPEN, MCI_OPEN_TYPE | MCI_OPEN_SHAREABLE,
			      (DWORD)(LPVOID)&mciOpenParms);
    if (dwReturn) {
	Con_Printf("CDAudio_Init: MCI_OPEN failed (%i)\n", dwReturn);
	return -1;
    }
    wDeviceID = mciOpenParms.wDeviceID;

    // Set the time format to track/minute/second/frame (TMSF).
    mciSetParms.dwTimeFormat = MCI_FORMAT_TMSF;
    dwReturn = mciSendCommand(wDeviceID, MCI_SET, MCI_SET_TIME_FORMAT,
			      (DWORD)(LPVOID)&mciSetParms);
    if (dwReturn) {
	Con_Printf("MCI_SET_TIME_FORMAT failed (%i)\n", dwReturn);
	mciSendCommand(wDeviceID, MCI_CLOSE, 0, (DWORD)NULL);
	return -1;
    }

    return 0;
}


void
CDDrv_CloseDevice(void)
{
    if (mciSendCommand(wDeviceID, MCI_CLOSE, MCI_WAIT, (DWORD)NULL))
	Con_DPrintf("CDAudio_Shutdown: MCI_CLOSE failed\n");
}
