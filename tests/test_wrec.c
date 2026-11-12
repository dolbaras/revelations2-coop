/* test_wrec.c: the window of the host's enemy records and the orphan rule (native/wrec_logic.h). */
#include <stdio.h>
#include <string.h>
#include "../native/wrec_logic.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

int main(void)
{
    unsigned idx[64], rot = 0;
    /* up to 12 live enemies: all of them in every batch, in list order */
    for (unsigned n = 1; n <= 12; n++) {
        rot = 5;
        unsigned c = wrec_window(n, WREC_PER_BATCH, &rot, idx);
        CHECK(c == n);
        for (unsigned k = 0; k < c; k++) CHECK(idx[k] == k);
    }
    CHECK(wrec_window(0, WREC_PER_BATCH, &rot, idx) == 0);
    /* more than 12 (live allepP28: 14 in stage 351; the 201 siege: 16; up to 48): every enemy within ceil(n / 12) batches, no index twice in a batch, every batch full */
    for (unsigned n = 13; n <= 48; n++) {
        unsigned seen[64];
        memset(seen, 0, sizeof seen);
        rot = 0;
        unsigned batches = (n + WREC_PER_BATCH - 1) / WREC_PER_BATCH;
        for (unsigned b = 0; b < batches; b++) {
            unsigned c = wrec_window(n, WREC_PER_BATCH, &rot, idx);
            CHECK(c == WREC_PER_BATCH);
            unsigned dup = 0;
            for (unsigned k = 0; k < c; k++) {
                CHECK(idx[k] < n);
                for (unsigned j = 0; j < k; j++)
                    if (idx[j] == idx[k]) dup = 1;
                seen[idx[k]] = 1;
            }
            CHECK(!dup);
        }
        unsigned all = 1;
        for (unsigned i = 0; i < n; i++)
            if (!seen[i]) all = 0;
        CHECK(all);
    }
    /* the 14 of stage 351: the 13th and 14th (indices 12, 13) are in the second batch */
    rot = 0;
    wrec_window(14, WREC_PER_BATCH, &rot, idx);
    wrec_window(14, WREC_PER_BATCH, &rot, idx);
    int has12 = 0, has13 = 0;
    for (unsigned k = 0; k < WREC_PER_BATCH; k++) has12 |= idx[k] == 12, has13 |= idx[k] == 13;
    CHECK(has12 && has13);
    /* the list shrinks between batches (an enemy died): the window stays inside the list */
    rot = 40;
    unsigned c = wrec_window(14, WREC_PER_BATCH, &rot, idx);
    CHECK(c == WREC_PER_BATCH);
    for (unsigned k = 0; k < c; k++) CHECK(idx[k] < 14);
    /* 0.3.1 R2: one tick of the host. 12 or fewer live enemies: exactly 0.3.0 (one batch, the list in order, the window state reset) */
    unsigned tidx[WREC_PER_BATCH * WREC_MAX_BATCHES], tcnt[WREC_MAX_BATCHES], widx[64];
    for (unsigned n = 1; n <= 12; n++) {
        unsigned r1 = 7, r2 = 7;
        unsigned nb = wrec_tick(n, WREC_PER_BATCH, WREC_MAX_BATCHES, &r1, tidx, tcnt);
        unsigned c = wrec_window(n, WREC_PER_BATCH, &r2, widx);
        CHECK(nb == 1 && tcnt[0] == c && r1 == r2);
        for (unsigned k = 0; k < c; k++) CHECK(tidx[k] == widx[k]);
    }
    CHECK(wrec_tick(0, WREC_PER_BATCH, WREC_MAX_BATCHES, &rot, tidx, tcnt) == 0);
    /* the host's list as the game holds it (n live) cut to the cap; then ticks: which enemies had a record */
    {
        static const unsigned ns[] = {13, 16, 40, 48, 49, 60, 100, 128};
        for (unsigned q = 0; q < sizeof ns / sizeof ns[0]; q++) {
            unsigned n = ns[q], live = n < wrec_live_cap() ? n : wrec_live_cap();
            unsigned seen[WREC_LIVE_MAX];
            memset(seen, 0, sizeof seen);
            unsigned r = 0, ticks = n <= 48 ? 1 : (n + 47) / 48, recs_max = 0;
            for (unsigned t = 0; t < ticks; t++) {
                unsigned nb = wrec_tick(live, WREC_PER_BATCH, WREC_MAX_BATCHES, &r, tidx, tcnt), recs = 0;
                CHECK(nb >= 1 && nb <= WREC_MAX_BATCHES);
                for (unsigned b = 0; b < nb; b++) {
                    CHECK(tcnt[b] >= 1 && tcnt[b] <= WREC_PER_BATCH);
                    for (unsigned k = 0; k < tcnt[b]; k++) { CHECK(tidx[b * WREC_PER_BATCH + k] < live); seen[tidx[b * WREC_PER_BATCH + k]]++; }
                    recs += tcnt[b];
                }
                if (recs > recs_max) recs_max = recs;
            }
            unsigned all = 1;
            for (unsigned i = 0; i < n; i++)
                if (!seen[i]) all = 0;
            CHECK(all);   /* every live enemy had a record: in every tick up to 48 (The Struggle's 40), within ceil(n / 48) ticks above (0.3.0: 12 a tick, none beyond 48) */
            if (n <= 48) for (unsigned i = 0; i < n; i++) CHECK(seen[i] == 1);   /* no enemy twice in a tick */
            CHECK(recs_max <= WREC_PER_BATCH * WREC_MAX_BATCHES);   /* the traffic bound: 48 records a tick */
        }
    }
    CHECK(wrec_live_cap() == WREC_LIVE_MAX);
    /* orphans: only after 10 s AND 40 batches without a record of that layout */
    CHECK(!wrec_orphan(20000, 15000, 100));
    CHECK(!wrec_orphan(30000, 15000, 10));
    CHECK(wrec_orphan(30000, 15000, 40));
    CHECK(wrec_orphan(5000, 0xFFFFE000u, 60));   /* the tick counter wrapped */
    printf("test_wrec: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
