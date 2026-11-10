/*
 * The co-op save of 0.2.1 (native/coopsave_logic.h, notes\coopsave.md), offline:
 *   1. ROUTING through a model of the game's save calls: the game's sSavedata only names "DATA0.DAT" through ISteamRemoteStorage (FileExists, GetFileSize,
 *      FileRead, FileWrite, FileDelete); the model has Steam's remote folder (the solo save, what Steam Cloud carries) and the mod's co-op file. A solo boot,
 *      a co-op game with checkpoints, a game over + retry (memory only), a second co-op game that loads the co-op save, the way back: the solo bytes must be
 *      exactly the ones from before, the co-op file must hold the co-op checkpoints. Built as the 0.2.0 model (CPS_MODEL_BEFORE: no routing) these checks fail:
 *      that model writes the co-op checkpoints into the solo save (tests/build.sh requires the failure).
 *   2. the in-memory solo image: kept at the co-op start, given back at the title (the model of coopsave.c's enter / leave), and the leave rule (cps_leave_due).
 *   3. the checkpoint record: built from a slot, the Continue rule (Episode 1, the half), the guest's apply (only mTempData's six fields, the flag block, the
 *      EpisodeFlags; the rest of the slot untouched), the wire (events <= 96 bytes, crc, lost / reordered events refused), the save bits of a player.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/coopsave_logic.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ a model of the save path */
typedef struct { char name[32]; unsigned char data[256]; int size, exists; unsigned writes; } mfile;
typedef struct {
    mfile steam[4];      /* userdata\<account>\287290\remote (what Steam Cloud uploads) */
    int nsteam;
    mfile coop;          /* the mod's co-op file (outside Steam's folders) */
    int co_op;           /* coopsave ON */
    unsigned steam_calls_on_save;   /* calls that reached Steam with the save name while co-op ran */
} world;

