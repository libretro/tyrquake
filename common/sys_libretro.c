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

#if defined(__CELLOS_LV2__)
#define BASEWIDTH 400
#define BASEHEIGHT 224
#elif defined(_XBOX360)
#define BASEWIDTH 512
#define BASEHEIGHT 224
#elif defined(ANDROID)|| defined(__QNX__) || defined(GEKKO) || defined(_XBOX1) || defined(IOS)
#define BASEWIDTH 320
#define BASEHEIGHT 200
#else /* for PC */
#define BASEWIDTH 640
#define BASEHEIGHT 480
#endif

unsigned MEMSIZE_MB;

#if defined(HW_DOL)
#define DEFAULT_MEMSIZE_MB 8
#elif defined(HW_RVL) || defined(_XBOX1)
#define DEFAULT_MEMSIZE_MB 24
#else
#define DEFAULT_MEMSIZE_MB 32
#endif

#define SAMPLERATE 44100

static qboolean nostdout = false;

cvar_t framerate = { "framerate", "60" };

char g_rom_dir[256];
char g_pak_path[256];
unsigned short	palette_data[256];

unsigned char *heap;

// =======================================================================
// General routines
// =======================================================================
//
#ifdef _XBOX1
#define DEBUG_SYS_PRINTF
#endif

void Sys_Printf(const char *fmt, ...)
{
#ifdef DEBUG_SYS_PRINTF
#ifdef _XBOX1
   char msg_new[1024], buffer[1024];
   snprintf(msg_new, sizeof(msg_new), "%s", fmt);
   va_list ap;
   va_start(ap, fmt);
   wvsprintf(buffer, msg_new, ap);
   OutputDebugStringA(buffer);
   va_end(ap);
#else
   va_list argptr;
   char text[MAX_PRINTMSG];
   unsigned char *p;

   va_start(argptr, fmt);
   vsnprintf(text, sizeof(text), fmt, argptr);
   va_end(argptr);

   if (nostdout)
      return;

   for (p = (unsigned char *)text; *p; p++) {
      if ((*p > 128 || *p < 32) && *p != 10 && *p != 13 && *p != 9)
         printf("[%02x]", *p);
      else
         putc(*p, stdout);
   }
#endif
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
#ifdef _XBOX1
   char msg_new[1024], buffer[1024];
   snprintf(msg_new, sizeof(msg_new), "Error: %s", error);
   va_list ap;
   va_start(ap, error);
   wvsprintf(buffer, msg_new, ap);
   OutputDebugStringA(buffer);
   va_end(ap);
#else
   va_list argptr;
   char string[MAX_PRINTMSG];

   va_start(argptr, error);
   vsnprintf(string, sizeof(string), error, argptr);
   va_end(argptr);
   fprintf(stderr, "Error: %s\n", string);
#endif

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
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "TyrQuake";
   info->library_version  = "v" stringify(TYR_VERSION);
   info->need_fullpath    = false;
   info->valid_extensions = "pak";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = framerate.value;
   info->timing.sample_rate = SAMPLERATE;

   info->geometry.base_width   = BASEWIDTH;
   info->geometry.base_height  = BASEHEIGHT;
   info->geometry.max_width    = BASEWIDTH;
   info->geometry.max_height   = BASEHEIGHT;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}


static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
retro_environment_t environ_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
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
   poll_cb();

	Key_Event(K_ESCAPE, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START));
	Key_Event(K_INS, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X));
	Key_Event(K_UPARROW, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP));
	Key_Event(K_DOWNARROW, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN));
	Key_Event(K_LEFTARROW, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT));
	Key_Event(K_RIGHTARROW, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT));
	Key_Event(K_ENTER, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B));
	Key_Event(K_MOUSE1, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y));
	Key_Event(K_COMMA, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L));
	Key_Event(K_PERIOD, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R));
	Key_Event(K_SLASH, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A));
	Key_Event(K_END, input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3));

