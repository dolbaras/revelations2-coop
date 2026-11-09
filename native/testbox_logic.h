/*
 * What a test instance answers to a message box instead of showing it (pure, tested in tests/test_testbox.c): no window may appear on a monitor in use by someone else.
 * Answer by the button set of the box (MB_* & 0xF): OK -> IDOK, OK/Cancel -> IDCANCEL, Abort/Retry/Ignore -> IDABORT, Yes/No/Cancel -> IDCANCEL, Yes/No -> IDNO,
 * Retry/Cancel -> IDCANCEL, Cancel/Try/Continue -> IDCANCEL.
 */
#ifndef R2_TESTBOX_LOGIC_H
#define R2_TESTBOX_LOGIC_H
#include <stdio.h>
static inline int testbox_answer(unsigned type)
{
    switch (type & 0xFu) {
    case 0: return 1;    /* MB_OK -> IDOK */
    case 2: return 3;    /* MB_ABORTRETRYIGNORE -> IDABORT */
    case 4: return 7;    /* MB_YESNO -> IDNO */
    default: return 2;   /* IDCANCEL */
    }
}

/* REV2_MULTI=1 (TEST copies only, never set for players): a second copy of the game on one PC. The game's single-instance check FUN_00C756F0 (thiscall, called from the
 * WinMain body FUN_00A75A90) calls CreateMutexA(NULL, TRUE, DTI_sBioMain->name = "sBioMain") and, on ERROR_ALREADY_EXISTS, restores the first copy's window
 * (FindWindowW(L"MT FRAMEWORK"), ShowWindow 9), closes the mutex and returns 0: WinMain returns 1 and the copy exits (the same pattern as Rev1's "sSkeletonMain").
 * Only that one name is made unique per process; "sSavedata::cStorageThread::mMutex" (the save thread, not owned, no exit path) stays shared. notes\downed-finish.md. */
#define TESTBOX_MULTI_MUTEX "sBioMain"
static inline int testbox_multi_mutex_name(const char *name, unsigned long pid, char *out, unsigned size)
{
    const char *m = TESTBOX_MULTI_MUTEX;
    unsigned i = 0;
    if (!name || !out || size < 32) return 0;
    while (m[i] && name[i] == m[i]) i++;
    if (m[i] || name[i]) return 0;   /* exactly "sBioMain" */
    snprintf(out, size, "%s-rev2-%lu", m, pid);
    return 1;
}
#endif
