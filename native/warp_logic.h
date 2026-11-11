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

/* all-episodes (lab): the start positions of Episodes 2-4 (the game's own sNNNN.stp / sNNNN_chp, notes\episode-catalog.md appendix A; flag -1 = a part start or a start position
 * without a CHP flag; where a start position had a list of flags the first one). Separate table: coordinates in these levels reach below 0 and above 5000 in y. A warp to them gives
 * ONLY the target's own flag (the walk-order accumulation of Episode 1 would set contradicting story flags of these stages). */
static const wl_chp_t g_wl_chp2[] = {
    /* the bonus episodes (stages of notes/bonus-episodes.md, start positions of the live census; no CHP flags): The Struggle 600 620 610 630, Little Miss 700 701 711 */
    {600, 0, -1, 14537, 3430, -24139, "The Struggle: hunt"},
    {620, 0, -1, -2872, 716, -74057, "The Struggle: graveyard defence"},
    {610, 0, -1, 44623, 1052, -48793, "The Struggle: village stealth"},
    {630, 0, -1, -12636, 2100, -90674, "The Struggle: canyon"},
    {700, 0, -1, 17271, 1112, -73669, "Little Miss: graveyard and sewer"},
    {701, 0, -1, 19581, 2400, -73890, "Little Miss: the town"},
    {711, 0, -1, 35724, 2000, -30792, "Little Miss: hill and tunnel"},
    {201, 0, -1, 41678, 397, -44751, "start"},
    {201, 1, 41, 35962, 759, -45040, "helicopter spot (siren starts)"},
    {201, 2, 25, 41529, 404, -41258, "fuel warehouse opened"},
    {201, 3, 197, 42320, 600, -46692, "battery hut entry"},
    {201, 4, 15, 36266, 728, -45145, "helicopter spot"},
    {201, 5, 45, 41749, 660, -45626, "tavern 2F (siren stopped)"},
    {201, 6, 51, 41729, 397, -44958, "tavern 1F (Pedro infected)"},
    {201, 7, 18, 44505, 193, -41685, "Pedro joins"},
    {203, 0, 0, 41732, 1176, -52496, "inside the tunnel"},
    {203, 1, 7, 39014, 2000, -57218, "outside the tunnel"},
    {203, 2, 12, 30714, 2450, -59820, "after parting with Neil"},
    {203, 3, 22, 24226, 2050, -65279, "apartment entrance"},
    {203, 4, 24, 28982, 2050, -60344, "in front of the park"},
    {203, 5, 23, 26972, 2450, -65115, "hospital entrance"},
    {203, 6, 68, 22679, 2750, -67032, "before the fat one"},
    {250, 0, -1, 28130, 3000, -13297, "start"},
    {250, 1, 9, 39637, 2500, -23445, "just before the first encounter"},
    {250, 2, 20, 38868, 2000, -31811, "entrance of the area with the key"},
    {251, 0, 50, 43774, 459, -39094, "start"},
    {251, 1, 14, 45302, 98, -43050, "before Glasp"},
    {253, 0, 1, 41746, 1150, -52620, "start"},
    {253, 1, 2, 30433, 2050, -57895, "first building"},
    {253, 2, 3, 28308, 2000, -59319, "main street"},
    {253, 3, 61, 24253, 2050, -65283, "in front of the apartment"},
    {253, 4, 4, 22743, 3250, -64802, "helicopter-crash spot"},
    {253, 5, 14, 26970, 2450, -65090, "hospital entrance"},
    {253, 6, 9, 32297, 1995, -58514, "after Vitaly 1st"},
    {254, 0, 100, 19508, 2400, -74364, "start position"},
    {300, 0, 300, 16907, 1950, -68889, "right after the start"},
    {300, 1, 200, 7036, 1950, -66799, "processing building entrance"},
    {300, 2, 201, 6462, 1950, -65499, "laser-sight room entrance"},
    {300, 3, 202, 2274, 1950, -69994, "slaughter building entrance"},
    {300, 4, 203, -3903, 1550, -65034, "blood bath cleared"},
    {300, 5, 204, -4958, 1550, -70359, "meat slicer cleared"},
    {300, 6, 205, 7236, 1952, -72141, "W building entrance"},
    {300, 7, 207, -5033, 1550, -70737, "before the meat slicer"},
    {300, 8, 208, 6424, 2450, -62874, "Hawk room entrance"},
    {300, 9, 209, 5699, 2402, -74678, "W building midpoint"},
    {300, 10, 210, 6301, 1950, -65489, "after the laser sight"},
    {300, 11, 206, 6311, 2450, -63414, "after taking the glass eye"},
    {300, 12, 40, 6350, 2450, -63249, "after taking the processing-plant key"},
    {300, 13, 50, 5732, 1952, -70042, "after escaping the dismantling yard"},
    {301, 0, 30, 11617, 1150, -77217, "start"},
    {301, 1, 31, 19322, 1300, -80187, "before the sluice with scaffold"},
    {301, 2, 33, 19851, 1100, -72821, "narrow-path area"},
    {301, 3, 32, 15410, 1000, -80453, "after the Evgeny event"},
    {301, 4, 34, 17621, 1000, -79960, "before the stairs"},
    {303, 0, 0, 17507, 1812, -74487, "start (underground)"},
    {303, 1, -1, 19196, 3250, -81162, "Ryan fight start"},
    {303, 2, 80, 17104, 3700, -80594, "inside the control room"},
    {306, 0, -1, 19588, 2359, -73672, "start position"},
    {351, 0, -1, 17284, 1112, -73685, "start"},
    {351, 1, 14, 21857, 1100, -79205, "after lowering Natalia"},
    {351, 2, -1, 13092, 1100, -77956, "for checking: before Evgeny"},
    {351, 3, 50, 15575, 1000, -80432, "before the Glasp fight"},
    {352, 0, 200, 10285, 1100, -78671, "start"},
    {352, 1, 201, 6364, 0, -77687, "NOUSE (unused)"},
    {352, 2, 202, 4753, 0, -76775, "passed sluice 3"},
    {352, 3, 203, -141, 499, -73904, "passed sluice 4"},
    {352, 4, 204, -3635, 800, -73717, "passed the sluice area"},
    {352, 5, 205, 6524, 0, -80224, "before the first sluice"},
    {354, 0, 205, -6597, 1700, -78137, "start position"},
    {354, 1, 200, -15605, 100, -83440, "just before getting the battery"},
    {354, 2, 201, -15061, 105, -83994, "right after the shield Hom is down"},
    {354, 3, 202, -10309, 300, -84498, "right after the 3rd conveyor"},
    {354, 4, 203, -11613, 1700, -87115, "just before the fall"},
    {354, 5, 204, -12431, 1600, -85649, "before the last battle"},
    {354, 6, -1, -10149, 500, -82470, "first hut after descending the cliff"},
    {354, 7, 206, -11894, 300, -84483, "when the fence door opened"},
    {400, 0, -1, 19562, 12739, -81965, "start"},
    {400, 1, 57, 17032, 15962, -82068, "collapse begins"},
    {400, 2, 60, 13811, 15360, -81256, "power room"},
    {400, 3, 64, 14109, 14760, -83855, "outer wall"},
    {400, 4, 42, 18304, 11335, -80940, "last spurt"},
    {451, 0, -1, -15566, 658, -94986, "start"},
    {451, 1, 110, -19598, 1010, -102976, "crane, start"},
    {451, 2, 111, -18600, 1499, -100166, "crane, middle"},
    {451, 3, 113, -18612, 1505, -97575, "crane, end"},
    {452, 0, 0, -19359, 2300, -94586, "start"},
    {452, 1, 15, -25613, -6899, -91859, "B1F"},
    {452, 2, 261, -31391, -6899, -91841, "central building"},
    {452, 3, 90, -32844, -6399, -89614, "when entering left"},
    {452, 4, 150, -33296, -6299, -95114, "right entrance"},
    {452, 5, -1, -33379, -11399, -91372, "B2F"},
    {452, 6, 198, -32617, -6399, -93195, "before the generator"},
    {452, 7, 155, -38722, -6299, -94478, "shield-Hom room entrance"},
    {452, 8, 110, -38376, -6301, -94640, "after leaving the shield-Hom room"},
    {452, 9, 205, -33003, -6399, -90123, "when leaving the left room"},
    {453, 0, 0, -59568, -16499, -105474, "start position"},
    {454, 0, -1, -74134, 3962, -97099, "below the cliff"},
    {455, 0, 0, -36811, -11399, -91883, "start"},
    {455, 1, 12, -44331, -12599, -91847, "entered the mansion"},
    {455, 2, 40, -45136, -12599, -88217, "before the 1F stairs"},
    {455, 3, 20, -45126, -13649, -93472, "got the L1 card"},
    {455, 4, 21, -48267, -13649, -88633, "got the emblem key"},
    {455, 5, 22, -47439, -12599, -95123, "got the L2 card"},
    {455, 6, 201, -45330, -13649, -94972, "rush room"},
    {455, 7, 303, -47194, -16649, -96239, "old abandoned mine C"},
    {455, 8, 330, -57934, -16587, -105560, "before the old mine C boss room"},
    {455, 9, 95, -47565, -13649, -90211, "added: before the morgue"},
};
#define WL_CHP2_N ((int)(sizeof g_wl_chp2 / sizeof g_wl_chp2[0]))

