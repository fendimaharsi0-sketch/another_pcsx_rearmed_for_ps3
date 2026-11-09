/*
 * audio_mix.h — PS1 SPU (dfsound) channel mixing, ported for a PS3 SPU.
 *
 * This is a self-contained port of the worker-side mixer from
 * plugins/dfsound/spu.c: do_channel_work() + the helpers it calls
 * (ADPCM decode, interpolation, ADSR, stereo mix, noise). It is written
 * so it compiles both under spu-gcc and under a host gcc (-DHOST_TEST)
 * for the hosttest/spuaudio comparison against the PPU mixer.
 *
 * Candidate B split: voice mixing lives here; REVERBDo stays on the PPU.
 * The SPU accumulates into RVB and returns it; the PPU runs the reverb
 * pass after reaping the job.
 *
 * Adaptations vs the PPU code (see SPU_AUDIO_DESIGN.md):
 * - Sample data comes from a PPU-gathered buffer of 16-byte ADPCM blocks,
 *   not from SPU RAM by raw pointer. work->ch[].start / .loop are
 *   gathered-block INDICES (0-based), not RAM byte offsets. The SPU
 *   advances them locally; the PPU does not read them back (its skip
 *   pass keeps the authoritative positions).
 * - iRawPitch is carried per channel in the job (the PPU work_item does
 *   not have it; FMOD needs it).
 * - Interpolation mode, noise state (dwNoiseCount/dwNoiseVal) and the
 *   reverb flag are carried in the job header.
 * - do_decode_bufs writes the ch1/ch3 capture samples to a linear
 *   output buffer (decode_bufs); the PPU places them into the SPU-RAM
 *   0x800/0xC00 ring (cursor = decode_pos).
 * - SSumLR and RVB are job outputs; sb_thread is in+out; the noise
 *   state is returned in noise_count_out / noise_val_out.
 * - Endianness: the PPU byte-swaps gathered blocks during the gather
 *   (SPU RAM is little-endian, PPU/SPU are big-endian), so this file
 *   reads blocks as native 16-bit values. The host test feeds blocks
 *   already swapped the same way.
 */

#ifndef AUDIO_MIX_H
#define AUDIO_MIX_H

#include <string.h>
#include "audio_work.h"             /* audio_work, audio_adsr, audio_sample_buf */

#ifndef AUDIO_INLINE
#define AUDIO_INLINE static inline
#endif

#define AUDIO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define AUDIO_MAX(a, b) ((a) > (b) ? (a) : (b))
#define AUDIO_MIN(a, b) ((a) < (b) ? (a) : (b))

AUDIO_INLINE void audio_ssat32_to_16(int *v)
{
	if (*v < -32768) *v = -32768;
	else if (*v > 32767) *v = 32767;
}

/* The job descriptor lives in audio_work.h (shared with the core's SPU
 * backend, which builds the job from a work_item). */

/* Context passed to the decode callback: the gathered blocks + the FMOD
 * scratch buffer + the job (for per-channel iRawPitch). */
typedef struct {
	const unsigned char *blocks; /* 16 bytes per block, PPU-gathered */
	int *iFMod;                  /* NSSIZE ints, local-store scratch */
	audio_work *work;            /* the job; start/loop advance in place */
} audio_ctx;

#include "gauss_i.h"

/* ------------------------------------------------------------------ */
/* ADPCM block decode (spu.c:197-237, decode_block_data)               */
/* ------------------------------------------------------------------ */

AUDIO_INLINE void audio_decode_block_data(short *dest,
	const unsigned char *src, int predict_nr, int shift_factor)
{
	static const int f[16][2] = {
		{    0,  0  },
		{   60,  0  },
		{  115, -52 },
		{   98, -55 },
		{  122, -60 }
	};
	int nSample;
	int fa, s_1, s_2, d, s;

	s_1 = dest[27];
	s_2 = dest[26];

	for (nSample = 0; nSample < 28; src++) {
		d = (int)*src;
		s = (int)(signed short)((d & 0x0f) << 12);

		fa  = s >> shift_factor;
		fa += ((s_1 * f[predict_nr][0]) >> 6) + ((s_2 * f[predict_nr][1]) >> 6);
		audio_ssat32_to_16(&fa);
		s_2 = s_1; s_1 = fa;

		dest[nSample++] = (short)fa;

		s = (int)(signed short)((d & 0xf0) << 8);
		fa  = s >> shift_factor;
		fa += ((s_1 * f[predict_nr][0]) >> 6) + ((s_2 * f[predict_nr][1]) >> 6);
		audio_ssat32_to_16(&fa);
		s_2 = s_1; s_1 = fa;

		dest[nSample++] = (short)fa;
	}
}

