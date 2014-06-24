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

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#elif defined(_WIN32) && defined(_XBOX)
#include <xtl.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif

#include "common.h"
#include "quakedef.h"
#include "d_local.h"
#include "sys.h"

#include "sound.h"
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

extern void CDAudio_Update(void);

unsigned width;
unsigned height;
unsigned device_type = 0;

static bool use_audio_cb;

unsigned MEMSIZE_MB;

#if defined(HW_DOL)
#define DEFAULT_MEMSIZE_MB 8
#elif defined(HW_RVL) || defined(_XBOX1)
#define DEFAULT_MEMSIZE_MB 24
#else
#define DEFAULT_MEMSIZE_MB 32
#endif

#define SAMPLERATE 44100
#define ANALOG_THRESHOLD 4096

cvar_t framerate = { "framerate", "60" };
static bool initial_resolution_set;

char g_rom_dir[256];
char g_pak_path[256];
unsigned short	palette_data[256];

unsigned char *heap;

#define MAX_PADS 1
static unsigned quake_devices[1];

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
}

void Sys_Error(const char *error, ...)
{
   char buffer[256];
   va_list ap;
   va_start(ap, error);
   vsprintf(buffer, error, ap);
   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "%s\n", buffer);
   va_end(ap);

   Host_Shutdown();
   exit(1);
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
#ifdef _WIN32
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
#if defined(GEKKO)
   return ticks_to_microsecs(gettime()) / 1000000.0;
#elif defined(__CELLOS_LV2__)
   return sys_time_get_system_time() / 1000000.0;
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
   return (pcount - startcount) / pfreq;
#else
   struct timeval tp;
   struct timezone tzp;
   static int secbase;

   gettimeofday(&tp, &tzp);

   if (!secbase) {
      secbase = tp.tv_sec;
      return tp.tv_usec / 1000000.0;
   }

   return (tp.tv_sec - secbase) + tp.tv_usec / 1000000.0;
#endif
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

static double _time;
static double oldtime;
static double newtime;


viddef_t vid;			// global video state

void retro_init(void)
{
   int i;
   struct retro_log_callback log;

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   for (i = 0; i < MAX_PADS; i++)
      quake_devices[i] = RETRO_DEVICE_JOYPAD;

   initial_resolution_set = true;
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
   info->library_version  = "v" stringify(TYR_VERSION);
   info->need_fullpath    = false;
   info->valid_extensions = "pak";
}

#define MAX_WIDTH 1024
#define MAX_HEIGHT 768

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = framerate.value;
   info->timing.sample_rate = SAMPLERATE;

   info->geometry.base_width   = width;
   info->geometry.base_height  = height;
   info->geometry.max_width    = MAX_WIDTH;
   info->geometry.max_height   = MAX_HEIGHT;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}


