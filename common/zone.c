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

#include "cmd.h"
#include "common.h"
#include "console.h"
#include "mathlib.h"
#include "quakedef.h"
#include "sys.h"
#include "zone.h"

#ifdef HEXEN2
#define	DYNAMIC_SIZE	0xc000
#else
#define	DYNAMIC_SIZE	0x40000		/* 256k */
#endif
#define	ZONEID		0x1d4a11
#define MINFRAGMENT	64

typedef struct memblock_s
{
    int size;		/* including the header and possibly tiny fragments */
    int tag;		/* a tag of 0 is a free block */
    int id;		/* should be ZONEID */
    int pad;		/* pad to 64 bit boundary */
    struct memblock_s *next, *prev;
} memblock_t;

typedef struct
{
    int size;			/* total bytes malloced, including header */
    memblock_t blocklist;	/* start/end cap for linked list */
    memblock_t *rover;
} memzone_t;

static void Cache_FreeLow(int new_low_hunk);
static void Cache_FreeHigh(int new_high_hunk);

/*
 * ============================================================================
 *
 * ZONE MEMORY ALLOCATION
 *
 * There is never any space between memblocks, and there will never be two
 * contiguous free memblocks.
 *
 * The rover can be left pointing at a non-empty block
 *
 * The zone calls are pretty much only used for small strings and structures,
 * all big things are allocated on the hunk.
 * ============================================================================
 */

static memzone_t *mainzone;

static void Z_ClearZone(memzone_t *zone, int size);


/*
 * ========================
 * Z_ClearZone
 * ========================
 */
static void
Z_ClearZone(memzone_t *zone, int size)
{
    memblock_t *block;

    /*
     * set the entire zone to one free block
     */
    zone->blocklist.next = zone->blocklist.prev = block =
	(memblock_t *)((byte *)zone + sizeof(memzone_t));
    zone->blocklist.tag  = 1;	/* in use block */
    zone->blocklist.id   = 0;
    zone->blocklist.size = 0;
    zone->rover          = block;

    block->prev          = block->next = &zone->blocklist;
    block->tag           = 0;		/* free block */
    block->id            = ZONEID;
    block->size          = size - sizeof(memzone_t);
}


/*
 * ========================
 * Z_Free
 * ========================
 */
void
Z_Free(const void *ptr)
{
   memblock_t *block, *other;

   if (!ptr)
      Sys_Error("%s: NULL pointer", __func__);

   block = (memblock_t *)((const byte *)ptr - sizeof(memblock_t));
   if (block->id != ZONEID)
      Sys_Error("%s: freed a pointer without ZONEID", __func__);
   if (block->tag == 0)
      Sys_Error("%s: freed a freed pointer", __func__);

   block->tag = 0;		/* mark as free */

   other = block->prev;
   if (!other->tag)
   {
      /* merge with previous free block */
      other->size += block->size;
      other->next = block->next;
      other->next->prev = other;
      if (block == mainzone->rover)
         mainzone->rover = other;
      block = other;
   }

   other = block->next;
   if (!other->tag)
   {
      /* merge the next free block onto the end */
      block->size += other->size;
      block->next = other->next;
      block->next->prev = block;
      if (other == mainzone->rover)
         mainzone->rover = block;
   }

    /*
     * Always start looking from the first available free block.
     * Slower, but not too bad and we don't fragment nearly as much.
     */
    if (block < mainzone->rover) {
	mainzone->rover = block;
    }
}

