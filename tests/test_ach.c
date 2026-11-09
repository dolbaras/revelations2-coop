/*
 * Stage 8b offline tests of native/ach_logic.h: the table of the 43 Steam achievements (against the game's own record map, copied from the image), the gate decision, the dedupe window,
 * the wire format of the forwarded event, the table in docs/achievements.md, and a model of two PCs (host and client) that applies the design to scripted gameplay and checks that
 * every PC credits exactly its own human, whoever runs the cause.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/ach_logic.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* the game's record map as read from the memory image of build 3658995 (Ach_RecordMapRecordIds 0x1461E64, Ach_RecordMapSteamIdx 0x1462124): index -> {record, steam index} */
static const unsigned short game_rec[43] = {0x02, 0x12, 0x17, 0x35, 0x3A, 0x49, 0x4F, 0x54, 0x64, 0x65, 0x66, 0x67, 0x6A, 0x6C, 0x6E, 0x70, 0x72, 0x78, 0x79, 0x7B, 0x7D, 0x7E,
                                            0xB4, 0xB5, 0xB6, 0x03, 0x13, 0x18, 0x36, 0x4A, 0x57, 0x04, 0x14, 0x19, 0x37, 0x4B, 0x4C, 0x05, 0x15, 0x1A, 0x38, 0x4D, 0x4E};
static const unsigned char game_steam[43] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
                                             37, 38, 39, 40, 41, 42, 43};
/* the names of the game's name table (0x1462068), index = Steam index */
static const char *const game_name[44] = {
    "ACH_ALL_ACHIEVEMENT_COMP", "ACH_ALL_EP1_CLEAR", "ACH_SURVIVAL_ALL_EP1_CLEAR", "ACH_INVISIBLE_ALL_EP1_CLEAR", "ACH_CHALLENGE_MEDAL_EP1_COMP", "ACH_CAMPAINF_PT_MEDIUM",
    "ACH_FLAME_NOT_USED", "ACH_PROGRESSOR_KILL", "ACH_HOMUNCULUS_KILL", "ACH_KNIFE_WEAPON_KILL", "ACH_BLOW_ATK_KILL", "ACH_GRAP_BLOW_KILL", "ACH_BACK_ATK_KILL", "ACH_LIGHT_ATK",
    "ACH_BAR_WEAPON_KILL", "ACH_STONE_KILL", "ACH_KNIFE_CLEAR", "ACH_SHOT_TWO_KILL", "ACH_HOMU_WEAPON_DEST", "ACH_PROG_JUMPATK_STOP", "ACH_GRAP_COMBINATION", "ACH_ASSIST_HOLD_HELP",
    "ACH_BOTTLE_FIVE_KILL", "ACH_SYS_RAID_MISSION_CLEAR", "ACH_SYS_RAID_USE_JUDGMENT", "ACH_SYS_RAID_DUNGEON_CLEAR", "ACH_ALL_EP2_CLEAR", "ACH_SURVIVAL_ALL_EP2_CLEAR",
    "ACH_INVISIBLE_ALL_EP2_CLEAR", "ACH_CHALLENGE_MEDAL_EP2_COMP", "ACH_PEDRO_PAST_KILL", "ACH_GHOST_KILL", "ACH_ALL_EP3_CLEAR", "ACH_SURVIVAL_ALL_EP3_CLEAR",
    "ACH_INVISIBLE_ALL_EP3_CLEAR", "ACH_CHALLENGE_MEDAL_EP3_COMP", "ACH_MOIRA_NEIL_KILLED", "ACH_FLOOD_GATE_ITEM_GET", "ACH_ALL_EP4_CLEAR", "ACH_SURVIVAL_ALL_EP4_CLEAR",
    "ACH_INVISIBLE_ALL_EP4_CLEAR", "ACH_CHALLENGE_MEDAL_EP4_COMP", "ACH_PAST_GHOST_KILL", "ACH_FLOOD_GATE_USED"};

