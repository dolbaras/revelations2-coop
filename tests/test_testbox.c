/* test_testbox.c: the answers a test instance gives to message boxes (native/testbox_logic.h). */
#include <stdio.h>
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
    printf("%d checks, %d failed\n", n, bad);
    return bad != 0;
}