static void *Z_TagMalloc(int size, int tag)
{
   int extra;
   memblock_t *start, *rover, *newobj, *base;

   if (!tag)
      Sys_Error("%s: tried to use a 0 tag", __func__);

   /*
    * Scan through the block list looking for the first free block of
    * sufficient size
    */
   size += sizeof(memblock_t);	/* account for size of block header */
   size += 4;			/* space for memory trash tester */
   size = (size + 7) & ~7;	/* align to 8-byte boundary */

   /* If we ended on an allocated block, skip forward to the first free block */
    start = mainzone->rover->prev;
    while (mainzone->rover->tag && mainzone->rover != start)
	mainzone->rover = mainzone->rover->next;
 
    base = rover = mainzone->rover;

   do {
      if (rover == start)	/* scaned all the way around the list */
         return NULL;
      if (rover->tag)
         base = rover = rover->next;
      else
         rover = rover->next;
   } while (base->tag || base->size < size);

   /* found a block big enough */
   extra = base->size - size;
   if (extra > MINFRAGMENT)
   {
      /* there will be a free fragment after the allocated block */
      newobj = (memblock_t *)((byte *)base + size);
      newobj->size = extra;
      newobj->tag = 0;		/* free block */
      newobj->prev = base;
      newobj->id = ZONEID;
      newobj->next = base->next;
      newobj->next->prev = newobj;
      base->next = newobj;
      base->size = size;
   }

   base->tag = tag;		   /* no longer a free block */

   /*
     * If we just allocated the first available block, the next
     * allocation starts looking after this one.
     */
    if (base == mainzone->rover)
	mainzone->rover = base->next;

   base->id = ZONEID;

   /* marker for memory trash testing */
   *(int *)((byte *)base + base->size - 4) = ZONEID;

   return (void *)((byte *)base + sizeof(memblock_t));
}


/*
 * ========================
 * Z_Malloc
 * ========================
 */
void * Z_Malloc(int size)
{
   void *buf = Z_TagMalloc(size, 1);
   if (!buf)
      Sys_Error("%s: failed on allocation of %i bytes", __func__, size);
   memset(buf, 0, size);

   return buf;
}

/*
 * ========================
 * Z_Realloc
 * ========================
 */
void *Z_Realloc(const void *ptr, int size)
{
   memblock_t *block;
   int orig_size;
   void *ret;

   if (!ptr)
      return Z_Malloc(size);

   block = (memblock_t *)((byte *)ptr - sizeof(memblock_t));
   if (block->id != ZONEID)
      Sys_Error("%s: realloced a pointer without ZONEID", __func__);
   if (!block->tag)
      Sys_Error("%s: realloced a freed pointer", __func__);

   orig_size = block->size;
   orig_size -= sizeof(memblock_t);
   orig_size -= 4;

   Z_Free(ptr);
   ret = Z_TagMalloc(size, 1);
   if (!ret)
      Sys_Error("%s: failed on allocation of %i bytes", __func__, size);
   if (ret != ptr)
      memmove(ret, ptr, qmin(orig_size, size));

   return ret;
}

/* ======================================================================= */

#define	HUNK_SENTINAL	0x1df001ed

#ifdef HEXEN2
#define HUNK_NAMELEN	20
#else
#define HUNK_NAMELEN	8
#endif

typedef struct
{
   int sentinal;
   int size;		/* including sizeof(hunk_t), -1 = not allocated */
} hunk_t;

static byte *hunk_base;
static int hunk_size;

static int hunk_low_used;
static int hunk_high_used;

static qboolean hunk_tempactive;
static int hunk_tempmark;

/*
 * ==============
 * Hunk_Check
 *
 * Run consistancy and sentinal trashing checks
 * ==============
 */
void Hunk_Check(void)
{
   hunk_t *h;

   for (h = (hunk_t *)hunk_base; (byte *)h != hunk_base + hunk_low_used;)
   {
      if (h->sentinal != HUNK_SENTINAL)
         Sys_Error("%s: trashed sentinal", __func__);
      if (h->size < sizeof(hunk_t) ||
            h->size + (byte *)h - hunk_base > hunk_size)
         Sys_Error("%s: bad size", __func__);
      h = (hunk_t *)((byte *)h + h->size);
   }
}

/*
 * ===================
 * Hunk_Alloc
 * ===================
 */
void *Hunk_Alloc(int size)
{
   hunk_t *h;

   if (size < 0)
      Sys_Error("%s: bad size: %i", __func__, size);

   size = sizeof(hunk_t) + ((size + 15) & ~15);

   if (hunk_size - hunk_low_used - hunk_high_used < size)
   {
      Sys_Error ("%s: failed on %i bytes", __func__, size);
   }

   h = (hunk_t *)(hunk_base + hunk_low_used);
   hunk_low_used += size;

   Cache_FreeLow(hunk_low_used);

   memset(h, 0, size);

   h->size = size;
   h->sentinal = HUNK_SENTINAL;

   return (void *)(h + 1);
}

int Hunk_LowMark(void)
{
   return hunk_low_used;
}