static void test_table(void)
{
    int rules[5] = {0}, seen_steam[44] = {0};
    for (int i = 0; i < ACH_N; i++) {
        const ach_entry_t *e = &ach_table[i];
        CHECK(e->rec == game_rec[i] || ach_by_record(game_rec[i]) != 0);   /* every record of the game's map is in the table */
        CHECK(e->steam >= 1 && e->steam <= 43);
        CHECK(!seen_steam[e->steam]);
        seen_steam[e->steam] = 1;
        CHECK(strcmp(e->name, game_name[e->steam]) == 0);
        CHECK(e->rule >= ACHR_PLAYER && e->rule <= ACHR_NA);
        rules[e->rule]++;
        CHECK(ach_by_record(e->rec) == e);                                  /* record ids are unique */
        if (e->kind != ACH_NOKIND) CHECK(e->kind < 0x51 && ach_by_kind(e->kind) == e);   /* a stat kind feeds one record */
        if (e->rule == ACHR_NA) CHECK(e->feeder == ACHF_NONE);
        if (e->feeder == ACHF_ENEMY && e->kind != ACH_NOKIND) {
            const ach_kindw_t *k = ach_kind_find(e->kind);
            CHECK(k && (k->fwd || !ach_pick_names_puppet(k->pick)));        /* every enemy-side kind the host can bump for the puppet is forwardable ... */
        }
        if (e->feeder == ACHF_KILLREP && e->kind != ACH_NOKIND) {
            const ach_kindw_t *k = ach_kind_find(e->kind);
            CHECK(k && !k->fwd);                                            /* ... and no kill-report kind is (the client runs the kill report itself) */
            CHECK(k && !k->enemy && (k->pick == ACHP_KILLER || k->pick == ACHP_KILLER_HUMAN));
        }
        if (e->feeder == ACHF_ENEMY && e->kind != ACH_NOKIND) {
            const ach_kindw_t *k = ach_kind_find(e->kind);
            CHECK(k && k->enemy);
        }
        if (e->feeder == ACHF_LEDGER) CHECK(e->kind == ACH_NOKIND && e->rule == ACHR_PLAYER);   /* P6: SHOT_TWO_KILL / BOTTLE_FIVE_KILL are campaign achievements */
    }
    for (int s = 1; s <= 43; s++) CHECK(seen_steam[s]);
    /* the game's own map has the same pairs, in the same order */
    for (int i = 0; i < 43; i++) {
        const ach_entry_t *e = ach_by_record(game_rec[i]);
        CHECK(e && e->steam == game_steam[i]);
    }
    CHECK(rules[ACHR_PLAYER] == 20 && rules[ACHR_SHARED] == 19 && rules[ACHR_RISKY] == 1 && rules[ACHR_NA] == 3);
    CHECK(ach_by_record(0x72)->feeder == ACHF_LEDGER && ach_by_record(0x7E)->feeder == ACHF_LEDGER);
    CHECK(ach_by_record(0x99) == 0 && ach_by_kind(0x01) == 0 && ach_by_kind(ACH_NOKIND) == 0);
    /* the forwardable kinds (P6: 0x30 is not one: FUN_00986560 bumps it only for a type-0 human, so the host never makes a puppet's bump to forward) */
    int nfwd = 0, nenemy = 0;
    for (int i = 0; i < ACH_NKINDW; i++) {
        const ach_kindw_t *k = &ach_kindw[i];
        nfwd += k->fwd, nenemy += k->enemy;
        CHECK(k->pick >= ACHP_KILLER && k->pick <= ACHP_MIXED);
        if (k->fwd) CHECK(k->enemy && ach_pick_names_puppet(k->pick));   /* a forwarded kind must be one the host's own code can bump for the puppet */
        if (k->enemy && ach_pick_names_puppet(k->pick)) CHECK(k->fwd);    /* ... and every such enemy-side kind is forwarded */
        CHECK(ach_by_kind(k->kind) != 0);                                 /* every wrapped kind feeds a Steam record */
    }
    CHECK(nfwd == 4 && nenemy == 5);
    CHECK(ach_fwd_find(0x2B) && !ach_fwd_find(0x30) && ach_fwd_find(0x37) && ach_fwd_find(0x38) && ach_fwd_find(0x39));
    CHECK(ach_kind_find(0x30) && ach_kind_find(0x30)->pick == ACHP_ARG_HUMAN && !ach_pick_names_puppet(ACHP_ARG_HUMAN));
    CHECK(!ach_pick_names_puppet(ACHP_KILLER_HUMAN) && ach_pick_names_puppet(ACHP_SEAT0_HITTER));
    CHECK(!ach_fwd_find(0x26) && !ach_fwd_find(0x0F) && !ach_fwd_find(0x1A) && !ach_fwd_find(0x50));
    CHECK(ach_kind_find(0x26) && ach_kind_find(0x26)->wrapper == 0x4092F0);
    CHECK(ach_kind_find(0x37)->wrapper == 0x409160 && ach_kind_find(0x37)->pick == ACHP_SEAT0_HITTER);   /* the wrapper uEmIntel_vf22 calls after FUN_00603EA0(0x37, ...) (0x60190A) */
}

static void test_gate(void)
{
    ach_cfg_t host = {1, 1, 1, 1}, client = {1, 1, 0, 1}, solo = {0, 1, 1, 1}, off = {1, 0, 1, 1}, hostnofwd = {1, 1, 1, 0};
    /* outside a session or with the gate off the game is untouched */
    CHECK(ach_gate(&solo, 0, 1, 0x2B) == ACH_ALLOW);
    CHECK(ach_gate(&off, 0, 1, 0x2B) == ACH_ALLOW);
    /* the local human is always credited */
    CHECK(ach_gate(&host, 1, 0, 0x26) == ACH_ALLOW && ach_gate(&client, 1, 0, 0x2B) == ACH_ALLOW);
    /* the partner's kill-report kinds are undone, never forwarded (the partner's PC credits itself) */
    CHECK(ach_gate(&host, 0, 1, 0x26) == ACH_UNDO && ach_gate(&host, 0, 1, 0x0F) == ACH_UNDO);
    CHECK(ach_gate(&client, 0, 1, 0x26) == ACH_UNDO);
    /* the host forwards the partner's enemy-side kinds ... */
    CHECK(ach_gate(&host, 0, 1, 0x2B) == ACH_UNDO_FORWARD && ach_gate(&host, 0, 1, 0x39) == ACH_UNDO_FORWARD);
    /* ... only with forwarding on, only for the partner's slot, never from a client */
    CHECK(ach_gate(&hostnofwd, 0, 1, 0x2B) == ACH_UNDO);
    CHECK(ach_gate(&host, 0, 0, 0x2B) == ACH_UNDO);
    CHECK(ach_gate(&client, 0, 1, 0x2B) == ACH_UNDO);
    /* a slot without a local human that is not the partner (an empty slot) is undone as well */
    CHECK(ach_gate(&host, 0, 0, 0x26) == ACH_UNDO);
}

static void test_ctx(void)
{
    CHECK(ach_ctx_record(0x7B) && ach_ctx_record(0x7A) && ach_ctx_record(0x7C) && ach_ctx_record(0x74));
    CHECK(!ach_ctx_record(0x67) && !ach_ctx_record(0x64) && !ach_ctx_record(0x02) && !ach_ctx_record(0x7D) && !ach_ctx_record(0x78));
    /* the only Steam-mapped record among them is GRAP_COMBINATION */
    int mapped = 0;
    for (unsigned r = 0; r < 0x200; r++)
        if (ach_ctx_record(r) && ach_by_record(r)) mapped++;
    CHECK(mapped == 1 && ach_by_record(0x7B) && ach_by_record(0x7B)->rule == ACHR_RISKY);
}

