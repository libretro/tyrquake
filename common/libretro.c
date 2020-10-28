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
#include "libretro_core_options.h"
#include <retro_miscellaneous.h>
#include <retro_timers.h>
#include <file/file_path.h>

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

#ifdef __PSL1GHT__
#include <lv2/systime.h>
#elif defined (__CELLOS_LV2__)
#include <sys/sys_time.h>
#include <sys/timer.h>
#endif

#define SURFCACHE_SIZE 10485760

#define RETRO_DEVICE_MODERN  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2)
#define RETRO_DEVICE_JOYPAD_ALT  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)

extern void CDAudio_Update(void);

unsigned width       = 320;
unsigned height      = 200;
unsigned device_type = 0;

unsigned MEMSIZE_MB;

static struct retro_rumble_interface rumble;
static bool libretro_supports_bitmasks = false;

#if defined(HW_DOL)
#define DEFAULT_MEMSIZE_MB 8
#elif defined(WIIU)
#define DEFAULT_MEMSIZE_MB 32
#elif defined(HW_RVL) || defined(_XBOX1) 
#define DEFAULT_MEMSIZE_MB 24
#else
#define DEFAULT_MEMSIZE_MB 32
#endif

#define DEFAULT_SAMPLERATE 48000
static uint16_t samplerate = DEFAULT_SAMPLERATE;

#define AUDIO_BUFFER_SIZE 4096
static int16_t audio_buffer[AUDIO_BUFFER_SIZE];
static unsigned audio_buffer_ptr;

// System analog stick range is -0x8000 to 0x8000
#define ANALOG_RANGE 0x8000
// Default deadzone: 15%
static int analog_deadzone = (int)(0.15f * ANALOG_RANGE);

#define GP_MAXBINDS 32

typedef struct {
   struct retro_input_descriptor desc[GP_MAXBINDS];
   struct {
      char *key;
      char *com;
   } bind[GP_MAXBINDS];
} gp_layout_t;

gp_layout_t modern = {
   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Swim Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Strafe Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Strafe Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Swim Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Previous Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Next Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Jump" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Fire" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Show Scores" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
      { 0 },
   },
   {
      {"JOY_LEFT",  "+moveleft"},     {"JOY_RIGHT", "+moveright"},
      {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
      {"JOY_B",     "+movedown"},     {"JOY_A",     "+moveright"},
      {"JOY_X",     "+moveup"},       {"JOY_Y",     "+moveleft"},
      {"JOY_L",     "impulse 12"},    {"JOY_R",     "impulse 10"},
      {"JOY_L2",    "+jump"},         {"JOY_R2",    "+attack"},
      {"JOY_SELECT","+showscores"},   {"JOY_START", "togglemenu"},
      { 0 },
   },
};

gp_layout_t classic = {
   {
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
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Move Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Swim Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Toggle Run Mode" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
      { 0 },
   },
   {
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
};

gp_layout_t classic_alt = {

   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Look Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Look Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Look Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Look Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Jump" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Fire" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "Run" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "Next Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "Move Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "Previous Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,"Toggle Run Mode" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Menu" },
      { 0 },
   },
   {
      {"JOY_LEFT",  "+moveleft"},     {"JOY_RIGHT", "+moveright"},
      {"JOY_DOWN",  "+back"},         {"JOY_UP",    "+forward"},
      {"JOY_B",     "+lookdown"},     {"JOY_A",     "+right"},
      {"JOY_X",     "+lookup"},       {"JOY_Y",     "+left"},
      {"JOY_L",     "+jump"},         {"JOY_R",     "+attack"},
      {"JOY_L2",    "+speed"},          {"JOY_R2",    "impulse 10"},
      {"JOY_L3",    "+movedown"},     {"JOY_R3",    "impulse 12"},
      {"JOY_SELECT","+togglewalk"},   {"JOY_START", "togglemenu"},
      { 0 },
   },
};

gp_layout_t *gp_layoutp = NULL;

static float framerate = 60.0f;
static bool initial_resolution_set = false;
static int invert_y_axis = 1;

unsigned char *heap;

#define MAX_PADS 1
static unsigned quake_devices[1];

static void extract_basename(char *buf, const char *path, size_t size)
{
   char *ext        = NULL;
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   ext = strrchr(buf, '.');
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
   path_mkdir(path);
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
#elif defined(__PSL1GHT__)
   newtime = sysGetSystemTime() / 1000000.0;
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

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   Sys_Init();
}

