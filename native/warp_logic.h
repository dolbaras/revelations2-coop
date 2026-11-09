/* warp_logic.h: the pure part of the lab stage warp (`warp`, warp.inc), so tests/test_warp.c can check it without a game.
 *
 * How the game keeps a checkpoint (Ghidra, notes\ep1-cover.md section 1): the game data of a run lives in cGameInfo = story block + 0x31320; the game area owns a
 * second cGameInfo, the RESTART COPY [aGame + 0x608], which a checkpoint (CHP scenario task FUN_00B4B800) and the end of every level load (FUN_004364C0) fill
 * from the current data. The stock restart (request FUN_004362E0 = sBioMain+0x285B8 |= 0x10000 -> Coop_UpdateMain -> FUN_004286C0 -> aGame state 0xC = FUN_004348D0)
 * copies the restart copy back over the current data (FUN_00973080, 0x1A610 bytes from +0x10F030) and re-creates the game area, which loads mTempData.mStageNo
 * at mStartPosIdx. A warp therefore writes three words and the target stage's flag words into the restart copy and asks for the stock restart: the game itself
 * does the rest, as after a death and "retry from checkpoint".
 *
 * Offsets (cGameInfo-relative): mStageNo +0x10F060, mStartPosIdx +0x10F064, mSceNo +0x10F068; per-stage scenario flags +0x123740: 20 slots of 0x44 bytes
 * {u32 stage id; u32 words[16]}, a free slot holds the id [0x126087C]; flag n = bit 0x80000000 >> (n & 31) of word n >> 5 (FUN_00975B50, FUN_00A90510).
 * The checkpoint table below is the game's own (soft\stage\sNNNN\sNNNN_chp: mStartPosNo + flag number; positions from nativePCNext\soft\stage\sNNNN\sNNNN.stp),
 * in walk order per stage. Lab only: the console is off in a player's game (no REV2_CONSOLE), and nothing of this is in the lobby. */
#ifndef WARP_LOGIC_H
#define WARP_LOGIC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WL_GI_STAGE 0x10F060u
#define WL_GI_STARTPOS 0x10F064u
#define WL_GI_SCENO 0x10F068u
#define WL_GI_FLAGS 0x123740u
#define WL_SLOTS 20
#define WL_SLOT_BYTES 0x44u
#define WL_WORDS 16
#define WL_FLAGS_N (WL_WORDS * 32)
#define WL_MAX_EXTRA 24

typedef struct {
    int stage, startpos, flag;   /* flag: the CHP's flag number (-1: the part start, no CHP) */
    int x, y, z;                 /* start position (whole units) */
    const char *name;
} wl_chp_t;

/* walk order inside each stage (151: sp5 = the cell block west, before the power lever sp3 and the control room sp4) */
static const wl_chp_t g_wl_chp[] = {
    {100, 0, -1, -12138, 236, -19865, "Claire's cell (part start)"},
    {100, 1, 8, -6640, 1600, -17752, "knife room"},
    {100, 2, 102, -3162, 1600, -18204, "checkpoint 2"},
    {100, 3, 21, -2291, 200, -19410, "pool room"},
    {100, 4, 156, -2313, 1600, -17170, "east corridor"},
    {101, 0, 0, 1254, 1600, -17033, "101 entry"},
    {101, 1, 1, 1069, 2350, -15492, "CHP1 (upstairs)"},
    {101, 4, 4, 5445, 2000, -18763, "push boxes / rusty key"},
    {101, 2, 2, 5412, 1600, -20758, "CHP2 (compactor pit)"},
    {101, 3, 3, 3689, 2000, -15007, "torture device (near the boost (101,1,1))"},
    {102, 0, 50, 7214, 2000, -17017, "102 exit"},
    {153, 0, 2, 2899, 100, -381, "landing (part start)"},
    {151, 0, 5, 3987, 1700, -8266, "151 entry"},
    {151, 1, 30, 6132, 1700, -10484, "room 3"},
    {151, 2, 60, 2842, 2350, -13618, "north shutter"},
    {151, 5, 53, 855, 2350, -18378, "cell block west"},
    {151, 3, 82, 3655, 2000, -19797, "power lever"},
    {151, 4, 86, 1417, 2408, -17064, "control room"},
    {152, 0, 0, 7170, 2000, -17016, "152 entry (west)"},
    {154, 0, 10, 16591, 3481, -22797, "154 landing"},
    {154, 1, 41, 12842, 3440, -25526, "Hom house"},
    {154, 2, 110, 13779, 3433, -29347, "forest"},
    {154, 3, 165, 22818, 4107, -26650, "shack area"},
    {154, 4, 200, 26831, 4350, -23977, "high ground"},
    {154, 5, 230, 27726, 4055, -18241, "generator"},
    {154, 6, 245, 25318, 3939, -18895, "crane"},
    {152, 1, 30, 24336, 2996, -14683, "back from the forest (east)"},
};
#define WL_CHP_N ((int)(sizeof g_wl_chp / sizeof g_wl_chp[0]))

