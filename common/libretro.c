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

#include "libretro.h"
#include <retro_miscellaneous.h>
#include <retro_stat.h>

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#elif defined(_WIN32) && defined(_XBOX)
#include <xtl.h>
#endif
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#include "cmd.h"
#include "common.h"
#include "quakedef.h"
#include "d_local.h"
#include "sys.h"

#include "qtypes.h"
#include "sound.h"
#include "bgmusic.h"
#include "keys.h"
#include "cdaudio_driver.h"

#ifdef NQ_HACK
#include "client.h"
#include "host.h"

qboolean isDedicated;
#endif

#ifdef GEKKO
#include <ogc/lwp_watchdog.h>
#endif

#ifdef __CELLOS_LV2__
#include <sys/sys_time.h>
#include <sys/timer.h>
#endif

#define SURFCACHE_SIZE 10485760

extern void CDAudio_Update(void);

unsigned width;
unsigned height;
unsigned device_type = 0;

unsigned MEMSIZE_MB;

static struct retro_rumble_interface rumble;

#if defined(HW_DOL)
#define DEFAULT_MEMSIZE_MB 8
#elif defined(WIIU)
#define DEFAULT_MEMSIZE_MB 32
#elif defined(HW_RVL) || defined(_XBOX1) 
#define DEFAULT_MEMSIZE_MB 24
#else
#define DEFAULT_MEMSIZE_MB 32
#endif

#define SAMPLERATE 44100
#define ANALOG_THRESHOLD 4096 * 4
//is there a deadzone setting in retroarch?
#define ANALOG_DEADZONE 2700
//is the range delcared somewhere in retroarch?
#define ANALOG_RANGE 32768



cvar_t framerate = { "framerate", "60", true };
static bool initial_resolution_set = false;


unsigned char *heap;

#define MAX_PADS 1
static unsigned quake_devices[1];

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

// =======================================================================
// General routines
// =======================================================================
//
static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
retro_environment_t environ_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;

void Sys_Printf(const char *fmt, ...)
{
#if 0
   char buffer[256];
   va_list ap;
   va_start(ap, fmt);
   vsprintf(buffer, fmt, ap);
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "%s\n", buffer);
   va_end(ap);
#endif
}

void Sys_Quit(void)
{
   Host_Shutdown();
}

void Sys_Init(void)
{
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "RGB565 is not supported.\n");
   }
}

bool Sys_Error(const char *error, ...)
{
   char buffer[256];
   va_list ap;
   va_start(ap, error);
   vsprintf(buffer, error, ap);
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "%s\n", buffer);
   va_end(ap);

   return false;
}

/*
   ============
   Sys_FileTime

   returns -1 if not present
   ============
   */
int Sys_FileTime(const char *path)
{
   struct stat buf;

   if (stat(path, &buf) == -1)
      return -1;

   return buf.st_mtime;
}

void Sys_mkdir(const char *path)
{
#if defined(PSP) || defined(VITA)
   sceIoMkdir(path, 0777);
#elif defined(_WIN32)
   mkdir(path);
#else
   mkdir(path, 0777);
#endif
}

void Sys_DebugLog(const char *file, const char *fmt, ...)
{
}