void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_variable variables[] = {
      { "resolution",
         "Resolution (restart); 320x200|320x240|320x480|360x200|360x240|360x400|360x480|400x224|480x272|512x224|512x240|512x384|512x512|640x224|640x240|640x448|640x480|720x576|800x480|800x600|960x720|1024x768" },
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

void retro_reset(void)
{
}

void Sys_SendKeyEvents(void)
{
   int port;
   poll_cb();

   for (port = 0; port < MAX_PADS; port++)
   {
      switch (quake_devices[port])
      {
         case RETRO_DEVICE_JOYPAD:
            {
               int lsx, lsy, rsx, rsy;
               lsx = 0;
               lsy = 0;
               rsx = 0;
               rsy = 0;

               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
                  Key_Event(K_UPARROW, 1);
               else
                  Key_Event(K_UPARROW, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
                  Key_Event(K_DOWNARROW, 1);
               else
                  Key_Event(K_DOWNARROW, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L))
                  Key_Event(K_COMMA, 1);
               else
                  Key_Event(K_COMMA, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R))
                  Key_Event(K_PERIOD, 1);
               else
                  Key_Event(K_PERIOD, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
                  Key_Event(K_ENTER, 1);
               else
                  Key_Event(K_ENTER, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
                  Key_Event(K_SLASH, 1);
               else
                  Key_Event(K_SLASH, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y))
                  Key_Event(K_MOUSE1, 1);
               else
                  Key_Event(K_MOUSE1, 0);
               Key_Event(K_ESCAPE, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START));
               Key_Event(K_INS, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X));
               if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
                  Key_Event(K_LEFTARROW, 1);
               else
                  Key_Event(K_LEFTARROW, 0);
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
                  Key_Event(K_RIGHTARROW, 1);
               else
                  Key_Event(K_RIGHTARROW, 0);

               Key_Event(K_END, input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3));
               if (input_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
               {
                  Cvar_SetValue("cl_forwardspeed", 200);
                  Cvar_SetValue("cl_backspeed", 200);
               }
               else
               {
                  Cvar_SetValue("cl_forwardspeed", 400);
                  Cvar_SetValue("cl_backspeed", 400);
               }
               lsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                     RETRO_DEVICE_ID_ANALOG_X);
               lsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                     RETRO_DEVICE_ID_ANALOG_Y);
               rsx = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                     RETRO_DEVICE_ID_ANALOG_X);
               rsy = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                     RETRO_DEVICE_ID_ANALOG_Y);

               if (lsx > ANALOG_THRESHOLD)
                  Key_Event(K_PERIOD, 1);

               if (lsx < -ANALOG_THRESHOLD)
                  Key_Event(K_COMMA, 1);

               if (lsy > ANALOG_THRESHOLD)
                  Key_Event(K_DOWNARROW, 1);

               if (lsy < -ANALOG_THRESHOLD)
                  Key_Event(K_UPARROW, 1);

               if (rsx > ANALOG_THRESHOLD)
                  Key_Event(K_RIGHTARROW, 1);

               if (rsx < -ANALOG_THRESHOLD)
                  Key_Event(K_LEFTARROW, 1);

               Key_Event(K_DEL, rsy > ANALOG_THRESHOLD);
               Key_Event(K_PGDN, rsy < -ANALOG_THRESHOLD);
            }
            break;
         case RETRO_DEVICE_KEYBOARD:
            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_w))
               Key_Event(K_UPARROW, 1);
            else if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_UP))
               Key_Event(K_UPARROW, 1);
            else
               Key_Event(K_UPARROW, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_s))
               Key_Event(K_DOWNARROW, 1);
            else if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_DOWN))
               Key_Event(K_DOWNARROW, 1);
            else
               Key_Event(K_DOWNARROW, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_a))
               Key_Event(K_COMMA, 1);
            else
               Key_Event(K_COMMA, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_d))
               Key_Event(K_PERIOD, 1);
            else
               Key_Event(K_PERIOD, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_SPACE) ||
                  input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_RETURN) ||
                  input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT)
               )
               Key_Event(K_ENTER, 1);
            else
               Key_Event(K_ENTER, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_1))
               Key_Event(K_1, 1);
            else
               Key_Event(K_1, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_2))
               Key_Event(K_2, 1);
            else
               Key_Event(K_2, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_3))
               Key_Event(K_3, 1);
            else
               Key_Event(K_3, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_4))
               Key_Event(K_4, 1);
            else
               Key_Event(K_4, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_5))
               Key_Event(K_5, 1);
            else
               Key_Event(K_5, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_6))
               Key_Event(K_6, 1);
            else
               Key_Event(K_6, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_7))
               Key_Event(K_7, 1);
            else
               Key_Event(K_7, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_8))
               Key_Event(K_8, 1);
            else
               Key_Event(K_8, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_TAB))
               Key_Event(K_TAB, 1);
            else
               Key_Event(K_TAB, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_c))
               Key_Event(K_SLASH, 1);
            else
               Key_Event(K_SLASH, 0);

            if (input_cb(port, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) ||
                  (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_LCTRL)))
               Key_Event(K_MOUSE1, 1);
            else
               Key_Event(K_MOUSE1, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_LEFT))
               Key_Event(K_LEFTARROW, 1);
            else
               Key_Event(K_LEFTARROW, 0);

            if (input_cb(port, RETRO_DEVICE_KEYBOARD, 0, RETROK_RIGHT))
               Key_Event(K_RIGHTARROW, 1);
            else
               Key_Event(K_RIGHTARROW, 0);
            break;
      }
   }
}


void Sys_Sleep(void)
{
#if defined(_WIN32)
   Sleep(1 * 1000);
#elif defined(__CELLOS_LV2__)
   sys_timer_usleep(1);
#else
   usleep(1);
#endif
}

static void update_variables(void)
{
   struct retro_variable var;
   
   var.key = "resolution";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && initial_resolution_set)
   {
      char *pch;
      char str[100];
      snprintf(str, sizeof(str), "%s", var.value);

      pch = strtok(str, "x");
      if (pch)
         width = strtoul(pch, NULL, 0);
      pch = strtok(str, "x");
      if (pch)
         height = strtoul(pch, NULL, 0);

      if (log_cb)
         log_cb(RETRO_LOG_INFO, "Got size: %u x %u.\n", width, height);

      initial_resolution_set = false;
   }
}

byte *vid_buffer;
short *zbuffer;
short *finalimage;
byte surfcache[256 * 1024];