/* P6: the record forward of GRAP_COMBINATION (EV_ACH_REC) */
static void test_rec_fwd(void)
{
    int n = 0;
    for (unsigned r = 0; r < 0x200; r++) n += ach_rec_fwd_ok(r);
    CHECK(n == 1 && ach_rec_fwd_ok(0x7B));                                 /* only the Steam-mapped context record travels */
    CHECK(!ach_rec_fwd_ok(0x7A) && !ach_rec_fwd_ok(0x7C) && !ach_rec_fwd_ok(0x74) && !ach_rec_fwd_ok(0x02) && !ach_rec_fwd_ok(0x7D));
    CHECK(ach_rec_fwd_accept(0x7B, 0));                                    /* the record's own condition names Claire (character 0x16) */
    CHECK(!ach_rec_fwd_accept(0x7B, 1) && !ach_rec_fwd_accept(0x7B, 2) && !ach_rec_fwd_accept(0x7B, 3) && !ach_rec_fwd_accept(0x7B, -1));
    CHECK(!ach_rec_fwd_accept(0x7C, 1));
    unsigned char b[ACH_EV_REC_LEN];
    unsigned rec = 0, seq = 0;
    ach_pack_rec(b, 0x7B, 0xBEEF);
    CHECK(ach_unpack_rec(b, ACH_EV_REC_LEN, &rec, &seq) && rec == 0x7B && seq == 0xBEEF);
    CHECK(!ach_unpack_rec(b, ACH_EV_REC_LEN - 1, &rec, &seq));
    ach_pack_rec(b, 0x02, 1);                                              /* an episode clear is never forwarded (each PC reaches it itself) */
    CHECK(!ach_unpack_rec(b, ACH_EV_REC_LEN, &rec, &seq));
    ach_pack_rec(b, 0x17B, 1);                                             /* the high byte counts */
    CHECK(!ach_unpack_rec(b, ACH_EV_REC_LEN, &rec, &seq));
    /* the record kinds of the event range of ach.inc stay apart from the bump */
    CHECK(ACH_EV_REC_LEN != ACH_EV_BUMP_LEN);
}

/* P6: ASSIST_HOLD_HELP slot decision and the forwarding switch */
static void test_assist_and_fwd_mode(void)
{
    CHECK(ach_assist_slot(0, 0, 1) == 0);    /* no session: the game's seat-0 answer */
    CHECK(ach_assist_slot(1, 0, -1) == 0);   /* the last attacker is no human of the session (a bomb, an enemy, stale): the game's answer */
    CHECK(ach_assist_slot(1, 0, 0) == 0);    /* slot 0 (seat 0) hit: as the game */
    CHECK(ach_assist_slot(1, 0, 1) == 1);    /* slot 1 (Moira / Natalia) hit: credited to slot 1 (the game would compare with slot 0 and bump nothing) */
    CHECK(ach_assist_slot(1, 1, 0) == 0);
    CHECK(ach_assist_slot(1, 0, 9) == 0);    /* nonsense slot: the game's answer */
    /* G-ACH: the default (auto) sends on the host; only an explicit off stops it */
    CHECK(ach_fwd_send(ACH_FWD_ON) && ach_fwd_send(ACH_FWD_AUTO) && !ach_fwd_send(ACH_FWD_OFF));
    CHECK(ach_fwd_accept(ACH_FWD_ON) && ach_fwd_accept(ACH_FWD_AUTO) && !ach_fwd_accept(ACH_FWD_OFF));
    /* the assist call patch only on an explicit request (no live call reached it in G1) */
    CHECK(!ach_assist_wanted(ACH_FWD_AUTO, 0) && !ach_assist_wanted(ACH_FWD_OFF, 0));
    CHECK(ach_assist_wanted(ACH_FWD_ON, 0) && ach_assist_wanted(ACH_FWD_AUTO, 1) && ach_assist_wanted(ACH_FWD_OFF, 1));
}

static void test_block_default(void)
{
    CHECK(ach_block_default(0, 0, 0) == 0);      /* a player's start: no console, no switch: real unlocks */
    CHECK(ach_block_default(0, 0, 1) == 1);      /* any lab run (REV2_CONSOLE set): blocked even if a script forgot the switch */
    CHECK(ach_block_default(1, '1', 0) == 1);    /* explicit block */
    CHECK(ach_block_default(1, '1', 1) == 1);
    CHECK(ach_block_default(1, '0', 1) == 0);    /* the manual switch for a deliberate real-unlock check in a console run */
    CHECK(ach_block_default(1, '0', 0) == 0);
    CHECK(ach_block_default(1, 'x', 1) == 0);    /* anything but "1" is not a block (the lab scripts write "1") */
}

static void test_dedupe_and_wire(void)
{
    CHECK(ach_dedupe_ok(5000, 0, 1500));            /* never bumped locally */
    CHECK(!ach_dedupe_ok(5000, 4000, 1500));        /* 1000 ms ago: the replica ran the cause itself */
    CHECK(ach_dedupe_ok(5000, 3500, 1500));         /* exactly the window: a new event */
    CHECK(ach_dedupe_ok(5000, 1000, 1500));
    CHECK(!ach_dedupe_ok(100, 4294967000u, 1500));  /* tick counter wrap */
    CHECK(ach_dedupe_ok(0, 0, 1500));
    unsigned char b[ACH_EV_BUMP_LEN];
    unsigned kind, n, seq;
    ach_pack_bump(b, 0x2B, 1, 0x1234, 0xDEADBEEF);
    CHECK(ach_unpack_bump(b, ACH_EV_BUMP_LEN, &kind, &n, &seq) && kind == 0x2B && n == 1 && seq == 0x1234);
    CHECK(b[4] == 0xEF && b[7] == 0xDE);
    CHECK(!ach_unpack_bump(b, ACH_EV_BUMP_LEN - 1, &kind, &n, &seq));   /* short */
    ach_pack_bump(b, 0x51, 1, 0, 0);
    CHECK(!ach_unpack_bump(b, ACH_EV_BUMP_LEN, &kind, &n, &seq));       /* kind out of range */
    ach_pack_bump(b, 0x2B, 0, 0, 0);
    CHECK(!ach_unpack_bump(b, ACH_EV_BUMP_LEN, &kind, &n, &seq));       /* n = 0 */
    ach_pack_bump(b, 0x2B, 200, 0, 0);
    CHECK(!ach_unpack_bump(b, ACH_EV_BUMP_LEN, &kind, &n, &seq));       /* n absurd */
}

