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
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "common.h"
#include "console.h"
#include "quakedef.h"
#include "sound.h"

#define PAINTBUFFER_SIZE 512
#define CHANNELS 2

portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
int snd_scaletable[32][256];
short *snd_out;

#define CLAMP16(x) (((x) > 0x7fff) ? 0x7fff : (((x) < -0x7fff) ? -0x7fff : (x)))

void S_TransferPaintBuffer(int endtime)
{
   int i;
   int snd_vol = volume.value * 256;
   int *snd_p = (int *)paintbuffer;
   int lpaintedtime = paintedtime;

   while (lpaintedtime < endtime)
   {
      // handle recirculating buffer issues
      int lpos = lpaintedtime & ((shm->samples >> 1) - 1);
      int snd_linear_count = (shm->samples >> 1) - lpos;

      snd_out = (short *)shm->buffer + (lpos << 1);
      if (lpaintedtime + snd_linear_count > endtime)
         snd_linear_count = endtime - lpaintedtime;
      snd_linear_count <<= 1;

      // write a linear blast of samples
      for (i = 0; i < snd_linear_count; i += 2)
      {
         snd_out[i]   = CLAMP16((snd_p[i]     * snd_vol) >> 8);
         snd_out[i+1] = CLAMP16((snd_p[i + 1] * snd_vol) >> 8);
      }

      snd_p        += snd_linear_count;
      lpaintedtime += (snd_linear_count >> 1);
   }
}


/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

void SND_PaintChannelFrom8(channel_t *ch, sfxcache_t *sc, int endtime);
void SND_PaintChannelFrom16(channel_t *ch, sfxcache_t *sc, int endtime);

void S_PaintChannels(int endtime)
{
   int i;
   channel_t *ch;
   sfxcache_t *sc;
   int ltime, count;

   while (paintedtime < endtime)
   {
      // if paintbuffer is smaller than DMA buffer
      int end = endtime;
      if (endtime - paintedtime > PAINTBUFFER_SIZE)
         end = paintedtime + PAINTBUFFER_SIZE;

      // clear the paint buffer
      memset(paintbuffer, 0,
            (end - paintedtime) * sizeof(portable_samplepair_t));

      // paint in the channels.
      ch = channels;
      for (i = 0; i < total_channels; i++, ch++)
      {
         if (!ch->sfx)
            continue;
         if (!ch->leftvol && !ch->rightvol)
            continue;
         sc = S_LoadSound(ch->sfx);
         if (!sc)
            continue;

         ltime = paintedtime;

         while (ltime < end)
         {	// paint up to end
            if (ch->end < end)
               count = ch->end - ltime;
            else
               count = end - ltime;

            if (count > 0)
            {
               if (sc->width == 1)
                  SND_PaintChannelFrom8(ch, sc, count);
               else
                  SND_PaintChannelFrom16(ch, sc, count);

               ltime += count;
            }
            // if at end of loop, restart
            if (ltime >= ch->end)
            {
               if (sc->loopstart >= 0)
               {
                  ch->pos = sc->loopstart;
                  ch->end = ltime + sc->length - ch->pos;
               }
               else
               {	// channel just stopped
                  ch->sfx = NULL;
                  break;
               }
            }
         }
      }

      // transfer out according to DMA format
      S_TransferPaintBuffer(end);
      paintedtime = end;
   }
}

void SND_InitScaletable(void)
{
   int i, j;

   for (i = 0; i < 32; i++)
      for (j = 0; j < 256; j++)
         snd_scaletable[i][j] = ((j < 128) ? j : j - 256) * i * 8;
}

void SND_PaintChannelFrom8(channel_t *ch, sfxcache_t *sc, int count)
{
   int *lscale, *rscale;
   uint8_t *sfx;
   int i;

   if (ch->leftvol > 255)
      ch->leftvol = 255;
   if (ch->rightvol > 255)
      ch->rightvol = 255;

   lscale = snd_scaletable[ch->leftvol >> 3];
   rscale = snd_scaletable[ch->rightvol >> 3];
   sfx    = (uint8_t*)sc->data + ch->pos;

   for (i = 0; i < count; i++)
   {
      int data = sfx[i];
      paintbuffer[i].left  += lscale[data];
      paintbuffer[i].right += rscale[data];
   }

   ch->pos += count;
}

void SND_PaintChannelFrom16(channel_t *ch, sfxcache_t *sc, int count)
{
   int i;
   int leftvol  = ch->leftvol;
   int rightvol = ch->rightvol;
   int16_t *sfx = (int16_t*)sc->data + ch->pos;

   for (i = 0; i < count; i++)
   {
      int data              = sfx[i];
      int left              = (data * leftvol) >> 8;
      int right             = (data * rightvol) >> 8;
      paintbuffer[i].left  += left;
      paintbuffer[i].right += right;
   }

   ch->pos += count;
}