static inline int wl_part_of(int stage)   /* 0 = Claire / Moira half, 1 = Barry / Natalia half, -1 = not a stage of the story (Episode 1: 100-102 / 151-154; 2-4: x00-x49 / x50-x99) */
{
    if (stage >= 100 && stage <= 102) return 0;
    if (stage >= 151 && stage <= 154) return 1;
    if (stage >= 200 && stage <= 499) return stage % 100 > 49 ? 1 : 0;
    if (stage >= 600 && stage <= 711) return 0;   /* the bonus episodes have one part */
    return -1;
}

/* the story's episode field of a bonus stage = its chapter number (live: Little Miss 700 / 701 / 711 read 9 / 10 / 11; The Struggle 600 620 610 630 = 5 6 7 8), 0 for any other stage */
static inline int wl_bonus_episode(int stage)
{
    switch (stage) {
    case 600: return 5;
    case 620: return 6;
    case 610: return 7;
    case 630: return 8;
    case 700: return 9;
    case 701: return 10;
    case 711: return 11;
    default: return 0;
    }
}

/* the part starts (100 / 0, 153 / 0): the opening scene of the part sets flags of its own (153: 2 3 4 13 23 at control); a restart there with only the CHP flag left the
 * N100 copy with no player and a black screen (run w1 2026-11-07 13:10, `sPlayer count=0`). There the stage's flags are KEPT by default (`clean` replaces them). */
