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
#include <sys/types.h>

#include "common.h"
#include "cvar.h"
#include "qwsvdef.h"
#include "server.h"
#include "sys.h"

#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

// FIXME - header hacks
extern int net_socket;

static cvar_t sys_extrasleep = { "sys_extrasleep", "0" };

static qboolean stdin_ready;

/*
===============================================================================

				REQUIRED SYS FUNCTIONS

===============================================================================
*/

/*
================
Sys_DoubleTime
================
*/
double
Sys_DoubleTime(void)
{
    struct timeval tp;
    struct timezone tzp;
    static int secbase;

    gettimeofday(&tp, &tzp);

    if (!secbase) {
	secbase = tp.tv_sec;
	return tp.tv_usec / 1000000.0;
    }

    return (tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
}

/*
================
Sys_Error
================
*/
void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char string[MAX_PRINTMSG];

    va_start(argptr, error);
    vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);

    exit(1);
}

/*
================
Sys_Printf
================
*/
void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;
    static char text[MAX_PRINTMSG];

    va_start(argptr, fmt);
    vsnprintf(text, sizeof(text), fmt, argptr);
    va_end(argptr);
}


/*
================
Sys_Quit
================
*/
void
Sys_Quit(void)
{
    exit(0);			// appkit isn't running
}

static int do_stdin = 1;

/*
================
Sys_ConsoleInput

Checks for a complete line of text typed in at the console, then forwards
it to the host command processor
================
*/
char *
Sys_ConsoleInput(void)
{
    static char text[256];
    int len;

    if (!stdin_ready || !do_stdin)
	return NULL;		// the select didn't say it was ready
    stdin_ready = false;

    len = read(0, text, sizeof(text));
    if (len == 0) {
	// end of file
	do_stdin = 0;
	return NULL;
    }
    if (len < 1)
	return NULL;
    text[len - 1] = 0;		// rip off the /n and terminate

    return text;
}

/*
=============
Sys_Init

Quake calls this so the system can register variables before host_hunklevel
is marked
=============
*/
void
Sys_Init(void)
{
    Cvar_RegisterVariable(&sys_extrasleep);
}

/*
=============
main
=============
*/
int
main(int argc, const char *argv[])
{
    double time, oldtime, newtime;
    quakeparms_t parms;
    fd_set fdset;
    struct timeval timeout;
    int j;

    memset(&parms, 0, sizeof(parms));

    COM_InitArgv(argc, argv);
    parms.argc = com_argc;
    parms.argv = com_argv;

    parms.memsize = 16 * 1024 * 1024;

    j = COM_CheckParm("-mem");
    if (j)
	parms.memsize = (int)(Q_atof(com_argv[j + 1]) * 1024 * 1024);
    if ((parms.membase = malloc(parms.memsize)) == NULL)
	Sys_Error("Can't allocate %d", parms.memsize);

    parms.basedir = stringify(QBASEDIR);

    SV_Init(&parms);

    // run one frame immediately for first heartbeat
    SV_Frame(0.1);

    // main loop
    oldtime = Sys_DoubleTime() - 0.1;
    while (1) {
	// select on the net socket and stdin
	// the only reason we have a timeout at all is so that if the last
	// connected client times out, the message would not otherwise
	// be printed until the next event.
	FD_ZERO(&fdset);
	if (do_stdin)
	    FD_SET(0, &fdset);
	FD_SET(net_socket, &fdset);
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	if (select(net_socket + 1, &fdset, NULL, NULL, &timeout) == -1)
	    continue;
	stdin_ready = FD_ISSET(0, &fdset);

	// find time passed since last cycle
	newtime = Sys_DoubleTime();
	time = newtime - oldtime;
	oldtime = newtime;

	SV_Frame(time);

	// extrasleep is just a way to generate a fucked up connection on purpose
	if (sys_extrasleep.value)
	    usleep(sys_extrasleep.value);
    }

    return 0;
}
