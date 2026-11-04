/*
 * ach_logic.h: stage 8b, achievements in co-op. Pure data and decisions (no game access), shared by the DLL glue (ach.inc) and the offline tests
 * (tests\test_ach.c). The design and the evidence for every row: docs\achievements.md (public), notes\stage8b.md (private).
 *
 * How the game awards an achievement (Revelations 2, build 3658995): game events bump the profile's stat table (FUN_00979580, "global total by kind", u32 x 0x51 at
 * story+0x20+0x1BC; FUN_00972E90 = the same event per player slot; FUN_00972ED0 = the human ledger), then a record wrapper compares the total with a goal and calls
 * Ach_SetRecordState(record, 2) (0x408F00), which sets the record bit in the save and queues the Steam unlock (Ach_RecordToSteam -> Ach_QueueSteamUnlock ->
 * AchNet_PostUnlockList -> ISteamUserStats::SetAchievement). The other records come from the local save (episode clears) or from story flags.
 *
 * Co-op rule (owner, RE5/RE6 style): every PC runs its own game and credits ITS human only. So a stat bump for a slot whose player is not the local human (type
 * 7920 != 0: the partner's puppet) must not move this profile, and a bump that only the other PC can see (an enemy of the host that reacts to the client's human)
 * is forwarded to the PC whose human caused it.
 */
#ifndef ACH_LOGIC_H
#define ACH_LOGIC_H

enum {                       /* how an achievement behaves in co-op */
    ACHR_PLAYER = 1,         /* earned by one human: credited only to the PC of the human who did it */
    ACHR_SHARED,             /* story / save state: every PC reaches it through its own copy of the campaign (the synced world) */
    ACHR_RISKY,              /* credit depends on a state the sync does not mirror exactly */
    ACHR_NA                  /* not reachable in the story campaign (Raid / bonus areas) */
};

enum {                       /* where the cause is seen */
    ACHF_NONE = 0,
    ACHF_KILLREP,            /* the kill report (FUN_00986230) of the PC whose human kills: the client runs it for its own kills (proven in 6d) */
    ACHF_ENEMY,              /* enemy-side code (AI of the host's real enemy reacting to a human): only the host sees it, forwarded to the client's PC */
    ACHF_SAVE,               /* local save / story flags / episode state machine */
    ACHF_PICKUP              /* the pickup window of the human who picks the item up */
};

typedef struct {
    unsigned char steam;     /* index in the game's name table (ISteamUserStats name = ach_name[steam]) */
    unsigned short rec;      /* the game's record id */
    const char *name;
    unsigned char rule;
    unsigned char feeder;
    unsigned char kind;      /* stat kind whose global total the record compares with its goal (0xFF = none) */
    const char *note;
} ach_entry_t;

#define ACH_N 43
#define ACH_NOKIND 0xFF