static mfile *steam_file(world *w, const char *name, int create)
{
    for (int i = 0; i < w->nsteam; i++)
        if (!strcmp(w->steam[i].name, name)) return &w->steam[i];
    if (!create || w->nsteam >= 4) return NULL;
    mfile *f = &w->steam[w->nsteam++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    return f;
}
/* the five wrapped slots: the route decides, as in coopsave.c's w_FileWrite ... w_GetFileSize */
static mfile *target(world *w, const char *name, int create)
{
    if (cps_route(w->co_op, name)) return &w->coop;
    if (w->co_op && cps_is_save_name(name)) w->steam_calls_on_save++;
    return steam_file(w, name, create);
}
static int m_write(world *w, const char *name, const void *d, int n)
{
    mfile *f = target(w, name, 1);
    if (!f || n > (int)sizeof f->data) return 0;
    memcpy(f->data, d, (size_t)n), f->size = n, f->exists = 1, f->writes++;
    return 1;
}
static int m_read(world *w, const char *name, void *d, int n)
{
    mfile *f = target(w, name, 0);
    if (!f || !f->exists) return 0;
    int k = n < f->size ? n : f->size;
    memcpy(d, f->data, (size_t)k);
    return k;
}
static int m_exists(world *w, const char *name) { mfile *f = target(w, name, 0); return f && f->exists; }
static int m_delete(world *w, const char *name)
{
    mfile *f = target(w, name, 0);
    if (!f || !f->exists) return 0;
    f->exists = 0, f->size = 0;
    return 1;
}

/* the game's side: its save data in memory ("BIN": one byte = the checkpoint, the rest = settings), the save manager's load / save requests */
typedef struct { unsigned char mem[64]; } game;
static void g_save(world *w, game *g) { m_write(w, CPS_SAVE_NAME, g->mem, sizeof g->mem); }
static int g_load(world *w, game *g)
{
    if (!m_exists(w, CPS_SAVE_NAME)) return 3;   /* sSavedata result 3: no file */
    return m_read(w, CPS_SAVE_NAME, g->mem, sizeof g->mem) == (int)sizeof g->mem ? 0 : 4;
}
/* coopsave.c enter / leave on the model: the solo image kept and given back */
static unsigned char g_kept[64];
static void enter(world *w, game *g)
{
    memcpy(g_kept, g->mem, sizeof g_kept);
#ifdef CPS_MODEL_BEFORE
    (void)w;   /* 0.2.0: no co-op save at all, the game plays on the solo save */
#else
    w->co_op = 1;
    if (m_exists(w, CPS_SAVE_NAME)) g_load(w, g);   /* the co-op save is the game's save now; none yet: the solo image is the start */
#endif
}
static void leave(world *w, game *g)
{
#ifndef CPS_MODEL_BEFORE
    memcpy(g->mem, g_kept, sizeof g_kept);
#endif
    w->co_op = 0;
}

static void test_names(void)
{
    CHECK(cps_is_save_name("DATA0.DAT") && cps_is_save_name("data0.dat") && cps_is_save_name("Data0.Dat"));
    CHECK(!cps_is_save_name("DATA0.DA") && !cps_is_save_name("DATA0.DAT ") && !cps_is_save_name("DATA1.DAT") && !cps_is_save_name("") && !cps_is_save_name(NULL));
    CHECK(!cps_is_save_name("remote.xml") && !cps_is_save_name("xDATA0.DAT"));
#ifndef CPS_MODEL_BEFORE
    CHECK(cps_route(1, "DATA0.DAT") && !cps_route(0, "DATA0.DAT") && !cps_route(1, "screenshot.jpg"));
#endif
    CHECK(cps_account_hash(92318434u) == cps_account_hash(92318434u) && cps_account_hash(92318434u) != cps_account_hash(92318435u));
    CHECK(cps_account_hash(92318434u) != 92318434u);   /* the account number itself is not the file name */
}

static void test_routing(void)
{
    world w;
    game g;
    memset(&w, 0, sizeof w);
    /* the player's solo game: the boot creates DATA0.DAT, the campaign reaches a checkpoint (byte 0 = 7) */
    memset(g.mem, 0, sizeof g.mem);
    CHECK(g_load(&w, &g) == 3);
    g.mem[0] = 7, g.mem[1] = 0x55;   /* checkpoint 7, a setting */
    g_save(&w, &g);
    mfile *solo = steam_file(&w, CPS_SAVE_NAME, 0);
    CHECK(solo && solo->exists && solo->writes == 1);
    mfile before = *solo;
    unsigned char mem_before[64];
    memcpy(mem_before, g.mem, sizeof mem_before);

    /* co-op game 1: no co-op save yet -> starts from the solo image; new game of the half; two checkpoints; a game over + retry (memory: no save call) */
    enter(&w, &g);
    CHECK(!memcmp(g.mem, mem_before, sizeof g.mem));
    g.mem[0] = 100;   /* new game of Claire's half */
    g_save(&w, &g);
    g.mem[0] = 101, g.mem[2] = 0x11;   /* checkpoint + a flag */
    g_save(&w, &g);
    g.mem[0] = 101;   /* game over, retry from the checkpoint: the restart copy in memory, nothing on disk */
    g.mem[1] = 0x66;  /* the options changed in the pause menu: saved */
    g_save(&w, &g);
    CHECK(w.coop.exists && w.coop.writes == 3 && w.coop.data[0] == 101 && w.coop.data[1] == 0x66);   /* the co-op save holds the co-op game */
    CHECK(solo->writes == before.writes && solo->size == before.size && !memcmp(solo->data, before.data, sizeof solo->data));   /* the solo save: byte for byte */
    CHECK(w.steam_calls_on_save == 0);   /* no call on the save name reached Steam while co-op ran */
    leave(&w, &g);
    CHECK(!memcmp(g.mem, mem_before, sizeof g.mem));   /* back at the title: the solo image, no trace (checkpoint 7, the old setting) */

    /* the solo game again: its Continue loads checkpoint 7 */
    memset(g.mem, 0, sizeof g.mem);
    CHECK(g_load(&w, &g) == 0 && g.mem[0] == 7 && g.mem[1] == 0x55);

    /* co-op game 2: the co-op save is loaded (Continue from checkpoint 101 with the co-op setting) */
    enter(&w, &g);
    CHECK(g.mem[0] == 101 && g.mem[1] == 0x66 && g.mem[2] == 0x11);
    g.mem[0] = 102;
    g_save(&w, &g);
    CHECK(w.coop.data[0] == 102);
    /* the game deletes a corrupt save and creates a new one: only the co-op file */
    CHECK(m_delete(&w, CPS_SAVE_NAME) && !w.coop.exists && solo->exists);
    g_save(&w, &g);
    leave(&w, &g);
    CHECK(!memcmp(solo->data, before.data, sizeof solo->data) && solo->writes == before.writes && solo->exists);
    CHECK(!memcmp(g.mem, mem_before, sizeof g.mem));
    /* another file of the game passes through during co-op */
    w.co_op = 1;
    CHECK(m_write(&w, "other.bin", "abc", 3) && steam_file(&w, "other.bin", 0) && steam_file(&w, "other.bin", 0)->writes == 1);
    w.co_op = 0;
}

static void test_leave_rule(void)
{
    cps_sm m;
    memset(&m, 0, sizeof m);
    CHECK(!cps_leave_due(&m, 0, 1, 1, 0, 0));   /* OFF: nothing */
    m.state = CPS_ON, m.enter_tick = 1000;
    int due = 0;
    for (int i = 0; i < 200 && !due; i++) due = cps_leave_due(&m, 0, 0, 1, 1, 1000 + (unsigned)i * 16);   /* the lobby screen (id 0x18): not the title menus */
    CHECK(!due);
    for (int i = 0; i < 60; i++) due |= cps_leave_due(&m, 1, 0, 1, 1, 5000);   /* in the level */
    CHECK(!due && m.level_seen);
    for (int i = 0; i < 89 && !due; i++) due = cps_leave_due(&m, 0, 1, 1, 1, 6000);
    CHECK(!due);   /* 89 passes at the title: not yet */
    CHECK(cps_leave_due(&m, 0, 1, 1, 1, 6000));   /* the 90th: leave */
    /* a save in flight at the title: wait for the save manager */
    memset(&m, 0, sizeof m);
    m.state = CPS_ON, m.level_seen = 1;
    due = 0;
    for (int i = 0; i < 300 && !due; i++) due = cps_leave_due(&m, 0, 1, i > 150, 1, 9000);
    CHECK(due && m.menu_frames == 90);
    /* a start that never reached a level: the solo save comes back when the session is gone, or after 60 s */
    memset(&m, 0, sizeof m);
    m.state = CPS_ON, m.enter_tick = 1000;
    due = 0;
    for (int i = 0; i < 100 && !due; i++) due = cps_leave_due(&m, 0, 1, 1, 1, 2000);
    CHECK(!due);
    CHECK(cps_leave_due(&m, 0, 1, 1, 0, 2000));
    m.menu_frames = 0;
    for (int i = 0; i < 100 && !due; i++) due = cps_leave_due(&m, 0, 1, 1, 1, 62000);
    CHECK(due);
}

static uint8_t slotA[CPS_SLOT_BYTES], slotB[CPS_SLOT_BYTES], slotC[CPS_SLOT_BYTES];
static void put(uint8_t *s, unsigned off, uint32_t v) { cps_wr32(s + off, v); }

typedef struct { uint8_t ev[600][96]; unsigned len[600]; int n, maxlen; } capture;
static int cap_emit(void *u, const uint8_t *b, unsigned len)
{
    capture *c = (capture *)u;
    if (c->n >= 600 || len > 96) return 0;
    memcpy(c->ev[c->n], b, len), c->len[c->n] = len, c->n++;
    if ((int)len > c->maxlen) c->maxlen = (int)len;
    return 1;
}

static void test_record(void)
{
    srand(7);
    for (unsigned i = 0; i < CPS_SLOT_BYTES; i++) slotA[i] = (uint8_t)rand(), slotB[i] = (uint8_t)rand();
    memset(slotA + CPS_FL_OFF, 0, CPS_FL_LEN), memset(slotA + CPS_EPF_OFF, 0, CPS_EPF_LEN);
    for (int k = 0; k < 300; k++) put(slotA, CPS_FL_OFF + 4u * (unsigned)(rand() % (CPS_FL_LEN / 4)), (uint32_t)rand() | 1u);   /* ~300 flag words set */
    put(slotA, CPS_EPF_OFF + 8, 0x80000000u);
    put(slotA, CPS_S_DIFF, 2), put(slotA, CPS_S_EPISODE, 1), put(slotA, CPS_S_2C, 0), put(slotA, CPS_S_STAGE, 101), put(slotA, CPS_S_STARTPOS, 2), put(slotA, CPS_S_SCENE, 0);
    cps_rec r;
    cps_rec_from_slot(&r, slotA);
    int part = -9, diff = -9, st = 0, sp = 0;
    CHECK(cps_rec_valid(&r) && cps_rec_continuable(&r, &part, &diff, &st, &sp) && part == 0 && diff == 2 && st == 101 && sp == 2);
    CHECK(cps_save_pflags(&r, 32, 64) == 32);
    cps_rec b = r;
    cps_rec_set_field(&b, CPS_S_STAGE, 153), b.crc = cps_rec_crc(&b);
    CHECK(cps_rec_continuable(&b, &part, NULL, NULL, NULL) && part == 1 && cps_save_pflags(&b, 32, 64) == 64);
    cps_rec_set_field(&b, CPS_S_STAGE, 201), cps_rec_set_field(&b, CPS_S_EPISODE, 2), b.crc = cps_rec_crc(&b);
    CHECK(!cps_rec_continuable(&b, NULL, NULL, NULL, NULL) && cps_save_pflags(&b, 32, 64) == 0);   /* Episode 2: not in co-op 0.2.x */
    b = r, b.fl[5] ^= 1;
    CHECK(!cps_rec_valid(&b));   /* a damaged record */
    CHECK(cps_stage_part(100) == 0 && cps_stage_part(102) == 0 && cps_stage_part(151) == 1 && cps_stage_part(154) == 1 && cps_stage_part(99) == -1);

    /* the guest follows: only the six mTempData fields, the flag block and the EpisodeFlags of its slot change */
    memcpy(slotC, slotB, sizeof slotC);
    cps_rec_apply_slot(&r, slotB);
    CHECK(cps_rd32(slotB + CPS_S_STAGE) == 101 && cps_rd32(slotB + CPS_S_STARTPOS) == 2 && cps_rd32(slotB + CPS_S_DIFF) == 2 && cps_rd32(slotB + CPS_S_EPISODE) == 1);
    CHECK(!memcmp(slotB + CPS_FL_OFF, slotA + CPS_FL_OFF, CPS_FL_LEN) && !memcmp(slotB + CPS_EPF_OFF, slotA + CPS_EPF_OFF, CPS_EPF_LEN));
    int other_same = 1;
    for (unsigned i = 0; i < CPS_SLOT_BYTES; i++) {
        int in_fields = (i >= CPS_S_DIFF && i < CPS_S_DIFF + 8) || (i >= CPS_S_2C && i < CPS_S_SCENE + 4);
        int in_blocks = (i >= CPS_FL_OFF && i < CPS_FL_OFF + CPS_FL_LEN) || (i >= CPS_EPF_OFF && i < CPS_EPF_OFF + CPS_EPF_LEN);
        if (!in_fields && !in_blocks && slotB[i] != slotC[i]) other_same = 0;
    }
    CHECK(other_same);   /* the guest's own inventory / stats parts of its slot stay its own */

    /* the wire */
    capture *c = calloc(1, sizeof *c);
    int n = cps_wire_send(&r, cap_emit, c);
    unsigned pairs = cps_rec_pairs(&r);
    CHECK(n == c->n && n == 2 + (int)((pairs + CPS_EV_PAIRS - 1) / CPS_EV_PAIRS) && c->maxlen <= 96);
    cps_rx x;
    cps_rx_reset(&x);
    int done = 0;
    for (int i = 0; i < c->n; i++) done += cps_rx_feed(&x, c->ev[i], c->len[i]);
    CHECK(done == 1 && x.complete && !x.bad && x.pairs_got == pairs);
    CHECK(!memcmp(x.rec.fl, r.fl, CPS_FL_LEN) && !memcmp(x.rec.epf, r.epf, CPS_EPF_LEN) && cps_rec_field(&x.rec, CPS_S_STAGE) == 101 && cps_rec_field(&x.rec, CPS_S_STARTPOS) == 2);
    memcpy(slotC, slotB, sizeof slotC);
    cps_rec_apply_slot(&x.rec, slotC);   /* the received record applies like the original */
    CHECK(!memcmp(slotC + CPS_FL_OFF, slotA + CPS_FL_OFF, CPS_FL_LEN) && cps_rd32(slotC + CPS_S_STAGE) == 101);
    /* a lost WORDS event: incomplete, never applied */
    cps_rx_reset(&x);
    for (int i = 0; i < c->n; i++)
        if (i != 2) cps_rx_feed(&x, c->ev[i], c->len[i]);
    CHECK(!x.complete && x.bad > 0);
    /* reordered: refused */
    cps_rx_reset(&x);
    cps_rx_feed(&x, c->ev[0], c->len[0]);
    cps_rx_feed(&x, c->ev[2], c->len[2]);
    cps_rx_feed(&x, c->ev[1], c->len[1]);
    for (int i = 3; i < c->n; i++) cps_rx_feed(&x, c->ev[i], c->len[i]);
    CHECK(!x.complete);
    /* a damaged value: the crc refuses it */
    cps_rx_reset(&x);
    for (int i = 0; i < c->n; i++) {
        uint8_t e[96];
        memcpy(e, c->ev[i], c->len[i]);
        if (i == 1) e[6] ^= 0x10;
        cps_rx_feed(&x, e, c->len[i]);
    }
    CHECK(!x.complete);
    /* a new HEAD starts again (a second start of the host) */
    for (int i = 0; i < c->n; i++) cps_rx_feed(&x, c->ev[i], c->len[i]);
    CHECK(x.complete && !memcmp(x.rec.fl, r.fl, CPS_FL_LEN));
    /* an empty flag block: HEAD + END only */
    memset(slotC + CPS_FL_OFF, 0, CPS_FL_LEN), memset(slotC + CPS_EPF_OFF, 0, CPS_EPF_LEN);
    cps_rec z;
    cps_rec_from_slot(&z, slotC);
    c->n = 0;
    CHECK(cps_wire_send(&z, cap_emit, c) == 2 && c->n == 2);
    cps_rx_reset(&x);
    for (int i = 0; i < c->n; i++) cps_rx_feed(&x, c->ev[i], c->len[i]);
    CHECK(x.complete);
    /* an invalid record is never sent */
    z.crc ^= 1;
    c->n = 0;
    CHECK(cps_wire_send(&z, cap_emit, c) == 0 && c->n == 0);
    free(c);
}

int main(void)
{
    test_names();
    test_routing();
    test_leave_rule();
    test_record();
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
