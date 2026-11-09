/* test_combat2.c: offline checks of the combat2 part of native/grapple_logic.h (notes\combat2.md):
 *   1. the puppet's default animation mask lets the partner's physical attack / follow-up (grapple ids 0x2B..0x36) through, and nothing else new;
 *   2. the host drops a pending DI that the partner's puppet made itself, so a partner's kick counts once (MHIT) whether or not the enemy has +0x6FB8;
 *   3. the lab's game button (FUN_0040AD80 kind -> sPad bit -> XInput) gives back the sPad bit through the game's XInput mapping (FUN_00C11E20);
 *   4. the stock player rule (FUN_00703300) as far as the Follow-Up gate and the weapon kind go: the m1 / m2 refusals and the lab unlock.
 * Built a second time with -DGP_MODEL_BEFORE_PUPPET (rev2-int 5a0af8d: mask without the grapple ids, no host drop): that model must fail (tests/build.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/grapple_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; if (g_fail < 40) printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned g_rng = 777;
static unsigned rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

/* 1. the mask: the old ids stay, the grapple ids 43..54 come in (combat3 adds more ids on top: tests/test_combat3.c checks the whole set; the other ids of word 1
 * that combat3 lets through are 32, 33, 39, 40, 41) */
static void test_mask(void)
{
    uint32_t bits[8];
    gp_puppet_anim_defaults(bits);
    for (uint32_t id = 0; id < 300; id++) {
        int old = id < 32 && ((0x0062207Cu >> id) & 1u);
        int grap = id >= 0x2B && id <= 0x36;
        if (old || grap) CHECK(gp_puppet_anim_allowed(bits, id));
    }
    CHECK((bits[1] & 0x007FF800u) == 0x007FF800u);
    CHECK(gp_puppet_anim_allowed(bits, GP_ACT_KICK) && gp_puppet_anim_allowed(bits, GP_ACT_FOLLOWUP));
    CHECK(!gp_puppet_anim_allowed(bits, 0x80000000u));   /* the scripted INT_MIN action is never forced */
}

/* 1b. the host's puppet over a grapple of its owner: the owner's samples at 20 Hz (idle, kick 43 for ~0.9 s, idle); the puppet is switched (ChangeAction) whenever the
 * sample's action differs from the puppet's and the mask allows it. The kick must be shown (the puppet enters 43), and the puppet must come back to idle. */
static void test_puppet_timeline(void)
{
    uint32_t bits[8];
    gp_puppet_anim_defaults(bits);
    for (int run = 0; run < 2000; run++) {
        int kick_from = 5 + (int)(rnd() % 20), kick_len = 12 + (int)(rnd() % 10), follow = rnd() % 2, loss = (int)(rnd() % 30);
        uint32_t pup = 2;
        int shown_kick = 0, shown_follow = 0;
        for (int s = 0; s < 80; s++) {
            uint32_t own = 2;
            if (s >= kick_from && s < kick_from + kick_len) own = GP_ACT_KICK;
            if (follow && s >= kick_from + kick_len + 4 && s < kick_from + kick_len + 20) own = GP_ACT_FOLLOWUP;
            if ((int)(rnd() % 100) < loss) continue;   /* a lost sample */
            if (own != pup && gp_puppet_anim_allowed(bits, own)) pup = own;
            if (pup == GP_ACT_KICK) shown_kick = 1;
            if (pup == GP_ACT_FOLLOWUP) shown_follow = 1;
        }
        if (loss < 25) CHECK(shown_kick);          /* 12+ samples of the kick, at most 25 % lost: at least one arrives */
        if (follow && loss < 25) CHECK(shown_follow);
        CHECK(pup == 2);
    }
}

/* 2. the host's drop of the puppet's own blows */
static void test_drop(void)
{
    const uint32_t pup = 0x2AAC11C0u, host = 0x2B000060u, brick_of_pup = 0x31000000u, brick_of_host = 0x31100000u;
    CHECK(gp_puppet_blow_drop(1, pup, 0, pup, 0) == 1);                 /* the puppet's kick / shot */
    CHECK(gp_puppet_blow_drop(1, brick_of_pup, pup, pup, 0) == 1);      /* an object the puppet threw (+0x19C0 = the puppet) */
    CHECK(gp_puppet_blow_drop(1, host, 0, pup, 0) == 0);                /* the host's own human */
    CHECK(gp_puppet_blow_drop(1, brick_of_host, host, pup, 0) == 0);    /* the host's thrown brick */
    CHECK(gp_puppet_blow_drop(1, 0, 0, pup, 0) == 0);                   /* no attacker (fall, fire) */
    CHECK(gp_puppet_blow_drop(0, pup, 0, pup, 0) == 0);                 /* nothing pending */
    CHECK(gp_puppet_blow_drop(1, pup, 0, 0, 0) == 0);                   /* no puppet (solo) */
    CHECK(gp_puppet_blow_drop(1, pup, 0, pup, 1) == 0);                 /* `net w mhit mailbox 1` (lab) posts MHIT with the puppet as attacker */
    /* the damage the host's enemy takes for ONE kick of the client: MHIT applies it directly (once); the puppet's local copy of the kick lands in the mailbox, the stock
     * FUN_0060DFD0 refuses a type-2 attacker only while +0x6FB8 is set (melee run M1: 1; the layout / checkpoint spawns set it, other creators may not) */
    for (int f6fb8 = 0; f6fb8 <= 1; f6fb8++) {
        int applied = 1;                                                /* MHIT */
        int pending = 1;                                                /* the puppet's copy */
        if (gp_puppet_blow_drop(pending, pup, 0, pup, 0)) pending = 0;  /* the host's guard, before the stock update */
        if (pending && !f6fb8) applied++;                               /* the stock update takes it */
        CHECK(applied == 1);
    }
}