/* ---- a model of two PCs. Each PC has a profile (global totals by kind), a record flag per kind and its own human slot. The "game" bumps for the acting slot exactly as the hooks see it:
 *      total += n, then the per-slot call decides (gate). The record fires when total >= goal. Events: a human acts on PC `where` (where the cause is visible); the host additionally
 *      sees the causes of the client's human for enemy-side kinds. ---- */
typedef struct {
    int host, session, fwd;
    unsigned total[0x51];
    unsigned last_mine[0x51];     /* ms of the last local bump */
    int done[0x51];               /* record fired for the kind */
    unsigned goal[0x51];
    int my_slot, rem_slot;
    unsigned n_undone, n_fwd_tx, n_fwd_dup;
    int my_char;                  /* CP_CHAR_* of the local human */
    int rec7b;                    /* GRAP_COMBINATION set in this save */
    unsigned n_rec_tx, n_rec_rx;
} pc_t;

typedef struct { unsigned kind, n, seq, tick; } wire_t;
static wire_t wire[64];
static int nwire;

static void pc_init(pc_t *p, int host, int my_slot)
{
    memset(p, 0, sizeof *p);
    p->host = host, p->session = 1, p->fwd = ACH_FWD_ON, p->my_slot = my_slot, p->rem_slot = 1 - my_slot;
    p->my_char = my_slot;   /* Claire / Moira by default (Barry / Natalia have the same slots) */
    for (int k = 0; k < 0x51; k++) p->goal[k] = 3;
}

/* the game bumps the kind for `slot` on this PC at time `now` (what h_a_statadd + h_a_statslot see) */
static void pc_bump(pc_t *p, int slot, unsigned kind, unsigned n, unsigned now)
{
    ach_cfg_t c = {p->session, 1, p->host, ach_fwd_send(p->fwd)};
    unsigned before = p->total[kind];
    p->total[kind] += n;
    int v = ach_gate(&c, slot == p->my_slot, slot == p->rem_slot, kind);
    if (v == ACH_ALLOW) p->last_mine[kind] = now | 1;
    else {
        p->total[kind] = before;
        p->n_undone++;
        if (v == ACH_UNDO_FORWARD) {
            wire[nwire].kind = kind, wire[nwire].n = n, wire[nwire].seq = (unsigned)nwire, wire[nwire].tick = now, nwire++;
            p->n_fwd_tx++;
        }
    }
    if (!p->done[kind] && p->total[kind] >= p->goal[kind]) p->done[kind] = 1;   /* the record wrapper after the bump */
}

static void pc_receive(pc_t *p, const wire_t *w, unsigned now)
{
    if (!p->session || p->host || !ach_fwd_accept(p->fwd) || !ach_fwd_find(w->kind)) return;
    if (!ach_dedupe_ok(now, p->last_mine[w->kind], 1500)) {
        p->n_fwd_dup++;
        return;
    }
    pc_bump(p, p->my_slot, w->kind, w->n, now);
}

static void deliver(pc_t *client, unsigned now)
{
    for (int i = 0; i < nwire; i++) pc_receive(client, &wire[i], now);
    nwire = 0;
}

