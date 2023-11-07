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
// console.c

#include <string.h>

#include "client.h"
#include "cmd.h"
#include "console.h"
#include "draw.h"
#include "keys.h"
#include "quakedef.h"
#include "screen.h"
#include "sys.h"
#include "zone.h"

#ifdef NQ_HACK
#include "host.h"
#include "sound.h"
#endif

#define CON_TEXTSIZE 16384
#define	NUM_CON_TIMES 4

console_t *con;			// point to current console
static console_t con_main;

int con_ormask = 0;
qboolean con_forcedup;
int con_totallines;		// total lines in console scrollback
int con_notifylines;		// scan lines to clear for notify lines

static int con_linewidth;	// characters across screen
static int con_vislines;

int
Con_GetWidth(void)
{
    return con_linewidth;
}

static float con_cursorspeed = 4;
static cvar_t con_notifytime = { "con_notifytime", "3" };	//seconds

static float con_times[NUM_CON_TIMES];	// realtime time the line was generated
					// for transparent notify lines

qboolean con_initialized;

/*
====================
Con_ToggleConsole_f
====================
*/
void
Con_ToggleConsole_f(void)
{
    Key_ClearTyping();

    if (key_dest == key_console) {
	if (!con_forcedup) {
	    key_dest = key_game;
	    Key_ClearTyping();
	}
    } else
	key_dest = key_console;

    Con_ClearNotify();
}

/*
================
Con_Clear_f
================
*/
void
Con_Clear_f(void)
{
    memset(con_main.text, ' ', CON_TEXTSIZE);
}


/*
================
Con_ClearNotify
================
*/
void
Con_ClearNotify(void)
{
    int i;

    for (i = 0; i < NUM_CON_TIMES; i++)
	con_times[i] = 0;
}


/*
================
Con_MessageMode_f
================
*/
void
Con_MessageMode_f(void)
{
    key_dest = key_message;
    chat_team = false;
}

/*
================
Con_MessageMode2_f
================
*/
void
Con_MessageMode2_f(void)
{
    key_dest = key_message;
    chat_team = true;
}

/*
================
Con_Resize

================
*/
static void Con_Resize(console_t * c)
{
   int width;
   char tbuf[CON_TEXTSIZE];

   width = (vid.width >> 3) - 2;

   if (width == con_linewidth)
      return;

   if (width < 1)		// video hasn't been initialized yet
   {
      width = 38;
      con_linewidth = width;
      con_totallines = CON_TEXTSIZE / con_linewidth;
      memset(c->text, ' ', CON_TEXTSIZE);
   }
   else
   {
      int i, j, numlines, numchars;
      int oldwidth = con_linewidth;
      int oldtotallines = con_totallines;

      con_linewidth  = width;
      con_totallines = CON_TEXTSIZE / con_linewidth;
      numlines = oldtotallines;

      if (con_totallines < numlines)
         numlines = con_totallines;

      numchars = oldwidth;

      if (con_linewidth < numchars)
         numchars = con_linewidth;

      memcpy(tbuf, c->text, CON_TEXTSIZE);
      memset(c->text, ' ', CON_TEXTSIZE);

      for (i = 0; i < numlines; i++)
      {
         for (j = 0; j < numchars; j++)
         {
            c->text[(con_totallines - 1 - i) * con_linewidth + j] =
               tbuf[((c->current - i + oldtotallines) %
                     oldtotallines) * oldwidth + j];
         }
      }
      Con_ClearNotify();
   }

   c->current = con_totallines - 1;
   c->display = c->current;
}

/*
================
Con_CheckResize

If the line width has changed, reformat the buffer.
================
*/
void
Con_CheckResize(void)
{
    Con_Resize(&con_main);
}

/*
===============
Con_Linefeed
===============
*/
void
Con_Linefeed(void)
{
    con->x = 0;
    if (con->display == con->current)
	con->display++;
    con->current++;
    memset(&con->text[(con->current % con_totallines) * con_linewidth]
	   , ' ', con_linewidth);
}

