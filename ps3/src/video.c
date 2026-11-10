/* RSX video output.
 *
 * Three display buffers in RSX memory (two if the third does not fit), flipped on vsync. Each display frame the
 * game picture (a texture in mapped main memory, or the GPU renderer's target in
 * RSX memory) is drawn as a quad into the back buffer, then the CPU-drawn overlay
 * is blended on top. The CPU waits for the previous flip before drawing into the
 * next display buffer. The RSX is not told to wait for the flip right after
 * queueing it: the GPU renderer's work for the next frame does not touch the
 * display buffers, and waiting there idled the RSX until the next vsync every
 * frame (the console log showed 3x + FXAA locked to 30 fps by it). It waits
 * just before the next display pass instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>
#include <rsx/rsx.h>
#include <rsx/rsx_program.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include <time.h>
#include <sys/stat.h>
#include "common.h"
#include "screenshot.h"
#include "shaders.h"

#define CB_SIZE     0x100000
#define HOST_SIZE   (32 * 1024 * 1024)

/* game frame: two 1024x512 XRGB8888 buffers in mapped main memory. The core
   emulates the next frame while the RSX may still be showing the last one, so it
   always renders into the buffer the last queued display frame did not use. */
#define FRAME_W     1024
#define FRAME_H     512
#define FRAME_PITCH (FRAME_W * 4)
#define FRAME_SIZE  (FRAME_PITCH * FRAME_H)

gcmContextData *rsx_ctx;
static u32 scr_w, scr_h;
static float scr_aspect = 16.f / 9.f;  /* shape of the TV picture */
/* Triple buffering: the next frame is drawn into the buffer that is neither on
   screen (disp) nor waiting for its flip (pend), so the CPU no longer waits for
   the refresh before drawing; it waits only when queueing its flip, if the
   previous one has not happened yet (lv2 reports one pending flip). Waiting
   before drawing cost 5-7 ms a frame (GT2 at 4x: 45 of 60 refreshes got a
   picture). With two buffers it waits before drawing, as before. */
static struct { u32 *ptr; u32 off; } fb[3];
static int nbuf = 2;
static u32 depth_off;
static int cur = -1, disp = 0, pend = -1;
/* PNG screenshot (pause menu "Save Screenshot"): one-shot. The menu sets the
   request and closes; the next clean presented game frame arms it (its buffer
   is the picture without any menu); the present after that reads the armed
   buffer, now on screen and fully drawn, and writes the PNG. shot_name is
   the game's serial (or ROM name) for the file name, taken at request time. */
static int shot_req, shot_armed;
static char shot_name[128];
static int shown;               /* game frame buffer read by the last queued display frame */
/* timing stats. No RSX timestamps: PSL1GHT's rsxSetTimeStamp writes past the space
   it reserves and leaves a garbage word in the command buffer, which hangs the RSX. */
static struct { unsigned frames, gpu_late; u64 t0, wait_us, wait_max, gap_max, last, gpu_wait_us; } vs;
/* frame-end label: written by the RSX when it has drawn everything before the
   flip, so the log can tell whether the CPU or the RSX limits the frame rate */
#define FRAME_LABEL 202
static volatile u32 *frame_label; static u32 frame_seq;

static u8 *frames; static u32 frames_off;
static u32 *ovl; static u32 ovl_off; static int ovl_full_flush = 1;

static rsxVertexProgram *vp; static void *vp_ucode;
static rsxFragmentProgram *fp_blit, *fp_fxaa, *fp_blitd, *fp_fxaad, *fp_color, *fp_colord;
static int fxaa_test(void);
static void bind_texture(u32 unit, u8 location, u32 offset, unsigned w, unsigned h, u32 pitch, int linear, int swap_rb);
static void quad_clip(float X0, float Y0, float X1, float Y1, float u0, float v0, float u1, float v1,
                      float tx, float ty, int prog);
static u32 fp_blit_off, fp_fxaa_off, fp_blitd_off, fp_fxaad_off, fp_color_off, fp_colord_off, blit_unit, fxaa_unit, color_unit;
/* colour correction (Salsa64/rsxkit mechanism, as the CRT filter used): the
   RSX has no fragment-program constant registers — a const's value lives
   inside the microcode, one copy per instruction that reads it. So the
   colour values patch a CELL shadow copy of the ucode (via the const's
   offset table) and, when anything changed, the shadow is copied into the
   RSX buffer; the next rsxLoadFragmentProgramLocation re-reads it.
   rsxSetFragmentProgramParameter is NOT used: it never delivered a value.
   Two consts (cc_params float4 + cc_gamma float), two programs (plain +
   dithered); the patch runs only when a slider moved, never per frame.
   color_ok is 0 when the programs failed to load: colour never engages and
   the picture is today's. */
static void *color_buf, *colord_buf; static u32 *color_shadow, *colord_shadow;
static u32 color_ucode_size, colord_ucode_size; static int color_ok;
static int cc_applied[5], cc_have_applied;
/* dithering of the RSX renderer's picture (video_set_dither): PS1 pixels per
   texture unit of the game texture; quick: menu redraws skip the smoothing */
static int dith_on, quick; static float dith_nw, dith_nh;

/* vertex slots in RSX memory: per display buffer, up to 4 quads per frame */
#define QUAD_FLOATS 32   /* 4 vertices * (pos2 + uv2 + texel2 + native2) */
#define QUADS 8          /* per display buffer and frame */
static float *vbuf; static u32 vbuf_off; static int quad_n;

unsigned video_width(void) { return scr_w; }
unsigned video_height(void) { return scr_h; }

/* Pause menu "Save Screenshot": only a request. The capture itself runs in
   video_present, on the next clean presented frame after the menu closes,
   so the picture has no menu on it. A request that is never armed (flips
   kept failing) stays pending until it is armed or cancelled. */
void video_request_screenshot(const char *name)
{
    snprintf(shot_name, sizeof shot_name, "%s", name && name[0] ? name : "unknown");
    shot_req = 1;
    shot_armed = 0;
}

void video_cancel_screenshot(void)
{
    shot_req = 0;
    shot_armed = 0;
}

void video_flush_cpu_writes(const void *p, size_t n)
{
    const char *a = (const char *)((uintptr_t)p & ~(uintptr_t)127), *e = (const char *)p + n;
    for (; a < e; a += 128) __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
}

static u32 load_fp(rsxFragmentProgram *p, u32 *unit, void **buf)
{
    void *uc, *m; u32 sz, off;
    rsxProgramAttrib *a;
    rsxFragmentProgramGetUCode(p, &uc, &sz);
    m = rsxMemalign(64, sz);
    if (!m) return 0;
    memcpy(m, uc, sz);
    rsxAddressToOffset(m, &off);
    a = rsxFragmentProgramGetAttrib(p, "tex");
    *unit = a ? a->index : 0;
    if (buf) *buf = m;
    return off;
}

