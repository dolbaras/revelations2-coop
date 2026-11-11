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
    /* orphans: only after 10 s AND 40 batches without a record of that layout */
    CHECK(!wrec_orphan(20000, 15000, 100));
    CHECK(!wrec_orphan(30000, 15000, 10));
    CHECK(wrec_orphan(30000, 15000, 40));
    CHECK(wrec_orphan(5000, 0xFFFFE000u, 60));   /* the tick counter wrapped */
    printf("test_wrec: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
