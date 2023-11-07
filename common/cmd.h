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

#ifndef CMD_H
#define CMD_H

#include "qtypes.h"
#include "cvar.h"

// cmd.h -- Command buffer and command execution

//===========================================================================

/*

Any number of commands can be added in a frame, from several different sources.
Most commands come from either keybindings or console line input, but remote
servers can also send across commands and entire text files can be execed.

The + command line options are also added to the command buffer.

The game starts with a Cbuf_AddText ("exec quake.rc\n"); Cbuf_Execute ();

*/


void Cbuf_Init(void);

// allocates an initial text buffer that will grow as needed

void Cbuf_AddText(const char *fmt, ...);

// as new commands are generated from the console or keybindings,
// the text is added to the end of the command buffer.

void Cbuf_InsertText(const char *text);

// when a command wants to issue other commands immediately, the text is
// inserted at the beginning of the buffer, before any remaining unexecuted
// commands.

void Cbuf_Execute(void);

// Pulls off \n terminated lines of text from the command buffer and sends
// them through Cmd_ExecuteString.  Stops when the buffer is empty.
// Normally called once per frame, but may be explicitly invoked.
// Do not call inside a command function!

//===========================================================================

/*
 * Command execution takes a null terminated string, breaks it into tokens,
 * then searches for a command or variable that matches the first token.
 */

/* Command function */
typedef void (*xcommand_t)(void);

/*
 * Command argument completion function.
 * Pass in the argument string
 * Returns a string tree of possible completions
 * Requires STree_AllocInit() prior to calling
 */
typedef struct stree_root *(*cmd_arg_f)(const char *);

#ifdef NQ_HACK
/*
 * In NQ, commands can come from three sources, but the handler functions may
 * choose to dissallow the action or forward it to a remote server if the
 * source is not apropriate.
 */
typedef enum {
    src_client,		/* came in over a net connection as a clc_stringcmd
			   host_client will be valid during this state. */
    src_command,		/* from the command buffer */
    ENSURE_INT_CMD_SOURCE = 0x70000000
} cmd_source_t;

extern cmd_source_t cmd_source;

/*
 * Parses a single line of text into arguments and tries to execute it.
 * The text can come from the command buffer, a remote client, or stdin.
 */
void Cmd_ExecuteString(const char *text, cmd_source_t src);
#endif
#ifdef QW_HACK
/*
 * Parses a single line of text into arguments and tries to execute it as if
 * it was typed at the console
 */
void Cmd_ExecuteString(const char *text);
#endif

void Cmd_Init(void);

void Cmd_AddCommand(const char *cmd_name, xcommand_t function);
void Cmd_SetCompletion(const char *cmd_name, cmd_arg_f completion);

// called by the init functions of other parts of the program to
// register commands and functions to call for them.
// The cmd_name is referenced later, so it should not be in temp memory

qboolean Cmd_Exists(const char *cmd_name);
qboolean Cmd_Alias_Exists(const char *cmd_name);
// used by the cvar code to check for cvar / command name overlap

int Cmd_Argc(void);
const char *Cmd_Argv(int arg);
const char *Cmd_Args(void);

// Returns the position (1 to argc-1) in the command's argument list
// where the given parameter apears, or 0 if not present

void Cmd_TokenizeString(const char *text);

// Takes a null terminated string.  Does not need to be /n terminated.
// breaks the string up into arg tokens.

void Cmd_ForwardToServer(void);

// adds the current command line as a clc_stringcmd to the client message.
// things like godmode, noclip, etc, are commands directed to the server,
// so when they are typed in at the console, they will need to be forwarded.

#ifdef NQ_HACK
void Cmd_Print(const char *text);

// used by command functions to send output to either the graphics console or
// passed as a print message to the client
#endif
#ifdef QW_HACK
void Cmd_StuffCmds_f(void);
#endif

#endif /* CMD_H */
