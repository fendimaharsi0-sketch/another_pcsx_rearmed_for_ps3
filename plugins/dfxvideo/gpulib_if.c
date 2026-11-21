/***************************************************************************
    copyright            : (C) 2001 by Pete Bernert, 2011 notaz

 ***************************************************************************/
/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version. See also the license.txt file for *
 *   additional informations.                                              *
 *                                                                         *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gpulib/gpu.h"
#include "../../include/arm_features.h"

#if defined(__GNUC__) && (__GNUC__ >= 6 || (defined(__clang_major__) && __clang_major__ >= 10))
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

#define u32 uint32_t

#define INFO_TW        0
#define INFO_DRAWSTART 1
#define INFO_DRAWEND   2
#define INFO_DRAWOFF   3

#define SHADETEXBIT(x) ((x>>24) & 0x1)
#define SEMITRANSBIT(x) ((x>>25) & 0x1)
#define PSXRGB(r,g,b) ((g<<10)|(b<<5)|r)

#define DATAREGISTERMODES unsigned short

#define DR_NORMAL        0
#define DR_VRAMTRANSFER  1

#define GPUSTATUS_READYFORVRAM        0x08000000

// byteswappings

#define SWAP16(x) __builtin_bswap16(x)
#define SWAP32(x) __builtin_bswap32(x)

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__

// big endian config
#define HOST2LE32(x) SWAP32(x)
#define HOST2BE32(x) (x)
#define LE2HOST32(x) SWAP32(x)
#define BE2HOST32(x) (x)

#define HOST2LE16(x) SWAP16(x)
#define HOST2BE16(x) (x)
#define LE2HOST16(x) SWAP16(x)
#define BE2HOST16(x) (x)

#else

// little endian config
#define HOST2LE32(x) (x)
#define HOST2BE32(x) SWAP32(x)
#define LE2HOST32(x) (x)
#define BE2HOST32(x) SWAP32(x)

#define HOST2LE16(x) (x)
#define HOST2BE16(x) SWAP16(x)
#define LE2HOST16(x) (x)
#define BE2HOST16(x) SWAP16(x)

#endif

#define GETLEs16(X) ((int16_t)GETLE16((uint16_t *)(X)))

#define GETLE16(X) LE2HOST16(*(uint16_t *)(X))
#define GETLE32_(X) LE2HOST32(*(uint32_t *)(X))
#define PUTLE16(X, Y) do{*((uint16_t *)(X))=HOST2LE16((uint16_t)(Y));}while(0)
#define PUTLE32_(X, Y) do{*((uint32_t *)(X))=HOST2LE32((uint32_t)(Y));}while(0)
#if defined(__arm__) && !defined(HAVE_ARMV6)
// for (very) old ARMs with no unaligned loads?
#define GETLE32(X) (*(uint16_t *)(X)|((uint32_t)((uint16_t *)(X))[1]<<16))
#define PUTLE32(X, Y) do{uint16_t *p_=(uint16_t *)(X);uint32_t y_=Y;p_[0]=y_;p_[1]=y_>>16;}while(0)
#else
#define GETLE32 GETLE32_
#define PUTLE32 PUTLE32_
#endif

/////////////////////////////////////////////////////////////////////////////

typedef struct VRAMLOADTTAG
{
 short x;
 short y;
 short Width;
 short Height;
 short RowsRemaining;
 short ColsRemaining;
 unsigned short *ImagePtr;
} VRAMLoad_t;

/////////////////////////////////////////////////////////////////////////////

typedef struct PSXPOINTTAG
{
 int32_t x;
 int32_t y;
} PSXPoint_t;

typedef struct PSXSPOINTTAG
{
 short x;
 short y;
} PSXSPoint_t;

typedef struct PSXRECTTAG
{
 short x0;
 short x1;
 short y0;
 short y1;
} PSXRect_t;

// linux defines for some windows stuff

