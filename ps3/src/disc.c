/* Discfix38: physical PS1 CDs, including guarded temporary storage patches.
 * The disc in the PS3's own drive, read through lv2's storage calls (custom
 * firmware; the app's SELF has the needed flag from make_self_npdrm).
 *
 * The drive takes ATAPI packet commands (sys_storage_execute_device_command,
 * command 1, as the Linux ps3rom driver sends them through lv1): READ TOC for
 * the tracks and READ CD for whole 2352-byte sectors, the way the PS1 reads
 * them (data with headers, and CD audio).
 *
 * The core calls the rcdrom_* functions (libpcsxcore/cdrom-async.c, built
 * with HAVE_CDROM for the PS3) for a game path starting with "cdrom:"; its
 * read-ahead thread keeps a few dozen sectors ahead of the game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <malloc.h>
#include <ppu-lv2.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <time.h>
#include "common.h"
#include "disc_access.h"

#define BDVD_DEVICE  0x0101000000000006ULL
#define SECTOR_RAW   2352

/* lv2's ATAPI command block (lv1_atapi_cmnd_block without the buffer fields) */
struct atapi_cmd {
    uint8_t  pkt[32];
    uint32_t pktlen;
    uint32_t blocks;
    uint32_t block_size;
    uint32_t proto;            /* 0 no data, 1 PIO data in, 3 DMA */
    uint32_t in_out;           /* 1 = read */
    uint32_t unknown;
} __attribute__((packed));

enum { PROTO_NONE = 0, PROTO_PIO_IN = 1, PROTO_DMA = 3 };

static s32 st_open(uint64_t dev, uint32_t *fd)
{
    lv2syscall4(600, dev, 0, (uint64_t)(uintptr_t)fd, 0);
    return_to_user_prog(s32);
}

static s32 st_close(uint32_t fd)
{
    lv2syscall1(601, fd);
    return_to_user_prog(s32);
}

static s32 st_atapi(uint32_t fd, struct atapi_cmd *c, void *buf, uint32_t len, uint32_t *status)
{
    lv2syscall7(616, fd, 1, (uint64_t)(uintptr_t)c, sizeof *c, (uint64_t)(uintptr_t)buf, len,
                (uint64_t)(uintptr_t)status);
    return_to_user_prog(s32);
}

/* Probe the synchronous syscall used by IRISMAN before the legacy 616 path.
 * READ CD + Q returns 2352 main bytes followed by 16 Q bytes (2368 total).
 * Some drive/firmware combinations accept that form but reject main-only. */
enum { READ_604_RAW, READ_604_Q, READ_616_RAW, READ_616_Q,
       READ_604_PIO, READ_616_PIO, READ_N };
static const char *const read_name[READ_N] = {
    "604 DMA raw", "604 DMA raw+Q", "616 DMA raw", "616 DMA raw+Q",
    "604 PIO raw", "616 PIO raw"
};

static s32 st_packet(uint32_t fd, struct atapi_cmd *c, void *buf, uint32_t len)
{
    lv2syscall6(604, fd, 1, (uint64_t)(uintptr_t)c, sizeof *c,
                (uint64_t)(uintptr_t)buf, len);
    return_to_user_prog(s32);
}

int disc_needs_cfw;   /* set when raw reads are refused and LV1 is out of reach (HEN) */

static uint64_t access_peek(int level, uint64_t address)
{
    if (level == 1) {
        lv2syscall1(8, address);
        return_to_user_prog(uint64_t);
    } else {
        lv2syscall1(6, address);
        return_to_user_prog(uint64_t);
    }
}

static void access_poke(int level, uint64_t address, uint64_t value)
{
    if (level == 1) { lv2syscall2(9, address, value); }
    else { lv2syscall2(7, address, value); }
}

/* the drive's mode for the disc in it (VSH sets 1 for a PS1 disc, 0x29 otherwise) */
static s32 st_disc_auth(uint64_t func, uint64_t param)
{
    lv2syscall2(864, func, param);
    return_to_user_prog(s32);
}

typedef struct {
    uint32_t fd;
    int big;                   /* READ_*: selected syscall/packet layout */
    disc_access access;
    int mode_changed;
    int opened;
    int ntracks;               /* tracks 1..ntracks */
    uint32_t start[100];       /* start LBA per track (index 1..) */
    int audio[100];
    uint32_t leadout;
    uint8_t *buf;              /* aligned bounce buffer, one raw sector */
    sys_mutex_t lock;          /* the core's read-ahead thread and the main thread both read */
    unsigned reads, errors;
    uint32_t last_status;      /* the drive's status for the last command */
    /* subchannel Q (LibCrypt): read with each sector once sub_mode is set */
    int sub_mode;              /* SUB_*: how the drive gives it */
    uint32_t q_lba;            /* the sector q[] belongs to (~0: none) */
    uint8_t q[12];             /* control/ADR .. CRC, as on the disc */
    unsigned q_bad, q_off;     /* Q with a bad CRC, Q of another sector */
} disc_t;
enum { SUB_NONE, SUB_RAW, SUB_Q };   /* READ CD sub-channel 001b (raw P-W, 96 bytes), 010b (formatted Q, 16 bytes) */

static int cmd_in(disc_t *d, const uint8_t *pkt, int pktlen, void *out, uint32_t len)
{
    struct atapi_cmd c;
    uint32_t status[2] = { 0, 0 };
    s32 r;
    memset(&c, 0, sizeof c);
    memcpy(c.pkt, pkt, pktlen);
    c.pktlen = 12;
    c.blocks = 1;
    c.block_size = len;
    c.proto = len == 0 ? PROTO_NONE : len < 2048 ? PROTO_PIO_IN :
              d->big >= READ_604_PIO ? PROTO_PIO_IN : PROTO_DMA;
    c.in_out = 1;
    if (d->lock) sysMutexLock(d->lock, 0);
    /* Do not expose stale bounce-buffer bytes when a command fails. */
    if (len > 4096) { if (d->lock) sysMutexUnlock(d->lock); return -1; }
    memset(d->buf, 0, 4096);
    if (d->big == READ_604_RAW || d->big == READ_604_Q || d->big == READ_604_PIO) {
        int tries = 0;
        do {
            r = st_packet(d->fd, &c, len ? d->buf : NULL, len);
            if ((uint32_t)r == 0x8001000a) sysUsleep(20000);
        } while ((uint32_t)r == 0x8001000a && ++tries < 10);
    } else {
        r = st_atapi(d->fd, &c, len ? d->buf : NULL, len, status);
        if (!r && status[0]) r = -1;
    }
    if (r == 0 && len && out) memcpy(out, d->buf, len);
    d->last_status = status[0];
    if (d->lock) sysMutexUnlock(d->lock);
    return r;
}