/*
================
Con_Print

Handles cursor positioning, line wrapping, etc
All console printing must go through this in order to be logged to disk
If no console is visible, the notify window will pop up.
================
*/
void
Con_Print(const char *txt)
{
    int y;
    int c, l;
    static int cr;
    int mask;

    if (txt[0] == 1 || txt[0] == 2) {
	mask = 128;		// go to colored text
	txt++;
#ifdef NQ_HACK
	if (txt[0] == 1)
	    S_LocalSound("misc/talk.wav");	// play talk wav
#endif
    } else
	mask = 0;

    while ((c = *txt)) {
	// count word length
	for (l = 0; l < con_linewidth; l++)
	    if (txt[l] <= ' ')
		break;

	// word wrap
	if (l != con_linewidth && (con->x + l > con_linewidth))
	    con->x = 0;

	txt++;

	if (cr) {
	    con->current--;
	    cr = false;
	}


	if (!con->x) {
	    Con_Linefeed();
	    // mark time for transparent overlay
	    if (con->current >= 0)
		con_times[con->current % NUM_CON_TIMES] = realtime;
	}

	switch (c) {
	case '\n':
	    con->x = 0;
	    break;

	case '\r':
	    con->x = 0;
	    cr = 1;
	    break;

	default:		// display character and advance
	    y = con->current % con_totallines;
	    con->text[y * con_linewidth + con->x] = c | mask | con_ormask;
	    con->x++;
	    if (con->x >= con_linewidth)
		con->x = 0;
	    break;
	}
    }
}


/*
================
Con_Printf

Handles cursor positioning, line wrapping, etc
================
*/
void Con_Printf(const char *fmt, ...)
{
   va_list argptr;
   char msg[MAX_PRINTMSG];

   va_start(argptr, fmt);
   vsnprintf(msg, sizeof(msg), fmt, argptr);
   va_end(argptr);

   if (!con_initialized)
      return;

#ifdef NQ_HACK
   if (cls.state == ca_dedicated)
      return;			// no graphics mode
#endif

   /* write it to the scrollable buffer */
   Con_Print(msg);

   /*
    * FIXME - not sure if this is ok, need to rework the screen update
    * criteria so it gets done once per frame unless explicitly flushed. For
    * now, don't update until we see a newline char.
    */
   if (!strchr(msg, '\n'))
      return;

   // update the screen immediately if the console is displayed
#ifdef NQ_HACK
   if (cls.state != ca_active && !scr_disabled_for_loading)
#else
      if (con_forcedup)
#endif
      {
         static qboolean inupdate;
         // protect against infinite loop if something in SCR_UpdateScreen calls
         // Con_Printd
         if (!inupdate)
         {
            inupdate = true;
            SCR_UpdateScreen();
            inupdate = false;
         }
      }
}

/*
==============================================================================

DRAWING

==============================================================================
*/


/*
================
Con_DrawInput

The input line scrolls horizontally if typing goes beyond the right edge
================
*/
void
Con_DrawInput(void)
{
    int y;
    int i;
    char *text;

    if (key_dest != key_console && !con_forcedup)
	return;			// don't draw anything

    text = key_lines[edit_line];

// add the cursor frame
    text[key_linepos] = 10 + ((int)(realtime * con_cursorspeed) & 1);

// fill out remainder with spaces
    for (i = key_linepos + 1; i < con_linewidth; i++)
	text[i] = ' ';

//      prestep if horizontally scrolling
    if (key_linepos >= con_linewidth)
	text += 1 + key_linepos - con_linewidth;

// draw it
    y = con_vislines - 22;
    for (i = 0; i < con_linewidth; i++)
	Draw_Character((i + 1) << 3, y, text[i]);

// remove cursor
    key_lines[edit_line][key_linepos] = 0;
}


/*
================
Con_DrawNotify

Draws the last few lines of output transparently over the game top
================
*/
void Con_DrawNotify(void)
{
   int i, x;
   char *text;
   float time;
   char *s;
   int v = 0;

   for (i = con->current - NUM_CON_TIMES + 1; i <= con->current; i++)
   {
      if (i < 0)
         continue;
      time = con_times[i % NUM_CON_TIMES];
      if (time == 0)
         continue;
      time = realtime - time;
      if (time > con_notifytime.value)
         continue;
      text = con->text + (i % con_totallines) * con_linewidth;

      clearnotify = 0;
      scr_copytop = 1;

      for (x = 0; x < con_linewidth; x++)
         Draw_Character((x + 1) << 3, v, text[x]);

      v += 8;
   }


   if (key_dest == key_message)
   {
      int skip;

      clearnotify = 0;
      scr_copytop = 1;

      if (chat_team)
      {
         Draw_String(8, v, "say_team:");
         skip = 11;
      }
      else
      {
         Draw_String(8, v, "say:");
         skip = 6;
      }

      s = chat_buffer;
      // FIXME = Truncating? should be while, not if?
      if (chat_bufferlen > (vid.width >> 3) - (skip + 1))
         s += chat_bufferlen - ((vid.width >> 3) - (skip + 1));

      x = 0;
      while (s[x])
      {
         Draw_Character((x + skip) << 3, v, s[x]);
         x++;
      }
      Draw_Character((x + skip) << 3, v,
            10 + ((int)(realtime * con_cursorspeed) & 1));
      v += 8;
   }

   if (v > con_notifylines)
      con_notifylines = v;
}