#define FALSE 0
#define TRUE 1
#define BOOL unsigned short
#define LOWORD(l)           ((unsigned short)(l))
#define HIWORD(l)           ((unsigned short)(((uint32_t)(l) >> 16) & 0xFFFF))
#define max(a,b)            (((a) > (b)) ? (a) : (b))
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#define DWORD uint32_t
#ifndef __int64
#define __int64 long long int
#endif

typedef struct RECTTAG
{
 int left;
 int top;
 int right;
 int bottom;
}RECT;

/////////////////////////////////////////////////////////////////////////////

typedef struct TWINTAG
{
 PSXRect_t  Position;
 int xmask, ymask;
} TWin_t;

/////////////////////////////////////////////////////////////////////////////

typedef struct PSXDISPLAYTAG
{
 PSXPoint_t  DisplayModeNew;
 PSXPoint_t  DisplayMode;
 PSXPoint_t  DisplayPosition;
 PSXPoint_t  DisplayEnd;

 int32_t        Double;
 int32_t        Height;
 int32_t        PAL;
 int32_t        InterlacedNew;
 int32_t        Interlaced;
 int32_t        RGB24New;
 int32_t        RGB24;
 PSXSPoint_t DrawOffset;
 int32_t        Disabled;
 PSXRect_t   Range;

} PSXDisplay_t;

/////////////////////////////////////////////////////////////////////////////

// draw.c

extern int32_t           GlobalTextAddrX,GlobalTextAddrY,GlobalTextTP;
extern int32_t           GlobalTextABR,GlobalTextPAGE;
extern short          ly0,lx0,ly1,lx1,ly2,lx2,ly3,lx3;
extern long           lLowerpart;
extern BOOL           bCheckMask;
extern unsigned short sSetMask;
extern unsigned long  lSetMask;
extern short          g_m1;
extern short          g_m2;
extern short          g_m3;
extern short          DrawSemiTrans;

// prim.c

extern BOOL           bUsingTWin;
extern TWin_t         TWin;
extern void (*primTableJ[256])(unsigned char *);
extern void (*primTableSkip[256])(unsigned char *);
extern unsigned short  usMirror;
extern int            iDither;
extern uint32_t  dwCfgFixes;
extern uint32_t  dwActFixes;
extern int            iUseFixes;
extern int            iUseDither;
extern BOOL           bDoVSyncUpdate;
extern int32_t           drawX;
extern int32_t           drawY;
extern int32_t           drawW;
extern int32_t           drawH;

// gpu.h

#define OPAQUEON   10
#define OPAQUEOFF  11

#define KEY_RESETTEXSTORE 1
#define KEY_SHOWFPS       2
#define KEY_RESETOPAQUE   4
#define KEY_RESETDITHER   8
#define KEY_RESETFILTER   16
#define KEY_RESETADVBLEND 32
#define KEY_BADTEXTURES   128
#define KEY_CHECKTHISOUT  256

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define RED(x) (x & 0xff)
#define BLUE(x) ((x>>16) & 0xff)
#define GREEN(x) ((x>>8) & 0xff)
#define COLOR(x) (x & 0xffffff)
#else
#define RED(x) ((x>>24) & 0xff)
#define BLUE(x) ((x>>8) & 0xff)
#define GREEN(x) ((x>>16) & 0xff)
#define COLOR(x) SWAP32(x & 0xffffff)
#endif

PSXDisplay_t      PSXDisplay;
unsigned char  *psxVub;
unsigned short *psxVuw;
unsigned short *psxVuw_eom;

long              lGPUstatusRet;
uint32_t          lGPUInfoVals[16];

VRAMLoad_t        VRAMWrite;
VRAMLoad_t        VRAMRead;

DATAREGISTERMODES DataWriteMode;
DATAREGISTERMODES DataReadMode;

BOOL           bCheckMask = FALSE;
unsigned short sSetMask = 0;
unsigned long  lSetMask = 0;
long           lLowerpart;

