/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE OggVorbis SOFTWARE CODEC SOURCE CODE.   *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE OggVorbis SOURCE CODE IS (C) COPYRIGHT 1994-2010             *
 * by the Xiph.Org Foundation http://www.xiph.org/                  *
 *                                                                  *
 ********************************************************************

 function: channel mapping 0 implementation
 last mod: $Id: mapping0.c 19441 2015-01-21 01:17:41Z xiphmont $

 ********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ogg/ogg.h>
#include "vorbis/codec.h"
#include "codec_internal.h"
#include "codebook.h"
#include "window.h"
#include "registry.h"
#include "psy.h"
#include "misc.h"

/* simplistic, wasteful way of doing this (unique lookup for each
   mode/submapping); there should be a central repository for
   identical lookups.  That will require minor work, so I'm putting it
   off as low priority.

   Why a lookup for each backend in a given mode?  Because the
   blocksize is set by the mode, and low backend lookups may require
   parameters from other areas of the mode/mapping */

static void mapping0_free_info(vorbis_info_mapping *i){
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)i;
  if(info){
    memset(info,0,sizeof(*info));
    _ogg_free(info);
  }
}

static void mapping0_pack(vorbis_info *vi,vorbis_info_mapping *vm,
                          oggpack_buffer *opb){
  int i;
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)vm;

  /* another 'we meant to do it this way' hack...  up to beta 4, we
     packed 4 binary zeros here to signify one submapping in use.  We
     now redefine that to mean four bitflags that indicate use of
     deeper features; bit0:submappings, bit1:coupling,
     bit2,3:reserved. This is backward compatable with all actual uses
     of the beta code. */

  if(info->submaps>1){
    oggpack_write(opb,1,1);
    oggpack_write(opb,info->submaps-1,4);
  }else
    oggpack_write(opb,0,1);

  if(info->coupling_steps>0){
    oggpack_write(opb,1,1);
    oggpack_write(opb,info->coupling_steps-1,8);

    for(i=0;i<info->coupling_steps;i++){
      oggpack_write(opb,info->coupling_mag[i],ov_ilog(vi->channels-1));
      oggpack_write(opb,info->coupling_ang[i],ov_ilog(vi->channels-1));
    }
  }else
    oggpack_write(opb,0,1);

  oggpack_write(opb,0,2); /* 2,3:reserved */

  /* we don't write the channel submappings if we only have one... */
  if(info->submaps>1){
    for(i=0;i<vi->channels;i++)
      oggpack_write(opb,info->chmuxlist[i],4);
  }
  for(i=0;i<info->submaps;i++){
    oggpack_write(opb,0,8); /* time submap unused */
    oggpack_write(opb,info->floorsubmap[i],8);
    oggpack_write(opb,info->residuesubmap[i],8);
  }
}

/* also responsible for range checking */
static vorbis_info_mapping *mapping0_unpack(vorbis_info *vi,oggpack_buffer *opb){
  int i,b;
  vorbis_info_mapping0 *info=_ogg_calloc(1,sizeof(*info));
  codec_setup_info     *ci=vi->codec_setup;
  memset(info,0,sizeof(*info));
  if(vi->channels<=0)goto err_out;

  b=oggpack_read(opb,1);
  if(b<0)goto err_out;
  if(b){
    info->submaps=oggpack_read(opb,4)+1;
    if(info->submaps<=0)goto err_out;
  }else
    info->submaps=1;

  b=oggpack_read(opb,1);
  if(b<0)goto err_out;
  if(b){
    info->coupling_steps=oggpack_read(opb,8)+1;
    if(info->coupling_steps<=0)goto err_out;
    for(i=0;i<info->coupling_steps;i++){
      /* vi->channels > 0 is enforced in the caller */
      int testM=info->coupling_mag[i]=
        oggpack_read(opb,ov_ilog(vi->channels-1));
      int testA=info->coupling_ang[i]=
        oggpack_read(opb,ov_ilog(vi->channels-1));

      if(testM<0 ||
         testA<0 ||
         testM==testA ||
         testM>=vi->channels ||
         testA>=vi->channels) goto err_out;
    }

  }

  if(oggpack_read(opb,2)!=0)goto err_out; /* 2,3:reserved */

  if(info->submaps>1){
    for(i=0;i<vi->channels;i++){
      info->chmuxlist[i]=oggpack_read(opb,4);
      if(info->chmuxlist[i]>=info->submaps || info->chmuxlist[i]<0)goto err_out;
    }
  }
  for(i=0;i<info->submaps;i++){
    oggpack_read(opb,8); /* time submap unused */
    info->floorsubmap[i]=oggpack_read(opb,8);
    if(info->floorsubmap[i]>=ci->floors || info->floorsubmap[i]<0)goto err_out;
    info->residuesubmap[i]=oggpack_read(opb,8);
    if(info->residuesubmap[i]>=ci->residues || info->residuesubmap[i]<0)goto err_out;
  }

  return info;

 err_out:
  mapping0_free_info(info);
  return(NULL);
}

