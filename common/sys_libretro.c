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

#ifdef _WIN32
#include <windows.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

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

#if defined(GEKKO) || defined(_XBOX) || defined(__CELLOS_LV2__)
#define BASEWIDTH 512
#define BASEHEIGHT 224
#else
#define BASEWIDTH 640
#define BASEHEIGHT 448
#endif
#define MEMSIZE_MB 16

static qboolean nostdout = false;

char g_rom_dir[256];
char g_pak_path[256];
unsigned short	palette_data[256];

unsigned char *heap;

// =======================================================================
// General routines
// =======================================================================

void Sys_Printf(const char *fmt, ...)
{
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
   va_list argptr;
   char string[MAX_PRINTMSG];

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
   return sys_time_get_system_time();
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

#ifdef NQ_HACK
#ifdef _WIN32
/*
 * For debugging - Print a Win32 system error string to stdout
 */
static void
Print_Win32SystemError(DWORD err)
{
    static PVOID buf;

    if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER
		      | FORMAT_MESSAGE_FROM_SYSTEM,
		      NULL, err, 0, (LPTSTR)(&buf), 0, NULL)) {
	printf("%s: %s\n", __func__, (LPTSTR)buf);
	fflush(stdout);
	LocalFree(buf);
    }
}
#endif
#endif

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
   info->valid_extensions = "pak|PAK|zip|ZIP"; // Anything is fine, we don't care.
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing = (struct retro_system_timing) {
      .fps = 60.0,
      .sample_rate = 30000.0,
   };

   info->geometry = (struct retro_game_geometry) {
      .base_width   = BASEWIDTH,
      .base_height  = BASEHEIGHT,
      .max_width    = BASEWIDTH,
      .max_height   = BASEHEIGHT,
      .aspect_ratio = 4.0 / 3.0,
   };
}


static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
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
#ifdef __CELLOS_LV2__
   sys_timer_usleep(1);
#else
   usleep(1);
#endif
}

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
         Sys_Sleep();
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
      {
         *olineptr++ = palette_data[*ilineptr++];
      }
   }

   video_cb(finalimage, BASEWIDTH, BASEHEIGHT, BASEWIDTH << 1);
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

   if (strstr(info->path, "hipnotic") || strstr(info->path, "rogue"))
      extract_directory(g_rom_dir, g_rom_dir, sizeof(g_rom_dir));

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

   /* Set up key descriptors */
   struct retro_input_descriptor desc[] = {
      { .port = 0, .device = RETRO_DEVICE_JOYPAD, .index = 0, .id = RETRO_DEVICE_ID_JOYPAD_LEFT,  .description = "Left" },
      { .port = 0, .device = RETRO_DEVICE_JOYPAD, .index = 0, .id = RETRO_DEVICE_ID_JOYPAD_UP,    .description = "Up" },
      { .port = 0, .device = RETRO_DEVICE_JOYPAD, .index = 0, .id = RETRO_DEVICE_ID_JOYPAD_DOWN,  .description = "Down" },
      { .port = 0, .device = RETRO_DEVICE_JOYPAD, .index = 0, .id = RETRO_DEVICE_ID_JOYPAD_RIGHT, .description = "Right" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

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
   return 2;
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
   return false;
}

int SNDDMA_GetDMAPos(void)
{
   return 0;
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