#if defined(__GNUC__) && __GNUC__ >= 6
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif

/////////////////////////////////////////////////////////////////////////////
// 2x "enhanced resolution" shadow buffer. The polygons are drawn once, at twice
// the resolution, into hiVuw; the core is handed that buffer as its frame (see
// hr_get_buf). In the "fast" variant the native VRAM is not drawn at all: it is
// brought up to date lazily, only for the rects something is about to read.
/////////////////////////////////////////////////////////////////////////////

unsigned short *psxDst;            // target of polygon span writes
int             dshift = 10;       // log2(row stride of psxDst): 10 native, 11 for 2x
static unsigned short *hiVuw;      // 2048x1024 2x copy of vram

extern int spr_render_on;
extern void spr_cap_sync(void);
extern void spr_flush(void);
extern void spr_set_hivram(void *v);
extern int spr_hires_ready(void);
extern void spr_wait_inflight(void);
extern void gpulib_hr_flush(void);   /* defined below, used by the tex queue */

#define hiOn (gpu.state.enhancement_active)
#define hr_mode (gpu.state.enhancement_mode)   /* 1 = 2x, 2 = 1.5x */

/* shadow-buffer traffic, for the log: the 2x mode's cost is mostly in here */
static unsigned long long hr_stat[4];   /* 0 up px, 1 up calls, 2 down px, 3 down calls */
void hr_stats(unsigned long long *up_px, unsigned *up_n,
              unsigned long long *down_px, unsigned *down_n)
{
  if (up_px) *up_px = hr_stat[0];
  if (up_n) *up_n = (unsigned)hr_stat[1];
  if (down_px) *down_px = hr_stat[2];
  if (down_n) *down_n = (unsigned)hr_stat[3];
  hr_stat[0] = hr_stat[1] = hr_stat[2] = hr_stat[3] = 0;
}

// pixel-double a native vram rect into the 2x buffer
static void hr_up(int x,int y,int w,int h)
{
 int r,i;
 if(!hiVuw) return;
 if(spr_render_on) spr_cap_sync();
 if(x<0){w+=x;x=0;}
 if(y<0){h+=y;y=0;}
 if(x+w>1024) w=1024-x;
 if(y+h>512)  h=512-y;
 if(w<=0||h<=0) return;
 hr_stat[0] += (unsigned long long)w*h; hr_stat[1]++;
 if(hr_mode==2)
  {
   /* 1.5x: each source pixel covers one or two destination pixels (3:2), in
      both directions, and the step follows the ABSOLUTE pixel X=x+i. A
      relative index is only right for even x: for odd x it smears one pixel
      sideways and leaves the rect's last destination column unwritten. The
      shadow buffer is the picture, so both show as lines. */
   int xo=x*3/2;
   for(r=0;r<h;r++)
    {
     const unsigned short *sp=psxVuw+(y+r)*1024+x;
     int sy=(y+r)*3/2, nr=((y+r+1)*3/2)-sy, q;
     unsigned short *d=hiVuw+sy*2048+xo;
     for(q=0;q<nr;q++)
      {
       for(i=0;i<w;i++)
        {
         int X=(x+i)*3/2-xo;
         d[X]=sp[i];
         if((x+i+1)*3/2-xo>X+1) d[X+1]=sp[i];
        }
       d+=2048;
      }
    }
  }
 else
 for(r=0;r<h;r++)
  {
   const unsigned short *sp=psxVuw+(y+r)*1024+x;
   uint32_t *d0=(uint32_t *)(hiVuw+((y+r)*2)*2048+x*2);
   uint32_t *d1=d0+1024;
   for(i=0;i<w;i++)
    {
     uint32_t v=sp[i];
     v=(v<<16)|v;
     d0[i]=v; d1[i]=v;
    }
  }
}

