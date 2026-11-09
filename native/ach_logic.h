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
    ACHF_PICKUP,             /* the pickup window of the human who picks the item up */
    ACHF_LEDGER              /* the per-shot ledger of slot 0 in sPlayer (FUN_006E8310): the PC whose human fired the shot / threw the bottle */
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
    {13, 0x6A, "ACH_LIGHT_ATK", ACHR_PLAYER, ACHF_ENEMY, 0x30, "Moira's light stuns (FUN_00986560 from the enemy's stunned actions): bumped only for a type-0 human, never for a puppet"},
    {14, 0x6C, "ACH_BAR_WEAPON_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x33, "kills with the bar-type melee weapons"},
    {15, 0x6E, "ACH_STONE_KILL", ACHR_PLAYER, ACHF_KILLREP, 0x35, "kills with thrown stones / bottles"},
    {16, 0x70, "ACH_KNIFE_CLEAR", ACHR_PLAYER, ACHF_SAVE, ACH_NOKIND, "episode 1 cleared with the knife only: follows the local human's weapon use"},
    {17, 0x72, "ACH_SHOT_TWO_KILL", ACHR_PLAYER, ACHF_LEDGER, ACH_NOKIND, "two kills with one shot: slot 0's shot ledger (sPlayer+0x2124) on the shooter's PC; FUN_006E8310 returns only for stage ids >= 1000 (Area_IsEpisodeArea)"},
    {18, 0x78, "ACH_HOMU_WEAPON_DEST", ACHR_PLAYER, ACHF_ENEMY, 0x38, "homunculus weapon destroyed: enemy intel of the host"},
    {19, 0x79, "ACH_PROG_JUMPATK_STOP", ACHR_PLAYER, ACHF_ENEMY, 0x39, "progressor jump attack stopped: enemy intel of the host"},
    {20, 0x7B, "ACH_GRAP_COMBINATION", ACHR_RISKY, ACHF_ENEMY, ACH_NOKIND, "record set directly by the host's enemy code for the involved human (no stat kind)"},
    {21, 0x7D, "ACH_ASSIST_HOLD_HELP", ACHR_PLAYER, ACHF_ENEMY, 0x37, "the human who hits an enemy holding his partner (uEmIntel_vf22): the game credits only the seat-0 slot, the mod the last attacker"},
    {22, 0x7E, "ACH_BOTTLE_FIVE_KILL", ACHR_PLAYER, ACHF_LEDGER, ACH_NOKIND, "five kills with one bottle / firebomb: slot 0's bottle ledger (sPlayer+0x2DA4) on the thrower's PC (same function as SHOT_TWO_KILL)"},
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
 * human's kills.
 * pick = how the game's own code chooses the slot it bumps (read in the decompiled reporters, P6 2026-11-07, <lab>\re2q\ach_p6a/b/c.out, g2c_8b13.out):
 *   KILLER        Result_OnEnemyKilled_B: the killer's slot, any type (the gate undoes the puppet's bump: proven T3, stage 8b)
 *   KILLER_HUMAN  Result_OnEnemyKilled_A: the killer must be a type-0 human (player+0x7920 == 0, `piVar6[0x1e48]`): a puppet's kill bumps nothing; the action id (+0x1A54) and the weapon
 *                 in hand (FUN_006FB230) are read when the report runs (on the client: when the host's DEATH arrives)
 *   ARG           FUN_00603EA0(kind, player): the player named by the enemy code (0x2B: the attacker of the hit, FUN_00606680)
 *   TARGET        FUN_00603EA0(kind, 0): the enemy's current target +0x38D0 (0x38: uEm0100Intel_vf62), whoever destroyed the arm
 *   ARG_HUMAN     FUN_00986560: a type-0 human only (0x30): the host never bumps it for a puppet, so it can never be forwarded
 *   SEAT0_HITTER  uEmIntel_vf22: the seat-0 slot (FUN_006E6E50 = GameMan_FindSlotBySeat(0); the stock join gives slot 0 seat 0 on every PC) if it is the enemy's last attacker
 *                 (+0x3940 = DamageInfo+0xDC, FUN_00ADD880); the mod's call patch (ach_assist_slot) answers the last attacker's slot instead
 *   MIXED         uEm1100Intel_vf62 (a player of the intel's state) / vf63 (the enemy's target): not resolved */
enum { ACHP_KILLER = 1, ACHP_KILLER_HUMAN, ACHP_ARG, ACHP_TARGET, ACHP_ARG_HUMAN, ACHP_SEAT0_HITTER, ACHP_MIXED };
typedef struct { unsigned char kind; unsigned char fwd; unsigned wrapper; unsigned char pick; unsigned char enemy; } ach_kindw_t;
#define ACH_NKINDW 13
static const ach_kindw_t ach_kindw[ACH_NKINDW] = {
    {0x0F, 0, 0x409400, ACHP_KILLER, 0},          /* PROGRESSOR_KILL    */
    {0x14, 0, 0x4092E0, ACHP_KILLER, 0},          /* HOMUNCULUS_KILL    */
    {0x18, 0, 0x4092B0, ACHP_KILLER, 0},          /* GHOST_KILL         */
    {0x26, 0, 0x4092F0, ACHP_KILLER_HUMAN, 0},    /* KNIFE_WEAPON_KILL  */
    {0x28, 0, 0x409190, ACHP_KILLER_HUMAN, 0},    /* BLOW_ATK_KILL      */
    {0x2A, 0, 0x4092C0, ACHP_KILLER_HUMAN, 0},    /* GRAP_BLOW_KILL     */
    {0x2B, 1, 0x409170, ACHP_ARG, 1},             /* BACK_ATK_KILL      */
    {0x30, 0, 0x409340, ACHP_ARG_HUMAN, 1},       /* LIGHT_ATK: no puppet bump exists to forward (P6) */
    {0x33, 0, 0x409180, ACHP_KILLER_HUMAN, 0},    /* BAR_WEAPON_KILL    */
    {0x35, 0, 0x409610, ACHP_KILLER_HUMAN, 0},    /* STONE_KILL         */
    {0x37, 1, 0x409160, ACHP_SEAT0_HITTER, 1},    /* ASSIST_HOLD_HELP   */
    {0x38, 1, 0x4092D0, ACHP_TARGET, 1},          /* HOMU_WEAPON_DEST   */
    {0x39, 1, 0x4093F0, ACHP_MIXED, 1},           /* PROG_JUMPATK_STOP  */
};

/* Can the game's own code (with the mod's assist call patch) bump this kind for the partner's PUPPET on the host? Only such a bump can be undone and forwarded. */
static int ach_pick_names_puppet(int pick)
{
    return pick == ACHP_KILLER || pick == ACHP_ARG || pick == ACHP_TARGET || pick == ACHP_SEAT0_HITTER || pick == ACHP_MIXED;
}

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

/* ---- record forward (P6): GRAP_COMBINATION ("Now That's Teamwork!": Moira stuns, Claire follows up with a physical attack) ----
 * FUN_006067B0 sets record 0x7B only when the enemy's player +0x38D0 is the character 0x16 (Claire, FUN_00403380); the context guard swallows it on the host exactly when that player
 * is the partner's puppet, i.e. when the CLIENT's human is Claire. With forwarding on, the host sends the swallowed record (EV_ACH_REC) and the client sets it through the game's
 * own Ach_SetRecordState (the record bit and the Steam queue stay consistent). Only Steam-mapped context records travel (0x7B); the receiver must play the character the record names. */
static int ach_rec_fwd_ok(unsigned rec)
{
    return ach_ctx_record(rec) && ach_by_record(rec) != 0;
}
static int ach_rec_fwd_accept(unsigned rec, int my_char /* CP_CHAR_*: 0 Claire, 1 Moira, 2 Barry, 3 Natalia */)
{
    return ach_rec_fwd_ok(rec) && rec == 0x7B && my_char == 0;
}
enum { ACH_EV_REC_LEN = 4 };   /* EV_ACH_REC: u16 record, u16 seq */
static void ach_pack_rec(unsigned char *b, unsigned rec, unsigned seq)
{
    b[0] = (unsigned char)rec, b[1] = (unsigned char)(rec >> 8), b[2] = (unsigned char)seq, b[3] = (unsigned char)(seq >> 8);
}
static int ach_unpack_rec(const unsigned char *b, unsigned len, unsigned *rec, unsigned *seq)
{
    if (len < ACH_EV_REC_LEN) return 0;
    *rec = (unsigned)(b[0] | (b[1] << 8)), *seq = (unsigned)(b[2] | (b[3] << 8));
    return ach_rec_fwd_ok(*rec);
}

/* ---- ASSIST_HOLD_HELP ("Friend Indeed": save your captured partner ten times), P6 ----
 * uEmIntel_vf22 (0x6017C0): after an enemy that holds a player (+0x1A68 record type 5, +0x6F5C bit 12) gets a hit reaction (action type 8 / 9), the game bumps kind 0x37 for the player in
 * SEAT 0 (call 0x6018DB: FUN_006E6E50 = GameMan_FindSlotBySeat(0), then sPlayer_GetPlayer) when he is the enemy's last attacker (+0x3940, copied from DamageInfo+0xDC by FUN_00ADD880).
 * The stock join (Coop_ApplyJoinLeave) gives slot 0 seat 0 and slot 1 seat 1, on every PC of the mod: Moira / Natalia could never earn it, and a client human in slot 1 never gets a bump
 * on the host to forward. The mod's call patch answers the slot of the last attacker when he is one of the two humans of the session; the game then makes its own bump for that slot
 * (local human: credited; partner's puppet: undone by the gate and forwarded). Outside a session, or when the last attacker is not a human, the game's own answer stands. */
static int ach_assist_slot(int session, int seat0_slot, int hitter_slot /* slot of the last attacker if he is a human of the session, else -1 */)
{
    if (!session || hitter_slot < 0 || hitter_slot > 7) return seat0_slot;
    return hitter_slot;
}

/* ---- forwarding switch (P6): 0 off, 1 on, 2 auto. A client replays what arrives unless it is OFF, so the host's switch decides for the session and the client needs no setting.
 * G-ACH (2026-11-07): the default (auto) now SENDS on the host. The forward path itself is proven on hardware with console-made causes (stage 8b s8b1: S3, T1, T2: one replay per
 * cause, dedupe, no overcount), a host that forwards was run 26 min with enemies, grabs and kills (G1: 0 sends, 0 faults), and a host that sends nothing changes nothing; the real
 * enemy-side causes (assist, stealth, Revenant arm, jump stop, Teamwork) could not be produced by the lab tools, so the acceptance runs (real play) are their live proof.
 * REV2_ACHFWD=0 / `net ach fwd 0` stops it. The ASSIST_HOLD_HELP call patch stays opt-in (REV2_ACHFWD=1, REV2_ACHASSIST=1, `net ach assist 1`): no live call reached it (G1: 0). */
enum { ACH_FWD_OFF = 0, ACH_FWD_ON = 1, ACH_FWD_AUTO = 2 };
static int ach_fwd_send(int mode) { return mode != ACH_FWD_OFF; }
static int ach_fwd_accept(int mode) { return mode != ACH_FWD_OFF; }
static int ach_assist_wanted(int mode, int env_assist) { return mode == ACH_FWD_ON || env_assist; }   /* the call patch: only on an explicit request */

/* ---- the gate: what happens to the stat bump (kind, n) the game makes for `slot` ---- */
typedef struct {
    int session;             /* a co-op session is running (otherwise the game is untouched) */
    int gate_on;             /* `net ach gate` (default 1) */
    int is_host;
    int fwd_on;              /* the host sends: ach_fwd_send(`net ach fwd` / REV2_ACHFWD) */
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

/* ---- the local human's melee-class actions (G-ACH, 2026-11-07) ----
 * Evaluator A of the kill report (Result_OnEnemyKilled_A 0x9808E0) reads the killer's action id (+0x1A54) when the report runs: 0x2B / 0x2D / 0x2E = a melee blow
 * (kind 0x28), 0x2F = the follow-up (kind 0x2A). On a client the report runs when the host's DEATH arrives (F6), so the mod keeps the last such action of its
 * human with its start and end time; action 28 is the knife lunge (s6f2: RT without aim, knife in the hand), which evaluator A counts by the weapon instead. */
static int ach_is_blow_act(unsigned act) { return act == 0x2B || act == 0x2D || act == 0x2E; }
static int ach_is_melee_act(unsigned act) { return ach_is_blow_act(act) || act == 0x2F || act == 28; }
static int ach_is_counted_act(unsigned act) { return ach_is_blow_act(act) || act == 0x2F; }   /* the action ids evaluator A counts (kinds 0x28 / 0x2A) */
/* actions that are no attack: idle 2, walk / run 3 and the on-foot family 4-6, the aim stance 13 / 17 / 21 (G0 / G1 traces, stage 6c learning log) */
static int ach_is_neutral_act(unsigned act) { return (act >= 2 && act <= 6) || act == 13 || act == 17 || act == 21; }

/* ---- F6 (G-ACH, 2026-11-07): the client's kill report runs when the host's DEATH arrives, one round trip after the killing blow. Measured on the N100 pair (run G1, knife
 * slash action 28, `lab\exp\gach_f6.py`): the report ran 57 / 74 ms after the client's killing HIT, and the slash that killed had only 140 ms of its 690 ms left at the HIT
 * (66 ms after the report). Evaluator A counts a melee blow (0x2B / 0x2D / 0x2E -> kind 0x28) or the follow-up (0x2F -> 0x2A) only if the killer still IS in that action when the
 * report runs; at an internet round trip of 100-300 ms a late-landing blow is read as idle: a lost kill credit (never an extra one). The knife (0x26), bar (0x33) and stone
 * (0x35) kinds count by the weapon in the hand, which a round trip does not change. The fix: during the DEATH replay of the client's own kill, evaluator A sees the last
 * counted blow of the local human when that blow ended at most `window` ms ago and nothing but neutral actions came after it (a new attack, the knife lunge included,
 * means the killing HIT may be that one: no substitution). Returns the action id the report must see. */
typedef struct {
    unsigned blow;       /* the last counted blow (0 = none) */
    unsigned end_ms;     /* its end tick (0 = still running) */
    int clean;           /* only neutral actions since it ended */
} ach_blowhist_t;
static void ach_blowhist_on_change(ach_blowhist_t *h, unsigned prev, unsigned next, unsigned now)
{
    if (ach_is_counted_act(next)) {
        h->blow = next, h->end_ms = 0, h->clean = 1;
        return;
    }
    if (h->blow && prev == h->blow && !h->end_ms) h->end_ms = now | 1u;   /* the blow ended (|1: 0 means running) */
    if (h->blow && h->end_ms && !ach_is_neutral_act(next)) h->clean = 0;   /* another action after it: a damage reaction, a lunge, a shot ... */
}
static unsigned ach_f6_action(const ach_blowhist_t *h, unsigned now_act, unsigned now, unsigned window_ms)
{
    if (ach_is_counted_act(now_act) || !h->blow || !h->end_ms || !h->clean) return now_act;
    if ((unsigned)(now - h->end_ms) > window_ms) return now_act;
    return h->blow;
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