void retro_deinit(void)
{
   Sys_Quit();
   if (heap)
      free(heap);

   libretro_supports_bitmasks = false;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
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


void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (port == 0)
   {
      switch (device)
      {
         case RETRO_DEVICE_JOYPAD:
            quake_devices[port] = RETRO_DEVICE_JOYPAD;
            environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, classic.desc);
            gp_layout_set_bind(classic);
            break;
         case RETRO_DEVICE_JOYPAD_ALT:
            quake_devices[port] = RETRO_DEVICE_JOYPAD;
            environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, classic_alt.desc);
            gp_layout_set_bind(classic_alt);
            break;
         case RETRO_DEVICE_MODERN:
            quake_devices[port] = RETRO_DEVICE_MODERN;
            environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, modern.desc);
            gp_layout_set_bind(modern);
            break;
         case RETRO_DEVICE_KEYBOARD:
            quake_devices[port] = RETRO_DEVICE_KEYBOARD;
            break;
         case RETRO_DEVICE_NONE:
         default:
            quake_devices[port] = RETRO_DEVICE_NONE;
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "[libretro]: Invalid device.\n");
      }
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
   info->timing.fps            = framerate;
   info->timing.sample_rate    = samplerate;

   info->geometry.base_width   = width;
   info->geometry.base_height  = height;
   info->geometry.max_width    = width;
   info->geometry.max_height   = height;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_controller_description port_1[] = {
      { "Gamepad Classic", RETRO_DEVICE_JOYPAD },
      { "Gamepad Classic Alt", RETRO_DEVICE_JOYPAD_ALT },
      { "Gamepad Modern", RETRO_DEVICE_MODERN },
      { "Keyboard + Mouse", RETRO_DEVICE_KEYBOARD },
   };

   static const struct retro_controller_info ports[] = {
      { port_1, 4 },
      { 0 },
   };

   environ_cb = cb;

   libretro_set_core_options(environ_cb);
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
         case RETRO_DEVICE_JOYPAD_ALT:
         case RETRO_DEVICE_MODERN:
            {
               unsigned i;
               int16_t ret    = 0;
               if (libretro_supports_bitmasks)
                  ret = input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
               else
               {
                  for (i=RETRO_DEVICE_ID_JOYPAD_B; i <= RETRO_DEVICE_ID_JOYPAD_R3; ++i)
                  {
                     if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, i))
                        ret |= (1 << i);
                  }
               }

               for (i=RETRO_DEVICE_ID_JOYPAD_B; 
                     i <= RETRO_DEVICE_ID_JOYPAD_R3; ++i)
               {
                  if (ret & (1 << i))
                     Key_Event(K_JOY_B + i, 1);
                  else
                     Key_Event(K_JOY_B + i, 0);
               }
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

            if (quake_devices[0] == RETRO_DEVICE_KEYBOARD) {
               if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_UP))
                  Key_Event(K_UPARROW, 1);
               else
                  Key_Event(K_UPARROW, 0);

               if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_DOWN))
                  Key_Event(K_DOWNARROW, 1);
               else
                  Key_Event(K_DOWNARROW, 0);

               if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_LEFT))
                  Key_Event(K_LEFTARROW, 1);
               else
                  Key_Event(K_LEFTARROW, 0);

               if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_RIGHT))
                  Key_Event(K_RIGHTARROW, 1);
               else
                  Key_Event(K_RIGHTARROW, 0);
               }
            break;
         case RETRO_DEVICE_NONE:
            break;
      }
   }
}

static void keyboard_cb(bool down, unsigned keycode,
      uint32_t character, uint16_t mod)
{
  // character-only events are discarded
  if (keycode != RETROK_UNKNOWN) {
      if (down)
         Key_Event((knum_t) keycode, 1);
      else
         Key_Event((knum_t) keycode, 0);
   }
}

void Sys_Sleep(void)
{
   retro_sleep(1);
}

const char *argv[MAX_NUM_ARGVS];
static const char *empty_string = "";

void retro_set_rumble_strong(void)
{
   uint16_t strength_strong = 0xffff;
   if (!rumble.set_rumble_state)
      return;

   rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, strength_strong);
}

void retro_unset_rumble_strong(void)
{
   if (!rumble.set_rumble_state)
      return;

   rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, 0);
}

extern int coloredlights;

bool state_rumble;