/* Decode one gathered block into sb. Reads the block at gathered index
 * work->ch[ch].start, advances it (looping to .loop on flag 1, or to
 * itself on flag 4). Mirrors decode_block_work (spu.c:759-787) but over
 * the gathered buffer instead of SPU RAM. */
static int audio_decode_block_work(void *ctx, int ch, audio_sample_buf *sb)
{
	const audio_ctx *c = (const audio_ctx *)ctx;
	audio_work *work = c->work; /* local copy; PPU never reads start/loop back */
	const unsigned char *blk;
	int predict_nr, shift_factor, flags;
	int start = work->ch[ch].start;
	int loop = work->ch[ch].loop;

	blk = c->blocks + (size_t)start * 16;
	predict_nr = blk[0];
	shift_factor = predict_nr & 0xf;
	predict_nr >>= 4;

	/* keep the last 4 decoded samples as history for the next block */
	sb->old[0] = sb->decode[28 - 4];
	sb->old[1] = sb->decode[28 - 3];
	sb->old[2] = sb->decode[28 - 2];
	sb->old[3] = sb->decode[28 - 1];
	audio_decode_block_data(sb->decode, blk + 2, predict_nr, shift_factor);

	flags = blk[1];
	if (flags & 4)
		loop = start;               /* loop address = this block */

	start += 1;                     /* next gathered block (not +16: indices) */

	if (flags & 1)                  /* 1: stop/loop */
		start = loop;

	work->ch[ch].start = start;
	work->ch[ch].loop = loop;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Interpolation (spu.c:150-195, 397-403)                              */
/* ------------------------------------------------------------------ */

AUDIO_INLINE int audio_interp_gauss_get(const short *sb, int sbpos, int spos)
{
	int vl = (spos >> 6) & ~3;
	int vr;
	vr  = gauss[vl + 0] * sb[sbpos - 4];
	vr += gauss[vl + 1] * sb[sbpos - 3];
	vr += gauss[vl + 2] * sb[sbpos - 2];
	vr += gauss[vl + 3] * sb[sbpos - 1];
	return vr >> 15;
}

AUDIO_INLINE int audio_interp_cubic_get(const short *sb, int sbpos, int spos)
{
#define audio_gval(x) sb[sbpos - 4 + (x)]
	int xd = (spos >> 1) + 1;
	int fa;

	fa  = audio_gval(3) - 3 * audio_gval(2) + 3 * audio_gval(1) - audio_gval(0);
	fa *= (xd - (2 << 15)) / 6;
	fa >>= 15;
	fa += audio_gval(2) - audio_gval(1) - audio_gval(1) + audio_gval(0);
	fa *= (xd - (1 << 15)) >> 1;
	fa >>= 15;
	fa += audio_gval(1) - audio_gval(0);
	fa *= xd;
	fa >>= 15;
	fa = fa + audio_gval(0);
	return fa;
#undef audio_gval
}

#define audio_interp_simple_get(buf, sbpos, spos) \
	((buf[(sbpos) - 2] * (0x10000 - (spos)) + buf[(sbpos) - 1] * (spos)) >> 16)

/* FMOD frequency change (spu.c:156-165). */
AUDIO_INLINE int audio_FModChangeFrequency(int pitch, int ns, int *fmod_buf)
{
	pitch = (signed short)pitch;
	pitch = ((32768 + fmod_buf[ns]) * pitch) >> 15;
	pitch &= 0xffff;
	if (pitch > 0x3fff)
		pitch = 0x3fff;

	fmod_buf[ns] = 0;

	return pitch << 4;
}

/* The per-sample loop, generated for each interpolation mode.
 * Mirrors make_do_samples (spu.c:354-375). */
#define audio_make_do_samples(name, fmod_code, interp_start, interp_get, interp_update) \
static int name(int *dst, \
	int (*decode_f)(void *ctx, int ch, audio_sample_buf *), void *ctx, \
	int ch, int ns_to, audio_sample_buf *sb, \
	int sinc, int *spos_p, int *sbpos_p) \
{ \
	short *buf = sb->buf; \
	int sboffset = 4; \
	int sbpos = (*sbpos_p) + sboffset; \
	int spos = *spos_p; \
	int ns, d, ret = ns_to; \
	interp_start; \
	for (ns = 0; ns < ns_to; ns++) { \
		dst[ns] = interp_get(buf, sbpos, spos); \
		fmod_code; \
		spos += sinc; \
		if (spos < 0x10000) \
			continue; \
		sbpos += spos >> 16; \
		spos = spos & 0xffff; \
		if (AUDIO_UNLIKELY(sbpos >= 28 + sboffset)) { \
			sbpos -= 28; \
			d = decode_f(ctx, ch, sb); \
			if (d && ns < ret) \
				ret = ns; \
		} \
		interp_update; \
	} \
	*spos_p = spos; \
	*sbpos_p = sbpos - sboffset; \
	return ret; \
}

#define audio_interp_none_start \
	short fa = buf[sbpos]
#define audio_interp_none_get(...) fa
#define audio_interp_none_update fa = buf[sbpos]

audio_make_do_samples(audio_do_samples_nointerp, ,
	audio_interp_none_start, audio_interp_none_get, audio_interp_none_update)
audio_make_do_samples(audio_do_samples_simple, ,
	, audio_interp_simple_get, )
audio_make_do_samples(audio_do_samples_gauss, ,
	, audio_interp_gauss_get, )
audio_make_do_samples(audio_do_samples_cubic, ,
	, audio_interp_cubic_get, )
audio_make_do_samples(audio_do_samples_fmod,
	sinc = audio_FModChangeFrequency(((audio_ctx *)ctx)->work->ch[ch].iRawPitch, ns, ((audio_ctx *)ctx)->iFMod),
	, audio_interp_gauss_get, )

/* Dispatch (spu.c:418-437, do_samples_adpcm). */
static int audio_do_samples_adpcm(int *dst,
	int (*decode_f)(void *ctx, int ch, audio_sample_buf *), void *ctx,
	int ch, int ns_to, int fmod, audio_sample_buf *sb, int sinc,
	int *spos, int *sbpos, int interp)
{
	if (fmod == 1)
		return audio_do_samples_fmod(dst, decode_f, ctx, ch, ns_to, sb, sinc, spos, sbpos);
	if (fmod)
		interp = 2;
	switch (interp) {
	case 0:
		return audio_do_samples_nointerp(dst, decode_f, ctx, ch, ns_to, sb, sinc, spos, sbpos);
	case 1:
		return audio_do_samples_simple(dst, decode_f, ctx, ch, ns_to, sb, sinc, spos, sbpos);
	default:
		return audio_do_samples_gauss(dst, decode_f, ctx, ch, ns_to, sb, sinc, spos, sbpos);
	case 3:
		return audio_do_samples_cubic(dst, decode_f, ctx, ch, ns_to, sb, sinc, spos, sbpos);
	}
}

/* ------------------------------------------------------------------ */
/* Noise (spu.c:485-530, do_lsfr_samples)                              */
/* ------------------------------------------------------------------ */

static void audio_do_lsfr_samples(int *dst, int ns_to, int ctrl,
	unsigned int *dwNoiseCount, unsigned int *dwNoiseVal)
{
	unsigned int counter = *dwNoiseCount;
	unsigned int val = *dwNoiseVal;
	unsigned int level, shift, bit;
	int ns;

	level = (ctrl >> 10) & 0x0f;
	level = 0x8000 >> level;

	for (ns = 0; ns < ns_to; ns++) {
		counter += 2;
		if (counter >= level) {
			counter -= level;
			shift = (val >> 10) & 0x1f;
			bit = (0x69696969 >> shift) & 1;
			bit ^= (val >> 15) & 1;
			val = (val << 1) | bit;
		}

		dst[ns] = (signed short)val;
	}

	*dwNoiseCount = counter;
	*dwNoiseVal = val;
}

/* ------------------------------------------------------------------ */
/* ADSR envelope (adsr.c:117-240, adsr_do + adsr_do_inner)             */
/* ------------------------------------------------------------------ */

enum audio_adsr_type {
	audio_adsr_noexp,
	audio_adsr_expinc,
	audio_adsr_expdec,
};

AUDIO_INLINE int audio_adsr_do_inner(int *samples, int ns, int ns_to,
	int *EnvelopeVol_, int env_step, int *cycles_, int cycles_step,
	int do_samples, enum audio_adsr_type type)
{
	int EnvelopeVol = *EnvelopeVol_;
	int cycles = *cycles_;
	int cycles_step_exp;
	switch (type) {
	case audio_adsr_noexp:
		if (cycles_step == 0x8000) {
			for (; ns < ns_to && !(EnvelopeVol & 0x8000); ns++, EnvelopeVol += env_step)
				if (do_samples)
					samples[ns] = samples[ns] * EnvelopeVol >> 15;
		} else {
			while (ns < ns_to) {
				if (do_samples)
					samples[ns] = samples[ns] * EnvelopeVol >> 15;
				ns++;
				if ((cycles += cycles_step) & 0x8000) {
					cycles = 0;
					EnvelopeVol += env_step;
					if (EnvelopeVol & 0x8000)
						break;
				}
			}
		}
		break;

	case audio_adsr_expinc:
		cycles_step_exp = cycles_step;
		if (EnvelopeVol >= 0x6000)
			cycles_step_exp = AUDIO_MAX(1, (cycles_step >> 2));
		while (ns < ns_to) {
			if (do_samples)
				samples[ns] = samples[ns] * EnvelopeVol >> 15;
			ns++;
			if ((cycles += cycles_step_exp) & 0x8000) {
				cycles = 0;
				cycles_step_exp = cycles_step;
				if (EnvelopeVol >= 0x6000)
					cycles_step_exp = AUDIO_MAX(1, (cycles_step >> 2));
				EnvelopeVol += env_step;
				if (EnvelopeVol >= 0x8000)
					break;
			}
		}
		break;

	case audio_adsr_expdec:
		while (ns < ns_to) {
			if (do_samples)
				samples[ns] = samples[ns] * EnvelopeVol >> 15;
			ns++;
			if ((cycles += cycles_step) & 0x8000) {
				cycles = 0;
				EnvelopeVol += env_step * EnvelopeVol >> 15;
				if (EnvelopeVol <= 0)
					break;
			}
		}
	}
	*EnvelopeVol_ = EnvelopeVol;
	*cycles_ = cycles;
	return ns;
}

static int audio_adsr_do(int *samples, audio_adsr *adsr, int ns_to,
	int do_samples)
{
	int EnvelopeVol = adsr->EnvelopeVol;
	int cycles_step, cycles = adsr->StepCounter;
	int ns = 0, level, env_step, rate;

	switch (adsr->State) {
	case AUDIO_ADSR_ATTACK:
		if (adsr->AttackRate == 0x7f)
			goto frozen;
		rate = adsr->AttackRate >> 2;
		cycles_step = AUDIO_MAX(1, 0x8000 >> AUDIO_MAX(0, rate - 11));
		env_step = (7 - (adsr->AttackRate & 3)) << AUDIO_MAX(0, 11 - rate);

		if (adsr->AttackModeExp)
			ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
				&cycles, cycles_step, do_samples, audio_adsr_expinc);
		else
			ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
				&cycles, cycles_step, do_samples, audio_adsr_noexp);
		if (EnvelopeVol < 0x7fff)
			break;
		EnvelopeVol = 0x7fff;
		adsr->State = AUDIO_ADSR_DECAY;
		/* fallthrough to decay */

	case AUDIO_ADSR_DECAY:
		level = (adsr->SustainLevel + 1) << 11;
		cycles_step = 0x8000 >> AUDIO_MAX(0, adsr->DecayRate - 11);
		env_step = (unsigned int)-8 << AUDIO_MAX(0, 11 - (int)adsr->DecayRate);

		while (ns < ns_to) {
			if (do_samples)
				samples[ns] = samples[ns] * EnvelopeVol >> 15;
			ns++;
			if ((cycles += cycles_step) & 0x8000) {
				cycles = 0;
				EnvelopeVol += env_step * EnvelopeVol >> 15;
				if (EnvelopeVol < level)
					break;
			}
		}
		if (EnvelopeVol >= level)
			break;
		adsr->State = AUDIO_ADSR_SUSTAIN;
		adsr->EnvelopeVol = EnvelopeVol;
		/* fallthrough to sustain */

	case AUDIO_ADSR_SUSTAIN:
		if (adsr->SustainIncrease && EnvelopeVol >= 0x7fff) {
			EnvelopeVol = 0x7fff;
			ns = ns_to;
			break;
		}
		if (adsr->SustainRate == 0x7f)
			goto frozen;
		rate = adsr->SustainRate >> 2;
		cycles_step = AUDIO_MAX(1, 0x8000 >> AUDIO_MAX(0, rate - 11));
		env_step = (7 - (adsr->SustainRate & 3)) << AUDIO_MAX(0, 11 - rate);

		if (adsr->SustainIncrease) {
			if (adsr->SustainModeExp)
				ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
					&cycles, cycles_step, do_samples, audio_adsr_expinc);
			else
				ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
					&cycles, cycles_step, do_samples, audio_adsr_noexp);
			if (EnvelopeVol >= 0x7fff) {
				EnvelopeVol = 0x7fff;
				ns = ns_to;
			}
		} else {
			env_step = ~env_step;
			if (adsr->SustainModeExp)
				ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
					&cycles, cycles_step, do_samples, audio_adsr_expdec);
			else
				ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
					&cycles, cycles_step, do_samples, audio_adsr_noexp);
			if (EnvelopeVol <= 0) {
				EnvelopeVol = 0;
				if (do_samples)
					for (; ns < ns_to; ns++)
						samples[ns] = 0;
			}
		}
		break;

	case AUDIO_ADSR_RELEASE:
		if (adsr->ReleaseRate == 0x1f)
			goto frozen;
		cycles_step = AUDIO_MAX(1, 0x8000 >> AUDIO_MAX(0, (int)adsr->ReleaseRate - 11));
		env_step = (unsigned int)-8 << AUDIO_MAX(0, 11 - (int)adsr->ReleaseRate);

		if (adsr->ReleaseModeExp)
			ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
				&cycles, cycles_step, do_samples, audio_adsr_expdec);
		else
			ns = audio_adsr_do_inner(samples, ns, ns_to, &EnvelopeVol, env_step,
				&cycles, cycles_step, do_samples, audio_adsr_noexp);

		EnvelopeVol = AUDIO_MAX(0, EnvelopeVol);
		break;
	}

	adsr->EnvelopeVol = EnvelopeVol;
	adsr->StepCounter = cycles;
	return ns;

