/* test_bomb.c: offline checks of the thrown bomb's one damage path (notes\bomb.md; native/invmirror_logic.h tb_* / bo_*, native/meleehit_logic.h mh_log_*).
 *   1. the BOOM event: round trip, garbage refused, who accepts it (the partner's own bomb with a throw number);
 *   2. the throw tables: throw numbers 1..255 (never 0), lookup by object and by number, expiry, a reused object address;
 *   3. the host's view: the client throws, its real bottle explodes on impact (the stock impact needs a LOCAL owner: FUN_00673000), the host's copy (owner = the type-2
 *      puppet) explodes at the client's explosion point through BOOM (b94ff2e: the copy never exploded: no explosion on the host's screen);
 *   4. damage once (the core rule): the client's real explosion on replicas A, B, C (C lethal) -> one MHIT each; the host's copy explodes (BOOM) and hits A, B, C and
 *      D (D only in its reach on the host); a direct hit + explosion; lag (the client's bottle hit only B); the copy's owner field cleared (uWeapon_vf35 writes
 *      +0x19C0 = 0); the host's own bomb; the other role order (the client's copy of the host's bomb): every enemy's hp on the host = start - the MHIT blows of the
 *      thrower's real explosion, exactly once, and the kill is the thrower's (b94ff2e: the copy with a cleared owner field doubled the damage);
 *   5. the kill credit of a burn tick (attacker 0, DI+0x2C bit 27; the igniter at enemy +0x7378) and of a weapon object (b94ff2e: killer slot -1, no credit);
 *   6. the MHIT log budgets: 60 damage-0 light lines do not hide a bottle's MHIT line (b94ff2e: one shared budget: flashlive run c read "no MHIT");
 *   7. one blow per replica update: an explosion that puts a DI into the enemy and into a part object in the same frame is applied once, as the stock reader
 *      FUN_006001D0 does (the DI with the most damage) (b94ff2e: every DI went out as MHIT: the sum).
 * Built a second time with -DBOMB_MODEL_BEFORE -DMH_MODEL_BEFORE_BOMB (rev2-int b94ff2e): that model must fail (tests/build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/grapple_logic.h"
#include "../native/meleehit_logic.h"
#include "../native/invmirror_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 60) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* 1. the BOOM event */
static void test_events(void)
{
    uint8_t b[TB_BOOM_LEN + 4];
    float p[3] = {4583.5f, 2100.25f, -19766.0f}, q[3];
    unsigned len = tb_boom_encode(b, 1, 7, 0x147, p);
    CHECK(len == TB_BOOM_LEN);
    int slot = -1;
    uint8_t seq = 0;
    uint16_t id = 0;
    CHECK(tb_boom_decode(b, len, &slot, &seq, &id, q) && slot == 1 && seq == 7 && id == 0x147 && !memcmp(p, q, sizeof p));
    CHECK(!tb_boom_decode(b, len - 1, &slot, &seq, &id, q));
    float nan = 0.0f / 0.0f, big = 3e8f;
    memcpy(b + 8, &nan, 4);
    CHECK(!tb_boom_decode(b, len, &slot, &seq, &id, q));
    memcpy(b + 8, &big, 4);
    CHECK(!tb_boom_decode(b, len, &slot, &seq, &id, q));
    /* who explodes a copy: the host (my slot 0, partner 1) for the client's bomb and the client for the host's; never this PC's own human's, never without a number */
    CHECK(tb_boom_accept(1, 0, 1, 7, 0x147) == 1);   /* b94ff2e: no BOOM at all */
    CHECK(tb_boom_accept(0, 1, 0, 7, 0x147) == 1);
    CHECK(tb_boom_accept(0, 0, 1, 7, 0x147) == 0);
    CHECK(tb_boom_accept(1, 0, 1, 0, 0x147) == 0);   /* an older peer's THROW carried no number */
    CHECK(tb_boom_accept(1, 0, 1, 7, 0x702) == 0);
    CHECK(tb_boom_accept(1, 0, 1, 7, 0x301) == 0);
    CHECK(tb_boom_explode(0x147) == 1 && tb_boom_explode(0x144) == 1 && tb_boom_explode(0x145) == 0);
    /* THROW keeps its layout: the throw number rides in the spare byte [1], an older decoder reads the same slot / id / vectors */
    float v[12], w[12];
    for (int i = 0; i < 12; i++) v[i] = (float)i * 3.5f - 7.0f;
    uint8_t t[TH_LEN];
    th_encode(t, 1, 0x147, v);
    t[1] = 200;
    uint16_t tid = 0;
    CHECK(th_decode(t, TH_LEN, &slot, &tid, w) && slot == 1 && tid == 0x147 && !memcmp(v, w, sizeof v) && t[1] == 200);
}

