/* test_testbox.c: the answers a test instance gives to message boxes (native/testbox_logic.h). */
#include <stdio.h>
#include <string.h>
#include "../native/testbox_logic.h"
static int bad, n;
static void chk(int ok, const char *what) { n++; if (!ok) { bad++; printf("FAIL: %s\n", what); } }
int main(void)
{
    chk(testbox_answer(0x00) == 1, "MB_OK -> IDOK");
    chk(testbox_answer(0x10) == 1, "MB_OK | MB_ICONHAND -> IDOK (icon bits ignored)");
    chk(testbox_answer(0x00040000 | 0x10) == 1, "MB_OK | MB_TOPMOST -> IDOK");
    chk(testbox_answer(0x01) == 2, "MB_OKCANCEL -> IDCANCEL");
    chk(testbox_answer(0x02) == 3, "MB_ABORTRETRYIGNORE -> IDABORT");
    chk(testbox_answer(0x03) == 2, "MB_YESNOCANCEL -> IDCANCEL");
    chk(testbox_answer(0x04) == 7, "MB_YESNO -> IDNO");
    chk(testbox_answer(0x05) == 2, "MB_RETRYCANCEL -> IDCANCEL");
    chk(testbox_answer(0x06) == 2, "MB_CANCELTRYCONTINUE -> IDCANCEL");
    /* REV2_MULTI: only the single-instance mutex "sBioMain" gets a per-process name */
    char b[64];
    chk(testbox_multi_mutex_name("sBioMain", 4242, b, sizeof b) == 1 && !strcmp(b, "sBioMain-rev2-4242"), "sBioMain -> sBioMain-rev2-<pid>");
    chk(testbox_multi_mutex_name("sSavedata::cStorageThread::mMutex", 4242, b, sizeof b) == 0, "the save thread's mutex stays shared");
    chk(testbox_multi_mutex_name("sBioMainX", 1, b, sizeof b) == 0 && testbox_multi_mutex_name("sBioMai", 1, b, sizeof b) == 0 && testbox_multi_mutex_name("sbiomain", 1, b, sizeof b) == 0, "exact name only");
    chk(testbox_multi_mutex_name(NULL, 1, b, sizeof b) == 0 && testbox_multi_mutex_name("", 1, b, sizeof b) == 0, "unnamed mutexes untouched");
    chk(testbox_multi_mutex_name("sBioMain", 1, b, 8) == 0, "a too small buffer leaves the name alone");
    chk(testbox_multi_mutex_name("sBioMain", 4294967295ul, b, sizeof b) == 1 && strlen(b) < 32, "the largest pid fits");
    printf("%d checks, %d failed\n", n, bad);
    return bad != 0;
}
