/* invmirror_logic.h: the partner's inventory copy follows its owner, the partner's throw is shown on this PC, the puppet's hand follows its owner (combat4, notes\combat4.md).
 * Pure logic, tested offline by tests/test_combat4.c; the game side is story.inc (inventory, GUI hand-over, hand) and world.inc (the throw).
 *
 * The personal inventory (static, the memory image 2026-11-08): sInventory [0x157D3F4] +0x418 = per slot cPersonalInventory (FUN_009943C0); entries at +0xC .. +0x94
 * (0x22 = 34 pointers: 0-19 the bag, 20-23 the weapons, 24-33 knife / light / key items), each a 0x38-byte entry (ctor FUN_00991280): +4 item id, +0xC count (a gun:
 * the loaded rounds; ids 0x102 0x136 0x13B 0x158 have no count, thunk FUN_0065BE10), +0x10 the stack limit, +0x18 a parameter (0x601 / 0x602 records), +0x30 the item
 * object of a weapon. Writers: PutItem 0x6ECE30 -> FUN_009934B0 (stack, new entries), FUN_009938F0 (a copy of an entry: the inventory GUI's hand-over), FUN_00993BC0
 * (remove an entry), FUN_00995170 (remove n of an id: reload, crafting), FUN_007151A0 (consume one: the end of a throw), FUN_008F70D0 (the GUI's use), and direct
 * count writes (the weapon fire, the GUI). The save keeps id, count and +0x18 per entry (FUN_00446280, the record of FUN_00975A30).
 *
 * Before combat4 only pickups (PICKUP) and unique grants (GRANT) crossed: nothing a human consumed (a thrown bottle, rounds, a herb, a used key, crafting) and nothing the
 * inventory GUI handed to the partner reached the other PC. The host's copy of a client's Moira went 0147 x1 -> x2 after two pickups and two throws (combat3 run b), and
 * every PC writes its OWN copy of the partner into its checkpoint save (docs\world.md "Checkpoints"), so a retry or a later solo game started from it.
 *
 * The rule: each PC is authoritative over its OWN human's inventory. The owner sends its entries (index, id, count, +0x18) when they change and a full set on request;
 * the other PC keeps the table and reconciles its copy of that slot to it (im_plan). An item this PC itself put into the partner's copy a moment ago (its own story
 * grant, a PICKUP replay, a hand-over in this PC's GUI) is left alone for IM_GRACE_MS, until the owner's table shows it. A change the inventory GUI of THIS PC's human
 * makes to the partner's copy (the stock hand-over "give to Moira", a pull) goes to the owner as an adjustment (im_gui_delta) and is applied there to the real items. */
#ifndef INVMIRROR_LOGIC_H
#define INVMIRROR_LOGIC_H
#include <stdint.h>
#include <string.h>

#define IM_N 34u                 /* entries of a cPersonalInventory (FUN_00994350 returns 0x22) */
#define IM_GRACE_MS 3000u        /* an id this PC changed in the partner's copy itself is not reconciled for this long */
#define IM_EV_MAX 96u            /* CP_EVENT_MAX */
#define IM_ENT_BYTES 11u         /* u8 index, u16 id, i32 count, u32 +0x18 */
#define IM_EV_ENTS 8u            /* (96 - 4) / 11 */
#define IM_F_BEGIN 1u            /* first event of a full set: the table is cleared first */
#define IM_F_END 2u              /* last event of a full set: the table is complete */

typedef struct { uint16_t id; int32_t count; uint32_t p18; int32_t max; } im_ent;   /* max: the entry's stack limit +0x10 (this PC's copy only; 0 = not known) */
typedef struct { im_ent e[IM_N]; } im_inv;

static inline int im_infinite(uint32_t id) { return id == 0x102u || id == 0x136u || id == 0x13Bu || id == 0x158u; }
static inline int im_is_weapon(uint32_t id) { return ((id >> 8) & 0xFFu) == 1u; }
static inline int im_is_throwable(uint32_t id) { return id >= 0x144u && id <= 0x149u; }   /* FUN_006F1860's list: 0x147 0x148 0x146 0x144 0x149 0x145 */
static inline int32_t im_units(const im_ent *e) { return !e->id ? 0 : im_infinite(e->id) ? 1 : e->count; }
static inline int32_t im_total(const im_inv *v, uint16_t id)
{
    int32_t t = 0;
    for (unsigned i = 0; i < IM_N; i++)
        if (v->e[i].id == id) t += im_units(&v->e[i]);
    return t;
}
static inline int im_has(const im_inv *v, uint16_t id)
{
    for (unsigned i = 0; i < IM_N; i++)
        if (v->e[i].id == id) return 1;
    return 0;
}
static inline int im_same(const im_ent *a, const im_ent *b)
{
    if (a->id != b->id) return 0;
    if (!a->id) return 1;
    return (im_infinite(a->id) || a->count == b->count) && a->p18 == b->p18;
}