static void test_two_pcs(void)
{
    pc_t H, C;
    /* host human = slot 1 (Natalia), client human = slot 0 (Barry): on the host the client's human is the puppet in slot 0, on the client the host's human is the puppet in slot 1 */
    pc_init(&H, 1, 1);
    pc_init(&C, 0, 0);
    nwire = 0;
    /* A. kill-report kinds: the client's PC runs the report for its own kills, the host's PC also runs it for the puppet's kill (undone) */
    for (int i = 0; i < 2; i++) {
        pc_bump(&C, 0, 0x26, 1, 1000 + i);   /* Barry kills with the knife: his PC credits him */
        pc_bump(&H, 0, 0x26, 1, 1000 + i);   /* the host sees the puppet's kill: undone */
    }
    CHECK(C.total[0x26] == 2 && H.total[0x26] == 0);
    CHECK(!C.done[0x26] && !H.done[0x26]);
    /* at goal-1 the partner's kill must not fire the record (the hole of the rollback-after design) */
    pc_bump(&H, 0, 0x26, 1, 2000);
    pc_bump(&H, 0, 0x26, 1, 2001);
    pc_bump(&H, 0, 0x26, 1, 2002);
    CHECK(H.total[0x26] == 0 && !H.done[0x26]);
    pc_bump(&C, 0, 0x26, 1, 2000);   /* his third: the record fires on his PC only */
    CHECK(C.done[0x26] && !H.done[0x26]);
    /* B. the host's own kills credit the host only */
    for (int i = 0; i < 3; i++) {
        pc_bump(&H, 1, 0x0F, 1, 3000 + i);
        pc_bump(&C, 1, 0x0F, 1, 3000 + i);   /* the client's PC sees the host human's kill as a replica death: undone */
    }
    CHECK(H.total[0x0F] == 3 && H.done[0x0F] && C.total[0x0F] == 0 && !C.done[0x0F]);
    /* C. enemy-side causes: only the host sees them. The host bumps for the client's puppet -> forwarded -> credited once on the client */
    for (int i = 0; i < 3; i++) {
        pc_bump(&H, 0, 0x2B, 1, 10000 + 5000u * i);
        deliver(&C, 10000 + 5000u * i + 30);
    }
    CHECK(H.total[0x2B] == 0 && !H.done[0x2B]);
    CHECK(C.total[0x2B] == 3 && C.done[0x2B]);
    CHECK(H.n_fwd_tx == 3 && C.n_fwd_dup == 0);
    /* D. the cause runs on both PCs (the replica's own code bumps for the client's human AND the host forwards): counted once */
    pc_bump(&C, 0, 0x38, 1, 40000);              /* the client's replica ran it */
    pc_bump(&H, 0, 0x38, 1, 40010);              /* the host's enemy ran it for the puppet: forwarded */
    deliver(&C, 40050);
    CHECK(C.total[0x38] == 1 && C.n_fwd_dup == 1);
    /* D2 (P6). LIGHT_ATK (0x30) is not forwarded: the host's FUN_00986560 needs a type-0 human, a stray puppet bump is undone and nothing is sent */
    unsigned tx_d2 = H.n_fwd_tx;
    pc_bump(&H, 0, 0x30, 1, 41000);
    CHECK(H.total[0x30] == 0 && H.n_fwd_tx == tx_d2 && nwire == 0);
    /* E. the host's human is involved: the host credits itself, nothing is forwarded; the client's PC (replicas without AI) sees nothing, and a stray replica bump is undone */
    unsigned tx0 = H.n_fwd_tx;
    pc_bump(&H, 1, 0x37, 1, 50000);
    pc_bump(&C, 1, 0x37, 1, 50000);
    CHECK(H.total[0x37] == 1 && C.total[0x37] == 0 && H.n_fwd_tx == tx0 && nwire == 0);
    /* F. nothing is credited to a PC for the whole script that its human did not do */
    CHECK(H.total[0x26] == 0 && H.total[0x2B] == 0 && H.total[0x30] == 0);
    CHECK(C.total[0x0F] == 0 && C.total[0x37] == 0);
    /* G. forwarding off on the client: the event is ignored there (and the host would not send it: both switches are needed) */
    C.fwd = 0;
    pc_bump(&H, 0, 0x39, 1, 60000);
    deliver(&C, 60040);
    CHECK(C.total[0x39] == 0);
    /* H. no session: the game is untouched (a solo player with the DLL loaded) */
    pc_t S;
    pc_init(&S, 1, 0);
    S.session = 0;
    pc_bump(&S, 1, 0x26, 1, 1);
    CHECK(S.total[0x26] == 1 && S.n_undone == 0);
}

/* P6 model of uEmIntel_vf22 on the HOST (only the host's enemies have AI): an enemy holding `held` is hit by `hitter`; the game compares its last attacker with the player of the slot that
 * FUN_006E6E50 answers (seat 0 = slot 0 on every PC of the mod), patched or not, and bumps kind 0x37 for that player when they are equal */
static int g_seat0_local;   /* 0: seat 0 = slot 0 on every PC (stock join, the expected case); 1: seat 0 = the local human's slot (the other reading, not excluded live) */
static void host_assist(pc_t *H, int hitter, int patched, unsigned now)
{
    int seat0 = g_seat0_local ? H->my_slot : 0;
    int cmp = patched ? ach_assist_slot(H->session, seat0, hitter) : seat0;
    if (cmp == hitter) pc_bump(H, cmp, 0x37, 1, now);
}

/* whichever slot holds seat 0, the patched game credits exactly the hitter on his own PC */
static void test_assist_seat_variants(void)
{
    for (g_seat0_local = 0; g_seat0_local < 2; g_seat0_local++)
        for (int host_slot = 0; host_slot < 2; host_slot++) {
            pc_t H, C;
            pc_init(&H, 1, host_slot);
            pc_init(&C, 0, 1 - host_slot);
            nwire = 0;
            host_assist(&H, host_slot, 1, 1000);       /* the host's human hits the holder */
            host_assist(&H, 1 - host_slot, 1, 6000);   /* the client's human hits the holder */
            deliver(&C, 6040);
            CHECK(H.total[0x37] == 1 && C.total[0x37] == 1);
            /* unpatched with seat 0 = the local human: the client's human is never credited (the second reading of F1) */
            pc_init(&H, 1, host_slot);
            pc_init(&C, 0, 1 - host_slot);
            nwire = 0;
            host_assist(&H, 1 - host_slot, 0, 6000);
            deliver(&C, 6040);
            if (g_seat0_local) CHECK(C.total[0x37] == 0);
            else CHECK(C.total[0x37] == (host_slot == 1 ? 1u : 0u));   /* seat 0 = slot 0: only a client in slot 0 */
        }
    g_seat0_local = 0;
}

