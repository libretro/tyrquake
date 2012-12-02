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

#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>

#include "client.h"
#include "common.h"
#include "host.h"
#include "quakedef.h"
#include "sys.h"

qboolean isDedicated;

static qboolean noconinput = false;
static qboolean nostdout = false;

// =======================================================================
// General routines
// =======================================================================

void
Sys_Printf(const char *fmt, ...)
{
    va_list argptr;
    char text[MAX_PRINTMSG];
    unsigned char *p;

    va_start(argptr, fmt);
    vsnprintf(text, sizeof(text), fmt, argptr);
    va_end(argptr);

    if (nostdout)
	return;

    // FIXME - compare with NQ + use ctype functions?
    for (p = (unsigned char *)text; *p; p++) {
	if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
	    printf("[%02x]", *p);
	else
	    putc(*p, stdout);
    }
}

void
Sys_Quit(void)
{
    Host_Shutdown();
    fcntl(STDIN_FILENO, F_SETFL,
	  fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);
    fflush(stdout);
    exit(0);
}

void
Sys_Init(void)
{
#ifdef USE_X86_ASM
    Sys_SetFPCW();
#endif
}

void
Sys_Error(const char *error, ...)
{
    va_list argptr;
    char string[MAX_PRINTMSG];

// change stdin to non blocking
    fcntl(STDIN_FILENO, F_SETFL,
	  fcntl(STDIN_FILENO, F_GETFL, 0) & ~O_NONBLOCK);

    va_start(argptr, error);
    vsnprintf(string, sizeof(string), error, argptr);
    va_end(argptr);
    fprintf(stderr, "Error: %s\n", string);

    Host_Shutdown();
    exit(1);

}

/*
============
Sys_FileTime

returns -1 if not present
============
*/
int
Sys_FileTime(const char *path)
{
    struct stat buf;

    if (stat(path, &buf) == -1)
	return -1;

    return buf.st_mtime;
}


void
Sys_mkdir(const char *path)
{
    mkdir(path, 0777);
}


void
Sys_DebugLog(const char *file, const char *fmt, ...)
{
    va_list argptr;
    static char data[MAX_PRINTMSG];
    int fd;

    va_start(argptr, fmt);
    vsnprintf(data, sizeof(data), fmt, argptr);
    va_end(argptr);
//    fd = open(file, O_WRONLY | O_BINARY | O_CREAT | O_APPEND, 0666);
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND, 0666);
    write(fd, data, strlen(data));
    close(fd);
}

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

// =======================================================================
// Sleeps for microseconds
// =======================================================================

// FIXME - need this at all? (see QW)
char *
Sys_ConsoleInput(void)
{
    static char text[256];
    int len;
    fd_set fdset;
    struct timeval timeout;

    if (cls.state == ca_dedicated) {
	FD_ZERO(&fdset);
	FD_SET(STDIN_FILENO, &fdset);	// stdin
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;
	if (select(STDIN_FILENO + 1, &fdset, NULL, NULL, &timeout) == -1
	    || !FD_ISSET(STDIN_FILENO, &fdset))
	    return NULL;

	len = read(STDIN_FILENO, text, sizeof(text));
	if (len < 1)
	    return NULL;
	text[len - 1] = 0;	// rip off the /n and terminate

	return text;
    }
    return NULL;
}

#ifndef USE_X86_ASM
void
Sys_HighFPPrecision(void)
{
}

void
Sys_LowFPPrecision(void)
{
}
#endif

int
main(int c, const char **v)
{
    double time, oldtime, newtime;
    quakeparms_t parms;
    int j;

    signal(SIGFPE, SIG_IGN);

    memset(&parms, 0, sizeof(parms));

    COM_InitArgv(c, v);
    parms.argc = com_argc;
    parms.argv = com_argv;

#ifdef GLQUAKE
    parms.memsize = 16 * 1024 * 1024;
#else
    parms.memsize = 8 * 1024 * 1024;
#endif

    j = COM_CheckParm("-mem");
    if (j)
	parms.memsize = (int)(Q_atof(com_argv[j + 1]) * 1024 * 1024);
    parms.membase = malloc(parms.memsize);
    parms.basedir = stringify(QBASEDIR);
// caching is disabled by default, use -cachedir to enable
//      parms.cachedir = cachedir;

    fcntl(STDIN_FILENO, F_SETFL,
	  fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);

    Host_Init(&parms);

    Sys_Init();

    if (COM_CheckParm("-nostdout"))
	nostdout = true;

    // Make stdin non-blocking
    // FIXME - check both return values
    if (!noconinput)
	fcntl(STDIN_FILENO, F_SETFL,
	      fcntl(STDIN_FILENO, F_GETFL, 0) | O_NONBLOCK);
    if (!nostdout)
	printf("TyrQuake -- Version %s\n", stringify(TYR_VERSION));

    oldtime = Sys_DoubleTime() - 0.1;
    while (1) {
// find time spent rendering last frame
	newtime = Sys_DoubleTime();
	time = newtime - oldtime;

	if (cls.state == ca_dedicated) {
	    if (time < sys_ticrate.value) {
		usleep(1);
		continue;	// not time to run a server only tic yet
	    }
	    time = sys_ticrate.value;
	}

	if (time > sys_ticrate.value * 2)
	    oldtime = newtime;
	else
	    oldtime += time;

	Host_Frame(time);
    }
}


/*
================
Sys_MakeCodeWriteable
================
*/
void
Sys_MakeCodeWriteable(unsigned long startaddr, unsigned long length)
{
    int r;
    unsigned long addr;
    int psize = getpagesize();

    addr = (startaddr & ~(psize - 1)) - psize;

//      fprintf(stderr, "writable code %lx(%lx)-%lx, length=%lx\n", startaddr,
//                      addr, startaddr+length, length);

    r = mprotect((char *)addr, length + startaddr - addr + psize, 7);

    if (r < 0)
	Sys_Error("Protection change failed");
}