/* ---- the owner: which entries changed since the last send ---- */
static inline int im_changed(const im_inv *sent, const im_inv *cur, uint8_t *idx)
{
    int n = 0;
    for (unsigned i = 0; i < IM_N; i++)
        if (!im_same(&sent->e[i], &cur->e[i])) idx[n++] = (uint8_t)i;
    return n;
}
/* combat4 follow-up: Moira's flashlight (0155) keeps its battery in the entry count; it drains / recharges every 100 ms while she shines (run combat4-b: ~10 INV
 * events a second). A change that is ONLY a battery count moving by less than IM_BATT_BIG goes out at most every IM_BATT_MS; any other change (another item, a new
 * or removed entry, a big battery step) goes out at once and carries the battery with it. The pending battery change is not lost: `sent` keeps the old value. */
#define IM_BATT_MS 1000u
#define IM_BATT_BIG 20
static inline int im_is_battery(uint32_t id) { return id == 0x155u; }
static inline int im_send_now(const im_inv *sent, const im_inv *cur, const uint8_t *idx, int n, uint32_t now, uint32_t last_batt)
{
    if (n <= 0) return 0;
#ifdef IM_MODEL_BEFORE_THROTTLE
    (void)sent, (void)cur, (void)idx, (void)now, (void)last_batt;
    return 1;   /* rev2-int 3e55c72: every change at once */
#else
    for (int k = 0; k < n; k++) {
        const im_ent *e = &cur->e[idx[k] % IM_N], *s = &sent->e[idx[k] % IM_N];
        if (!e->id || e->id != s->id || !im_is_battery(e->id) || e->p18 != s->p18) return 1;
        int32_t d = e->count - s->count;
        if (d >= IM_BATT_BIG || -d >= IM_BATT_BIG) return 1;
    }
    return !last_batt || now - last_batt >= IM_BATT_MS;
#endif
}
static inline int im_has_battery(const im_inv *cur, const uint8_t *idx, int n)
{
    for (int k = 0; k < n; k++)
        if (im_is_battery(cur->e[idx[k] % IM_N].id)) return 1;
    return 0;
}

/* one INV event: [0] slot, [1] flags, [2] seq, [3] n, then n entries; returns the length */
static inline unsigned im_encode(uint8_t *b, int slot, unsigned flags, uint8_t seq, const im_inv *cur, const uint8_t *idx, unsigned n)
{
    if (n > IM_EV_ENTS) n = IM_EV_ENTS;
    b[0] = (uint8_t)slot, b[1] = (uint8_t)flags, b[2] = seq, b[3] = (uint8_t)n;
    for (unsigned k = 0; k < n; k++) {
        const im_ent *e = &cur->e[idx[k] % IM_N];
        uint8_t *p = b + 4 + IM_ENT_BYTES * k;
        p[0] = idx[k];
        memcpy(p + 1, &e->id, 2), memcpy(p + 3, &e->count, 4), memcpy(p + 7, &e->p18, 4);
    }
    return 4 + IM_ENT_BYTES * n;
}
/* the indices of a full set: every non-empty entry (the empty ones are implied by IM_F_BEGIN) */
static inline int im_full_list(const im_inv *cur, uint8_t *idx)
{
    int n = 0;
    for (unsigned i = 0; i < IM_N; i++)
        if (cur->e[i].id) idx[n++] = (uint8_t)i;
    return n;
}

/* ---- the other PC: the owner's table ---- */
typedef struct { im_inv inv; uint8_t have, building; uint32_t n_ev, n_full, n_dropped, n_bad; } im_table;
static inline void im_table_reset(im_table *t) { memset(t, 0, sizeof *t); }
static inline int im_table_apply(im_table *t, const uint8_t *b, unsigned len)
{
    if (len < 4) return t->n_bad++, 0;
    unsigned flags = b[1], n = b[3];
    if (n > IM_EV_ENTS || len < 4 + IM_ENT_BYTES * n) return t->n_bad++, 0;
    if (flags & IM_F_BEGIN) memset(&t->inv, 0, sizeof t->inv), t->building = 1, t->have = 0;
    else if (!t->have && !t->building) return t->n_dropped++, 0;   /* a change before any full set: its base is not known here (a full set is requested) */
    for (unsigned k = 0; k < n; k++) {
        const uint8_t *p = b + 4 + IM_ENT_BYTES * k;
        if (p[0] >= IM_N) { t->n_bad++; continue; }
        im_ent *e = &t->inv.e[p[0]];
        memcpy(&e->id, p + 1, 2), memcpy(&e->count, p + 3, 4), memcpy(&e->p18, p + 7, 4);
        e->max = 0;
        if (!e->id) e->count = 0, e->p18 = 0;
    }
    if (flags & IM_F_END) t->building = 0, t->have = 1, t->n_full++;
    t->n_ev++;
    return 1;
}