double Sys_DoubleTime(void)
{
   static int first = true;
   static double oldtime = 0.0, curtime = 0.0;
   double newtime;
#if defined(WIIU)
   uint64_t OSGetSystemTime();
   newtime = (OSGetSystemTime() / 62156250.f);
#elif defined(GEKKO)
   newtime = ticks_to_microsecs(gettime()) / 1000000.0;
#elif defined(__CELLOS_LV2__)
   newtime = sys_time_get_system_time() / 1000000.0;
#elif defined(_WIN32)
   static double pfreq;
   static __int64 startcount;
   __int64 pcount;

   if (!pfreq)
   {
      __int64 freq;
      if (QueryPerformanceFrequency((LARGE_INTEGER*)&freq) && freq > 0)
      {
         //hardware timer available
         pfreq = (double)freq;
         QueryPerformanceCounter((LARGE_INTEGER*)&startcount);
      }
   }

   QueryPerformanceCounter((LARGE_INTEGER*)&pcount);
   /* TODO -check for wrapping - is it necessary? */
   newtime = (pcount - startcount) / pfreq;
#else
   struct timeval tp;
   gettimeofday(&tp, NULL);
   newtime = (double)tp.tv_sec + tp.tv_usec / 1000000.0;
#endif

   if (first)
   {
      first = false;
      oldtime = newtime;
   }

   if (newtime < oldtime)
   {
#if 0
      // warn if it's significant
      if (newtime - oldtime < -0.01)
         Con_Printf("Sys_DoubleTime: time stepped backwards (went from %f to %f, difference %f)\n", oldtime, newtime, newtime - oldtime);
#endif
   }
   else
      curtime += newtime - oldtime;
   oldtime = newtime;

   return curtime;
}

// =======================================================================
// Sleeps for microseconds
// =======================================================================

void
IN_Accumulate(void)
{}

char * Sys_ConsoleInput(void)
{
   return NULL;
}

qboolean
window_visible(void)
{
    return true;
}

void Sys_HighFPPrecision(void)
{
}

void Sys_LowFPPrecision(void)
{
}

viddef_t vid;			// global video state

void retro_init(void)
{
   struct retro_log_callback log;

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;
}

void retro_deinit(void)
{
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         quake_devices[port] = RETRO_DEVICE_JOYPAD;
         break;
      case RETRO_DEVICE_KEYBOARD:
         quake_devices[port] = RETRO_DEVICE_KEYBOARD;
         break;
      default:
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[libretro]: Invalid device.\n");
   }
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "TyrQuake";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = "v" stringify(TYR_VERSION) GIT_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = "pak";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = framerate.value;
   info->timing.sample_rate = SAMPLERATE;

   info->geometry.base_width   = width;
   info->geometry.base_height  = height;
   info->geometry.max_width    = width;
   info->geometry.max_height   = height;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

#define GP_MAXBINDS 32

typedef struct {
   char *name;
   struct retro_input_descriptor desc[GP_MAXBINDS];
   struct {
      char *key;
      char *com;
   } bind[GP_MAXBINDS];
} gp_layout_t;

const char *layouts = "Change retropad layout; 1: New layout|2: Old layout";