/* 2. the throw tables */
static void test_tables(void)
{
    tb_tab_t tb;
    memset(&tb, 0, sizeof tb);
    int zero = 0, seen[256] = {0};
    for (int i = 0; i < 600; i++) {
        uint8_t s = tb_next_seq(&tb);
        if (!s) zero++;
        seen[s]++;
    }
    CHECK(zero == 0 && seen[1] >= 2 && seen[255] >= 2);
    memset(&tb, 0, sizeof tb);
    uint32_t now = 1000000;
    tb_add(&tb, 0x2B698060u, 1, 3, 0x147, now);
    tb_add(&tb, 0x2B438060u, 1, 4, 0x147, now + 500);
    CHECK(tb_by_obj(&tb, 0x2B698060u, now + 600) && tb_by_obj(&tb, 0x2B698060u, now + 600)->seq == 3);
    CHECK(tb_by_seq(&tb, 1, 4, now + 600) && tb_by_seq(&tb, 1, 4, now + 600)->obj == 0x2B438060u);
    CHECK(!tb_by_seq(&tb, 0, 4, now + 600));   /* another thrower */
    CHECK(!tb_by_obj(&tb, 0, now));
    CHECK(!tb_by_obj(&tb, 0x2B698060u, now + TB_MAX_MS + 1));   /* forgotten */
    /* the game reuses a freed bomb's memory for the next bomb: the new throw replaces the old entry (no second entry for one address) */
    tb_add(&tb, 0x2B698060u, 1, 9, 0x147, now + 3000);
    int n = 0;
    for (unsigned i = 0; i < TB_N; i++) n += tb.e[i].used && tb.e[i].obj == 0x2B698060u;
    CHECK(n == 1 && tb_by_obj(&tb, 0x2B698060u, now + 3001)->seq == 9 && !tb_by_seq(&tb, 1, 3, now + 3001));
    /* more throws than entries: the oldest goes, the newest stay */
    for (int i = 0; i < 12; i++) tb_add(&tb, 0x30000000u + 0x10000u * (unsigned)i, 1, (uint8_t)(20 + i), 0x147, now + 4000 + (uint32_t)i * 10);
    CHECK(tb_by_seq(&tb, 1, 31, now + 4200) && !tb_by_seq(&tb, 1, 20, now + 4200));
    /* the same number again after a wrap: the newest copy with it */
    tb_add(&tb, 0x40000000u, 1, 31, 0x147, now + 9000);
    CHECK(tb_by_seq(&tb, 1, 31, now + 9001) && tb_by_seq(&tb, 1, 31, now + 9001)->obj == 0x40000000u);
}

/* ---- a model of the pair for 3 / 4: the stock bomb, the events, the host's enemies ---- */
#define NE 4
typedef struct { int hp, dead_by; } enemy_t;
typedef struct {
    tb_tab_t mine, copies;     /* per PC */
    int my_slot, rem_slot;
    uint32_t human, puppet;    /* this PC's objects of the two slots */
} pcm_t;
typedef struct { uint32_t obj, owner; int local_owner, exploded; float at[3]; } bomb_t;
static const uint32_t CL_MOIRA = 0x29F50060u, CL_PUP_CLAIRE = 0x29F30060u, H_CLAIRE = 0x2AF28060u, H_PUP_MOIRA = 0x2AF50060u;