static inline int wl_part_of(int stage)   /* Episode 1: 0 = Claire / Moira, 1 = Barry / Natalia, -1 = not a stage of Episode 1 */
{
    if (stage >= 100 && stage <= 102) return 0;
    if (stage >= 151 && stage <= 154) return 1;
    return -1;
}

/* the part starts (100 / 0, 153 / 0): the opening scene of the part sets flags of its own (153: 2 3 4 13 23 at control); a restart there with only the CHP flag left the
 * N100 copy with no player and a black screen (run w1 2026-11-07 13:10, `sPlayer count=0`). There the stage's flags are KEPT by default (`clean` replaces them). */
static inline int wl_part_start(int stage, int startpos)
{
#ifdef WL_MODEL_BEFORE_W1   /* the first version replaced the words at a part start too (tests/build.sh: the checks must fail on it) */
    (void)stage, (void)startpos;
    return 0;
#else
    return startpos == 0 && (stage == 100 || stage == 153);
#endif
}

static inline const wl_chp_t *wl_find(int stage, int startpos)
{
    for (int i = 0; i < WL_CHP_N; i++)
        if (g_wl_chp[i].stage == stage && g_wl_chp[i].startpos == startpos) return &g_wl_chp[i];
    return 0;
}

/* the CHP flags of that stage up to and including the target in walk order (a stage-entry target gives only its own flag) */
static inline int wl_chp_flags(int stage, int startpos, int *out, int max)
{
    int n = 0;
    for (int i = 0; i < WL_CHP_N; i++) {
        if (g_wl_chp[i].stage != stage) continue;
        if (g_wl_chp[i].flag >= 0 && n < max) out[n++] = g_wl_chp[i].flag;
        if (g_wl_chp[i].startpos == startpos) return n;
    }
    return -1;   /* no such checkpoint */
}

static inline void wl_bit_set(unsigned *w, int n, int v)
{
    if (n < 0 || n >= WL_FLAGS_N) return;
    unsigned b = 0x80000000u >> (n & 31);
    if (v) w[n >> 5] |= b;
    else w[n >> 5] &= ~b;
}
static inline int wl_bit_get(const unsigned *w, int n) { return n >= 0 && n < WL_FLAGS_N && (w[n >> 5] & (0x80000000u >> (n & 31))) != 0; }

/* the slot of a stage in a flag table image (20 x {id, words[16]}): found, else the first free one (claimed when claim != 0), else -1. Like FUN_00972390. */
static inline int wl_slot(unsigned *tab, int stage, unsigned free_id, int claim)
{
    for (int i = 0; i < WL_SLOTS; i++)
        if (tab[i * (WL_SLOT_BYTES / 4)] == (unsigned)stage) return i;
    for (int i = 0; i < WL_SLOTS; i++)
        if (tab[i * (WL_SLOT_BYTES / 4)] == free_id) {
            if (claim) {
                tab[i * (WL_SLOT_BYTES / 4)] = (unsigned)stage;
                memset(&tab[i * (WL_SLOT_BYTES / 4) + 1], 0, WL_WORDS * 4);
            }
            return i;
        }
    return -1;
}

typedef struct {
    int stage, startpos;
    int go;                 /* 1: ask the game for the restart (default); 0: write only ("set": the next retry / death uses it) */
    int keep;               /* 1: keep the stage's other flags in the restart copy; 0 (default): the stage's words become exactly the CHP flags + the extras */
    int clean;              /* 1: replace the words even at a part start (see wl_part_start) */
    int nplus, nminus;
    int plus[WL_MAX_EXTRA], minus[WL_MAX_EXTRA];
    const wl_chp_t *chp;
} wl_plan_t;

/* parse "<stage> <startpos> [set|go] [keep] [+n ...] [-n ...]" (also "<stage>" alone = start pos 0); returns 0 and fills err on a bad line.
 * cur_stage: the story's stage now (the warp stays inside the current part of Episode 1: the part's resources and characters are loaded). */