gp_layout_t gp_layouts[] = {
   {
      .name = "1: New layout",
      .desc = {
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Look right" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Look down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Look left" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Look up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Previous weapon" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Next weapon" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Jump" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Fire" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Toggle run mode" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Swim up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Toggle console" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
         { 0 },
      },
      .bind = {
         {"JOY_LEFT",  "+moveleft"},     {"JOY_RIGHT", "+moveright"},
         {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
         {"JOY_B",     "+right"},        {"JOY_A",     "+lookdown"},
         {"JOY_X",     "+left"},         {"JOY_Y",     "+lookup"},
         {"JOY_L",     "impulse 12"},    {"JOY_R",     "impulse 10"},
         {"JOY_L2",    "+jump"},         {"JOY_R2",    "+attack"},
         {"JOY_L3",    "+togglewalk"},   {"JOY_R3",    "+moveup"},
         {"JOY_SELECT","toggleconsole"}, {"JOY_START", "togglemenu"},
         { 0 },
      },
   }, {
      .name = "2: Old layout",
      .desc = {
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Jump" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Cycle Weapon" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Freelook" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Fire" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Strafe Left" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Strafe Right" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Look Up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Look Down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Swim down" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Swim up" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Toggle Run Mode" },
         { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
         { 0 },
      },
      .bind = {
         {"JOY_LEFT",  "+left"},         {"JOY_RIGHT", "+right"},
         {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
         {"JOY_B",     "+jump"} ,        {"JOY_A",     "impulse 10"},
         {"JOY_X",     "+klook"},        {"JOY_Y",     "+attack"},
         {"JOY_L",     "+moveleft"},     {"JOY_R",     "+moveright"},
         {"JOY_L2",    "+lookup"},       {"JOY_R2",    "+lookdown"},
         {"JOY_L3",    "+movedown"},     {"JOY_R3",    "+moveup"},
         {"JOY_SELECT","+togglewalk"},   {"JOY_START", "togglemenu"},
         { 0 },
      },
   },
   { 0,},
};

gp_layout_t *gp_layoutp = NULL;

void gp_layout_set_desc(gp_layout_t gp_layout) {
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, gp_layout.desc);
}

void gp_layout_set_bind(gp_layout_t gp_layout)
{
   char buf[100];
   unsigned i;
   for (i=0; gp_layout.bind[i].key; ++i)
   {
      snprintf(buf, sizeof(buf), "bind %s \"%s\"", gp_layout.bind[i].key,
                                                   gp_layout.bind[i].com);
      Cmd_ExecuteString(buf, src_command);
   }
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_variable variables[] = {
      { "tyrquake_colored_lighting", "Colored lighting (restart); disabled|enabled" },
      { "tyrquake_resolution",
         "Resolution (restart); 320x200|640x400|960x600|1280x800|1600x1000|1920x1200|320x240|320x480|360x200|360x240|360x400|360x480|400x224|480x272|512x224|512x240|512x384|512x512|640x224|640x240|640x448|640x480|720x576|800x480|800x600|960x720|1024x768|1280x720|1600x900|1920x1080" },
      { "tyrquake_retropad_layout", layouts },
      { NULL, NULL },
   };

   static const struct retro_controller_description port_1[] = {
      { "RetroPad", RETRO_DEVICE_JOYPAD },
      { "RetroKeyboard/Mouse", RETRO_DEVICE_KEYBOARD },
   };

   static const struct retro_controller_info ports[] = {
      { port_1, 2 },
      { 0 },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

extern void M_Game_StartNewGame(void);

void retro_reset(void)
{
   M_Game_StartNewGame();
}

void Sys_SendKeyEvents(void)
{
   int port;

   if (!poll_cb)
      return;

   poll_cb();

   if (!input_cb)
      return;

   for (port = 0; port < MAX_PADS; port++)
   {
      if (!input_cb)
         break;

      switch (quake_devices[port])
      {
         case RETRO_DEVICE_JOYPAD:
            {
               unsigned i;
               for (i=RETRO_DEVICE_ID_JOYPAD_B; i <= RETRO_DEVICE_ID_JOYPAD_R3; ++i)
               {
                   if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, i))
                      Key_Event(K_JOY_B + i, 1);
                   else
                      Key_Event(K_JOY_B + i, 0);
               }

               /* Disabled, need a way to switch between analog and digital
                *  movement for the sticks.
               int lsx=0, lsy=0, rsx=0, rsy=0;
               lsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                     RETRO_DEVICE_ID_ANALOG_X);
               lsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                     RETRO_DEVICE_ID_ANALOG_Y);
               rsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                     RETRO_DEVICE_ID_ANALOG_X);
               rsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                     RETRO_DEVICE_ID_ANALOG_Y);


               if (lsx > ANALOG_THRESHOLD)
                  Key_Event(K_AUX1, 1);
               else
                  Key_Event(K_AUX1, 0);
               if (lsx < -ANALOG_THRESHOLD)
                  Key_Event(K_AUX2, 1);
               else
                  Key_Event(K_AUX2, 0);
               if (lsy > ANALOG_THRESHOLD)
                  Key_Event(K_AUX3, 1);
               else
                  Key_Event(K_AUX3, 0);
               if (lsy < -ANALOG_THRESHOLD)
                  Key_Event(K_AUX4, 1);
               else
                  Key_Event(K_AUX4, 0);

               if (rsx > ANALOG_THRESHOLD)
                  Key_Event(K_AUX5, 1);
               else
                  Key_Event(K_AUX5, 0);
               if (rsx < -ANALOG_THRESHOLD)
                  Key_Event(K_AUX6, 1);
               else
                  Key_Event(K_AUX6, 0);
               if (rsy > ANALOG_THRESHOLD)
                  Key_Event(K_AUX7, 1);
               else
                  Key_Event(K_AUX7, 0);
               if (rsy < -ANALOG_THRESHOLD)
                  Key_Event(K_AUX8, 1);
               else
                  Key_Event(K_AUX8, 0);
               */
            }
            break;
         case RETRO_DEVICE_KEYBOARD:
            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT))
               Key_Event(K_MOUSE1, 1);
            else
               Key_Event(K_MOUSE1, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT))
               Key_Event(K_MOUSE2, 1);
            else
               Key_Event(K_MOUSE2, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE))
               Key_Event(K_MOUSE3, 1);
            else
               Key_Event(K_MOUSE3, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP))
               Key_Event(K_MOUSE4, 1);
            else
               Key_Event(K_MOUSE4, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN))
               Key_Event(K_MOUSE5, 1);
            else
               Key_Event(K_MOUSE5, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP))
               Key_Event(K_MOUSE6, 1);
            else
               Key_Event(K_MOUSE6, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN))
               Key_Event(K_MOUSE7, 1);
            else
               Key_Event(K_MOUSE7, 0);

            break;
      }
   }
}

