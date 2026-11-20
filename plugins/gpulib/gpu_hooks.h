/* Optional front-end hooks into the GPU plugin, used by the PS3 front-end's
 * hardware renderer to replay drawing on the RSX. All NULL by default, in
 * which case nothing changes. */
#ifndef __GPULIB_GPU_HOOKS_H__
#define __GPULIB_GPU_HOOKS_H__

#include <stdint.h>

struct gpu_frontend_hooks {
	/* a GP0 drawing/fill/state command, before the software renderer runs it;
	 * words are little-endian as in PS1 memory. Returning nonzero for a drawing
	 * or fill command (0x02, 0x20..0x7f) means the front-end drew it and the
	 * software renderer skips it (the front-end then owns VRAM contents: see
	 * vram_read) */
	int (*cmd)(const uint32_t *list, const uint32_t *list_end);
	/* VRAM area written outside drawing commands (uploads, copies, state load) */
	void (*vram_written)(int x, int y, int w, int h);
	/* VRAM area about to be read from the core's VRAM (GPU->CPU transfer, copy
	 * source, savestate): a front-end that draws itself must bring it up to date */
	void (*vram_read)(int x, int y, int w, int h);
	/* VRAM-to-VRAM copy (GP0 80h; msb: set bit 15); nonzero: the front-end
	 * did it in its own VRAM and the core's copy is skipped (the front-end
	 * then owns the destination, see vram_read) */
	int (*vram_copy)(int sx, int sy, int dx, int dy, int w, int h, int msb);
	/* drawing state registers E1..E6 reloaded (host-endian) */
	void (*sync_ecmds)(const uint32_t *ex_regs);
	/* show the display area; return nonzero if the front-end presented it,
	 * in which case the software frame is not sent */
	int (*present)(int x, int y, int w, int h, int bgr24);
};

extern struct gpu_frontend_hooks gpu_frontend_hooks;
/* native VRAM, 1024x512 little-endian halfwords */
const uint16_t *gpulib_get_vram(void);
/* drawing state registers, ex_regs[1..6] = E1..E6 (host-endian) */
const uint32_t *gpulib_get_ex_regs(void);
/* run the software renderer on its own thread (1) or inline (0); no effect
 * without USE_ASYNC_GPU. Off while front-end hooks are installed: they are
 * called from wherever the renderer runs. */
void gpulib_set_thread_rendering(int on);
/* ask for the software renderer's enhanced-resolution shadow buffer: 0 off,
   1 = 2x (2048x1024), 2 = 1.5x (1536x768). check_mode_change() picks it up on
   the next frame */
void gpulib_set_enhancement(int mode);
/* the mode the core is actually using for this frame (0 = off, or no buffer) */
int gpulib_enhancement_active(void);
/* the 2x decision's inputs, for the log (any argument may be NULL) */
void gpulib_enhancement_state(int *en, int *act, int *has_buf,
                              int *hres, int *vres, int *src_y, int *src_y_old);
/* 2x shadow-buffer traffic since the last call: pixels and calls, up and down */
void hr_stats(unsigned long long *up_px, unsigned *up_n,
              unsigned long long *down_px, unsigned *down_n);
/* downsample the texture rects the recorded polygons will read (2x fast mode).
 * dfxvideo only; the SPU renderer calls it once per batch, before sending it. */
void gpulib_hr_flush(void);
/* the SPU polygon renderer: wait for the SPUs without submitting the recorded
 * prims, so the flush above can write native VRAM first */
void spr_wait_inflight(void);
/* a CPU->VRAM transfer is in progress (it may span several frames): its rect
 * already holds newer data in the core's VRAM than a front-end copy has */
int gpulib_pending_write(int *x, int *y, int *w, int *h);

/* time spent on the emulation thread, in timebase ticks (PowerPC only):
 * [0] GPU command processing and frame output (incl. waits for the GPU thread),
 * [1] SPU sample generation */
extern uint64_t gpulib_prof_ticks[2];
static inline uint64_t gpulib_prof_now(void)
{
#if defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
	return __builtin_ppc_get_timebase();
#else
	return 0;
#endif
}

#endif