static int read_toc(disc_t *d)
{
    uint8_t pkt[12] = { 0x43, 0, 0, 0, 0, 0, 0, 0x03, 0x24, 0, 0, 0 };   /* READ TOC, LBA, format 0, 804 bytes */
    uint8_t t[804];
    int i, n, len, r;
    memset(t, 0, sizeof t);
    if ((r = cmd_in(d, pkt, 10, t, sizeof t)) != 0) {
        logf_("disc: READ TOC failed (%#x, drive status %#x)\n", (unsigned)r, (unsigned)d->last_status);
        return -1;
    }
    len = (t[0] << 8 | t[1]) + 2;
    n = (len - 4) / 8;
    d->ntracks = 0; d->leadout = 0;
    if (len < 12 || len > (int)sizeof t || (len - 4) % 8) {
        logf_("disc: invalid TOC length %d\n", len); return -1;
    }
    for (i = 0; i < n && i < 100; i++) {
        const uint8_t *e = t + 4 + i * 8;
        uint32_t lba = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
        int tn = e[2];
        if (tn == 0xaa) d->leadout = lba;
        else if (tn >= 1 && tn < 100) {
            d->start[tn] = lba; d->audio[tn] = !(e[1] & 0x04);
            if (tn > d->ntracks) d->ntracks = tn;
        }
    }
    logf_("disc: %d track%s, %u sectors, first track %s\n", d->ntracks, d->ntracks == 1 ? "" : "s",
          (unsigned)d->leadout, d->ntracks && d->audio[1] ? "audio" : "data");
    return d->ntracks > 0 && d->leadout ? 0 : -1;
}

/* the Q channel (bit 6 of each byte) of raw P-W subchannel data */
static void q_from_raw(const uint8_t *pw, uint8_t *q)
{
    int i, b;
    for (i = 0; i < 12; i++) {
        uint8_t v = 0;
        for (b = 0; b < 8; b++) v = (uint8_t)(v << 1 | (pw[i * 8 + b] >> 6 & 1));
        q[i] = v;
    }
}

/* the CRC of a Q, as the core checks it (libpcsxcore/misc.c) */
extern unsigned short calcCrc(const unsigned char *d, int len);
static int q_crc_ok(const uint8_t *q) { return calcCrc(q, 10) == (q[10] << 8 | q[11]); }
static uint8_t bcd(unsigned v) { return (uint8_t)(v / 10 << 4 | v % 10); }
/* Q's absolute position is this sector (ADR 1) */
static int q_is_at(const uint8_t *q, uint32_t lba)
{
    uint32_t a = lba + 150;
    return (q[0] & 15) == 1 && q[7] == bcd(a / 4500) && q[8] == bcd(a / 75 % 60) && q[9] == bcd(a % 75);
}

static int read_sub(disc_t *d, uint32_t lba, void *out, int mode)
{
    /* READ CD: any sector type, 1 sector, sync + headers + data + EDC/ECC (2352 bytes),
       and the subchannel if asked */
    uint8_t pkt[12] = { 0xbe, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                        0, 0, 1, 0xf8, 0, 0 };
    uint8_t sector[SECTOR_RAW + 96];
    int with_q = d->big == READ_604_Q || d->big == READ_616_Q;
    uint32_t len = SECTOR_RAW;
    int r;
    if (with_q && mode == SUB_NONE) mode = SUB_Q;   /* this drive layout always has Q */
    if (mode == SUB_RAW) { pkt[10] = 1; len += 96; }
    else if (mode == SUB_Q) { pkt[10] = 2; len += 16; }
    r = cmd_in(d, pkt, 12, sector, len);
    if (!r) {
        memcpy(out, sector, SECTOR_RAW);
        if (mode != SUB_NONE) {   /* the read-ahead thread and the main thread both read */
            if (d->lock) sysMutexLock(d->lock, 0);
            if (mode == SUB_RAW) q_from_raw(sector + SECTOR_RAW, d->q);
            else memcpy(d->q, sector + SECTOR_RAW, 12);
            d->q_lba = lba;
            if (d->lock) sysMutexUnlock(d->lock);
        }
    }
    d->reads++;
    if (r && ++d->errors <= 5 && d->reads > 1)
        logf_("disc: reading sector %u failed (%#x, drive status %#x)\n", (unsigned)lba, (unsigned)r, (unsigned)d->last_status);
    return r;
}

static int read_raw(disc_t *d, uint32_t lba, void *out) { return read_sub(d, lba, out, d->sub_mode); }

/* which subchannel read gives this sector's Q, with a CRC that checks: tried
   on three sectors of the first track (log: the Q bytes) */