int video_init(void)
{
    void *host = memalign(1024 * 1024, HOST_SIZE);
    videoState st; videoResolution res; videoConfiguration vc;
    u32 sz; int i;

    if (!host || rsxInit(&rsx_ctx, CB_SIZE, HOST_SIZE, host) != 0 || !rsx_ctx) return 0;
    if (videoGetState(0, 0, &st) != 0 || st.state != 0) return 0;
    if (videoGetResolution(st.displayMode.resolution, &res) != 0) return 0;
    memset(&vc, 0, sizeof vc);
    vc.resolution = st.displayMode.resolution;
    vc.format = VIDEO_BUFFER_FORMAT_XRGB;
    vc.pitch = res.width * 4;
    vc.aspect = st.displayMode.aspect;
    if (videoConfigure(0, &vc, NULL, 0) != 0) return 0;
    gcmSetFlipMode(GCM_FLIP_VSYNC);
    scr_w = res.width; scr_h = res.height;
    scr_aspect = st.displayMode.aspect == VIDEO_ASPECT_4_3 ? 4.f / 3.f : 16.f / 9.f;

    for (i = 0; i < 3; i++) {
        fb[i].ptr = (u32 *)rsxMemalign(64, scr_w * scr_h * 4);
        if (!fb[i].ptr) { if (i < 2) return 0; break; }   /* no third buffer: double buffering */
        memset(fb[i].ptr, 0, scr_w * scr_h * 4);
        rsxAddressToOffset(fb[i].ptr, &fb[i].off);
        gcmSetDisplayBuffer(i, fb[i].off, scr_w * 4, scr_w, scr_h);
        nbuf = i + 1;
    }
    {
        void *d = rsxMemalign(64, scr_w * scr_h * 4);
        if (!d) return 0;
        rsxAddressToOffset(d, &depth_off);
    }
    vbuf = (float *)rsxMemalign(128, 3 * QUADS * QUAD_FLOATS * sizeof(float));
    if (!vbuf) return 0;
    rsxAddressToOffset(vbuf, &vbuf_off);

    /* the two game frame buffers and the overlay share one mapped block */
    frames = (u8 *)memalign(1024 * 1024, 4 * 1024 * 1024 + 4 * 1024 * 1024);
    if (!frames) return 0;
    memset(frames, 0, 8 * 1024 * 1024);
    if (gcmMapMainMemory(frames, 8 * 1024 * 1024, &frames_off) != 0) return 0;
    ovl = (u32 *)(frames + 2 * FRAME_SIZE);
    ovl_off = frames_off + 2 * FRAME_SIZE;

    vp = (rsxVertexProgram *)quad_vpo;
    rsxVertexProgramGetUCode(vp, &vp_ucode, &sz);
    fp_blit = (rsxFragmentProgram *)blit_fpo;
    fp_fxaa = (rsxFragmentProgram *)fxaa_fpo;
    if (!(fp_blit_off = load_fp(fp_blit, &blit_unit, NULL))) return 0;
    if (!(fp_fxaa_off = load_fp(fp_fxaa, &fxaa_unit, NULL))) return 0;
    {   /* colour correction: same sampler as the plain ones. Soft-fail: the
           app still runs with today's picture if it cannot load. */
        u32 uc2;
        fp_color = (rsxFragmentProgram *)color_fpo;
        fp_colord = (rsxFragmentProgram *)colord_fpo;
        if ((fp_color_off = load_fp(fp_color, &color_unit, &color_buf)) &&
            (fp_colord_off = load_fp(fp_colord, &uc2, &colord_buf))) {
            void *uc;
            rsxFragmentProgramGetUCode(fp_color, &uc, &sz);
            color_ucode_size = sz;
            color_shadow = malloc(sz);
            if (color_shadow) memcpy(color_shadow, uc, sz);
            rsxFragmentProgramGetUCode(fp_colord, &uc, &sz);
            colord_ucode_size = sz;
            colord_shadow = malloc(sz);
            if (colord_shadow) memcpy(colord_shadow, uc, sz);
            color_ok = color_shadow && colord_shadow;
            if (!color_ok) logf_("video: colour program shadow failed, colour correction off\n");
            if (uc2 != color_unit) logf_("video: colour dithering program uses another texture unit (%u)\n", uc2);
        } else logf_("video: colour program failed to load, colour correction off\n");
    }
    {   /* dithering variants: same samplers as the plain ones */
        u32 ub, uf;
        fp_blitd = (rsxFragmentProgram *)blitd_fpo;
        fp_fxaad = (rsxFragmentProgram *)fxaad_fpo;
        if (!(fp_blitd_off = load_fp(fp_blitd, &ub, NULL)) || !(fp_fxaad_off = load_fp(fp_fxaad, &uf, NULL))) return 0;
        if (ub != blit_unit || uf != fxaa_unit) logf_("video: dithering programs use other texture units (%u %u)\n", ub, uf);
    }

    if (!fxaa_test()) {
        /* the half-precision FXAA drew wrong on this console: the float build */
        u32 u1, u2;
        fp_fxaa = (rsxFragmentProgram *)fxaaf_fpo; fp_fxaad = (rsxFragmentProgram *)fxaafd_fpo;
        if (!(fp_fxaa_off = load_fp(fp_fxaa, &u1, NULL)) || !(fp_fxaad_off = load_fp(fp_fxaad, &u2, NULL))) return 0;
        fxaa_unit = u1;
        if (!fxaa_test()) logf_("video: the float FXAA program fails its test too: FXAA (RSX) may look wrong\n");
    }

    gcmResetFlipStatus();
    pend = -1; disp = 0; cur = -1;
    logf_("video: %ux%u, resolution id %d, aspect %d, %d display buffers (%s)\n",
          scr_w, scr_h, (int)st.displayMode.resolution, (int)st.displayMode.aspect, nbuf,
          nbuf == 3 ? "triple buffering" : "double buffering: no memory for a third");
    return 1;
}

/* FXAA self-test: the FXAA program on a 64x64 picture with edges, drawn by the
 * RSX and compared with the same algorithm computed here. (The console drew
 * dark outlines with programs the PC model of the shader got right, so the
 * result on the real hardware decides; the log shows what differed.) */
