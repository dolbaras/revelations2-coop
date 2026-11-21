/*
 * gate_logic.h: does the mod run on this rerev2.exe? (0.3.3) Pure logic (no Windows calls), so tests\test_gate_exe.c checks it; native/proxy.c reads the file
 * and the environment and logs the answer.
 *
 * Input: the exe's SHA-256 (lower-case hex, 64 characters), the text of the consent file coop\anyexe.txt (or NULL), REV2_ANYEXE=1 (lab).
 * Order: a build of native/exe_variants.h with level >= 1 -> on (caveat flag from the table); else the consent file naming exactly this SHA-256 -> on, "by consent";
 * else the lab switch -> on; else off (the proxy only forwards DirectInput8Create). Every build of the table has level 1 or 2 (a build that must stay off is simply not in the table).
 * The consent file is written by the installer when the player says "install anyway" for a build that is not in the table; it holds one line
 * `sha256=<64 hex>` (anything else in the file is ignored), so a game update that changes the exe asks again.
 */
#ifndef R2_GATE_LOGIC_H
#define R2_GATE_LOGIC_H

#include <stddef.h>
#include <string.h>

typedef struct { const char *sha, *name, *kind; int level, caveat; } r2_variant_t;
static const r2_variant_t r2_variants[] = {
#define R2_VARIANT(sha, name, kind, level, caveat) { sha, name, kind, level, caveat },
#include "exe_variants.h"
#undef R2_VARIANT
};
#define R2_VARIANT_COUNT ((int)(sizeof r2_variants / sizeof r2_variants[0]))

enum { GATE_OFF = 0, GATE_TABLE = 1, GATE_CONSENT = 2, GATE_LAB = 3 };

typedef struct { int mode; const r2_variant_t *variant; } gate_t;

static const r2_variant_t *gate_find(const char *sha)
{
    if (!sha || !sha[0]) return NULL;
    for (int i = 0; i < R2_VARIANT_COUNT; i++)
        if (strcmp(sha, r2_variants[i].sha) == 0) return &r2_variants[i];
    return NULL;
}

/* 1 when the consent text holds a line sha256=<this hash> (case-insensitive hex, spaces and CR around it ignored). */
static int gate_consent_ok(const char *text, size_t n, const char *sha)
{
    if (!text || !sha || strlen(sha) != 64) return 0;
    for (size_t i = 0; i + 7 + 64 <= n; i++) {
        if (i && text[i - 1] != '\n') continue;
        if (strncmp(text + i, "sha256=", 7) != 0) continue;
        size_t k = 0;
        for (; k < 64; k++) {
            char a = text[i + 7 + k], b = sha[k];
            if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (k == 64) {
            char tail = i + 7 + 64 < n ? text[i + 7 + 64] : '\n';
            if (tail == '\n' || tail == '\r' || tail == ' ' || tail == '\t') return 1;
        }
    }
    return 0;
}

/* The hash a session compares with the partner's ("different game build"): a copy with the Steam wrapper removed ("unwrapped") has the same game code as the Steam
 * build (notes\compat-repack.md), so it carries the hash of the "steam" row and the two can play together; any other exe carries its own hash. */
static const char *gate_build_tag(const char *sha)
{
    const r2_variant_t *v = gate_find(sha);
    if (v && !strcmp(v->kind, "unwrapped"))
        for (int i = 0; i < R2_VARIANT_COUNT; i++)
            if (!strcmp(r2_variants[i].kind, "steam")) return r2_variants[i].sha;
    return sha;
}

static gate_t gate_decide(const char *sha, const char *consent, size_t consent_n, int lab_anyexe)
{
    gate_t g = { GATE_OFF, NULL };
    const r2_variant_t *v = gate_find(sha);
    if (v) {
        g.variant = v;
        g.mode = GATE_TABLE;
        return g;
    }
    if (gate_consent_ok(consent, consent_n, sha)) g.mode = GATE_CONSENT;
    else if (lab_anyexe) g.mode = GATE_LAB;
    return g;
}

#endif