static void sub_probe(disc_t *d)
{
    static const int modes[2] = { SUB_RAW, SUB_Q };
    uint8_t sec[SECTOR_RAW];
    int m, k;
    for (m = 0; m < 2; m++) {
        int good = 0;
        for (k = 0; k < 3; k++) {
            uint32_t lba = d->start[1] + 16 + k * 1000;
            int r = read_sub(d, lba, sec, modes[m]);
            const uint8_t *q = d->q;
            if (r) { logf_("disc: subchannel (%s) refused (%#x)\n", modes[m] == SUB_RAW ? "raw P-W" : "Q", (unsigned)r); break; }
            logf_("disc: subchannel (%s) at sector %u: Q %02x %02x %02x %02x:%02x:%02x %02x %02x:%02x:%02x crc %02x%02x (%s, %s)\n",
                  modes[m] == SUB_RAW ? "raw P-W" : "Q", (unsigned)lba, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9],
                  q[10], q[11], q_crc_ok(q) ? "CRC ok" : "CRC bad", q_is_at(q, lba) ? "this sector" : "not this sector");
            if (q_crc_ok(q) && q_is_at(q, lba)) good++;
        }
        if (good == 3) { d->sub_mode = modes[m]; break; }
    }
    d->q_lba = ~0u;
    logf_("disc: subchannel %s\n", d->sub_mode == SUB_RAW ? "read (raw P-W): LibCrypt-protected discs work" :
          d->sub_mode == SUB_Q ? "read (formatted Q): LibCrypt-protected discs work" :
          "not readable: LibCrypt-protected discs freeze later (copy the disc and add its .sbi)");
}

static void disc_free(disc_t *d)
{
    if (!d) return;
    if (d->mode_changed) {
        s32 r = st_disc_auth(0x5004, 0x29);
        logf_("disc: restore normal drive mode: %#x\n", (unsigned)r);
    }
    if (d->opened) st_close(d->fd);
    if (disc_access_restore(&d->access, access_peek, access_poke))
        logf_("disc: access restore incomplete; reboot to clear temporary patches\n");
    if (d->lock) sysMutexDestroy(d->lock);
    free(d->buf);
    free(d);
}

/* allow_patch: 1 = as needed (starting the disc), 0 = never, 2 = only for a disc
   no longer than a CD (the game list's check: a PS3 or movie disc in the drive
   never gets the storage-access patches) */
static int toc_seen;                    /* the last open read a CD-sized track list */
static disc_t *disc_open(char *label, size_t label_n, int allow_patch)
{
    disc_t *d = (disc_t *)calloc(1, sizeof *d);
    uint8_t sec[SECTOR_RAW];
    s32 r;
    int p;
    if (!d) return NULL;
    d->buf = (uint8_t *)memalign(128, 4096);
    if (!d->buf) { free(d); return NULL; }
    if ((r = st_open(BDVD_DEVICE, &d->fd)) != 0) {
        logf_("disc: cannot open the drive (%#x)\n", (unsigned)r);
        d->fd = 0; disc_free(d); return NULL;
    }
    d->opened = 1;
    /* READ TOC is normally permitted even before storage access is unlocked. */
    if (read_toc(d) != 0) {
        d->big = READ_616_RAW;
        if (read_toc(d) != 0) { disc_free(d); return NULL; }
    }
    if (d->audio[1]) { logf_("disc: the first track is audio, not a game disc\n"); disc_free(d); return NULL; }
    toc_seen = d->leadout <= 405000;    /* 90 minutes of CD sectors */
    if (allow_patch == 2 && !toc_seen) { logf_("disc: not a CD (%u sectors)\n", (unsigned)d->leadout); disc_free(d); return NULL; }
    /* First try working homebrew packet formats with no memory changes. */
    for (p = 0; p < READ_N; p++) {
        d->big = p;
        if ((r = read_raw(d, d->start[1] + 16, sec)) == 0) break;
        logf_("disc: sector 16 rejected (%#x, status %#x, %s)\n",
              (unsigned)r, (unsigned)d->last_status, read_name[p]);
    }
    if (p == READ_N && !allow_patch) {
        logf_("disc: plain reads blocked; the disc is identified when it is started\n");
        disc_free(d); return NULL;
    }
    if (p == READ_N) {
        uint8_t sense[18];
        uint8_t pkt[12] = { 3, 0, 0, 0, sizeof sense };
        int saved = d->big;
        d->big = READ_604_RAW;
        if (!cmd_in(d, pkt, 6, sense, sizeof sense))
            logf_("disc: sense format %02x key %02x ASC/ASCQ %02x/%02x\n",
                  sense[0], sense[2] & 15, sense[12], sense[13]);
        d->big = saved;
        logf_("disc: raw reads blocked; checking CFW storage-access signatures\n");
        logf_("disc: kernel TOC=%016llx; LV2 sites A=%016llx B=%016llx D=%016llx\n",
              (unsigned long long)access_peek(2, UINT64_C(0x8000000000003000)),
              (unsigned long long)access_peek(2, UINT64_C(0x80000000002ecaa0)),
              (unsigned long long)access_peek(2, UINT64_C(0x80000000002ecab0)),
              (unsigned long long)access_peek(2, UINT64_C(0x800000000030e648)));
        log_sync();
        r = disc_access_enable(&d->access, access_peek, access_poke);
        logf_("disc: storage access patch result %d (0=verified, -1=kernel, -2=LV1, -3=flag, -4=write)\n", (int)r);
        if (r != 0) {
            /* no LV1 access at all (HEN, or CFW without LV1 peek): nothing can unlock the reads */
            if (access_peek(1, 0x16fa60) == UINT64_C(0xffffffff80010003)) {
                disc_needs_cfw = 1;
                logf_("disc: no LV1 access (HEN?): raw disc reads need custom firmware\n");
            }
            logf_("disc: kernel markers CEX+10=%016llx CEX=%016llx DEX=%016llx\n",
                  (unsigned long long)access_peek(2, UINT64_C(0x80000000002ed818)),
                  (unsigned long long)access_peek(2, UINT64_C(0x80000000002ed808)),
                  (unsigned long long)access_peek(2, UINT64_C(0x800000000030f3b0)));
            logf_("disc: LV1 sites: %016llx %016llx %016llx %016llx\n",
                  (unsigned long long)access_peek(1, 0x16fa60),
                  (unsigned long long)access_peek(1, 0x16fa84),
                  (unsigned long long)access_peek(1, 0x16fafc),
                  (unsigned long long)access_peek(1, 0x16fb04));
            disc_free(d); return NULL;
        }
        for (p = 0; p < 5; ++p) {
            disc_patch_site *v = &d->access.site[p];
            logf_("disc: LV%d @%llx %016llx -> %016llx (%s)\n", v->level,
                  (unsigned long long)v->address, (unsigned long long)v->original,
                  (unsigned long long)v->patched, v->changed ? "temporary" : "already enabled");
        }
        log_sync();
        /* Retry after permission changes, then reset/authenticate if required.
         * 0x29 is the normal disc-tools mode; 1 is PS1 mode. Both are tried. */
        {
            static const uint32_t modes[] = { 0, 0x29, 1 };
            unsigned m;
            for (m = 0; m < sizeof modes / sizeof modes[0]; ++m) {
                if (m) {
                    r = st_disc_auth(0x5004, modes[m]);
                    logf_("disc: drive mode %#x: %#x\n", modes[m], (unsigned)r);
                    if (r) continue;
                    d->mode_changed = 1;
                    sysUsleep(100000);
                }
                for (p = 0; p < READ_N; ++p) {
                    d->big = p;
                    if (!read_raw(d, d->start[1] + 16, sec)) break;
                }
                if (p < READ_N) break;
            }
        }
        if (p == READ_N) {
            logf_("disc: raw reads still fail after storage unlock\n");
            disc_free(d); return NULL;
        }
    }
    d->reads = d->errors = 0;
    /* PS1 discs use Mode 2 (24-byte prefix); tolerate Mode 1 (16-byte prefix)
     * for identifying the label, while preserving raw bytes for the core. */
    p = sec[15] == 1 ? 16 : 24;
    if (sec[0] != 0 || sec[1] != 0xff || sec[11] != 0 ||
        (sec[15] != 1 && sec[15] != 2) || memcmp(sec + p + 1, "CD001", 5)) { logf_("disc: no ISO 9660 volume on the disc\n"); disc_free(d); return NULL; }
    if (label) {
        int i;
        snprintf(label, label_n, "%.32s", (const char *)sec + p + 40);
        for (i = (int)strlen(label) - 1; i >= 0 && label[i] == ' '; i--) label[i] = 0;
        if (!label[0]) snprintf(label, label_n, "PS1 disc");
    }
    logf_("disc: PS1 data track found (sectors read as %s), volume \"%s\"\n", read_name[d->big], label ? label : "");
    return d;
}

