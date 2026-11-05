/* test_bgl.c: offline model of the loader mirror (native/bgl_logic.h, stage 6j): the engine's list handling around bgl_merge, replayed frame by frame.
 * The model engine: every frame `prev := cur`; a zone hit (the test gives the list) rebuilds `cur`; then the mirror hook (bgl_merge) may rewrite `cur`; then the engine's unload step compares prev with cur
 * and unloads the triples that are in prev but not in cur (the model counts them in `unloaded`). Two PCs exchange their local lists. The bug that killed the first run on hardware: PC B's human stood on
 * block (100,4) while PC A's human stepped onto stage 101: with the old behaviour B's list lost (100,4) (unloaded under him) as soon as the zone list changed. */
#include <stdio.h>
#include <string.h>
#include "../native/bgl_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

typedef struct {
    bgl_t cur[BGL_SLOTS], prev[BGL_SLOTS];
    bgl_state_t m;
    bgl_t local[BGL_SLOTS];
    int local_n;
    int mirror;
    bgl_t unloaded[64];
    int n_unloaded;
    bgl_t asked[64];
    int n_asked;
} pc_t;

static void pc_init(pc_t *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < BGL_SLOTS; i++) bgl_clear_slot(&p->cur[i]), bgl_clear_slot(&p->prev[i]);
    p->mirror = 1;
}

static void pc_set(bgl_t *l, int n, const bgl_t *src)
{
    for (int i = 0; i < BGL_SLOTS; i++) bgl_clear_slot(&l[i]);
    for (int i = 0; i < n; i++) l[i] = src[i];
}

/* one frame of the engine; hit = list to rebuild from (or NULL: no zone hit this frame) */
static void pc_frame(pc_t *p, const bgl_t *hit, int hit_n)
{
    memcpy(p->prev, p->cur, sizeof p->cur);
    if (hit) pc_set(p->cur, hit_n, hit);
    if (p->mirror) {
        bgl_t cur2[BGL_SLOTS], ask[BGL_SLOTS];
        int ask_n = 0;
        bgl_merge(&p->m, p->cur, p->prev, cur2, p->local, &p->local_n, ask, &ask_n);
        memcpy(p->cur, cur2, sizeof cur2);
        for (int i = 0; i < ask_n; i++) p->asked[p->n_asked++ % 64] = ask[i];
    } else {
        p->local_n = 0;
        for (int i = 0; i < BGL_SLOTS; i++) if (bgl_valid(&p->cur[i])) p->local[p->local_n++] = p->cur[i];
    }
    for (int i = 0; i < BGL_SLOTS; i++)   /* the unload step */
        if (bgl_valid(&p->prev[i]) && bgl_find(p->cur, BGL_SLOTS, &p->prev[i]) < 0) p->unloaded[p->n_unloaded++ % 64] = p->prev[i];
}

static int unloaded_has(const pc_t *p, int stage, int block)
{
    bgl_t t = {0, stage, block};
    int n = p->n_unloaded < 64 ? p->n_unloaded : 64;
    return bgl_find(p->unloaded, n, &t) >= 0;
}

static void send_list(pc_t *from, pc_t *to)
{
    unsigned char b[40];
    int len = bgl_pack(from->local, from->local_n, b);
    bgl_t l[BGL_SLOTS];
    int c = bgl_unpack(b, len, l);
    CHECK(c == from->local_n);
    for (int i = 0; i < c; i++) to->m.remote[i] = l[i];
    to->m.remote_n = c;
}