static void keyboard_cb(bool down, unsigned keycode,
      uint32_t character, uint16_t mod)
{
	if (down)
		Key_Event((knum_t) keycode, 1);
	else
		Key_Event((knum_t) keycode, 0);
}

void Sys_Sleep(void)
{
   retro_sleep(1);
}

extern int coloredlights;

static void update_variables(bool startup)
{
   struct retro_variable var;

   var.key = "tyrquake_colored_lighting";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && startup)
   {
      if (!strcmp(var.value, "enabled"))
         coloredlights = 1;
      else
         coloredlights = 0;
   }
   else
      coloredlights = 0;
   
   var.key = "tyrquake_resolution";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && !initial_resolution_set)
   {
      char *pch;
      char str[100];
      snprintf(str, sizeof(str), "%s", var.value);

      pch = strtok(str, "x");
      if (pch)
         width = strtoul(pch, NULL, 0);
      pch = strtok(NULL, "x");
      if (pch)
         height = strtoul(pch, NULL, 0);

      if (log_cb)
         log_cb(RETRO_LOG_INFO, "Got size: %u x %u.\n", width, height);

      initial_resolution_set = true;
   }

   var.key = "tyrquake_retropad_layout";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      unsigned i;
      for (i=0; gp_layouts[i].name; ++i)
      {
         if (strcmp(var.value, gp_layouts[i].name) == 0)
         {
            gp_layoutp = gp_layouts + i;
            gp_layout_set_desc(*gp_layoutp);
         }
      }
   }
}

static void update_env_variables(void)
{
   const char *default_username = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_USERNAME, &default_username))
   {
      if (default_username && default_username[0] != '\0')
      {
         char setplayer[256];
         sprintf(setplayer, "name %s", default_username);
         retro_cheat_set(0, true, setplayer);
      }
   }
}

byte *vid_buffer;
short *zbuffer;
short *finalimage;
byte* surfcache;

static void audio_process(void);
static void audio_callback(void);

static bool did_flip;

bool shutdown_core = false;

void retro_run(void)
{
   static bool has_set_username = false;
   did_flip = false;
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);
   if (!has_set_username)
   {
      update_env_variables();
      has_set_username = true;
   }

   if (gp_layoutp != NULL)
   {
      gp_layout_set_bind(*gp_layoutp);
      gp_layoutp = NULL;
   }

   Host_Frame(0.016667);

   if (shutdown_core)
      return;

   if (!did_flip)
      video_cb(NULL, width, height, width << 1); /* dupe */
   audio_process();
   audio_callback();
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
    {
       buf[0] = '.';
       buf[1] = '\0';
    }
}

const char *argv[MAX_NUM_ARGVS];
static const char *empty_string = "";

void retro_set_rumble_strong(void)
{
   if (!rumble.set_rumble_state)
      return;

   uint16_t strength_strong = 0xffff;
   rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, strength_strong);
}