static void
Con_DrawDLBar(void)
{
#ifdef QW_HACK
    char dlbar[256];
    const char *dlname;
    char *buf;
    int bufspace;
    int namespace, len;
    int barchars, totalchars, markpos;
    int i;

    if (!cls.download)
	return;

    /*
     * totalchars = the space available for the whole bar + text
     * barchars   = the space for just the bar
     *
     * Bar looks like this:
     *  filename     <====*====> 42%
     *  longfilen... <==*======> 31%
     */
    dlname = COM_SkipPath(cls.downloadname);

    buf = dlbar;
    bufspace = sizeof(dlbar) - 1;

    namespace  = qmin(con_linewidth / 3, 20);
    totalchars = qmin(con_linewidth - 2, (int)sizeof(dlbar) - 1);

    if (strlen(dlname) > namespace) {
	len = snprintf(buf, bufspace, "%.*s... ", namespace - 2, dlname);
    } else {
	len = snprintf(buf, bufspace, "%-*s ", namespace + 1, dlname);
    }
    buf += len;
    totalchars -= len;
    barchars = totalchars - 7; /* 7 => 2 endcaps, space, "100%" */

    markpos = barchars * cls.downloadpercent / 100;
    *buf++ = '\x80';
    for (i = 0; i < barchars; i++) {
	if (i == markpos)
	    *buf++ = '\x83';
	else
	    *buf++ = '\x81';
    }
    *buf++ = '\x82';
    snprintf(buf, 6, " %3d%%", cls.downloadpercent);

    /* draw it */
    Draw_String(8, con_vislines - 22 + 8, dlbar);
#endif
}

/*
================
Con_DrawConsole

Draws the console with the solid background
FIXME - The input line at the bottom should only be drawn if typing is allowed
================
*/
void Con_DrawConsole(int lines)
{
   int i, x, y;
   int rows;
   char *text;
   int row;

   if (lines <= 0)
      return;

   // draw the background
   Draw_ConsoleBackground(lines);

   // draw the text
   con_vislines = lines;
   rows = (lines - 22) >> 3;	// rows of text to draw
   y = lines - 30;

   // draw from the bottom up
   if (con->display != con->current) {
      // draw arrows to show the buffer is backscrolled
      for (x = 0; x < con_linewidth; x += 4)
         Draw_Character((x + 1) << 3, y, '^');
      y -= 8;
      rows--;
   }

   row = con->display;
   for (i = 0; i < rows; i++, y -= 8, row--) {
      if (row < 0)
         break;
      if (con->current - row >= con_totallines)
         break;		// past scrollback wrap point

      text = con->text + (row % con_totallines) * con_linewidth;
      for (x = 0; x < con_linewidth; x++)
         Draw_Character((x + 1) << 3, y, text[x]);
   }

   // draw the download bar, if needed
   Con_DrawDLBar();

   // draw the input prompt, user text, and cursor if desired
   Con_DrawInput();
}

/*
================
Con_Init
================
*/
void
Con_Init(void)
{
    con_main.text = (char*)Hunk_AllocName(CON_TEXTSIZE, "conmain");

    con = &con_main;
    con_linewidth = -1;
    Con_CheckResize();

    Con_Printf("Console initialized.\n");

    /* register our commands */
    Cvar_RegisterVariable(&con_notifytime);

    Cmd_AddCommand("toggleconsole", Con_ToggleConsole_f);
    Cmd_AddCommand("messagemode", Con_MessageMode_f);
    Cmd_AddCommand("messagemode2", Con_MessageMode2_f);
    Cmd_AddCommand("clear", Con_Clear_f);

    con_initialized = true;
}
