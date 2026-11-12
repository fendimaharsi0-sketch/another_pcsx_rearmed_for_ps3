/* RSX backend for the hardware renderer (see hwr.h), and its wiring into the
 * core's gpu_frontend_hooks.
 *
 * Full mode (the default when the startup self-test passes):
 *  - native VRAM: 1024x512 X1R5G5B5 surface in RSX local memory,
 *    big-endian halfwords holding the PS1 value (PS1 red in the surface's blue
 *    field, so the bits line up). Every batch is drawn into it at 1x and into
 *    the upscaled target; textures are sampled from it. The software renderer
 *    draws nothing.
 *  - bit 15 (STP/mask bit): the 15-bit surface cannot store a per-pixel value,
 *    but its X bit can be written as 0 (X1R5G5B5_Z1R5G5B5) or 1 (_O1R5G5B5). The
 *    programs output bit 15 as alpha. Each submit is drawn into the native VRAM
 *    with X = 0, every triangle writing its own depth (a counter); then the ops
 *    that can set bit 15 are redrawn with the X = 1 format, colour kept by the
 *    blend (dst*1 + src*0), alpha test bit 15 = 1, depth EQUAL: only pixels where
 *    that triangle is still on top. Two surface switches per submit.
 *  - RSX-side barrier (backend label write + wait for it, texture cache flush)
 *    where the hybrid mode took a snapshot: drawing that a later primitive
 *    textures from must be in memory. (Wait-for-idle alone was not enough on the
 *    console: render-to-texture text came out partly stale.) The label byte order
 *    is checked at start-up, so the wait cannot mismatch.
 *  - drawn-block map: 64x32 blocks the RSX drew into since the core's VRAM was
 *    last brought up to date; reads only copy those back.
 *  - CPU-side VRAM accesses: before the core reads VRAM (GPU->CPU transfer, copy
 *    source, savestate) the RSX is drained and the drawn blocks of the rect are
 *    copied native -> core. After the core writes VRAM (upload, copy, savestate
 *    load) the rect goes into a staging ring in main memory and the RSX copies it
 *    into the native VRAM in order with its drawing (viewing both as A8R8G8B8,
 *    so all 16 bits arrive): no CPU wait. Both convert the byte order.
 * Hybrid mode (fallback): the software renderer keeps drawing; each batch
 *    textures from a snapshot of the core's VRAM (byte-swapped to the same
 *    big-endian layout).
 *
 * Memory: upscaled colour target + depth in RSX local memory sized for 4x;
 * 16 snapshot slots (hybrid) and the native VRAM in mapped main memory; an
 * 8 MiB vertex ring. Every submit ends with a backend label carrying a sequence
 * number, so the CPU only waits when it is about to reuse memory the RSX may
 * still read, or needs the RSX's drawing in main memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <sys/systime.h>
#include <sys/stat.h>
#include "common.h"
#include "core_hooks.h"
#include "shaders.h"
#include "hwr.h"
#include "gpu_hooks.h"           /* from the core: plugins/gpulib */

#define HWR_MAXS     4      /* widest scale: 1024 * 4 = 4096, the RSX's surface and texture limit */
#define HWR_ROWS     2560   /* target rows: 512 * 5 (the 5x setting scales only the height past 4x) */
#define HWR_NSNAP    16
#define HWR_SNAPSZ   (1024 * 1024)
#define HWR_VRING    (8 * 1024 * 1024)
#define HWR_VMISC    (256 * 1024)    /* the ring's last part: copy quads (rsx_copy2), so they never
                                        land in a batch the core is still writing (vtx_area) */
#define HWR_VMAIN    (HWR_VRING - HWR_VMISC)
#define HWR_VAREA_MIN (1024 * 1024)  /* a batch area smaller than this: start the ring again */
#define HWR_VTX_S32K 5           /* RSX vertex format: signed 16-bit, not normalized (missing from PSL1GHT) */
#define HWR_LABEL    200          /* CPU-visible sequence; libgcm reserves 0..63 */
#define HWR_BLABEL   201          /* RSX-side barrier */
#define HWR_STAGE    (8 * 1024 * 1024)   /* upload staging ring */

static struct {
    hwr_dev base;
    int ok, failed, dead, S, on;   /* S: the scale code in half-steps (4 = 2x, 9 = 4.5x, 10 = 5x; in auto mode, the computed code) */
    int SX; float SY;              /* scale of the target: width, height */
    int full_ok;                  /* self-test result: full mode works on this console */
    u32 tgt_off, tgt_pitch, dep_off; volatile u32 *tgt;
    u8 *snap; u32 snap_off; u32 snap_seq[HWR_NSNAP]; int slot;
    u8 *nat; u32 nat_off;         /* native VRAM (full mode) */
    u8 *ndep; u32 ndep_off;       /* its depth buffer (Z24S8: per-triangle depth, the stencil mask plane) */
    u8 *stg; u32 stg_off, spos;   /* upload staging ring (main memory) */
    u8 *rb, *rbdep; u32 rb_off, rbdep_off;   /* readback buffer (main memory, VRAM layout) */
    rsxVertexProgram *qvp; void *qvpu;      /* plain textured quad (for upload copies) */
    rsxFragmentProgram *fblit; u32 fblit_off, fblit_unit;
    u8 *vring; u32 vring_off; u32 vpos, mpos;   /* mpos: in the copy-quad part */
    volatile u32 *label, *blabel; u32 seq, bseq;
    rsxVertexProgram *vp; void *vpu;
    /* program sets: [0] byte-decoding programs, [1] fast programs (texture unit decodes) */
    rsxVertexProgram *vps[2]; void *vpus[2];
    rsxProgramConst *vp_native[2];   /* the "native" constant (1 in the native pass) */
    int vp_cur;                   /* set of the loaded one (-1: unknown) */
    unsigned vp_epoch;            /* video_vp_epoch when it was loaded */
    /* program sets: [fast][filter][prog], filter 1 = bilinear (textured modes
       only; flat/copy/self entries alias the nearest program), filter 2 =
       Bilinear (Performance): mode 3 uses a hardware-LINEAR program, CLUT
       modes alias the manual bilinear program, the rest alias nearest.
       Filter is only chosen for the upscaled pass (see op_filt): the native
       pass, which writes the VRAM later textures are read from, always stays
       nearest. */
    rsxFragmentProgram *fps[2][3][HWR_NPROG]; u32 fps_off[2][3][HWR_NPROG];
    int ua[2][3][HWR_NPROG], ub[2][3][HWR_NPROG];   /* units of "vram" / "vram16" and "vram8" (-1) */
    int fast;                     /* program set in use */
    int draw_native;              /* the draw_ops pass in progress is the native (1x) pass */
    int px, py, pw, ph;           /* displayed VRAM rect of the last presented frame */
    int check;                    /* VRAM check: the software renderer draws too, as a reference */
    int bit15;                    /* native VRAM keeps bit 15 (self-test result) */
    u32 zc;                       /* native-pass depth counter (triangles since the last depth clear) */
    int nat_one;                  /* current native surface writes X = 1 (-1: not set) */
    int fix_blend, fix_alpha, fix_depth;   /* bit-15 fix-up pass variant (self-test picks) */
    u32 cpu_waits, syncs_in, syncs_out;
    u64 sync_ticks;               /* timebase ticks spent in CPU<->RSX VRAM syncs */
    /* where the GPU time goes (timebase ticks, since the last stats line) */
    u64 t_cmd, t_submit, t_copy, t_wait, t_present, t_up, t_nat;
    int vp_loaded;                /* the renderer's vertex program is the RSX's current one */
    int mask_used;                /* the game has drawn with the mask check (since the renderer started) */
    int cpu_read;                 /* the game has read VRAM back with the CPU (bit 15 is data then) */
    int self_ok;                  /* the bit-15 self copy passed its start-up test (diagnostic only: it
                                     passed and failed on the same console, see cpu_vram_read) */
    int shadow;                   /* the software renderer draws too, as with the VRAM check (cpu_vram_read) */
    unsigned pframes;             /* frames presented (present_hook) */
    unsigned read_last, read_win, read_n;   /* reads: frame of the last, start of the window, frames with reads in it */
    int mask_resync;              /* the target's stencil mask plane needs a full copy from the native VRAM */
} R;

/* PSL1GHT pre-swaps bytes 0 and 2 of a backend label value (like libgcm) so the
   value lands in memory unchanged; label_swz = 1 if this console stores it
   swapped after all (measured at start-up) */
static int label_swz;
static inline u32 label_value(void)
{
    u32 v = *R.label;
    return label_swz ? (v & 0xff00ff00) | ((v >> 16) & 0xff) | ((v & 0xff) << 16) : v;
}
static inline int seq_done(u32 s) { return (s32)(label_value() - s) >= 0; }

/* blocks of 64x32 VRAM pixels where the native VRAM is newer than the core's */
static unsigned char drawn[16][16];
static void mark_drawn(int x0, int y0, int x1, int y1, int v)   /* exclusive end */
{
    int bx, by;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > 1024) x1 = 1024;
    if (y1 > 512) y1 = 512;
    if (x1 <= x0 || y1 <= y0) return;
    if (!v) {          /* clearing: only blocks the rect covers completely */
        x0 = (x0 + 63) & ~63; y0 = (y0 + 31) & ~31; x1 &= ~63; y1 &= ~31;
        if (x1 <= x0 || y1 <= y0) return;
    }
    for (by = y0 >> 5; by <= (y1 - 1) >> 5; by++)
        for (bx = x0 >> 6; bx <= (x1 - 1) >> 6; bx++) drawn[by][bx] = (unsigned char)v;
}

/* per op of the submit: 0 = an opaque textured op none of whose texels can have bit 15
   (every triangle's CLUT lacks STP, see hwr_clut_stp), so its stencil-only STP draw and
   its bit-15 fix-up would write nothing and are skipped. A CLUT in an area the RSX drew
   (the core's copy is older) counts as having STP. The ops' areas are marked drawn here,
   in order, so a CLUT drawn earlier in the same submit counts too. */
static unsigned char op_stp[4096];
static void stp_flags(const hwr_op *ops, int nops, const hwr_vtx *v)
{
    const uint16_t *core = gpulib_get_vram();
    int i, ck[8]; unsigned char cr[8];
    memset(ck, -1, sizeof ck);             /* small cache of CLUTs checked (reset per submit) */
    for (i = 0; i < nops && i < 4096; i++) {
        const hwr_op *o = &ops[i];
        int b = HWR_PROG_BASE(o->prog), s = 1;
        if (!o->mset && o->pass == 0 && (b == 1 || b == 2 || b == 5 || b == 6) && core) {
            unsigned k;
            s = 0;
            for (k = 0; k + 2 < o->count && !s; k += 3) {
                const hwr_vtx *a = &v[o->first + k];
                int n, key, h, bx, x0, x1, by;
                if (a->mode != 1 && a->mode != 2) { s = 1; break; }
                key = (a->mode << 19) | ((a->cy & 511) << 10) | (a->cx & 1023);
                h = (key ^ (key >> 7) ^ (key >> 13)) & 7;
                if (ck[h] == key) { s = cr[h]; continue; }
                n = a->mode == 1 ? 16 : 256;
                by = (a->cy & 511) >> 5;
                x0 = a->cx & 1023; x1 = x0 + n - 1;
                for (bx = x0 >> 6; bx <= x1 >> 6 && !s; bx++) if (drawn[by][bx & 15]) s = 1;
                if (!s) s = hwr_clut_stp(core, a->mode, a->cx, a->cy);
                ck[h] = key; cr[h] = (unsigned char)s;
            }
        }
        op_stp[i] = (unsigned char)s;
        if (o->prog != 4 && o->bx1 > o->bx0 && o->by1 > o->by0)
            mark_drawn(o->bx0, o->by0, o->bx1, o->by1, 1);
    }
}

static void give_up(const char *why)
{
    R.dead = 1;
    logf_("hwr: %s, GPU renderer disabled\n", why);
    /* the software renderer takes over drawing (the hooks are re-read per call) */
    memset(&gpu_frontend_hooks, 0, sizeof gpu_frontend_hooks);
}

/* waits for the RSX to pass sequence s; after 2 s assume it is stuck */
static int wait_seq(u32 s)
{
    int n = 0;
    if (seq_done(s)) return 1;
    if (R.dead) return 0;
    rsxFlushBuffer(rsx_ctx);
    R.cpu_waits++;
    {
    u64 t0 = gpulib_prof_now();
    while (!seq_done(s)) {
        usleep(30);
        if (++n > 2000000 / 30) { give_up("RSX did not respond"); return 0; }
    }
    R.t_wait += gpulib_prof_now() - t0;
    }
    return 1;
}

/* everything queued so far is drawn and in memory */
static int drain(void)
{
    hwr_flush();
    R.seq++;
    rsxSetWriteBackendLabel(rsx_ctx, HWR_LABEL, R.seq);
    return wait_seq(R.seq);
}

/* uploads a fragment program; ua/ub: texture units of its samplers (b: -1 if none) */
static u32 load_fp(rsxFragmentProgram *fp, const char *na, const char *nb, int *ua, int *ub)
{
    void *uc, *m; u32 sz, off;
    rsxProgramAttrib *a;
    rsxFragmentProgramGetUCode(fp, &uc, &sz);
    m = rsxMemalign(64, sz);
    if (!m) return 0;
    memcpy(m, uc, sz);
    rsxAddressToOffset(m, &off);
    a = rsxFragmentProgramGetAttrib(fp, na);
    *ua = a ? (int)a->index : -1;
    a = nb ? rsxFragmentProgramGetAttrib(fp, nb) : NULL;
    *ub = a ? (int)a->index : -1;
    return off;
}