static void hr_up_clip(int x,int y,int w,int h)
{
 int x1=x+w,y1=y+h;
 if(x<drawX) x=drawX;
 if(y<drawY) y=drawY;
 if(x1>drawW+1) x1=drawW+1;
 if(y1>drawH+1) y1=drawH+1;
 if(x1>x && y1>y) hr_up(x,y,x1-x,y1-y);
}

// ---- fast 2x: polygons skip the native pass, native vram is synced lazily ----
// The stale region is kept as 16x16-pixel blocks (64x32 of them over 1024x512),
// not as one rect. Downsampling a rect clears the blocks it covers, so a texture
// page is brought current once for the whole frame instead of once per polygon
// that textures from it; a single rect cannot even describe "two corners stale".
#define HR_BW 64
#define HR_BH 32
#define HR_BB (HR_BW * HR_BH)
#define HR_BBYTES ((HR_BB + 7) / 8)
static unsigned char hr_blk[HR_BBYTES];

/* set/clear the block range [b0,b1) - whole bytes in the middle, so a full-screen
   rect is 32 row operations instead of 2048 single-bit ones */
static void hr_blk_range(int b0,int b1,int on)
{
 if(b0<0) b0=0;
 if(b1>HR_BB) b1=HR_BB;
 if(b1<=b0) return;
 while(b0<b1 && (b0&7)) { if(on) hr_blk[b0>>3] |= (unsigned char)(1<<(b0&7)); else hr_blk[b0>>3] &= (unsigned char)~(1<<(b0&7)); b0++; }
 while(b0+8<=b1)  { hr_blk[b0>>3] = (unsigned char)(on ? 0xff : 0x00); b0+=8; }
 while(b0<b1)     { if(on) hr_blk[b0>>3] |= (unsigned char)(1<<(b0&7)); else hr_blk[b0>>3] &= (unsigned char)~(1<<(b0&7)); b0++; }
}

static void hr_blk_set(int x0,int y0,int x1,int y1)
{
 int bx0=x0>>4, bx1=(x1-1)>>4, by0=y0>>4, by1=(y1-1)>>4, by;
 if(bx1<bx0) bx1=bx0;
 if(by1<by0) by1=by0;
 for(by=by0;by<=by1;by++) hr_blk_range(by*HR_BW+bx0, by*HR_BW+bx1+1, 1);
}

static void hr_blk_clear(int x0,int y0,int x1,int y1)
{
 int bx0=x0>>4, bx1=(x1-1)>>4, by0=y0>>4, by1=(y1-1)>>4, by;
 if(bx1<bx0) bx1=bx0;
 if(by1<by0) by1=by0;
 for(by=by0;by<=by1;by++) hr_blk_range(by*HR_BW+bx0, by*HR_BW+bx1+1, 0);
}

static int hr_blk_any(void)
{
 int i;
 for(i=0;i<HR_BBYTES;i++) if(hr_blk[i]) return 1;
 return 0;
}

static void hr_down(int x0,int y0,int x1,int y1);
/* downsample, and stop tracking, every stale block inside a rect. Runs per block
   row rather than one rect: the stale region is usually sparse, and copying its
   bounding box instead would copy (and drain for) plenty of current pixels. */
static void hr_sync_blocks(int x,int y,int w,int h)
{
 int bx0,bx1,by0,by1,bx,by;
 if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;}
 if(x+w>1024) w=1024-x; if(y+h>512) h=512-y;
 if(w<=0||h<=0) return;
 bx0=x>>4; bx1=(x+w-1)>>4; by0=y>>4; by1=(y+h-1)>>4;
 for(by=by0;by<=by1;by++)
  {
   int run=-1;
   for(bx=bx0;bx<=bx1+1;bx++)
    {
     int set = (bx<=bx1) && (hr_blk[(by*HR_BW+bx)>>3] & (1 << ((by*HR_BW+bx)&7)));
     if(set && run<0) run=bx;
     if(!set && run>=0)
      {
       hr_down(run<<4, by<<4, bx<<4, (by<<4)+16);
       hr_blk_clear(run<<4, by<<4, bx<<4, (by<<4)+16);   /* current now */
       run=-1;
      }
    }
  }
}

