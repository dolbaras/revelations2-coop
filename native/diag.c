/*
 * Start-up diagnostics (always on, cheap, log only; nothing here changes what the game does):
 *   - every CreateThread of the game: start routine and caller as exe+offset, time since process start
 *   - every wait that took long (WaitForSingleObject / WaitForMultipleObjects / Sleep / SleepEx >= 300 ms): caller, time
 *   - every distinct window message the game's window received before the first Present (testmode.c calls diag_message)
 * They answer "what is the game waiting for during the slow start" (notes\stage2a.md).
 *
 *   REV2_PASS=1   pass-through mode: none of these hooks (and none of the Steam/pad/dinput hooks, see proxy.c) is installed
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "r2.h"

static DWORD t_start;
static volatile LONG n_threads, n_waits;

static const char *where_buf(const void *addr, char *buf, int size)
{
    HMODULE m = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        char name[MAX_PATH] = "?";
        GetModuleFileNameA(m, name, sizeof name);
        const char *b = strrchr(name, '\\');
        snprintf(buf, size, "%s+%lx", b ? b + 1 : name, (unsigned long)((const char *)addr - (const char *)m));
    } else
        snprintf(buf, size, "%p", addr);
    return buf;
}

typedef HANDLE (WINAPI *CreateThread_t)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
typedef DWORD (WINAPI *WaitForSingleObject_t)(HANDLE, DWORD);
typedef DWORD (WINAPI *WaitForMultipleObjects_t)(DWORD, const HANDLE *, BOOL, DWORD);
typedef void (WINAPI *Sleep_t)(DWORD);
typedef DWORD (WINAPI *SleepEx_t)(DWORD, BOOL);
static CreateThread_t real_CreateThread;
static WaitForSingleObject_t real_WaitForSingleObject;
static WaitForMultipleObjects_t real_WaitForMultipleObjects;
static Sleep_t real_Sleep;
static SleepEx_t real_SleepEx;

static HANDLE WINAPI hook_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid)
{
    HANDLE h = real_CreateThread(sa, stack, fn, arg, flags, tid);
    LONG n = InterlockedIncrement(&n_threads);
    if (n <= 80) {
        char a[80], b[80];
        r2_log("diag: CreateThread #%ld start=%s caller=%s arg=%p flags=%lx -> %p (+%lu ms)", n, where_buf((void *)fn, a, sizeof a),
               where_buf(__builtin_return_address(0), b, sizeof b), arg, flags, h, GetTickCount() - t_start);
    }
    return h;
}

#define LONGWAIT 300
static void note_wait(const char *what, DWORD asked, DWORD took, const void *caller, DWORD result)
{
    if (took < LONGWAIT || InterlockedIncrement(&n_waits) > 150)
        return;
    char b[80];
    r2_log("diag: %s(%lu ms) took %lu ms result=%lx thread=%lu caller=%s (+%lu ms)", what, asked, took, result, GetCurrentThreadId(),
           where_buf(caller, b, sizeof b), GetTickCount() - t_start);
}

static DWORD WINAPI hook_WaitForSingleObject(HANDLE h, DWORD ms)
{
    DWORD t = GetTickCount();
    DWORD r = real_WaitForSingleObject(h, ms);
    note_wait("WaitForSingleObject", ms, GetTickCount() - t, __builtin_return_address(0), r);
    return r;
}

static DWORD WINAPI hook_WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms)
{
    DWORD t = GetTickCount();
    DWORD r = real_WaitForMultipleObjects(n, h, all, ms);
    note_wait("WaitForMultipleObjects", ms, GetTickCount() - t, __builtin_return_address(0), r);
    return r;
}

static void WINAPI hook_Sleep(DWORD ms)
{
    DWORD t = GetTickCount();
    real_Sleep(ms);
    note_wait("Sleep", ms, GetTickCount() - t, __builtin_return_address(0), 0);
}

static DWORD WINAPI hook_SleepEx(DWORD ms, BOOL alertable)
{
    DWORD t = GetTickCount();
    DWORD r = real_SleepEx(ms, alertable);
    note_wait("SleepEx", ms, GetTickCount() - t, __builtin_return_address(0), r);
    return r;
}

/* window messages: the first time each id is seen (and its count at the first Present, console "msgs") */
static volatile LONG msg_count[0x400];
static volatile LONG msg_other;
void diag_message(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= 0x400) {
        InterlockedIncrement(&msg_other);
        return;
    }
    if (InterlockedIncrement(&msg_count[msg]) == 1)
        r2_log("diag: first window message 0x%03x wp=%lx lp=%lx (+%lu ms)", msg, (unsigned long)wp, (unsigned long)lp, GetTickCount() - t_start);
}

void diag_report(char *out, int size)
{
    int o = snprintf(out, size, "threads created %ld, long waits %ld; window messages seen (id:count):", n_threads, n_waits);
    for (int i = 0; i < 0x400 && o + 24 < size; i++)
        if (msg_count[i])
            o += snprintf(out + o, size - o, " %x:%ld", i, msg_count[i]);
    snprintf(out + o, size - o, " other:%ld", msg_other);
}

void diag_install(void)
{
    t_start = GetTickCount();
    real_CreateThread = (CreateThread_t)r2_patch_iat("KERNEL32.dll", "CreateThread", (void *)hook_CreateThread);
    real_WaitForSingleObject = (WaitForSingleObject_t)r2_patch_iat("KERNEL32.dll", "WaitForSingleObject", (void *)hook_WaitForSingleObject);
    real_WaitForMultipleObjects = (WaitForMultipleObjects_t)r2_patch_iat("KERNEL32.dll", "WaitForMultipleObjects", (void *)hook_WaitForMultipleObjects);
    real_Sleep = (Sleep_t)r2_patch_iat("KERNEL32.dll", "Sleep", (void *)hook_Sleep);
    real_SleepEx = (SleepEx_t)r2_patch_iat("KERNEL32.dll", "SleepEx", (void *)hook_SleepEx);
}