/* ---- ids this PC changed in the partner's copy itself (story grant, PICKUP replay, its own GUI hand-over): not reconciled for IM_GRACE_MS ---- */
typedef struct { uint16_t id; uint32_t t; } im_grace_t;
typedef struct { im_grace_t g[16]; } im_grace;
static inline void im_grace_note(im_grace *g, uint16_t id, uint32_t now)
{
    int free_i = -1, old_i = 0;
    for (int i = 0; i < 16; i++) {
        if (g->g[i].id == id) { g->g[i].t = now ? now : 1; return; }
        if (!g->g[i].id && free_i < 0) free_i = i;
        if (g->g[i].t < g->g[old_i].t) old_i = i;
    }
    int k = free_i >= 0 ? free_i : old_i;
    g->g[k].id = id, g->g[k].t = now ? now : 1;
}
static inline int im_grace_on(const im_grace *g, uint16_t id, uint32_t now)
{
    for (int i = 0; i < 16; i++)
        if (g->g[i].id == id && g->g[i].t && now - g->g[i].t < IM_GRACE_MS) return 1;
    return 0;
}

/* ---- the reconcile plan: this PC's copy `loc` of the partner's slot to the owner's table `own` ----
 * SET   entry idx count = n (a stack shrinks / grows, a gun's loaded rounds)
 * ADD   PutItem(id, n, p18) (n < 0: the game's default count): an item the copy lacks
 * DEL   remove entry idx (FUN_00993BC0, as the GUI's use does): a stack of a non-weapon used up
 * USE1  the stock consume of one (FUN_007151A0, the end of a throw): the last throwable of a stack, so the puppet's sub-weapon pointer is refreshed the stock way
 * KEEPW a weapon the owner no longer has: weapons are never removed by the mirror (their object may be in the puppet's hand); logged only */
enum { IM_OP_SET = 1, IM_OP_ADD, IM_OP_DEL, IM_OP_USE1, IM_OP_KEEPW };
typedef struct { uint8_t op, idx; uint16_t id; int32_t n; uint32_t p18; } im_op;

static inline int im_push(im_op *ops, int max, int *k, unsigned op, unsigned idx, unsigned id, int32_t n, uint32_t p18)
{
    if (*k >= max) return 0;
    ops[*k].op = (uint8_t)op, ops[*k].idx = (uint8_t)idx, ops[*k].id = (uint16_t)id, ops[*k].n = n, ops[*k].p18 = p18;
    (*k)++;
    return 1;
}
static inline uint32_t im_p18_of(const im_inv *v, uint16_t id)
{
    for (unsigned i = 0; i < IM_N; i++)
        if (v->e[i].id == id) return v->e[i].p18;
    return 0;
}

