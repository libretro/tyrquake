/* Copyright  (C) 2010-2015 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (retro_stat.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#ifdef _MSC_VER
#define setmode _setmode
#endif
#ifdef _XBOX
#include <xtl.h>
#define INVALID_FILE_ATTRIBUTES -1
#else
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <windows.h>
#endif
#elif defined(VITA)
#define SCE_ERROR_ERRNO_EEXIST 0x80010011
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(PSP)
#include <pspkernel.h>
#endif

#ifdef __HAIKU__
#include <kernel/image.h>
#endif

#if defined(__CELLOS_LV2__)
#include <cell/cell_fs.h>
#endif

#if defined(VITA)
#define FIO_SO_ISDIR PSP2_S_ISDIR
#endif

#if (defined(__CELLOS_LV2__) && !defined(__PSL1GHT__)) || defined(__QNX__) || defined(PSP)
#include <unistd.h> /* stat() is defined here */
#endif

#include <retro_miscellaneous.h>
#include <boolean.h>

/**
 * path_is_directory:
 * @path               : path
 *
 * Checks if path is a directory.
 *
 * Returns: true (1) if path is a directory, otherwise false (0).
 */
bool path_is_directory(const char *path)
{
#if defined(VITA) || defined(PSP)
   SceIoStat buf;
   if (sceIoGetstat(path, &buf) < 0)
      return false;
   return FIO_SO_ISDIR(buf.st_mode);
#elif defined(__CELLOS_LV2__)
    CellFsStat buf;
    if (cellFsStat(path, &buf) < 0)
       return false;
    return ((buf.st_mode & S_IFMT) == S_IFDIR);
#elif defined(_WIN32)
   DWORD ret = GetFileAttributes(path);
   return (ret & FILE_ATTRIBUTE_DIRECTORY) && (ret != INVALID_FILE_ATTRIBUTES);
#else
   struct stat buf;
   if (stat(path, &buf) < 0)
      return false;

   return S_ISDIR(buf.st_mode);
#endif
}

/**
 * path_mkdir_norecurse:
 * @dir                : directory
 *
 * Create directory on filesystem.
 *
 * Returns: true (1) if directory could be created, otherwise false (0).
 **/
bool mkdir_norecurse(const char *dir)
{
   int ret;
#if defined(_WIN32)
   ret = _mkdir(dir);
#elif defined(IOS)
   ret = mkdir(dir, 0755);
#elif defined(VITA) || defined(PSP)
   ret = sceIoMkdir(dir, 0777);
#else
   ret = mkdir(dir, 0750);
#endif
   /* Don't treat this as an error. */
#if defined(VITA)
   if ((ret == SCE_ERROR_ERRNO_EEXIST) && path_is_directory(dir))
      ret = 0;
#else 
   if (ret < 0 && errno == EEXIST && path_is_directory(dir))
      ret = 0;
#endif
   if (ret < 0)
      printf("mkdir(%s) error: %s.\n", dir, strerror(errno));
   return ret == 0;
}

bool stat_is_valid(const char *path)
{
#if defined(VITA) || defined(PSP)
   SceIoStat buf;
   if (sceIoGetstat(path, &buf) < 0)
      return false;
   return true;
#elif defined(__CELLOS_LV2__)
    CellFsStat buf;
    if (cellFsStat(path, &buf) < 0)
       return false;
    return true;
#elif defined(_WIN32)
   DWORD ret = GetFileAttributes(path);
   return (ret != INVALID_FILE_ATTRIBUTES);
#else
   struct stat buf;
   if (stat(path, &buf) < 0)
      return false;
   return true;
#endif
}