void Hunk_FreeToLowMark(int mark)
{
   if (mark < 0 || mark > hunk_low_used)
      Sys_Error("%s: bad mark %i", __func__, mark);
   memset(hunk_base + mark, 0, hunk_low_used - mark);
   hunk_low_used = mark;
}

int Hunk_HighMark(void)
{
   if (hunk_tempactive)
   {
      hunk_tempactive = false;
      Hunk_FreeToHighMark(hunk_tempmark);
   }

   return hunk_high_used;
}

void Hunk_FreeToHighMark(int mark)
{
   if (hunk_tempactive)
   {
      hunk_tempactive = false;
      Hunk_FreeToHighMark(hunk_tempmark);
   }
   if (mark < 0 || mark > hunk_high_used)
      Sys_Error("%s: bad mark %i", __func__, mark);
   memset(hunk_base + hunk_size - hunk_high_used, 0, hunk_high_used - mark);
   hunk_high_used = mark;
}


/*
 * ===================
 * Hunk_HighAlloc
 * ===================
 */
void *Hunk_HighAlloc(int size)
{
   hunk_t *h;

   if (size < 0)
      Sys_Error("%s: bad size: %i", __func__, size);

   if (hunk_tempactive)
   {
      Hunk_FreeToHighMark(hunk_tempmark);
      hunk_tempactive = false;
   }

   size = sizeof(hunk_t) + ((size + 15) & ~15);

   if (hunk_size - hunk_low_used - hunk_high_used < size)
   {
      Con_Printf("Hunk_HighAlloc: failed on %i bytes\n", size);
      return NULL;
   }

   hunk_high_used += size;
   Cache_FreeHigh(hunk_high_used);

   h = (hunk_t *)(hunk_base + hunk_size - hunk_high_used);

   memset(h, 0, size);
   h->size = size;
   h->sentinal = HUNK_SENTINAL;

   return (void *)(h + 1);
}


/*
 * =================
 * Hunk_TempAlloc
 *
 * Return space from the top of the hunk
 * =================
 */
void *Hunk_TempAlloc(int size)
{
   void *buf;

   size = (size + 15) & ~15;

   if (hunk_tempactive)
   {
      Hunk_FreeToHighMark(hunk_tempmark);
      hunk_tempactive = false;
   }

   hunk_tempmark = Hunk_HighMark();

   buf = Hunk_HighAlloc(size);

   hunk_tempactive = true;

   return buf;
}

/*
 * =====================
 * Hunk_TempAllocExtend
 *
 * Extend the existing temp hunk allocation.
 * Size is the number of extra bytes required
 * =====================
 */
void *Hunk_TempAllocExtend(int size)
{
   hunk_t *old, *newobj;

   if (!hunk_tempactive)
      Sys_Error("%s: temp hunk not active", __func__);

   old = (hunk_t *)(hunk_base + hunk_size - hunk_high_used);

   if (old->sentinal != HUNK_SENTINAL)
      Sys_Error("%s: old sentinal trashed\n", __func__);

   size = (size + 15) & ~15;
   if (hunk_size - hunk_low_used - hunk_high_used < size) {
      Con_Printf("%s: failed on %i bytes\n", __func__, size);
      return NULL;
   }

   hunk_high_used += size;
   Cache_FreeHigh(hunk_high_used);

   newobj = (hunk_t *)(hunk_base + hunk_size - hunk_high_used);
   memmove(newobj, old, sizeof(hunk_t));
   newobj->size += size;

   return (void *)(newobj + 1);
}

/*
 * ===========================================================================
 *
 * CACHE MEMORY
 *
 * ===========================================================================
 */

#define CACHE_NAMELEN 32

typedef struct cache_system_s
{
   int size;			/* including this header */
   cache_user_t *user;
   struct cache_system_s *prev, *next;
   struct cache_system_s *lru_prev, *lru_next;	/* for LRU flushing */
} cache_system_t;

static cache_system_t cache_head;
static cache_system_t *Cache_TryAlloc(int size, qboolean nobottom);

static INLINE cache_system_t *Cache_System(const cache_user_t *c)
{
   return (cache_system_t *)((byte *)c->data - c->pad) - 1;
}

static INLINE void *Cache_Data(const cache_system_t *c)
{
   return (byte *)(c + 1) + c->user->pad;
}

/*
 * ===========
 * Cache_Move
 * ===========
 */