frozen:
	if (EnvelopeVol < 0x7ff0 && do_samples) {
		for (; ns < ns_to; ns++)
			samples[ns] = samples[ns] * EnvelopeVol >> 15;
	}
	adsr->EnvelopeVol = EnvelopeVol;
	adsr->StepCounter = cycles;
	return ns;
}

AUDIO_INLINE int audio_MixADSR(int *samples, audio_adsr *adsr, int ns_to)
{
	return audio_adsr_do(samples, adsr, ns_to, 1);
}

/* ------------------------------------------------------------------ */
/* Stereo mix (spu.c:549-581, mix_chan / mix_chan_rvb)                 */
/* ------------------------------------------------------------------ */

/* Mix ChanBuf into SSumLR. The PPU version reads a global ChanBuf;
 * here the source buffer is passed explicitly (local-store scratch). */
static void audio_mix_chan(const int *src, int *SSumLR, int count, int lv, int rv)
{
	int *dst = SSumLR;
	int l, r;

	while (count--) {
		int sval = *src++;
		l = (sval * lv) >> 14;
		r = (sval * rv) >> 14;
		*dst++ += l;
		*dst++ += r;
	}
}

static void audio_mix_chan_rvb(const int *src, int *SSumLR, int count,
	int lv, int rv, int *rvb)
{
	int *dst = SSumLR;
	int *drvb = rvb;
	int l, r;

	while (count--) {
		int sval = *src++;
		l = (sval * lv) >> 14;
		r = (sval * rv) >> 14;
		*dst++ += l;
		*dst++ += r;
		*drvb++ += l;
		*drvb++ += r;
	}
}