static inline int im_plan(const im_inv *own, const im_inv *loc, const im_grace *g, uint32_t now, im_op *ops, int max)
{
#ifdef IM_MODEL_BEFORE_C4
    (void)own, (void)loc, (void)g, (void)now, (void)ops, (void)max;
    return 0;   /* rev2-int 7f75163: no mirror, only PICKUP / GRANT cross */
#else
    uint16_t ids[2 * IM_N];
    int nid = 0, k = 0;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned i = 0; i < IM_N; i++) {
            uint16_t id = (s ? loc : own)->e[i].id;
            int dup = 0;
            for (int j = 0; j < nid && !dup; j++) dup = ids[j] == id;
            if (id && !dup) ids[nid++] = id;
        }
    for (int j = 0; j < nid; j++) {
        uint16_t id = ids[j];
        if (g && im_grace_on(g, id, now)) continue;
        int32_t to = im_total(own, id), tl = im_total(loc, id);
        int oh = im_has(own, id), lh = im_has(loc, id), wpn = im_is_weapon(id), thr = im_is_throwable(id);
        if (im_infinite(id)) {   /* presence only */
            if (oh && !lh) im_push(ops, max, &k, IM_OP_ADD, 0, id, -1, im_p18_of(own, id));
            else if (!oh && lh) im_push(ops, max, &k, wpn ? IM_OP_KEEPW : IM_OP_DEL, 0, id, 0, 0);
            if (!oh && lh && !wpn)
                for (unsigned i = 0; i < IM_N; i++)
                    if (loc->e[i].id == id) ops[k - 1].idx = (uint8_t)i;
            continue;
        }
        if (to == tl && (oh == lh || !wpn || thr)) continue;
        if (tl > to || (lh && !oh)) {   /* the copy has more: shrink from the last stack */
            int32_t d = tl - to;
            if (!oh && wpn && !thr) {   /* a weapon the owner does not hold at all */
                im_push(ops, max, &k, IM_OP_KEEPW, 0, id, d, 0);
                continue;
            }
            for (int i = (int)IM_N - 1; i >= 0 && d > 0; i--) {
                const im_ent *e = &loc->e[i];
                if (e->id != id) continue;
                int32_t c = e->count, take = c < d ? c : d;
                if (take <= 0) continue;
                if (c - take >= 1 || (wpn && !thr)) {   /* a gun keeps its entry with 0 rounds */
                    im_push(ops, max, &k, IM_OP_SET, (unsigned)i, id, c - take, e->p18);
                } else if (thr) {   /* the last of a throwable stack: down to 1, then the stock consume */
                    if (c > 1) im_push(ops, max, &k, IM_OP_SET, (unsigned)i, id, 1, e->p18);
                    im_push(ops, max, &k, IM_OP_USE1, (unsigned)i, id, 1, e->p18);
                } else {
                    im_push(ops, max, &k, IM_OP_DEL, (unsigned)i, id, c, e->p18);
                }
                d -= take;
            }
            continue;
        }
        /* the copy has less */
        int32_t d = to - tl;
        if (wpn && !thr) {
            int done = 0;
            for (unsigned i = 0; i < IM_N && !done; i++)
                if (loc->e[i].id == id) im_push(ops, max, &k, IM_OP_SET, i, id, loc->e[i].count + d, loc->e[i].p18), done = 1;
            if (!done) im_push(ops, max, &k, IM_OP_ADD, 0, id, to, im_p18_of(own, id));
            continue;
        }
        for (unsigned i = 0; i < IM_N && d > 0; i++) {
            const im_ent *e = &loc->e[i];
            if (e->id != id) continue;
            int32_t room = e->max > 0 ? e->max - e->count : d;
            if (room <= 0) continue;
            int32_t put = room < d ? room : d;
            im_push(ops, max, &k, IM_OP_SET, i, id, e->count + put, e->p18);
            d -= put;
        }
        if (d > 0) im_push(ops, max, &k, IM_OP_ADD, 0, id, d, im_p18_of(own, id));
    }
    return k;
#endif
}

/* apply a plan to a model inventory (the offline tests; the game side does the same with the game's functions) */
static inline int im_model_put(im_inv *v, uint16_t id, int32_t n, uint32_t p18, int32_t stack)
{
    if (n < 0) n = 1;
    for (unsigned i = 0; i < IM_N && n > 0 && !im_infinite(id); i++)
        if (v->e[i].id == id && (stack <= 0 || v->e[i].count < stack)) {
            int32_t room = stack > 0 ? stack - v->e[i].count : n, put = room < n ? room : n;
            v->e[i].count += put, n -= put;
        }
    while (n > 0 || (im_infinite(id) && !im_has(v, id))) {
        unsigned i = 0;
        while (i < IM_N && v->e[i].id) i++;
        if (i == IM_N) return 0;
        int32_t put = stack > 0 && n > stack ? stack : n;
        v->e[i].id = id, v->e[i].count = im_infinite(id) ? -1 : put, v->e[i].p18 = p18, v->e[i].max = stack;
        n -= put;
        if (im_infinite(id)) break;
    }
    return 1;
}
static inline void im_model_apply(im_inv *v, const im_op *ops, int n, int32_t stack)
{
    for (int i = 0; i < n; i++) {
        const im_op *o = &ops[i];
        im_ent *e = &v->e[o->idx % IM_N];
        switch (o->op) {
        case IM_OP_SET:
            if (e->id == o->id) e->count = o->n;
            break;
        case IM_OP_ADD:
            im_model_put(v, o->id, o->n, o->p18, stack);
            break;
        case IM_OP_DEL:
            if (e->id == o->id) memset(e, 0, sizeof *e);
            break;
        case IM_OP_USE1: {   /* FUN_007151A0: the stack with the lowest count, -1, removed below 1 */
            int best = -1;
            for (unsigned j = 0; j < IM_N; j++)
                if (v->e[j].id == o->id && (best < 0 || v->e[j].count < v->e[best].count)) best = (int)j;
            if (best >= 0 && --v->e[best].count < 1) memset(&v->e[best], 0, sizeof v->e[best]);
            break;
        }
        default:
            break;
        }
    }
}