int main(void)
{
    /* 1. wire form round trip, bad input refused */
    {
        bgl_t l[3] = {{-1, 101, 0}, {1, 101, 1}, {1, 102, 0}}, o[5];
        unsigned char b[40];
        int n = bgl_pack(l, 3, b);
        CHECK(n == 19 && b[0] == 3);
        CHECK(bgl_unpack(b, n, o) == 3 && o[0].ep == -1 && o[0].stage == 101 && o[2].stage == 102 && o[1].block == 1);
        CHECK(bgl_unpack(b, 5, o) == -1);
        b[0] = 9;
        CHECK(bgl_unpack(b, n, o) == -1);
        b[0] = 1, b[3] = 0xFF, b[4] = 0xFF;
        CHECK(bgl_unpack(b, 7, o) == -1);   /* stage 65535 */
    }
    /* 2. the hardware bug: B stands on (100,4), A's zone list changes to stage 101 blocks. Without the mirror B's block is unloaded; with it, it stays. */
    {
        pc_t a, b;
        pc_init(&a), pc_init(&b);
        bgl_t l100[2] = {{-1, 100, 4}, {1, 101, 0}}, l101[3] = {{-1, 101, 0}, {1, 101, 1}, {1, 101, 2}};
        /* B (the human left behind) is in the zone of block (100,4); A (the leader) is in the same zone first */
        for (int f = 0; f < 3; f++) pc_frame(&a, l100, 2), pc_frame(&b, l100, 2), send_list(&a, &b), send_list(&b, &a);
        CHECK(a.n_unloaded == 0 && b.n_unloaded == 0);
        /* A steps into stage 101: its zone list is the stage 101 list; B still hits the old zone every frame */
        for (int f = 0; f < 6; f++) { pc_frame(&a, l101, 3), pc_frame(&b, l100, 2), send_list(&a, &b), send_list(&b, &a); }
        CHECK(!unloaded_has(&a, 100, 4));   /* A keeps (100,4): B needs it */
        CHECK(a.local_n == 3 && bgl_find(a.local, a.local_n, &(bgl_t){0, 100, 4}) < 0);   /* ... but does not claim it as its own: no echo */
        CHECK(!unloaded_has(&b, 101, 1) && !unloaded_has(&b, 101, 2));   /* B gets A's blocks without having hit a zone */
        CHECK(bgl_find(b.cur, BGL_SLOTS, &(bgl_t){0, 101, 2}) >= 0);
        /* B walks on: its own zone list becomes stage 101 too; (100,4) is dropped by both */
        for (int f = 0; f < 6; f++) { pc_frame(&a, l101, 3), pc_frame(&b, l101, 3), send_list(&a, &b), send_list(&b, &a); }
        CHECK(unloaded_has(&a, 100, 4) && unloaded_has(&b, 100, 4));
        CHECK(a.local_n == 3 && b.local_n == 3);
        /* the control run: the same story without the mirror loses the block on A at once */
        pc_t c;
        pc_init(&c);
        c.mirror = 0;
        for (int f = 0; f < 3; f++) pc_frame(&c, l100, 2);
        pc_frame(&c, l101, 3);
        CHECK(unloaded_has(&c, 100, 4));
    }
    /* 3. no zone hit this frame: the merged-only entries stay, and are dropped when the partner drops them */
    {
        pc_t a;
        pc_init(&a);
        bgl_t mine[1] = {{1, 100, 2}};
        pc_frame(&a, mine, 1);
        a.m.remote[0] = (bgl_t){1, 101, 0}, a.m.remote[1] = (bgl_t){1, 101, 1}, a.m.remote_n = 2;
        pc_frame(&a, NULL, 0);
        CHECK(bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 101, 0}) >= 0 && bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 101, 1}) >= 0);
        CHECK(a.local_n == 1 && a.local[0].stage == 100);
        CHECK(a.m.mo_n == 2 && a.n_asked == 2);
        for (int f = 0; f < 4; f++) pc_frame(&a, NULL, 0);
        CHECK(a.m.mo_n == 2 && a.local_n == 1 && a.n_unloaded == 0);
        a.m.remote_n = 1;   /* the partner dropped (101,1) */
        a.n_unloaded = 0;
        pc_frame(&a, NULL, 0);
        CHECK(bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 101, 1}) < 0 && bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 101, 0}) >= 0);
        CHECK(a.m.removed == 1);
        pc_frame(&a, NULL, 0);
        CHECK(unloaded_has(&a, 101, 1) && !unloaded_has(&a, 101, 0));   /* the engine's own unload step takes the dropped block */
        a.m.remote_n = 0;
        pc_frame(&a, NULL, 0);
        pc_frame(&a, NULL, 0);
        CHECK(a.m.mo_n == 0 && a.local_n == 1 && bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 101, 0}) < 0);
    }
    /* 4. a triple that both humans need is the local human's own: it is sent, not treated as merged-only; a full list overflows politely */
    {
        pc_t a;
        pc_init(&a);
        bgl_t mine[3] = {{-1, 101, 0}, {1, 101, 1}, {1, 101, 2}};
        pc_frame(&a, mine, 3);
        a.m.remote[0] = (bgl_t){1, 101, 1}, a.m.remote_n = 1;
        pc_frame(&a, mine, 3);
        CHECK(a.m.mo_n == 0 && a.local_n == 3 && a.n_asked == 0);
        bgl_t remote5[5] = {{1, 102, 0}, {1, 102, 1}, {1, 102, 2}, {1, 102, 3}, {1, 102, 4}};
        for (int i = 0; i < 5; i++) a.m.remote[i] = remote5[i];
        a.m.remote_n = 5;
        pc_frame(&a, mine, 3);
        CHECK(a.m.mo_n == 2 && a.m.overflow == 3 && a.local_n == 3);
        CHECK(bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 102, 0}) >= 0 && bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 102, 1}) >= 0);
    }
    /* 5. invalid / empty slots are never merged or sent; duplicates in the partner's list count once */
    {
        pc_t a;
        pc_init(&a);
        bgl_t own[1] = {{-1, 100, 0}};
        pc_frame(&a, own, 1);
        a.m.remote[0] = (bgl_t){1, 101, 0}, a.m.remote[1] = (bgl_t){1, 101, 0}, a.m.remote[2] = (bgl_t){-1, -1, -1}, a.m.remote_n = 3;
        pc_frame(&a, NULL, 0);
        CHECK(a.m.mo_n == 1 && a.m.merged == 1 && a.local_n == 1);
        unsigned char b[40];
        CHECK(bgl_pack(a.local, a.local_n, b) == 7 && b[0] == 1);
        pc_t e;   /* an engine list with nothing in it never sends anything and merges nothing (6k) */
        pc_init(&e);
        e.m.remote[0] = (bgl_t){1, 101, 0}, e.m.remote_n = 1;
        pc_frame(&e, NULL, 0);
        CHECK(e.m.mo_n == 0 && e.m.merged == 0 && e.local_n == 0 && e.n_asked == 0);
        CHECK(bgl_pack(e.local, e.local_n, b) == 1 && b[0] == 0);
    }
    /* 6. the retry (6k): the engine tears the level down and empties the loader's list while the partner's list received before the death is still held. Hardware: the old logic merged that stale list
     * into the emptied list and asked for its blocks while the level was being rebuilt (AccessViolation in a worker thread, 5 of 5 retries). Now nothing is merged, the merged-only entries are dropped, and the
     * mirror works again once the local human hits a zone of the rebuilt level and the partner's fresh list arrived. */
    {
        pc_t a;
        pc_init(&a);
        bgl_t own[2] = {{-1, 100, 0}, {1, 100, 1}};
        pc_frame(&a, own, 2);
        a.m.remote[0] = (bgl_t){1, 100, 2}, a.m.remote[1] = (bgl_t){1, 100, 3}, a.m.remote_n = 2;   /* blocks only the partner needs */
        for (int f = 0; f < 3; f++) pc_frame(&a, NULL, 0);
        CHECK(a.m.mo_n == 2 && a.n_asked >= 2 && bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 100, 3}) >= 0);
        unsigned asked_before = (unsigned)a.n_asked, removed_before = a.m.removed;
        bgl_t none[1];
        for (int f = 0; f < 5; f++) pc_frame(&a, f == 0 ? none : NULL, 0);   /* the reset frame, then the frames of the load: the partner's list is stale and still held */
        CHECK(a.m.remote_n == 2);
        CHECK(a.m.mo_n == 0 && a.local_n == 0 && (unsigned)a.n_asked == asked_before);   /* nothing asked for the stale blocks */
        for (int i = 0; i < BGL_SLOTS; i++) CHECK(!bgl_valid(&a.cur[i]));   /* nothing was written into the loader's list */
        CHECK(a.m.idle == 5 && a.m.removed == removed_before);   /* the engine's reset already took the written entries; the mirror's own record is dropped */
        /* the stale list goes (the glue drops it when the load begins), the level is rebuilt: the local human hits the checkpoint's zone, the partner's fresh list arrives */
        a.m.remote_n = 0;
        bgl_t cp[2] = {{-1, 100, 0}, {1, 100, 1}};
        pc_frame(&a, cp, 2);
        CHECK(a.local_n == 2 && a.m.mo_n == 0 && (unsigned)a.n_asked == asked_before);
        a.m.remote[0] = (bgl_t){1, 100, 1}, a.m.remote[1] = (bgl_t){1, 100, 2}, a.m.remote_n = 2;
        pc_frame(&a, NULL, 0);
        CHECK(a.m.mo_n == 1 && bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 100, 2}) >= 0 && a.local_n == 2);   /* the mirror works again */
        /* a retry that empties the list when only merged-only entries are left in it (the engine never rebuilt it) is the same */
        a.local_n = 0;
        for (int f = 0; f < 2; f++) pc_frame(&a, f == 0 ? none : NULL, 0);
        CHECK(a.m.mo_n == 0 && a.local_n == 0 && !bgl_valid(&a.cur[0]) && !bgl_valid(&a.cur[1]) && !bgl_valid(&a.cur[2]));
    }
    /* 7. the mirror's purpose stays (6j): the humans are far apart (A in the old stage's block, the partner in stage 101): the lists are exchanged and the merged-only entries exist as long as A's own list is
     * non-empty, and a retry then drops them too */
    {
        pc_t a, b;
        pc_init(&a), pc_init(&b);
        bgl_t la[1] = {{-1, 100, 4}}, lb[2] = {{-1, 101, 0}, {1, 101, 1}};
        for (int f = 0; f < 4; f++) pc_frame(&a, la, 1), pc_frame(&b, lb, 2), send_list(&a, &b), send_list(&b, &a);
        CHECK(a.m.mo_n == 2 && b.m.mo_n == 1 && a.local_n == 1 && b.local_n == 2);
        CHECK(bgl_find(a.cur, BGL_SLOTS, &(bgl_t){0, 101, 1}) >= 0 && bgl_find(b.cur, BGL_SLOTS, &(bgl_t){0, 100, 4}) >= 0);
        CHECK(a.n_unloaded == 0 && b.n_unloaded == 0);
        bgl_t none[1];
        pc_frame(&a, none, 0), pc_frame(&b, none, 0);   /* the retry: both loaders reset */
        CHECK(a.m.mo_n == 0 && b.m.mo_n == 0 && a.local_n == 0 && b.local_n == 0);
        send_list(&a, &b), send_list(&b, &a);
        unsigned ma = a.m.merged, mb = b.m.merged;
        for (int f = 0; f < 3; f++) pc_frame(&a, NULL, 0), pc_frame(&b, NULL, 0);
        CHECK(a.m.mo_n == 0 && b.m.mo_n == 0 && a.m.merged == ma && b.m.merged == mb);   /* nothing merged from the empty lists */
    }
    printf("test_bgl: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
