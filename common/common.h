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
// common.h -- general definitions

#ifndef COMMON_H
#define COMMON_H

#include <stdarg.h>
#include <retro_inline.h>
#include <streams/file_stream.h>

#include "qtypes.h"
#include "shell.h"

#ifdef NQ_HACK
#include "quakedef.h"
#endif
#ifdef QW_HACK
#include "bothdefs.h"
#include "protocol.h"
#endif

#define MAX_NUM_ARGVS 50

#define stringify__(x) #x
#define stringify(x) stringify__(x)

#ifdef QW_HACK
#define	MAX_INFO_STRING 196
#define	MAX_SERVERINFO_STRING 512
#define	MAX_LOCALINFO_STRING 32768
#endif

//============================================================================

typedef struct sizebuf_s {
    qboolean allowoverflow;	// if false, do a Sys_Error
    qboolean overflowed;	// set to true if the buffer size failed
    byte *data;
    int maxsize;
    int cursize;
} sizebuf_t;

#ifdef NQ_HACK
void SZ_Alloc(sizebuf_t *buf, int startsize);
void SZ_Free(sizebuf_t *buf);
#endif
void SZ_Clear(sizebuf_t *buf);
void SZ_Write(sizebuf_t *buf, const void *data, int length);
void SZ_Print(sizebuf_t *buf, const char *data); // strcats onto the sizebuf

//============================================================================

typedef struct link_s {
    struct link_s *prev, *next;
} link_t;

void ClearLink(link_t *l);
void RemoveLink(link_t *l);
void InsertLinkBefore(link_t *l, link_t *before);
void InsertLinkAfter(link_t *l, link_t *after);

//============================================================================

#define Q_MAXCHAR ((char)0x7f)
#define Q_MAXSHORT ((short)0x7fff)
#define Q_MAXINT ((int)0x7fffffff)
#define Q_MAXLONG ((int)0x7fffffff)
#define Q_MAXFLOAT ((int)0x7fffffff)

#define Q_MINCHAR ((char)0x80)
#define Q_MINSHORT ((short)0x8000)
#define Q_MININT ((int)0x80000000)
#define Q_MINLONG ((int)0x80000000)
#define Q_MINFLOAT ((int)0x7fffffff)

/*
 * ========================================================================
 *                          BYTE ORDER FUNCTIONS
 * ========================================================================
 */

static INLINE short bswap16(short s)
{
    return ((s & 255) << 8) | ((s >> 8) & 255);
}
static INLINE int bswap32(int l)
{
    return
          (((l >>  0) & 255) << 24)
        | (((l >>  8) & 255) << 16)
        | (((l >> 16) & 255) <<  8)
        | (((l >> 24) & 255) <<  0);
}

#ifdef MSB_FIRST
#define BigShort(s) (s)
#define BigLong(l) (l)
#define BigFloat(f) (f)
static INLINE short LittleShort(short s) { return bswap16(s); }
static INLINE int LittleLong(int l) { return bswap32(l); }
static INLINE float LittleFloat(float f)
{
   union {
      float f;
      byte b[4];
   } dat1, dat2;

   dat1.f = f;
   dat2.b[0] = dat1.b[3];
   dat2.b[1] = dat1.b[2];
   dat2.b[2] = dat1.b[1];
   dat2.b[3] = dat1.b[0];
   return dat2.f;
}
#else
static INLINE short BigShort(short s) { return bswap16(s); }
static INLINE int BigLong(int l) { return bswap32(l); }
static INLINE float BigFloat(float f)
{
   union {
      float f;
      byte b[4];
   } dat1, dat2;

   dat1.f = f;
   dat2.b[0] = dat1.b[3];
   dat2.b[1] = dat1.b[2];
   dat2.b[2] = dat1.b[1];
   dat2.b[3] = dat1.b[0];
   return dat2.f;
}
#define LittleShort(s) (s)
#define LittleLong(l) (l)
#define LittleFloat(f) (f)
#endif

//============================================================================

#ifdef QW_HACK
extern struct usercmd_s nullcmd;
#endif

void MSG_WriteChar(sizebuf_t *sb, int c);
void MSG_WriteByte(sizebuf_t *sb, int c);
void MSG_WriteShort(sizebuf_t *sb, int c);
void MSG_WriteLong(sizebuf_t *sb, int c);
void MSG_WriteFloat(sizebuf_t *sb, float f);
void MSG_WriteString(sizebuf_t *sb, const char *s);
void MSG_WriteStringf(sizebuf_t *sb, const char *fmt, ...);
void MSG_WriteStringvf(sizebuf_t *sb, const char *fmt, va_list ap);
void MSG_WriteCoord(sizebuf_t *sb, float f);
void MSG_WriteAngle(sizebuf_t *sb, float f);
void MSG_WriteAngle16(sizebuf_t *sb, float f);
#ifdef QW_HACK
void MSG_WriteDeltaUsercmd(sizebuf_t *sb, const struct usercmd_s *from,
			   const struct usercmd_s *cmd);