/* ---- the inventory GUI of this PC's human changed the partner's copy (a hand-over / a pull): per id, what the owner has to add (n > 0) or take (n < 0) ---- */
static inline int im_gui_delta(const im_inv *before, const im_inv *after, im_op *adj, int max)
{
#ifdef IM_MODEL_BEFORE_C4
    (void)before, (void)after, (void)adj, (void)max;
    return 0;   /* 7f75163: nothing of the GUI's hand-over crossed */
#endif
    int k = 0;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned i = 0; i < IM_N; i++) {
            uint16_t id = (s ? before : after)->e[i].id;
            if (!id) continue;
            int seen = 0;
            for (int j = 0; j < k && !seen; j++) seen = adj[j].id == id;
            if (seen) continue;
            int32_t d = im_total(after, id) - im_total(before, id);
            if (d) im_push(adj, max, &k, IM_OP_ADD, 0, id, d, im_has(after, id) ? im_p18_of(after, id) : im_p18_of(before, id));
        }
    return k;
}
/* flashlive (CH1, pair run flashlive-a 2026-11-08 22:50, noteslashlive.md): host Claire passed a bottle to Moira in the REAL inventory screen (T, cursor, C):
 * her count 2 -> 1, the host's copy of Moira +1, and 130 ms later the mirror took it back (`USE1 item 0147 ... owner's total 0`): the bottle vanished. The pass runs
 * in one of the screen's state handlers (code at 0x8F51xx, reached outside uGUIInventoryCampaign vf5 0x8FA590: the hook saw 2 calls for the whole open / pass /
 * close), so the vf5 bracket never saw the difference. im_pass_delta is the per-frame watcher's rule: between two frames (the copy re-read after each reconcile, so
 * the mirror's own operations never count), an id that LEFT this PC's human and ARRIVED in its copy of the partner (a pass), or the other way round (a pull), is a
 * hand-over of min(both) units: an ADJ for the owner. Anything else (a throw, a pickup, the mirror) changes only one side. -DIM_MODEL_BEFORE_FL = rev2-int 6b40a90. */
static inline int im_pass_delta(const im_inv *mine0, const im_inv *mine1, const im_inv *part0, const im_inv *part1, im_op *adj, int max)
{
#ifdef IM_MODEL_BEFORE_FL
    (void)mine0, (void)mine1, (void)part0, (void)part1, (void)adj, (void)max;
    return 0;
#else
    const im_inv *all[4] = {mine0, mine1, part0, part1};
    int k = 0;
    for (unsigned s = 0; s < 4; s++)
        for (unsigned i = 0; i < IM_N; i++) {
            uint16_t id = all[s]->e[i].id;
            if (!id || im_infinite(id)) continue;
            int seen = 0;
            for (int j = 0; j < k && !seen; j++) seen = adj[j].id == id;
            if (seen) continue;
            int32_t dm = im_total(mine1, id) - im_total(mine0, id), dp = im_total(part1, id) - im_total(part0, id), n = 0;
            if (dm < 0 && dp > 0) n = -dm < dp ? -dm : dp;          /* a pass: mine -> the partner */
            else if (dm > 0 && dp < 0) n = -(dm < -dp ? dm : -dp);   /* a pull: the partner -> mine */
            if (n) im_push(adj, max, &k, IM_OP_ADD, 0, id, n, im_has(part1, id) ? im_p18_of(part1, id) : im_p18_of(part0, id));
        }
    return k;
#endif
}
/* one ADJ event: [0] the target slot (the owner), [1] n, then n x {u16 id, i32 delta, u32 +0x18} */
static inline unsigned im_adj_encode(uint8_t *b, int slot, const im_op *adj, unsigned n)
{
    if (n > 9) n = 9;
    b[0] = (uint8_t)slot, b[1] = (uint8_t)n;
    for (unsigned k = 0; k < n; k++) memcpy(b + 2 + 10 * k, &adj[k].id, 2), memcpy(b + 4 + 10 * k, &adj[k].n, 4), memcpy(b + 8 + 10 * k, &adj[k].p18, 4);
    return 2 + 10 * n;
}
static inline int im_adj_decode(const uint8_t *b, unsigned len, int *slot, im_op *adj, int max)
{
    if (len < 2) return -1;
    unsigned n = b[1];
    if (n > 9 || (int)n > max || len < 2 + 10 * n) return -1;
    *slot = b[0];
    for (unsigned k = 0; k < n; k++) {
        memset(&adj[k], 0, sizeof adj[k]);
        adj[k].op = IM_OP_ADD;
        memcpy(&adj[k].id, b + 2 + 10 * k, 2), memcpy(&adj[k].n, b + 4 + 10 * k, 4), memcpy(&adj[k].p18, b + 8 + 10 * k, 4);
    }
    return (int)n;
}