static const ach_entry_t ach_table[ACH_N] = {
    {1, 0x02, "ACH_ALL_EP1_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "Ach_OnEpisodeClear, campaign type 0, episode cleared in the local save"},
    {2, 0x12, "ACH_SURVIVAL_ALL_EP1_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "campaign type 2 (Survival) must be the same on both PCs"},
    {3, 0x17, "ACH_INVISIBLE_ALL_EP1_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "campaign type 1 (Invisible) must be the same on both PCs"},
    {4, 0x35, "ACH_CHALLENGE_MEDAL_EP1_COMP", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "challenge medals of the episode in the local save (they follow the per-player stats)"},
    {5, 0x3A, "ACH_CAMPAINF_PT_MEDIUM", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "story points (story+0xE8C) of the local profile at the chapter end"},
    {6, 0x49, "ACH_FLAME_NOT_USED", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "area 0x66 model event + a local save flag"},
    {7, 0x4F, "ACH_PROGRESSOR_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x0F, "class kills of the Progressor"},
    {8, 0x54, "ACH_HOMUNCULUS_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x14, "class kills of the Homunculus"},
    {9, 0x64, "ACH_KNIFE_WEAPON_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x26, "kills with the knife in the killer's hand"},
    {10, 0x65, "ACH_BLOW_ATK_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x28, "kills by a melee blow of the killer (action 0x2B/0x2D/0x2E)"},
    {11, 0x66, "ACH_GRAP_BLOW_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x2A, "kills by the grapple finisher (action 0x2F)"},
    {12, 0x67, "ACH_BACK_ATK_KILL", ACHR_PLAYER, ACHF_ENEMY, 0x2B, "back attack: counted by the enemy's own code (host); forwarded to the client"},
    {13, 0x6A, "ACH_LIGHT_ATK", ACHR_PLAYER, ACHF_ENEMY, 0x30, "flashlight hits (enemy grapple-dead demo actions of the host)"},
    {14, 0x6C, "ACH_BAR_WEAPON_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x33, "kills with the bar-type melee weapons"},
    {15, 0x6E, "ACH_STONE_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x35, "kills with thrown stones / bottles"},
    {16, 0x70, "ACH_KNIFE_CLEAR", ACHR_PLAYER, ACHF_SAVE, ACH_NOKIND, "episode 1 cleared with the knife only: follows the local human's weapon use"},
    {17, 0x72, "ACH_SHOT_TWO_KILL", ACHR_NA, ACHF_NONE, ACH_NOKIND, "returns at once in episode areas (Raid / bonus areas only)"},
    {18, 0x78, "ACH_HOMU_WEAPON_DEST", ACHR_PLAYER, ACHF_ENEMY, 0x38, "homunculus weapon destroyed: enemy intel of the host"},
    {19, 0x79, "ACH_PROG_JUMPATK_STOP", ACHR_PLAYER, ACHF_ENEMY, 0x39, "progressor jump attack stopped: enemy intel of the host"},
    {20, 0x7B, "ACH_GRAP_COMBINATION", ACHR_RISKY, ACHF_ENEMY, ACH_NOKIND, "record set directly by the host's enemy code for the involved human (no stat kind)"},
    {21, 0x7D, "ACH_ASSIST_HOLD_HELP", ACHR_PLAYER, ACHF_ENEMY, 0x37, "the human who frees a held partner: enemy intel of the host"},
    {22, 0x7E, "ACH_BOTTLE_FIVE_KILL", ACHR_NA, ACHF_NONE, ACH_NOKIND, "returns at once in episode areas (Raid / bonus areas only)"},
    {23, 0xB4, "ACH_SYS_RAID_MISSION_CLEAR", ACHR_NA, ACHF_NONE, ACH_NOKIND, "Raid only: the mod is off in Raid"},
    {24, 0xB5, "ACH_SYS_RAID_USE_JUDGMENT", ACHR_NA, ACHF_NONE, ACH_NOKIND, "Raid only"},
    {25, 0xB6, "ACH_SYS_RAID_DUNGEON_CLEAR", ACHR_NA, ACHF_NONE, ACH_NOKIND, "Raid only"},
    {26, 0x03, "ACH_ALL_EP2_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {27, 0x13, "ACH_SURVIVAL_ALL_EP2_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {28, 0x18, "ACH_INVISIBLE_ALL_EP2_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {29, 0x36, "ACH_CHALLENGE_MEDAL_EP2_COMP", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {30, 0x4A, "ACH_PEDRO_PAST_KILL", ACHR_PLAYER, ACHF_KILLREP, ACH_NOKIND, "special enemy kill of the past timeline (setter not resolved: assumed the kill report)"},
    {31, 0x57, "ACH_GHOST_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x18, "class kills of the Ghost"},
    {32, 0x04, "ACH_ALL_EP3_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {33, 0x14, "ACH_SURVIVAL_ALL_EP3_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {34, 0x19, "ACH_INVISIBLE_ALL_EP3_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {35, 0x37, "ACH_CHALLENGE_MEDAL_EP3_COMP", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {36, 0x4B, "ACH_MOIRA_NEIL_KILLED", ACHR_PLAYER, ACHF_KILLREP, ACH_NOKIND, "boss kill (setter not resolved: assumed the kill report)"},
    {37, 0x4C, "ACH_FLOOD_GATE_ITEM_GET", ACHR_PLAYER, ACHF_PICKUP, ACH_NOKIND, "episode 3 part 1 area 0x15F: the key item window of the human who picks it up"},
    {38, 0x05, "ACH_ALL_EP4_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {39, 0x15, "ACH_SURVIVAL_ALL_EP4_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {40, 0x1A, "ACH_INVISIBLE_ALL_EP4_CLEAR", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {41, 0x38, "ACH_CHALLENGE_MEDAL_EP4_COMP", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "see EP1"},
    {42, 0x4D, "ACH_PAST_GHOST_KILL", ACHR_PLAYER, ACHF_KILLREP, ACH_NOKIND, "episode 4 part 0: per-part ghost kills (FUN_00975ae0(0,0x18)) against the goal"},
    {43, 0x4E, "ACH_FLOOD_GATE_USED", ACHR_SHARED, ACHF_SAVE, ACH_NOKIND, "episode 4 part 1: story flags 6 and 7 (synced), polled every frame"},
};

/* ACH_ALL_ACHIEVEMENT_COMP (Steam index 0) is derived: record 1 when 0x76 of the records 2..0x7F are done in the local save; it has no entry in the game's record map */
#define ACH_ALL_COMP_RECORD 1

static const ach_entry_t *ach_by_record(unsigned rec)
{
    for (int i = 0; i < ACH_N; i++)
        if (ach_table[i].rec == rec) return &ach_table[i];
    return 0;
}

static const ach_entry_t *ach_by_kind(unsigned kind)
{
    for (int i = 0; i < ACH_N; i++)
        if (ach_table[i].kind == kind && kind != ACH_NOKIND) return &ach_table[i];
    return 0;
}

/* The stat kinds that feed a Steam-mapped record, with the game's record check for the kind (the "wrapper": called with ecx = sBioAchievement right after the bump, it compares the
 * global total with the goal and calls Ach_SetRecordState). fwd = the HOST's enemy code bumps this kind for the human it involves (the client's AI is off, so the client's PC never
 * sees the cause): such a bump made for the partner's puppet on the host is forwarded to the partner's PC. The other kinds come from the kill report, which every PC runs for its own
 * human's kills. */
typedef struct { unsigned char kind; unsigned char fwd; unsigned wrapper; } ach_kindw_t;
#define ACH_NKINDW 13
static const ach_kindw_t ach_kindw[ACH_NKINDW] = {
    {0x0F, 0, 0x409400},   /* PROGRESSOR_KILL    */
    {0x14, 0, 0x4092E0},   /* HOMUNCULUS_KILL    */
    {0x18, 0, 0x4092B0},   /* GHOST_KILL         */
    {0x26, 0, 0x4092F0},   /* KNIFE_WEAPON_KILL  */
    {0x28, 0, 0x409190},   /* BLOW_ATK_KILL      */
    {0x2A, 0, 0x4092C0},   /* GRAP_BLOW_KILL     */
    {0x2B, 1, 0x409170},   /* BACK_ATK_KILL      */
    {0x30, 1, 0x409340},   /* LIGHT_ATK          */
    {0x33, 0, 0x409180},   /* BAR_WEAPON_KILL    */
    {0x35, 0, 0x409610},   /* STONE_KILL         */
    {0x37, 1, 0x409160},   /* ASSIST_HOLD_HELP   */
    {0x38, 1, 0x4092D0},   /* HOMU_WEAPON_DEST   */
    {0x39, 1, 0x4093F0},   /* PROG_JUMPATK_STOP  */
};

static const ach_kindw_t *ach_kind_find(unsigned kind)
{
    for (int i = 0; i < ACH_NKINDW; i++)
        if (ach_kindw[i].kind == kind) return &ach_kindw[i];
    return 0;
}

static const ach_kindw_t *ach_fwd_find(unsigned kind)
{
    const ach_kindw_t *k = ach_kind_find(kind);
    return k && k->fwd ? k : 0;
}

/* Records that the enemy code sets DIRECTLY (no stat kind) for the human it involves: FUN_006067B0 sets 0x74, 0x7A, 0x7B (the Steam-mapped GRAP_COMBINATION) and 0x7C, and
 * uEm1100Intel_vf62 sets 0x7B / 0x7C through FUN_00603F20. While the enemy's target is the partner's puppet these sets are swallowed (context guard); every other record set,
 * including the wrapper checks of the counted kinds that run inside the same functions, passes. */
static int ach_ctx_record(unsigned rec)
{
    return rec == 0x74 || rec == 0x7A || rec == 0x7B || rec == 0x7C;
}

/* ---- the gate: what happens to the stat bump (kind, n) the game makes for `slot` ---- */
typedef struct {
    int session;             /* a co-op session is running (otherwise the game is untouched) */
    int gate_on;             /* `net ach gate` (default 1) */
    int is_host;
    int fwd_on;              /* `net ach fwd` / REV2_ACHFWD=1: forward the partner's enemy-side bumps */
} ach_cfg_t;

enum { ACH_ALLOW = 0,        /* the game's own bump stands (the local human's, or no session) */
       ACH_UNDO,             /* not this profile's: the global total is restored and the per-slot bump is skipped */
       ACH_UNDO_FORWARD };   /* ... and the partner's PC gets the event (it is the only PC that cannot see the cause) */

/* slot_local: the slot's player is a type-0 human on this PC; slot_remote: it is the partner's puppet */
static int ach_gate(const ach_cfg_t *c, int slot_local, int slot_remote, unsigned kind)
{
    if (!c->session || !c->gate_on) return ACH_ALLOW;
    if (slot_local) return ACH_ALLOW;
    if (c->is_host && c->fwd_on && slot_remote && ach_fwd_find(kind)) return ACH_UNDO_FORWARD;
    return ACH_UNDO;
}

/* The receiver credits the forwarded event unless its own code already bumped the same kind for its human a moment ago (the replica ran the same cause itself):
 * a window instead of a counter, so a doubled cause never counts twice (and two genuine events within the window count once: undercount, never overcount). */
static int ach_dedupe_ok(unsigned now_ms, unsigned last_local_ms, unsigned window_ms)
{
    if (!last_local_ms) return 1;
    return (unsigned)(now_ms - last_local_ms) >= window_ms;
}

/* Are the Steam writers blocked? REV2_ACHBLOCK=1 blocks, =0 passes, unset follows the test console (REV2_CONSOLE set = a lab / test run = blocked; a player's start = real). */
static int ach_block_default(int has_env, char env_first, int has_console)
{
    return has_env ? env_first == '1' : has_console;
}

/* ---- wire format of the forwarded event (EV_ACH_BUMP, 8 bytes): u8 kind, u8 n, u16 seq, u32 tick (sender's ms) ---- */
enum { ACH_EV_BUMP_LEN = 8 };
static void ach_pack_bump(unsigned char *b, unsigned kind, unsigned n, unsigned seq, unsigned tick)
{
    b[0] = (unsigned char)kind, b[1] = (unsigned char)n, b[2] = (unsigned char)seq, b[3] = (unsigned char)(seq >> 8);
    b[4] = (unsigned char)tick, b[5] = (unsigned char)(tick >> 8), b[6] = (unsigned char)(tick >> 16), b[7] = (unsigned char)(tick >> 24);
}
static int ach_unpack_bump(const unsigned char *b, unsigned len, unsigned *kind, unsigned *n, unsigned *seq)
{
    if (len < ACH_EV_BUMP_LEN) return 0;
    *kind = b[0], *n = b[1], *seq = (unsigned)(b[2] | (b[3] << 8));
    return *kind < 0x51 && *n >= 1 && *n <= 50;
}

/* ---- episode clears (stage 6h; read from the code, shown live on the pair) -----------------------------------------------------------------------------------------------------------------
 * The save keeps two "episode cleared" bit tables, byte [difficulty + 4 * campaign type], bit (episode - 1). `Game_EpisodeClearHandler` (0x431680) marks table B (FUN_0097E880, save + 0xE8C) after
 * part 0 (Claire / Moira) and table A (FUN_0097E820, save + 0xE70) after part 1 (Barry / Natalia); `Ach_OnEpisodeClear` (0x409230) asks table A (FUN_0097D660): ACH_ALL_EPn_CLEAR is credited at the
 * end of part 1, the end of part 0 fires nothing. Part = story + 0x15C4A8, difficulty = story + 0x1403E8. */
typedef struct { unsigned char a[7 * 4], b[7 * 4]; } ach_clear_tables_t;
static void ach_epclear_mark(ach_clear_tables_t *t, int part, int type, int ep, int diff)
{
    if (ep < 1 || ep > 4 || diff < 0 || diff > 3 || type < 0 || type > 6) return;
    (part == 0 ? t->b : t->a)[diff + 4 * type] |= (unsigned char)(1u << (ep - 1));
}
static int ach_epclear_credited(const ach_clear_tables_t *t, int type, int ep)
{
    if (ep < 1 || ep > 4 || type < 0 || type > 6) return 0;
    for (int d = 0; d < 4; d++)
        if (t->a[d + 4 * type] & (1u << (ep - 1))) return 1;
    return 0;
}

#endif