/* the stock flight update (uWpBomb_vf134): the impact starts the explosion only when the owner is a local player (FUN_00673000), then the Raid sender (our BOOM hook) */
static int stock_impact(bomb_t *b, const float at[3])
{
    if (!b->local_owner) return 0;
    b->exploded = 1;
    memcpy(b->at, at, sizeof b->at);
    return 1;
}
/* h_boomsend: the thrower's PC */
static int boom_send(pcm_t *pc, const bomb_t *b, uint32_t now, uint8_t *ev)
{
    tb_ent_t *t = tb_by_obj(&pc->mine, b->obj, now);
    if (!t || t->boomed) return 0;
    t->boomed = 1;
    return (int)tb_boom_encode(ev, pc->my_slot, t->seq, t->id, b->at);
}
/* w_boom_apply: the other PC */
static int boom_apply(pcm_t *pc, const uint8_t *ev, unsigned len, bomb_t *copy, uint32_t now)
{
    int slot;
    uint8_t seq;
    uint16_t id;
    float pos[3];
    if (!tb_boom_decode(ev, len, &slot, &seq, &id, pos) || !tb_boom_accept(slot, pc->my_slot, pc->rem_slot, seq, id)) return 0;
    tb_ent_t *t = tb_by_seq(&pc->copies, slot, seq, now);
    if (!t || t->boomed || t->obj != copy->obj || !tb_boom_explode(id)) return 0;
    t->boomed = 1;
    copy->exploded = 1;   /* action 8 at the thrower's point */
    memcpy(copy->at, pos, sizeof pos);
    return 1;
}
/* one throw of the client (slot 1): THROW (the copy on the host with the number), returns the two bombs */
static void client_throw(pcm_t *cl, pcm_t *ho, bomb_t *real, bomb_t *copy, uint32_t now)
{
    memset(real, 0, sizeof *real), memset(copy, 0, sizeof *copy);
    real->obj = 0x2B438060u, real->owner = CL_MOIRA, real->local_owner = 1;
    copy->obj = 0x2B698060u, copy->owner = H_PUP_MOIRA, copy->local_owner = 0;   /* w_throw_apply: owner +0x19C0 = the puppet (type 2: not local) */
    uint8_t seq = tb_next_seq(&cl->mine);
    tb_add(&cl->mine, real->obj, cl->my_slot, seq, 0x147, now);
    tb_add(&ho->copies, copy->obj, cl->my_slot, seq, 0x147, now + 9);
}

