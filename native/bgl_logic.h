/* bgl_logic.h: the pure part of the "loader serves both humans" mirror (stage 6j), so tests/test_bgl.c can replay it without a game.
 *
 * The game's block streamer (sBGLoad, scen.inc) keeps a current list of up to five (episode, stage, block) triples, rebuilt only when the VIEWPORT-0 player (the local human) hits a BGLoad zone, and unloads
 * every block the previous frame's list had and the current one has not. Each PC therefore streams for its own human only. Found on hardware (s6j1): the story stage changed on both PCs when slot 0 stepped
 * onto the next stage's floor, the list became the next stage's blocks, the block under the OTHER human (still in the old stage) was unloaded and he fell to his death.
 * The mirror: every PC sends its own list (without what it merged for the partner) and, just before the game's unload step (`FUN_00466C30`, once a frame), puts the partner's triples into the free slots of the
 * current list and asks the game to load them. A triple that only the partner needs is "merged-only"; it is not sent back (no echo) and it is removed again when the partner's list drops it.
 *   bgl_merge(): one call per frame, inputs: the current list as read, the previous frame's copy, the state; outputs: the list to write back, the local list to send, the triples to ask the loader for. */
#ifndef BGL_LOGIC_H
#define BGL_LOGIC_H

#define BGL_SLOTS 5

typedef struct { int ep, stage, block; } bgl_t;

typedef struct {
    bgl_t remote[BGL_SLOTS];   /* the partner's own list as received */
    int remote_n;
    bgl_t mo[BGL_SLOTS];       /* triples this PC merged for the partner in the previous call (not its own need) */
    int mo_n;
    unsigned merged, overflow, removed;   /* counters: slots filled / triples that found no free slot / stale merged-only entries dropped */
} bgl_state_t;

static inline int bgl_valid(const bgl_t *t) { return t->stage >= 0 || t->block >= 0; }   /* the engine's free slot is (-1, -1, -1) */
static inline int bgl_same(const bgl_t *a, const bgl_t *b) { return a->stage == b->stage && a->block == b->block; }

static inline int bgl_find(const bgl_t *l, int n, const bgl_t *t)
{
    for (int i = 0; i < n; i++)
        if (bgl_valid(&l[i]) && bgl_same(&l[i], t)) return i;
    return -1;
}

static inline int bgl_list_equal(const bgl_t *a, const bgl_t *b)
{
    for (int i = 0; i < BGL_SLOTS; i++)
        if (a[i].ep != b[i].ep || a[i].stage != b[i].stage || a[i].block != b[i].block) return 0;
    return 1;
}

static inline void bgl_clear_slot(bgl_t *t) { t->ep = t->stage = t->block = -1; }

/* cur: the engine's list (read); prev: its copy of the previous frame (the engine copies cur -> prev at the start of its update); cur2: what to write back (may equal cur);
 * local: this PC's own need (to send); ask: triples that were merged this call and must be requested from the loader. Returns 1 when cur2 differs from cur. */
static inline int bgl_merge(bgl_state_t *m, const bgl_t cur[BGL_SLOTS], const bgl_t prev[BGL_SLOTS], bgl_t cur2[BGL_SLOTS], bgl_t local[BGL_SLOTS], int *local_n, bgl_t ask[BGL_SLOTS], int *ask_n)
{
    int changed = !bgl_list_equal(cur, prev);   /* a zone hit rebuilt the list this frame: everything in it is the local human's own need */
    for (int i = 0; i < BGL_SLOTS; i++) cur2[i] = cur[i];
    /* 1. the previous call's merged-only entries: still there when nothing changed; drop those the partner no longer asks for */
    if (!changed)
        for (int k = 0; k < m->mo_n; k++) {
            int i = bgl_find(cur2, BGL_SLOTS, &m->mo[k]);
            if (i >= 0 && bgl_find(m->remote, m->remote_n, &m->mo[k]) < 0) {
                bgl_clear_slot(&cur2[i]);
                m->removed++;
            }
        }
    /* 2. the local need = the list without the entries that are merged-only (unless the engine just rebuilt it) */
    int n = 0;
    for (int i = 0; i < BGL_SLOTS; i++) {
        if (!bgl_valid(&cur[i])) continue;
        if (!changed && bgl_find(m->mo, m->mo_n, &cur[i]) >= 0) continue;
        local[n++] = cur[i];
    }
    *local_n = n;
    /* 3. merge the partner's triples into the free slots */
    int mo_n = 0, an = 0;
    bgl_t mo_new[BGL_SLOTS];
    for (int r = 0; r < m->remote_n; r++) {
        const bgl_t *t = &m->remote[r];
        if (!bgl_valid(t) || bgl_find(mo_new, mo_n, t) >= 0) continue;   /* a duplicate inside the partner's list */
        if (bgl_find(cur2, BGL_SLOTS, t) >= 0) {
            if (!changed && bgl_find(m->mo, m->mo_n, t) >= 0) {   /* still merged-only from the last call: keep asking for it */
                mo_new[mo_n++] = *t;
                ask[an++] = *t;
            }
            continue;   /* otherwise the local human needs it too: it is his, not merged */
        }
        int f = -1;
        for (int i = 0; i < BGL_SLOTS; i++)
            if (!bgl_valid(&cur2[i])) { f = i; break; }
        if (f < 0) { m->overflow++; continue; }
        cur2[f] = *t;
        m->merged++;
        mo_new[mo_n++] = *t;
        ask[an++] = *t;
    }
    for (int i = 0; i < mo_n; i++) m->mo[i] = mo_new[i];
    m->mo_n = mo_n;
    *ask_n = an;
    return !bgl_list_equal(cur, cur2);
}

/* the wire form of a list: count, then (ep i16, stage u16, block u16) x count */
static inline int bgl_pack(const bgl_t *l, int n, unsigned char *b)
{
    int o = 0, c = 0;
    for (int i = 0; i < n && c < BGL_SLOTS; i++) {
        if (!bgl_valid(&l[i])) continue;
        b[1 + o] = (unsigned char)(l[i].ep & 0xFF), b[2 + o] = (unsigned char)((l[i].ep >> 8) & 0xFF);
        b[3 + o] = (unsigned char)(l[i].stage & 0xFF), b[4 + o] = (unsigned char)((l[i].stage >> 8) & 0xFF);
        b[5 + o] = (unsigned char)(l[i].block & 0xFF), b[6 + o] = (unsigned char)((l[i].block >> 8) & 0xFF);
        o += 6, c++;
    }
    b[0] = (unsigned char)c;
    return 1 + o;
}

static inline int bgl_unpack(const unsigned char *b, int len, bgl_t *l)
{
    if (len < 1) return -1;
    int c = b[0];
    if (c > BGL_SLOTS || len < 1 + 6 * c) return -1;
    for (int i = 0; i < c; i++) {
        l[i].ep = (short)(b[1 + 6 * i] | (b[2 + 6 * i] << 8));
        l[i].stage = (int)(unsigned)(b[3 + 6 * i] | (b[4 + 6 * i] << 8));
        l[i].block = (int)(unsigned)(b[5 + 6 * i] | (b[6 + 6 * i] << 8));
        if (l[i].stage > 5000 || l[i].block > 63) return -1;
    }
    return c;
}

#endif
