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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <linux/cdrom.h>
#include <paths.h>

#include "cdaudio.h"
#include "cmd.h"
#include "common.h"
#include "console.h"
#include "quakedef.h"
#include "sound.h"

#ifdef NQ_HACK
#include "client.h"
#endif

// FIXME - transitional hacks
extern int CDAudio_Init_Common(void);

qboolean cdValid = false;
qboolean playing = false;
qboolean wasPlaying = false;
static qboolean initialized = false;
qboolean enabled = true;
qboolean playLooping = false;
byte remap[100];
byte playTrack;
byte maxTrack;

static int cdfile = -1;
static char cd_dev[64] = _PATH_DEV "cdrom";
static struct cdrom_volctrl drv_vol_saved;
static struct cdrom_volctrl drv_vol;

void
CDAudio_Eject(void)
{
    if (cdfile == -1 || !enabled)
	return;			// no cd init'd

    if (ioctl(cdfile, CDROMEJECT) == -1)
	Con_DPrintf("ioctl cdromeject failed\n");
}


void
CDAudio_CloseDoor(void)
{
    if (cdfile == -1 || !enabled)
	return;			// no cd init'd

    if (ioctl(cdfile, CDROMCLOSETRAY) == -1)
	Con_DPrintf("ioctl cdromclosetray failed\n");
}

int
CDAudio_GetAudioDiskInfo(void)
{
    struct cdrom_tochdr tochdr;

    cdValid = false;

    if (ioctl(cdfile, CDROMREADTOCHDR, &tochdr) == -1) {
	Con_DPrintf("ioctl cdromreadtochdr failed\n");
	return -1;
    }

    if (tochdr.cdth_trk0 < 1) {
	Con_DPrintf("CDAudio: no music tracks\n");
	return -1;
    }

    cdValid = true;
    maxTrack = tochdr.cdth_trk1;

    return 0;
}


void
CDAudio_Play(byte track, qboolean looping)
{
    struct cdrom_tocentry entry;
    struct cdrom_ti ti;

    if (cdfile == -1 || !enabled)
	return;

    if (!cdValid) {
	CDAudio_GetAudioDiskInfo();
	if (!cdValid)
	    return;
    }

    track = remap[track];

    if (track < 1 || track > maxTrack) {
	Con_DPrintf("CDAudio: Bad track number %u.\n", track);
	return;
    }
    // don't try to play a non-audio track
    entry.cdte_track = track;
    entry.cdte_format = CDROM_MSF;
    if (ioctl(cdfile, CDROMREADTOCENTRY, &entry) == -1) {
	Con_DPrintf("ioctl cdromreadtocentry failed\n");
	return;
    }
    if (entry.cdte_ctrl == CDROM_DATA_TRACK) {
	Con_Printf("CDAudio: track %i is not audio\n", track);
	return;
    }

    if (playing) {
	if (playTrack == track)
	    return;
	CDAudio_Stop();
    }

    ti.cdti_trk0 = track;
    ti.cdti_trk1 = track;
    ti.cdti_ind0 = 1;
    ti.cdti_ind1 = 99;

    if (ioctl(cdfile, CDROMPLAYTRKIND, &ti) == -1) {
	Con_DPrintf("ioctl cdromplaytrkind failed\n");
	return;
    }

    if (ioctl(cdfile, CDROMRESUME) == -1)
	Con_DPrintf("ioctl cdromresume failed\n");

    playLooping = looping;
    playTrack = track;
    playing = true;
}


void
CDAudio_Stop(void)
{
    if (cdfile == -1 || !enabled)
	return;

    if (!playing)
	return;

    if (ioctl(cdfile, CDROMSTOP) == -1)
	Con_DPrintf("ioctl cdromstop failed (%d)\n", errno);

    wasPlaying = false;
    playing = false;
}

void
CDAudio_Pause(void)
{
    if (cdfile == -1 || !enabled)
	return;

    if (!playing)
	return;

    if (ioctl(cdfile, CDROMPAUSE) == -1)
	Con_DPrintf("ioctl cdrompause failed\n");

    wasPlaying = playing;
    playing = false;
}