#define AUDIO_BUFFER_SAMPLES (8 * 1024)
static int16_t audio_buffer[AUDIO_BUFFER_SAMPLES];
static unsigned audio_buffer_ptr;

static void audio_process(void)
{
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
   //audio_process();
  
   float samples_per_frame = (2 * SAMPLERATE) / framerate.value;
   unsigned read_end = audio_buffer_ptr + samples_per_frame;
   if (read_end > AUDIO_BUFFER_SAMPLES)
      read_end = AUDIO_BUFFER_SAMPLES;

   unsigned read_first  = read_end - audio_buffer_ptr;
   unsigned read_second = samples_per_frame - read_first;

   audio_batch_cb(audio_buffer + audio_buffer_ptr, read_first >> 1);
   audio_buffer_ptr = (audio_buffer_ptr + read_first) & (AUDIO_BUFFER_SAMPLES - 1);
   audio_batch_cb(audio_buffer + audio_buffer_ptr, read_second >> 1);
}

void retro_run(void)
{

   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();

   // find time spent rendering last frame
   newtime = Sys_DoubleTime();
   _time = newtime - oldtime;

#ifdef NQ_HACK
   if (cls.state == ca_dedicated) {
      if (_time < sys_ticrate.value) {
         //Sys_Sleep();
         //TODO - do something proper for this instead of just 'returning'
         //continue;	// not time to run a server only tic yet
         return;
      }
      _time = sys_ticrate.value;
   }
   if (_time > sys_ticrate.value * 2)
      oldtime = newtime;
   else
   {
      oldtime += _time;
   }
#endif
#ifdef QW_HACK
   oldtime = newtime;
#endif

   Host_Frame(_time);

   video_cb(finalimage, width, height, width << 1);
#if 1
   audio_process();
#endif
   if (!use_audio_cb)
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
      buf[0] = '\0';
}

static void audio_set_state(bool enable)
{
   (void)enable;
}


bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_audio_callback cb = { audio_callback, audio_set_state };
   quakeparms_t parms;

   update_variables();

   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   snprintf(g_pak_path, sizeof(g_pak_path), "%s", info->path);

   MEMSIZE_MB = DEFAULT_MEMSIZE_MB;

   if (strstr(info->path, "hipnotic") || strstr(info->path, "rogue")
         || strstr(info->path, "HIPNOTIC")
         || strstr(info->path, "ROGUE"))
   {
#if defined(HW_RVL) || defined(_XBOX1)
      MEMSIZE_MB = 16;
#endif
      extract_directory(g_rom_dir, g_rom_dir, sizeof(g_rom_dir));
   }

   memset(&parms, 0, sizeof(parms));

   COM_InitArgv(0, NULL);

   parms.argc = com_argc;
   parms.argv = com_argv;
   parms.basedir = g_rom_dir;
   parms.memsize = MEMSIZE_MB * 1024 * 1024;

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
   Host_Init(&parms);

   Cvar_Set("cl_bob", "0.02");
   Cvar_Set("crosshair", "0");
   Cvar_Set("viewsize", "100");
   Cvar_Set("showram", "0");
   Cvar_Set("dither_filter", "1");
   Cvar_RegisterVariable(&framerate);
   Cvar_Set("framerate", "60");

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "RGB565 is not supported.\n");
      return false;
   }

#ifdef NQ_HACK
   oldtime = Sys_DoubleTime() - 0.1;
#endif
#ifdef QW_HACK
   oldtime = Sys_DoubleTime();
#endif

   use_audio_cb = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &cb);

   return true;
}

void retro_unload_game(void)
{
   Sys_Quit();
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
   (void)code;
}

/*
 * VIDEO (TODO)
 */



unsigned short d_8to16table[256];


void VID_SetPalette(unsigned char *palette)
{
}

void VID_ShiftPalette(unsigned char *palette)
{
   unsigned char *ilineptr = (unsigned char*)vid.buffer;
   unsigned short *olineptr = (unsigned short*)finalimage;
   unsigned y;

   for (y = 0; y < width * height; ++y)
      *olineptr++ = palette_data[*ilineptr++];
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
    D_InitCaches(surfcache, sizeof(surfcache));
}

void VID_Shutdown(void)
{
   if (vid_buffer)
      free(vid_buffer);
   if (zbuffer)
      free(zbuffer);
   if (finalimage)
      free(finalimage);
}

void VID_Update(vrect_t *rects)
{
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

qboolean SNDDMA_Init(void)
{
   shm = &sn;
   shm->speed = SAMPLERATE;
   shm->channels = 2;
   shm->samplepos = 0;
   shm->samplebits = 16;
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
   int mx, my;

   if (quake_devices[0] != RETRO_DEVICE_KEYBOARD)
      return;

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