void retro_unset_rumble_strong(void)
{
   if (!rumble.set_rumble_state)
      return;

   rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, 0);
}

bool retro_load_game(const struct retro_game_info *info)
{
   unsigned i;
   char g_rom_dir[1024], g_pak_path[1024];
   char cfg_file[1024];
   char *path_lower;
   quakeparms_t parms;

   if (!info)
      return false;

   path_lower = strdup(info->path);

   for (i=0; path_lower[i]; ++i)
       path_lower[i] = tolower(path_lower[i]);

   struct retro_keyboard_callback cb = { keyboard_cb };
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cb);

   update_variables(true);

   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   snprintf(g_pak_path, sizeof(g_pak_path), "%s", info->path);

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
      log_cb(RETRO_LOG_INFO, "Rumble environment supported.\n");
   else
      log_cb(RETRO_LOG_INFO, "Rumble environment not supported.\n");

   MEMSIZE_MB = DEFAULT_MEMSIZE_MB;

   if ( strstr(path_lower, "id1") ||
        strstr(path_lower, "quoth") ||
        strstr(path_lower, "hipnotic") ||
        strstr(path_lower, "rogue") )
   {
#if (defined(HW_RVL) && !defined(WIIU)) || defined(_XBOX1)
      MEMSIZE_MB = 16;
#endif
      extract_directory(g_rom_dir, g_rom_dir, sizeof(g_rom_dir));
   }

   memset(&parms, 0, sizeof(parms));

   parms.argc = 1;
   parms.basedir = g_rom_dir;
   parms.memsize = MEMSIZE_MB * 1024 * 1024;
   argv[0] = empty_string;

   if (strstr(g_pak_path, "rogue"))
   {
      parms.argc++;
      argv[1] = "-rogue";
   }
   else if (strstr(g_pak_path, "hipnotic"))
   {
      parms.argc++;
      argv[1] = "-hipnotic";
   }
   else if (strstr(g_pak_path, "quoth"))
   {
      parms.argc++;
      argv[1] = "-quoth";
   }
   else if (!strstr(g_pak_path, "id1"))
   {
      char basename[1024];
      parms.argc++;
      argv[1] = "-game";
      parms.argc++;
      extract_basename(basename, g_rom_dir, sizeof(basename));
      argv[2] = basename;
      extract_directory(g_rom_dir, g_rom_dir, sizeof(g_rom_dir));
   }

   parms.argv = argv;

   COM_InitArgv(parms.argc, parms.argv);

   parms.argc = com_argc;
   parms.argv = com_argv;

   heap = (unsigned char*)malloc(parms.memsize);

   parms.membase = heap;