/* ---- the puppet's hand (story.inc w_apply_equipment): the owner's hand slot (FUN_006FB060: 0 none / the default item +0x7340, 1..3 weapon slots, 4 the light)
 * against the puppet's. combat3 set the hand only for owner slots 1..3 and never cleared it: after a throw the puppet kept the bottle in its hand. */
enum { EQ_HAND_KEEP = 0, EQ_HAND_SLOT, EQ_HAND_LIGHT, EQ_HAND_DEFAULT };
static inline int eq_hand_plan(unsigned owner_hand, const uint16_t owner_items[3], unsigned puppet_hand)
{
    if (owner_hand >= 1 && owner_hand <= 3) return owner_items[owner_hand - 1] && puppet_hand != owner_hand ? EQ_HAND_SLOT : EQ_HAND_KEEP;
#ifdef EQ_MODEL_BEFORE_C4
    return EQ_HAND_KEEP;
#else
    if (owner_hand == 4) return puppet_hand != 4 ? EQ_HAND_LIGHT : EQ_HAND_KEEP;           /* FUN_00710360(p, x, 4, 1): the light into the hand */
    if (owner_hand == 0) return puppet_hand >= 1 && puppet_hand <= 3 ? EQ_HAND_DEFAULT : EQ_HAND_KEEP;   /* FUN_00710360(p, -1, 0, 1): the default item +0x7340 */
    return EQ_HAND_KEEP;
#endif
}

/* ---- an attack object a human holds (combat4): the weapon pointers player +0x7340 .. +0x73E0 step 0x10 (the default item +0x7340, the hand +0x7380, the weapon slots
 * +0x73A0 / +0x73B0 / +0x73C0, the light, ...). A weapon's own collision may name the weapon object as the DI attacker instead of the player (its owner is +0x43E8, not
 * the thrown object's +0x19C0): MHIT (mh_take) and P2 (gp_puppet_blow_drop) look one level up through +0x19C0 only, so such a blow of the client counted on neither
 * side the same way: the client ignored it, the host applied the puppet's own copy. With this both sides treat it as the human's (one damage, from the owner). */
#define HW_N 11
#define HW_FIRST 0x7340u
static inline int hw_held(uint32_t att, const uint32_t held[HW_N])
{
    if (!att) return 0;
    for (int i = 0; i < HW_N; i++)
        if (held[i] == att) return 1;
    return 0;
}

/* ---- the partner's throw (world.inc): FUN_009F7C70 (thiscall, 6 stack args: player, projectile, start, velocity, two more vectors) runs on the thrower's PC right after
 * the stock launch (projectile vtable +0x210) in cPlAction_pl_act_weapon_fire state 3 (and FUN_006DB230). A type-2 puppet stops in state 3 before the launch
 * (player vtable +0xE0 = 0), so the other PC never saw the throw. The Raid receiver (FUN_009DF640, message kind 8) creates the same uWpBomb from these vectors:
 * resource FUN_00B5B4C0((id | 0x70000) << 8), object sItem->FUN_0063CC80(res, 0xd, 0, 1, 0, 0), owner +0x19C0 = the thrower's player, launch vtable +0x210.
 * Event body: [0] slot, [1] 0, [2..3] item id, then 4 x 3 floats (48 bytes). */
#define TH_LEN 52u
#define TH_ID_SPECIAL 0x702u   /* the Raid receiver's other branch (not a uWpBomb of the thrower): not mirrored */
static inline unsigned th_encode(uint8_t *b, int slot, uint16_t id, const float v[12])
{
    b[0] = (uint8_t)slot, b[1] = 0;
    memcpy(b + 2, &id, 2), memcpy(b + 4, v, 48);
    return TH_LEN;
}
static inline int th_decode(const uint8_t *b, unsigned len, int *slot, uint16_t *id, float v[12])
{
    if (len < TH_LEN) return 0;
    *slot = b[0];
    memcpy(id, b + 2, 2), memcpy(v, b + 4, 48);
    for (int i = 0; i < 12; i++)
        if (!(v[i] == v[i]) || v[i] > 1e7f || v[i] < -1e7f) return 0;   /* NaN / garbage */
    return 1;
}
/* the receiving PC spawns it for the thrower's puppet: only the partner's own throw (not this PC's human, not an echo), not the special branch */
static inline int th_accept(int slot, int my_slot, int rem_slot, uint16_t id)
{
#ifdef TH_MODEL_BEFORE_C4
    (void)slot, (void)my_slot, (void)rem_slot, (void)id;
    return 0;   /* 7f75163: no throw event, the puppet's stock throw stops before the launch */
#else
    return slot >= 0 && slot != my_slot && slot == rem_slot && id && id != TH_ID_SPECIAL && ((id >> 8) & 0xFFu) == 1u;
#endif
}

