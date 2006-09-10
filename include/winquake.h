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

#ifndef WINQUAKE_H
#define WINQUAKE_H

// winquake.h: Win32-specific Quake header file
// FIXME - all this can be moved to win specific files for sound, vid, etc.

#ifndef _WIN32
#error "You shouldn't be including this file for non-Win32 stuff!"
#endif

// FIXME - mousewheel redefined? What is this magic number?
#include <windows.h>
#ifndef WM_MOUSEWHEEL
# define WM_MOUSEWHEEL 0x020A
#endif

#ifndef SERVERONLY
# include <ddraw.h>
# include <dsound.h>
#endif

#include "qtypes.h"

extern HINSTANCE global_hInstance;
extern int global_nCmdShow;

#ifndef SERVERONLY

extern LPDIRECTDRAW lpDD;
extern qboolean DDActive;
extern LPDIRECTDRAWSURFACE lpPrimary;
extern LPDIRECTDRAWSURFACE lpFrontBuffer;
extern LPDIRECTDRAWSURFACE lpBackBuffer;
extern LPDIRECTDRAWPALETTE lpDDPal;
extern LPDIRECTSOUND pDS;
extern LPDIRECTSOUNDBUFFER pDSBuf;

extern DWORD gSndBufSize;

//#define SNDBUFSIZE 65536

#endif

typedef enum {
    MS_WINDOWED, MS_FULLSCREEN, MS_FULLDIB, MS_UNINIT
} modestate_t;

extern modestate_t modestate;

extern HWND mainwindow;
extern qboolean ActiveApp;

extern qboolean WinNT;

//
// vid.h (or remove)
//

int VID_ForceUnlockedAndReturnState(void);
void VID_ForceLockState(int lk);
void VID_SetDefaultMode(void);

extern int window_center_x, window_center_y;
extern RECT window_rect;

//
// input.h (or remove)
//

void IN_ShowMouse(void);
void IN_DeactivateMouse(void);
void IN_HideMouse(void);
void IN_ActivateMouse(void);
void IN_RestoreOriginalMouseState(void);
void IN_SetQuakeMouseState(void);
void IN_MouseEvent(int mstate);
void IN_UpdateClipCursor(void);

extern qboolean mouseinitialized;

//
// sound.h (or remove)
//

void S_BlockSound(void);
void S_UnblockSound(void);

// misc stuff that should be elsewhere...

extern qboolean winsock_lib_initialized;
extern HWND hwnd_dialog;
extern HANDLE hinput, houtput;

// Networking?

int (PASCAL FAR *pWSAStartup) (WORD wVersionRequired, LPWSADATA lpWSAData);
int (PASCAL FAR *pWSACleanup) (void);
int (PASCAL FAR *pWSAGetLastError) (void);

SOCKET (PASCAL FAR *psocket) (int af, int type, int protocol);
int (PASCAL FAR *pioctlsocket) (SOCKET s, long cmd, u_long FAR *argp);
int (PASCAL FAR *psetsockopt) (SOCKET s, int level, int optname,
			       const char FAR *optval, int optlen);
int (PASCAL FAR *precvfrom) (SOCKET s, char FAR *buf, int len,
			     int flags, struct sockaddr FAR *from,
			     int FAR *fromlen);
int (PASCAL FAR *psendto) (SOCKET s, const char FAR *buf, int len,
			   int flags, const struct sockaddr FAR *to,
			   int tolen);
int (PASCAL FAR *pclosesocket) (SOCKET s);
int (PASCAL FAR *pgethostname) (char FAR *name, int namelen);
struct hostent FAR *(PASCAL FAR *pgethostbyname)(const char FAR *name);
struct hostent FAR *(PASCAL FAR *pgethostbyaddr)(const char FAR *addr,
						 int len, int type);
int (PASCAL FAR *pgetsockname) (SOCKET s, struct sockaddr FAR *name,
				int FAR *namelen);

/*
 * net stuff
 */
#define MAXHOSTNAMELEN 256
extern int winsock_initialized;
extern WSADATA winsockdata;

#endif /* WINQUAKE_H */
