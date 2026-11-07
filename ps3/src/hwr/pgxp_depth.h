/* PGXP textures: the corner depths of one polygon, made to fit one surface.
 *
 * PGXP-lite finds a corner's depth by the screen pixel it lands on, so a corner
 * can come with another vertex's depth (a fighter's hand in front of a wall) or
 * with none (a corner the game made with the CPU). Flattening such a polygon for
 * that frame made its texture jump between perspective and affine, which on a big
 * wall is plain to see. Instead the odd corner is repaired:
 *  - a quad is flat in 3D, so 1/depth is linear across it on screen: the other
 *    three corners give the fourth exactly, every frame the same;
 *  - a triangle's odd corner takes the mean of the other two.
 * A corner is odd when it has no depth, or, when the depths do not fit one
 * surface, the one furthest (in ratio) from the others' middle.
 *
 * pgxp_depths(x, y, z, n): x, y the corners on screen, z their depths (0 none);
 * returns 1 with z completed and fitting, 0 when the polygon should stay flat. */
#ifndef PGXP_DEPTH_H
#define PGXP_DEPTH_H

#include <math.h>

/* 1/depth at corner k of a quad from the other three (0 when they are in a line) */
static float pgxp_plane_q(const float *x, const float *y, const float *z, int k)
{
    int a = (k + 1) & 3, b = (k + 2) & 3, c = (k + 3) & 3;
    float x1 = x[b] - x[a], y1 = y[b] - y[a], q1 = 1.f / z[b] - 1.f / z[a];
    float x2 = x[c] - x[a], y2 = y[c] - y[a], q2 = 1.f / z[c] - 1.f / z[a];
    float det = x1 * y2 - x2 * y1, qa, qb;
    if (det < 4.f && det > -4.f) return 0.f;
    qa = (q1 * y2 - q2 * y1) / det;
    qb = (x1 * q2 - x2 * q1) / det;
    return 1.f / z[a] + qa * (x[k] - x[a]) + qb * (y[k] - y[a]);
}

/* (divisions are slow on the PS3's PPU, tens of cycles each, and this runs for
   every textured polygon: one reciprocal per corner used, one for the plane) */
static inline int pgxp_depths_fit(const float *x, const float *y, const float *z, int n)
{
    float zmin = z[0], zmax = z[0];
    int i;
    for (i = 1; i < n; i++) { if (z[i] < zmin) zmin = z[i]; if (z[i] > zmax) zmax = z[i]; }
    if (zmin <= 0.f || zmax > 64.f * zmin) return 0;
    if (n == 4) {   /* the fourth corner against the plane through the other three (pgxp_plane_q) */
        float x1 = x[1] - x[0], y1 = y[1] - y[0], x2 = x[2] - x[0], y2 = y[2] - y[0];
        float det = x1 * y2 - x2 * y1;
        if (det >= 4.f || det <= -4.f) {
            float ia = 1.f / z[0], q1 = 1.f / z[1] - ia, q2 = 1.f / z[2] - ia, id = 1.f / det;
            float qa = (q1 * y2 - q2 * y1) * id, qb = (x1 * q2 - x2 * q1) * id;
            float q = ia + qa * (x[3] - x[0]) + qb * (y[3] - y[0]);
            if (q != 0.f) { float r = q * z[3]; return r > 0.85f && r < 1.15f; }
        }
    }
    return zmax <= 2.f * zmin;   /* a triangle (or a quad with three corners in a line) */
}

static int pgxp_depths(const float *x, const float *y, float *z, int n)
{
    int i, j, odd = -1, known = 0;
    float zk[4], lo, hi;
    for (i = 0; i < n; i++) if (z[i] > 0.f) known++;
    if (known == n) {
        float l[4], best = 0.f;
        if (pgxp_depths_fit(x, y, z, n)) return 1;
        /* the corner furthest from the middle of the others is the odd one */
        for (i = 0; i < n; i++) l[i] = logf(z[i]);
        for (i = 0; i < n; i++) {
            float o[3], mid, d;
            int m = 0;
            for (j = 0; j < n; j++) if (j != i) o[m++] = l[j];
            if (m == 3) {   /* middle of three */
                float t;
                if (o[0] > o[1]) { t = o[0]; o[0] = o[1]; o[1] = t; }
                if (o[1] > o[2]) { t = o[1]; o[1] = o[2]; o[2] = t; }
                if (o[0] > o[1]) { t = o[0]; o[0] = o[1]; o[1] = t; }
                mid = o[1];
            } else mid = 0.5f * (o[0] + o[1]);
            d = fabsf(l[i] - mid);
            if (d > best) { best = d; odd = i; }
        }
        /* a triangle's corner only somewhat off may be its own (a steep near
           triangle): only one at least 2.5 times off is taken as borrowed */
        if (n == 3 && best < 0.9163f) return 0;   /* log(2.5) */
    } else if (known == n - 1) {
        for (i = 0; i < n; i++) if (z[i] <= 0.f) odd = i;
    } else return 0;
    if (odd < 0) return 0;
    /* the others must agree among themselves */
    lo = hi = 0.f;
    for (i = 0; i < n; i++) {
        zk[i] = z[i];
        if (i == odd) continue;
        if (lo == 0.f || z[i] < lo) lo = z[i];
        if (z[i] > hi) hi = z[i];
    }
    if (hi > 2.f * lo) return 0;
    if (n == 4) {
        float q = pgxp_plane_q(x, y, zk, odd);
        if (!(q > 0.f)) return 0;
        zk[odd] = 1.f / q;
    } else {
        float s = 0.f;
        for (i = 0; i < n; i++) if (i != odd) s += z[i];
        zk[odd] = s / (float)(n - 1);
    }
    if (!pgxp_depths_fit(x, y, zk, n)) return 0;
    for (i = 0; i < n; i++) z[i] = zk[i];
    return 1;
}

/* mode2w: the depth a mode-2 polygon uses for one corner. Lite's depth is
 * keyed by the screen word, so it can be another vertex's depth; the RAM
 * shadow's is keyed by the packet's own RAM address and comes from the
 * same GTE op as the corner's position, so when the shadow has a
 * validated entry its depth wins. The position is untouched (only the
 * depth is picked here); a shadow miss keeps lite's depth. */
static inline float pgxp_pick_z(float lite_z, float shadow_z, int has_shadow)
{
    return has_shadow ? shadow_z : lite_z;
}

#endif