static void update_variables(bool startup)
{
   struct retro_variable var;

   var.key = "tyrquake_framerate";
   var.value = NULL;

   if (startup)
   {
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      {
         if (!strcmp(var.value, "auto"))
         {
            float target_framerate = 0.0f;

            if (!environ_cb(RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE,
                  &target_framerate))
               target_framerate = 60.0f;

            framerate = target_framerate;
         }
         else
            framerate = atof(var.value);
      }
      else
         framerate = 60.0f;

      /* Note: The audio handling code of the game engine
       * completely falls apart below 50 FPS. To go any
       * lower than this, we have to manipulate the actual
       * audio sample rate to achieve a fixed 'samples per
       * frame' matching the default frame rate of 60. This
       * means we get progressively lower quality audio as
       * the frame rate decreases, but the alternative is
       * no sound at all... */
      if (framerate > 49.0f)
         samplerate = DEFAULT_SAMPLERATE;
      else
      {
         samplerate = (uint16_t)((float)DEFAULT_SAMPLERATE * (framerate / 60.0f));
         /* Round up to the nearest power of 2 */
         samplerate = (samplerate + 0x1) & ~0x1;
      }
   }

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

   var.key = "tyrquake_rumble";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         state_rumble = false;
      else
         state_rumble = true;
   }

   var.key = "tyrquake_invert_y_axis";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "disabled") == 0)
         invert_y_axis = 1;
      else
         invert_y_axis = -1;
   }
   
   var.key = "tyrquake_analog_deadzone";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
		analog_deadzone = (int)(atoi(var.value) * 0.01f * ANALOG_RANGE);
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
   bool updated = false;

   did_flip = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);
   if (!has_set_username)
   {
      update_env_variables();
      has_set_username = true;
   }
   
   if (!state_rumble)
      retro_unset_rumble_strong();

   Host_Frame(1.0 / framerate);

   if (shutdown_core)
      return;

   if (!did_flip)
      video_cb(NULL, width, height, width << 1); /* dupe */
   audio_process();
   audio_callback();
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base = NULL;

   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
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

bool retro_load_game(const struct retro_game_info *info)
{
   unsigned i;
   char g_rom_dir[1024], g_pak_path[1024], g_save_dir[1024];
   char cfg_file[1024];
   char *path_lower;
   quakeparms_t parms;
#if defined(_WIN32)
   char slash = '\\';
#else
   char slash = '/';
#endif
   bool use_external_savedir = false;
   const char *base_save_dir = NULL;
   struct retro_keyboard_callback cb = { keyboard_cb };

   if (!info)
      return false;

   path_lower = strdup(info->path);

   for (i=0; path_lower[i]; ++i)
       path_lower[i] = tolower(path_lower[i]);

   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cb);

   update_variables(true);

   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   snprintf(g_pak_path, sizeof(g_pak_path), "%s", info->path);
   
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base_save_dir) && base_save_dir)
   {
		if (strlen(base_save_dir) > 0)
		{
			// Get game 'name' (i.e. subdirectory)
			char game_name[1024];
			extract_basename(game_name, g_rom_dir, sizeof(game_name));
			
			// > Build final save path
			snprintf(g_save_dir, sizeof(g_save_dir), "%s%c%s", base_save_dir, slash, game_name);
			use_external_savedir = true;
			
			// > Create save directory, if required
			if (!path_is_directory(g_save_dir))
			{
				use_external_savedir = path_mkdir(g_save_dir);
			}
		}
   }
   // > Error check
   if (!use_external_savedir)
   {
		// > Use ROM directory fallback...
		snprintf(g_save_dir, sizeof(g_save_dir), "%s", g_rom_dir);
	}
	else
	{
		// > Final check: is the save directory the same as the 'rom' directory?
		//   (i.e. ensure logical behaviour if user has set a bizarre save path...)
		use_external_savedir = (strcmp(g_save_dir, g_rom_dir) != 0);
	}

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
      char tmp_dir[1024];
      tmp_dir[0] = '\0';

#if (defined(HW_RVL) && !defined(WIIU)) || defined(_XBOX1)
      MEMSIZE_MB = 16;