/* ---- bomb (2026-11-09, notes\bomb.md): ONE damage path for a thrown bomb (the Raid model).
 * FACT (static): uWpBomb_vf134 (0x673CA0, the flight update) starts the impact explosion of a throwable (ids 0x144..0x149, 0x160, 0x161: action 8 through the bomb's
 * action controller, the object at bomb +0x1A38, its vtable +0x14 (8, 0, 0, -1, 0)) only when FUN_00673000 says the owner +0x19C0 is a LOCAL player (owner vtable
 * +0xE0); then it calls the Raid sender FUN_009F6EF0(owner, bomb) (message 0x21: the net id +0x3788, the item id, the position +0xE0). The Raid receiver FUN_009E07E0
 * finds the remote copy by that net id, puts it at that position (+0x40) and starts the same action 8 (0x145: FUN_00674C70 + FUN_00642720 + vtable +0x3C instead).
 * So the copy that THROW creates for the puppet (owner = a type-2 player: +0xE0 = 0) never exploded: the damage of flashlive run c (and run a's kill with killer slot
 * 1, which is MHIT's attacker) came through MHIT from the thrower's real explosion; "no MHIT, no puppet blow dropped" was the log cap (60 MHIT lines spent on the
 * light's damage-0 hits 11 minutes before the throw).
 * The path, enforced: the thrower's own explosion deals the damage (on the client its replicas send MHIT, on the host the stock hit); the copy explodes for the eye
 * only (EV_W_BOOM at the thrower's explosion point, as message 0x21 does) and every blow of a copy is dropped on the host. LIVE (run a2, 2026-11-09): the explosion's
 * DIs name the bomb's OWNER as attacker (the puppet for the copy, the client's human for the real bomb), so P2's `attacker == puppet` rule drops the copy's blows;
 * this table of copies is the second line for a DI that names the copy object itself (or its +0x19C0, which uWeapon_vf35 can clear). The thrower keeps a table
 * of its own projectiles to name the throw in BOOM. */
#define TB_N 8u
#define TB_MAX_MS 20000u   /* an entry is forgotten after 20 s (a bottle explodes or ends within a few seconds) */
#define TB_BOOM_LEN 16u    /* EV_W_BOOM: [0] thrower slot, [1] throw seq, [2..3] item id, [4..15] the explosion point (3 floats) */
typedef struct { uint32_t obj, t; uint16_t id; uint8_t slot, seq, used, boomed; } tb_ent_t;
typedef struct { tb_ent_t e[TB_N]; uint8_t last_seq; uint32_t n_add, n_boom; } tb_tab_t;

/* the next throw number of this PC (1..255; 0 = an older peer that sent none) */
static inline uint8_t tb_next_seq(tb_tab_t *tb)
{
    tb->last_seq = (uint8_t)(tb->last_seq + 1u);
    if (!tb->last_seq) tb->last_seq = 1;
    return tb->last_seq;
}
static inline int tb_live(const tb_ent_t *e, uint32_t now) { return e->used && (uint32_t)(now - e->t) < TB_MAX_MS; }
static inline void tb_add(tb_tab_t *tb, uint32_t obj, int slot, uint8_t seq, uint16_t id, uint32_t now)
{
    tb_ent_t *dst = &tb->e[0];
    for (unsigned i = 0; i < TB_N; i++) {
        tb_ent_t *e = &tb->e[i];
        if (e->used && e->obj == obj) { dst = e; break; }   /* the game reuses a freed object's memory: the new throw replaces the old entry */
        if (!tb_live(e, now)) { dst = e; break; }
        if ((uint32_t)(now - e->t) > (uint32_t)(now - dst->t)) dst = e;
    }
    dst->obj = obj, dst->t = now, dst->id = id, dst->slot = (uint8_t)slot, dst->seq = seq, dst->used = 1, dst->boomed = 0;
    tb->n_add++;
}
static inline tb_ent_t *tb_by_obj(tb_tab_t *tb, uint32_t obj, uint32_t now)
{
    if (!obj) return 0;
    for (unsigned i = 0; i < TB_N; i++)
        if (tb_live(&tb->e[i], now) && tb->e[i].obj == obj) return &tb->e[i];
    return 0;
}
static inline tb_ent_t *tb_by_seq(tb_tab_t *tb, int slot, uint8_t seq, uint32_t now)
{
    tb_ent_t *best = 0;
    for (unsigned i = 0; i < TB_N; i++) {
        tb_ent_t *e = &tb->e[i];
        if (!tb_live(e, now) || e->slot != (uint8_t)slot || e->seq != seq) continue;
        if (!best || (uint32_t)(now - e->t) < (uint32_t)(now - best->t)) best = e;   /* the newest one with that number (seq wraps after 255 throws) */
    }
    return best;
}

