/*
 * test_lm_glyphs.c <arc folder>: the 0.2.0 End Co-Op row of the host's pause menu is drawn by the GAME's own GUI with its bitmap font (the label and the help line come
 * from our Msg_GetString ids, menu.c h_msg_get): every character of LT_END_COOP / LT_END_COOP_HELP must be a glyph of the face the game uses for that language
 * (Msg_<lang>.arc: sys_eng for the Latin / Cyrillic languages and Japanese, sys_kor, sys_chS, sys_chT). A missing glyph would show as a gap in the game's menu.
 * Needs the game's archives (tests/build.sh passes the folder when it finds a game copy); without them it prints why and passes.
 */
#include <stdio.h>
#include <string.h>

#include "../native/gametex.h"
#include "../native/lobby.h"

static unsigned utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = s[0];
    int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 3;
    if (k) c &= 0x3F >> k;
    for (int i = 1; i <= k; i++) c = (c << 6) | (s[i] & 0x3F);
    *p += k + 1;
    return c;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("test_lm_glyphs: no game folder given: skipped\n");
        return 0;
    }
    /* lobby_lang_names order: en fr it de es ru pl ptB ja ko zhS zhT */
    static const char *const arc_of[12] = {"Msg_eng.arc", "Msg_fre.arc", "Msg_ita.arc", "Msg_ger.arc", "Msg_spa.arc", "Msg_rus.arc", "Msg_pol.arc", "Msg_ptB.arc",
                                           "Msg_jpn.arc", "Msg_kor.arc", "Msg_chS.arc", "Msg_chT.arc"};
    static const char *const face_of[12] = {"sys_eng", "sys_eng", "sys_eng", "sys_eng", "sys_eng", "sys_eng", "sys_eng", "sys_eng", "sys_eng", "sys_kor", "sys_chS", "sys_chT"};
    int checks = 0, fails = 0;
    for (int lang = 0; lang < 12; lang++) {
        char arc[600], gfd[64], pages[96];
        snprintf(arc, sizeof arc, "%s/%s", argv[1], arc_of[lang]);
        snprintf(gfd, sizeof gfd, "ui_N\\00_font\\%s", face_of[lang]);
        snprintf(pages, sizeof pages, "ui_N\\00_font\\%s_%%02d_AM_NOMIP", face_of[lang]);
        gt_font *f = gt_font_open(arc, gfd, pages);
        checks++;
        if (!f) {
            printf("FAIL: %s %s: font not readable\n", lobby_lang_names[lang], arc);
            fails++;
            continue;
        }
        checks++;
        if (!gt_font_find(f, 'A') || gt_font_find(f, 0x1F600) || gt_font_find(f, 0xE000)) {   /* controls: the lookup finds a letter, not an emoji / a private-use point */
            printf("FAIL: %s: the glyph lookup does not discriminate\n", lobby_lang_names[lang]);
            fails++;
        }
        static const int texts[2] = {LT_END_COOP, LT_END_COOP_HELP};
        for (int t = 0; t < 2; t++) {
            const char *s = lobby_own_text(texts[t], lang), *p = s;
            while (*p) {
                unsigned cp = utf8_next(&p);
                checks++;
                if (cp != ' ' && !gt_font_find(f, cp)) {
                    printf("FAIL: %s: U+%04X is not in %s (%s)\n", lobby_lang_names[lang], cp, face_of[lang], s);
                    fails++;
                }
            }
        }
        gt_font_free(f);
    }
    printf("test_lm_glyphs: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