/* 3. the host's view of the client's bomb */
static void test_view(void)
{
    pcm_t cl = {0}, ho = {0};
    cl.my_slot = 1, cl.rem_slot = 0, cl.human = CL_MOIRA, cl.puppet = CL_PUP_CLAIRE;
    ho.my_slot = 0, ho.rem_slot = 1, ho.human = H_CLAIRE, ho.puppet = H_PUP_MOIRA;
    uint32_t now = 5000;
    bomb_t real, copy;
    client_throw(&cl, &ho, &real, &copy, now);
    float hit[3] = {4583.0f, 2000.0f, -19766.0f};
    CHECK(stock_impact(&copy, hit) == 0);   /* the copy itself never explodes (flashlive: the damage came through MHIT, not from the copy) */
    CHECK(stock_impact(&real, hit) == 1);
    uint8_t ev[TB_BOOM_LEN];
    unsigned len = (unsigned)boom_send(&cl, &real, now + 800, ev);
    CHECK(len == TB_BOOM_LEN);
    CHECK(boom_send(&cl, &real, now + 801, ev) == 0);   /* once per bomb */
    CHECK(boom_apply(&ho, ev, len, &copy, now + 820) == 1);
    CHECK(copy.exploded && !memcmp(copy.at, hit, sizeof hit));   /* b94ff2e: the host never saw the partner's bottle explode */
    CHECK(boom_apply(&ho, ev, len, &copy, now + 830) == 0);   /* a repeated event does not explode it twice */
    /* the other role order: the host's Claire throws, the client's copy explodes */
    bomb_t hreal = {0x2C000060u, H_CLAIRE, 1, 0, {0}}, ccopy = {0x2C100060u, CL_PUP_CLAIRE, 0, 0, {0}};
    uint8_t s2 = tb_next_seq(&ho.mine);
    tb_add(&ho.mine, hreal.obj, ho.my_slot, s2, 0x147, now);
    tb_add(&cl.copies, ccopy.obj, ho.my_slot, s2, 0x147, now + 9);
    CHECK(stock_impact(&hreal, hit));
    len = (unsigned)boom_send(&ho, &hreal, now + 900, ev);
    CHECK(len && boom_apply(&cl, ev, len, &ccopy, now + 920) == 1 && ccopy.exploded);
    /* a copy whose lifetime ended before the explosion event: nothing happens (the receiver checks sItem's list; here: no entry) */
    pcm_t ho2 = ho;
    memset(&ho2.copies, 0, sizeof ho2.copies);
    bomb_t lost = copy;
    lost.exploded = 0;
    CHECK(boom_apply(&ho2, ev, len, &lost, now + 940) == 0 && !lost.exploded);
}