#ifdef NQ_HACK
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "Quake Libretro -- TyrQuake Version %s\n", stringify(TYR_VERSION));
#endif
#ifdef QW_HACK
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "QuakeWorld Libretro -- TyrQuake Version %s\n", stringify(TYR_VERSION));
#endif

   Sys_Init();

   if (!Host_Init(&parms))
   {
      Host_Shutdown();
      struct retro_message msg;
      char msg_local[256];

      snprintf(msg_local, sizeof(msg_local),
            "PAK archive loading failed...");
      msg.msg    = msg_local;
      msg.frames = 360;
      if (environ_cb)
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
      return false;
   }

   Cvar_Set("cl_bob", "0.02");
   Cvar_Set("crosshair", "0");
   Cvar_Set("viewsize", "100");
   Cvar_Set("showram", "0");
   Cvar_Set("dither_filter", "0");
   Cvar_Set("r_lerpmove", "1");
   Cvar_RegisterVariable(&framerate);
   Cvar_Set("framerate", "60");
   Cvar_Set("sys_ticrate", "0.016667");


   /* Override some default binds with more modern ones if we are booting the 
    * game for the first time. */
   snprintf(cfg_file, sizeof(cfg_file), "%s/config.cfg", g_rom_dir);

   if (path_is_valid(cfg_file))
   {
       Cvar_Set("gamma", "0.95");
       Cmd_ExecuteString("bind ' \"toggleconsole\"", src_command);
       Cmd_ExecuteString("bind ~ \"toggleconsole\"", src_command);
       Cmd_ExecuteString("bind ` \"toggleconsole\"", src_command);

       Cmd_ExecuteString("bind f \"+moveup\"", src_command);
       Cmd_ExecuteString("bind c \"+movedown\"", src_command);

       Cmd_ExecuteString("bind a \"+moveleft\"", src_command);
       Cmd_ExecuteString("bind d \"+moveright\"", src_command);
       Cmd_ExecuteString("bind w \"+forward\"", src_command);
       Cmd_ExecuteString("bind s \"+back\"", src_command);

       Cmd_ExecuteString("bind e \"impulse 10\"", src_command);
       Cmd_ExecuteString("bind q \"impulse 12\"", src_command);
   }

   Cmd_ExecuteString("bind AUX1 \"+moveright\"", src_command);
   Cmd_ExecuteString("bind AUX2 \"+moveleft\"", src_command);
   Cmd_ExecuteString("bind AUX3 \"+back\"", src_command);
   Cmd_ExecuteString("bind AUX4 \"+forward\"", src_command);
   Cmd_ExecuteString("bind AUX5 \"+right\"", src_command);
   Cmd_ExecuteString("bind AUX6 \"+left\"", src_command);
   Cmd_ExecuteString("bind AUX7 \"+lookup\"", src_command);
   Cmd_ExecuteString("bind AUX8 \"+lookdown\"", src_command);

   return true;
}



void retro_unload_game(void)
{
   Sys_Quit();
   if (heap)
      free(heap);
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;

   Cmd_ExecuteString(code, src_command);
}

/*
 * VIDEO
 */

unsigned short d_8to16table[256];

#define MAKECOLOR(r, g, b) (((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xf8) >> 3))


void VID_SetPalette(unsigned char *palette)
{
   unsigned i, j;
   unsigned short *pal = &d_8to16table[0];

   for(i = 0, j = 0; i < 256; i++, j += 3)
      *pal++ = MAKECOLOR(palette[j], palette[j+1], palette[j+2]);

}

unsigned 	d_8to24table[256];


void	VID_SetPalette2 (unsigned char *palette)
{
	byte	*pal;
	unsigned r,g,b;
	unsigned v;
	unsigned short i;
	unsigned	*table;

	pal = palette;
	table = d_8to24table;
	for (i=0 ; i<256 ; i++)
	{
		r = pal[0];
		g = pal[1];
		b = pal[2];
		if (r>255) r = 255;
		if (g>255) g = 255;
		if (b>255) b = 255;
		pal += 3;
		v = (255<<24) + (r<<0) + (g<<8) + (b<<16);
		*table++ = v;
		
	}

	
	d_8to24table[255] &= 0xffffff;	// 255 is transparent
	d_8to24table[0] &= 0x000000;	// black is black

}

void VID_ShiftPalette(unsigned char *palette)
{

   VID_SetPalette(palette);
}

void VID_Init(unsigned char *palette)
{
   /* TODO */
   vid_buffer = (byte*)malloc(width * height * sizeof(byte));
   zbuffer = (short*)malloc(width * height * sizeof(short));
   finalimage = (short*)malloc(width * height * sizeof(short));

    vid.width = width;
    vid.height = height;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.conwidth = vid.width;
    vid.conheight = vid.height;
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = vid_buffer;
    vid.rowbytes = width;
    vid.conrowbytes = vid.rowbytes;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

    d_pzbuffer = zbuffer;
    surfcache = malloc(SURFCACHE_SIZE);
    D_InitCaches(surfcache, SURFCACHE_SIZE);
}

void VID_Shutdown(void)
{
   if (vid_buffer)
      free(vid_buffer);
   if (zbuffer)
      free(zbuffer);
   if (finalimage)
      free(finalimage);
   if (surfcache)
      free(surfcache);
   vid_buffer = NULL;
   zbuffer    = NULL;
   finalimage = NULL;
   surfcache  = NULL;
}