#define hr_dirty hr_blk_any()

static void hr_dirty_add(int x0,int y0,int x1,int y1)
{
 if(x0<drawX) x0=drawX;
 if(y0<drawY) y0=drawY;
 if(x1>drawW+1) x1=drawW+1;
 if(y1>drawH+1) y1=drawH+1;
 if(x0<0) x0=0;
 if(y0<0) y0=0;
 if(x1>1024) x1=1024;
 if(y1>512) y1=512;
 if(x1<=x0||y1<=y0) return;
 hr_blk_set(x0,y0,x1,y1);
}

/* Texture rects the recorded polygons will read. hr_tex_sync only records them;
   gpulib_hr_flush downsamples them once per SPU batch, so a texture page is
   brought current once per batch instead of once per polygon. */
#define HR_TEX_MAX 16
static struct { int x, y, w, h; } hr_texq[HR_TEX_MAX];
static int hr_texn, hr_flushing;

static void hr_texq_push(int x,int y,int w,int h)
{
 int i;
 for(i=0;i<hr_texn;i++)
  if(hr_texq[i].x==x && hr_texq[i].y==y && hr_texq[i].w==w && hr_texq[i].h==h) return;
 if(hr_texn>=HR_TEX_MAX) gpulib_hr_flush();   /* too many distinct pages: do them now */
 hr_texq[hr_texn].x=x; hr_texq[hr_texn].y=y;
 hr_texq[hr_texn].w=w; hr_texq[hr_texn].h=h;
 hr_texn++;
}

// keep the native vram current for a rect of the enhanced image. Both modes take
// the nearest source pixel (2x: every even one; 1.5x: index*3/2 rounded down).
static void hr_down(int x0,int y0,int x1,int y1)
{
 int r,i;
 /* inside a flush the caller has already waited for the SPUs (gpulib_hr_flush) */
 if(spr_render_on && !hr_flushing) spr_cap_sync();
 hr_stat[2] += (unsigned long long)(x1-x0)*(y1-y0); hr_stat[3]++;
 if(hr_mode==2)
  for(r=y0;r<y1;r++)
   {
    unsigned short *d=psxVuw+r*1024;
    const unsigned short *sp=hiVuw+(r*3/2)*2048;
    for(i=x0;i<x1;i++) d[i]=sp[i*3/2];
   }
 else
 for(r=y0;r<y1;r++)
  {
   unsigned short *d=psxVuw+r*1024;
   const unsigned short *sp=hiVuw+(r*2)*2048;
   for(i=x0;i<x1;i++) d[i]=sp[i*2];
  }
}

static void hr_sync_all(void)
{
 if(!hr_blk_any()) return;
 hr_sync_blocks(0,0,1024,512);
 memset(hr_blk,0,sizeof hr_blk);
}

static void hr_sync(int x,int y,int w,int h)
{
 if(!hr_blk_any()) return;
 hr_sync_blocks(x,y,w,h);
}

/* Downsample the texture rects the recorded polygons will read. Called from
   spu_poly's submit() after the SPUs are off VRAM and before the batch goes out,
   so a texture page costs one drain and one downsample per batch instead of one
   per polygon. Re-entrant calls (a flush from inside hr_down) are ignored. */
void gpulib_hr_flush(void)
{
 int i;
 if(hr_flushing || !hr_texn) return;
 hr_flushing = 1;
 spr_wait_inflight();
 for(i=0;i<hr_texn;i++)
  hr_sync(hr_texq[i].x,hr_texq[i].y,hr_texq[i].w,hr_texq[i].h);
 hr_texn = 0;
 hr_flushing = 0;
}