static inline int wl_part_start(int stage, int startpos)
{
#ifdef WL_MODEL_BEFORE_W1   /* the first version replaced the words at a part start too (tests/build.sh: the checks must fail on it) */
    (void)stage, (void)startpos;
    return 0;
#else
    return startpos == 0 && (stage == 100 || stage == 153 || stage == 201 || stage == 250 || stage == 306 || stage == 351 || stage == 400 || stage == 451);
#endif
}

static inline const wl_chp_t *wl_find(int stage, int startpos)
{
    for (int i = 0; i < WL_CHP_N; i++)
        if (g_wl_chp[i].stage == stage && g_wl_chp[i].startpos == startpos) return &g_wl_chp[i];
    for (int i = 0; i < WL_CHP2_N; i++)
        if (g_wl_chp2[i].stage == stage && g_wl_chp2[i].startpos == startpos) return &g_wl_chp2[i];
    return 0;
}

/* the CHP flags of that stage up to and including the target in walk order (a stage-entry target gives only its own flag) */
static inline int wl_chp_flags(int stage, int startpos, int *out, int max)
{
    int n = 0;
    if (stage >= 200) {   /* Episodes 2-4 and the bonus stages: the target's own flag only (see g_wl_chp2) */
        const wl_chp_t *c = wl_find(stage, startpos);
        if (!c) return -1;
        if (c->flag >= 0 && max > 0) out[n++] = c->flag;
        return n;
    }
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
    int cross;              /* 1 (`half`): the target is in the OTHER half of the episode (the second-half start of the lobby, notes: allep): the story part is written too */
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
        } else if (!strcmp(tok, "half")) {
            p->cross = 1;
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
    if (!p->chp) { snprintf(err, esz, "stage %d has no start position %d in the warp tables (warp list)", p->stage, p->startpos); return 0; }
    int pc = wl_part_of(cur_stage), pt = wl_part_of(p->stage);
    if (pc < 0) { snprintf(err, esz, "the story is in stage %d, not in a story stage: start the part first (menu start <episode> <part> 1)", cur_stage); return 0; }
    if (pc != pt && !p->cross) { snprintf(err, esz, "stage %d belongs to the other half of the episode (now in stage %d): start that part first", p->stage, cur_stage); return 0; }
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