/* game list: is there a PS1 disc in the drive? label: its volume name */
int disc_probe(char *label, size_t n)
{
    disc_t *d = disc_open(label, n, 1);
    if (!d) return 0;
    disc_free(d);
    return 1;
}

/* ---- the game list: which game is in the drive (a background thread) ----
   The serial comes from the BOOT line of SYSTEM.CNF (BOOT = cdrom:\SLUS_007.07;1),
   the title from USRDIR/ps1titles.txt (tools/make_titles.py, libretro-database). */
static const uint8_t *sector_data(const uint8_t *sec) { return sec + (sec[15] == 1 ? 16 : 24); }
static uint32_t le32p(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int read_serial(disc_t *d, char *serial, size_t n)
{
    uint8_t sec[SECTOR_RAW];
    const uint8_t *pvd, *dat;
    uint32_t root, size, lba, i, cnf = 0;
    serial[0] = 0;
    if (read_raw(d, d->start[1] + 16, sec)) return -1;
    pvd = sector_data(sec);
    root = le32p(pvd + 156 + 2); size = le32p(pvd + 156 + 10);
    for (lba = 0; lba < (size + 2047) / 2048 && lba < 16 && !cnf; lba++) {
        if (read_raw(d, root + lba, sec)) return -1;
        dat = sector_data(sec);
        for (i = 0; i < 2048 && dat[i] >= 34 && i + dat[i] <= 2048; i += dat[i]) {
            const uint8_t *r = dat + i;
            if (r[32] >= 10 && !strncasecmp((const char *)r + 33, "SYSTEM.CNF", 10)) { cnf = le32p(r + 2); break; }
        }
    }
    if (!cnf || read_raw(d, cnf, sec)) return -1;
    {
        char t[2049], *b, *e, *f;
        size_t k = 0;
        memcpy(t, sector_data(sec), 2048); t[2048] = 0;
        for (b = t; *b && strncasecmp(b, "BOOT", 4); b++) ;
        if (!*b || !(b = strchr(b, '='))) return -1;
        for (b++; *b == ' ' || *b == '\t'; b++) ;
        for (e = b; *e && *e != ';' && *e != '\r' && *e != '\n' && *e != ' '; e++) ;
        *e = 0;
        if ((f = strrchr(b, '\\'))) b = f + 1;
        else if ((f = strrchr(b, ':'))) b = f + 1;
        for (; *b && k + 1 < n; b++)
            if ((*b >= 'A' && *b <= 'Z') || (*b >= '0' && *b <= '9')) serial[k++] = *b;
            else if (*b >= 'a' && *b <= 'z') serial[k++] = (char)(*b - 32);
        serial[k] = 0;
    }
    return serial[0] ? 0 : -1;
}

static int title_for(const char *serial, char *title, size_t n)
{
    char line[256];
    size_t sl = strlen(serial);
    FILE *f = fopen(APP_DIR "/ps1titles.txt", "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, serial, sl) && line[sl] == '\t') {
            char *e = line + strlen(line);
            while (e > line && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
            snprintf(title, n, "%s", line + sl + 1);
            fclose(f);
            return 1;
        }
    fclose(f);
    return 0;
}

/* for the Memory cards tab: a game's name from its serial (SLES01301) */
int disc_title_for(const char *serial, char *title, size_t n) { return title_for(serial, title, n); }

static volatile int id_state;           /* 1 while the thread runs */
static int id_found;
static char id_title[128];
static char id_serial[16];              /* for the game list's cover lookup */
static sys_ppu_thread_t id_thread;
static int id_joinable;

static void id_main(void *arg)
{
    char label[40], serial[16];
    disc_t *d;
    (void)arg;
    id_found = 0; id_title[0] = 0; id_serial[0] = 0;
    toc_seen = 0;
    if (!(d = disc_open(label, sizeof label, 2)) && toc_seen) id_found = 2;   /* a CD it could not read */
    if (d) {
        id_found = 1;
        if (read_serial(d, serial, sizeof serial) == 0) {
            snprintf(id_serial, sizeof id_serial, "%s", serial);
            if (!title_for(serial, id_title, sizeof id_title)) {
                /* not in the list: the label, else the serial as SLUS-00707 */
                if (label[0] && strcmp(label, "PS1 disc")) snprintf(id_title, sizeof id_title, "%s", label);
                else snprintf(id_title, sizeof id_title, "%.4s-%s", serial, serial + 4);
            }
            logf_("disc: in the drive: %s (%s)\n", id_title, serial);
        } else snprintf(id_title, sizeof id_title, "%s", strcmp(label, "PS1 disc") ? label : "");
        disc_free(d);
    }
    id_state = 0;
    sysThreadExit(0);
}

/* starts identifying the disc in the drive (nothing if a check is running) */
void disc_identify_start(void)
{
    if (id_state) return;
    if (id_joinable) { u64 r; sysThreadJoin(id_thread, &r); id_joinable = 0; }
    id_state = 1;
    if (sysThreadCreate(&id_thread, id_main, NULL, 1500, 64 * 1024, THREAD_JOINABLE, (char *)"disc id") != 0) {
        id_state = 0; id_found = 0;
        return;
    }
    id_joinable = 1;
}

/* -1 still reading, 0 no PS1 disc, 1 a disc (title: its name, may be empty),
   2 a CD that could not be read (details in the log) */
int disc_identify_result(char *title, size_t n)
{
    if (id_state) return -1;
    if (id_joinable) { u64 r; sysThreadJoin(id_thread, &r); id_joinable = 0; }
    if (title) snprintf(title, n, "%s", id_title);
    return id_found;
}

/* The disc's serial as read from SYSTEM.CNF ("" while reading or unknown);
   the game list uses it to find the disc's cover in the webMAN pack. */
const char *disc_identify_serial(void)
{
    if (id_state) return "";
    if (id_joinable) { u64 r; sysThreadJoin(id_thread, &r); id_joinable = 0; }
    return id_serial;
}

/* before the disc is opened to play it: the background check must have finished */
void disc_identify_wait(void)
{
    while (id_state) sysUsleep(20000);
    disc_identify_result(NULL, 0);
}

/* the core's read-ahead thread yields with retro_sleep(0) -> nanosleep, which
   PSL1GHT's libc lacks */
int nanosleep(const struct timespec *req, struct timespec *rem)
{
    u64 us = (u64)req->tv_sec * 1000000 + (u64)req->tv_nsec / 1000;
    if (rem) rem->tv_sec = rem->tv_nsec = 0;
    if (us) sysUsleep(us); else sysThreadYield();
    return 0;
}

/* ---- copying the disc to the HDD ----
   Every sector of the disc, 2352 bytes each as the drive reads them (data with
   headers, CD audio as on the disc), goes into <name>.bin, written as
   <name>.bin.part until the last sector is in; then <name>.cue lists the tracks
   (INDEX 01 at each track's start from the TOC). The core then plays it as any
   disc image: no drive seeks while playing. The game list drives it a slice at a
   time so it can show progress and cancel. Subchannel data is not copied
   (as when playing the disc: LibCrypt-protected PAL games need an .sbi file). */
#include <sys/stat.h>
#define COPY_CHUNK 32                   /* sectors per write */
#define COPY_N 16                       /* sectors per READ CD while copying (under 64 KiB, as Linux's ps3rom) */
struct disc_copy {
    disc_t *d;
    FILE *f;
    uint32_t lba, total;
    int mode[100];                      /* data tracks: sector mode (1 or 2) */
    uint8_t *chunk;
    int nchunk;
    uint8_t *bounce;                    /* COPY_N sectors with Q, aligned */
    int multi;                          /* 1 checked and in use, 0 not checked yet, -1 one sector per command */
    int multi_fails;
    uint64_t t_start, t_read;           /* for the log: when it started, time inside read commands */
    unsigned cmds;                      /* read commands sent */
    uint32_t next_report;               /* sector of the next progress line in the log */
    char bin_name[200], bin[512], part[512], cue[512], err[640];
};

/* READ CD for n sectors in one command, in the layout the reader settled on for
   single sectors (syscall, DMA/PIO, with or without Q): one command per sector
   copied at about 0.7 MB/s, the per-command overhead. */
static int read_raw_n(disc_t *d, uint32_t lba, int n, uint8_t *out, uint8_t *bounce)
{
    uint8_t pkt[12] = { 0xbe, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                        0, 0, (uint8_t)n, 0xf8, 0, 0 };
    int with_q = d->big == READ_604_Q || d->big == READ_616_Q, k;
    uint32_t ss = with_q ? SECTOR_RAW + 16 : SECTOR_RAW, len = ss * (uint32_t)n, status[2] = { 0, 0 };
    struct atapi_cmd c;
    s32 r;
    if (with_q) pkt[10] = 2;
    memset(&c, 0, sizeof c);
    memcpy(c.pkt, pkt, 12);
    c.pktlen = 12;
    c.blocks = 1;
    c.block_size = len;
    c.proto = d->big >= READ_604_PIO ? PROTO_PIO_IN : PROTO_DMA;
    c.in_out = 1;
    memset(bounce, 0, len);
    if (d->big == READ_604_RAW || d->big == READ_604_Q || d->big == READ_604_PIO) {
        int tries = 0;
        do {
            r = st_packet(d->fd, &c, bounce, len);
            if ((uint32_t)r == 0x8001000a) sysUsleep(20000);
        } while ((uint32_t)r == 0x8001000a && ++tries < 10);
    } else {
        r = st_atapi(d->fd, &c, bounce, len, status);
        if (!r && status[0]) r = -1;
    }
    if (r) return r;
    for (k = 0; k < n; k++) memcpy(out + (size_t)k * SECTOR_RAW, bounce + (size_t)k * ss, SECTOR_RAW);
    d->reads += (unsigned)n;
    return 0;
}

static s32 fs_disk_free(const char *path, uint64_t *total_free, uint64_t *avail)
{
    lv2syscall3(840, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)total_free, (uint64_t)(uintptr_t)avail);
    return_to_user_prog(s32);
}

/* name: the file name without extension (already safe for the file system).
   NULL and err set if it cannot start. */
struct disc_copy *disc_copy_begin(const char *dir, const char *name, char *err, size_t errn)
{
    struct disc_copy *c = (struct disc_copy *)calloc(1, sizeof *c);
    FILE *t;
    uint64_t tot = 0, avail = 0, need;
    if (!c) { snprintf(err, errn, "Out of memory."); return NULL; }
    disc_identify_wait();               /* the game list's check must not read at the same time */
    mkdir(dir, 0777);
    snprintf(c->bin_name, sizeof c->bin_name, "%s.bin", name);
    snprintf(c->bin, sizeof c->bin, "%s/%s.bin", dir, name);
    snprintf(c->part, sizeof c->part, "%s/%s.bin.part", dir, name);
    snprintf(c->cue, sizeof c->cue, "%s/%s.cue", dir, name);
    if ((t = fopen(c->cue, "rb")) || (t = fopen(c->bin, "rb"))) {
        fclose(t);
        snprintf(err, errn, "Already on the HDD: %s.cue", name);
        free(c); return NULL;
    }
    if (!(c->d = disc_open(NULL, 0, 1))) {
        snprintf(err, errn, "Could not read the disc (details in the log with Debug log on).");
        free(c); return NULL;
    }
    c->total = c->d->leadout;
    need = (uint64_t)c->total * SECTOR_RAW;
    if (fs_disk_free(dir, &tot, &avail) == 0 && avail && avail < need + (16u << 20)) {
        snprintf(err, errn, "Not enough space on the HDD: the copy needs %u MB, %u MB free.",
                 (unsigned)(need >> 20) + 1, (unsigned)(avail >> 20));
        disc_free(c->d); free(c); return NULL;
    }
    c->chunk = (uint8_t *)malloc(COPY_CHUNK * SECTOR_RAW);
    c->bounce = (uint8_t *)memalign(128, COPY_N * (SECTOR_RAW + 16));
    if (!c->bounce) c->multi = -1;
    if (!c->chunk || !(c->f = fopen(c->part, "wb"))) {
        snprintf(err, errn, "Cannot create %s", c->part);
        free(c->chunk); free(c->bounce); disc_free(c->d); free(c); return NULL;
    }
    logf_("disc copy: %u sectors (%u MB) to %s\n", (unsigned)c->total, (unsigned)(need >> 20), c->bin);
    {   /* the drive's top CD read speed (MMC SET CD SPEED, 0xffff = as fast as it can): drives
           often spin a CD slowly for raw reads otherwise. A refusal changes nothing. */
        uint8_t pkt[12] = { 0xbb, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0 };
        s32 r = cmd_in(c->d, pkt, 12, NULL, 0);
        logf_("disc copy: drive set to its top CD speed: %s (%#x)\n", r ? "refused" : "ok", (unsigned)r);
    }
    c->t_start = sysGetSystemTime();
    c->next_report = 32768;
    return c;
}

static int copy_flush(struct disc_copy *c)
{
    if (c->nchunk && fwrite(c->chunk, SECTOR_RAW, (size_t)c->nchunk, c->f) != (size_t)c->nchunk) {
        snprintf(c->err, sizeof c->err, "Writing to the HDD failed (disk full?).");
        return -1;
    }
    c->nchunk = 0;
    return 0;
}

static int copy_write_cue(struct disc_copy *c)
{
    FILE *f = fopen(c->cue, "w");
    int t;
    if (!f) return -1;
    fprintf(f, "FILE \"%s\" BINARY\r\n", c->bin_name);
    for (t = 1; t <= c->d->ntracks; t++) {
        uint32_t l = c->d->start[t];
        fprintf(f, "  TRACK %02d %s\r\n", t, c->d->audio[t] ? "AUDIO" : c->mode[t] == 1 ? "MODE1/2352" : "MODE2/2352");
        fprintf(f, "    INDEX 01 %02u:%02u:%02u\r\n", (unsigned)(l / 75 / 60), (unsigned)(l / 75 % 60), (unsigned)(l % 75));
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* copies for about ms milliseconds; 1 more to do, 0 finished (the .bin and .cue
   are complete), -1 failed (disc_copy_error says why) */
int disc_copy_step(struct disc_copy *c, unsigned ms, uint32_t *done, uint32_t *total)
{
    uint64_t t0 = sysGetSystemTime();
    while (c->lba < c->total && sysGetSystemTime() - t0 < (uint64_t)ms * 1000) {
        uint8_t *sec = c->chunk + (size_t)c->nchunk * SECTOR_RAW;
        int tries = 0, t;
        if (c->multi >= 0 && c->nchunk + COPY_N <= COPY_CHUNK && c->lba + COPY_N <= c->total) {
            uint64_t tr = sysGetSystemTime();
            int k, ok = read_raw_n(c->d, c->lba, COPY_N, sec, c->bounce) == 0;
            c->t_read += sysGetSystemTime() - tr; c->cmds++;
            if (ok && c->multi == 0) {   /* the first batch: the same bytes as single reads? */
                uint8_t one[SECTOR_RAW];
                for (k = 0; k < COPY_N && ok; k++)
                    ok = read_raw(c->d, c->lba + k, one) == 0 && !memcmp(one, sec + (size_t)k * SECTOR_RAW, SECTOR_RAW);
                c->multi = ok ? 1 : -1;
                logf_("disc copy: %d sectors per command %s\n", COPY_N, ok ? "verified" : "differ from single reads: one at a time");
                if (!ok) continue;
            }
            if (ok) {
                for (k = 0; k < COPY_N; k++, c->lba++, c->nchunk++)
                    for (t = 1; t <= c->d->ntracks; t++)
                        if (c->d->start[t] == c->lba && !c->d->audio[t]) c->mode[t] = sec[(size_t)k * SECTOR_RAW + 15] == 1 ? 1 : 2;
                if (c->nchunk == COPY_CHUNK && copy_flush(c)) return -1;
                continue;
            }
            if (++c->multi_fails >= 3 || c->multi == 0) {   /* the drive refuses batches: one at a time */
                c->multi = -1;
                logf_("disc copy: %d-sector reads fail, reading one sector at a time\n", COPY_N);
            }
            /* this batch: sector by sector below (with retries) */
        }
        for (;;) {
            uint64_t tr = sysGetSystemTime();
            int rr = read_raw(c->d, c->lba, sec);
            c->t_read += sysGetSystemTime() - tr; c->cmds++;
            if (rr == 0) break;
            if (++tries == 4) {
                snprintf(c->err, sizeof c->err, "The drive could not read sector %u (scratched or dirty disc?).", (unsigned)c->lba);
                return -1;
            }
            sysUsleep(50000);
        }
        for (t = 1; t <= c->d->ntracks; t++)        /* a data track's mode, from its first sector */
            if (c->d->start[t] == c->lba && !c->d->audio[t]) c->mode[t] = sec[15] == 1 ? 1 : 2;
        c->lba++;
        if (++c->nchunk == COPY_CHUNK && copy_flush(c)) return -1;
    }
    *done = c->lba; *total = c->total;
    if (c->lba >= c->next_report || c->lba >= c->total) {   /* every 77 MB and at the end */
        double secs = (sysGetSystemTime() - c->t_start) / 1e6, mb = c->lba * (double)SECTOR_RAW / 1048576.0;
        logf_("disc copy: %u MB in %.0f s (%.2f MB/s), %s, %.2f ms per read command, reading %.0f%% of the time\n",
              (unsigned)mb, secs, secs > 0 ? mb / secs : 0.0,
              c->multi == 1 ? "16 sectors per command" : "1 sector per command",
              c->cmds ? c->t_read / 1000.0 / c->cmds : 0.0, secs > 0 ? c->t_read / 1e4 / secs : 0.0);
        c->next_report += 32768;
    }
    if (c->lba < c->total) return 1;
    if (copy_flush(c)) return -1;
    if (fclose(c->f) != 0) { c->f = NULL; snprintf(c->err, sizeof c->err, "Writing to the HDD failed (disk full?)."); return -1; }
    c->f = NULL;
    if (rename(c->part, c->bin) != 0) { remove(c->part); snprintf(c->err, sizeof c->err, "Cannot rename %s", c->part); return -1; }
    if (copy_write_cue(c)) { remove(c->bin); snprintf(c->err, sizeof c->err, "Cannot write %s", c->cue); return -1; }
    logf_("disc copy: done, %u sectors read, %u read errors retried\n", c->d->reads, c->d->errors);
    return 0;
}

const char *disc_copy_error(struct disc_copy *c) { return c->err; }
const char *disc_copy_cue(struct disc_copy *c) { return c->cue; }

/* ends a copy (finished or not): the disc is closed (temporary patches undone)
   and an unfinished copy's file removed */
void disc_copy_end(struct disc_copy *c)
{
    if (!c) return;
    if (c->f) { fclose(c->f); remove(c->part); logf_("disc copy: stopped at sector %u\n", (unsigned)c->lba); }
    disc_free(c->d);
    free(c->chunk);
    free(c->bounce);
    free(c);
}

/* ---- the core's physical CD interface (libpcsxcore/cdrom-async.c) ---- */
struct CdrStat { uint32_t Type, Status; uint8_t nodisk, mode1; };   /* as in libpcsxcore/plugins.h */

static void lba2msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    *m = lba / 75 / 60; lba -= *m * 75 * 60;
    *s = lba / 75; *f = lba - *s * 75;
}

void *rcdrom_open(const char *name, uint32_t *total_lba, uint32_t *have_sub)
{
    disc_t *d = disc_open(NULL, 0, 1);
    sys_mutex_attr_t a;
    (void)name;
    if (!d) return NULL;
    memset(&a, 0, sizeof a);              /* as log.c makes its lock */
    a.attr_protocol = SYS_MUTEX_PROTOCOL_FIFO; a.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
    if (sysMutexCreate(&d->lock, &a) != 0) d->lock = 0;
    *total_lba = d->leadout;
    d->q_lba = ~0u;
    sub_probe(d);
    *have_sub = d->sub_mode != SUB_NONE;
    return d;
}

void rcdrom_close(void *stream)
{
    disc_t *d = (disc_t *)stream;
    if (d) logf_("disc: %u sectors read, %u errors; subchannel Q with a bad CRC %u, of another sector %u\n",
                 d->reads, d->errors, d->q_bad, d->q_off);
    disc_free(d);
}

int rcdrom_getTN(void *stream, uint8_t *tn)
{
    disc_t *d = (disc_t *)stream;
    tn[0] = 1; tn[1] = (uint8_t)d->ntracks;
    return 0;
}

/* rt: minutes, seconds, frames (not BCD) of the track start, track 0 = disc end */
int rcdrom_getTD(void *stream, uint32_t total_lba, uint8_t track, uint8_t *rt)
{
    disc_t *d = (disc_t *)stream;
    uint32_t lba = track == 0 ? total_lba : (track <= d->ntracks ? d->start[track] : 0);
    lba2msf(lba + 150, &rt[0], &rt[1], &rt[2]);
    return 0;
}

int rcdrom_getStatus(void *stream, struct CdrStat *stat)
{
    disc_t *d = (disc_t *)stream;
    stat->Type = d->audio[1] ? 2 : 1;
    return 0;
}

int rcdrom_readSector(void *stream, unsigned int lba, void *b)
{
    return read_raw((disc_t *)stream, lba, b);
}

/* the core's subchannel: 96 bytes, Q at 12..23 (libpcsxcore/cdrom.c struct SubQ).
   The Q of the sector read last is kept, so a sector and its Q take one read.
   A LibCrypt sector's Q has a bad CRC, which the core leaves out as a PS1 does. */
int rcdrom_readSub(void *stream, unsigned int lba, void *b)
{
    disc_t *d = (disc_t *)stream;
    uint8_t *o = (uint8_t *)b, sec[SECTOR_RAW];
    uint8_t q[12];
    int tries;
    if (!d || d->sub_mode == SUB_NONE) return -1;
    for (tries = 0; ; tries++) {
        if (d->lock) sysMutexLock(d->lock, 0);
        if (d->q_lba == lba) { memcpy(q, d->q, 12); break; }
        if (d->lock) sysMutexUnlock(d->lock);
        if (tries == 2 || read_raw(d, lba, sec)) return -1;
    }
    memset(o, 0, 96);
    memcpy(o + 12, q, 12);
    if (!q_crc_ok(q)) { if (++d->q_bad <= 40) logf_("disc: subchannel Q at sector %u: bad CRC (LibCrypt?)\n", lba); }
    else if (!q_is_at(q, lba) && (q[0] & 15) == 1) { if (++d->q_off <= 10) logf_("disc: subchannel Q at sector %u is of another sector\n", lba); }
    if (d->lock) sysMutexUnlock(d->lock);
    return 0;
}

/* always "inserted" for now: a failed check would make the core eject the disc
   in the middle of a game (disc changes go through the in-game menu later) */
int rcdrom_isMediaInserted(void *stream)
{
    (void)stream;
    return 1;
}

/* ---- the swap picker's physical entry: tray commands ----
   Upstream v0.2's disc_eject/disc_wait_ready (its disc.c), ported. Upstream
   sends these on the core's open disc stream (its cur_disc), so they only
   work while the core holds a physical disc. The picker needs them while an
   IMAGE is playing — the drive holds whatever disc the user wants to swap
   to, and the core has no stream open on it — so the tray functions open
   their own minimal handle: st_open only, no TOC read, no sector reads, no
   storage-access patches. The drive is only asked TEST UNIT READY /
   START STOP UNIT. The handle is shared by the three calls and closed by
   disc_tray_close(); every path out of the prompt screen closes it, so no
   fd is held after the swap screen returns. */
static disc_t *tray_disc;   /* the prompt flow's drive handle, NULL when closed */

static int tray_open(void)
{
    disc_t *d;
    s32 r;
    if (tray_disc) return 1;
    d = (disc_t *)calloc(1, sizeof *d);
    if (!d) return 0;
    d->buf = (uint8_t *)memalign(128, 4096);
    if (!d->buf) { free(d); return 0; }
    if ((r = st_open(BDVD_DEVICE, &d->fd)) != 0) {
        logf_("disc: cannot open the drive for tray commands (%#x)\n", (unsigned)r);
        free(d->buf); free(d); return 0;
    }
    d->opened = 1;
    /* The lv2 execute-command path (status-returning), the one the header
       comment describes for ATAPI packets; the 604 path would deliver the
       packet too, but only 616 reports the drive's status. */
    d->big = READ_616_RAW;
    tray_disc = d;
    return 1;
}

static void tray_close_raw(void)
{
    disc_t *d = tray_disc;
    if (!d) return;
    tray_disc = NULL;
    /* Minimal close: this handle never read a TOC, never changed the drive
       mode and never installed storage patches, so disc_free's restore
       steps do not apply; just close the fd and free it. */
    if (d->opened) st_close(d->fd);
    free(d->buf);
    free(d);
}

/* START STOP UNIT: byte 4 is LoEj (bit 1) + Start (bit 0). Eject = LoEj,
   load (close the tray) = LoEj + Start. 1 when the drive took the command. */
int disc_tray_eject(void)
{
    static const uint8_t pkt[12] = { 0x1b, 0, 0, 0, 0x02, 0 };
    int r;
    if (!tray_open()) return 0;
    r = cmd_in(tray_disc, pkt, 6, NULL, 0);
    logf_("disc: tray eject: %s (%#x, drive status %#x)\n", r ? "refused" : "done",
          (unsigned)r, (unsigned)tray_disc->last_status);
    return r == 0;
}

static int disc_tray_load(void)
{
    static const uint8_t pkt[12] = { 0x1b, 0, 0, 0, 0x03, 0 };
    int r;
    if (!tray_disc) return 1;   /* nothing open: the tray is not ours to close */
    r = cmd_in(tray_disc, pkt, 6, NULL, 0);
    logf_("disc: tray load: %s (%#x, drive status %#x)\n", r ? "refused" : "done",
          (unsigned)r, (unsigned)tray_disc->last_status);
    return r == 0;
}

/* 1 once the drive has a disc ready to read (TEST UNIT READY), 0 after ms
   milliseconds without one. */
int disc_tray_wait_ready(int ms)
{
    static const uint8_t pkt[12] = { 0, 0, 0, 0, 0, 0 };
    u64 t0 = sysGetSystemTime();
    if (!tray_open()) return 0;
    for (;;) {
        if (cmd_in(tray_disc, pkt, 6, NULL, 0) == 0) { logf_("disc: a disc is ready\n"); return 1; }
        if (sysGetSystemTime() - t0 > (u64)ms * 1000) {
            logf_("disc: no disc ready (drive status %#x)\n", (unsigned)tray_disc->last_status);
            return 0;
        }
        sysUsleep(250000);
    }
}

/* Close the prompt flow's drive handle. Loads the tray first when one is
   open, so the drive is never left open by a cancelled or failed swap;
   loading an already-closed tray is a no-op for the drive. Safe to call
   when nothing is open. */
void disc_tray_close(void)
{
    if (tray_disc) disc_tray_load();
    tray_close_raw();
}
