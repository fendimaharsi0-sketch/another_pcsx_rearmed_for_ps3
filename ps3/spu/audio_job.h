/*
 * audio_job.h — job descriptor for the audio SPU program (audio_spu.c).
 *
 * The PPU writes one audio_job (128 bytes, 128-byte aligned) to main
 * memory and a word (job index + 1) to the audio SPU thread's inbound
 * mailbox. The SPU DMA's the audio_work struct (audio_mix.h) and the
 * gathered ADPCM blocks in, runs audio_do_channel_work(), DMA's the
 * audio_work back out, and writes seq to done_ea.
 *
 * DMA layout: the audio_work struct is ~21.5 KiB, so it is transferred
 * in 16 KiB chunks (the MFC limit). The gathered blocks are transferred
 * in 16 KiB chunks as well. All transfers are 16-byte aligned and sized
 * in multiples of 16. The PPU allocates work_ea / blocks_ea 128-byte
 * aligned and sized to ((sizeof(audio_work) + 15) & ~15) /
 * (nblocks * 16) respectively.
 *
 * This job is NOT part of the mdec_spu.c thread (which also runs the AA
 * jobs and needs the CI's SPU GCC 9.5 for aa_kernel.h). The audio
 * program is a separate SPU thread (the 6th usable SPU, currently idle)
 * so it compiles under the VM's spu-gcc 7.2.0 and can be host-tested.
 */

#ifndef AUDIO_JOB_H
#define AUDIO_JOB_H

#include <stdint.h>

typedef struct {
	uint64_t work_ea;   /* audio_work in main memory (DMA in, then out) */
	uint64_t blocks_ea; /* gathered ADPCM blocks, 16 bytes each */
	uint64_t done_ea;   /* seq goes here when the output is in memory */
	uint32_t nblocks;   /* number of gathered ADPCM blocks */
	uint32_t seq;
	uint32_t pad[24];   /* to 128 bytes */
} audio_job;

#endif /* AUDIO_JOB_H */
