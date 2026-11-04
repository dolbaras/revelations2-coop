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
            CHECK(k && k->fwd);                                             /* every enemy-side kind is forwardable ... */
        }
        if (e->feeder == ACHF_KILLREP && e->kind != ACH_NOKIND) {
            const ach_kindw_t *k = ach_kind_find(e->kind);
            CHECK(k && !k->fwd);                                            /* ... and no kill-report kind is (the client runs the kill report itself) */
        }
    }
    for (int s = 1; s <= 43; s++) CHECK(seen_steam[s]);
    /* the game's own map has the same pairs, in the same order */
    for (int i = 0; i < 43; i++) {
        const ach_entry_t *e = ach_by_record(game_rec[i]);
        CHECK(e && e->steam == game_steam[i]);
    }
    CHECK(rules[ACHR_PLAYER] == 18 && rules[ACHR_SHARED] == 19 && rules[ACHR_RISKY] == 1 && rules[ACHR_NA] == 5);
    CHECK(ach_by_record(0x99) == 0 && ach_by_kind(0x01) == 0 && ach_by_kind(ACH_NOKIND) == 0);
    /* the five forwardable kinds */
    int nfwd = 0;
    for (int i = 0; i < ACH_NKINDW; i++) nfwd += ach_kindw[i].fwd;
    CHECK(nfwd == 5);
    CHECK(ach_fwd_find(0x2B) && ach_fwd_find(0x30) && ach_fwd_find(0x37) && ach_fwd_find(0x38) && ach_fwd_find(0x39));
    CHECK(!ach_fwd_find(0x26) && !ach_fwd_find(0x0F) && !ach_fwd_find(0x1A) && !ach_fwd_find(0x50));
    CHECK(ach_kind_find(0x26) && ach_kind_find(0x26)->wrapper == 0x4092F0);
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
} pc_t;

typedef struct { unsigned kind, n, seq, tick; } wire_t;
static wire_t wire[64];
static int nwire;

static void pc_init(pc_t *p, int host, int my_slot)
{
    memset(p, 0, sizeof *p);
    p->host = host, p->session = 1, p->fwd = 1, p->my_slot = my_slot, p->rem_slot = 1 - my_slot;
    for (int k = 0; k < 0x51; k++) p->goal[k] = 3;
}

/* the game bumps the kind for `slot` on this PC at time `now` (what h_a_statadd + h_a_statslot see) */
static void pc_bump(pc_t *p, int slot, unsigned kind, unsigned n, unsigned now)
{
    ach_cfg_t c = {p->session, 1, p->host, p->fwd};
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
    if (!p->session || p->host || !p->fwd || !ach_fwd_find(w->kind)) return;
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
    pc_bump(&C, 0, 0x30, 1, 40000);              /* the client's replica ran it */
    pc_bump(&H, 0, 0x30, 1, 40010);              /* the host's enemy ran it for the puppet: forwarded */
    deliver(&C, 40050);
    CHECK(C.total[0x30] == 1 && C.n_fwd_dup == 1);
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
    test_doc();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