static inline int wl_parse(const char *line, int cur_stage, wl_plan_t *p, char *err, int esz)
{
    memset(p, 0, sizeof *p);
    p->go = 1, p->startpos = 0;
    const char *s = line;
    char tok[24];
    int k = 0, used;
    while (s && sscanf(s, "%23s%n", tok, &used) == 1) {
        s += used;
        if (k == 0) {
            p->stage = atoi(tok);
        } else if (k == 1 && tok[0] >= '0' && tok[0] <= '9') {
            p->startpos = atoi(tok);
        } else if (!strcmp(tok, "set")) {
            p->go = 0;
        } else if (!strcmp(tok, "go")) {
            p->go = 1;
        } else if (!strcmp(tok, "clean")) {
            p->clean = 1;
        } else if (!strcmp(tok, "keep")) {
            p->keep = 1;
        } else if ((tok[0] == '+' || tok[0] == '-') && tok[1] >= '0' && tok[1] <= '9') {
            int v = atoi(tok + 1);
            if (v < 0 || v >= WL_FLAGS_N) { snprintf(err, esz, "flag %d out of 0..%d", v, WL_FLAGS_N - 1); return 0; }
            if (tok[0] == '+') { if (p->nplus < WL_MAX_EXTRA) p->plus[p->nplus++] = v; }
            else if (p->nminus < WL_MAX_EXTRA) p->minus[p->nminus++] = v;
        } else {
            snprintf(err, esz, "unknown word '%s' (usage: warp <stage> [startpos] [set|go] [keep|clean] [+flag ...] [-flag ...])", tok);
            return 0;
        }
        k++;
    }
    if (k == 0) { snprintf(err, esz, "usage: warp <stage> [startpos] [set|go] [keep|clean] [+flag ...] [-flag ...] | warp status | warp list"); return 0; }
    p->chp = wl_find(p->stage, p->startpos);
    if (!p->chp) { snprintf(err, esz, "stage %d has no start position %d in the Episode 1 table (warp list)", p->stage, p->startpos); return 0; }
    int pc = wl_part_of(cur_stage), pt = wl_part_of(p->stage);
    if (pc < 0) { snprintf(err, esz, "the story is in stage %d, not in Episode 1: start the part first (menu start 1 <part> 1)", cur_stage); return 0; }
    if (pc != pt) { snprintf(err, esz, "stage %d belongs to the other half of Episode 1 (now in stage %d): start that part first", p->stage, cur_stage); return 0; }
    return 1;
}

/* the stage's 16 flag words after the warp: (keep ? old : 0) | the CHP flags up to the target | plus, minus cleared */
static inline void wl_words(const wl_plan_t *p, const unsigned *old, unsigned *out)
{
    int f[32], n = wl_chp_flags(p->stage, p->startpos, f, 32);
    int keep = p->keep || (wl_part_start(p->stage, p->startpos) && !p->clean);
    for (int i = 0; i < WL_WORDS; i++) out[i] = keep && old ? old[i] : 0;
    for (int i = 0; i < n; i++) wl_bit_set(out, f[i], 1);
    for (int i = 0; i < p->nplus; i++) wl_bit_set(out, p->plus[i], 1);
    for (int i = 0; i < p->nminus; i++) wl_bit_set(out, p->minus[i], 0);
}

/* ---- the retry coupling of a session (0.2.0 product rule, not only lab)
 * The pause menu's "Retry" row (pause result 2, sBioMain_vf6 -> FUN_004362E0) restarts ONE game from ITS checkpoint; the glue of the other PC only marks itself loaded
 * (a full load and a stage change look the same to it: CS_N_LOAD_BEGIN), so the partner would stay where it was while the retrying PC goes back: the pair splits.
 * The death-screen retry is coupled already (both are dead, the partner confirms its own death screen). Rule: the retrying PC tells the partner (event RETRY, reason
 * pause); the partner, in play and alive, asks its own game for the same stock restart (each PC loads its own checkpoint, as after a death). The lab warp rides on the same
 * event (reason warp + the warp line): the partner writes the same checkpoint into its restart copy and restarts. */
enum { WL_RC_PAUSE = 1, WL_RC_WARP = 2 };
enum { WL_RC_IGNORE = 0, WL_RC_RESTART = 1, WL_RC_APPLY_WARP = 2 };
/* what the receiver does: the coupling switch, the reason, its aGame state (3 = in play), its own human's hp; why is set for the log */
static inline int wl_retry_decide(int on, int reason, int agame_state, int my_hp, const char **why)
{
#ifdef WL_MODEL_BEFORE_RC   /* rev2-int before ep1-cover: nothing was coupled (the partner of a pause Retry only marked itself loaded) */
    (void)on, (void)reason, (void)agame_state, (void)my_hp;
    *why = "no coupling";
    return WL_RC_IGNORE;
#endif
    if (!on) { *why = "the retry coupling is off"; return WL_RC_IGNORE; }
    if (reason != WL_RC_PAUSE && reason != WL_RC_WARP) { *why = "unknown reason"; return WL_RC_IGNORE; }
    if (agame_state == 11) { *why = "this PC shows its death screen (the death retry is coupled already)"; return WL_RC_IGNORE; }
    if (agame_state != 3) { *why = "this game is not in play (a load, a scene state or a menu)"; return WL_RC_IGNORE; }
    if (my_hp <= 0) { *why = "my human is dead (the death screen retry follows)"; return WL_RC_IGNORE; }
    *why = reason == WL_RC_WARP ? "the partner warped: the same checkpoint here" : "the partner retries from its checkpoint: this PC retries too";
    return reason == WL_RC_WARP ? WL_RC_APPLY_WARP : WL_RC_RESTART;
}

#endif