void VID_Update(vrect_t *rects)
{
   unsigned y;
   struct retro_framebuffer fb = {0};
   unsigned pitch              = width << 1;
   uint8_t *ilineptr           = (uint8_t*)vid.buffer;
   uint16_t *olineptr          = (uint16_t*)finalimage;
   uint16_t *pal               = (uint16_t*)&d_8to16table;
   uint16_t *ptr               = (uint16_t*)finalimage;

   fb.width                    = width;
   fb.height                   = height;
   fb.access_flags             = RETRO_MEMORY_ACCESS_WRITE;

   if (!video_cb)
      return;

   if (environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER, &fb) 
         && fb.format == RETRO_PIXEL_FORMAT_RGB565)
   {
      ptr      = (uint16_t*)fb.data;
      olineptr = (uint16_t*)fb.data;
      pitch    = fb.pitch << 1;
   }

   if (!rects)
      return;

   for (y = 0; y < rects->width * rects->height; ++y)
      *olineptr++ = pal[*ilineptr++];

   if (video_cb)
      video_cb(ptr, width, height, pitch);
   did_flip = true;
}

qboolean VID_IsFullScreen(void)
{
    return true;
}

void VID_LockBuffer(void)
{
}

void VID_UnlockBuffer(void)
{
}

void D_BeginDirectRect(int x, int y, const byte *pbitmap, int width, int height)
{
}

void D_EndDirectRect(int x, int y, int width, int height)
{
}

/*
 * SOUND (TODO)
 */

#define AUDIO_BUFFER_SAMPLES (4096)

static int16_t audio_buffer[AUDIO_BUFFER_SAMPLES];
static unsigned audio_buffer_ptr;

static void audio_process(void)
{
   /* adds music raw samples and/or advances midi driver */
   BGM_Update(); 
   /* update audio */
   if (cls.state == ca_active)
   {
      S_Update(r_origin, vpn, vright, vup);
      CL_DecayLights();
   }
   else
      S_Update(vec3_origin, vec3_origin, vec3_origin, vec3_origin);

   CDAudio_Update();
}

static void audio_callback(void)
{
   float samples_per_frame = (2 * SAMPLERATE) / framerate.value;
   unsigned read_end = audio_buffer_ptr + samples_per_frame;
   if (read_end > AUDIO_BUFFER_SAMPLES)
      read_end = AUDIO_BUFFER_SAMPLES;

   unsigned read_first  = read_end - audio_buffer_ptr;
   unsigned read_second = samples_per_frame - read_first;

   audio_batch_cb(audio_buffer + audio_buffer_ptr, read_first / (shm->samplebits / 8));
   audio_buffer_ptr += read_first;
   if (read_second >= 1) {
      audio_batch_cb(audio_buffer, read_second / (shm->samplebits / 8));
      audio_buffer_ptr = read_second;
   }
}

qboolean SNDDMA_Init(dma_t *dma)
{
   shm = dma;
   shm->speed = SAMPLERATE;
   shm->channels = 2;
   shm->samplepos = 0;
   shm->samplebits = 16;
   shm->signed8 = 0;
   shm->samples = AUDIO_BUFFER_SAMPLES;
   shm->buffer = (unsigned char *volatile)audio_buffer;

   return true;
}

int SNDDMA_GetDMAPos(void)
{
   return shm->samplepos = audio_buffer_ptr;
}

int SNDDMA_LockBuffer(void)
{
   return 0;
}

void SNDDMA_UnlockBuffer(void)
{
}

void SNDDMA_Shutdown(void)
{
}

void SNDDMA_Submit(void)
{
}

/*
 * CD (TODO)
 */