#define FT 64
static float ft_tex(const u32 *t, int x, int y, int ch)
{
    if (x < 0) x = 0;
    if (x > FT - 1) x = FT - 1;
    if (y < 0) y = 0;
    if (y > FT - 1) y = FT - 1;
    return (float)((t[y * FT + x] >> (16 - 8 * ch)) & 255) * (1.f / 255.f);
}
static void ft_sample(const u32 *t, float u, float v, float *o)
{
    float x = u * FT - 0.5f, y = v * FT - 0.5f, fx, fy;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), ch;
    fx = x - x0; fy = y - y0;
    for (ch = 0; ch < 3; ch++)
        o[ch] = (ft_tex(t, x0, y0, ch) * (1 - fx) + ft_tex(t, x0 + 1, y0, ch) * fx) * (1 - fy) +
                (ft_tex(t, x0, y0 + 1, ch) * (1 - fx) + ft_tex(t, x0 + 1, y0 + 1, ch) * fx) * fy;
}
static void ft_ref(const u32 *t, int px, int py, float *out)
{
    float tx = 1.f / FT, u = (px + 0.5f) * tx, v = (py + 0.5f) * tx, h = 0.5f * tx, c[3], m[3];
    float lNW, lNE, lSW, lSE, lM, lMax, lMin, lMaxS, lMinM, dx, dy, r, d1x, d1y, d2x, d2y, mn, a[3], b[3], q[3];
    int k, edge;
    ft_sample(t, u - h, v - h, c); lNW = c[1];
    ft_sample(t, u + h, v - h, c); lNE = c[1];
    ft_sample(t, u - h, v + h, c); lSW = c[1];
    ft_sample(t, u + h, v + h, c); lSE = c[1];
    ft_sample(t, u, v, m);
    lM = m[1] + 1.f / 384.f;
    lMax = fmaxf(fmaxf(lNE, lSE), fmaxf(lNW, lSW)); lMin = fminf(fminf(lNE, lSE), fminf(lNW, lSW));
    lMaxS = fmaxf(lMax, lM); lMinM = fminf(lMin, lM);
    edge = lMaxS - lMinM >= fmaxf(0.05f, lMaxS * 0.125f);
    if (!edge) { for (k = 0; k < 3; k++) out[k] = m[k]; return; }
    dx = (lSW + lSE) - (lNW + lNE); dy = (lNW + lSW) - (lNE + lSE);
    r = 1.f / sqrtf(fmaxf(dx * dx + dy * dy, 1e-4f));
    d1x = dx * r; d1y = dy * r;
    ft_sample(t, u - d1x * 0.5f * tx, v - d1y * 0.5f * tx, a);
    ft_sample(t, u + d1x * 0.5f * tx, v + d1y * 0.5f * tx, c);
    for (k = 0; k < 3; k++) a[k] += c[k];
    mn = fmaxf(fminf(fabsf(d1x), fabsf(d1y)) * 8.f, 1.f / 64.f);
    d2x = fminf(fmaxf(d1x / mn, -2.f), 2.f); d2y = fminf(fmaxf(d1y / mn, -2.f), 2.f);
    ft_sample(t, u - d2x * 2.f * tx, v - d2y * 2.f * tx, b);
    ft_sample(t, u + d2x * 2.f * tx, v + d2y * 2.f * tx, q);
    for (k = 0; k < 3; k++) b[k] = (b[k] + q[k]) * 0.25f + a[k] * 0.25f;
    for (k = 0; k < 3; k++) out[k] = (b[1] < lMin || b[1] > lMax) ? a[k] * 0.5f : b[k];
}
static int fxaa_test(void)
{
    static u32 *src, *dst, src_off, dst_off, dep_off;
    volatile u32 *lab = (volatile u32 *)gcmGetLabelAddress(204);
    gcmSurface sf; int i, x, y, bad = 0, shown = 0, worst = 0, k;
    float scale[4] = { FT * 0.5f, FT * -0.5f, 0.5f, 0.0f }, offs[4] = { FT * 0.5f, FT * 0.5f, 0.5f, 0.0f };
    if (!src) {
        void *d;
        src = (u32 *)rsxMemalign(4096, FT * FT * 4); dst = (u32 *)rsxMemalign(4096, FT * FT * 4); d = rsxMemalign(4096, FT * FT * 4);
        if (!src || !dst || !d) return 1;
        rsxAddressToOffset(src, &src_off); rsxAddressToOffset(dst, &dst_off); rsxAddressToOffset(d, &dep_off);
    }
    for (y = 0; y < FT; y++)          /* a slanted edge, a thin line and a checker corner */
        for (x = 0; x < FT; x++)
            src[y * FT + x] = (x + 2 * y > 90) ? 0xffdcc8b4 : (x == 10 || (x > 40 && y < 12 && ((x ^ y) & 4))) ? 0xfff0f0f0 : 0xff3c4650;
    memset(dst, 0, FT * FT * 4);
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8; sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = dst_off; sf.colorPitch[0] = FT * 4;
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX; sf.depthOffset = dep_off; sf.depthPitch = FT * 4;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1; sf.width = FT; sf.height = FT;
    video_reset_state();
    rsxSetSurface(rsx_ctx, &sf);
    rsxSetViewport(rsx_ctx, 0, 0, FT, FT, 0.0f, 1.0f, scale, offs);
    rsxSetScissor(rsx_ctx, 0, 0, FT, FT);
    bind_texture(fxaa_unit, GCM_LOCATION_RSX, src_off, FT, FT, FT * 4, 1, 0);
    quad_n = 0;
    quad_clip(-1.f, 1.f, 1.f, -1.f, 0.f, 0.f, 1.f, 1.f, 1.f / FT, 1.f / FT, 1);
    quad_n = 0;
    *lab = 0;
    rsxSetWriteBackendLabel(rsx_ctx, 204, 0x5a5a5a5a);
    rsxFlushBuffer(rsx_ctx);
    for (i = 0; i < 20000 && *lab != 0x5a5a5a5a; i++) usleep(50);
    if (*lab != 0x5a5a5a5a) { logf_("video: FXAA self-test: the RSX did not finish\n"); return 1; }
    for (y = 0; y < FT; y++)
        for (x = 0; x < FT; x++) {
            float ref[3]; int d = 0;
            u32 got = dst[y * FT + x];
            ft_ref(src, x, y, ref);
            for (k = 0; k < 3; k++) {
                int g = (int)((got >> (16 - 8 * k)) & 255), w = (int)(ref[k] * 255.f + 0.5f), e = g > w ? g - w : w - g;
                if (e > d) d = e;
            }
            if (d > worst) worst = d;
            if (d > 12) {
                if (shown++ < 4) {
                    logf_("video: FXAA self-test: pixel %d,%d got %06x want %02x%02x%02x\n", x, y, (unsigned)(got & 0xffffff),
                          (int)(ref[0] * 255.f + 0.5f), (int)(ref[1] * 255.f + 0.5f), (int)(ref[2] * 255.f + 0.5f));
                }
                bad++;
            }
        }
    logf_("video: FXAA self-test (%s program): %d of %d pixels wrong, largest difference %d\n",
          fp_fxaa == (rsxFragmentProgram *)fxaa_fpo ? "half" : "float", bad, FT * FT, worst);
    return bad <= FT * FT / 100;
}

void video_shutdown(void)
{
    if (pend >= 0) { while (gcmGetFlipStatus() != 0) usleep(200); disp = pend; pend = -1; }
}

void *video_frame_buffer(size_t *pitch)
{
    *pitch = FRAME_PITCH;
    return frames + (shown ^ 1) * FRAME_SIZE;
}

/* the core only redraws the display rectangle: clear leftovers from another game */
void video_clear_frame_buffer(void) { memset(frames, 0, 2 * FRAME_SIZE); }

/* the queued flip has happened (waits for it): its buffer is on screen now.
   stats: counted as the frame's flip wait; rsx_wait: the RSX may only draw into the
   old buffer after the flip (double buffering) */
static void wait_flip(int rsx_wait)
{
    int n = 0;
    u64 w0 = sysGetSystemTime(), w;
    WHERE("waiting for flip");
    if (frame_label && (s32)(*frame_label - frame_seq) < 0) {   /* the RSX is behind */
        u64 g0 = sysGetSystemTime();
        int k = 0;
        vs.gpu_late++;
        while ((s32)(*frame_label - frame_seq) < 0 && ++k < 20000) usleep(50);
        vs.gpu_wait_us += sysGetSystemTime() - g0;
    }
    while (gcmGetFlipStatus() != 0) {
        usleep(200);
        if (++n == 5000) {      /* 1 s: a flip takes at most a frame or two */
            logf_("video: flip not done after 1 s (status %u)\n", (unsigned)gcmGetFlipStatus());
            break;
        }
    }
    gcmResetFlipStatus();
    disp = pend; pend = -1;
    /* the display pass may only start once the flip happened (the CPU just saw it
       happen, so this costs the RSX nothing) */
    if (rsx_wait) gcmSetWaitFlip(rsx_ctx);
    w = sysGetSystemTime() - w0;
    vs.wait_us += w;
    if (w > vs.wait_max) vs.wait_max = w;
}

/* Low latency: waits until the last queued frame is on screen, so the pad is
   read and the next frame emulated right after a refresh and shown at the next
   one. Without it the next frame is emulated while the last one still waits for
   its flip, and is shown a refresh later. Returns how long the RSX was still
   drawing that frame (microseconds). */
unsigned video_wait_shown(void)
{
    u64 g = vs.gpu_wait_us;
    if (pend >= 0) wait_flip(nbuf < 3);
    return (unsigned)(vs.gpu_wait_us - g);
}