// called before commands that read the native destination/vram
static void hr_pre_cmd(unsigned cmd, const uint32_t *list)
{
 const short *sl=(const short *)list;
 int x,y,w,h;
 if(!hr_dirty) return;
 if(cmd>=0x40 && cmd<0x60) { if((cmd&2)||bCheckMask) hr_sync_all(); return; }
 if(cmd>=0x60 && cmd<0x80)
  {
   if(!((cmd&2)||bCheckMask)) return;
   x=GETLEs16(&sl[2]); y=GETLEs16(&sl[3]);
   x=((int)x<<21)>>21; y=((int)y<<21)>>21;
   x+=PSXDisplay.DrawOffset.x; y+=PSXDisplay.DrawOffset.y;
   switch((cmd>>3)&3)
    {
     case 0:
      if(cmd&4){w=GETLEs16(&sl[6])&0x3ff;h=GETLEs16(&sl[7])&0x1ff;}
      else     {w=GETLEs16(&sl[4])&0x3ff;h=GETLEs16(&sl[5])&0x1ff;}
      break;
     case 1: w=h=1; break;
     case 2: w=h=8; break;
     default: w=h=16; break;
    }
   hr_sync(x,y,w,h);
   return;
  }
 if((cmd&0xe0)==0xc0 || (cmd&0xe0)==0x80) hr_sync_all();
}

static void *hr_get_buf(int *x,int *y,int *w,int *h,int *vram_h)
{
 /* both modes draw into the same 2048-pixel-stride buffer; 1.5x only fills the
    top-left 1536x768 of it, so the frame handed over is 3/2 the native size */
 if(hr_mode==2)
  {
   *x=*x*3/2; *y=*y*3/2; *w=*w*3/2; *h=*h*3/2; *vram_h=768;
  }
 else
  {
   *x*=2; *y*=2; *w*=2; *h*=2; *vram_h=1024;
  }
 return hiVuw;
}

#include "soft.c"
#include "prim.c"

/////////////////////////////////////////////////////////////////////////////

static void set_vram(void *vram)
{
 psxVub=vram;
 psxVuw=(unsigned short *)psxVub;
 psxVuw_eom=psxVuw+1024*512;                           // pre-calc of end of vram
 psxDst=psxVuw; dshift=10;
}

int renderer_init(void)
{
 set_vram(gpu.vram);
 if(!hiVuw) { hiVuw=(unsigned short *)memalign(128,2048*1024*2); if(hiVuw) memset(hiVuw,0,2048*1024*2); }
 spr_set_hivram(hiVuw);
 gpu.get_enhancement_bufer = hiVuw ? hr_get_buf : NULL;
 psxDst=psxVuw; dshift=10;

 PSXDisplay.RGB24        = FALSE;                      // init some stuff
 PSXDisplay.Interlaced   = FALSE;
 PSXDisplay.DrawOffset.x = 0;
 PSXDisplay.DrawOffset.y = 0;
 PSXDisplay.DisplayMode.x= 320;
 PSXDisplay.DisplayMode.y= 240;
 PSXDisplay.Disabled     = FALSE;
 PSXDisplay.Range.x0=0;
 PSXDisplay.Range.x1=0;
 PSXDisplay.Double = 1;

 DataWriteMode = DR_NORMAL;
 lGPUstatusRet = 0x14802000;

 return 0;
}

void renderer_finish(void)
{
}

void renderer_notify_screen_change(const struct psx_gpu_screen *screen)
{
}

#include "../gpulib/gpu_timing.h"
#include "../gpulib/gpu_hooks.h"
extern const unsigned char cmd_lengths[256];