/* Start a channel's decode buffer (spu.c:122-126, StartSoundSB). */
static void audio_StartSoundSB(audio_sample_buf *sb)
{
	memset(sb->decode + 28 - 5, 0, sizeof(sb->decode[0]) * 5);
}

/* Capture decoded samples for channels 1 and 3 (spu.c:600-620,
 * do_decode_bufs). The PPU version writes into the SPU-RAM ring at
 * 0x800/0xC00; here the samples go to a linear output buffer and the
 * PPU applies the ring (cursor = work->decode_pos). */
static void audio_do_decode_bufs(const int *src, unsigned short *dst_out,
	int count, int decode_pos)
{
	int cursor = decode_pos;
	int i;
	(void)cursor; /* the PPU applies the ring; we emit in order */
	for (i = 0; i < count; i++)
		dst_out[i] = (unsigned short)(src[i] & 0xffff);
}

/* ------------------------------------------------------------------ */
/* The worker mixer (spu.c:894-946, do_channel_work), candidate B:     */
/* no REVERBDo here — the PPU runs it on the returned RVB.             */
/*                                                                     */
/* Inputs (in local store): work (job), blocks (gathered ADPCM),       */
/* iFMod scratch, ChanBuf scratch. Outputs are written into work.      */
/* ------------------------------------------------------------------ */