#include "os.h"
#include "lpc.h"
#include "lsp.h"
#include "envelope.h"
#include "mdct.h"
#include "psy.h"
#include "scales.h"

static int mapping0_forward(vorbis_block *vb){
  vorbis_dsp_state      *vd=vb->vd;
  vorbis_info           *vi=vd->vi;
  codec_setup_info      *ci=vi->codec_setup;
  private_state         *b=vb->vd->backend_state;
  vorbis_block_internal *vbi=(vorbis_block_internal *)vb->internal;
  int                    n=vb->pcmend;
  int i,j,k;

  int    *nonzero    = alloca(sizeof(*nonzero)*vi->channels);
  float  **gmdct     = _vorbis_block_alloc(vb,vi->channels*sizeof(*gmdct));
  int    **iwork      = _vorbis_block_alloc(vb,vi->channels*sizeof(*iwork));
  int ***floor_posts = _vorbis_block_alloc(vb,vi->channels*sizeof(*floor_posts));

  float global_ampmax=vbi->ampmax;
  float *local_ampmax=alloca(sizeof(*local_ampmax)*vi->channels);
  int blocktype=vbi->blocktype;

  int modenumber=vb->W;
  vorbis_info_mapping0 *info=ci->map_param[modenumber];
  vorbis_look_psy *psy_look=b->psy+blocktype+(vb->W?2:0);

  vb->mode=modenumber;

  for(i=0;i<vi->channels;i++){
    float scale=4.f/n;
    float scale_dB;

    float *pcm     =vb->pcm[i];
    float *logfft  =pcm;

    iwork[i]=_vorbis_block_alloc(vb,n/2*sizeof(**iwork));
    gmdct[i]=_vorbis_block_alloc(vb,n/2*sizeof(**gmdct));

    scale_dB=todB(&scale) + .345; /* + .345 is a hack; the original
                                     todB estimation used on IEEE 754
                                     compliant machines had a bug that
                                     returned dB values about a third
                                     of a decibel too high.  The bug
                                     was harmless because tunings
                                     implicitly took that into
                                     account.  However, fixing the bug
                                     in the estimator requires
                                     changing all the tunings as well.
                                     For now, it's easier to sync
                                     things back up here, and
                                     recalibrate the tunings in the
                                     next major model upgrade. */

    /* window the PCM data */
    _vorbis_apply_window(pcm,b->window,ci->blocksizes,vb->lW,vb->W,vb->nW);

    /* transform the PCM data */
    /* only MDCT right now.... */
    mdct_forward(b->transform[vb->W][0],pcm,gmdct[i]);

    /* FFT yields more accurate tonal estimation (not phase sensitive) */
    drft_forward(&b->fft_look[vb->W],pcm);
    logfft[0]=scale_dB+todB(pcm)  + .345; /* + .345 is a hack; the
                                     original todB estimation used on
                                     IEEE 754 compliant machines had a
                                     bug that returned dB values about
                                     a third of a decibel too high.
                                     The bug was harmless because
                                     tunings implicitly took that into
                                     account.  However, fixing the bug
                                     in the estimator requires
                                     changing all the tunings as well.
                                     For now, it's easier to sync
                                     things back up here, and
                                     recalibrate the tunings in the
                                     next major model upgrade. */
    local_ampmax[i]=logfft[0];
    for(j=1;j<n-1;j+=2){
      float temp=pcm[j]*pcm[j]+pcm[j+1]*pcm[j+1];
      temp=logfft[(j+1)>>1]=scale_dB+.5f*todB(&temp)  + .345; /* +
                                     .345 is a hack; the original todB
                                     estimation used on IEEE 754
                                     compliant machines had a bug that
                                     returned dB values about a third
                                     of a decibel too high.  The bug
                                     was harmless because tunings
                                     implicitly took that into
                                     account.  However, fixing the bug
                                     in the estimator requires
                                     changing all the tunings as well.
                                     For now, it's easier to sync
                                     things back up here, and
                                     recalibrate the tunings in the
                                     next major model upgrade. */
      if(temp>local_ampmax[i])local_ampmax[i]=temp;
    }

    if(local_ampmax[i]>0.f)local_ampmax[i]=0.f;
    if(local_ampmax[i]>global_ampmax)global_ampmax=local_ampmax[i];

  }

  {
    float   *noise        = _vorbis_block_alloc(vb,n/2*sizeof(*noise));
    float   *tone         = _vorbis_block_alloc(vb,n/2*sizeof(*tone));

    for(i=0;i<vi->channels;i++){
      /* the encoder setup assumes that all the modes used by any
         specific bitrate tweaking use the same floor */

      int submap=info->chmuxlist[i];

      /* the following makes things clearer to *me* anyway */
      float *mdct    =gmdct[i];
      float *logfft  =vb->pcm[i];

      float *logmdct =logfft+n/2;
      float *logmask =logfft;

      vb->mode=modenumber;

      floor_posts[i]=_vorbis_block_alloc(vb,PACKETBLOBS*sizeof(**floor_posts));
      memset(floor_posts[i],0,sizeof(**floor_posts)*PACKETBLOBS);

      for(j=0;j<n/2;j++)
        logmdct[j]=todB(mdct+j)  + .345; /* + .345 is a hack; the original
                                     todB estimation used on IEEE 754
                                     compliant machines had a bug that
                                     returned dB values about a third
                                     of a decibel too high.  The bug
                                     was harmless because tunings
                                     implicitly took that into
                                     account.  However, fixing the bug
                                     in the estimator requires
                                     changing all the tunings as well.
                                     For now, it's easier to sync
                                     things back up here, and
                                     recalibrate the tunings in the
                                     next major model upgrade. */

      /* first step; noise masking.  Not only does 'noise masking'
         give us curves from which we can decide how much resolution
         to give noise parts of the spectrum, it also implicitly hands
         us a tonality estimate (the larger the value in the
         'noise_depth' vector, the more tonal that area is) */

      _vp_noisemask(psy_look,
                    logmdct,
                    noise); /* noise does not have by-frequency offset
                               bias applied yet */
      /* second step: 'all the other crap'; all the stuff that isn't
         computed/fit for bitrate management goes in the second psy
         vector.  This includes tone masking, peak limiting and ATH */

      _vp_tonemask(psy_look,
                   logfft,
                   tone,
                   global_ampmax,
                   local_ampmax[i]);

      /* third step; we offset the noise vectors, overlay tone
         masking.  We then do a floor1-specific line fit.  If we're
         performing bitrate management, the line fit is performed
         multiple times for up/down tweakage on demand. */

        _vp_offset_and_mix(psy_look,
                           noise,
                           tone,
                           1,
                           logmask,
                           mdct,
                           logmdct);

      /* this algorithm is hardwired to floor 1 for now; abort out if
         we're *not* floor1.  This won't happen unless someone has
         broken the encode setup lib.  Guard it anyway. */
      if(ci->floor_type[info->floorsubmap[submap]]!=1)return(-1);

      floor_posts[i][PACKETBLOBS/2]=
        floor1_fit(vb,b->flr[info->floorsubmap[submap]],
                   logmdct,
                   logmask);

      /* are we managing bitrate?  If so, perform two more fits for
         later rate tweaking (fits represent hi/lo) */
      if(vorbis_bitrate_managed(vb) && floor_posts[i][PACKETBLOBS/2]){
        /* higher rate by way of lower noise curve */

        _vp_offset_and_mix(psy_look,
                           noise,
                           tone,
                           2,
                           logmask,
                           mdct,
                           logmdct);

        floor_posts[i][PACKETBLOBS-1]=
          floor1_fit(vb,b->flr[info->floorsubmap[submap]],
                     logmdct,
                     logmask);

        /* lower rate by way of higher noise curve */
        _vp_offset_and_mix(psy_look,
                           noise,
                           tone,
                           0,
                           logmask,
                           mdct,
                           logmdct);

        floor_posts[i][0]=
          floor1_fit(vb,b->flr[info->floorsubmap[submap]],
                     logmdct,
                     logmask);

        /* we also interpolate a range of intermediate curves for
           intermediate rates */
        for(k=1;k<PACKETBLOBS/2;k++)
          floor_posts[i][k]=
            floor1_interpolate_fit(vb,b->flr[info->floorsubmap[submap]],
                                   floor_posts[i][0],
                                   floor_posts[i][PACKETBLOBS/2],
                                   k*65536/(PACKETBLOBS/2));
        for(k=PACKETBLOBS/2+1;k<PACKETBLOBS-1;k++)
          floor_posts[i][k]=
            floor1_interpolate_fit(vb,b->flr[info->floorsubmap[submap]],
                                   floor_posts[i][PACKETBLOBS/2],
                                   floor_posts[i][PACKETBLOBS-1],
                                   (k-PACKETBLOBS/2)*65536/(PACKETBLOBS/2));
      }
    }
  }
  vbi->ampmax=global_ampmax;

  /*
    the next phases are performed once for vbr-only and PACKETBLOB
    times for bitrate managed modes.

    1) encode actual mode being used
    2) encode the floor for each channel, compute coded mask curve/res
    3) normalize and couple.
    4) encode residue
    5) save packet bytes to the packetblob vector

  */

  /* iterate over the many masking curve fits we've created */

  {
    int **couple_bundle=alloca(sizeof(*couple_bundle)*vi->channels);
    int *zerobundle=alloca(sizeof(*zerobundle)*vi->channels);

    for(k=(vorbis_bitrate_managed(vb)?0:PACKETBLOBS/2);
        k<=(vorbis_bitrate_managed(vb)?PACKETBLOBS-1:PACKETBLOBS/2);
        k++){
      oggpack_buffer *opb=vbi->packetblob[k];

      /* start out our new packet blob with packet type and mode */
      /* Encode the packet type */
      oggpack_write(opb,0,1);
      /* Encode the modenumber */
      /* Encode frame mode, pre,post windowsize, then dispatch */
      oggpack_write(opb,modenumber,b->modebits);
      if(vb->W){
        oggpack_write(opb,vb->lW,1);
        oggpack_write(opb,vb->nW,1);
      }

      /* encode floor, compute masking curve, sep out residue */
      for(i=0;i<vi->channels;i++){
        int submap=info->chmuxlist[i];
        int *ilogmask=iwork[i];

        nonzero[i]=floor1_encode(opb,vb,b->flr[info->floorsubmap[submap]],
                                 floor_posts[i][k],
                                 ilogmask);
      }

      /* our iteration is now based on masking curve, not prequant and
         coupling.  Only one prequant/coupling step */

      /* quantize/couple */
      /* incomplete implementation that assumes the tree is all depth
         one, or no tree at all */
      _vp_couple_quantize_normalize(k,
                                    &ci->psy_g_param,
                                    psy_look,
                                    info,
                                    gmdct,
                                    iwork,
                                    nonzero,
                                    ci->psy_g_param.sliding_lowpass[vb->W][k],
                                    vi->channels);


      /* classify and encode by submap */
      for(i=0;i<info->submaps;i++){
        int ch_in_bundle=0;
        long **classifications;
        int resnum=info->residuesubmap[i];

        for(j=0;j<vi->channels;j++){
          if(info->chmuxlist[j]==i){
            zerobundle[ch_in_bundle]=0;
            if(nonzero[j])zerobundle[ch_in_bundle]=1;
            couple_bundle[ch_in_bundle++]=iwork[j];
          }
        }

        classifications=_residue_P[ci->residue_type[resnum]]->
          class(vb,b->residue[resnum],couple_bundle,zerobundle,ch_in_bundle);

        ch_in_bundle=0;
        for(j=0;j<vi->channels;j++)
          if(info->chmuxlist[j]==i)
            couple_bundle[ch_in_bundle++]=iwork[j];

        _residue_P[ci->residue_type[resnum]]->
          forward(opb,vb,b->residue[resnum],
                  couple_bundle,zerobundle,ch_in_bundle,classifications,i);
      }

      /* ok, done encoding.  Next protopacket. */
    }

  }

  return(0);
}

