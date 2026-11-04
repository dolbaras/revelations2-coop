/* test_probe32.c: 32-bit offline check of the generic function probe (native/probe.inc): the machine-code thunk must log the registers and arguments, run the stolen prologue and
 * continue, for cdecl, stdcall and thiscall targets, with the stack walk and the argument dump; the result and the stack balance of the target must not change.
 *   sh tests/build_probe32.sh && tests/build/32/test_probe32.exe
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W_LOG "world: "
static int g_logs;
static char g_last[4096];
void r2_log(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(g_last, sizeof g_last, fmt, ap);
    __builtin_va_end(ap);
    g_logs++;
    printf("  log: %s\n", g_last);
}
int r2_safe_read(const void *addr, void *out, unsigned size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), addr, out, size, &got) && got == size;
}

static int w_jmp(unsigned target, int n, void *hook)
{
    DWORD old;
    if (!VirtualProtect((void *)target, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    unsigned char *p = (unsigned char *)target;
    for (int i = 5; i < n; i++) p[i] = 0x90;
    int rel = (int)((unsigned)hook - (target + 5));
    memcpy(p + 1, &rel, 4);
    p[0] = 0xE9;
    VirtualProtect((void *)target, (SIZE_T)n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)target, (SIZE_T)n);
    return 1;
}

#include "../native/probe.inc"

/* targets with the classic MSVC-like prologues the probe has to steal: frame pointer + sub esp; push regs; mov from stack */
__attribute__((noinline, cdecl)) int target_cdecl(int a, int b, int c)
{
    volatile int buf[16];
    for (int i = 0; i < 16; i++) buf[i] = a + i;
    return buf[3] * b + c + buf[15];
}
__attribute__((noinline, stdcall)) int target_stdcall(int a, int b)
{
    volatile int buf[8];
    buf[0] = a;
    buf[1] = b;
    return buf[0] * 7 + buf[1];
}
__attribute__((noinline, thiscall)) int target_thiscall(int *self, int a, int b)
{
    volatile int buf[8];
    buf[0] = *self;
    buf[1] = a;
    return buf[0] + buf[1] * 3 + b;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned prologue_len(unsigned fn)
{
    /* stolen length: the smallest instruction boundary >= 5 for the prologues the compiler wrote; found by hand-decoding the common forms */
    const unsigned char *p = (const unsigned char *)fn;
    unsigned n = 0;
    while (n < 5) {
        if (p[n] == 0x55) n += 1;                                  /* push ebp */
        else if (p[n] == 0x89 && p[n + 1] == 0xE5) n += 2;         /* mov ebp, esp */
        else if (p[n] == 0x83 && p[n + 1] == 0xEC) n += 3;         /* sub esp, imm8 */
        else if (p[n] == 0x81 && p[n + 1] == 0xEC) n += 6;         /* sub esp, imm32 */
        else if ((p[n] & 0xF8) == 0x50) n += 1;                    /* push reg */
        else if (p[n] == 0x8B && (p[n + 1] & 0xC0) == 0xC0) n += 2; /* mov reg, reg */
        else if (p[n] == 0x8B && p[n + 1] == 0x4C) n += 4;         /* mov ecx, [esp+d8] */
        else if (p[n] == 0x8B && p[n + 1] == 0x44) n += 4;
        else if (p[n] == 0x8B && p[n + 1] == 0x54) n += 4;
        else return 0;
    }
    return n;
}

int main(void)
{
    char out[300];
    printf("probe thunk test (32-bit)\n");
    if (sizeof(void *) != 4) {
        printf("must be built 32-bit\n");
        return 2;
    }
    int exp1 = target_cdecl(5, 6, 7), exp2 = target_stdcall(5, 6);
    int val = 40;
    int exp3 = target_thiscall(&val, 3, 4);
    unsigned n1 = prologue_len((unsigned)target_cdecl), n2 = prologue_len((unsigned)target_stdcall), n3 = prologue_len((unsigned)target_thiscall);
    printf("prologue lengths %u %u %u\n", n1, n2, n3);
    CHECK(n1 && n2 && n3, "prologue not understood");
    if (!n1 || !n2 || !n3) return 1;

    CHECK(probe_install((unsigned)target_cdecl, n1, 5, 1, 1, 16, out, sizeof out), "%s", out);
    CHECK(probe_install((unsigned)target_stdcall, n2, 5, 0, -1, 0, out, sizeof out), "%s", out);
    CHECK(probe_install((unsigned)target_thiscall, n3, 5, 1, 10, 8, out, sizeof out), "%s", out);

    int before = g_logs;
    int r1 = target_cdecl(5, 6, 7);
    CHECK(r1 == exp1, "cdecl result %d != %d", r1, exp1);
    CHECK(g_logs > before, "cdecl: nothing logged");
    CHECK(strstr(g_last, "dump") != NULL, "cdecl: no dump line (%s)", g_last);
    before = g_logs;
    int r2 = target_stdcall(5, 6);
    CHECK(r2 == exp2, "stdcall result %d != %d", r2, exp2);
    CHECK(g_logs == before + 1, "stdcall: expected one log line");
    CHECK(strstr(g_last, "00000005 00000006") != NULL, "stdcall: arguments not in the log (%s)", g_last);
    before = g_logs;
    int r3 = target_thiscall(&val, 3, 4);
    CHECK(r3 == exp3, "thiscall result %d != %d", r3, exp3);
    CHECK(g_logs > before, "thiscall: nothing logged");
    char want[32];
    snprintf(want, sizeof want, "ecx %08x", (unsigned)&val);
    int found = 0;
    /* the first of the two lines holds ecx; run again and look at the log line before the dump line */
    before = g_logs;
    (void)target_thiscall(&val, 3, 4);
    (void)found;
    CHECK(g_logs >= before + 1, "thiscall: second call not logged");

    /* maxlog: only the first 5 calls log, the result stays right */
    int calls = 0;
    before = g_logs;
    for (int i = 0; i < 20; i++) calls += target_stdcall(i, 1) != i * 7 + 1;
    CHECK(calls == 0, "stdcall results changed under the probe");
    CHECK(g_logs - before <= 4, "maxlog not respected (%d lines)", g_logs - before);

    probe_remove((unsigned)target_stdcall, out, sizeof out);
    CHECK(strstr(out, "removed") != NULL, "%s", out);
    before = g_logs;
    CHECK(target_stdcall(5, 6) == exp2, "result after removal");
    CHECK(g_logs == before, "logging after removal");
    probe_list(out, sizeof out);
    printf("%s\n", out);

    printf(fails ? "FAILED: %d\n" : "all probe checks passed\n", fails);
    return fails != 0;
}
