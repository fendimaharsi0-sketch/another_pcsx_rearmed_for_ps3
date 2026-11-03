/* Cheat database parsing + per-game cheat state, for the PS3 front-end.
 * Pure C (no PS3 headers): the parser and the state file are host-testable.
 *
 * The database is DuckStation's chtdb (the cheats/ directory, one INI-like
 * .cht file per serial). The execution engine is NOT here: upstream
 * PCSX-ReARMed's
 * libpcsxcore/cheat.c (AddCheat / ApplyCheats at VBlank) already exists in
 * this tree and is fed through retro_cheat_set by main.c. This module only
 * turns a .cht file into codes the engine understands, and remembers which
 * codes the user enabled.
 *
 * Format rules (from DuckStation's cheats.cpp, kept deliberately close):
 * - [Name] starts a code; a backslash in a name is a UI group, shown as-is.
 * - Metadata lines (Key = Value) are recognised ANYWHERE in a section:
 *   Type (Gameshark/Assembly), Activation (Manual/EndFrame/StartFrame),
 *   Option (Name:Value, repeatable), OptionRange (min:max), Author,
 *   Description, Ignore. Unknown keys are ignored.
 * - Body lines are two hex words (ADDR VAL). Lines that do not parse as
 *   such are skipped, DuckStation-style. Comments start with ';' or '#'.
 * - A '?' run in the second word is an option field: the chosen option
 *   value is substituted into those bits before the code is fed to the
 *   engine. A raw '?' line must NEVER reach the engine: its sscanf "%x"
 *   reads the '?' as 0 and silently writes the wrong value.
 * - The engine only implements 17 classic opcodes (see cheat.h). A code
 *   containing any other opcode is marked unsupported and never fed: the
 *   engine would disable the whole code at apply time without telling the
 *   user why. SLIDE (0x50) and MEMCPY (0xC2) consume the following line as
 *   payload, so the filter skips it instead of judging it as an opcode.
 * - Activation mapping: EndFrame is the engine's native timing (ApplyCheats
 *   runs at VBlank). StartFrame has no engine equivalent and runs at
 *   VBlank too. Manual codes are fed by the Save action and disabled after
 *   the first emulated frame (main.c cheat_post_frame); they are NOT fed
 *   at game start or when the gate is switched on, because a manual code
 *   fires on the user's trigger, not on boot.
 * - State file (one per serial, next to the per-game settings): one enabled
 *   code name per line, or Name<TAB>value for a code with an option value.
 *   Names, never indices: the database order changes between releases.
 *   A name missing from the current database is skipped, so a database
 *   update degrades gracefully instead of enabling the wrong code. */
#ifndef PS1PS3_CHEATDB_H
#define PS1PS3_CHEATDB_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;             /* exact [Name] text (malloc'd) */
    int type_assembly;      /* Type = Assembly: needs a MIPS assembler, unsupported */
    int bad_meta;           /* unknown Type or Activation: shown, never fed */
    int ignored;            /* Ignore = true: dropped at parse time, never listed */
    int manual;             /* Activation = Manual */
    int start_frame;        /* Activation = StartFrame (runs at VBlank, like EndFrame) */
    int supported;          /* 0 = extended opcode / '?' in address word / unparseable */
    int has_options;        /* Option lines or OptionRange present */
    int is_range;           /* OptionRange (numeric) instead of an Option list */
    uint32_t range_min, range_max;
    char **opt_names;       /* Option list names */
    uint32_t *opt_vals;     /* Option list values, same order */
    int n_opts;
    char *body;             /* body lines only, "\n"-joined, malloc'd */
    int has_question;       /* a '?' field exists in the body (needs a value) */
} cht_code;

typedef struct {
    cht_code *codes;
    int n_codes;
} cht_db;

/* Loads a .cht file. Returns 0 on success, -1 if the file cannot be opened,
   -2 on allocation failure. A file that parses to zero codes is success
   (an empty list), not an error: the screen shows "no cheats". */
int cht_db_load(const char *path, cht_db *db);
void cht_db_free(cht_db *db);

/* Builds the engine string for one code: body lines normalised to
   "ADDR VAL", with every '?' field replaced by opt_value (masked to the
   field width and shifted to the field position). Returns a malloc'd
   string, or NULL if the code is unsupported. Caller frees. */
char *cht_code_substitute(const cht_code *c, uint32_t opt_value);

/* Dashed serial for file names: the core reports CdromId dashless
   ("SLUS01251"); database and state files use "SLUS-01251". A serial that
   already has a dash, or is too short to dash, is copied unchanged. */
void cht_dashed_serial(const char *serial, char *out, size_t n);

/* ---- per-game state file ---- */
typedef struct {
    char *name;
    int has_value;
    uint32_t value;
} cht_state;

int cht_state_load(const char *path, cht_state **out, int *n_out);
void cht_state_free(cht_state *st, int n);
/* Writes the enabled codes: one name per line, Name<TAB>value for codes
   with options. enabled[i] and opt_values[i] run parallel to codes[]. */
int cht_state_save(const char *path, const cht_code *codes,
                   const int *enabled, const uint32_t *opt_values, int n);

#endif