static inline unsigned tb_boom_encode(uint8_t *b, int slot, uint8_t seq, uint16_t id, const float pos[3])
{
    b[0] = (uint8_t)slot, b[1] = seq;
    memcpy(b + 2, &id, 2), memcpy(b + 4, pos, 12);
    return TB_BOOM_LEN;
}
static inline int tb_boom_decode(const uint8_t *b, unsigned len, int *slot, uint8_t *seq, uint16_t *id, float pos[3])
{
    if (len < TB_BOOM_LEN) return 0;
    *slot = b[0], *seq = b[1];
    memcpy(id, b + 2, 2), memcpy(pos, b + 4, 12);
    for (int i = 0; i < 3; i++)
        if (!(pos[i] == pos[i]) || pos[i] > 1e7f || pos[i] < -1e7f) return 0;
    return 1;
}
/* the partner's own bomb (not an echo of this PC's), with a throw number, a throwable id as THROW accepts it */
static inline int tb_boom_accept(int slot, int my_slot, int rem_slot, uint8_t seq, uint16_t id)
{
#ifdef BOMB_MODEL_BEFORE
    (void)slot, (void)my_slot, (void)rem_slot, (void)seq, (void)id;
    return 0;   /* b94ff2e: no BOOM, the copy of the partner's bomb never exploded (the stock impact needs a local owner) */
#else
    return seq != 0 && th_accept(slot, my_slot, rem_slot, id);
#endif
}
/* what the receiver does with the copy (FUN_009E07E0's two branches): 1 = position + action 8 (the explosion), 0 = nothing (0x145 takes the stock's other branch,
 * FUN_00674C70 / FUN_00642720 / vtable +0x3C, which is not reproduced: that copy just ends by itself) */
#define TB_ID_OTHER_BRANCH 0x145u
static inline int tb_boom_explode(uint16_t id) { return id != TB_ID_OTHER_BRANCH; }

/* the host's P2 decision for a pending DI on its enemy (before the stock update): the partner's puppet's own blow (combat2: attacker or its +0x19C0 is the puppet), or a
 * blow of a copy of the partner's throw (this table: the copy is the attacker whatever its +0x19C0 says). The real damage of both comes as MHIT. */
static inline int tb_p2_drop(int valid, uint32_t attacker, uint32_t attacker_owner, uint32_t puppet, int mailbox_lab, int attacker_is_copy)
{
#ifndef BOMB_MODEL_BEFORE
    if (valid && attacker && attacker_is_copy) return 1;
#else
    (void)attacker_is_copy;
#endif
    if (!valid || !puppet || mailbox_lab) return 0;   /* gp_puppet_blow_drop (grapple_logic.h), repeated here so that this header stands alone in the tests */
    return attacker == puppet || (attacker && attacker_owner == puppet);
}

/* the kill credit of a DamageInfo (the host's DEATH killer slot and its stat guard): plain_slot = the attacker's slot when the attacker is a human, weapon_slot = the
 * slot of a weapon object's owner +0x19C0 (the host's own bomb), igniter_slot = the same for enemy +0x7378.
 * FACT (static): the hit handler FUN_0061B050 keeps the attacker of a DI with DI+0x28 bit 24 (a burning blow) at enemy +0x7378 and starts the timer +0x7358; the
 * enemy's own update then runs FUN_0061AC70, which builds a NEW DamageInfo per tick (damage +0x7360, attacker 0, DI+0x2C bit 27) and runs the hit handler with it.
 * Such a tick that killed had killer slot -1: no credit for the client whose MHIT lit it, and the host's profile kept the kill. */
#define BO_TICK_BIT 27u   /* DI+0x2C */
static inline int bo_credit_slot(int plain_slot, int weapon_slot, int attacker_null, uint32_t fl2c, int igniter_slot)
{
    if (plain_slot >= 0) return plain_slot;
#ifdef BOMB_MODEL_BEFORE
    (void)weapon_slot, (void)attacker_null, (void)fl2c, (void)igniter_slot;
    return -1;
#else
    if (!attacker_null) return weapon_slot;
    if ((fl2c >> BO_TICK_BIT) & 1u) return igniter_slot;
    return -1;
#endif
}
#endif