/* 4. damage once. One blow = one DamageInfo the collision puts into the enemy's mailbox (attacker, its owner field +0x19C0, damage). */
typedef struct { int enemy; uint32_t attacker, owner; int dmg; } blow_t;
/* the client: a blow on a replica becomes an MHIT when mh_take says MINE (one level up through +0x19C0, never a copy of the partner's throw) */
static int client_mhit(const pcm_t *cl, const blow_t *bl, int replica_hp, uint32_t now)
{
    mh_take_in_t in = {1, bl->attacker, bl->owner, cl->human, 0x00002004u, replica_hp};
    int t = mh_take(&in);
#ifndef BOMB_MODEL_BEFORE
    if (bl->attacker && tb_by_obj((tb_tab_t *)&cl->copies, bl->attacker, now)) t = MH_TAKE_OTHER;
#else
    (void)now;
#endif
    return t == MH_TAKE_MINE;
}
/* the host: a blow of its own world lands in the mailbox; P2 drops the puppet's and the copies' blows before the stock update reads it */
static void host_blow(pcm_t *ho, enemy_t *e, const blow_t *bl, uint32_t now)
{
    enemy_t *x = &e[bl->enemy];
    if (x->hp <= 0) return;
    int is_copy = tb_by_obj(&ho->copies, bl->attacker, now) != 0;
    if (tb_p2_drop(1, bl->attacker, bl->owner, ho->puppet, 0, is_copy)) return;
    x->hp -= bl->dmg;
    if (x->hp <= 0) {
        int plain = bl->attacker == ho->human ? ho->my_slot : bl->attacker == ho->puppet ? ho->rem_slot : -1;
        int weapon = bl->owner == ho->human ? ho->my_slot : bl->owner == ho->puppet ? ho->rem_slot : -1;
        x->dead_by = bo_credit_slot(plain, weapon, bl->attacker == 0, 0, -1);
    }
}
/* the host: an MHIT of the client (mh_post: the rebuilt DI's attacker is the puppet) */
static void host_mhit(pcm_t *ho, enemy_t *e, const blow_t *bl, uint32_t now)
{
    blow_t m = {bl->enemy, ho->puppet, 0, bl->dmg};
    if (!mh_apply_ok(e[bl->enemy].hp)) return;
    e[bl->enemy].hp -= m.dmg;
    if (e[bl->enemy].hp <= 0) e[bl->enemy].dead_by = bo_credit_slot(ho->rem_slot, -1, 0, 0, -1);
    (void)now;
}
static void run_case(const char *name, int owner_cleared, int copy_shot, const blow_t *real, int nreal, const int *copy_targets, int ncopy, const int *start, const int *want, int want_kill_enemy)
{
    pcm_t cl = {0}, ho = {0};
    cl.my_slot = 1, cl.rem_slot = 0, cl.human = CL_MOIRA, cl.puppet = CL_PUP_CLAIRE;
    ho.my_slot = 0, ho.rem_slot = 1, ho.human = H_CLAIRE, ho.puppet = H_PUP_MOIRA;
    enemy_t e[NE];
    for (int i = 0; i < NE; i++) e[i].hp = start[i], e[i].dead_by = -9;
    uint32_t now = 70000;
    bomb_t rb, cb;
    client_throw(&cl, &ho, &rb, &cb, now);
    /* the client's real explosion: MHIT for each blow on a live replica (the replicas show the host's hp) */
    for (int i = 0; i < nreal; i++)
        if (client_mhit(&cl, &real[i], e[real[i].enemy].hp, now + 800)) host_mhit(&ho, e, &real[i], now + 820);
    /* the host's copy explodes at the same point (BOOM) and its collision hits the host's enemies around it */
    uint8_t ev[TB_BOOM_LEN];
    float at[3] = {0, 0, 0};
    stock_impact(&rb, at);
    unsigned len = (unsigned)boom_send(&cl, &rb, now + 800, ev);
    int exploded = len && boom_apply(&ho, ev, len, &cb, now + 820);
    if (copy_shot) exploded = 1;   /* the copy explodes on the host without BOOM (GUESS: the host's human shoots it in flight, uWpBomb_vf111 -> action 8) */
    if (exploded)
        for (int i = 0; i < ncopy; i++) {
            blow_t cbl = {copy_targets[i], cb.obj, owner_cleared ? 0u : ho.puppet, 900};
            host_blow(&ho, e, &cbl, now + 830);
        }
    int ok = 1;
    for (int i = 0; i < NE; i++)
        if (e[i].hp != want[i] && !(want[i] <= 0 && e[i].hp <= 0)) ok = 0;
    if (want_kill_enemy >= 0 && e[want_kill_enemy].dead_by != 1) ok = 0;
    if (!ok) printf("  case '%s': hp %d %d %d %d (want %d %d %d %d), kill of %d by slot %d\n", name, e[0].hp, e[1].hp, e[2].hp, e[3].hp, want[0], want[1], want[2], want[3], want_kill_enemy,
                    want_kill_enemy >= 0 ? e[want_kill_enemy].dead_by : -1);
    CHECK(ok);
}
static void test_damage_once(void)
{
    const int start[NE] = {3000, 2900, 600, 500};
    /* the area hit of run c: A 1224, B (Em1140) 720, C killed; the copy also reaches D on the host */
    blow_t area[3] = {{0, 0x2B438060u, CL_MOIRA, 1224}, {1, 0x2B438060u, CL_MOIRA, 720}, {2, 0x2B438060u, CL_MOIRA, 650}};
    int copy_all[4] = {0, 1, 2, 3};
    int want_area[NE] = {1776, 2180, 0, 500};
    run_case("area", 0, 0, area, 3, copy_all, 4, start, want_area, 2);
    run_case("area, the copy's owner field cleared", 1, 0, area, 3, copy_all, 4, start, want_area, 2);
    run_case("area, the copy shot in flight, owner field cleared", 1, 1, area, 3, copy_all, 4, start, want_area, 2);   /* b94ff2e: A, B, D took the copy's 900 as well */
    /* a direct hit: the impact blow and then the explosion on the same enemy are two stock blows (two MHIT, applied once each) */
    blow_t direct[2] = {{0, 0x2B438060u, CL_MOIRA, 100}, {0, 0x2B438060u, CL_MOIRA, 1224}};
    int copy_a[1] = {0};
    int want_direct[NE] = {1676, 2900, 600, 500};
    run_case("direct", 0, 0, direct, 2, copy_a, 1, start, want_direct, -1);
    run_case("direct, owner cleared", 1, 0, direct, 2, copy_a, 1, start, want_direct, -1);
    /* lag: on the client the bottle only reached B; the copy at the same point reaches A and B on the host: only B takes damage */
    blow_t lag[1] = {{1, 0x2B438060u, CL_MOIRA, 720}};
    int copy_ab[2] = {0, 1};
    int want_lag[NE] = {3000, 2180, 600, 500};
    run_case("lag", 1, 0, lag, 1, copy_ab, 2, start, want_lag, -1);
    run_case("lag, the copy shot", 1, 1, lag, 1, copy_ab, 2, start, want_lag, -1);
    /* a lethal hit on a weak enemy and an overkill second blow (the dead-enemy guard): one death, the client's */
    blow_t lethal[2] = {{3, 0x2B438060u, CL_MOIRA, 700}, {3, 0x2B438060u, CL_MOIRA, 300}};
    int copy_d[1] = {3};
    int want_lethal[NE] = {3000, 2900, 600, 0};
    run_case("lethal", 1, 0, lethal, 2, copy_d, 1, start, want_lethal, 3);
    /* the replicas never act on the copy's blows on the client: the client's copy of the HOST's bomb (owner = the client's puppet, or cleared) is never sent */
    pcm_t cl = {0};
    cl.my_slot = 1, cl.human = CL_MOIRA, cl.puppet = CL_PUP_CLAIRE;
    tb_add(&cl.copies, 0x2C100060u, 0, 5, 0x147, 100);
    blow_t c1 = {0, 0x2C100060u, CL_PUP_CLAIRE, 900}, c2 = {0, 0x2C100060u, 0, 900}, c3 = {0, 0x2C100060u, CL_MOIRA, 900};
    CHECK(!client_mhit(&cl, &c1, 500, 200) && !client_mhit(&cl, &c2, 500, 200));
#ifndef BOMB_MODEL_BEFORE
    CHECK(!client_mhit(&cl, &c3, 500, 200));   /* even a copy whose owner field names this PC's human (never the case today) is not sent */
#else
    (void)c3;
#endif
    /* the host's own bomb: a stock blow on its enemy, never dropped, the kill is the host's (weapon owner) */
    pcm_t ho = {0};
    ho.my_slot = 0, ho.rem_slot = 1, ho.human = H_CLAIRE, ho.puppet = H_PUP_MOIRA;
    enemy_t e[NE] = {{600, -9}, {600, -9}, {600, -9}, {600, -9}};
    blow_t own = {0, 0x2C000060u, H_CLAIRE, 900};
    host_blow(&ho, e, &own, 300);
    CHECK(e[0].hp <= 0);
#ifndef BOMB_MODEL_BEFORE
    CHECK(e[0].dead_by == 0);   /* b94ff2e: killer slot -1 (the attacker is the bomb object) */
#endif
    /* the puppet's own blow (its replayed swing) is still dropped (combat2 P2) */
    blow_t pb = {1, H_PUP_MOIRA, 0, 45};
    host_blow(&ho, e, &pb, 300);
    CHECK(e[1].hp == 600);
}