static void test_assist_model(void)
{
    /* roles A: host = Natalia (slot 1), client = Barry (slot 0) */
    for (int patched = 0; patched < 2; patched++) {
        pc_t H, C;
        pc_init(&H, 1, 1);
        pc_init(&C, 0, 0);
        nwire = 0;
        for (int i = 0; i < 3; i++) {
            host_assist(&H, 0, patched, 1000 + 4000u * i);   /* Barry (the client's human) frees the held Natalia: seat 0 = his puppet, the game itself names him */
            deliver(&C, 1000 + 4000u * i + 40);
        }
        CHECK(C.total[0x37] == 3 && C.done[0x37] && H.total[0x37] == 0);   /* forwarded and credited to Barry on his PC, with or without the patch */
        for (int i = 0; i < 3; i++) host_assist(&H, 1, patched, 20000 + 4000u * i);   /* Natalia (the host's human) frees the held Barry */
        if (!patched) CHECK(H.total[0x37] == 0 && !H.done[0x37]);             /* the gap: the stock code compares with seat 0 (Barry's puppet) and bumps nothing */
        else CHECK(H.total[0x37] == 3 && H.done[0x37] && nwire == 0);          /* patched: her own bump, credited on her PC, nothing forwarded */
        CHECK(C.total[0x37] == 3);                                             /* never credited to Barry for her help */
    }
    /* roles B: host = Barry (slot 0), client = Natalia (slot 1) */
    for (int patched = 0; patched < 2; patched++) {
        pc_t H, C;
        pc_init(&H, 1, 0);
        pc_init(&C, 0, 1);
        nwire = 0;
        for (int i = 0; i < 3; i++) host_assist(&H, 0, patched, 1000 + 4000u * i);   /* Barry (host) frees Natalia */
        CHECK(H.total[0x37] == 3 && H.done[0x37] && nwire == 0);
        for (int i = 0; i < 3; i++) {
            host_assist(&H, 1, patched, 20000 + 4000u * i);                    /* Natalia (client) frees Barry: the host's enemy reacts */
            deliver(&C, 20000 + 4000u * i + 40);
        }
        if (!patched) CHECK(C.total[0x37] == 0 && !C.done[0x37]);             /* the gap: no bump for slot 1 on the host, nothing to forward */
        else CHECK(C.total[0x37] == 3 && C.done[0x37]);                       /* patched: the host bumps for her puppet, undoes it and forwards it */
        CHECK(H.total[0x37] == 3);                                             /* the host never gets her help */
    }
    /* a hitter that is no human of the session (an exploding bottle object, a stale +0x3940): the game's own answer, nothing changes */
    pc_t H;
    pc_init(&H, 1, 1);
    nwire = 0;
    int cmp = ach_assist_slot(1, 0, -1);
    CHECK(cmp == 0);
}

/* P6 model of FUN_006067B0 + the context guard + EV_ACH_REC: the record 0x7B is set by the host's enemy code for the player at +0x38D0 when that player is Claire */
static void host_grap_combo(pc_t *H, pc_t *C, int claire_slot)
{
    int target_is_partner = claire_slot == H->rem_slot;
    if (!target_is_partner) {
        H->rec7b = 1;   /* the host's own Claire: the record stands on the host */
        return;
    }
    if (ach_fwd_send(H->fwd) && ach_rec_fwd_ok(0x7B)) {   /* swallowed by the context guard, forwarded */
        unsigned char b[ACH_EV_REC_LEN];
        unsigned rec, seq;
        ach_pack_rec(b, 0x7B, ++H->n_rec_tx);
        if (ach_unpack_rec(b, ACH_EV_REC_LEN, &rec, &seq) && !C->host && ach_fwd_accept(C->fwd) && ach_rec_fwd_accept(rec, C->my_char)) C->rec7b = 1, C->n_rec_rx++;
    }
}

static void test_recfwd_model(void)
{
    pc_t H, C;
    /* host = Moira, client = Claire */
    pc_init(&H, 1, 1);
    pc_init(&C, 0, 0);
    host_grap_combo(&H, &C, 0);
    CHECK(!H.rec7b && C.rec7b && C.n_rec_rx == 1);
    host_grap_combo(&H, &C, 0);   /* again: idempotent (the record bit is already set) */
    CHECK(C.rec7b && !H.rec7b);
    /* forwarding switched off on the host: the client cannot earn it, the host is still never credited */
    pc_init(&H, 1, 1);
    pc_init(&C, 0, 0);
    H.fwd = ACH_FWD_OFF;
    host_grap_combo(&H, &C, 0);
    CHECK(!H.rec7b && !C.rec7b);
    /* G-ACH: the default modes on both PCs (auto / auto) carry it */
    pc_init(&H, 1, 1);
    pc_init(&C, 0, 0);
    H.fwd = C.fwd = ACH_FWD_AUTO;
    host_grap_combo(&H, &C, 0);
    CHECK(!H.rec7b && C.rec7b && C.n_rec_rx == 1);
    /* host = Claire, client = Moira: the host's Claire earns it on her PC, nothing travels */
    pc_init(&H, 1, 0);
    pc_init(&C, 0, 1);
    host_grap_combo(&H, &C, 0);
    CHECK(H.rec7b && !C.rec7b && H.n_rec_tx == 0);
    /* a client that is not Claire refuses a 0x7B (a wrong character in a hand-made event) */
    pc_init(&H, 1, 1);
    pc_init(&C, 0, 0);
    C.my_char = 2;   /* Barry */
    host_grap_combo(&H, &C, 0);
    CHECK(!C.rec7b);
}

/* P6 / G-ACH: the host's switch decides; both PCs in the default mode (auto) forward; an explicit off on either side stops it */
static void test_fwd_auto_model(void)
{
    pc_t H, C;
    pc_init(&H, 1, 1);
    pc_init(&C, 0, 0);
    C.fwd = ACH_FWD_AUTO;
    nwire = 0;
    pc_bump(&H, 0, 0x2B, 1, 1000);
    deliver(&C, 1040);
    CHECK(C.total[0x2B] == 1);
    H.fwd = ACH_FWD_AUTO;   /* G-ACH: a host in the default mode sends (the product default since 2026-11-07) */
    pc_bump(&H, 0, 0x2B, 1, 9000);
    CHECK(nwire == 1 && H.total[0x2B] == 0);
    deliver(&C, 9040);
    CHECK(C.total[0x2B] == 2);
    H.fwd = ACH_FWD_OFF;    /* REV2_ACHFWD=0 on the host: nothing travels, the host still never keeps the partner's bump */
    pc_bump(&H, 0, 0x2B, 1, 15000);
    CHECK(nwire == 0 && H.total[0x2B] == 0);
    deliver(&C, 15040);
    CHECK(C.total[0x2B] == 2);
    C.fwd = ACH_FWD_OFF;    /* an explicit off on the client wins */
    H.fwd = ACH_FWD_ON;
    pc_bump(&H, 0, 0x2B, 1, 20000);
    deliver(&C, 20040);
    CHECK(C.total[0x2B] == 2);
}

