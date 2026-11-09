/*
 * audio_spu.c — SPU program: PS1 SPU (sound) channel mixing.
 *
 * A persistent SPU thread. The PPU writes an audio_job (audio_job.h) to
 * main memory and a word (job index + 1) to this thread's inbound
 * mailbox. The SPU DMA's the audio_work struct (audio_mix.h) and the
 * gathered ADPCM blocks in, runs audio_do_channel_work() (candidate B:
 * voice mixing only, reverb stays on the PPU), DMA's the audio_work
 * back out, and writes the job's seq to done_ea. A zero mailbox word
 * ends the thread.
 *
 * This is a SEPARATE thread from mdec_spu.c (which also runs the AA
 * jobs and needs the CI's SPU GCC 9.5 for aa_kernel.h). It uses the 6th
 * usable SPU, which is otherwise idle. It compiles under the VM's
 * spu-gcc 7.2.0 and can be host-tested (-DHOST_TEST, memcpy for DMA,
 * every transfer checked against the MFC's alignment and size rules).
 */

#include <stdint.h>
#include <string.h>
#include "audio_job.h"
#include "audio_mix.h"

#ifdef HOST_TEST
#include <assert.h>
#include <stdio.h>
/* Mirror the real MFC rules: a DMA is 1/2/4/8/16 bytes, or a multiple
 * of 16 up to 16384; both addresses are aligned to the transfer size
 * (16-byte aligned for the large case). The done word is a 4-byte DMA,
 * so the small-size case is real, not a test artifact. */
static void dma_check(const void *ls, uint64_t ea, uint32_t size)
{
	assert(size > 0 && size <= 16384);
	if (size <= 16) {
		assert(size == 1 || size == 2 || size == 4 || size == 8 ||
		       size == 16);
		assert(((uintptr_t)ls & (size - 1)) == 0 &&
		       (ea & (size - 1)) == 0);
	} else {
		assert(size % 16 == 0);
		assert(((uintptr_t)ls & 15) == 0 && (ea & 15) == 0);
	}
}
#define DMA_GET(ls, ea, size, tag) (dma_check(ls, ea, size), memcpy((ls), (void *)(uintptr_t)(ea), (size)))
#define DMA_PUT(ls, ea, size, tag) (dma_check(ls, ea, size), memcpy((void *)(uintptr_t)(ea), (ls), (size)))
#define DMA_WAIT(mask) ((void)0)
#else
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>
#define DMA_GET(ls, ea, size, tag) mfc_get((ls), (ea), (size), (tag), 0, 0)
#define DMA_PUT(ls, ea, size, tag) mfc_put((ls), (ea), (size), (tag), 0, 0)
#define DMA_WAIT(mask) do { mfc_write_tag_mask(mask); spu_mfcstat(MFC_TAG_UPDATE_ALL); } while (0)
#endif

#define TAG_IN   1
#define TAG_OUT  2
#define TAG_DONE 4

/* Local store. audio_work is ~21.5 KiB; blocks hold up to
 * AUDIO_MAXBLOCKS 16-byte ADPCM blocks (64 KiB worst case). */
static audio_work work __attribute__((aligned(128)));
static unsigned char blocks[AUDIO_MAXBLOCKS * 16] __attribute__((aligned(128)));
static int ls_ChanBuf[AUDIO_NSSIZE] __attribute__((aligned(128)));
static int ls_iFMod[AUDIO_NSSIZE] __attribute__((aligned(128)));

/* Copy size bytes between local store and main memory in 16 KiB chunks
 * (the MFC limit), in the given direction. */
static void dma_chunked(void *ls, uint64_t ea, uint32_t size, int to_spu)
{
	uint32_t off = 0;
	while (off < size) {
		uint32_t chunk = size - off;
		if (chunk > 16384)
			chunk = 16384;
		if (to_spu)
			DMA_GET((char *)ls + off, ea + off, chunk, TAG_IN);
		else
			DMA_PUT((char *)ls + off, ea + off, chunk, TAG_OUT);
		off += chunk;
	}
	DMA_WAIT(1 << (to_spu ? TAG_IN : TAG_OUT));
}

static void process_audio(uint64_t job_ea)
{
	audio_job job;
	uint32_t work_size;

	DMA_GET(&job, job_ea, sizeof job, TAG_IN);
	DMA_WAIT(1 << TAG_IN);

	/* audio_work, rounded up to a 16-byte multiple (the PPU allocates
	 * that many bytes). */
	work_size = (uint32_t)((sizeof work + 15) & ~15u);
	dma_chunked(&work, job.work_ea, work_size, 1);

	if (job.nblocks)
		dma_chunked(blocks, job.blocks_ea, job.nblocks * 16, 1);

	memset(ls_ChanBuf, 0, sizeof ls_ChanBuf);
	memset(ls_iFMod, 0, sizeof ls_iFMod);
	audio_do_channel_work(&work, blocks, ls_iFMod, ls_ChanBuf);

	dma_chunked(&work, job.work_ea, work_size, 0);

	/* Signal completion: the PPU polls this word. */
	{
		uint32_t seq = job.seq;
		DMA_PUT(&seq, job.done_ea, sizeof seq, TAG_DONE);
		DMA_WAIT(1 << TAG_DONE);
	}
}

#ifdef HOST_TEST
void host_spu_mailbox(uint64_t job_ea, uint32_t cmd) { (void)cmd; process_audio(job_ea); }
#else
int main(uint64_t job_ea, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	(void)arg1; (void)arg2; (void)arg3;
	for (;;) {
		uint32_t cmd = spu_read_in_mbox();
		if (!cmd)
			break;
		/* cmd is the job index + 1; the job array base is arg0. */
		process_audio(job_ea + (uint64_t)(cmd - 1) * sizeof(audio_job));
	}
	spu_thread_exit(0);
	return 0;
}
#endif