int CDDrv_IsAudioTrack(byte track) { return 0; }
int CDDrv_PlayTrack(byte track) { return 1; }
int CDDrv_IsPlaying(byte track) { return 0; }
int CDDrv_InitDevice(void) { return -1; }
void CDDrv_CloseDevice(void) { }
void CDDrv_Eject(void) { }
void CDDrv_CloseDoor(void) { }
void CDDrv_Stop(void) { }
void CDDrv_Pause(void) { }
void CDDrv_Resume(byte track) { }
int CDDrv_GetMaxTrack(byte *track) { return 0; }
int CDDrv_SetVolume(byte volume) { return -1; }

/*
 * INPUT (TODO)
 */
static void windowed_mouse_f(struct cvar_s *var)
{
}

cvar_t _windowed_mouse = { "_windowed_mouse", "0", true, false, 0, windowed_mouse_f };

void
IN_Init(void)
{
   int i;
   for (i = 0; i < MAX_PADS; i++)
      quake_devices[i] = RETRO_DEVICE_JOYPAD;
}

void
IN_Shutdown(void)
{
}

void
IN_Commands(void)
{
}

void
IN_Move(usercmd_t *cmd)
{
   static int cur_mx;
   static int cur_my;
   int mx, my, lsx, lsy, rsx, rsy;

   if (quake_devices[0] == RETRO_DEVICE_KEYBOARD) {
      mx = input_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
      my = input_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

      if (mx != cur_mx || my != cur_my)
      {

         mx *= sensitivity.value;
         my *= sensitivity.value;

         cl.viewangles[YAW] -= m_yaw.value * mx;

         V_StopPitchDrift();

         cl.viewangles[PITCH] += m_pitch.value * my;

         if (cl.viewangles[PITCH] > 80)
            cl.viewangles[PITCH] = 80;
         if (cl.viewangles[PITCH] < -70)
            cl.viewangles[PITCH] = -70;
         cur_mx = mx;
         cur_my = my;
      }
   } else if (quake_devices[0] == RETRO_DEVICE_JOYPAD) {
      // Left stick move
      lsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      lsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);

      if (lsx > ANALOG_DEADZONE || lsx < -ANALOG_DEADZONE) {
         if (lsx > ANALOG_DEADZONE)
            lsx = lsx - ANALOG_DEADZONE;
         if (lsx < -ANALOG_DEADZONE)
            lsx = lsx + ANALOG_DEADZONE;
         cmd->sidemove += cl_sidespeed.value * lsx / ANALOG_RANGE;
      }

      if (lsy > ANALOG_DEADZONE || lsy < -ANALOG_DEADZONE) {
         if (lsy > ANALOG_DEADZONE)
            lsy = lsy - ANALOG_DEADZONE;
         if (lsy < -ANALOG_DEADZONE)
            lsy = lsy + ANALOG_DEADZONE;
         cmd->forwardmove -= cl_forwardspeed.value * lsy / ANALOG_RANGE;
      }

      // Right stick Look
      rsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      rsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);

      if (rsx > ANALOG_DEADZONE || rsx < -ANALOG_DEADZONE) {
         if (rsx > ANALOG_DEADZONE)
            rsx = rsx - ANALOG_DEADZONE;
         if (rsx < -ANALOG_DEADZONE)
            rsx = rsx + ANALOG_DEADZONE;
         // For now we are sharing the sensitivity with the mouse setting
         cl.viewangles[YAW] -= rsx * sensitivity.value / ANALOG_RANGE;
      }

      V_StopPitchDrift();
      if (rsy > ANALOG_DEADZONE || rsy < -ANALOG_DEADZONE) {
         if (rsy > ANALOG_DEADZONE)
            rsy = rsy - ANALOG_DEADZONE;
         if (rsy < -ANALOG_DEADZONE)
            rsy = rsy + ANALOG_DEADZONE;
         cl.viewangles[PITCH] -= rsy * sensitivity.value / ANALOG_RANGE;
      }

      if (cl.viewangles[PITCH] > 80)
         cl.viewangles[PITCH] = 80;
      if (cl.viewangles[PITCH] < -70)
         cl.viewangles[PITCH] = -70;
   }
}

/*
===========
IN_ModeChanged
===========
*/
void
IN_ModeChanged(void)
{
}