static int alloc_once(void)
{
    static const unsigned char *fps[2][HWR_NPROG] = {
        { hwr_f0_fpo, hwr_f1_fpo, hwr_f2_fpo, hwr_f3_fpo, hwr_f4_fpo, hwr_f1w_fpo, hwr_f2w_fpo, hwr_f3w_fpo,
          hwr_mf0_fpo, hwr_mf1_fpo, hwr_mf2_fpo, hwr_mf3_fpo, hwr_mf4_fpo, hwr_mf1w_fpo, hwr_mf2w_fpo, hwr_mf3w_fpo,
          hwr_f5_fpo },
        { hwr_ff0_fpo, hwr_ff1_fpo, hwr_ff2_fpo, hwr_ff3_fpo, hwr_ff4_fpo, hwr_ff1w_fpo, hwr_ff2w_fpo, hwr_ff3w_fpo,
          hwr_fmf0_fpo, hwr_fmf1_fpo, hwr_fmf2_fpo, hwr_fmf3_fpo, hwr_fmf4_fpo, hwr_fmf1w_fpo, hwr_fmf2w_fpo, hwr_fmf3w_fpo,
          hwr_ff5_fpo } };
    /* Bilinear variants, textured modes only (index = prog, NULL = alias nearest).
       build.sh names them with a trailing 'b': f1b, f1wb, mf1b, mf1wb, fast ff1b, fmf1b, ... */
    static const unsigned char *fps_b[2][HWR_NPROG] = {
        { NULL, hwr_f1b_fpo, hwr_f2b_fpo, hwr_f3b_fpo, NULL, hwr_f1wb_fpo, hwr_f2wb_fpo, hwr_f3wb_fpo,
          NULL, hwr_mf1b_fpo, hwr_mf2b_fpo, hwr_mf3b_fpo, NULL, hwr_mf1wb_fpo, hwr_mf2wb_fpo, hwr_mf3wb_fpo,
          NULL },
        { NULL, hwr_ff1b_fpo, hwr_ff2b_fpo, hwr_ff3b_fpo, NULL, hwr_ff1wb_fpo, hwr_ff2wb_fpo, hwr_ff3wb_fpo,
          NULL, hwr_fmf1b_fpo, hwr_fmf2b_fpo, hwr_fmf3b_fpo, NULL, hwr_fmf1wb_fpo, hwr_fmf2wb_fpo, hwr_fmf3wb_fpo,
          NULL } };
    /* Performance variants, fast mode 3 only (index = prog, NULL = alias the
       bilinear entry, which for CLUT modes is manual and for the rest is
       nearest). build.sh names them with a trailing 'p': ff3p, ff3wp, fmf3p,
       fmf3wp. The slow row is all NULL on purpose: the byte-decoding
       fallback has no hardware-LINEAR program and stays manual bilinear. */
    static const unsigned char *fps_p[2][HWR_NPROG] = {
        { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
          NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
          NULL },
        { NULL, NULL, NULL, hwr_ff3p_fpo, NULL, NULL, NULL, hwr_ff3wp_fpo,
          NULL, NULL, NULL, hwr_fmf3p_fpo, NULL, NULL, NULL, hwr_fmf3wp_fpo,
          NULL } };
    int s;
    void *tgt, *dep; u32 sz; int i;
    if (R.ok) return 1;
    if (R.failed) return 0;             /* do not leak another allocation per game start */
    R.failed = 1;
    R.tgt_pitch = 1024 * HWR_MAXS * 4;
    tgt = rsxMemalign(4096, R.tgt_pitch * HWR_ROWS);
    dep = rsxMemalign(4096, R.tgt_pitch * HWR_ROWS);
    R.snap = (u8 *)memalign(1024 * 1024, HWR_NSNAP * HWR_SNAPSZ);
    /* native VRAM and its depth buffer in local memory: the RSX textures from it
       and draws into it constantly; the CPU only reads it for readbacks */
    R.nat = (u8 *)rsxMemalign(4096, 1024 * 1024);
    R.ndep = (u8 *)rsxMemalign(4096, 2 * 1024 * 1024);   /* Z24S8: depth + the mask plane */
    R.vring = (u8 *)memalign(1024 * 1024, HWR_VRING);
    R.stg = (u8 *)memalign(1024 * 1024, HWR_STAGE);
    R.rb = (u8 *)memalign(1024 * 1024, 1024 * 1024);
    R.rbdep = (u8 *)memalign(1024 * 1024, 1024 * 1024);
    if (!tgt || !dep || !R.snap || !R.nat || !R.ndep || !R.vring || !R.stg || !R.rb || !R.rbdep) { logf_("hwr: out of memory\n"); return 0; }
    R.tgt = (volatile u32 *)tgt;
    rsxAddressToOffset(tgt, &R.tgt_off);
    rsxAddressToOffset(dep, &R.dep_off);
    rsxAddressToOffset(R.nat, &R.nat_off);
    rsxAddressToOffset(R.ndep, &R.ndep_off);
    memset(R.nat, 0, 1024 * 1024);
    if (gcmMapMainMemory(R.snap, HWR_NSNAP * HWR_SNAPSZ, &R.snap_off) != 0 ||
        gcmMapMainMemory(R.stg, HWR_STAGE, &R.stg_off) != 0 ||
        gcmMapMainMemory(R.rb, 1024 * 1024, &R.rb_off) != 0 ||
        gcmMapMainMemory(R.rbdep, 1024 * 1024, &R.rbdep_off) != 0 ||
        gcmMapMainMemory(R.vring, HWR_VRING, &R.vring_off) != 0) { logf_("hwr: map failed\n"); return 0; }
    R.spos = 0;
    R.qvp = (rsxVertexProgram *)quad_vpo;
    rsxVertexProgramGetUCode(R.qvp, &R.qvpu, &sz);
    R.fblit = (rsxFragmentProgram *)blit_fpo;
    {
        void *uc, *m; u32 n;
        rsxProgramAttrib *a;
        rsxFragmentProgramGetUCode(R.fblit, &uc, &n);
        if (!(m = rsxMemalign(64, n))) return 0;
        memcpy(m, uc, n);
        rsxAddressToOffset(m, &R.fblit_off);
        a = rsxFragmentProgramGetAttrib(R.fblit, "tex");
        R.fblit_unit = a ? a->index : 0;
    }
    R.label = (volatile u32 *)gcmGetLabelAddress(HWR_LABEL);
    R.blabel = (volatile u32 *)gcmGetLabelAddress(HWR_BLABEL);
    *R.label = 0; R.seq = 0;
    *R.blabel = 0; R.bseq = 0;
    {   /* how the backend label value lands in memory */
        int n = 0;
        rsxSetWriteBackendLabel(rsx_ctx, HWR_LABEL, 0x00112233);
        rsxFlushBuffer(rsx_ctx);
        while (*R.label == 0 && ++n < 100000) usleep(10);
        label_swz = *R.label == 0x00332211;
        logf_("hwr: backend label test: wrote 00112233, memory holds %08x%s\n", (unsigned)*R.label,
              *R.label == 0x00112233 ? " (as expected)" : label_swz ? " (byte-swapped, compensating)" : " (unexpected!)");
        if (*R.label != 0x00112233 && !label_swz) { logf_("hwr: cannot synchronise with the RSX\n"); return 0; }
        *R.label = 0;
    }
    memset(R.snap_seq, 0, sizeof R.snap_seq);
    R.vps[0] = (rsxVertexProgram *)hwr_vpo;
    R.vps[1] = (rsxVertexProgram *)hwr_fast_vpo;
    R.vp_cur = -1;
    for (s = 0; s < 2; s++) {
        rsxVertexProgramGetUCode(R.vps[s], &R.vpus[s], &sz);
        R.vp_native[s] = rsxVertexProgramGetConst(R.vps[s], "native");
        if (!R.vp_native[s]) logf_("hwr: vertex program %d has no 'native' constant\n", s);
        for (i = 0; i < HWR_NPROG; i++) {
            R.fps[s][0][i] = (rsxFragmentProgram *)fps[s][i];
            if (!(R.fps_off[s][0][i] = load_fp(R.fps[s][0][i], s ? "vram16" : "vram", s ? "vram8" : NULL,
                                            &R.ua[s][0][i], &R.ub[s][0][i]))) return 0;
            if (fps_b[s][i]) {
                R.fps[s][1][i] = (rsxFragmentProgram *)fps_b[s][i];
                if (!(R.fps_off[s][1][i] = load_fp(R.fps[s][1][i], s ? "vram16" : "vram", s ? "vram8" : NULL,
                                                &R.ua[s][1][i], &R.ub[s][1][i]))) return 0;
            } else {
                /* flat/copy/self: the bilinear entry is the nearest program */
                R.fps[s][1][i] = R.fps[s][0][i]; R.fps_off[s][1][i] = R.fps_off[s][0][i];
                R.ua[s][1][i] = R.ua[s][0][i]; R.ub[s][1][i] = R.ub[s][0][i];
            }
            if (fps_p[s][i]) {
                R.fps[s][2][i] = (rsxFragmentProgram *)fps_p[s][i];
                if (!(R.fps_off[s][2][i] = load_fp(R.fps[s][2][i], s ? "vram16" : "vram", s ? "vram8" : NULL,
                                                &R.ua[s][2][i], &R.ub[s][2][i]))) return 0;
            } else {
                /* no Performance program: CLUT modes fall back to manual
                   bilinear, the rest to nearest — see the fps_p comment */
                R.fps[s][2][i] = R.fps[s][1][i]; R.fps_off[s][2][i] = R.fps_off[s][1][i];
                R.ua[s][2][i] = R.ua[s][1][i]; R.ub[s][2][i] = R.ub[s][1][i];
            }
        }
    }
    R.ok = 1; R.failed = 0;
    return 1;
}

