/* DualShock 3 / Sixaxis input. */
#include <string.h>
#include <io/pad.h>
#include <stdio.h>
#include "common.h"

#define PORTS 2
static pad_state_t pads[PORTS];
static uint32_t prev[PORTS], pressed;
/* vibration the game asks for (large motor 0-255, small motor on/off) and what the pad has */
static uint8_t rum_want[PORTS][2], rum_sent[PORTS][2];
static unsigned rum_age[PORTS];

void input_rumble(int port, int large, unsigned strength)
{
    if (port < 0 || port >= PORTS) return;
    if (large) rum_want[port][1] = strength >> 8;
    else rum_want[port][0] = strength ? 1 : 0;
}

void input_rumble_stop(void)
{
    memset(rum_want, 0, sizeof rum_want);
}

/* sent only when it changed, and a change of speed at most every 3rd poll (each one
   goes over Bluetooth); off at once */
static void rumble_send(int p)
{
    padActParam a;
    uint8_t s = cfg.vibration ? rum_want[p][0] : 0, l = cfg.vibration ? rum_want[p][1] : 0;
    rum_age[p]++;
    if (s == rum_sent[p][0] && l == rum_sent[p][1]) return;
    if ((s || l) && rum_age[p] < 3 && (rum_sent[p][0] || rum_sent[p][1])) return;
    memset(&a, 0, sizeof a);
    a.small_motor = s; a.large_motor = l;
    if (ioPadSetActDirect(p, &a) == 0) { rum_sent[p][0] = s; rum_sent[p][1] = l; rum_age[p] = 0; }
}

void input_init(void)
{
    ioPadInit(7);
}

void input_poll(void)
{
    padInfo info;
    int p;
    pressed = 0;
    if (ioPadGetInfo(&info) != 0) return;
    for (p = 0; p < PORTS; p++) {
        padData d;
        pad_state_t *s = &pads[p];
        if (!info.status[p]) {
            if (s->connected) logf_("input: pad %d disconnected\n", p);
            memset(s, 0, sizeof *s); s->lx = s->ly = s->rx = s->ry = 128; prev[p] = 0;
            rum_sent[p][0] = rum_sent[p][1] = 0; continue;
        }
        if (!s->connected) logf_("input: pad %d connected\n", p);
        s->connected = 1;
        rumble_send(p);
        if (ioPadGetData(p, &d) != 0 || d.len == 0) continue;   /* no change since last read */
        s->buttons =
            (d.BTN_UP ? PAD_UP : 0) | (d.BTN_DOWN ? PAD_DOWN : 0) | (d.BTN_LEFT ? PAD_LEFT : 0) |
            (d.BTN_RIGHT ? PAD_RIGHT : 0) | (d.BTN_CROSS ? PAD_CROSS : 0) | (d.BTN_CIRCLE ? PAD_CIRCLE : 0) |
            (d.BTN_SQUARE ? PAD_SQUARE : 0) | (d.BTN_TRIANGLE ? PAD_TRIANGLE : 0) | (d.BTN_L1 ? PAD_L1 : 0) |
            (d.BTN_R1 ? PAD_R1 : 0) | (d.BTN_L2 ? PAD_L2 : 0) | (d.BTN_R2 ? PAD_R2 : 0) |
            (d.BTN_L3 ? PAD_L3 : 0) | (d.BTN_R3 ? PAD_R3 : 0) | (d.BTN_START ? PAD_START : 0) |
            (d.BTN_SELECT ? PAD_SELECT : 0);
        s->lx = d.ANA_L_H; s->ly = d.ANA_L_V; s->rx = d.ANA_R_H; s->ry = d.ANA_R_V;
        /* optional: sticks press buttons too (for games made for the digital pad).
           Past ~40% deflection; the real buttons keep working */
        /* Bypassed when analog is on: in analog mode the stick and the D-pad /
           buttons are independent inputs (some games use both, differently);
           in digital mode the mappings are useful, since digital games
           ignore the stick values entirely. */
        if (cfg.stick_dpad && !cfg.analog)
            s->buttons |= (s->ly < 128 - 52 ? PAD_UP : 0) | (s->ly > 128 + 52 ? PAD_DOWN : 0) |
                          (s->lx < 128 - 52 ? PAD_LEFT : 0) | (s->lx > 128 + 52 ? PAD_RIGHT : 0);
        if (cfg.stick_buttons && !cfg.analog)
            s->buttons |= (s->ry < 128 - 52 ? PAD_TRIANGLE : 0) | (s->ry > 128 + 52 ? PAD_CROSS : 0) |
                          (s->rx < 128 - 52 ? PAD_SQUARE : 0) | (s->rx > 128 + 52 ? PAD_CIRCLE : 0);
    }
    for (p = 0; p < PORTS; p++) {
        pressed |= pads[p].buttons & ~prev[p];
        prev[p] = pads[p].buttons;
    }
}

const pad_state_t *input_pad(int port) { return &pads[port]; }

const char *input_names(uint32_t b, char *out, size_t n)
{
    static const char *nm[16] = { "Up", "Down", "Left", "Right", "X", "O", "Square", "Triangle",
                                  "L1", "R1", "L2", "R2", "L3", "R3", "Start", "Select" };
    size_t o = 0; int i;
    out[0] = 0;
    for (i = 0; i < 16; i++)
        if (b & (1u << i)) o += snprintf(out + o, o < n ? n - o : 0, "%s%s", o ? "+" : "", nm[i]);
    return out;
}
uint32_t input_pressed(void) { return pressed; }