static int mapping0_inverse(vorbis_block *vb,vorbis_info_mapping *l){
  vorbis_dsp_state     *vd=vb->vd;
  vorbis_info          *vi=vd->vi;
  codec_setup_info     *ci=vi->codec_setup;
  private_state        *b=vd->backend_state;
  vorbis_info_mapping0 *info=(vorbis_info_mapping0 *)l;

  int                   i,j;
  long                  n=vb->pcmend=ci->blocksizes[vb->W];

  float **pcmbundle=alloca(sizeof(*pcmbundle)*vi->channels);
  int    *zerobundle=alloca(sizeof(*zerobundle)*vi->channels);

  int   *nonzero  =alloca(sizeof(*nonzero)*vi->channels);
  void **floormemo=alloca(sizeof(*floormemo)*vi->channels);

  /* recover the spectral envelope; store it in the PCM vector for now */
  for(i=0;i<vi->channels;i++){
    int submap=info->chmuxlist[i];
    floormemo[i]=_floor_P[ci->floor_type[info->floorsubmap[submap]]]->
      inverse1(vb,b->flr[info->floorsubmap[submap]]);
    if(floormemo[i])
      nonzero[i]=1;
    else
      nonzero[i]=0;
    memset(vb->pcm[i],0,sizeof(*vb->pcm[i])*n/2);
  }

  /* channel coupling can 'dirty' the nonzero listing */
  for(i=0;i<info->coupling_steps;i++){
    if(nonzero[info->coupling_mag[i]] ||
       nonzero[info->coupling_ang[i]]){
      nonzero[info->coupling_mag[i]]=1;
      nonzero[info->coupling_ang[i]]=1;
    }
  }

  /* recover the residue into our working vectors */
  for(i=0;i<info->submaps;i++){
    int ch_in_bundle=0;
    for(j=0;j<vi->channels;j++){
      if(info->chmuxlist[j]==i){
        if(nonzero[j])
          zerobundle[ch_in_bundle]=1;
        else
          zerobundle[ch_in_bundle]=0;
        pcmbundle[ch_in_bundle++]=vb->pcm[j];
      }
    }

    _residue_P[ci->residue_type[info->residuesubmap[i]]]->
      inverse(vb,b->residue[info->residuesubmap[i]],
              pcmbundle,zerobundle,ch_in_bundle);
  }

  /* channel coupling */
  for(i=info->coupling_steps-1;i>=0;i--){
    float *pcmM=vb->pcm[info->coupling_mag[i]];
    float *pcmA=vb->pcm[info->coupling_ang[i]];

    for(j=0;j<n/2;j++){
      float mag=pcmM[j];
      float ang=pcmA[j];

      if(mag>0)
        if(ang>0){
          pcmM[j]=mag;
          pcmA[j]=mag-ang;
        }else{
          pcmA[j]=mag;
          pcmM[j]=mag+ang;
        }
      else
        if(ang>0){
          pcmM[j]=mag;
          pcmA[j]=mag+ang;
        }else{
          pcmA[j]=mag;
          pcmM[j]=mag-ang;
        }
    }
  }

  /* compute and apply spectral envelope */
  for(i=0;i<vi->channels;i++){
    float *pcm=vb->pcm[i];
    int submap=info->chmuxlist[i];
    _floor_P[ci->floor_type[info->floorsubmap[submap]]]->
      inverse2(vb,b->flr[info->floorsubmap[submap]],
               floormemo[i],pcm);
  }

  /* transform the PCM data; takes PCM vector, vb; modifies PCM vector */
  /* only MDCT right now.... */
  for(i=0;i<vi->channels;i++){
    float *pcm=vb->pcm[i];
    mdct_backward(b->transform[vb->W][0],pcm,pcm);
  }

  /* all done! */
  return(0);
}

/* export hooks */
const vorbis_func_mapping mapping0_exportbundle={
  &mapping0_pack,
  &mapping0_unpack,
  &mapping0_free_info,
  &mapping0_forward,
  &mapping0_inverse
};
