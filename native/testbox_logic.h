/*
 * What a test instance answers to a message box instead of showing it (pure, tested in tests/test_testbox.c): no window may appear on a monitor in use by someone else.
 * Answer by the button set of the box (MB_* & 0xF): OK -> IDOK, OK/Cancel -> IDCANCEL, Abort/Retry/Ignore -> IDABORT, Yes/No/Cancel -> IDCANCEL, Yes/No -> IDNO,
 * Retry/Cancel -> IDCANCEL, Cancel/Try/Continue -> IDCANCEL.
 */
#ifndef R2_TESTBOX_LOGIC_H
#define R2_TESTBOX_LOGIC_H
static inline int testbox_answer(unsigned type)
{
    switch (type & 0xFu) {
    case 0: return 1;    /* MB_OK -> IDOK */
    case 2: return 3;    /* MB_ABORTRETRYIGNORE -> IDABORT */
    case 4: return 7;    /* MB_YESNO -> IDNO */
    default: return 2;   /* IDCANCEL */
    }
}
#endif