/* ---- F6 (G-ACH): the client's kill report at the host's DEATH. A blow timeline: the blow action runs [start, start + dur), its HIT lands at start + hit_at, the report runs at
 * HIT + rtt (+ one frame). Stock: evaluator A sees the action at the report; with the fix: ach_f6_action on the history the tracker builds from the action changes. */
typedef struct { unsigned t; unsigned act; } actev_t;
static unsigned act_at(const actev_t *ev, int n, unsigned t)
{
    unsigned a = 2;
    for (int i = 0; i < n && ev[i].t <= t; i++) a = ev[i].act;
    return a;
}
static unsigned report_sees(const actev_t *ev, int n, unsigned t_report, int fixed)
{
    ach_blowhist_t h = {0, 0, 0};
    unsigned prev = 2;
    for (int i = 0; i < n && ev[i].t <= t_report; i++) {
        ach_blowhist_on_change(&h, prev, ev[i].act, ev[i].t);
        prev = ev[i].act;
    }
    unsigned cur = act_at(ev, n, t_report);
    return fixed ? ach_f6_action(&h, cur, t_report, 1000) : cur;
}
static void test_f6(void)
{
    /* the measured case of G1 (knife slash, but timed like a blow): blow 690 ms, HIT at +550, report 74 ms later -> still inside: both see it */
    actev_t a[] = {{1000, 0x2B}, {1690, 2}};
    CHECK(report_sees(a, 2, 1000 + 550 + 74, 0) == 0x2B && report_sees(a, 2, 1000 + 550 + 74, 1) == 0x2B);
    /* internet round trips: the stock report reads idle (a lost credit), the fix gives the blow back */
    unsigned rtts[] = {150, 250, 400};
    for (int i = 0; i < 3; i++) {
        unsigned tr = 1000 + 550 + rtts[i];
        CHECK(report_sees(a, 2, tr, 0) == 2);
        CHECK(report_sees(a, 2, tr, 1) == 0x2B);
    }
    /* the follow-up (0x2F, kind 0x2A) the same way */
    actev_t f[] = {{1000, 0x2F}, {1500, 3}};
    CHECK(report_sees(f, 2, 1700, 0) == 3 && report_sees(f, 2, 1700, 1) == 0x2F);
    /* never an extra credit: */
    actev_t k[] = {{1000, 0x2B}, {1600, 2}, {1700, 28}, {2380, 2}};   /* a knife lunge after the blow: the killing HIT may be the lunge's */
    CHECK(report_sees(k, 4, 2450, 1) == 2);
    actev_t s[] = {{1000, 0x2D}, {1600, 13}, {1700, 22}, {1900, 13}};  /* aimed and fired after the blow (22 = fire) */
    CHECK(report_sees(s, 4, 2000, 1) == 13);
    actev_t r[] = {{1000, 0x2E}, {1600, 158}, {2600, 2}};            /* hit by an enemy after it (a reaction): no substitution (undercount accepted) */
    CHECK(report_sees(r, 3, 2700, 1) == 2);
    CHECK(report_sees(a, 2, 1690 + 1002, 1) == 2);                   /* outside the window (end tick |1) */
    actev_t n[] = {{1000, 28}, {1690, 2}};                             /* the knife lunge counts by the weapon: nothing to give back */
    CHECK(report_sees(n, 2, 1800, 1) == 2);
    CHECK(report_sees(n, 0, 1800, 1) == 2);                            /* no history at all */
    actev_t w[] = {{1000, 0x2B}, {1690, 3}, {1800, 4}, {1900, 2}, {2000, 17}};   /* neutral actions only (walk, crouch family, aim): still the blow */
    CHECK(report_sees(w, 5, 2100, 1) == 0x2B);
    actev_t b2[] = {{1000, 0x2B}, {1500, 2}, {1600, 0x2E}};           /* a second blow running at the report: evaluator A reads it itself */
    CHECK(report_sees(b2, 3, 1650, 1) == 0x2E && report_sees(b2, 3, 1650, 0) == 0x2E);
    /* a tick wrap between the end of the blow and the report */
    ach_blowhist_t z = {0x2B, 0xFFFFFFF1u, 1};
    CHECK(ach_f6_action(&z, 2, 100, 1000) == 0x2B && ach_f6_action(&z, 2, 1200, 1000) == 2);
    /* the counted ids */
    CHECK(ach_is_counted_act(0x2B) && ach_is_counted_act(0x2D) && ach_is_counted_act(0x2E) && ach_is_counted_act(0x2F) && !ach_is_counted_act(28) && !ach_is_counted_act(0x2C));
    CHECK(ach_is_neutral_act(2) && ach_is_neutral_act(3) && ach_is_neutral_act(21) && !ach_is_neutral_act(22) && !ach_is_neutral_act(28) && !ach_is_neutral_act(158));
}

/* P6: composition of world.inc's stat guard (h_killrep, line 266: snapshot of the three tables, roll back after the report unless the killer is the local human; the host rolls back
 * only the partner's puppet's kills) with the gate (no bump for a slot that is not a local human, BEFORE the record wrapper). One kill report = one bump of a class kind + the wrapper. */
static int statguard_roll(int active, int host, int mine, int theirs) { return active && !mine && (host ? theirs : 1); }