static void Cache_Move(cache_system_t *c)
{
   /* we are clearing up space at the bottom, so only allocate it late */
   cache_system_t *newobj = Cache_TryAlloc(c->size, true);

   if (newobj)
   {
      int pad;
      memcpy(newobj + 1, c + 1, c->size - sizeof(cache_system_t));
      newobj->user = c->user;
      pad = c->user->pad;
      Cache_Free(c->user);
      newobj->user->pad = pad;
      newobj->user->data = Cache_Data(newobj);
   }
   else
   {
      /* tough luck... */
      Cache_Free(c->user);
   }
}

/*
 * ============
 * Cache_FreeLow
 *
 * Throw things out until the hunk can be expanded to the given point
 * ============
 */
static void Cache_FreeLow(int new_low_hunk)
{
   cache_system_t *c;

   while (1)
   {
      c = cache_head.next;
      if (c == &cache_head)
         return;		/* nothing in cache at all */
      if ((byte *)c >= hunk_base + new_low_hunk)
         return;		/* there is space to grow the hunk */
      Cache_Move(c);		/* reclaim the space */
   }
}

/*
 * ============
 * Cache_FreeHigh
 *
 * Throw things out until the hunk can be expanded to the given point
 * ============
 */
static void Cache_FreeHigh(int new_high_hunk)
{
   cache_system_t *c;
   cache_system_t *prev = NULL;

   while (1)
   {
      c = cache_head.prev;
      if (c == &cache_head)
         return;		/* nothing in cache at all */
      if ((byte *)c + c->size <= hunk_base + hunk_size - new_high_hunk)
         return;		/* there is space to grow the hunk */
      if (c == prev)
         Cache_Free(c->user);	/* didn't move out of the way */
      else
      {
         Cache_Move(c);	/* try to move it */
         prev = c;
      }
   }
}

static void Cache_UnlinkLRU(cache_system_t *cs)
{
   if (!cs->lru_next || !cs->lru_prev)
      Sys_Error("%s: NULL link", __func__);

   cs->lru_next->lru_prev = cs->lru_prev;
   cs->lru_prev->lru_next = cs->lru_next;

   cs->lru_prev = cs->lru_next = NULL;
}

static void Cache_MakeLRU(cache_system_t *cs)
{
   if (cs->lru_next || cs->lru_prev)
      Sys_Error("%s: active link", __func__);

   cache_head.lru_next->lru_prev = cs;
   cs->lru_next = cache_head.lru_next;
   cs->lru_prev = &cache_head;
   cache_head.lru_next = cs;
}

/*
 * ============
 * Cache_TryAlloc
 *
 * Looks for a free block of memory between the high and low hunk marks
 * Size should already include the header and padding
 * ============
 */
static cache_system_t *Cache_TryAlloc(int size, qboolean nobottom)
{
   cache_system_t *cs, *newobj;

   /* is the cache completely empty? */
   if (!nobottom && cache_head.prev == &cache_head)
   {
      if (hunk_size - hunk_high_used - hunk_low_used < size)
         Sys_Error("%s: %i is greater than free hunk", __func__, size);

      newobj = (cache_system_t *)(hunk_base + hunk_low_used);
      memset(newobj, 0, sizeof(*newobj));
      newobj->size = size;

      cache_head.prev = cache_head.next = newobj;
      newobj->prev = newobj->next = &cache_head;

      Cache_MakeLRU(newobj);
      return newobj;
   }

   /* search from the bottom up for space */
   newobj = (cache_system_t *)(hunk_base + hunk_low_used);
   cs = cache_head.next;

   do
   {
      if (!nobottom || cs != cache_head.next)
      {
         if ((byte *)cs - (byte *)newobj >= size)
         {	/* found space */
            memset(newobj, 0, sizeof(*newobj));
            newobj->size = size;

            newobj->next = cs;
            newobj->prev = cs->prev;
            cs->prev->next = newobj;
            cs->prev = newobj;

            Cache_MakeLRU(newobj);

            return newobj;
         }
      }

      /* continue looking */
      newobj = (cache_system_t *)((byte *)cs + cs->size);
      cs = cs->next;

   } while (cs != &cache_head);

   /* try to allocate one at the very end */
   if (hunk_base + hunk_size - hunk_high_used - (byte *)newobj >= size) {
      memset(newobj, 0, sizeof(*newobj));
      newobj->size = size;

      newobj->next = &cache_head;
      newobj->prev = cache_head.prev;
      cache_head.prev->next = newobj;
      cache_head.prev = newobj;

      Cache_MakeLRU(newobj);

      return newobj;
   }

   return NULL;		/* couldn't allocate */
}