/* 5. the kill credit */
static void test_credit(void)
{
    const uint32_t tick = 1u << BO_TICK_BIT;
    CHECK(bo_credit_slot(1, -1, 0, 0, -1) == 1);        /* the puppet (MHIT) */
    CHECK(bo_credit_slot(0, -1, 0, 0, -1) == 0);        /* the host's human */
    CHECK(bo_credit_slot(-1, 0, 0, 0, -1) == 0);        /* the host's own bomb object: its owner (b94ff2e: -1) */
    CHECK(bo_credit_slot(-1, 1, 1, tick, 1) == 1);      /* a burn tick: the igniter at +0x7378 is the puppet (the MHIT lit it) (b94ff2e: -1) */
    CHECK(bo_credit_slot(-1, -1, 1, tick, -1) == -1);   /* a burn tick without an igniter */
    CHECK(bo_credit_slot(-1, -1, 1, 0, 1) == -1);       /* attacker 0 but not a tick */
    CHECK(bo_credit_slot(-1, -1, 0, tick, 1) == -1);    /* an enemy as attacker: never the igniter */
}

/* 6. the MHIT log budgets */
static void test_log_budget(void)
{
    uint32_t used[MH_LOG_KINDS] = {0};
    int light_logged = 0;
    for (int i = 0; i < 2689; i++) light_logged += mh_log_take(used, mh_log_kind(0, 0), 60);   /* the light: damage-0 hits of Moira herself */
    CHECK(light_logged == 60);
    CHECK(mh_log_take(used, mh_log_kind(1224, 1), 60) == 1);   /* the bottle's MHIT (an object of the human, with damage) 11 minutes later (b94ff2e: no line) */
    CHECK(mh_log_take(used, mh_log_kind(45, 0), 60) == 1);     /* a knife hit with damage */
    CHECK(mh_log_kind(0, 0) == MH_LOG_PLAIN && mh_log_kind(5, 0) == MH_LOG_DAMAGE && mh_log_kind(0, 1) == MH_LOG_OBJECT);
    int obj_logged = 0;
    for (int i = 0; i < 100; i++) obj_logged += mh_log_take(used, MH_LOG_OBJECT, 60);
    CHECK(obj_logged == 59);   /* each budget still ends */
}

