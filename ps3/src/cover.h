/* Cover art for the game list, read from the user's own files and the
   webMAN MOD cover pack (never bundled in this package: the pack's art
   is not ours to ship).
   stb_image v2.28 (public domain, in stb_image.h) decodes JPG/PNG with no
   new library for the CI toolchain to build; formats it cannot read fall
   back to the placeholder panel, they never crash the menu.
   Small by design, for an old machine: only the selected game's cover is
   kept, never the whole list; a file over 1 MB or an image over 1024x1024
   is refused outright (the pack's covers are 268x268 or smaller, so this
   cap has headroom without letting a stray huge image eat PS3 memory;
   art larger than the 264px panel box is fitted to it on load).
   Lookup, in order: (1) a cover named after the game file itself (the
   user's own custom cover, highest priority), (2) the user's own covers
   folder (USRDIR/covers/), by game file name, then by serial, (3) the
   pack by the serial in the file name — or, when the name carries none,
   by the serial read out of the disc image itself (SYSTEM.CNF, .cue /
   .m3u / .pbp; .chd is compressed and cannot be read this way),
   (4) webMAN MOD's own game list, mygames.xml, whose icon path is used
   directly (see cover.c).
   A missing or stale mygames.xml simply skips layer 4; it is never an
   error and never blocks the list. */
#ifndef COVER_H
#define COVER_H

typedef struct {
    unsigned char *px;      /* RGBA, w*h*4 bytes, or NULL when no cover */
    int w, h;
} cover_img_t;

/* The serial of a game file, as the disc code writes it ("SLUS00707",
   out is 10 bytes). From the file name when it has one, else from the
   image (see cover.c). 1 when found, 0 with out empty otherwise. */
int cover_serial(const char *path, char *out);

/* Load the cover for one game list entry. is_disc: the disc in the drive
   (disc_serial is its serial, "" when unknown); otherwise game_path is the
   game file and its name is used for the lookup. Returns 1 with *out filled
   when a cover was decoded, 0 with px NULL when none was found or readable. */
int cover_load(const char *game_path, int is_disc, const char *disc_serial, cover_img_t *out);
void cover_free(cover_img_t *c);
/* Forget the parsed mygames.xml (call it when the game list is rescanned,
   so a refreshed webMAN list is picked up on the next cover lookup). The
   XML itself is read lazily, once, on the next lookup after this call. */
void cover_xml_invalidate(void);

#endif