typedef struct { unsigned total; int done; } kr_t;
static void kill_report(kr_t *t, unsigned goal, int gate_on, int guard_on, int host, int killer_local, int killer_partner)
{
    unsigned snap = t->total;
    int roll = statguard_roll(guard_on, host, killer_local, killer_partner);
    ach_cfg_t c = {1, gate_on, host, 0};
    int v = (killer_local || killer_partner) ? ach_gate(&c, killer_local, killer_partner, 0x0F) : ACH_ALLOW;   /* a killer that is no player: the per-slot call does not name a session slot */
    t->total++;
    if (v != ACH_ALLOW) t->total = snap;   /* the gate puts the global total back before the wrapper */
    if (!t->done && t->total >= goal) t->done = 1;   /* the wrapper inside the report: record + Steam queue (cannot be rolled back) */
    if (roll) t->total = snap;             /* the stat guard after the report */
}

static void test_statguard(void)
{
    kr_t t;
    /* the partner's kill at goal-1 on the host: gate + guard, gate alone: no record, total unchanged */
    t.total = 99, t.done = 0;
    kill_report(&t, 100, 1, 1, 1, 0, 1);
    CHECK(t.total == 99 && !t.done);
    t.total = 99, t.done = 0;
    kill_report(&t, 100, 1, 0, 1, 0, 1);
    CHECK(t.total == 99 && !t.done);
    /* the guard alone (gate off): the total is restored but the record already fired: the hole of 6d that the gate closes */
    t.total = 99, t.done = 0;
    kill_report(&t, 100, 0, 1, 1, 0, 1);
    CHECK(t.total == 99 && t.done);
    /* the local human's kill (host or client): credited by both */
    t.total = 99, t.done = 0;
    kill_report(&t, 100, 1, 1, 0, 1, 0);
    CHECK(t.total == 100 && t.done);
    t.total = 99, t.done = 0;
    kill_report(&t, 100, 1, 1, 1, 1, 0);
    CHECK(t.total == 100 && t.done);
    /* a kill without a player killer (trap, compactor, burning statue) on the HOST: neither removes it, the host's profile counts it (stock single-player behaviour);
     * the client never runs a report for it (its replica dies with an empty DamageInfo, stage 6d), so only the host's account moves */
    t.total = 5, t.done = 0;
    kill_report(&t, 100, 1, 1, 1, 0, 0);
    CHECK(t.total == 6);
    /* the same on a client (if a report ever ran there): the guard rolls it back afterwards; at goal-1 the record would fire before it (the gate cannot see a non-player killer) */
    t.total = 99, t.done = 0;
    kill_report(&t, 100, 1, 1, 0, 0, 0);
    CHECK(t.total == 99 && t.done);
    /* the two never double-undo: with both on, a partner's kill leaves exactly the pre-report value */
    for (unsigned g = 1; g < 6; g++) {
        t.total = 3, t.done = 0;
        kill_report(&t, g, 1, 1, 1, 0, 1);
        CHECK(t.total == 3 && t.done == (3 >= g));
    }
}

/* docs/achievements.md must carry the same table: rows "| <steam> | ACH_NAME | 0x<rec> | PLAYER|SHARED|RISKY|N/A | ..." */
static void test_doc(void)
{
    const char *paths[] = {"../docs/achievements.md", "../../docs/achievements.md", "docs/achievements.md", "mod/docs/achievements.md"};
    FILE *f = NULL;
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0] && !f; i++) f = fopen(paths[i], "r");
    if (!f) {
        printf("(docs/achievements.md not found from this directory: table comparison skipped)\n");
        return;
    }
    int found[ACH_N] = {0}, rows = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned steam, rec;
        char name[64], rule[16];
        if (line[0] != '|') continue;
        if (sscanf(line, "| %u | %63s | 0x%x | %15s |", &steam, name, &rec, rule) != 4) continue;
        const ach_entry_t *e = ach_by_record(rec);
        CHECK(e != NULL);
        if (!e) continue;
        rows++;
        CHECK(e->steam == steam);
        CHECK(strcmp(e->name, name) == 0);
        const char *want = e->rule == ACHR_PLAYER ? "PLAYER" : e->rule == ACHR_SHARED ? "SHARED" : e->rule == ACHR_RISKY ? "RISKY" : "N/A";
        CHECK(strcmp(rule, want) == 0);
        found[e - ach_table] = 1;
    }
    fclose(f);
    CHECK(rows == ACH_N);
    for (int i = 0; i < ACH_N; i++) CHECK(found[i]);
}

static void test_epclear(void)
{
    ach_clear_tables_t t;
    memset(&t, 0, sizeof t);
    CHECK(!ach_epclear_credited(&t, 0, 1));
    ach_epclear_mark(&t, 0, 0, 1, 1);   /* the end of Claire's half (hardware: no ach: RECORD) */
    CHECK(!ach_epclear_credited(&t, 0, 1));
    ach_epclear_mark(&t, 1, 0, 1, 1);   /* the end of Barry's half (hardware: RECORD 0x02) */
    CHECK(ach_epclear_credited(&t, 0, 1));
    CHECK(!ach_epclear_credited(&t, 0, 2));
    CHECK(!ach_epclear_credited(&t, 1, 1));
    ach_epclear_mark(&t, 1, 0, 9, 0);   /* out of range: ignored */
    ach_epclear_mark(&t, 1, 0, 2, 7);
    CHECK(!ach_epclear_credited(&t, 0, 2));
    ach_epclear_mark(&t, 1, 0, 2, 3);   /* any difficulty counts */
    CHECK(ach_epclear_credited(&t, 0, 2));
}

int main(void)
{
    test_table();
    test_epclear();
    test_gate();
    test_ctx();
    test_block_default();
    test_dedupe_and_wire();
    test_two_pcs();
    test_rec_fwd();
    test_assist_and_fwd_mode();
    test_assist_model();
    test_assist_seat_variants();
    test_recfwd_model();
    test_fwd_auto_model();
    test_f6();
    test_statguard();
    test_doc();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