/* 3. the game's XInput -> sPad mapping (FUN_00C11E20) and the lab's inverse */
static uint32_t game_spad_from_xinput(uint16_t b, uint8_t lt, uint8_t rt)
{
    uint32_t u = 0;
    if (b & 0x10) u = 8;
    if (b & 0x20) u |= 1;
    if (b & 1) u |= 0x10;
    if (b & 2) u |= 0x40;
    if (b & 4) u |= 0x80;
    if (b & 8) u |= 0x20;
    if (b & 0x8000) u |= 0x1000;
    if (b & 0x2000) u |= 0x2000;
    if (b & 0x1000) u |= 0x4000;
    if (b & 0x4000) u |= 0x8000;
    if (b & 0x100) u |= 0x100;
    if (b & 0x200) u |= 0x200;
    if (b & 0x40) u |= 2;
    if (b & 0x80) u |= 4;
    if (lt > 30) u |= 0x400;   /* the threshold byte +0x15 */
    if (rt > 30) u |= 0x800;
    return u;
}
static void test_buttons(void)
{
    CHECK(gp_spad_bit_for_kind(0x29) == 0x200u);   /* pad, control type 0: RB */
    CHECK(gp_spad_bit_for_kind(0x21) == 0x800u);   /* pad, other type: RT */
    CHECK(gp_spad_bit_for_kind(0x1C) == 0x8000u);  /* keyboard */
    CHECK(gp_spad_bit_for_kind(99) == 0);
    gp_xpad_t x;
    CHECK(gp_xinput_for_spad(0x200, &x) && x.buttons == 0x200 && !x.lt && !x.rt);
    CHECK(gp_xinput_for_spad(0x800, &x) && x.buttons == 0 && x.rt == 255);
    CHECK(gp_xinput_for_spad(0x8000, &x) && x.buttons == 0x4000);   /* the X button */
    CHECK(gp_xinput_for_spad(0x4000, &x) && x.buttons == 0x1000);   /* A (m1/m2 pressed XInput A = sPad 0x4000, never the grapple button) */
    CHECK(!gp_xinput_for_spad(0x10, &x));                           /* d-pad: not produced by the lab */
    static const uint32_t bitset[] = {0x100, 0x200, 0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000};
    for (unsigned i = 0; i < 8; i++) {
        CHECK(gp_xinput_for_spad(bitset[i], &x));
        CHECK(game_spad_from_xinput(x.buttons, x.lt, x.rt) == bitset[i]);
    }
    for (uint32_t k = 0; k < 42; k++) {
        uint32_t bit = gp_spad_bit_for_kind(k);
        if (!bit || (bit & ~0xFF00u)) continue;   /* stick directions are not buttons */
        CHECK(gp_xinput_for_spad(bit, &x));
        CHECK(game_spad_from_xinput(x.buttons, x.lt, x.rt) == bit);
    }
}

/* 4. the player rule: Barry (0x15) in the campaign (Area_IsEpisodeArea(-1) = 0 at stage 151, live m1) */
static void test_kind_rule(void)
{
    CHECK(gp_player_kind_ok(0x15, 0, 0, 0, 1, 2, 0) == 1);   /* the physical attack: no skill needed */
    CHECK(gp_player_kind_ok(0x15, 0, 0, 0, 0, 0, 0) == 1);   /* ... and no weapon needed */
    CHECK(gp_player_kind_ok(0x15, 1, 0, 0, 1, 2, 0) == 0);   /* follow-up without Follow-Up: refused (m1 / m2 on both copies) */
    CHECK(gp_player_kind_ok(0x15, 1, 0, 1, 1, 2, 0) == 1);   /* with the skill (lab unlock) and a non-knife weapon */
    CHECK(gp_player_kind_ok(0x15, 1, 0, 1, 1, 1, 0) == 0);   /* with the knife as FUN_006FB200's weapon (kind 1) */
    CHECK(gp_player_kind_ok(0x15, 1, 1, 0, 1, 2, 0) == 1);   /* Raid area: no skill gate */
    CHECK(gp_player_kind_ok(0x16, 1, 0, 0, 1, 2, 0) == 0);   /* Claire the same */
    CHECK(gp_player_kind_ok(0x18, 0, 0, 0, 0, 0, 0) == 0);   /* Natalia: no physical attack of kind 0 */
    CHECK(gp_player_kind_ok(0x18, 3, 0, 0, 0, 0, 0) == 1);
    CHECK(gp_player_kind_ok(0x15, 2, 0, 0, 1, 2, 1) == 0);   /* the surprise attack is not for a type-1 player */
    CHECK(GP_SKILL_BYTE_OFS == 0x20u + 0x7955u + 1u * 8u);
}

int main(void)
{
    test_mask();
    test_puppet_timeline();
    test_drop();
    test_buttons();
    test_kind_rule();
#ifdef GP_MODEL_BEFORE_PUPPET
    printf("test_combat2 (model before the puppet fix): %d checks, %d failed\n", g_checks, g_fail);
#else
    printf("test_combat2: %d checks, %d failed\n", g_checks, g_fail);
#endif
    return g_fail ? 1 : 0;
}