/* the buffer to draw the next frame into: not on screen, not waiting for its flip */
static void pick_buffer(void)
{
    int i;
    for (i = 0; i < nbuf; i++)
        if (i != disp && i != pend) { cur = i; return; }
    cur = disp ^ 1;                       /* (two buffers, one pending: cannot happen after wait_flip) */
}

void video_begin(void)
{
    {
        u64 now = sysGetSystemTime();
        if (vs.last && now - vs.last > vs.gap_max) vs.gap_max = now - vs.last;
        vs.last = now;
        if (!vs.t0) vs.t0 = now;
        if (now - vs.t0 >= 10000000) {
            logf_("video: %.1f fps, flip wait avg %.2f ms max %.2f ms, longest frame %.1f ms; "
                  "RSX still drawing when the CPU was ready: %u of %u frames, waited %.2f ms per frame\n",
                  vs.frames * 1e6 / (now - vs.t0), vs.frames ? vs.wait_us / 1000.0 / vs.frames : 0.0,
                  vs.wait_max / 1000.0, vs.gap_max / 1000.0, vs.gpu_late, vs.frames,
                  vs.frames ? vs.gpu_wait_us / 1000.0 / vs.frames : 0.0);
            vs.frames = vs.gpu_late = 0; vs.t0 = now; vs.wait_us = vs.wait_max = vs.gap_max = vs.gpu_wait_us = 0;
        }
    }
    if (pend >= 0 && (nbuf < 3 || gcmGetFlipStatus() == 0))
        wait_flip(nbuf < 3);              /* double buffering: the hidden buffer only once it flipped */
    if (cur < 0) pick_buffer();
    /* the RSX has passed the previous flip, so it is done with everything
       queued before it: the frame buffer used two frames ago is free again.
       (quad_n restarts at the flip, in video_present: a frame begun twice must not
       overwrite vertices the RSX has not drawn yet) */
}

static void set_surface(void)
{
    gcmSurface sf; int i;
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = fb[cur].off; sf.colorPitch[0] = scr_w * 4;
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = depth_off; sf.depthPitch = scr_w * 4;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = scr_w; sf.height = scr_h;
    rsxSetSurface(rsx_ctx, &sf);
}