int renderer_do_cmd_list(uint32_t *list, int list_len, uint32_t *ex_regs,
 int *cycles_sum_out, int *cycles_last, int *last_cmd)
{
  int cpu_cycles_sum = 0, cpu_cycles = *cycles_last;
  unsigned int cmd = 0, len;
  uint32_t *list_start = list;
  uint32_t *list_end = list + list_len;

  /* leaving 2x: the native vram must be complete before anything reads it */
  if (!hiOn && hr_dirty) hr_sync_all();
  /* in 2x the PPU draws into the shadow buffer unless the SPU renderer took the
     polygons; either way the two must not be in flight at the same time */
  if (hiOn && !(spr_render_on && spr_hires_ready())) spr_cap_sync();

  for (; list < list_end; list += 1 + len)
  {
    short *slist = (void *)list;
    cmd = GETLE32(list) >> 24;
    len = cmd_lengths[cmd];
    if (list + 1 + len > list_end) {
      cmd = -1;
      break;
    }

    /* the SPU polygon renderer draws into the core's VRAM, so the PPU must not
       touch it (fills, image transfers, the VRAM copy, and in 2x mode the
       sprites/tiles, which read the lazily synced native area) while the SPUs
       are still on it */
    if (spr_render_on && (cmd == 0x02 || (cmd >= 0x40 && cmd < 0x60) ||
                          (cmd >= 0x80 && cmd < 0xe0) || (hiOn && cmd >= 0x60 && cmd < 0x80)))
      spr_cap_sync();

    /* a command that reads the native destination needs the 2x -> native
       downsample for its rect first */
    if (hiOn) hr_pre_cmd(cmd, list);

#ifndef TEST
    if (0x80 <= cmd && cmd < 0xe0)
      break; // image i/o, forward to upper layer
    else if ((cmd & 0xf8) == 0xe0)
      ex_regs[cmd & 7] = GETLE32(list);
#endif

    /* the length and cycle bookkeeping below still runs for commands the
       front-end drew itself; only the software drawing is skipped */
    if (!(gpu_frontend_hooks.cmd && (cmd == 0x02 || (cmd >= 0x20 && cmd < 0x80) || (cmd >= 0xe1 && cmd <= 0xe6))
          && gpu_frontend_hooks.cmd(list, list_end) && cmd < 0x80))
      primTableJ[cmd]((void *)list);
    else if ((cmd & 0xe4) == 0x24)
      /* textured polygon drawn by the front end: keep its texture-page side effect
         (GPUSTAT bits 0-8, the state later sprites and E1 syncs start from) */
      UpdateGlobalTP((unsigned short)(GETLE32(&list[4 + ((cmd >> 4) & 1)]) >> 16));

    switch(cmd)
    {
      case 0x48 ... 0x4F:
      {
        u32 num_vertexes = 2;
        u32 *list_position = &(list[3]);

        while(1)
        {
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          if(list_position >= list_end) {
            cmd = -1;
            goto breakloop;
          }

          if((*list_position & HOST2LE32(0xf000f000)) == HOST2LE32(0x50005000))
            break;

          list_position++;
          num_vertexes++;
        }

        len += (num_vertexes - 2);
        break;
      }

      case 0x58 ... 0x5F:
      {
        u32 num_vertexes = 2;
        u32 *list_position = &(list[4]);

        while(1)
        {
          gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));

          if(list_position >= list_end) {
            cmd = -1;
            goto breakloop;
          }

          if((*list_position & HOST2LE32(0xf000f000)) == HOST2LE32(0x50005000))
            break;

          list_position += 2;
          num_vertexes++;
        }

        len += (num_vertexes - 2) * 2;
        break;
      }

#ifdef TEST
      case 0xA0:          //  sys -> vid
      {
        u32 load_width = LE2HOST16(slist[4]);
        u32 load_height = LE2HOST16(slist[5]);
        u32 load_size = load_width * load_height;

        len += load_size / 2;
        break;
      }
#endif

      // timing
      case 0x02:
        gput_sum(cpu_cycles_sum, cpu_cycles,
            gput_fill(LE2HOST16(slist[4]) & 0x3ff, LE2HOST16(slist[5]) & 0x1ff));
        break;
      case 0x20 ... 0x23: gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base());    break;
      case 0x24 ... 0x27: gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_t());  break;
      case 0x28 ... 0x2B: gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base());    break;
      case 0x2C ... 0x2F: gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_t());  break;
      case 0x30 ... 0x33: gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_g());  break;
      case 0x34 ... 0x37: gput_sum(cpu_cycles_sum, cpu_cycles, gput_poly_base_gt()); break;
      case 0x38 ... 0x3B: gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_g());  break;
      case 0x3C ... 0x3F: gput_sum(cpu_cycles_sum, cpu_cycles, gput_quad_base_gt()); break;
      case 0x40 ... 0x47: gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));        break;
      case 0x50 ... 0x57: gput_sum(cpu_cycles_sum, cpu_cycles, gput_line(0));        break;
      case 0x60 ... 0x63:
        gput_sum(cpu_cycles_sum, cpu_cycles,
            gput_sprite(LE2HOST16(slist[4]) & 0x3ff, LE2HOST16(slist[5]) & 0x1ff));
        break;
      case 0x64 ... 0x67:
        gput_sum(cpu_cycles_sum, cpu_cycles,
            gput_sprite(LE2HOST16(slist[6]) & 0x3ff, LE2HOST16(slist[7]) & 0x1ff));
        break;
      case 0x68 ... 0x6B: gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(1, 1));   break;
      case 0x70 ... 0x73:
      case 0x74 ... 0x77: gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(8, 8));   break;
      case 0x78 ... 0x7B:
      case 0x7C ... 0x7F: gput_sum(cpu_cycles_sum, cpu_cycles, gput_sprite(16, 16)); break;
    }
  }