#if 0
   int analog_left_x = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
         RETRO_DEVICE_ID_ANALOG_X);
   int analog_left_y = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
         RETRO_DEVICE_ID_ANALOG_Y);
   int analog_right_x = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
         RETRO_DEVICE_ID_ANALOG_X);
   int analog_right_y = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
         RETRO_DEVICE_ID_ANALOG_Y);

   if (analog_left_x > 0)
      Key_Event(K_PERIOD, 1);

   if (analog_left_x < 0)
      Key_Event(K_COMMA, 1);

   if (analog_left_y > 0)
      Key_Event(K_DOWNARROW, 1);

   if (analog_left_y < 0)
      Key_Event(K_UPARROW, 1);

   if (analog_right_x > 0)
      Key_Event(K_RIGHTARROW, 1);

   if (analog_right_x < 0)
      Key_Event(K_LEFTARROW, 1);

   Key_Event(K_DEL, analog_right_y > 0);
   Key_Event(K_PGDN, analog_right_y < 0);
#endif

	if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT))
   {
      Cvar_SetValue("cl_forwardspeed", 200);
      Cvar_SetValue("cl_backspeed", 200);
   }
   else
   {
      Cvar_SetValue("cl_forwardspeed", 400);
      Cvar_SetValue("cl_backspeed", 400);
   }

}

static short finalimage[BASEWIDTH * BASEHEIGHT];

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

#define AUDIO_BUFFER_SAMPLES (8 * 1024)
static int16_t audio_buffer[AUDIO_BUFFER_SAMPLES];
static unsigned audio_buffer_ptr;
void retro_run(void)
{
   unsigned char *ilineptr = (unsigned char*)vid.buffer;
   unsigned short *olineptr = (unsigned short*)finalimage;
   unsigned y, x;

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
      oldtime += _time;
#endif
#ifdef QW_HACK
   oldtime = newtime;
#endif

   Host_Frame(_time);

   for (y = 0; y < BASEHEIGHT; ++y)
   {
      for (x = 0; x < BASEWIDTH; ++x)
         *olineptr++ = palette_data[*ilineptr++];
   }

   video_cb(finalimage, BASEWIDTH, BASEHEIGHT, BASEWIDTH << 1);
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

bool retro_load_game(const struct retro_game_info *info)
{
   quakeparms_t parms;

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
   fprintf(stderr, "Quake Libretro -- TyrQuake Version %s\n", stringify(TYR_VERSION));
#endif
#ifdef QW_HACK
   fprintf(stderr, "QuakeWorld Libretro -- TyrQuake Version %s\n", stringify(TYR_VERSION));
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
      fprintf(stderr, "RGB565 is not supported.\n");
      return false;
   }

   /* set keyboard callback */

   //struct retro_keyboard_callback cb = { keyboard_cb };
   //environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cb);

#ifdef NQ_HACK
   oldtime = Sys_DoubleTime() - 0.1;
#endif
#ifdef QW_HACK
   oldtime = Sys_DoubleTime();
#endif

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


byte vid_buffer[BASEWIDTH * BASEHEIGHT];
short zbuffer[BASEWIDTH * BASEHEIGHT];
byte surfcache[256 * 1024];

unsigned short d_8to16table[256];


void VID_SetPalette(unsigned char *palette)
{
}

void VID_ShiftPalette(unsigned char *palette)
{
}

void VID_Init(unsigned char *palette)
{
   /* TODO */
    vid.width = BASEWIDTH;
    vid.height = BASEHEIGHT;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.conwidth = vid.width;
    vid.conheight = vid.height;
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = vid_buffer;
    vid.rowbytes = BASEWIDTH;
    vid.conrowbytes = vid.rowbytes;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);

    d_pzbuffer = zbuffer;
    D_InitCaches(surfcache, sizeof(surfcache));
}

void VID_Shutdown(void)
{
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