/* 7. one blow per replica update */
static void test_pick(void)
{
    int32_t dmg[MH_NPARTS + 1] = {0};
    uint8_t take[MH_NPARTS + 1] = {0};
    CHECK(mh_pick_blow(dmg, take) == -1);
    take[0] = 1, dmg[0] = 612;
    CHECK(mh_pick_blow(dmg, take) == 0);
    take[3] = 1, dmg[3] = 612;   /* the same damage in part 2's object: the main DI stays (ties keep the earlier one) */
    CHECK(mh_pick_blow(dmg, take) == 0);
    dmg[3] = 700;
    CHECK(mh_pick_blow(dmg, take) == 3);
    take[0] = 0;
    CHECK(mh_pick_blow(dmg, take) == 3);
    /* the host's damage for one explosion that wrote the main DI (612) and two part DIs (612, 300) in one frame: one application of 612 */
    int32_t d2[MH_NPARTS + 1] = {612, 0, 612, 0, 300, 0, 0, 0, 0, 0, 0};
    uint8_t t2[MH_NPARTS + 1] = {1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
    int picked = mh_pick_blow(d2, t2), host = 0, sent = 0;
    for (int i = 0; i <= MH_NPARTS; i++)
        if (t2[i] && mh_send_this(i, picked)) host += d2[i], sent++;
    CHECK(sent == 1 && host == 612);   /* b94ff2e: 3 MHIT, 1524 damage */
    CHECK(!mh_send_this(-1, -1));
    /* a DI already sent is not fresh: the next update's new blow in another part goes out alone */
    mh_dedupe_t dd;
    memset(&dd, 0, sizeof dd);
    CHECK(mh_dedupe_peek(&dd, 0x2B288060u, 0, 0x1234u, 10) == 1);
    CHECK(mh_dedupe_new(&dd, 0x2B288060u, 0, 0x1234u, 10) == 1);
    CHECK(mh_dedupe_peek(&dd, 0x2B288060u, 0, 0x1234u, 20) == 0 && mh_dedupe_peek(&dd, 0x2B288060u, 0, 0x9999u, 20) == 1);
    mh_dedupe_clear(&dd, 0x2B288060u, 0, 30);
    CHECK(mh_dedupe_peek(&dd, 0x2B288060u, 0, 0x1234u, 40) == 1);
}

int main(void)
{
    test_events();
    test_tables();
    test_view();
    test_damage_once();
    test_credit();
    test_log_budget();
    test_pick();
    printf("test_bomb: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