/*
 * ============
 * Cache_Flush
 *
 * Throw everything out, so new data will be demand cached
 * ============
 */
void Cache_Flush(void)
{
   while (cache_head.next != &cache_head)
      Cache_Free(cache_head.next->user);	/* reclaim the space */
}

/*
 * ============
 * Cache_Report
 * ============
 */
void Cache_Report(void)
{
   Con_DPrintf("%4.1f megabyte data cache\n",
         (hunk_size - hunk_high_used -
          hunk_low_used) / (float)(1024 * 1024));
}

/*
 * ============
 * Cache_Init
 * ============
 */
static void Cache_Init(void)
{
   cache_head.next = cache_head.prev = &cache_head;
   cache_head.lru_next = cache_head.lru_prev = &cache_head;

   Cmd_AddCommand ("flush", Cache_Flush);
}

/*
 * ==============
 * Cache_Free
 *
 * Frees the memory and removes it from the LRU list
 * ==============
 */
void Cache_Free(cache_user_t *c)
{
   cache_system_t *cs;

   if (!c->data)
      Sys_Error("%s: not allocated", __func__);

   cs = Cache_System(c);
   cs->prev->next = cs->next;
   cs->next->prev = cs->prev;
   cs->next = cs->prev = NULL;

   c->pad = 0;
   c->data = NULL;

   Cache_UnlinkLRU(cs);
}

/*
 * ==============
 * Cache_Check
 * ==============
 */
void *Cache_Check(const cache_user_t *c)
{
   cache_system_t *cs;

   if (!c->data)
      return NULL;

   cs = Cache_System(c);

   /* move to head of LRU */
   Cache_UnlinkLRU(cs);
   Cache_MakeLRU(cs);

   return c->data;
}


/*
 * ==============
 * Cache_Alloc
 * ==============
 */
void *Cache_Alloc(cache_user_t *c, int size)
{
   return Cache_AllocPadded(c, 0, size);
}

/*
 * ==============
 * Cache_AllocPadded
 * ==============
 */
void *Cache_AllocPadded(cache_user_t *c, int pad, int size)
{
   if (c->data)
      Sys_Error("%s: allready allocated", __func__);

   if (size <= 0)
      Sys_Error("%s: size %i", __func__, size);

   size = (size + pad + sizeof(cache_system_t) + 15) & ~15;

   /* find memory for it */
   while (1)
   {
      cache_system_t *cs = Cache_TryAlloc(size, false);

      if (cs)
      {
         cs->user = c;
         c->pad = pad;
         c->data = Cache_Data(cs);
         break;
      }
      /* free the least recently used cache data */
      if (cache_head.lru_prev == &cache_head)
         Sys_Error("%s: out of memory", __func__);
      /* not enough memory at all */
      Cache_Free(cache_head.lru_prev->user);
   }

   return Cache_Check(c);
}

static void Cache_f(void)
{
   if (Cmd_Argc() == 2)
   {
      if (!strcmp(Cmd_Argv(1), "flush"))
      {
         Cache_Flush();
         return;
      }
   }
   Con_Printf("Usage: cache print|flush\n");
}

/* ========================================================================= */


/*
 * ========================
 * Memory_Init
 * ========================
 */
void Memory_Init(void *buf, int size)
{
   int p;
   int zonesize = DYNAMIC_SIZE;

   hunk_base = (byte*)buf;
   hunk_size = size;
   hunk_low_used = 0;
   hunk_high_used = 0;

   Cache_Init();
   p = COM_CheckParm("-zone");
   if (p) {
      if (p < com_argc - 1)
         zonesize = Q_atoi(com_argv[p + 1]) * 1024;
      else
         Sys_Error("%s: you must specify a size in KB after -zone",
               __func__);
   }
   mainzone = (memzone_t*)Hunk_Alloc(zonesize);
   Z_ClearZone(mainzone, zonesize);

   /* Needs to be added after the zone init... */
   Cmd_AddCommand("flush", Cache_Flush);
   Cmd_AddCommand("cache", Cache_f);
}
