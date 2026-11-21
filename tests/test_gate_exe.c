/* test_gate_exe.c: the exe gate of 0.3.3 (native/exe_variants.h + native/gate_logic.h): which rerev2.exe runs the mod.
 * The proxy reads the same table and the consent file coop\anyexe.txt; the installer reads the same table (tests\test_installer.ps1 section "variants"). */
#include <stdio.h>
#include <string.h>
#include "../native/gate_logic.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

#define STEAM "55ac7d2bd80704c6a8a1fa7bd2371726adbaa01ae0fda056198fb042404367f9"
#define UNWRAPPED "4c26ba8c7736db96c2c2fc82ac96e8a1a942ae11a4be75e70ec3ef1547bb4133"
#define OTHER "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

static gate_t d(const char *sha, const char *consent, int lab) { return gate_decide(sha, consent, consent ? strlen(consent) : 0, lab); }

int main(void)
{
    /* the table: lower-case SHA-256s, unique names and hashes, levels 0..2, caveat 0/1 */
    for (int i = 0; i < R2_VARIANT_COUNT; i++) {
        const r2_variant_t *v = &r2_variants[i];
        CHECK(strlen(v->sha) == 64);
        int hex = 1;
        for (int k = 0; k < 64; k++) hex &= (v->sha[k] >= '0' && v->sha[k] <= '9') || (v->sha[k] >= 'a' && v->sha[k] <= 'f');
        CHECK(hex);
        CHECK(v->level >= 1 && v->level <= 2 && (v->caveat == 0 || v->caveat == 1));
        CHECK(!strcmp(v->kind, "steam") || !strcmp(v->kind, "unwrapped") || !strcmp(v->kind, "other"));
        for (int j = i + 1; j < R2_VARIANT_COUNT; j++) CHECK(strcmp(v->sha, r2_variants[j].sha) != 0 && strcmp(v->name, r2_variants[j].name) != 0);
    }
    CHECK(R2_VARIANT_COUNT >= 2);

    /* the Steam build: on, no caveat, whatever the consent file says */
    gate_t g = d(STEAM, NULL, 0);
    CHECK(g.mode == GATE_TABLE && g.variant && !g.variant->caveat && g.variant->level == 2 && !strcmp(g.variant->kind, "steam"));
    g = d(STEAM, "sha256=" OTHER "\n", 0);
    CHECK(g.mode == GATE_TABLE);

    /* the copy with the Steam wrapper removed: on, with the caveat */
    g = d(UNWRAPPED, NULL, 0);
    CHECK(g.mode == GATE_TABLE && g.variant && g.variant->caveat && !strcmp(g.variant->kind, "unwrapped"));

    /* an exe that is not in the table: off ... */
    g = d(OTHER, NULL, 0);
    CHECK(g.mode == GATE_OFF && !g.variant);
    g = d("", NULL, 0);                       /* hash unreadable */
    CHECK(g.mode == GATE_OFF);
    g = d(NULL, "sha256=" OTHER "\n", 0);
    CHECK(g.mode == GATE_OFF);                /* no hash: no consent can apply */
    g = d(NULL, NULL, 1);
    CHECK(g.mode == GATE_LAB);                /* (the lab switch never needed the hash) */
    g = d(OTHER, "", 0);
    CHECK(g.mode == GATE_OFF);

    /* ... unless the consent file names exactly that hash */
    g = d(OTHER, "sha256=" OTHER "\n", 0);
    CHECK(g.mode == GATE_CONSENT);
    g = d(OTHER, "sha256=" OTHER, 0);                       /* no newline at the end */
    CHECK(g.mode == GATE_CONSENT);
    g = d(OTHER, "# the player said yes\r\nsha256=" OTHER "\r\n", 0);   /* a comment line first, CRLF */
    CHECK(g.mode == GATE_CONSENT);
    g = d(OTHER, "sha256=0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\n", 0);   /* upper case */
    CHECK(g.mode == GATE_CONSENT);
    g = d(OTHER, "sha256=" STEAM "\n", 0);                  /* another build's hash */
    CHECK(g.mode == GATE_OFF);
    g = d(OTHER, "sha256=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde\n", 0);   /* 63 characters */
    CHECK(g.mode == GATE_OFF);
    g = d(OTHER, "sha256=" OTHER "0\n", 0);                 /* 65 characters */
    CHECK(g.mode == GATE_OFF);
    g = d(OTHER, "x sha256=" OTHER "\n", 0);                /* not at the start of a line */
    CHECK(g.mode == GATE_OFF);
    g = d(OTHER, "sha256 = " OTHER "\n", 0);
    CHECK(g.mode == GATE_OFF);
    g = d(OTHER, "# sha256=" OTHER "\n", 0);                /* commented out */
    CHECK(g.mode == GATE_OFF);

    /* the lab switch (REV2_ANYEXE=1) */
    g = d(OTHER, NULL, 1);
    CHECK(g.mode == GATE_LAB);
    g = d(STEAM, NULL, 1);
    CHECK(g.mode == GATE_TABLE);              /* a known build is a known build */

    /* the build tag a session compares: the unwrapped copy carries the Steam build's hash, everything else its own */
    CHECK(!strcmp(gate_build_tag(UNWRAPPED), STEAM));
    CHECK(!strcmp(gate_build_tag(STEAM), STEAM));
    CHECK(!strcmp(gate_build_tag(OTHER), OTHER));
    CHECK(!strcmp(gate_build_tag(""), ""));

    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