/* state every pass of ours relies on; the GPU renderer changes some of it */
void video_reset_state(void)
{
    float scale[4] = { scr_w * 0.5f, scr_h * -0.5f, 0.5f, 0.0f }, offs[4] = { scr_w * 0.5f, scr_h * 0.5f, 0.5f, 0.0f };
    set_surface();
    rsxSetViewport(rsx_ctx, 0, 0, scr_w, scr_h, 0.0f, 1.0f, scale, offs);
    rsxSetScissor(rsx_ctx, 0, 0, scr_w, scr_h);
    rsxSetColorMask(rsx_ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
    rsxSetColorMaskMrt(rsx_ctx, 0);
    rsxSetDepthTestEnable(rsx_ctx, GCM_FALSE);
    rsxSetDepthWriteEnable(rsx_ctx, GCM_FALSE);
    rsxSetCullFaceEnable(rsx_ctx, GCM_FALSE);
    rsxSetBlendEnable(rsx_ctx, GCM_FALSE);
    rsxSetUserClipPlaneControl(rsx_ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
                               GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
}

void video_clear(uint32_t argb)
{
    video_reset_state();
    rsxSetClearColor(rsx_ctx, argb);
    rsxClearSurface(rsx_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

/* swap_rb: the GPU renderer's target holds PS1 red in the blue channel */
static void bind_texture(u32 unit, u8 location, u32 offset, unsigned w, unsigned h, u32 pitch, int linear, int swap_rb)
{
    gcmTexture t;
    memset(&t, 0, sizeof t);
    t.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN | GCM_TEXTURE_FORMAT_NRM;
    t.mipmap = 1; t.dimension = GCM_TEXTURE_DIMS_2D; t.cubemap = GCM_FALSE;
    t.remap = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) |
               (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
               ((swap_rb ? GCM_TEXTURE_REMAP_COLOR_R : GCM_TEXTURE_REMAP_COLOR_B) << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
               ((swap_rb ? GCM_TEXTURE_REMAP_COLOR_B : GCM_TEXTURE_REMAP_COLOR_R) << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) |
               (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
    t.width = w; t.height = h; t.depth = 1; t.location = location;
    t.pitch = pitch; t.offset = offset;
    rsxInvalidateTextureCache(rsx_ctx, GCM_INVALIDATE_TEXTURE);
    rsxLoadTexture(rsx_ctx, unit, &t);
    rsxTextureControl(rsx_ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
    rsxTextureFilter(rsx_ctx, unit, 0, linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
                     linear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
    rsxTextureWrapMode(rsx_ctx, unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE,
                       0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void video_set_dither(int on, float native_w, float native_h) { dith_on = on; dith_nw = native_w; dith_nh = native_h; }
void video_redraw_quick(int on) { quick = on; }

/* clip-space rectangle <- texture rectangle (normalised). prog: bit 0 FXAA, bit 1
   PS1 dithering (the texture is the game picture: dith_nw/nh give PS1 pixels),
   bit 2 colour correction (final pass only: it replaces the Picture filter
   while any slider is non-zero; the overlay and the intermediate passes
   never set it) */
static void quad_clip(float X0, float Y0, float X1, float Y1, float u0, float v0, float u1, float v1,
                      float tx, float ty, int prog)
{
    int fxaa = prog & 1, dith = (prog & 2) != 0;
    float n0 = u0 * dith_nw, n1 = u1 * dith_nw, m0 = v0 * dith_nh, m1 = v1 * dith_nh;
    float *q;
    u32 off;
    int slot = cur * QUADS + quad_n;
    if (quad_n >= QUADS) return;
    quad_n++;
    q = vbuf + slot * QUAD_FLOATS;
    off = vbuf_off + slot * QUAD_FLOATS * sizeof(float);
    {
        float v[QUAD_FLOATS] = {
            X0, Y0, u0, v0, tx, ty, n0, m0,   X1, Y0, u1, v0, tx, ty, n1, m0,
            X0, Y1, u0, v1, tx, ty, n0, m1,   X1, Y1, u1, v1, tx, ty, n1, m1 };
        memcpy(q, v, sizeof v);
    }
    __asm__ volatile("sync" : : : "memory");
    rsxInvalidateVertexCache(rsx_ctx);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_POS,  0, off,     32, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX0, 0, off + 8, 32, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX1, 0, off + 16, 32, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxBindVertexArrayAttrib(rsx_ctx, GCM_VERTEX_ATTRIB_TEX2, 0, off + 24, 32, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
    rsxLoadVertexProgram(rsx_ctx, vp, vp_ucode);
    video_vp_epoch++;          /* the hardware renderer reloads its own program */
    if (prog & 4) rsxLoadFragmentProgramLocation(rsx_ctx, dith ? fp_colord : fp_color, dith ? fp_colord_off : fp_color_off, GCM_LOCATION_RSX);
    else if (fxaa) rsxLoadFragmentProgramLocation(rsx_ctx, dith ? fp_fxaad : fp_fxaa, dith ? fp_fxaad_off : fp_fxaa_off, GCM_LOCATION_RSX);
    else rsxLoadFragmentProgramLocation(rsx_ctx, dith ? fp_blitd : fp_blit, dith ? fp_blitd_off : fp_blit_off, GCM_LOCATION_RSX);
    rsxDrawVertexArray(rsx_ctx, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
}

/* screen rectangle (pixels) <- texture rectangle (normalised) */
static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 float tx, float ty, int fxaa)
{
    quad_clip(x0 / scr_w * 2.f - 1.f, 1.f - y0 / scr_h * 2.f, x1 / scr_w * 2.f - 1.f, 1.f - y1 / scr_h * 2.f,
              u0, v0, u1, v1, tx, ty, fxaa);
}

/* FXAA runs on the game picture at its own resolution (the upscaled target or the
   software frame) into this buffer, which is then scaled to the screen: several
   times fewer pixels than running it on the 1080p output */
#define FX_W 2560
#define FX_H 2048
static u32 fx_off, fx_dep_off; static int fx_ok;

static int fx_alloc(void)
{
    void *c, *d;
    if (fx_ok) return fx_ok > 0;
    c = rsxMemalign(4096, FX_W * 4 * FX_H);
    d = rsxMemalign(4096, FX_W * 4 * FX_H);
    if (!c || !d) { fx_ok = -1; logf_("video: no memory for the FXAA buffer, FXAA runs at screen resolution\n"); return 0; }
    rsxAddressToOffset(c, &fx_off);
    rsxAddressToOffset(d, &fx_dep_off);
    fx_ok = 1;
    return 1;
}

static void fx_surface(unsigned w, unsigned h)
{
    gcmSurface sf; int i;
    float scale[4] = { w * 0.5f, h * -0.5f, 0.5f, 0.0f }, offs[4] = { w * 0.5f, h * 0.5f, 0.5f, 0.0f };
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_RSX; sf.colorOffset[0] = fx_off; sf.colorPitch[0] = FX_W * 4;
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = fx_dep_off; sf.depthPitch = FX_W * 4;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = w; sf.height = h;
    rsxSetSurface(rsx_ctx, &sf);
    rsxSetViewport(rsx_ctx, 0, 0, w, h, 0.0f, 1.0f, scale, offs);
    rsxSetScissor(rsx_ctx, 0, 0, w, h);
}

/* Patch one const's 4 floats into a colour shadow ucode (rsxkit pattern).
   c->index is a byte offset into the .fpo pointing at the const's offset
   table { u32 num; u32 offset[]; } — every microcode offset where the
   const's value appears. Values are written with 16-bit halves swapped
   (RSX microcode endianness). Returns 1 if anything changed. */
static int color_patch_one(rsxFragmentProgram *fp, u32 *shadow, u32 size, const char *name, const float v[4])
{
    rsxProgramConst *c = rsxFragmentProgramGetConst(fp, name);
    const rsxConstOffsetTable *t;
    u32 w[4]; unsigned i; int changed = 0;
    if (!c || !shadow) return 0;
    t = (const rsxConstOffsetTable *)((const u8 *)fp + c->index);
    /* sanity: a const cannot appear in thousands of instructions; a wild
       table pointer (bad index) must not loop the whole ucode */
    if (t->num > 4096) return 0;
    for (i = 0; i < 4; i++) {
        u32 x;
        memcpy(&x, &v[i], 4);
        w[i] = (x << 16) | (x >> 16);
    }
    for (i = 0; i < t->num; i++) {
        u32 off = t->offset[i];
        /* off is ucode-relative; never write outside the shadow */
        if (off + 16 > size) return 0;
        if (memcmp((u8 *)shadow + off, w, 16)) {
            memcpy((u8 *)shadow + off, w, 16);
            changed = 1;
        }
    }
    return changed;
}

/* Colour correction is on when the programs loaded and any slider is
   non-zero; all zero is the picture as it is, drawn by today's programs. */
static int color_active(void)
{
    return color_ok && (cfg.color_brightness || cfg.color_contrast ||
                        cfg.color_saturation || cfg.color_temperature || cfg.color_gamma);
}

/* Push cfg's slider values into the colour programs, but only when a slider
   moved since the last push: the per-frame cost is a 4-int compare. Panel
   edits apply live, no restart. */
static void color_apply(void)
{
    static int logged;
    int v[5] = { cfg.color_brightness, cfg.color_contrast, cfg.color_saturation, cfg.color_temperature, cfg.color_gamma };
    float f[4], g[4];
    int changed = 0;
    if (!color_ok) return;
    if (cc_have_applied && v[0] == cc_applied[0] && v[1] == cc_applied[1] &&
        v[2] == cc_applied[2] && v[3] == cc_applied[3] && v[4] == cc_applied[4]) return;
    /* Gate the once-log on debug_log: logf_ drops lines when debug is off,
       so an ungated once-flag fires (and is lost) on the first colour frame
       of the app session — then no later game in the same launch ever logs. */
    if (!logged && cfg.debug_log) {
        rsxProgramConst *c1, *c2, *g1, *g2;
        logged = 1;
        logf_("video: color program num_const %u\n", rsxFragmentProgramGetNumConst(fp_color));
        c1 = rsxFragmentProgramGetConst(fp_color, "cc_params");
        c2 = rsxFragmentProgramGetConst(fp_colord, "cc_params");
        g1 = rsxFragmentProgramGetConst(fp_color, "cc_gamma");
        g2 = rsxFragmentProgramGetConst(fp_colord, "cc_gamma");
        logf_("video: color const cc_params: %s / %s, cc_gamma: %s / %s\n",
              c1 ? "found" : "NULL", c2 ? "found" : "NULL", g1 ? "found" : "NULL", g2 ? "found" : "NULL");
    }
    f[0] = (float)v[0]; f[1] = (float)v[1]; f[2] = (float)v[2]; f[3] = (float)v[3];
    g[0] = (float)v[4]; g[1] = 0.0f; g[2] = 0.0f; g[3] = 0.0f;
    changed |= color_patch_one(fp_color, color_shadow, color_ucode_size, "cc_params", f);
    changed |= color_patch_one(fp_colord, colord_shadow, colord_ucode_size, "cc_params", f);
    changed |= color_patch_one(fp_color, color_shadow, color_ucode_size, "cc_gamma", g);
    changed |= color_patch_one(fp_colord, colord_shadow, colord_ucode_size, "cc_gamma", g);
    if (changed) {
        /* The shadow changed: wait until the RSX finished the previous
           frame (CPU poll on the backend label, same pattern as wait_flip),
           then copy the patched ucode into the RSX buffers. The next
           rsxLoadFragmentProgramLocation in quad_clip re-reads it. */
        if (frame_label) {
            unsigned k = 0;
            while ((s32)(*frame_label - frame_seq) < 0 && ++k < 20000) usleep(50);
        }
        memcpy(color_buf, color_shadow, color_ucode_size);
        memcpy(colord_buf, colord_shadow, colord_ucode_size);
    }
    cc_applied[0] = v[0]; cc_applied[1] = v[1]; cc_applied[2] = v[2]; cc_applied[3] = v[3]; cc_applied[4] = v[4];
    cc_have_applied = 1;
}

/* ---- edge smoothing on the SPUs ----
 * Each frame the RSX scales the game picture to its screen size into a buffer in
 * main memory (IN) and writes a label; the SPUs (spu_mdec.c, ps3/spu) wait for the
 * label, smooth the edges into OUT, and the next frame shows OUT. One frame later
 * than the other filters, but the RSX only copies. OUT is drawn before the next
 * IN copy and its label are queued, so the SPUs never overwrite OUT while the
 * RSX may still read it; IN is only reused once the SPUs are done with it. */
#define AA_LABEL 203
#define AA_PITCH (1920 * 4)
static struct { u8 *mem; u32 off, size; int ok, pending, stall; unsigned w, h, seq; volatile u32 *label; } sa;

static int sa_alloc(void)
{
    u32 total;
    if (sa.ok) return sa.ok > 0;
    sa.ok = -1;
    sa.size = AA_PITCH * scr_h;
    total = (2 * sa.size + 0xfffff) & ~0xfffffu;          /* mapping granularity: 1 MiB */
    sa.mem = (u8 *)memalign(1024 * 1024, total);
    if (!sa.mem || gcmMapMainMemory(sa.mem, total, &sa.off) != 0) {
        logf_("video: no memory for SPU smoothing, using FXAA on the RSX\n");
        return 0;
    }
    sa.label = (volatile u32 *)gcmGetLabelAddress(AA_LABEL);
    *sa.label = 0;
    sa.ok = 1;
    logf_("video: SPU smoothing buffers %u KiB\n", (unsigned)(total / 1024));
    return 1;
}

/* label value whose bytes 0 and 2 are equal, so it lands in memory the same way
   whether or not this console swaps them (see hwr_rsx.c) */
static u32 sa_label_value(unsigned s) { u32 a = s & 0xff, b = (s >> 8) & 0xff; return b << 24 | a << 16 | b << 8 | a; }

static void sa_surface(unsigned w, unsigned h)
{
    gcmSurface sf; int i;
    float scale[4] = { w * 0.5f, h * -0.5f, 0.5f, 0.0f }, offs[4] = { w * 0.5f, h * 0.5f, 0.5f, 0.0f };
    memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    sf.colorLocation[0] = GCM_LOCATION_CELL; sf.colorOffset[0] = sa.off; sf.colorPitch[0] = AA_PITCH;
    for (i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
    /* no depth test: OUT stands in as the (unused) depth buffer */
    sf.depthFormat = GCM_SURFACE_ZETA_Z24S8; sf.depthLocation = GCM_LOCATION_CELL;
    sf.depthOffset = sa.off + sa.size; sf.depthPitch = AA_PITCH;
    sf.type = GCM_SURFACE_TYPE_LINEAR; sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = w; sf.height = h;
    rsxSetSurface(rsx_ctx, &sf);
    rsxSetViewport(rsx_ctx, 0, 0, w, h, 0.0f, 1.0f, scale, offs);
    rsxSetScissor(rsx_ctx, 0, 0, w, h);
}

static void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                 float tx, float ty, int fxaa);

/* 0: not possible this frame (the caller draws the picture another way) */
static int draw_spu_aa(u8 loc, u32 off, unsigned tw, unsigned th, u32 pitch,
                       float u0, float v0, float u1, float v1, int swap_rb, float x0, float y0, float x1, float y1, int d, int cc)
{
    unsigned W = ((unsigned)(x1 - x0 + 0.5f)) & ~3u, H = (unsigned)(y1 - y0 + 0.5f);
    int shown_out = 0;
    if (!spu_aa_available() || W < 16 || W > 1920 || H < 16 || H > scr_h || !sa_alloc()) return 0;
    if (sa.pending) {
        int r = spu_aa_wait(sa.stall ? 0 : 25000);
        if (r == 0) { sa.stall = 1; return 0; }          /* SPUs still busy: IN cannot be reused yet */
        sa.pending = 0; sa.stall = 0;
        if (r < 0) logf_("video: SPU smoothing missed the RSX's label\n");
        if (r > 0 && sa.w == W && sa.h == H) {
            bind_texture(cc ? color_unit : blit_unit, GCM_LOCATION_CELL, sa.off + sa.size, W, H, AA_PITCH, 0, swap_rb);
            quad(x0, y0, x0 + W, y0 + H, 0.f, 0.f, 1.f, 1.f, 1.f / W, 1.f / H, cc ? 4 : 0);
            shown_out = 1;
        }
    }
    if (!shown_out) {
        bind_texture(cc ? color_unit : blit_unit, loc, off, tw, th, pitch, 1, swap_rb);
        quad(x0, y0, x1, y1, u0, v0, u1, v1, 1.f / tw, 1.f / th, cc ? (4 | d) : d);
    }
    /* this frame, scaled to the screen, into IN for the SPUs (channels as they are) */
    sa_surface(W, H);
    bind_texture(blit_unit, loc, off, tw, th, pitch, 1, 0);
    quad_clip(-1.f, 1.f, 1.f, -1.f, u0, v0, u1, v1, 1.f / tw, 1.f / th, d);
    sa.seq = sa.seq % 65535 + 1;
    rsxSetWriteBackendLabel(rsx_ctx, AA_LABEL, sa_label_value(sa.seq));
    video_reset_state();
    if (spu_aa_start((uint64_t)(uintptr_t)sa.mem, (uint64_t)(uintptr_t)(sa.mem + sa.size),
                     (uint64_t)(uintptr_t)sa.label, sa_label_value(sa.seq), W, H, AA_PITCH)) {
        sa.pending = 1; sa.w = W; sa.h = H;
    }
    return 1;
}

/* the part of the TV signal everything is drawn in: the screen adjustment
   (Settings > Screen size) for TVs that cut off or don't fill the edges */
static void screen_area(float *ax, float *ay, float *aw, float *ah)
{
    *aw = scr_w * cfg.screen_w / 100.f; *ah = scr_h * cfg.screen_h / 100.f;
    *ax = (scr_w - *aw) * 0.5f; *ay = (scr_h - *ah) * 0.5f;
}

/* screen rectangle for the game picture and the fraction of its height shown
   (below 1 for the zoom modes: the top and bottom are cropped evenly). Can reach
   past the screen edges when the screen size is above 100%. */
static void game_rect(float *x0, float *y0, float *x1, float *y1, float *shown)
{
    float shape, w, h, ax, ay, aw, ah;   /* shape: width/height of the picture on the TV */
    screen_area(&ax, &ay, &aw, &ah);
    h = ah;
    *shown = 1.f;
    switch (cfg.aspect) {
    case ASPECT_14_9: shape = 14.f / 9.f; break;
    case ASPECT_ZOOM_14_9: shape = 14.f / 9.f; *shown = (4.f / 3.f) / shape; break;
    case ASPECT_ZOOM: shape = scr_aspect; *shown = (4.f / 3.f) / shape; break;
    case ASPECT_FULL: shape = scr_aspect; break;
    default: shape = 4.f / 3.f; break;
    }
    /* widescreen: the 3D was drawn for 16:9 (gte_widescreen), shown that wide
       whatever the aspect setting (on a 4:3 TV, as wide as it goes) */
    if (cfg.widescreen) { shape = 16.f / 9.f; *shown = 1.f; }
    if (shape > scr_aspect) { shape = scr_aspect; }
    if (*shown > 1.f) *shown = 1.f;
    w = aw * shape / scr_aspect;       /* works for non-square pixels (480p/576p) too */
    *x0 = ax + (aw - w) * 0.5f; *y0 = ay + (ah - h) * 0.5f;
    *x1 = *x0 + w; *y1 = *y0 + h;
}

/* cuts the rectangle (and its texture coordinates) to the screen */
static void clip_to_screen(float *x0, float *y0, float *x1, float *y1, float *u0, float *v0, float *u1, float *v1)
{
    float w = *x1 - *x0, h = *y1 - *y0, du = *u1 - *u0, dv = *v1 - *v0;
    if (w <= 0.f || h <= 0.f) return;
    if (*x0 < 0.f) { *u0 -= du * *x0 / w; *x0 = 0.f; }
    if (*x1 > scr_w) { *u1 -= du * (*x1 - scr_w) / w; *x1 = (float)scr_w; }
    if (*y0 < 0.f) { *v0 -= dv * *y0 / h; *y0 = 0.f; }
    if (*y1 > scr_h) { *v1 -= dv * (*y1 - scr_h) / h; *y1 = (float)scr_h; }
}

/* the borders around the game picture only need clearing for two frames (both
   display buffers) after something else was drawn there or the picture moved */
static int border_dirty = 3, drew_game, drew_other;   /* frames whose borders still need clearing: one per display buffer */
static float last_rect[4];

static void draw_game_texture(u8 loc, u32 off, unsigned tw, unsigned th, u32 pitch,
                              float u0, float v0, float u1, float v1, int swap_rb, int dither)
{
    float x0, y0, x1, y1;
    int fxaa = cfg.filter == FILTER_FXAA && !quick, linear = cfg.filter != FILTER_SHARP, d = dither ? 2 : 0;
    /* Colour correction replaces the Picture filter on the final pass only,
       while any slider is non-zero (same precedence the CRT filter had):
       the FXAA-into-buffer and SPU-IN passes stay plain, so the correction
       is never applied twice. It also applies on quick redraws, so the
       picture behind a menu matches the picture in the game. */
    int cc = color_active();
    color_apply();
    {
        float shown, c;
        game_rect(&x0, &y0, &x1, &y1, &shown);
        c = (v1 - v0) * (1.f - shown) * 0.5f;
        v0 += c; v1 -= c;
        clip_to_screen(&x0, &y0, &x1, &y1, &u0, &v0, &u1, &v1);
    }
    if (x0 != last_rect[0] || y0 != last_rect[1] || x1 != last_rect[2] || y1 != last_rect[3]) {
        last_rect[0] = x0; last_rect[1] = y0; last_rect[2] = x1; last_rect[3] = y1;
        border_dirty = nbuf;
    }
    drew_game = 1;
    if (border_dirty) video_clear(0xff000000);
    else video_reset_state();
    if (cfg.filter == FILTER_FXAA_SPU && !quick) {
        if (draw_spu_aa(loc, off, tw, th, pitch, u0, v0, u1, v1, swap_rb, x0, y0, x1, y1, d, cc)) return;
        fxaa = !spu_aa_available();    /* SPUs unavailable: FXAA on the RSX; busy: plain this frame */
    }
    if (fxaa) {
        /* source pixels covered by the picture */
        unsigned sw = (unsigned)((u1 - u0) * tw + 0.5f), sh = (unsigned)((v1 - v0) * th + 0.5f);
        if ((float)sw * sh > 0.6f * (x1 - x0) * (y1 - y0)) {
            /* the source has nearly as many pixels as the screen picture or more
               (3x/4x, 14:9 at 2x): FXAA straight onto the screen, on the bilinearly
               scaled picture, in one pass with offsets of one screen pixel. (FXAA at
               source resolution into a buffer costs that buffer's pixels plus a second
               full-screen pass: only worth it for a much smaller source.) */
            bind_texture(cc ? color_unit : fxaa_unit, loc, off, tw, th, pitch, 1, swap_rb);
            quad(x0, y0, x1, y1, u0, v0, u1, v1, (u1 - u0) / (x1 - x0), (v1 - v0) / (y1 - y0), cc ? (4 | d) : (1 | d));
            return;
        }
        if (sw && sh && sw <= FX_W && sh <= FX_H && fx_alloc()) {
            fx_surface(sw, sh);
            bind_texture(fxaa_unit, loc, off, tw, th, pitch, 1, swap_rb);
            quad_clip(-1.f, 1.f, 1.f, -1.f, u0, v0, u1, v1, 1.f / tw, 1.f / th, 1 | d);
            video_reset_state();
            bind_texture(cc ? color_unit : blit_unit, GCM_LOCATION_RSX, fx_off, FX_W, FX_H, FX_W * 4, 1, 0);
            quad(x0, y0, x1, y1, 0.f, 0.f, (float)sw / FX_W, (float)sh / FX_H, 1.f / FX_W, 1.f / FX_H, cc ? 4 : 0);
            return;
        }
    }
    bind_texture(cc ? color_unit : (fxaa ? fxaa_unit : blit_unit), loc, off, tw, th, pitch, linear, swap_rb);
    quad(x0, y0, x1, y1, u0, v0, u1, v1, 1.f / tw, 1.f / th, cc ? (4 | d) : (fxaa | d));
}

void video_draw_frame(const void *pix, unsigned w, unsigned h, size_t pitch)
{
    u32 off;
    if (!pix || !w || !h) return;
    if ((const u8 *)pix < frames || (const u8 *)pix >= frames + 2 * FRAME_SIZE) {
        /* the core used its own buffer: copy into the one the RSX is not reading */
        u8 *dst = frames + (shown ^ 1) * FRAME_SIZE;
        unsigned y;
        if (w > FRAME_W) w = FRAME_W;
        if (h > FRAME_H) h = FRAME_H;
        for (y = 0; y < h; y++) memcpy(dst + y * FRAME_PITCH, (const u8 *)pix + y * pitch, w * 4);
        pix = dst; pitch = FRAME_PITCH;
    }
    shown = (const u8 *)pix >= frames + FRAME_SIZE;
    video_flush_cpu_writes(pix, pitch * h);
    off = frames_off + (u32)((const u8 *)pix - frames);
    draw_game_texture(GCM_LOCATION_CELL, off, w, h, pitch, 0.f, 0.f, 1.f, 1.f, 0, 0);   /* the core dithers its own frames */
}


void video_draw_rsx_texture(uint32_t offset, unsigned tex_w, unsigned tex_h, size_t pitch,
                            float u0, float v0, float u1, float v1)
{
    draw_game_texture(GCM_LOCATION_RSX, offset, tex_w, tex_h, pitch, u0, v0, u1, v1, 1, dith_on);
}

unsigned video_vp_epoch;
/* The overlay (menus, FPS, notices) is one picture in main memory that the RSX
   reads for each frame: before the CPU changes it, the RSX must have finished
   drawing the frames queued with the old picture (with three buffers nothing else
   waits for that: the menu blinked, frames caught it half redrawn). Only when it
   is about to change, so frames that leave it alone never wait; waits for the
   RSX's drawing, not the refresh. */
static int ovl_synced;
static unsigned ovl_gen;                  /* counts clears: drawers that keep text there redraw after one */
unsigned video_overlay_generation(void) { return ovl_gen; }
static void overlay_sync(void)
{
    if (ovl_synced) return;
    ovl_synced = 1;
    if (frame_label && (s32)(*frame_label - frame_seq) < 0) {
        u64 g0 = sysGetSystemTime();
        int k = 0;
        WHERE("waiting for the RSX to finish the previous frame (overlay)");
        vs.gpu_late++;
        while ((s32)(*frame_label - frame_seq) < 0 && ++k < 20000) usleep(50);
        vs.gpu_wait_us += sysGetSystemTime() - g0;
    }
}
uint32_t *video_overlay(void) { overlay_sync(); return ovl; }
void video_overlay_clear(void) { overlay_sync(); ovl_gen++; memset(ovl, 0, OVL_W * OVL_H * 4); ovl_full_flush = 1; }
void video_overlay_clear_rows(int y0, int y1) { overlay_sync(); ovl_gen++; memset(ovl + y0 * OVL_W, 0, (y1 - y0) * OVL_W * 4); }

static void draw_overlay(int y0, int y1)
{
    video_reset_state();
    if (ovl_full_flush) { y0 = 0; y1 = OVL_H; ovl_full_flush = 0; }
    if (y0 == 0 && y1 == OVL_H) drew_other = 1;     /* a menu over the borders */
    video_flush_cpu_writes(ovl + y0 * OVL_W, (y1 - y0) * OVL_W * 4);
    rsxSetBlendFunc(rsx_ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ZERO, GCM_ONE);
    rsxSetBlendEquation(rsx_ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
    rsxSetBlendEnable(rsx_ctx, GCM_TRUE);
    bind_texture(blit_unit, GCM_LOCATION_CELL, ovl_off, OVL_W, OVL_H, OVL_W * 4, 1, 0);
    /* only the rows that changed (the FPS counter): blending the whole 1080p screen
       every frame costs the RSX several milliseconds */
    {
        float ax, ay, aw, ah;
        screen_area(&ax, &ay, &aw, &ah);
        quad(ax, ay + (float)y0 * ah / OVL_H, ax + aw, ay + (float)y1 * ah / OVL_H,
             0, (float)y0 / OVL_H, 1, (float)y1 / OVL_H, 1.f / OVL_W, 1.f / OVL_H, 0);
    }
    rsxSetBlendEnable(rsx_ctx, GCM_FALSE);
}

/* called after this frame's picture was drawn (a notice or menu over the borders
   went away): the borders of every display buffer need clearing from the next frame
   on: nbuf + 1, as the present right after counts one. (One too few left the old
   text in a buffer, and it blinked every time that buffer was shown.) */
void video_borders_dirty(void) { border_dirty = nbuf + 1; }

/* width of the black border left of the game picture, in overlay pixels */
int video_overlay_border(void)
{
    float x0, y0, x1, y1, shown, ax, ay, aw, ah;
    game_rect(&x0, &y0, &x1, &y1, &shown);
    screen_area(&ax, &ay, &aw, &ah);
    return x0 > ax ? (int)((x0 - ax) * OVL_W / aw) : 0;
}

void video_draw_overlay(void) { ovl_full_flush = 1; draw_overlay(0, OVL_H); }
void video_draw_overlay_rows(int y0, int y1) { draw_overlay(y0, y1); }

/* Build the screenshot path: APP_DIR/screenshots/<name>_<YYYYMMDD-HHMMSS>_pgxp<M>_cull<C>.png,
   with a _2/_3/... suffix when two captures land in the same second (the
   one-shot flag makes that possible: request, capture, menu, request again).
   Mirrors hooks_dump_vram's first-free-number naming. Returns 0 or -1. */
static int screenshot_make_path(char *out, size_t n, const char *name, int pgxp, int cull)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    int k;
    struct stat st;
    if (!out || n < 64 || !tm) return -1;
    if (!strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", tm)) return -1;
    mkdir(APP_DIR "/screenshots", 0777);
    for (k = 0; k < 10000; k++) {
        if (k == 0)
            snprintf(out, n, APP_DIR "/screenshots/%s_%s_pgxp%d_cull%d.png", name, ts, pgxp, cull);
        else
            snprintf(out, n, APP_DIR "/screenshots/%s_%s_pgxp%d_cull%d_%d.png", name, ts, pgxp, cull, k + 1);
        if (stat(out, &st) != 0) return 0;
    }
    return -1;
}

/* Write the armed frame as a PNG and log it, on every path (success or
   failure) clearing the request so it never fires twice or on another
   game. The armed buffer is on screen now (disp), fully drawn: the flip
   for it completed in this present's wait (or in the video_begin just
   before it). The CPU reads it directly, as the FXAA self-test reads its
   RSX-drawn test buffer (no cache op: the CPU never writes the display
   buffers after the init memset, and the flip is the RSX's completion
   signal for the frame). */
static void screenshot_capture(void)
{
    char path[512];
    int ok = 0;
    path[0] = 0;
    if (screenshot_make_path(path, sizeof path, shot_name, cfg.pgxp, cfg.pgxp_culling) == 0)
        ok = screenshot_write_png(path, fb[disp].ptr, scr_w, scr_h) == 0;
    if (ok) {
        /* the counters are the running 10 s window (partial): they reset
           only on the periodic log print, so label them as such */
        extern unsigned int hwr_pgxp_polys, hwr_pgxp_misses, hwr_pgxp_alone, hwr_pgxp_amb;
        extern unsigned int pgxp_lite_addr_hits, pgxp_lite_hits, pgxp_lite_welds;
        extern unsigned int pgxp_nclip_total, pgxp_nclip_flip, pgxp_nclip_restore, pgxp_nclip_nogte;
        extern unsigned int pgxp_nclip_guess, pgxp_nclip_dzero;
        logf_("screenshot: saved %s (%ux%u, pgxp %d cull %d cpu %s; running window, partial — PGXP %u sub-pixel, %u whole-pixel, corners addr %u value %u, amb %u alone %u welds %u, PGXP culling correction %u recomputed %u flipped %u restored %u nogte %u guessed %u dzero)\n",
              path, scr_w, scr_h, cfg.pgxp, cfg.pgxp_culling, cfg.dynarec ? "dynarec" : "interp",
              hwr_pgxp_polys, hwr_pgxp_misses, pgxp_lite_addr_hits, pgxp_lite_hits, hwr_pgxp_amb, hwr_pgxp_alone, pgxp_lite_welds,
              pgxp_nclip_total, pgxp_nclip_flip, pgxp_nclip_restore, pgxp_nclip_nogte, pgxp_nclip_guess, pgxp_nclip_dzero);
    } else {
        logf_("screenshot: failed to write %s\n", path[0] ? path : shot_name);
    }
    shot_armed = 0;
    shot_req = 0;
}

void video_present(void)
{
    int dg, doth;   /* this frame's picture, snapshotted before the reset below */
    int was_armed = shot_armed;   /* an arm from a previous present: this one captures it */
    WHERE("present");
    watchdog_frame();
    dg = drew_game; doth = drew_other;
    /* a frame without the game picture (menus): the borders need clearing again */
    if (!drew_game || drew_other) border_dirty = nbuf;
    else if (border_dirty) border_dirty--;
    drew_game = drew_other = 0;
    if (!frame_label) { frame_label = (volatile u32 *)gcmGetLabelAddress(FRAME_LABEL); *frame_label = 0; frame_seq = 0; }
    if (cur < 0) pick_buffer();          /* (presented without video_begin) */
    /* lv2 tracks one pending flip: the previous one must have happened first. With
       three buffers this wait overlaps the drawing above, instead of preceding it.
       (Before this frame's label: the wait's check is whether the RSX finished the
       previous frame.) */
    if (pend >= 0) wait_flip(0);
    rsxSetWriteBackendLabel(rsx_ctx, FRAME_LABEL, ++frame_seq);
    ovl_synced = 0;                       /* this frame reads the overlay as it is now */
    if (gcmSetFlip(rsx_ctx, cur) == 0) {
        rsxFlushBuffer(rsx_ctx);
        pend = cur;
        cur = -1;
        quad_n = 0;
        vs.frames++;
        /* a screenshot request arms on the first clean game frame queued
           for flip (game picture, no overlay). The frame just flipped in
           is NOT it: the first frame after the menu closes is a full
           overlay draw (drew_other = 1), so arming skips it by itself. */
        if (shot_req && dg && !doth) shot_armed = 1;
    } else {
        /* not queued: keep drawing into the same (hidden) buffer next time */
        rsxFlushBuffer(rsx_ctx);
        logf_("video: flip failed\n");
    }
    /* the buffer armed on an earlier present went on screen in this
       present's wait (or in the video_begin just before it): read it now,
       after the new flip is queued, so the encode never delays the display */
    if (was_armed) screenshot_capture();
}