breakloop:
  ex_regs[1] &= ~0x1ff;
  ex_regs[1] |= lGPUstatusRet & 0x1ff;

  *cycles_sum_out += cpu_cycles_sum;
  *cycles_last = cpu_cycles;
  *last_cmd = cmd;
  spr_flush();           /* hand over the tail of this command list */
  return list - list_start;
}

void renderer_sync_ecmds(uint32_t *ecmds_)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  // the funcs below expect LE
  uint32_t i, ecmds[8];
  for (i = 1; i <= 6; i++)
    ecmds[i] = HTOLE32(ecmds_[i]);
#else
  uint32_t *ecmds = ecmds_;
#endif
  cmdTexturePage((unsigned char *)&ecmds[1]);
  cmdTextureWindow((unsigned char *)&ecmds[2]);
  cmdDrawAreaStart((unsigned char *)&ecmds[3]);
  cmdDrawAreaEnd((unsigned char *)&ecmds[4]);
  cmdDrawOffset((unsigned char *)&ecmds[5]);
  cmdSTP((unsigned char *)&ecmds[6]);
  if (gpu_frontend_hooks.sync_ecmds)
    gpu_frontend_hooks.sync_ecmds(ecmds_);
}

void renderer_update_caches(int x, int y, int w, int h, int state_changed)
{
  /* the core just wrote the native VRAM (texture upload, copy, state load): the
     2x buffer must mirror it, or the next hr_tex_sync would downsample a stale
     hiVuw rect over the fresh texture and the texture would disappear */
  if (hiOn) hr_up(x, y, w, h);
  if (gpu_frontend_hooks.vram_written)
    gpu_frontend_hooks.vram_written(x, y, w, h);
}

void renderer_flush_queues(void)
{
}

void renderer_set_interlace(int enable, int is_odd)
{
}

#include "../../frontend/plugin_lib.h"

void renderer_set_config(const struct rearmed_cbs *cbs)
{
 iUseDither = cbs->dithering;
 dwActFixes = cbs->gpu_peops.dwActFixes;
 if (cbs->pl_set_gpu_caps)
  cbs->pl_set_gpu_caps(0);
 set_vram(gpu.vram);
}

// vim:ts=2:shiftwidth=2:expandtab
