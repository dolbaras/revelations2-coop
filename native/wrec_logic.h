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

#define WREC_ORPHAN_MS 10000u      /* no record of this layout for 10 s ... */
#define WREC_ORPHAN_BATCHES 40u    /* ... while at least 40 batches of the host arrived (every enemy is in one of every ceil(n / 12) batches) */

/* a replica is an orphan (its enemy is gone on the host) */
static int wrec_orphan(unsigned now, unsigned last_seen, unsigned batches_since)
{
    return (int)(now - last_seen) >= (int)WREC_ORPHAN_MS && batches_since >= WREC_ORPHAN_BATCHES;
}

#endif
