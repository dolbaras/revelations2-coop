/* wrec_logic.h: the pure part of the host's enemy records (world.inc w_send_enemies / w_apply_enemy_record), so tests/test_wrec.c can check it without a game.
 *
 * 1. The window. A WORLD batch holds at most CP_WORLD_MAX (400) bytes = 12 records of 32 bytes. The host used to put the FIRST 12 live enemies of its lists into every batch, so
 *    an enemy beyond the 12th never had a record: no position / health / action updates, and when its one-time SPAWN arrived while the guest was still loading, its replica was
 *    never made (live allepP28, stage 351: 14 enemies on the host, 12 on the guest; the two uEm4400 (351,31,0) / (351,31,1) were missing for good). Now the window turns: with more
 *    than `per` live enemies each batch takes the next `per` of them, so every enemy has a record at least every ceil(n / per) batches.
 * 2. Orphans. A replica of the guest whose layout has had no record for a while although the host's batches keep coming belongs to an enemy the host no longer has (removed by
 *    its story without a death, e.g. an NPC parked at the origin: live allepP27, the guest kept Chavez uEm8800 (201,10,1) that the host had dropped). wrec_orphan() says when.
 * WREC_MODEL_BEFORE_ROT: the logic before the window (tests/build.sh checks that test_wrec fails with it). */
#ifndef WREC_LOGIC_H
#define WREC_LOGIC_H

#define WREC_PER_BATCH 12u

/* the indices (into the host's list of n live enemies) for this batch; *rot is the caller's state (start of the next window). Returns how many were written (<= per). */
static unsigned wrec_window(unsigned n, unsigned per, unsigned *rot, unsigned *idx)
{
    if (!n || !per) return 0;
#ifdef WREC_MODEL_BEFORE_ROT
    (void)rot;
    unsigned c = n < per ? n : per;
    for (unsigned k = 0; k < c; k++) idx[k] = k;
    return c;
#else
    if (n <= per) {
        for (unsigned k = 0; k < n; k++) idx[k] = k;
        *rot = 0;
        return n;
    }
    unsigned start = *rot % n;
    for (unsigned k = 0; k < per; k++) idx[k] = (start + k) % n;
    *rot = (start + per) % n;
    return per;
#endif
}

/* 3. 0.3.1 (R2, notes\fix031-plan.md): more enemies. The host read at most 48 live enemies and sent ONE batch per 66 ms tick: with n live enemies each had a record every
 *    ceil(n / 12) ticks (40 in The Struggle's forest 600: every 264 ms, steps on the guest's PC), and beyond 48 never (their replicas stood still and the orphan rule could kill
 *    them on the guest's PC). Now the list holds up to WREC_LIVE_MAX (the game's two enemy arrays of 64) and a tick sends ceil(n / 12) batches, at most WREC_MAX_BATCHES:
 *    up to 48 live enemies every one has a record every tick, above that the window turns (every ceil(n / 48) ticks). With 12 or fewer the tick is EXACTLY 0.3.0's:
 *    one batch, the list in order. Traffic: one full batch = 12 x 32 = 384 bytes + ~76 bytes of headers (message 16, ENet 16-20, UDP/IP 28; a Steam relay adds a few
 *    dozen) at 15 ticks a second: 0.3.0 at most ~7 KB/s; now 40 enemies = 40 x 32 + 4 x 76 = 1584 bytes a tick = ~24 KB/s, 128 enemies = 48 x 32 + 4 x 76 = 1840 bytes =
 *    ~28 KB/s (about 220 kbit/s), only while more than 12 are alive.
 *    -DWREC_MODEL_BEFORE_031 = the 0.3.0 tick (one batch, 48 at most): tests/build.sh checks that test_wrec fails with it. */
#define WREC_MAX_BATCHES 4u
#define WREC_LIVE_MAX 128u          /* array size of the host's / guest's lists */
static unsigned wrec_live_cap(void)
{
#ifdef WREC_MODEL_BEFORE_031
    return 48u;
#else
    return WREC_LIVE_MAX;
#endif
}
/* the batches of one tick: idx[b * per + k] (k < counts[b]) are the indices of batch b; returns the number of batches */
static unsigned wrec_tick(unsigned n, unsigned per, unsigned maxb, unsigned *rot, unsigned *idx, unsigned *counts)
{
    if (!n || !per || !maxb) return 0;
#ifdef WREC_MODEL_BEFORE_031
    (void)maxb;
    counts[0] = wrec_window(n, per, rot, idx);
    return 1;
#else
    if (n <= per) {
        counts[0] = wrec_window(n, per, rot, idx);   /* 0.3.0 exactly */
        return 1;
    }
    if (n <= per * maxb) {
        unsigned nb = (n + per - 1) / per;
        for (unsigned b = 0; b < nb; b++) {
            unsigned c = n - b * per < per ? n - b * per : per;
            for (unsigned k = 0; k < c; k++) idx[b * per + k] = b * per + k;
            counts[b] = c;
        }
        *rot = 0;
        return nb;
    }
    for (unsigned b = 0; b < maxb; b++) counts[b] = wrec_window(n, per, rot, idx + b * per);
    return maxb;
#endif
}

#define WREC_ORPHAN_MS 10000u      /* no record of this layout for 10 s ... */
#define WREC_ORPHAN_BATCHES 40u    /* ... while at least 40 batches of the host arrived (every enemy is in one of every ceil(n / 12) batches) */

/* a replica is an orphan (its enemy is gone on the host) */
static int wrec_orphan(unsigned now, unsigned last_seen, unsigned batches_since)
{
    return (int)(now - last_seen) >= (int)WREC_ORPHAN_MS && batches_since >= WREC_ORPHAN_BATCHES;
}

#endif