/* the upscaled target (native = 0) or the native VRAM surface (native = 1) */
static void set_surface(int native)
{
    gcmSurface sf; int i;
    int SX = native ? 1 : R.SX, WI = 1024 * SX, HI = native ? 512 : (int)(512.f * R.SY + 0.5f);
    float W = (float)WI, H = (float)HI;
    float scale[4] = { W * 0.5f, H * -0.5f, 0.5f, 0.0f }, offs[4] = { W * 0.5f, H * 0.5f, 0.5f, 0.0f };
    memset(&sf, 0, sizeof sf);
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    if (native) {
        sf.colorFormat = R.nat_one > 0 ? GCM_SURFACE_X1R5G5B5_O1R5G5B5 : GCM_SURFACE_X1R5G5B5_Z1R5G5B5;
        sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = R.nat_off; sf.colorPitch[0] = 2048;
    } else {
        sf.colorFormat = GCM_SURFACE_X8R8G8B8;
        sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = R.tgt_off; sf.colorPitch[0] = R.tgt_pitch;
    }
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    if (native) {
        sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX;
        sf.depthOffset = R.ndep_off; sf.depthPitch = 4096;
    } else {
        sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX;
        sf.depthOffset = R.dep_off; sf.depthPitch = R.tgt_pitch;
    }
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = WI; sf.height = HI;
    rsxSetSurface(rsx_ctx, &sf);
    rsxSetViewport(rsx_ctx, 0, 0, WI, HI, 0.0f, 1.0f, scale, offs);
    rsxSetColorMask(rsx_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetColorMaskMrt(rsx_ctx, 0);
    /* the RSX dithers by default when writing 15-bit colour (the console's
       self-test log showed native VRAM values off by one): PS1 values must be exact */
    rsxSetDitherEnable(rsx_ctx, GCM_FALSE);
    /* native pass: every draw writes its depth (see the top of the file) */
    rsxSetDepthTestEnable(rsx_ctx, native ? GCM_TRUE : GCM_FALSE);
    rsxSetDepthWriteEnable(rsx_ctx, native ? GCM_TRUE : GCM_FALSE);
    rsxSetDepthFunc(rsx_ctx, GCM_ALWAYS);
    rsxSetAlphaTestEnable(rsx_ctx, GCM_FALSE);
    rsxSetCullFaceEnable(rsx_ctx, GCM_FALSE);
    rsxSetUserClipPlaneControl(rsx_ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
}

/* VRAM (native surface or a snapshot slot) as a texture. view 0: 512x512
   A8R8G8B8 (4 bytes per texel, the byte-decoding programs), 1: 1024x512 A1R5G5B5
   (one halfword per texel, decoded by the texture unit), 2: 2048x512 8-bit (one
   byte per texel, in every channel) */
static void bind_view(u32 unit, int view)
{
    gcmTexture t;
    u32 c = view == 2 ? GCM_TEXTURE_REMAP_COLOR_B : 0;
    memset(&t, 0, sizeof t);
    t.format = (view == 0 ? GCM_TEXTURE_FORMAT_A8R8G8B8 : view == 1 ? GCM_TEXTURE_FORMAT_A1R5G5B5 : GCM_TEXTURE_FORMAT_B8) |
               GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
    t.mipmap = 1; t.dimension = GCM_TEXTURE_DIMS_2D; t.cubemap = GCM_FALSE;
    t.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
               ((view == 2 ? c : GCM_TEXTURE_REMAP_COLOR_B) << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
               ((view == 2 ? c : GCM_TEXTURE_REMAP_COLOR_G) << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
               ((view == 2 ? c : GCM_TEXTURE_REMAP_COLOR_R) << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
               ((view == 2 ? c : GCM_TEXTURE_REMAP_COLOR_A) << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    t.width = view == 0 ? 512 : view == 1 ? 1024 : 2048; t.height = 512; t.depth = 1;
    t.location = R.base.full ? GCM_LOCATION_RSX : GCM_LOCATION_CELL;
    t.pitch = 2048; t.offset = R.base.full ? R.nat_off : R.snap_off + R.slot * HWR_SNAPSZ;
    rsxLoadTexture(rsx_ctx, unit, &t);
    rsxTextureControl(rsx_ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(rsx_ctx, unit, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(rsx_ctx, unit, GCM_TEXTURE_REPEAT, GCM_TEXTURE_REPEAT, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

/* swaps the bytes of every halfword; n multiple of 8, both 8-byte aligned */
static void swap16_copy(void *dst, const void *src, size_t n)
{
    u64 *d = (u64 *)dst; const u64 *s = (const u64 *)src;
    size_t i;
    for (i = 0; i < n / 8; i++) {
        u64 x = s[i];
        d[i] = ((x & 0x00ff00ff00ff00ffull) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffull);
    }
}

/* hybrid: snapshot of the core's VRAM, converted to big-endian halfwords.
   full: barrier, everything drawn so far becomes visible to texture fetches */
static int rsxr_snapshot(hwr_dev *d, const uint16_t *vram)
{
    (void)d;
    if (R.dead) return 0;
    if (R.base.full) {
        u32 m;
        R.bseq++;
        m = label_swz ? (R.bseq & 0xff00ff00) | ((R.bseq >> 16) & 0xff) | ((R.bseq & 0xff) << 16) : R.bseq;
        rsxSetWriteBackendLabel(rsx_ctx, HWR_BLABEL, R.bseq);
        rsxSetWaitLabel(rsx_ctx, HWR_BLABEL, m);     /* the value as it lands in memory */
        rsxInvalidateTextureCache(rsx_ctx, GCM_INVALIDATE_TEXTURE);
        return 1;
    }
    R.slot = (R.slot + 1) % HWR_NSNAP;
    if (!wait_seq(R.snap_seq[R.slot])) return 0; /* RSX done with this slot's last batch */
    swap16_copy(R.snap + R.slot * HWR_SNAPSZ, vram, HWR_SNAPSZ);
    video_flush_cpu_writes(R.snap + R.slot * HWR_SNAPSZ, HWR_SNAPSZ);
    rsxInvalidateTextureCache(rsx_ctx, GCM_INVALIDATE_TEXTURE);
    return 1;
}

static void set_blend(int b)
{
    switch (b) {
    case HWR_BLEND_NONE: rsxSetBlendEnable(rsx_ctx, GCM_FALSE); return;
    case HWR_BLEND_AVG:  rsxSetBlendColor(rsx_ctx, 0x80808080, 0); rsxSetBlendEquation(rsx_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
                         rsxSetBlendFunc(rsx_ctx, GCM_CONSTANT_COLOR, GCM_CONSTANT_COLOR, GCM_ONE, GCM_ZERO); break;
    case HWR_BLEND_ADD:  rsxSetBlendEquation(rsx_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
                         rsxSetBlendFunc(rsx_ctx, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ZERO); break;
    case HWR_BLEND_SUB:  rsxSetBlendEquation(rsx_ctx, GCM_FUNC_REVERSE_SUBTRACT, GCM_FUNC_ADD);
                         rsxSetBlendFunc(rsx_ctx, GCM_ONE, GCM_ONE, GCM_ONE, GCM_ZERO); break;
    case HWR_BLEND_ADDQ: rsxSetBlendColor(rsx_ctx, 0x40404040, 0); rsxSetBlendEquation(rsx_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
                         rsxSetBlendFunc(rsx_ctx, GCM_CONSTANT_COLOR, GCM_ONE, GCM_ONE, GCM_ZERO); break;
    }
    rsxSetBlendEnable(rsx_ctx, GCM_TRUE);
}

/* native surface writing bit 15 as one (O1) or zero (Z1); only the format changes */
static void nat_format(int one)
{
    gcmSurface sf; int i;
    if (R.nat_one == one) return;
    R.nat_one = one;
    memset(&sf, 0, sizeof sf);
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorFormat = one ? GCM_SURFACE_X1R5G5B5_O1R5G5B5 : GCM_SURFACE_X1R5G5B5_Z1R5G5B5;
    sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = R.nat_off; sf.colorPitch[0] = 2048;
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = R.ndep_off; sf.depthPitch = 4096;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = 1024; sf.height = 512;
    rsxSetSurface(rsx_ctx, &sf);
}

/* loads fragment program variant p at filter index filt (see op_filt) and makes
   sure its texture units hold the views it samples.
   Performance (filt 2) is the same selection, except its fast mode-3
   programs need the vram16 unit on LINEAR: bind_view always sets NEAREST,
   so the filter is fixed up after it, tracked per unit in boundf (-1
   unknown) so a switch back to a nearest/manual program restores NEAREST. */
/* filter index for one op in the current pass: 0 nearest, 1 bilinear,
   2 bilinear (performance). Only the upscaled pass of a running renderer
   filters at all, and only textured programs; sprites use sprite_texfilter,
   everything else uses texfilter. Flat/copy/self alias nearest anyway. */
static int self_testing;   /* the self-test runs: its ops are drawn unfiltered (else their texels blend and the test fails) */
static int op_filt(const hwr_op *o)
{
    int base = o->prog & 7, cfgf;
    if (self_testing || !R.on || R.draw_native) return 0;
    if (!(base == 1 || base == 2 || base == 3 || base == 5 || base == 6 || base == 7)) return 0;
    cfgf = o->sprite ? cfg.sprite_texfilter : cfg.texfilter;
    return cfgf == 2 ? 2 : cfgf ? 1 : 0;
}

static void use_prog(int p, int filt, signed char *bound, signed char *boundf)
{
    int f = R.fast, base = p & 7, a, b, lin = 0;
    a = R.ua[f][filt][p]; b = R.ub[f][filt][p];
    rsxLoadFragmentProgramLocation(rsx_ctx, R.fps[f][filt][p], R.fps_off[f][filt][p], GCM_LOCATION_RSX);
    if (a >= 0 && a < 16 && bound[a] != (f ? 1 : 0)) { bound[a] = (signed char)(f ? 1 : 0); bind_view((u32)a, bound[a]); boundf[a] = 0; }
    if (b >= 0 && b < 16 && bound[b] != 2) { bound[b] = 2; bind_view((u32)b, 2); boundf[b] = 0; }
    lin = filt == 2 && f && (base == 3 || base == 7);
    if (a >= 0 && a < 16 && boundf[a] != lin) {
        boundf[a] = (signed char)lin;
        rsxTextureFilter(rsx_ctx, (u32)a, 0, lin ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
                         lin ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    }
}

/* how the op's pixels get bit 15 in the native VRAM: 0 they do not (pass 1 = STP=0
   texels, flat = only with the mask flag), 1 the redraw fix-up, 2 the self copy.
   A blended op on a console whose RSX does not keep the colour while setting X
   (fix_blend 0) would lose its blend in the redraw: its pixels are copied onto
   themselves instead (program HWR_PROG_SELF reads the native VRAM after a barrier).
   Blended STP texels only once the game has used the mask check or read VRAM
   back: until then nothing reads their bit 15 and the extra pass would only cost
   time. (The X-Files reads back the area where it builds its cursor from the
   bytes of its 24-bit picture: without bit 15 the cursor had wrong colours.) */
static int op_bit15(const hwr_op *o)
{
    int b = HWR_PROG_BASE(o->prog);
    if (o->prog >= HWR_PROG_SELF || b == 4) return 0;
    if (!o->mset && (b == 0 || o->pass == 1)) return 0;
    if (o->blend == HWR_BLEND_NONE || R.fix_blend) return 1;
    return o->mset || R.mask_used ? 2 : 0;
}

/* the stencil-only draws for the STP bits of opaque textured ops (see draw_ops) */
static int pend[4096], npend;
static void flush_stp(const hwr_op *ops, int native, int *prog, int *filt, signed char *bound, signed char *boundf, int *sc0, int *sc1, int *sc2, int *sc3)
{
    int k, SX = native ? 1 : R.SX;
    float SY = native ? 1.f : R.SY;
    rsxSetColorMask(rsx_ctx, 0);
    rsxSetAlphaTestEnable(rsx_ctx, GCM_TRUE);
    rsxSetAlphaFunc(rsx_ctx, GCM_GEQUAL, 128);
    rsxSetDepthFunc(rsx_ctx, GCM_EQUAL);
    rsxSetDepthWriteEnable(rsx_ctx, GCM_FALSE);
    rsxSetStencilFunc(rsx_ctx, GCM_ALWAYS, 1, 1);
    for (k = 0; k < npend; k++) {
        const hwr_op *o = &ops[pend[k]];
        int p = o->prog >= HWR_PROG_MCHK && o->prog < HWR_PROG_SELF ? HWR_PROG_BASE(o->prog) : o->prog;
        int f = op_filt(o);
        if (p != *prog || f != *filt) { *prog = p; *filt = f; use_prog(p, f, bound, boundf); }
        if (o->sx0 != *sc0 || o->sy0 != *sc1 || o->sx1 != *sc2 || o->sy1 != *sc3) {
            int y0 = (int)ceilf(o->sy0 * SY - 0.5f), y1 = (int)ceilf(o->sy1 * SY - 0.5f);
            *sc0 = o->sx0; *sc1 = o->sy0; *sc2 = o->sx1; *sc3 = o->sy1;
            rsxSetScissor(rsx_ctx, o->sx0 * SX, y0, (o->sx1 - o->sx0) * SX, y1 - y0);
        }
        rsxDrawVertexArray(rsx_ctx, GCM_TYPE_TRIANGLES, o->first, o->count);
    }
    npend = 0;
    rsxSetColorMask(rsx_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetAlphaTestEnable(rsx_ctx, GCM_FALSE);
    rsxSetDepthFunc(rsx_ctx, GCM_ALWAYS);
    rsxSetDepthWriteEnable(rsx_ctx, GCM_TRUE);
}

static void draw_ops(const hwr_op *ops, int nops, int native)
{
    int i, prog = -1, filt = -1, blend = -1, SX = native ? 1 : R.SX, fix = 0, skey = -1;
    R.draw_native = native;   /* op_filt reads this: filtering only when native == 0 */
    float SY = native ? 1.f : R.SY;
    int sc0 = -1, sc1 = -1, sc2 = -1, sc3 = -1;
    signed char bound[16];                  /* view bound on each texture unit (-1 unknown) */
    signed char boundf[16];                 /* LINEAR (1) or NEAREST (0) on each unit (-1 unknown) */
    memset(bound, -1, sizeof bound);
    memset(boundf, -1, sizeof boundf);
    int um = R.mask_used;                   /* each surface's own mask plane (stencil) */
    {   /* the vertex program, loaded again only when something else used the RSX's
           vertex program slot; the pass is its "native" constant (cheap to change) */
        float nv[4] = { native ? 1.f : 0.f, 0.f, 0.f, 0.f };
        if (!R.vp_loaded || R.vp_cur != R.fast || R.vp_epoch != video_vp_epoch) {
            rsxLoadVertexProgram(rsx_ctx, R.vps[R.fast], R.vpus[R.fast]);
            R.vp_loaded = 1; R.vp_cur = R.fast; R.vp_epoch = video_vp_epoch;
        }
        if (R.vp_native[R.fast]) rsxSetVertexProgramParameter(rsx_ctx, R.vps[R.fast], R.vp_native[R.fast], nv);
    }
    R.nat_one = native ? 0 : -1;
    set_surface(native);
    if (R.zc == 0 && (native || um)) { /* depth counter restarted: clear the depth (not the stencil) */
        if (native) rsxSetScissor(rsx_ctx, 0, 0, 1024, 512);
        else rsxSetScissor(rsx_ctx, 0, 0, 1024 * R.SX, (int)(512.f * R.SY + 0.5f));
        rsxSetClearDepthStencil(rsx_ctx, 0);
        rsxClearSurface(rsx_ctx, GCM_CLEAR_Z);
    }
    if (um) {
        /* Mask bits in each surface's stencil, once the game uses the mask check.
           Upscaled: at 3x one native pixel is 9 target pixels, and in a cut-out
           texture (leaves) they need not agree, so a check against the native VRAM
           fogged leaf pixels twice (white) or not at all (dark). Native: the check
           read bit 15 from the native VRAM as a texture, which needed a barrier before
           every mask-checked primitive over fresh drawing (Silent Hill: up to 66 a
           frame, the RSX starved). The stencil holds each pixel's bit 15: every draw
           writes its value (1 for the mask-set flag and STP texels of semi-transparent
           draws), opaque textured draws a second time with the alpha test where the
           texel's STP bit is set (depth EQUAL: only where that triangle is on top),
           copies from the native VRAM set it from the copied bit 15 (in the native
           pass only the stencil: uploads), and mask-checked draws pass only where it
           is 0. */
        rsxSetDepthTestEnable(rsx_ctx, GCM_TRUE);
        rsxSetDepthWriteEnable(rsx_ctx, GCM_TRUE);
        rsxSetDepthFunc(rsx_ctx, GCM_ALWAYS);
        rsxSetTwoSidedStencilTestEnable(rsx_ctx, GCM_FALSE);
        rsxSetStencilTestEnable(rsx_ctx, GCM_TRUE);
        rsxSetStencilMask(rsx_ctx, 0xff);
        rsxSetStencilOp(rsx_ctx, GCM_KEEP, GCM_KEEP, GCM_REPLACE);
    }
    npend = 0;
    for (i = 0; i < nops; i++) {
        const hwr_op *o = &ops[i];
        int p = o->prog, chk = p >= HWR_PROG_MCHK && p < HWR_PROG_SELF, v = 0;
        /* copies from the native VRAM into the target; in the native pass only for the
           stencil (the uploaded rect's bit 15) */
        if (native && p == 4 && !um) continue;
        if (um && chk) {
            p = HWR_PROG_BASE(p);       /* the stencil does the check: */
            if (npend) {                /* the stencil bits of the draws before it first */
                flush_stp(ops, native, &prog, &filt, bound, boundf, &sc0, &sc1, &sc2, &sc3);
                skey = -1;
            }
        }
        { int f = op_filt(o); if (p != prog || f != filt) { prog = p; filt = f; use_prog(prog, f, bound, boundf); } }
        if (o->blend != blend) { blend = o->blend; set_blend(blend); }
        if (o->sx0 != sc0 || o->sy0 != sc1 || o->sx1 != sc2 || o->sy1 != sc3) {
            sc0 = o->sx0; sc1 = o->sy0; sc2 = o->sx1; sc3 = o->sy1;
            {   /* target rows whose centres fall inside the native rows (4.5x: fractional) */
                int y0 = (int)ceilf(sc1 * SY - 0.5f), y1 = (int)ceilf(sc3 * SY - 0.5f);
                rsxSetScissor(rsx_ctx, sc0 * SX, y0, (sc2 - sc0) * SX, y1 - y0);
            }
        }
        if (um) {
            int key;
            v = o->mset || o->pass == 2;
            key = (!chk ? 0 : v ? 1 : 2) * 2 + v;
            if (key != skey) {                  /* (a flush sets skey = -1: it leaves ALWAYS, 1) */
                rsxSetStencilFunc(rsx_ctx, !chk ? GCM_ALWAYS : v ? GCM_NOTEQUAL : GCM_EQUAL, v, 1);
                skey = key;
            }
        }
        if (native && p == 4) rsxSetColorMask(rsx_ctx, 0);
        rsxDrawVertexArray(rsx_ctx, GCM_TYPE_TRIANGLES, o->first, o->count);
        if (native && p == 4)
            rsxSetColorMask(rsx_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
        /* opaque textured (or a copy): the pixels whose texel has bit 15 get stencil 1,
           in a second stencil-only draw, collected and done together (before the next
           mask-checked draw, and at the end): the per-triangle depth keeps each to the
           pixels its triangle still owns */
        if (um && !v && o->pass == 0 && HWR_PROG_BASE(p) != 0 && op_stp[i]) pend[npend++] = i;
        if (native && R.bit15) fix |= op_bit15(o);
    }
    if (npend) { flush_stp(ops, native, &prog, &filt, bound, boundf, &sc0, &sc1, &sc2, &sc3); blend = -1; }
    if (um) {
        rsxSetStencilTestEnable(rsx_ctx, GCM_FALSE);
        if (!native) {
            rsxSetDepthTestEnable(rsx_ctx, GCM_FALSE);
            rsxSetDepthWriteEnable(rsx_ctx, GCM_FALSE);
        }
    }
    if (fix & 1) {
        /* bit 15 fix-up: the pixels drawn above have X = 0. Redraw the ops that can
           set it with the X = 1 format, keeping the colour (blend dst*1 + src*0),
           only where the program's alpha (bit 15) is 1 and only where this very
           triangle is still the top one (depth EQUAL against its own value) */
        nat_format(1);
        rsxSetBlendEnable(rsx_ctx, R.fix_blend ? GCM_TRUE : GCM_FALSE);
        rsxSetBlendEquation(rsx_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
        rsxSetBlendFunc(rsx_ctx, GCM_ZERO, GCM_ONE, GCM_ZERO, GCM_ONE);
        rsxSetAlphaTestEnable(rsx_ctx, R.fix_alpha ? GCM_TRUE : GCM_FALSE);
        rsxSetAlphaFunc(rsx_ctx, GCM_GEQUAL, 128);
        rsxSetDepthFunc(rsx_ctx, R.fix_depth ? GCM_EQUAL : GCM_ALWAYS);
        rsxSetDepthWriteEnable(rsx_ctx, GCM_FALSE);
        for (i = 0; i < nops; i++) {
            const hwr_op *o = &ops[i];
            int fp = um && o->prog >= HWR_PROG_MCHK && o->prog < HWR_PROG_SELF ? HWR_PROG_BASE(o->prog) : o->prog;
            if (op_bit15(o) != 1 || !op_stp[i]) continue;   /* (no texel with bit 15: nothing to set) */
            { int f = op_filt(o); if (fp != prog || f != filt) { prog = fp; filt = f; use_prog(prog, f, bound, boundf); } }
            if (o->sx0 != sc0 || o->sy0 != sc1 || o->sx1 != sc2 || o->sy1 != sc3) {
                sc0 = o->sx0; sc1 = o->sy0; sc2 = o->sx1; sc3 = o->sy1;
                rsxSetScissor(rsx_ctx, sc0, sc1, sc2 - sc0, sc3 - sc1);
            }
            rsxDrawVertexArray(rsx_ctx, GCM_TYPE_TRIANGLES, o->first, o->count);
        }
        rsxSetAlphaTestEnable(rsx_ctx, GCM_FALSE);
        rsxSetDepthFunc(rsx_ctx, GCM_ALWAYS);
    }
    if (fix & 2) {
        /* self copy for blended ops (see op_bit15): once the blended colours are in
           memory (barrier), each pixel the op's triangle still owns (depth EQUAL)
           is written again with its own colour and the X = 1 format, blend off */
        rsxr_snapshot(&R.base, NULL);
        nat_format(1);
        rsxSetBlendEnable(rsx_ctx, GCM_FALSE);
        rsxSetAlphaTestEnable(rsx_ctx, GCM_FALSE);
        rsxSetDepthFunc(rsx_ctx, GCM_EQUAL);
        rsxSetDepthWriteEnable(rsx_ctx, GCM_FALSE);
        prog = HWR_PROG_SELF; filt = 0; use_prog(prog, 0, bound, boundf);
        for (i = 0; i < nops; i++) {
            const hwr_op *o = &ops[i];
            if (op_bit15(o) != 2) continue;
            if (o->sx0 != sc0 || o->sy0 != sc1 || o->sx1 != sc2 || o->sy1 != sc3) {
                sc0 = o->sx0; sc1 = o->sy0; sc2 = o->sx1; sc3 = o->sy1;
                rsxSetScissor(rsx_ctx, sc0, sc1, sc2 - sc0, sc3 - sc1);
            }
            rsxDrawVertexArray(rsx_ctx, GCM_TYPE_TRIANGLES, o->first, o->count);
        }
        rsxSetDepthFunc(rsx_ctx, GCM_ALWAYS);
    }
    rsxSetBlendEnable(rsx_ctx, GCM_FALSE);
    if (native) { rsxSetDepthTestEnable(rsx_ctx, GCM_FALSE); rsxSetDepthWriteEnable(rsx_ctx, GCM_FALSE); }
}

static void rsxr_submit_(hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv);
static void rsxr_submit(hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv)
{
    u64 t0 = gpulib_prof_now();
    rsxr_submit_(d, ops, nops, v, nv);
    R.t_submit += gpulib_prof_now() - t0;
}

/* PGXP-lite kill gate: while the setting is off the whole lite path is dead.
 * pgxp_lite_on lives in the core (libpcsxcore/pgxp_lite.c); this file already
 * takes it in log_stats, so the plain extern here adds no new link requirement.
 * The ring layout follows the switch: the lite path splits the ring (batches in
 * the main part, copy quads in the last 256 KiB), the pre-lite path uses the
 * whole ring for both. On a switch, drain everything queued and restart both
 * positions, so a batch never overwrites a copy quad the RSX is still reading
 * (or the other way round). */
extern int pgxp_lite_on;
extern int pgxp_cull_track;   /* core, libpcsxcore/pgxp_lite.c: the culling correction's effective track */
static int pgxp_on(void) { return pgxp_lite_on; }
static int ring_lite = -1;   /* the layout the ring is in now; -1: not started */
static int ring_sync(int lite)   /* 1: the ring is in this layout (drained first if it changed) */
{
    if (lite != ring_lite) {
        if (!wait_seq(R.seq)) return 0;
        R.vpos = 0; R.mpos = 0;
        ring_lite = lite;
    }
    return 1;
}

/* the core writes a batch's vertices straight into the ring (see hwr_dev.vtx_area):
   from R.vpos to the end of the main part, at most VCAP (hwr_core.c) */
static hwr_vtx *rsxr_vtx_area(hwr_dev *d, int *cap)
{
    u32 n;
    (void)d;
    if (R.dead) return NULL;
    if (!pgxp_on()) return NULL;   /* kill gate: OFF = the core's own buffer, exactly like pre-lite */
    if (!ring_sync(1)) return NULL;   /* the ring layout follows the switch (see above) */
    if (R.vpos + HWR_VAREA_MIN > HWR_VMAIN) {     /* wrap: everything already queued must finish first */
        if (!wait_seq(R.seq)) return NULL;
        R.vpos = 0;
    }
    n = (HWR_VMAIN - R.vpos) / sizeof(hwr_vtx);
    *cap = n > 65536 ? 65536 : (int)n;
    return (hwr_vtx *)(R.vring + R.vpos);
}

static void rsxr_submit_(hwr_dev *d, const hwr_op *ops, int nops, const hwr_vtx *v, int nv)
{
    hwr_sprites_apart = cfg.sprite_texfilter != cfg.texfilter;   /* (for the next batches) */
    u32 bytes = (u32)nv * sizeof *v, base, st = sizeof *v;
    int lite = pgxp_on();
    int inplace = lite && (const u8 *)v == R.vring + R.vpos;   /* written there by the core */
    (void)d;
    if (!nops || !nv || R.dead) return;
    if (!ring_sync(lite)) return;   /* the ring layout follows the switch (see above) */
    if (!inplace && R.vpos + bytes > (lite ? HWR_VMAIN : HWR_VRING)) {  /* wrap: everything already queued must finish first */
        if (!wait_seq(R.seq)) return;
        R.vpos = 0;
    }
    {
    u64 t0 = gpulib_prof_now();
    if (!inplace) {   /* the ring lines are claimed with dcbz first: a plain store would make the PPU
                         read each 128-byte line from memory before overwriting it */
        u8 *d = R.vring + R.vpos, *e = d + ((bytes + 127) & ~127u);
        for (; d < e; d += 128) __asm__ volatile("dcbz 0,%0" : : "r"(d) : "memory");
        memcpy(R.vring + R.vpos, v, bytes);
    }
    if (R.base.full) {      /* native-pass depth: one value per triangle, increasing */
        hwr_vtx *q = (hwr_vtx *)(R.vring + R.vpos);
        int k;
        float z, dz = 1.0f / 32768.0f;
        /* window depth 0.5 + z/2: one 16-bit depth step per triangle, never rounded together
           (multiples of 2^-15 below 1 add up exactly) */
        if (R.zc + (u32)nv / 3 + 2 > 32000) R.zc = 0;           /* draw_ops clears the depth */
        z = (float)(R.zc + 1) * dz;
        for (k = 0; k + 2 < nv; k += 3, z += dz) q[k].z = q[k + 1].z = q[k + 2].z = z;
    }
    video_flush_cpu_writes(R.vring + R.vpos, bytes);
    R.t_copy += gpulib_prof_now() - t0;
    }
    base = R.vring_off + R.vpos;
    R.vpos = (R.vpos + bytes + 127) & ~127u;

    rsxInvalidateVertexCache(rsx_ctx);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_POS,  0, base,      st, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX0, 0, base + 16, st, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
    /* whole-number attributes are 16-bit integers the RSX converts (see hwr.h) */
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX1, 0, base + 32, st, 4, HWR_VTX_S32K, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX2, 0, base + 40, st, 4, HWR_VTX_S32K, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX3, 0, base + 48, st, 4, HWR_VTX_S32K, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX4, 0, base + 56, st, 4, HWR_VTX_S32K, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX5, 0, base + 64, st, 4, HWR_VTX_S32K, GCM_LOCATION_CELL);
    if (!R.mask_used) {
        int i;
        for (i = 0; i < nops; i++)
            if (ops[i].prog >= HWR_PROG_MCHK && ops[i].prog < HWR_PROG_SELF) {
                R.mask_used = 1;
                R.base.mask_stencil = 1;   /* from now on: no barriers for the mask check */
                R.mask_resync = 1;     /* the mask planes: rebuilt from the native VRAM */
                logf_("hwr: the game draws with the mask check (emulated)\n");
                break;
            }
    }
    /* which ops can set bit 15 at all; also marks what the native VRAM is about to
       have that the core lacks (areas from hwr_core) */
    if (R.base.full) stp_flags(ops, nops, v);
    else memset(op_stp, 1, sizeof op_stp);
    {
    u64 t0 = gpulib_prof_now();
    draw_ops(ops, nops, 0);
    R.t_up += gpulib_prof_now() - t0;
    }
    if (R.base.full) {
        u64 t0 = gpulib_prof_now();
        draw_ops(ops, nops, 1);
        R.zc += (u32)nv / 3 + 1;
        R.t_nat += gpulib_prof_now() - t0;
    }
    /* the display pass only uses POS/TEX0/TEX1: switch off the others */
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX2, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX3, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX4, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX5, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    R.seq++;
    rsxSetWriteBackendLabel(rsx_ctx, HWR_LABEL, R.seq);
    if (!R.base.full) R.snap_seq[R.slot] = R.seq;
    rsxFlushBuffer(rsx_ctx);
}

/* Resolution that the TV can show: drawing a 480-line picture (interlaced menus,
   Tekken 3's character select) 4x tall makes 1920 rows for a 1080-line screen,
   and the RSX ran out of fill rate there. The scale for the picture shown is
   capped at what the screen needs (rounded up, at least 2x), never raised
   above the setting; it changes after the picture size has been stable for a
   few frames, and the upscaled copy is rebuilt from the native VRAM.
   Auto resolution replaces that cap with the target: the scale is the target
   height over the picture height, in 0.5x steps (ties to the lower step),
   clamped to what the target buffer holds (1x..5x); the width follows the
   height (floor), capped at HWR_MAXS — the 4.5x setting's shape. */
static struct { int sx; float sy; int frames; } want;

/* scale code (half-steps) -> the target scales: width is the code's whole
   part, capped at HWR_MAXS; height is the code in halves (9 -> 4.5x) */
static void scale_from_code(int code, int *sx, float *sy)
{
    int cx = code / 2;
    *sx = cx > HWR_MAXS ? HWR_MAXS : cx;
    *sy = (float)code / 2.0f;
}

static void scale_for(int w, int h, int *sx, float *sy)
{
    int at = cfg_auto_target_h();
    int need;
    if (at && h > 0) {
        /* auto: nearest 0.5x step to the target, ties to the lower one:
           steps = ceil(2*at/h - 0.5), in integers (4*at + h - 1) / (2*h) */
        int steps = (4 * at + h - 1) / (2 * h);
        float cy;
        if (steps < 2) steps = 2;
        if (steps > 10) steps = 10;
        cy = (float)steps / 2.0f;
        *sy = cy;
        *sx = (int)cy > HWR_MAXS ? HWR_MAXS : (int)cy;
        return;
    }
    scale_from_code(R.S, sx, sy);
    /* only high-resolution pictures (480 lines, 512/640 wide): a 240-line game keeps
       the setting even past the screen's lines (that extra smooths the edges) */
    if (h > 256) { need = ((int)video_height() + h / 2) / h; if (need < 2) need = 2; if ((float)need < *sy) *sy = (float)need; }
    if (w > 400) { need = ((int)video_width() + w / 2) / w; if (need < 2) need = 2; if (need < *sx) *sx = need; }
}

static int rsxr_present(hwr_dev *d, int x, int y, int w, int h)
{
    int sx; float sy;
    (void)d;
    if (R.dead) return 0;
    R.px = x; R.py = y; R.pw = w; R.ph = h;
    scale_for(w, h, &sx, &sy);
    if (sx != want.sx || sy != want.sy) { want.sx = sx; want.sy = sy; want.frames = 0; }
    else if (want.frames < 1000) want.frames++;
    hooks_frame_from_gpu(w, h);
    return 1;
}

/* after the frame at the old scale has been shown */
static void apply_scale(void)
{
    if (!R.on || R.dead || want.frames < 3 || (want.sx == R.SX && want.sy == R.SY)) return;
    hwr_flush();
    if (cfg_auto_target_h())
        logf_("hwr: %dx%d picture: auto target %d, drawing at %dx wide, %gx tall (the screen has %u lines)\n",
              R.pw, R.ph, cfg_auto_target_h(), want.sx, want.sy, video_height());
    else
        logf_("hwr: %dx%d picture: drawing at %dx wide, %gx tall (the screen has %u lines)\n",
              R.pw, R.ph, want.sx, want.sy, video_height());
    R.SX = want.sx; R.SY = want.sy;
    /* auto: the scale code follows the computed scale, so turning auto off
       restarts the renderer only if the manual scale differs from it */
    if (cfg_auto_target_h()) R.S = (int)(want.sy * 2.0f + 0.5f);
    hwr_attach_vram(gpulib_get_vram());     /* everything is copied from the native VRAM at the new scale */
}

/* ---- full mode: core VRAM <-> native VRAM ---- */

/* copies a VRAM rect (may wrap at the edges) between the core's VRAM (little-endian
   halfwords) and the native surface (big-endian). to_native: direction */
static void sync_rect(int x, int y, int w, int h, int to_native)
{
    u16 *core = (u16 *)gpulib_get_vram();
    int row, ux, uy, uw, uh, up = !to_native && gpulib_pending_write(&ux, &uy, &uw, &uh);
    if (w <= 0 || h <= 0) return;
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    x &= 1023; y &= 511;
    /* whole rows when the rect wraps horizontally; 8-byte aligned spans */
    if (x + w > 1024) { x = 0; w = 1024; }
    w = ((x + w + 3) & ~3) - (x & ~3); x &= ~3;
    for (row = 0; row < h; row++) {
        int yy = (y + row) & 511;
        u8 *n = R.nat + (yy * 1024 + x) * 2;
        u16 *c = core + yy * 1024 + x;
        /* local memory is not cached by the PPU: plain stores and loads */
        if (to_native) {
            swap16_copy(n, c, w * 2);
            __asm__ volatile("sync" : : : "memory");
        } else {
            if (up && ((yy - uy) & 511) < uh) {
                /* an upload in progress already put newer data here: keep it */
                int i;
                for (i = 0; i < w; i++)
                    if (((x + i - ux) & 1023) >= uw) {   /* big-endian halfword -> little-endian */
                        ((u8 *)c)[i * 2] = n[i * 2 + 1]; ((u8 *)c)[i * 2 + 1] = n[i * 2];
                    }
            } else
                swap16_copy(c, n, w * 2);
        }
    }
}

/* calls f for the parts of a VRAM rect that do not wrap */
static void split_rect(int x, int y, int w, int h, void (*f)(int, int, int, int))
{
    if (w > 1024) w = 1024;
    if (h > 512) h = 512;
    x &= 1023; y &= 511;
    if (x + w > 1024) { split_rect(x, y, 1024 - x, h, f); split_rect(0, y, x + w - 1024, h, f); return; }
    if (y + h > 512) { f(x, y, w, 512 - y); f(x, 0, w, y + h - 512); return; }
    f(x, y, w, h);
}
static void undraw(int x, int y, int w, int h) { mark_drawn(x, y, x + w, y + h, 0); }

/* shrinks a rect (not wrapping) to the part the RSX drew into; 0 if none */
static int drawn_bbox(int *x, int *y, int *w, int *h)
{
    int bx, by, X0 = 99, Y0 = 99, X1 = -1, Y1 = -1;
    for (by = *y >> 5; by <= (*y + *h - 1) >> 5; by++)
        for (bx = *x >> 6; bx <= (*x + *w - 1) >> 6; bx++)
            if (drawn[by][bx]) {
                if (bx < X0) X0 = bx;
                if (bx > X1) X1 = bx;
                if (by < Y0) Y0 = by;
                if (by > Y1) Y1 = by;
            }
    if (X1 < 0) return 0;
    {
        int x0 = X0 * 64 > *x ? X0 * 64 : *x, y0 = Y0 * 32 > *y ? Y0 * 32 : *y;
        int x1 = (X1 + 1) * 64 < *x + *w ? (X1 + 1) * 64 : *x + *w, y1 = (Y1 + 1) * 32 < *y + *h ? (Y1 + 1) * 32 : *y + *h;
        *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    }
    return 1;
}

/* VRAM reads: the RSX copies the drawn blocks into a main-memory buffer laid out
   like VRAM (the PPU reads RSX local memory at only ~16 MB/s, which made videos
   that clear the screen every frame crawl), then the CPU converts from there */
static struct { int x, y, w, h; } rd[8];
static int nrd;
static void read_collect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0 || nrd >= 8 || !drawn_bbox(&x, &y, &w, &h)) return;   /* core VRAM is current */
    rd[nrd].x = x; rd[nrd].y = y; rd[nrd].w = w; rd[nrd].h = h; nrd++;
}

/* one quad of a copy between two 512x512 A8R8G8B8 views of VRAM (one pixel = two
   halfwords); colour mask selects the halfwords of edge pixels */
static void copy_quad(u32 vbase, int k, int px0, int px1, int y0, int y1, float v0, float v1, u32 mask, int du)
{
    float *q = (float *)(R.vring + vbase - R.vring_off) + k * 24;
    float X0 = px0 / 256.f - 1.f, X1 = px1 / 256.f - 1.f, Y0 = 1.f - y0 / 256.f, Y1 = 1.f - y1 / 256.f;
    float u0 = (px0 + du) / 512.f, u1 = (px1 + du) / 512.f;   /* du: source pixels to the right */
    float v[24] = { X0, Y0, u0, v0, 0.f, 0.f,   X1, Y0, u1, v0, 0.f, 0.f,
                    X0, Y1, u0, v1, 0.f, 0.f,   X1, Y1, u1, v1, 0.f, 0.f };
    memcpy(q, v, sizeof v);
    video_flush_cpu_writes(q, sizeof v);
    rsxSetColorMask(rsx_ctx, mask);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_POS,  0, vbase + k * 96,      24, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX0, 0, vbase + k * 96 + 8,  24, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX1, 0, vbase + k * 96 + 16, 24, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
    rsxDrawVertexArray(rsx_ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
}

/* Copies VRAM rect (x, y, w, h) (not wrapping) between two buffers laid out like
   VRAM (2048-byte rows, big-endian halfwords), as a byte-exact A8R8G8B8 draw.
   The source texture is src_h rows tall starting at VRAM row src_row0. exact:
   only the halfwords of the rect are written (else whole pixel pairs). The source
   rect is (sx, sy): x and sx must be both even or both odd (whole pixel pairs). */
static void rsx_copy2(u8 dst_loc, u32 dst_off, u32 ddep_off, u8 src_loc, u32 src_off, int src_h, int src_row0,
                      int sx, int sy, int x, int y, int w, int h, int exact)
{
    int px0 = x >> 1, px1 = (x + w + 1) >> 1, i, du = (sx >> 1) - (x >> 1);
    float v0 = (float)(sy - src_row0) / src_h, v1 = (float)(sy + h - src_row0) / src_h;
    u32 vbase;
    gcmSurface sf; gcmTexture t;
    int lite = pgxp_on();
    if (!ring_sync(lite)) return;   /* the ring layout follows the switch (see above) */
    if (lite) {
        if (R.mpos + 3 * 96 > HWR_VMISC) {   /* wrap: the copies queued so far must be done */
            R.seq++;
            rsxSetWriteBackendLabel(rsx_ctx, HWR_LABEL, R.seq);
            if (!wait_seq(R.seq)) return;
            R.mpos = 0;
        }
        vbase = R.vring_off + HWR_VMAIN + R.mpos;
        R.mpos = (R.mpos + 3 * 96 + 127) & ~127u;
    } else {   /* kill gate: pre-lite, the copy quads share the ring with the batches */
        if (R.vpos + 3 * 96 > HWR_VRING) { if (!wait_seq(R.seq)) return; R.vpos = 0; }
        vbase = R.vring_off + R.vpos;
        R.vpos = (R.vpos + 3 * 96 + 127) & ~127u;
    }
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_A8R8G8B8; sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = dst_loc; sf.colorOffset[0] = dst_off; sf.colorPitch[0] = 2048;
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = dst_loc;   /* same location and pitch */
    sf.depthOffset = ddep_off; sf.depthPitch = 2048;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = 512; sf.height = 512;
    rsxSetSurface(rsx_ctx, &sf);
    {
        float scale[4] = { 256.f, -256.f, 0.5f, 0.0f }, offs[4] = { 256.f, 256.f, 0.5f, 0.0f };
        rsxSetViewport(rsx_ctx, 0, 0, 512, 512, 0.0f, 1.0f, scale, offs);
    }
    rsxSetScissor(rsx_ctx, 0, 0, 512, 512);
    rsxSetDitherEnable(rsx_ctx, GCM_FALSE);
    rsxSetBlendEnable(rsx_ctx, GCM_FALSE);
    rsxSetDepthTestEnable(rsx_ctx, GCM_FALSE);
    rsxSetCullFaceEnable(rsx_ctx, GCM_FALSE);
    memset(&t, 0, sizeof t);
    t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
    t.mipmap = 1; t.dimension = GCM_TEXTURE_DIMS_2D; t.cubemap = GCM_FALSE;
    t.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    t.width = 512; t.height = src_h; t.depth = 1; t.location = src_loc;
    t.pitch = 2048; t.offset = src_off;
    rsxLoadTexture(rsx_ctx, R.fblit_unit, &t);
    rsxTextureControl(rsx_ctx, R.fblit_unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(rsx_ctx, R.fblit_unit, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(rsx_ctx, R.fblit_unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                       0, GCM_TEXTURE_ZFUNC_LESS, 0);
    rsxInvalidateVertexCache(rsx_ctx);
    rsxLoadVertexProgram(rsx_ctx, R.qvp, R.qvpu);
    R.vp_loaded = 0;
    rsxLoadFragmentProgramLocation(rsx_ctx, R.fblit, R.fblit_off, GCM_LOCATION_RSX);
    {
        /* a pixel holds halfwords 2n (bytes A,R) and 2n+1 (bytes G,B) */
        const u32 FIRST = GCM_COLOR_MASK_A | GCM_COLOR_MASK_R, SECOND = GCM_COLOR_MASK_G | GCM_COLOR_MASK_B;
        int a = px0, b = px1, k = 0;
        if (exact && (x & 1)) { copy_quad(vbase, k++, a, a + 1, y, y + h, v0, v1, SECOND, du); a++; }
        if (exact && ((x + w) & 1) && b > a) { copy_quad(vbase, k++, b - 1, b, y, y + h, v0, v1, FIRST, du); b--; }
        if (b > a) copy_quad(vbase, k++, a, b, y, y + h, v0, v1, FIRST | SECOND, du);
    }
    rsxSetColorMask(rsx_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
}

static void rsx_copy(u8 dst_loc, u32 dst_off, u32 ddep_off, u8 src_loc, u32 src_off, int src_h, int src_row0,
                     int x, int y, int w, int h, int exact)
{
    rsx_copy2(dst_loc, dst_off, ddep_off, src_loc, src_off, src_h, src_row0, x, y, x, y, w, h, exact);
}

/* VRAM rect (not wrapping) from the core's VRAM into the native VRAM, by the RSX */
static void upload_part(int x, int y, int w, int h)
{
    u16 *core = (u16 *)gpulib_get_vram();
    u32 bytes = (u32)h * 2048, sbase;
    int row, cx0 = x & ~3, cx1 = (x + w + 3) & ~3;
    if (w <= 0 || h <= 0 || R.dead) return;
    if (R.spos + bytes > HWR_STAGE) {          /* ring wraps: the RSX must be done with all of it */
        if (!wait_seq(R.seq)) return;
        R.spos = 0;
    }
    sbase = R.spos;
    R.spos = (R.spos + bytes + 127) & ~127u;
    /* rows at the same byte offsets as in VRAM, so texture and target coordinates match */
    for (row = 0; row < h; row++) {
        u8 *d = R.stg + sbase + row * 2048 + cx0 * 2;
        swap16_copy(d, core + (y + row) * 1024 + cx0, (cx1 - cx0) * 2);
        video_flush_cpu_writes(d, (cx1 - cx0) * 2);
    }
    /* earlier draws must be done with this area (reading or writing) */
    rsxr_snapshot(&R.base, NULL);
    rsx_copy(GCM_LOCATION_RSX, R.nat_off, R.ndep_off, GCM_LOCATION_CELL, R.stg_off + sbase, h, y, x, y, w, h, 1);
    R.seq++;
    rsxSetWriteBackendLabel(rsx_ctx, HWR_LABEL, R.seq);
    rsxFlushBuffer(rsx_ctx);
    mark_drawn(x, y, x + w, y + h, 0);     /* the core has the same data now */
}

/* ---- GP0 trace of one frame (VRAM check mode, armed by a VRAM dump) ----
 * For finding renderer differences on a PC: the core's VRAM and E1..E6 state at
 * the start of the next frame, then everything that frame does to VRAM, in
 * order, little-endian 32-bit words:
 *   "GP0T" version=1, 1 MiB VRAM, 8 words E-registers (as gpulib keeps them)
 *   1 n words...        GP0 command words given to the renderer
 *   2 x y w h + w*h halfwords (padded to a word)   CPU/DMA write into VRAM
 *   3 x y w h           the frame is shown */
static struct { int armed, on; u32 *w; u32 n, cap; char path[600]; } T;

static void tr_put(u32 v) { if (T.n < T.cap) T.w[T.n++] = v; }

static void tr_vram_write(int x, int y, int w, int h)
{
    const u16 *core = (const u16 *)gpulib_get_vram();
    int r, c;
    u32 pend = 0, have = 0;
    if (!T.on) return;
    tr_put(2); tr_put((u32)x); tr_put((u32)y); tr_put((u32)w); tr_put((u32)h);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) {
            u32 v = core[((y + r) & 511) * 1024 + ((x + c) & 1023)];
            v = (v >> 8 | v << 8) & 0xffff;              /* host halfword -> the LE value */
            if (have) { tr_put(pend | v << 16); have = 0; } else { pend = v; have = 1; }
        }
    if (have) tr_put(pend);
}

static void tr_frame(int x, int y, int w, int h)
{
    if (T.on) {
        FILE *f;
        u32 i;
        tr_put(3); tr_put((u32)x); tr_put((u32)y); tr_put((u32)w); tr_put((u32)h);
        T.on = 0;
        if ((f = fopen(T.path, "wb"))) {
            for (i = 0; i < T.n; i++) {
                u32 v = T.w[i];
                unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
                fwrite(b, 1, 4, f);
            }
            fclose(f);
        }
        logf_("gp0 trace: %u words%s -> %s\n", (unsigned)T.n, T.n >= T.cap ? " (FULL, cut short)" : "", T.path);
        free(T.w); T.w = NULL;
    } else if (T.armed) {
        const u16 *core = (const u16 *)gpulib_get_vram();
        const uint32_t *ex = gpulib_get_ex_regs();
        int i;
        T.armed = 0;
        T.cap = 4u << 20; T.n = 0;
        if (!(T.w = (u32 *)malloc(T.cap * 4))) { logf_("gp0 trace: no memory\n"); return; }
        T.on = 1;
        tr_put(0x54305047); tr_put(1);                       /* "GP0T" */
        for (i = 0; i < 1024 * 512; i += 2) {
            u32 a = core[i], b = core[i + 1];
            a = (a >> 8 | a << 8) & 0xffff; b = (b >> 8 | b << 8) & 0xffff;
            tr_put(a | b << 16);
        }
        for (i = 0; i < 8; i++) tr_put(ex[i]);
    }
}

static void full_vram_written(int x, int y, int w, int h)
{
    tr_vram_write(x, y, w, h);
    {
    u64 t0 = gpulib_prof_now();
    hwr_flush();                     /* draws queued before the upload come first */
    split_rect(x, y, w, h, upload_part);
    R.syncs_in++;
    R.sync_ticks += gpulib_prof_now() - t0;
    hwr_vram_written(x, y, w, h);    /* redraws the rect into the upscaled target */
    }
}

static void full_vram_read(int x, int y, int w, int h)
{
    u64 t0 = gpulib_prof_now();
    u16 *core = (u16 *)gpulib_get_vram();
    int i, row, ux, uy, uw, uh, up;
    /* primitives still queued in hwr_core are not in the drawn-block map yet: submit
       them first (the Tekken 3 pause screen copied from an area drawn by primitives
       that were still queued, and got the stale core VRAM) */
    SUBWHERE("hwr readback: submitting");
    hwr_flush();
    nrd = 0;
    split_rect(x, y, w, h, read_collect);
    if (!nrd) { SUBWHERE(NULL); return; }
    rsxr_snapshot(&R.base, NULL);              /* drawing done before it is copied */
    for (i = 0; i < nrd; i++)
        rsx_copy(GCM_LOCATION_CELL, R.rb_off, R.rbdep_off, GCM_LOCATION_RSX, R.nat_off, 512, 0,
                 rd[i].x, rd[i].y, rd[i].w, rd[i].h, 0);
    SUBWHERE("hwr readback: waiting for the RSX");
    if (!drain()) { SUBWHERE(NULL); return; }
    SUBWHERE("hwr readback: converting");
    up = gpulib_pending_write(&ux, &uy, &uw, &uh);
    for (i = 0; i < nrd; i++) {
        for (row = 0; row < rd[i].h; row++) {
            int yy = rd[i].y + row, x0 = rd[i].x & ~3, n = ((rd[i].x + rd[i].w + 3) & ~3) - x0;
            u8 *src = R.rb + (yy * 1024 + x0) * 2;
            u16 *c = core + yy * 1024 + x0;
            const char *a = (const char *)((uintptr_t)src & ~(uintptr_t)127), *e = (const char *)src + n * 2;
            int k;
            for (; a < e; a += 128) __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");   /* the RSX wrote it */
            __asm__ volatile("sync" : : : "memory");
            /* only the rect's halfwords; an upload in progress already put newer data there */
            for (k = 0; k < n; k++) {
                int xx = x0 + k;
                if (xx < rd[i].x || xx >= rd[i].x + rd[i].w) continue;
                if (up && ((yy - uy) & 511) < uh && ((xx - ux) & 1023) < uw) continue;
                /* big-endian halfword in the buffer -> little-endian in the core's VRAM
                   (byte by byte: the console self-test caught a host-order store here) */
                ((u8 *)c)[k * 2] = src[k * 2 + 1]; ((u8 *)c)[k * 2 + 1] = src[k * 2];
            }
        }
        mark_drawn(rd[i].x, rd[i].y, rd[i].x + rd[i].w, rd[i].y + rd[i].h, 0);
        R.syncs_out++;
    }
    SUBWHERE(NULL);
    R.sync_ticks += gpulib_prof_now() - t0;
}

/* The game reads VRAM back (GP0 C0h, or a copy the core does itself). What it
   reads must be exact, bit 15 included, and the RSX cannot always give that: on
   consoles where it cannot blend and set bit 15 in one pass, blended pixels have
   bit 15 = 0 (the X-Files builds its cursor from such pixels: wrong colours), and
   the pass that copies them onto themselves with bit 15 (op_bit15 2) is not
   reliable (its start-up test passed and failed on the same console; used, it
   striped the X-Files' menus). So from the first read on, the software renderer
   draws too, as with the VRAM check, and reads come from its exact copy; the RSX
   still draws the picture. That costs the software renderer's CPU time (Soul
   Reaver, which reads once while loading: 80% speed), so it starts only once a
   game reads in 3 frames within 5 s (until then reads come from the RSX, as before)
   and ends after 10 s without reads. */
#define SHADOW_READ_FRAMES 3
#define SHADOW_WINDOW 300
#define SHADOW_IDLE 600
static int cmd_check(const uint32_t *list, const uint32_t *end);
static int cmd_timed(const uint32_t *list, const uint32_t *end);
static int full_vram_copy(int sx, int sy, int dx, int dy, int w, int h, int msb);
extern int gpulib_read_by_cpu;   /* plugins/gpulib/gpu.c */
static void cpu_vram_read(int x, int y, int w, int h)
{
    unsigned f = R.pframes;
    if (R.check || R.shadow) { if (gpulib_read_by_cpu) R.read_last = f; return; }   /* the core's VRAM is exact */
    full_vram_read(x, y, w, h);
    /* only the game's own reads count: copies the core does itself and savestates
       only need the RSX's pixels (as before build 114) */
    if (!gpulib_read_by_cpu) return;
    R.cpu_read = 1;
    if (R.read_n && f == R.read_last) return;               /* this frame is counted */
    if (!R.read_n || f - R.read_win > SHADOW_WINDOW) { R.read_win = f; R.read_n = 0; }
    R.read_last = f;
    if (++R.read_n < SHADOW_READ_FRAMES) return;
    full_vram_read(0, 0, 1024, 512);                         /* the software renderer continues from here */
    R.shadow = 1; R.read_n = 0;
    gpu_frontend_hooks.cmd = cmd_check;
    gpu_frontend_hooks.vram_copy = NULL;
    logf_("hwr: the game reads VRAM back often: the software renderer draws too (exact reads)\n");
}

/* 10 s without reads: the RSX alone again (it drew everything meanwhile too) */
static void shadow_idle_check(void)
{
    if (!R.shadow || R.pframes - R.read_last < SHADOW_IDLE) return;
    R.shadow = 0;
    gpu_frontend_hooks.cmd = cmd_timed;
    gpu_frontend_hooks.vram_copy = full_vram_copy;
    logf_("hwr: no VRAM reads for 10 s: the software renderer stops drawing\n");
}

/* A VRAM-to-VRAM copy (GP0 80h) done by the RSX in the native VRAM: through the
   CPU it cost a readback of the source when the RSX had drawn there (a wait for
   the RSX each time) and an upload of the destination. RE3 makes hundreds per
   frame in some scenes (nearly frozen), Rayman many. The source goes to the
   readback buffer first (it may overlap the destination), then to its place; the
   destination is then newer in the native VRAM than in the core's copy. 0: the
   core copies it itself (wrapping rects, the mask bit set, odd/even mismatch,
   a source the core's VRAM already has, the VRAM check) */
static unsigned gpu_copies;
static int full_vram_copy(int sx, int sy, int dx, int dy, int w, int h, int msb)
{
    u64 t0;
    int bx = sx, by = sy, bw = w, bh = h;
    if (R.dead || R.check || R.shadow || msb || w <= 0 || h <= 0) return 0;
    if (sx + w > 1024 || dx + w > 1024 || sy + h > 512 || dy + h > 512 || ((sx ^ dx) & 1)) return 0;
    if (!drawn_bbox(&bx, &by, &bw, &bh)) return 0;   /* the core's copy of the source is current */
    t0 = gpulib_prof_now();
    hwr_flush();                                   /* draws queued before the copy come first */
    rsxr_snapshot(&R.base, NULL);                  /* ... and are done */
    rsx_copy(GCM_LOCATION_CELL, R.rb_off, R.rbdep_off, GCM_LOCATION_RSX, R.nat_off, 512, 0, sx, sy, w, h, 0);
    rsxr_snapshot(&R.base, NULL);                  /* the buffer written before it is read */
    rsx_copy2(GCM_LOCATION_RSX, R.nat_off, R.ndep_off, GCM_LOCATION_CELL, R.rb_off, 512, 0, sx, sy, dx, dy, w, h, 1);
    rsxr_snapshot(&R.base, NULL);                  /* later readbacks reuse the buffer */
    R.seq++;
    rsxSetWriteBackendLabel(rsx_ctx, HWR_LABEL, R.seq);
    rsxFlushBuffer(rsx_ctx);
    mark_drawn(dx, dy, dx + w, dy + h, 1);         /* newer in the native VRAM now */
    hwr_vram_written(dx, dy, w, h);                /* redrawn into the upscaled target */
    gpu_copies++;
    R.sync_ticks += gpulib_prof_now() - t0;
    return 1;
}

/* the core's VRAM complete (before a savestate, so the core's own hook call finds
   nothing left to read back) */
void hooks_sync_vram_for_state(void)
{
    if (R.on && R.base.full && !R.dead && !R.check && !R.shadow) full_vram_read(0, 0, 1024, 512);
}

/* ---- startup self-test ---- */

static const uint16_t test_pat[16] = { 0x001f, 0x03e0, 0x7c00, 0x0001, 0x0020, 0x0400, 0x7fff, 0x1234,
                                       0x5678, 0x0f0f, 0x70f0, 0x2a55, 0x5aa5, 0x0123, 0x4567, 0x3c3c };

/* draws known VRAM data with the copy, 15-bit, 4-bit and 8-bit programs (and a flat
   colour in full mode), reads the pixels back from the target (and the native
   surface) and logs anything that differs. Returns the number of wrong pixels. */
static int self_test_(int full);
/* With the Texture or Sprite filter on, the test's texels came out blended with
   their neighbours: it failed, and the console fell back to the hybrid renderer
   for the whole session (NFS Porsche: its menu text gone). */
static int self_test(int full)
{
    int r;
    self_testing = 1;
    r = self_test_(full);
    self_testing = 0;
    return r;
}
static int self_test_(int full)
{
    static const char *names[5] = { "VRAM copy", "15-bit texture", "4-bit CLUT", "8-bit CLUT", "flat colour" };
    static const int progs[5] = { 4, 3, 1, 2, 0 };
    uint8_t *v = (uint8_t *)calloc(1, HWR_SNAPSZ);     /* little-endian, like the core's VRAM */
    hwr_vtx vt[30]; hwr_op ops[5];
    int k, i, bad_total = 0, nk = full ? 5 : 4;
    if (!v) return 1;
    for (i = 0; i < 16; i++) {
        v[i * 2] = test_pat[i] & 0xff; v[i * 2 + 1] = test_pat[i] >> 8;    /* row 0: colours (and the CLUT) */
        v[2 * 1024 * 2 + i / 2] = (uint8_t)((i & ~1) | (((i & ~1) + 1) << 4)); /* row 2: 4-bit indexes 0..15 */
        v[3 * 1024 * 2 + i] = (uint8_t)i;                                  /* row 3: 8-bit indexes 0..15 */
    }
    memset(vt, 0, sizeof vt);
    for (k = 0; k < nk; k++) {
        float y0 = 10 + k, y1 = y0 + 1, tv = k == 2 ? 2 : k == 3 ? 3 : 0;
        float q[6][2] = { { 0, 0 }, { 16, 0 }, { 0, 1 }, { 16, 0 }, { 0, 1 }, { 16, 1 } };
        for (i = 0; i < 6; i++) {
            hwr_vtx *o = &vt[k * 6 + i];
            o->x = q[i][0] ? (k == 4 ? 8 : 16) : 0; o->y = q[i][1] ? y1 : y0;
            o->u = q[i][0]; o->v = q[i][1] * 0.999f;   /* stays in texel row 0 */
            o->r = o->g = o->b = 128;
            o->tpx = 0; o->tpy = tv; o->cx = 0; o->cy = 0;
            o->mode = (float)progs[k]; o->raw = 1;
            o->umin = 0; o->vmin = 0; o->umax = 255; o->vmax = 255;
        }
        ops[k].blend = HWR_BLEND_NONE; ops[k].prog = (uint8_t)progs[k]; ops[k].sprite = 0;
        ops[k].sx0 = 0; ops[k].sy0 = 0; ops[k].sx1 = 1024; ops[k].sy1 = 512;
        ops[k].first = k * 6; ops[k].count = 6;
    }
    if (full) {
        /* flat colour 0x5678 as an 8-bit vertex colour, pixels 0..7 of row 14 */
        u32 c = 0x5678, r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        for (i = 0; i < 6; i++) { vt[24 + i].r = r << 3 | r >> 2; vt[24 + i].g = g << 3 | g >> 2; vt[24 + i].b = b << 3 | b >> 2; }
        /* the test data goes into the native VRAM the way an upload does, plus an
           upload that starts and ends inside a pixel pair: row 5, x 1..5 */
        u16 *cv = (u16 *)gpulib_get_vram();
        if (!drain()) { free(v); return 1; }
        memcpy(cv, v, 8 * 1024 * 2);                             /* rows 0..7 of the core's VRAM */
        split_rect(0, 0, 1024, 8, upload_part);
        for (i = 1; i <= 5; i++) cv[5 * 1024 + i] = (u16)(0x8000 | (0x1111 * i)); /* LE in memory: host-swapped below */
        for (i = 1; i <= 5; i++) { u16 t = cv[5 * 1024 + i]; cv[5 * 1024 + i] = (u16)((t >> 8) | (t << 8)); }
        upload_part(1, 5, 5, 1);
        rsxr_snapshot(&R.base, NULL);                           /* barrier */
    } else if (!rsxr_snapshot(&R.base, (const uint16_t *)v)) { free(v); return 1; }
    rsxr_submit(&R.base, ops, nk, vt, nk * 6);
    free(v);
    if (!drain()) { logf_("hwr self-test: RSX did not finish\n"); return 1; }
    for (k = 0; k < nk; k++) {
        int bad = 0, n = k == 4 ? 8 : 16;
        for (i = 0; i < n; i++) {
            int px = i * R.SX + R.SX / 2, py = (int)(((float)(10 + k) + 0.5f) * R.SY);
            u32 got = R.tgt[py * (R.tgt_pitch / 4) + px] & 0xffffff;
            u32 c = k == 4 ? 0x5678 : test_pat[i], r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
            /* the target holds PS1 blue in its red channel */
            u32 want = ((b * 255 + 15) / 31) << 16 | ((g * 255 + 15) / 31) << 8 | ((r * 255 + 15) / 31);
            int dr = (int)(got >> 16) - (int)(want >> 16), dg = (int)((got >> 8) & 255) - (int)((want >> 8) & 255),
                db = (int)(got & 255) - (int)(want & 255);
            if (dr * dr > 4 || dg * dg > 4 || db * db > 4) {
                if (bad < 4) logf_("hwr self-test: %s texel %d (VRAM %04x): target got %06x, want %06x\n",
                                   names[k], i, (unsigned)c, (unsigned)got, (unsigned)want);
                bad++;
            }
            if (full && k != 0) {      /* the copy program is not drawn into the native VRAM */
                const u8 *p = R.nat + ((10 + k) * 1024 + i) * 2;
                u32 nat = (u32)p[0] << 8 | p[1];
                if (nat != (c & 0x7fff)) {
                    if (bad < 4) logf_("hwr self-test: %s texel %d: native VRAM got %04x, want %04x\n",
                                       names[k], i, (unsigned)nat, (unsigned)(c & 0x7fff));
                    bad++;
                }
            }
        }
        if (bad) logf_("hwr self-test (%s mode): %s: %d wrong\n", full ? "full" : "hybrid", names[k], bad);
        bad_total += bad;
    }
    if (full) {     /* the partial upload: x 0, 6, 7 untouched (0), 1..5 with bit 15 kept */
        int bad = 0;
        for (i = 0; i < 8; i++) {
            const u8 *p = R.nat + (5 * 1024 + i) * 2;
            u32 got = (u32)p[0] << 8 | p[1], want = (i >= 1 && i <= 5) ? 0x8000 | (0x1111 * i) : 0;
            if (got != want) { if (bad < 4) logf_("hwr self-test: upload x %d: native VRAM got %04x, want %04x\n", i, (unsigned)got, (unsigned)want); bad++; }
        }
        if (bad) logf_("hwr self-test (full mode): partial-pixel upload: %d wrong\n", bad);
        bad_total += bad;
    }
    if (full) {     /* readback through the RSX: rows 11..13 (texture tests) into the core's VRAM */
        const u8 *cv8 = (const u8 *)gpulib_get_vram();
        int bad = 0, r;
        mark_drawn(0, 10, 16, 15, 1);
        full_vram_read(0, 11, 16, 3);
        for (r = 11; r <= 13; r++)
            for (i = 0; i < 16; i++) {
                const u8 *p = cv8 + (r * 1024 + i) * 2;
                u32 got = p[0] | p[1] << 8, want = test_pat[i] & 0x7fff;
                if (got != want) { if (bad < 4) logf_("hwr self-test: readback row %d x %d: got %04x, want %04x\n", r, i, (unsigned)got, (unsigned)want); bad++; }
            }
        if (bad) logf_("hwr self-test (full mode): readback: %d wrong\n", bad);
        bad_total += bad;
    }
    if (full && !bad_total) {
        /* bit 15: an opaque 15-bit texture with STP texels (row 5: 0, 9111, a222, b333,
           c444, d555, 0, 0) drawn over row 15 x 0..7, then in the same op a quad with
           STP=0 texels (row 0) over x 3..4: the later quad must stay on top across the
           two native draws. 0000 texels are transparent and leave the 0s there. */
        static const u16 want[8] = { 0, 0x9111, 0xa222, 0x001f, 0x03e0, 0xd555, 0, 0 };
        hwr_vtx bt[12]; hwr_op bo;
        int bad = 0;
        float qx[6] = { 0, 1, 0, 1, 0, 1 }, qy[6] = { 0, 0, 1, 0, 1, 1 };
        memset(bt, 0, sizeof bt); memset(&bo, 0, sizeof bo);
        for (i = 0; i < 12; i++) {
            hwr_vtx *o = &bt[i];
            int b = i >= 6;
            float fx = qx[i % 6], fy = qy[i % 6];
            o->x = b ? 3 + 2 * fx : 8 * fx; o->y = 15 + fy;
            o->u = b ? 2 * fx : 8 * fx; o->v = fy * 0.999f;
            o->r = o->g = o->b = 128;
            o->tpy = b ? 0 : 5; o->mode = 3; o->raw = 1;
            o->umin = 0; o->vmin = 0; o->umax = 255; o->vmax = 255;
        }
        bo.blend = HWR_BLEND_NONE; bo.prog = 3; bo.sprite = 0; bo.sx0 = 0; bo.sy0 = 0; bo.sx1 = 1024; bo.sy1 = 512;
        bo.first = 0; bo.count = 12;
        {
            /* fix-up variants: the first two are usable (the second redraws the colour, so
               it skips blended ops); the rest only tell from the log which step fails */
            static const struct { int blend, alpha, depth; const char *name; } V[] = {
                { 1, 1, 1, "blend keep + alpha test + depth equal" },
                { 0, 1, 1, "redraw colour + alpha test + depth equal" },
                { 1, 1, 0, "blend keep + alpha test, no depth test (diagnostic)" },
                { 1, 0, 0, "blend keep only (diagnostic)" },
                { 0, 0, 0, "redraw colour only (diagnostic)" },
            };
            int v, chosen = -1;
            u16 *cv = (u16 *)gpulib_get_vram();
            for (v = 0; v < (int)(sizeof V / sizeof V[0]); v++) {
                char got_s[64]; int n = 0;
                bad = 0;
                for (i = 0; i < 8; i++) cv[15 * 1024 + i] = 0;      /* row 15 back to zero */
                upload_part(0, 15, 8, 1);
                R.bit15 = 1; R.fix_blend = V[v].blend; R.fix_alpha = V[v].alpha; R.fix_depth = V[v].depth;
                rsxr_snapshot(&R.base, NULL);
                rsxr_submit(&R.base, &bo, 1, bt, 12);
                if (!drain()) return 1;
                for (i = 0; i < 8; i++) {
                    const u8 *p = R.nat + (15 * 1024 + i) * 2;
                    u32 got = (u32)p[0] << 8 | p[1];
                    if (got != want[i]) bad++;
                    if (i >= 1 && i <= 5) n += snprintf(got_s + n, sizeof got_s - n, " %04x", (unsigned)got);
                }
                logf_("hwr self-test: bit 15 fix-up %s: %s (x 1..5:%s, want 9111 a222 001f 03e0 d555)\n",
                      V[v].name, bad ? "wrong" : "OK", got_s);
                if (!bad && chosen < 0 && v < 2) chosen = v;
            }
            R.bit15 = chosen >= 0;
            if (chosen >= 0) {
                R.fix_blend = V[chosen].blend; R.fix_alpha = 1; R.fix_depth = 1;
                logf_("hwr self-test (full mode): bit 15 (STP/mask bit) kept in the native VRAM (%s)\n", V[chosen].name);
            } else logf_("hwr self-test (full mode): bit 15 not kept: drawing stores it as 0, like before\n");
        }
        R.self_ok = 0;
        if (R.bit15 && !R.fix_blend) {
            /* the self copy (op_bit15 2): a blended op (add) with STP texels (row 5)
               over row 16 x 0..7, which holds 0: the colours stay the texels', and
               bit 15 must come out set. Only a log line and a flag: games that read
               VRAM back use it only if it passes (build 111 used it untested: on a
               console it striped blended pixels) */
            static const u16 want2[8] = { 0, 0x9111, 0xa222, 0xb333, 0xc444, 0xd555, 0, 0 };
            hwr_op so = bo;
            char got_s[64]; int n = 0, sbad = 0, keep = R.cpu_read;
            u16 *cv = (u16 *)gpulib_get_vram();
            for (i = 0; i < 8; i++) cv[16 * 1024 + i] = 0;
            upload_part(0, 16, 8, 1);
            for (i = 0; i < 12; i++) { bt[i].y += 1.f; bt[i].pass = 2; }
            so.blend = HWR_BLEND_ADD; so.pass = 2; so.count = 6;     /* the first quad only */
            R.cpu_read = 1; R.self_ok = 1;
            rsxr_snapshot(&R.base, NULL);
            rsxr_submit(&R.base, &so, 1, bt, 6);
            R.cpu_read = keep;
            if (!drain()) return 1;
            for (i = 0; i < 8; i++) {
                const u8 *p = R.nat + (16 * 1024 + i) * 2;
                u32 got = (u32)p[0] << 8 | p[1];
                if (got != want2[i]) sbad++;
                n += snprintf(got_s + n, sizeof got_s - n, " %04x", (unsigned)got);
            }
            R.self_ok = !sbad;
            logf_("hwr self-test: bit 15 self copy of blended pixels: %s (x 0..7:%s, want 0000 9111 a222 b333 c444 d555 0000 0000)\n",
                  sbad ? "wrong, not used" : "OK", got_s);
            for (i = 0; i < 8; i++) cv[16 * 1024 + i] = 0;
            upload_part(0, 16, 8, 1);
        }
    }
    if (!bad_total) logf_("hwr self-test (%s mode): all paths match the PS1\n", full ? "full" : "hybrid");
    return bad_total;
}

/* ---- VRAM check (debug) ---- */

/* the software renderer keeps drawing into the core's VRAM as a reference */
static int cmd_check(const uint32_t *list, const uint32_t *end)
{
    if (T.on) {                       /* raw words as in PS1 memory (little-endian) */
        const uint32_t *p;
        tr_put(1); tr_put((u32)(end - list));
        for (p = list; p < end; p++) { u32 v = *p; tr_put(v >> 24 | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | v << 24); }
    }
    hwr_cmd(list, end);
    return 0;
}

static void bmp_header(FILE *f)
{
    unsigned char h[54] = { 'B', 'M' };
    u32 size = 54 + 1024 * 512 * 3, v[] = { size, 0, 54, 40, 1024, 512 };
    int i;
    for (i = 0; i < 6; i++) { h[2 + i * 4] = v[i]; h[3 + i * 4] = v[i] >> 8; h[4 + i * 4] = v[i] >> 16; h[5 + i * 4] = v[i] >> 24; }
    h[26] = 1; h[28] = 24;
    fwrite(h, 1, 54, f);
}

/* writes the native VRAM (as the RSX drew it), the core's VRAM (as the software
   renderer drew it, when the check is on) and a difference map as BMP files, and
   logs which 64x32 blocks differ and how */
int hooks_dump_vram(const char *name)
{
    const u8 *core = (const u8 *)gpulib_get_vram();
    static unsigned short cnt[16][16], b15[16][16];
    static u32 first[16][16][3];
    char path[3][600];
    FILE *f[3];
    unsigned char row[1024 * 3 * 3];
    int x, y, k, total = 0, total15 = 0, num = 1;
    if (!R.on || !R.base.full || R.dead) { logf_("vram dump: only with the RSX renderer in full mode\n"); return 0; }
    hwr_flush();
    rsxr_snapshot(&R.base, NULL);
    rsx_copy(GCM_LOCATION_CELL, R.rb_off, R.rbdep_off, GCM_LOCATION_RSX, R.nat_off, 512, 0, 0, 0, 1024, 512, 0);
    if (!drain()) return 0;
    {
        const char *a = (const char *)R.rb, *e = a + 1024 * 1024;
        for (; a < e; a += 128) __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
        __asm__ volatile("sync" : : : "memory");
    }
    mkdir(APP_DIR "/dumps", 0777);
    {   /* numbered: <game>_<n>_rsx/_software/_diff.bmp and _gp0.bin, n = the first free number,
           so later dumps do not overwrite earlier ones and each trace keeps its pictures */
        struct stat st;
        for (num = 1; num < 10000; num++) {
            snprintf(path[0], sizeof path[0], APP_DIR "/dumps/%s_%d_rsx.bmp", name, num);
            if (stat(path[0], &st) != 0) break;
        }
    }
    snprintf(path[1], sizeof path[1], APP_DIR "/dumps/%s_%d_software.bmp", name, num);
    snprintf(path[2], sizeof path[2], APP_DIR "/dumps/%s_%d_diff.bmp", name, num);
    for (k = 0; k < 3; k++) { f[k] = fopen(path[k], "wb"); if (f[k]) bmp_header(f[k]); }
    memset(cnt, 0, sizeof cnt); memset(b15, 0, sizeof b15);
    for (y = 511; y >= 0; y--) {       /* BMP rows run bottom-up */
        for (x = 0; x < 1024; x++) {
            u32 n = (u32)R.rb[(y * 1024 + x) * 2] << 8 | R.rb[(y * 1024 + x) * 2 + 1];
            u32 c = core[(y * 1024 + x) * 2] | (u32)core[(y * 1024 + x) * 2 + 1] << 8;
            int d = 0, i;
            unsigned char *p0 = row + x * 3, *p1 = row + 3072 + x * 3, *p2 = row + 6144 + x * 3;
            for (i = 0; i < 3; i++) {
                int a = (n >> (i * 5)) & 31, b = (c >> (i * 5)) & 31;
                if (a - b > 1 || b - a > 1) d = 1;
            }
            /* BMP pixels are blue, green, red */
            p0[0] = ((n >> 10) & 31) << 3; p0[1] = ((n >> 5) & 31) << 3; p0[2] = (n & 31) << 3;
            p1[0] = ((c >> 10) & 31) << 3; p1[1] = ((c >> 5) & 31) << 3; p1[2] = (c & 31) << 3;
            if (d) { p2[0] = 0; p2[1] = 0; p2[2] = 255; }                       /* colour differs: red */
            else if ((n ^ c) & 0x8000) { p2[0] = 255; p2[1] = 255; p2[2] = 0; } /* only bit 15: cyan */
            else { p2[0] = p1[0] >> 2; p2[1] = p1[1] >> 2; p2[2] = p1[2] >> 2; } /* same: dimmed */
            if (d || ((n ^ c) & 0x8000)) {
                int bx = x >> 6, by = y >> 5;
                if (!cnt[by][bx] && !b15[by][bx]) { first[by][bx][0] = (u32)(x | y << 16); first[by][bx][1] = n; first[by][bx][2] = c; }
                if (d) { cnt[by][bx]++; total++; } else { b15[by][bx]++; total15++; }
            }
        }
        for (k = 0; k < 3; k++) if (f[k]) fwrite(row + k * 3072, 1, 3072, f[k]);
    }
    for (k = 0; k < 3; k++) if (f[k]) fclose(f[k]);
    logf_("vram dump: %s (and _software, _diff); %d pixels differ in colour, %d only in bit 15%s\n", path[0],
          total, total15, R.check ? "" : " (VRAM check is off: the software copy is not a reference)");
    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            if (cnt[y][x] + b15[y][x] >= 16)
                logf_("vram dump: block x %d-%d y %d-%d: %u colour, %u bit-15 differences; first at %u,%u: rsx %04x software %04x\n",
                      x * 64, x * 64 + 63, y * 32, y * 32 + 31, cnt[y][x], b15[y][x],
                      (unsigned)(first[y][x][0] & 0xffff), (unsigned)(first[y][x][0] >> 16),
                      (unsigned)first[y][x][1], (unsigned)first[y][x][2]);
    if (R.check && !T.on) {            /* and a GP0 trace of the next frame */
        snprintf(T.path, sizeof T.path, APP_DIR "/dumps/%s_%d_gp0.bin", name, num);
        T.armed = 1;
        logf_("vram dump %d: the next frame's GP0 trace goes to %s\n", num, T.path);
    } else if (R.check)
        logf_("vram dump %d: no GP0 trace (the previous dump's trace is still being recorded)\n", num);
    return f[0] != NULL ? num : 0;
}

/* ---- gpu_frontend_hooks ---- */
static int present_hook(int x, int y, int w, int h, int bgr24)
{
    int r;
    u64 t0 = gpulib_prof_now();
    tr_frame(x, y, w, h);
    R.pframes++;
    shadow_idle_check();
    r = hwr_present(x, y, w, h, bgr24);
    /* the core shows this frame from its own VRAM: bring the area up to date */
    if (!r && R.base.full && !R.dead && !R.check && !R.shadow) full_vram_read(x, y, bgr24 ? (w * 3 + 1) / 2 : w, h);
    R.t_present += gpulib_prof_now() - t0;
    return r;
}

/* hwr_cmd with its time counted */
static int cmd_timed(const uint32_t *list, const uint32_t *end)
{
    u64 t0;
    int r;
    if (!cfg.debug_log) return hwr_cmd(list, end);   /* timed only for the debug log */
    t0 = gpulib_prof_now();
    r = hwr_cmd(list, end);
    R.t_cmd += gpulib_prof_now() - t0;
    return r;
}

/* set by the front end while the BIOS shows its logo: the software renderer draws
   it (the RSX renderer was GPU-bound there: under 45 fps, broken sound) */
int hwr_boot_hold;

static void start(void)
{
    static int tested;
    int at;
    if (R.dead || R.on || hwr_boot_hold) return;
    at = cfg_auto_target_h();
    if (!cfg.gpu_scale && !at) return;
    if (!alloc_once()) return;
    gpulib_set_thread_rendering(0);     /* the hooks below must run on this thread */
    /* auto: start at a safe 2x; scale_for/apply_scale corrects it within 3 frames */
    R.S = at ? 4 : cfg.gpu_scale;
    if (R.S == 1) R.S = 2;   /* 0.5x is not a scale (a hand-edited file): 1x instead */
    scale_from_code(R.S, &R.SX, &R.SY);
    want.sx = R.SX; want.sy = R.SY; want.frames = 0;
    R.base.snapshot = rsxr_snapshot; R.base.submit = rsxr_submit; R.base.present = rsxr_present;
    R.base.vtx_area = rsxr_vtx_area;
    R.cpu_waits = R.syncs_in = R.syncs_out = 0; R.sync_ticks = 0; gpu_copies = 0;
    R.mask_used = R.mask_resync = R.base.mask_stencil = 0;
    R.cpu_read = 0; R.shadow = 0; R.read_n = 0;
    if (!tested) {
        tested = 1;
        R.base.full = 1;
        R.bit15 = 0;           /* the basic tests run without it; its own test turns it on */
        R.fast = 1;            /* programs where the texture unit decodes VRAM, if they pass */
        R.full_ok = self_test(1) == 0;
        if (!R.full_ok && !R.dead) {
            logf_("hwr: the fast programs failed the self-test, trying the byte-decoding ones\n");
            R.fast = 0;
            R.full_ok = self_test(1) == 0;
        }
        if (!R.full_ok && !R.dead) {
            logf_("hwr: full mode failed its self-test, using the hybrid renderer\n");
            R.base.full = 0;
            self_test(0);
        }
        if (R.dead) return;
    }
    R.base.full = R.full_ok;
    /* start from a black target */
    set_surface(0);
    rsxSetScissor(rsx_ctx, 0, 0, 1024 * R.SX, (int)(512.f * R.SY + 0.5f));
    rsxSetClearColor(rsx_ctx, 0);
    rsxClearSurface(rsx_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
    if (R.base.full) {
        /* the software renderer drew everything so far */
        if (!drain()) return;
        sync_rect(0, 0, 1024, 512, 1);
        memset(drawn, 0, sizeof drawn);
    }
    hwr_attach(&R.base, gpulib_get_vram(), NULL);
    hwr_sync_ecmds(gpulib_get_ex_regs());
    R.check = R.base.full && cfg.vram_check;
    gpu_frontend_hooks.cmd = R.check ? cmd_check : cmd_timed;
    gpu_frontend_hooks.vram_written = R.base.full ? full_vram_written : hwr_vram_written;
    gpu_frontend_hooks.vram_read = R.base.full ? cpu_vram_read : NULL;   /* (VRAM check: only notes the read) */
    gpu_frontend_hooks.vram_copy = R.base.full && !R.check ? full_vram_copy : NULL;
    gpu_frontend_hooks.sync_ecmds = hwr_sync_ecmds;
    gpu_frontend_hooks.present = present_hook;
    R.on = 1;
    /* the self-test ran at the first game start, maybe into another game's log */
    logf_("hwr: self-test: full mode %s, bit 15 %s, %s programs\n", R.full_ok ? "passed" : "FAILED",
          !R.full_ok ? "-" : R.bit15 ? "kept" : "NOT kept (failed its test)", R.fast ? "fast" : "byte-decoding");
    logf_("hwr: GPU renderer on, %dx wide, %gx tall (%dx%d), %s mode\n", R.SX, R.SY, 1024 * R.SX, (int)(512.f * R.SY + 0.5f),
          R.base.full ? (R.check ? "full, with the VRAM check (software renderer draws too)" : "full (the RSX draws everything)")
                      : "hybrid (software renderer + RSX)");
    logf_("hwr: texture filter %s\n", cfg.texfilter == 2 ? "bilinear (performance: hardware 15-bit, manual CLUT, upscaled pass only)" :
          cfg.texfilter ? "bilinear (upscaled pass only)" : "nearest");
}

static void log_stats(void)
{
    /* polygon timing (hwr_core.c): one in 8 timed, the log multiplies by 8 */
    extern unsigned long long hwr_poly_t_all, hwr_poly_t_parse, hwr_poly_t_shift, hwr_poly_t_setup, hwr_poly_t_emit;
    extern unsigned int hwr_poly_n;
    if (!R.on) return;
    logf_("hwr: frames %u prims %u %s %u (max %u/frame) software fallbacks %u overflow %u cpu-waits %u%s\n",
          hwr_stats.frames, hwr_stats.prims, R.base.full ? "barriers" : "snapshots", hwr_stats.snapshots,
          hwr_stats.max_snaps, hwr_stats.fallbacks, hwr_stats.overflow_frames, R.cpu_waits,
          hwr_stats.mask_checks && !R.base.full ? " (mask-check drawing: not emulated in hybrid mode)" : "");
    if (hwr_stats.mask_checks) logf_("hwr: %u primitives drawn with the mask check on\n", hwr_stats.mask_checks);
    {   /* PGXP-lite (hwr_core.c), counted since the last line */
        extern unsigned int hwr_pgxp_polys, hwr_pgxp_misses, hwr_pgxp_persp, hwr_pgxp_flat, hwr_pgxp_misfit, hwr_pgxp_alone, hwr_pgxp_amb;
        extern unsigned int pgxp_lite_addr_hits, pgxp_lite_hits, pgxp_lite_welds;
        extern unsigned int pgxp_nclip_total, pgxp_nclip_flip, pgxp_nclip_restore, pgxp_nclip_nogte;
        extern unsigned int pgxp_nclip_guess, pgxp_nclip_dzero;
        if (pgxp_lite_on)
            logf_("hwr: PGXP: %u polygons at sub-pixel positions, %u at whole pixels (not from the GTE; of those %u with one corner found alone); corners found by address %u, by value %u; %u polygons with a corner standing for two vertices; %u vertices welded to a neighbour\n",
                  hwr_pgxp_polys, hwr_pgxp_misses, hwr_pgxp_alone, pgxp_lite_addr_hits, pgxp_lite_hits, hwr_pgxp_amb, pgxp_lite_welds);
        {
            extern unsigned int hwr_pgxp_grown; extern float hwr_pgxp_grow;
            if (pgxp_lite_on) logf_("hwr: PGXP seam fill %.2f pixel: %u polygons drawn larger\n", hwr_pgxp_grow, hwr_pgxp_grown);
            hwr_pgxp_grown = 0;
        }
        pgxp_lite_addr_hits = pgxp_lite_hits = pgxp_lite_welds = 0;
        if (pgxp_cull_track)
            logf_("hwr: PGXP culling correction: %u triangles recomputed, %u flipped, %u restored (integer winding was zero), %u fell back (not in the lite table), %u with a guessed corner, %u with zero precise winding\n",
                  pgxp_nclip_total, pgxp_nclip_flip, pgxp_nclip_restore, pgxp_nclip_nogte, pgxp_nclip_guess, pgxp_nclip_dzero);
        pgxp_nclip_total = pgxp_nclip_flip = pgxp_nclip_restore = pgxp_nclip_nogte = 0;
        pgxp_nclip_guess = pgxp_nclip_dzero = 0;
        if (pgxp_lite_on >= 2)
            logf_("hwr: PGXP textures: %u polygons in perspective (of those %u with a corner depth repaired), %u flat\n",
                  hwr_pgxp_persp, hwr_pgxp_misfit, hwr_pgxp_flat);
        {   /* PGXP's share of the polygon time below (the polygon line itself is in the debug block) */
            extern unsigned long long hwr_pgxp_t_find, hwr_pgxp_t_pick, hwr_pgxp_t_depth, hwr_pgxp_t_seam, pgxp_lite_ticks;
            static unsigned pg_last;
            double n = hwr_stats.frames - pg_last, k, k8;
            if (n < 1) n = 1;
            k = 1000.0 / (double)sysGetTimebaseFrequency() / n;
            k8 = k * 8;   /* the renderer times one polygon in 8 (PG_SAMPLE in hwr_core.c) */
            pg_last = hwr_stats.frames;
            if (pgxp_lite_on)
                logf_("hwr: PGXP ms per frame: finding corners %.2f, choosing between vertices %.2f, depths %.2f, seam fill %.2f; working out the GTE's projections %.2f\n",
                      hwr_pgxp_t_find * k8, hwr_pgxp_t_pick * k8, hwr_pgxp_t_depth * k8, hwr_pgxp_t_seam * k8, pgxp_lite_ticks * k);
            hwr_pgxp_t_find = hwr_pgxp_t_pick = hwr_pgxp_t_depth = hwr_pgxp_t_seam = 0;
            pgxp_lite_ticks = 0;
        }
        hwr_pgxp_polys = hwr_pgxp_misses = hwr_pgxp_persp = hwr_pgxp_flat = hwr_pgxp_misfit = hwr_pgxp_alone = hwr_pgxp_amb = 0;
    }
    if (R.base.full)
        logf_("hwr: VRAM syncs: %u CPU->RSX, %u RSX->CPU, %.1f ms total; %u VRAM copies done by the RSX\n", R.syncs_in, R.syncs_out,
              R.sync_ticks * 1000.0 / (double)sysGetTimebaseFrequency(), gpu_copies);
    if (cfg.debug_log) {   /* per frame since the last line (submit, copy and waits are inside the commands) */
        static unsigned last_frames;
        double k = 1000.0 / (double)sysGetTimebaseFrequency(), n = hwr_stats.frames - last_frames;
        if (n < 1) n = 1;
        logf_("hwr: ms per frame: commands %.2f (drawing ops sent %.2f, of that copying vertices %.2f, "
              "upscaled pass %.2f, native pass %.2f), waiting for the RSX %.2f, frame end %.2f\n",
              R.t_cmd * k / n, R.t_submit * k / n, R.t_copy * k / n, R.t_up * k / n, R.t_nat * k / n,
              R.t_wait * k / n, R.t_present * k / n);
        {   /* polygons: the sampled ticks x8 estimate all of them */
            double k8 = k * 8;
            logf_("hwr: polygons per frame %.0f, ms per frame: all %.2f (reading %.2f, sample shift %.2f, set-up and submits %.2f, vertices out %.2f)\n",
                  hwr_poly_n / n, hwr_poly_t_all * k8 / n, hwr_poly_t_parse * k8 / n, hwr_poly_t_shift * k8 / n, hwr_poly_t_setup * k8 / n, hwr_poly_t_emit * k8 / n);
        }
        last_frames = hwr_stats.frames;
    }
    R.t_cmd = R.t_submit = R.t_copy = R.t_wait = R.t_present = R.t_up = R.t_nat = 0;
    hwr_poly_t_all = hwr_poly_t_parse = hwr_poly_t_shift = hwr_poly_t_setup = hwr_poly_t_emit = 0; hwr_poly_n = 0;
}

/* a notice for the screen when the full renderer could not be used, else NULL */
const char *hooks_gpu_notice(void)
{
    if (R.on && !R.base.full) return "RSX full renderer failed its self-test: using the slower hybrid renderer (see the log)";
    if (R.dead) return "RSX renderer stopped responding: using the software renderer (see the log)";
    return NULL;
}

/* called every 10 s from the game loop */
void hooks_log_gpu_stats(void) { log_stats(); }

static void stop(void)
{
    if (!R.on) return;
    log_stats();
    memset(&gpu_frontend_hooks, 0, sizeof gpu_frontend_hooks);
    hwr_detach();
    if (R.base.full && !R.check && !R.shadow)
        full_vram_read(0, 0, 1024, 512);   /* the software renderer continues from here */
    else
        drain();
    R.on = 0;
}

void hooks_init(void) {}
void hooks_game_started(void) { start(); }
void hooks_game_stopping(void) { stop(); }

void hooks_apply_gpu_settings(void)
{
    /* auto: the manual scale is ignored while a target is set, so changing it
       must not restart the renderer (R.S is the computed scale code then) */
    if (R.on && ((!cfg_auto_target_h() && cfg.gpu_scale != R.S) || (R.base.full && R.check != cfg.vram_check))) stop();
    if (!R.on && (cfg.gpu_scale || cfg_auto_target_h())) start();
    if (!R.on) gpulib_set_thread_rendering(cfg.gpu_thread);
}

void hooks_draw_gpu_frame(void)
{
    float TW = 1024.f * HWR_MAXS, TH = (float)HWR_ROWS, SX = (float)R.SX, SY = R.SY;
    video_set_dither(cfg.dithering, TW / SX, TH / SY);
    video_draw_rsx_texture(R.tgt_off, 1024 * HWR_MAXS, HWR_ROWS, R.tgt_pitch,
                           R.px * SX / TW, R.py * SY / TH, (R.px + R.pw) * SX / TW, (R.py + R.ph) * SY / TH);
    apply_scale();
    if (R.mask_resync && R.on && !R.dead) {
        /* first mask check: the stencil was not kept until now; one full copy from the
           native VRAM gives every target pixel its bit 15 (the copy ops set it) */
        R.mask_resync = 0;
        hwr_attach_vram(gpulib_get_vram());
    }
}

/* the scale the picture is drawn at now (for the settings menu) */
void hooks_gpu_scale(int *sx, float *sy) { *sx = R.on ? R.SX : 0; *sy = R.on ? R.SY : 0.f; }

/* VRAM syncs so far and the timebase ticks they took (for the slow-frame log;
   both restart when the renderer is switched on again) */
void hwr_rsx_sync_stats(unsigned *n, uint64_t *ticks)
{
    *n = R.syncs_in + R.syncs_out;
    *ticks = R.sync_ticks;
}
