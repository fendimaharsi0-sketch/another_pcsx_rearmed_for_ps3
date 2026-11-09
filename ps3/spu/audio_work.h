/* audio_work.h — the SPU audio job descriptor, shared by the mixing core
 * (audio_mix.h, run on the SPU), the SPU program (audio_spu.c), the
 * PPU-side API (spu_audio.c) and the core's SPU backend (dfsound/spu.c,
 * which builds the job from a work_item). Function-free: only the struct
 * and constants, so including it adds no code.
 *
 * Layout rules (the PPU copies fields into this verbatim):
 * - audio_adsr must keep the same bitfield layout as the PPU's ADSRInfoEx
 *   (externals.h): same field order and widths, so a memcpy is exact.
 * - audio_sample_buf must keep the same layout as the PPU's sample_buf:
 *   4 history shorts + 28 decoded shorts (32 shorts total).
 * - ch[].start / .loop are gathered-block INDICES (0-based), not RAM byte
 *   offsets. The SPU advances them locally; the PPU does not read them
 *   back (its skip pass keeps the authoritative positions).
 */
#ifndef AUDIO_WORK_H
#define AUDIO_WORK_H

#define AUDIO_NSSIZE 914            /* ((44100/50 + 32) & ~1), externals.h:66 */
#define AUDIO_MAXCHAN 24
#define AUDIO_MAXBLOCKS 4096        /* gathered ADPCM blocks per job (PPU-sized) */

/* ADSR envelope state (externals.h:80-94). Bitfield layout must match the
 * PPU struct so the PPU can copy it into the job verbatim. */
enum audio_adsr_state {
	AUDIO_ADSR_ATTACK = 0,
	AUDIO_ADSR_DECAY = 1,
	AUDIO_ADSR_SUSTAIN = 2,
	AUDIO_ADSR_RELEASE = 3,
};

typedef struct {
	unsigned char State:2;
	unsigned char AttackModeExp:1;
	unsigned char SustainModeExp:1;
	unsigned char SustainIncrease:1;
	unsigned char ReleaseModeExp:1;
	unsigned char AttackRate;
	unsigned char DecayRate;
	unsigned char SustainLevel;
	unsigned char SustainRate;
	unsigned char ReleaseRate;
	int EnvelopeVol;
	int StepCounter;
} audio_adsr;

/* ADPCM decode buffer: old[4] history + 28 decoded samples (externals.h). */
typedef union {
	short buf[4 + 28];
	struct {
		short old[4];
		short decode[28];
	};
} audio_sample_buf;

/* One job. Mirrors struct work_item (spu.c:724-749) with the SPU
 * adaptations listed in the header comment. The PPU builds this,
 * DMA's it in, the SPU fills the outputs, the PPU DMA's it back.
 * Named struct so common.h can forward-declare it. */
typedef struct audio_work {
	int ns_to;
	int ctrl;               /* spuCtrl, for the noise level */
	int decode_pos;         /* start cursor for do_decode_bufs */
	int rvb_addr;           /* 0 = no reverb accumulate */
	int interp;             /* interpolation mode 0-3 */
	unsigned int noise_count; /* dwNoiseCount in */
	unsigned int noise_val;   /* dwNoiseVal in */
	unsigned int channels_new;
	unsigned int channels_on;
	unsigned int channels_silent; /* kept for compat, not used by the SPU */
	struct {
		int spos;
		int sbpos;
		int sinc;
		int start;      /* gathered-block index */
		int loop;       /* gathered-block index */
		short vol_l;
		short vol_r;
		unsigned short ns_to;
		unsigned short bNoise:1;
		unsigned short bFMod:2;
		unsigned short bRVBActive:1;
		unsigned short bStarting:1;
		int iRawPitch;  /* for FMOD */
		audio_adsr adsr;
	} ch[AUDIO_MAXCHAN];
	int SSumLR[AUDIO_NSSIZE * 2];      /* out: stereo mix accumulator */
	int RVB[AUDIO_NSSIZE * 2];         /* out: reverb input accumulate */
	audio_sample_buf sb_thread[AUDIO_MAXCHAN + 1]; /* in+out */
	unsigned short decode_bufs[2][AUDIO_NSSIZE]; /* out: ch1/ch3 capture */
	/* noise_count / noise_val above are updated in place and read back
	 * by the PPU after the job; no separate _out fields are needed. */
} audio_work;

#endif /* AUDIO_WORK_H */