#endif
#ifdef NQ_HACK
void MSG_WriteControlHeader(sizebuf_t *sb);
#endif

extern int msg_readcount;
extern qboolean msg_badread;	// set if a read goes beyond end of message

void MSG_BeginReading(void);
#ifdef QW_HACK
int MSG_GetReadCount(void);
#endif
int MSG_ReadChar(void);
int MSG_ReadByte(void);
int MSG_ReadShort(void);
int MSG_ReadLong(void);
float MSG_ReadFloat(void);
char *MSG_ReadString(void);
#ifdef QW_HACK
char *MSG_ReadStringLine(void);
#endif

float MSG_ReadCoord(void);
float MSG_ReadAngle(void);
float MSG_ReadAngle16(void);
#ifdef QW_HACK
void MSG_ReadDeltaUsercmd(const struct usercmd_s *from, struct usercmd_s *cmd);
#endif
#ifdef NQ_HACK
int MSG_ReadControlHeader(void);
#endif

//============================================================================

int Q_atoi(const char *str);
float Q_atof(const char *str);

//============================================================================

extern char com_token[1024];
extern qboolean com_eof;

const char *COM_Parse(const char *data);

extern unsigned com_argc;
extern const char **com_argv;

unsigned COM_CheckParm(const char *parm);
#ifdef QW_HACK
void COM_AddParm(const char *parm);
#endif

void COM_Init(void);
void COM_InitArgv(int argc, const char **argv);

const char *COM_SkipPath(const char *pathname);
const char *COM_FileExtension(const char *in);
qboolean COM_FileExists(const char *filename);
void COM_StripExtension(char *filename);
void COM_DefaultExtension(char *path, const char *extension);
int COM_CheckExtension(const char *path, const char *extn);

char *va(const char *format, ...);

// does a varargs printf into a temp buffer

//============================================================================

extern int com_filesize;
struct cache_user_s;

extern char com_basedir[MAX_OSPATH];
extern char com_gamedir[MAX_OSPATH];
extern char com_savedir[MAX_OSPATH];
extern int file_from_pak; // global indicating that file came from a pak

void COM_WriteFile(const char *filename, const void *data, int len);
int COM_FOpenFile(const char *filename, RFILE **file);
void COM_ScanDir(struct stree_root *root, const char *path,
		 const char *pfx, const char *ext, qboolean stripext);

void *COM_LoadStackFile(const char *path, void *buffer, int bufsize,
			unsigned long *length);
void *COM_LoadTempFile(const char *path);
void *COM_LoadHunkFile(const char *path);
void COM_LoadCacheFile(const char *path, struct cache_user_s *cu);
#ifdef QW_HACK
void COM_CreatePath(const char *path);
void COM_Gamedir(const char *dir);
#endif

extern struct cvar_s registered;
extern qboolean standard_quake, rogue, hipnotic;

#ifdef QW_HACK
char *Info_ValueForKey(const char *infostring, const char *key);
void Info_RemoveKey(char *infostring, const char *key);
void Info_RemovePrefixedKeys(char *infostring, char prefix);
void Info_SetValueForKey(char *infostring, const char *key, const char *value,
			 int maxsize);
void Info_SetValueForStarKey(char *infostring, const char *key,
			     const char *value, int maxsize);
void Info_Print(const char *infostring);

unsigned Com_BlockChecksum(const void *buffer, int length);
void Com_BlockFullChecksum(const void *buffer, int len,
			   unsigned char outbuf[16]);
byte COM_BlockSequenceCheckByte(const byte *base, int length, int sequence,
				unsigned mapchecksum);
byte COM_BlockSequenceCRCByte(const byte *base, int length, int sequence);

int build_number(void);

extern char gamedirfile[];
#endif


// Leilei Colored lighting

extern byte	palmap2[64][64][64];	// 18-bit lookup table 

/* The following FS_*() stdio replacements are necessary if one is
 * to perform non-sequential reads on files reopened on pak files
 * because we need the bookkeeping about file start/end positions.
 * Allocating and filling in the fshandle_t structure is the users'
 * responsibility when the file is initially opened. */

typedef struct _fshandle_t
{
	RFILE *file;
	qboolean pak;	/* is the file read from a pak */
	long start;	/* file or data start position */
	long length;	/* file or data size */
	long pos;	/* current position relative to start */
} fshandle_t;

size_t FS_fread(void *ptr, size_t size, size_t nmemb, fshandle_t *fh);
int FS_fseek(fshandle_t *fh, long offset, int whence);
long FS_ftell(fshandle_t *fh);
void FS_rewind(fshandle_t *fh);
int FS_feof(fshandle_t *fh);
int FS_ferror(fshandle_t *fh);
int FS_fclose(fshandle_t *fh);
int FS_fgetc(fshandle_t *fh);
char *FS_fgets(char *s, int size, fshandle_t *fh);
long FS_filelength (fshandle_t *fh);

#endif /* COMMON_H */