static void audio_do_channel_work(audio_work *work,
	const unsigned char *blocks, int *iFMod, int *ChanBuf)
{
	audio_ctx ctx;
	unsigned int mask;
	int spos, sbpos;
	int d, ch, ns_to;

	ctx.blocks = blocks;
	ctx.iFMod = iFMod;
	ctx.work = work;

	ns_to = work->ns_to;

	/* zero the accumulators */
	memset(work->SSumLR, 0, (size_t)ns_to * sizeof(work->SSumLR[0]) * 2);
	if (work->rvb_addr)
		memset(work->RVB, 0, (size_t)ns_to * sizeof(work->RVB[0]) * 2);

	mask = work->channels_new;
	for (ch = 0; mask != 0; ch++, mask >>= 1) {
		if (mask & 1)
			audio_StartSoundSB(&work->sb_thread[ch]);
	}

	mask = work->channels_on;
	for (ch = 0; mask != 0; ch++, mask >>= 1) {
		if (!(mask & 1))
			continue;

		d = work->ch[ch].ns_to;
		spos = work->ch[ch].spos;
		sbpos = work->ch[ch].sbpos;

		if (work->ch[ch].bNoise) {
			audio_do_lsfr_samples(ChanBuf, d, work->ctrl,
				&work->noise_count, &work->noise_val);
		} else {
			audio_do_samples_adpcm(ChanBuf, audio_decode_block_work, &ctx,
				ch, d, work->ch[ch].bFMod, &work->sb_thread[ch],
				work->ch[ch].sinc, &spos, &sbpos, work->interp);
		}

		d = audio_MixADSR(ChanBuf, &work->ch[ch].adsr, d);
		if (d < ns_to) {
			work->ch[ch].adsr.EnvelopeVol = 0;
			memset(&ChanBuf[d], 0, (size_t)(ns_to - d) * sizeof(ChanBuf[0]));
		}

		if (ch == 1 || ch == 3)
			audio_do_decode_bufs(ChanBuf, work->decode_bufs[ch == 3 ? 1 : 0],
				ns_to, work->decode_pos);

		if (work->ch[ch].bFMod == 2)               /* fmod freq channel */
			memcpy(iFMod, ChanBuf, (size_t)ns_to * sizeof(iFMod[0]));
		if (work->ch[ch].bRVBActive && work->rvb_addr)
			audio_mix_chan_rvb(ChanBuf, work->SSumLR, ns_to,
				work->ch[ch].vol_l, work->ch[ch].vol_r, work->RVB);
		else
			audio_mix_chan(ChanBuf, work->SSumLR, ns_to,
				work->ch[ch].vol_l, work->ch[ch].vol_r);
	}

	/* NOTE: no REVERBDo here (candidate B). The PPU runs it on RVB.
	 * noise_count / noise_val were updated in place and are read back
	 * by the PPU from the job. */
}

#endif /* AUDIO_MIX_H */