#endif
      extract_directory(tmp_dir, g_rom_dir, sizeof(tmp_dir));
      strncpy(g_rom_dir, tmp_dir, sizeof(g_rom_dir) - 1);
   }

   memset(&parms, 0, sizeof(parms));

   parms.argc = 1;
   parms.basedir = g_rom_dir;
   parms.savedir = g_save_dir;
   parms.use_exernal_savedir = use_external_savedir ? 1 : 0;
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

   if (!Host_Init(&parms))
   {
      struct retro_message msg;
      char msg_local[256];

      Host_Shutdown();

      snprintf(msg_local, sizeof(msg_local),
            "PAK archive loading failed...");
      msg.msg    = msg_local;
      msg.frames = 360;
      if (environ_cb)
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
      return false;
   }

   /* Override some default binds with more modern ones if we are booting the 
    * game for the first time. */
   snprintf(cfg_file, sizeof(cfg_file), "%s%cconfig.cfg", g_save_dir, slash);

   if (!path_is_valid(cfg_file))
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

   if (code)
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
	unsigned r,g,b;
	unsigned v;
	unsigned short i;
	byte *pal       = palette;
	unsigned *table = d_8to24table;

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
   unsigned x, y;
   unsigned pitch              = width;
   uint8_t *ilineptr           = (uint8_t*)vid.buffer;
   uint16_t *olineptr          = (uint16_t*)finalimage;
   uint16_t *pal               = (uint16_t*)&d_8to16table;
   uint16_t *ptr               = (uint16_t*)finalimage;

   if (!video_cb || !rects || did_flip)
      return;

   for (y = 0; y < rects->height; ++y)
   {
      for (x = 0; x < rects->width; ++x)
         *olineptr++ = pal[*ilineptr++];
      olineptr += pitch - rects->width;
   }

   video_cb(ptr, width, height, pitch << 1);
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

static void
audio_batch_cb_blocking(int16_t * sa, size_t sz)
{
   while (sz)
   {
      size_t r = audio_batch_cb(sa, sz);
      sz -= r;
      sa += r;
   }
}

static void audio_callback(void)
{
   unsigned read_first, read_second;
   const int nchans = 2;
   int samples_per_frame = (nchans * samplerate) / framerate;
   unsigned read_end = audio_buffer_ptr + samples_per_frame;

   if (read_end > AUDIO_BUFFER_SIZE)
      read_end = AUDIO_BUFFER_SIZE;

   read_first  = read_end - audio_buffer_ptr;
   read_second = samples_per_frame - read_first;

   audio_batch_cb_blocking(audio_buffer + audio_buffer_ptr, read_first / nchans);
   audio_buffer_ptr += read_first;
   if (read_second >= 1)
   {
      audio_batch_cb_blocking(audio_buffer, read_second / nchans);
      audio_buffer_ptr = read_second;
   }
}

qboolean SNDDMA_Init(dma_t *dma)
{
   shm = dma;
   shm->speed = samplerate;
   shm->channels = 2;
   shm->samplepos = 0;
   shm->samplebits = 16;
   shm->signed8 = 0;
   shm->samples = AUDIO_BUFFER_SIZE;
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
   } else if (quake_devices[0] != RETRO_DEVICE_NONE && quake_devices[0] != RETRO_DEVICE_KEYBOARD) {
      // Left stick move
      lsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      lsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);

      if (lsx > analog_deadzone || lsx < -analog_deadzone) {
         if (lsx > analog_deadzone)
            lsx = lsx - analog_deadzone;
         if (lsx < -analog_deadzone)
            lsx = lsx + analog_deadzone;
         cmd->sidemove += cl_sidespeed.value * lsx / (ANALOG_RANGE - analog_deadzone);
      }

      if (lsy > analog_deadzone || lsy < -analog_deadzone) {
         if (lsy > analog_deadzone)
            lsy = lsy - analog_deadzone;
         if (lsy < -analog_deadzone)
            lsy = lsy + analog_deadzone;
         cmd->forwardmove -= cl_forwardspeed.value * lsy / (ANALOG_RANGE - analog_deadzone);
      }

      // Right stick Look
      rsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      rsy = invert_y_axis * input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);

      if (rsx > analog_deadzone || rsx < -analog_deadzone)
      {
         if (rsx > analog_deadzone)
            rsx = rsx - analog_deadzone;
         if (rsx < -analog_deadzone)
            rsx = rsx + analog_deadzone;
         // For now we are sharing the sensitivity with the mouse setting
         cl.viewangles[YAW] -= (float)(sensitivity.value * rsx / (ANALOG_RANGE - analog_deadzone)) / (framerate / 60.0f);
      }

      V_StopPitchDrift();
      if (rsy > analog_deadzone || rsy < -analog_deadzone)
      {
         if (rsy > analog_deadzone)
            rsy = rsy - analog_deadzone;
         if (rsy < -analog_deadzone)
            rsy = rsy + analog_deadzone;
         cl.viewangles[PITCH] -= (float)(sensitivity.value * rsy / (ANALOG_RANGE - analog_deadzone)) / (framerate / 60.0f);
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