void
CDAudio_Resume(void)
{
    if (cdfile == -1 || !enabled)
	return;

    if (!cdValid)
	return;

    if (!wasPlaying)
	return;

    if (ioctl(cdfile, CDROMRESUME) == -1)
	Con_DPrintf("ioctl cdromresume failed\n");
    playing = true;
}

void
CDAudio_Update(void)
{
    struct cdrom_subchnl subchnl;
    static time_t lastchk;

    if (!enabled)
	return;

    if ((int)(255.0 * bgmvolume.value) != (int)drv_vol.channel0) {
	if (bgmvolume.value > 1.0f)
	    Cvar_SetValue ("bgmvolume", 1.0f);
	if (bgmvolume.value < 0.0f)
	    Cvar_SetValue ("bgmvolume", 0.0f);

	drv_vol.channel0 = drv_vol.channel2 =
	    drv_vol.channel1 = drv_vol.channel3 = bgmvolume.value * 255.0;
	if (ioctl(cdfile, CDROMVOLCTRL, &drv_vol) == -1 )
	    Con_DPrintf("ioctl CDROMVOLCTRL failed\n");
    }

    if (playing && lastchk < time(NULL)) {
	lastchk = time(NULL) + 2;	//two seconds between chks
	subchnl.cdsc_format = CDROM_MSF;
	if (ioctl(cdfile, CDROMSUBCHNL, &subchnl) == -1) {
	    Con_DPrintf("ioctl cdromsubchnl failed\n");
	    playing = false;
	    return;
	}
	if (subchnl.cdsc_audiostatus != CDROM_AUDIO_PLAY &&
	    subchnl.cdsc_audiostatus != CDROM_AUDIO_PAUSED) {
	    playing = false;
	    if (playLooping)
		CDAudio_Play(playTrack, true);
	}
    }
}

int
CDAudio_Init(void)
{
    int i;

#ifdef NQ_HACK
    // FIXME - not a valid client state in QW?
    if (cls.state == ca_dedicated)
	return -1;
#endif

    if (COM_CheckParm("-nocdaudio"))
	return -1;

    if ((i = COM_CheckParm("-cddev")) != 0 && i < com_argc - 1) {
	strncpy(cd_dev, com_argv[i + 1], sizeof(cd_dev));
	cd_dev[sizeof(cd_dev) - 1] = 0;
    }

    if ((cdfile = open(cd_dev, O_RDONLY | O_NONBLOCK)) == -1) {
	Con_Printf("CDAudio_Init: open of \"%s\" failed (%i)\n", cd_dev,
		   errno);
	cdfile = -1;
	return -1;
    }

    for (i = 0; i < 100; i++)
	remap[i] = i;
    initialized = true;
    enabled = true;

    Con_Printf("CD Audio Initialized\n");

    if (CDAudio_GetAudioDiskInfo()) {
	Con_Printf("CDAudio_Init: No CD in player.\n");
	cdValid = false;
    }

    CDAudio_Init_Common();

    /* get drive's current volume */
    if (ioctl(cdfile, CDROMVOLREAD, &drv_vol_saved) == -1) {
	Con_DPrintf("ioctl CDROMVOLREAD failed\n");
	drv_vol_saved.channel0 = drv_vol_saved.channel2 =
	    drv_vol_saved.channel1 = drv_vol_saved.channel3 = 255.0;
    }
    /* set our own volume */
    drv_vol.channel0 = drv_vol.channel2 =
	drv_vol.channel1 = drv_vol.channel3 = bgmvolume.value * 255.0;
    if (ioctl(cdfile, CDROMVOLCTRL, &drv_vol) == -1)
	Con_Printf("ioctl CDROMVOLCTRL failed\n");

    return 0;
}


void
CDAudio_Shutdown(void)
{
    if (!initialized)
	return;
    CDAudio_Stop();

    /* Restore the saved volume setting */
    if (ioctl(cdfile, CDROMVOLCTRL, &drv_vol_saved) == -1)
	Con_DPrintf("ioctl CDROMVOLCTRL failed\n");

    close(cdfile);
    cdfile = -1;
}
