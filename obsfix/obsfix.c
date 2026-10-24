// ObsFix.dll — тормоза обсервера в Dawn of War II: Retribution 3.19 (Elite).
//
// Причина: FUN_00AE6850 («какой по счёту слот у локального игрока») в онлайне каждый раз строит полную копию
// GameInfo матча (все слоты по 200 байт, строки, векторы) через сессию RLink, проходит по слотам и всё уничтожает.
// Её зовёт FUN_00AE5E00 («я обсервер? свой/чужой») — у обсервера (нет своего игрока, +0x174 == 0) десятки раз за
// кадр. В реплее (офлайн, флаг сессии & 0x558 == 0) функция сразу возвращает 0 — поэтому реплей не тормозит.
// Замер (FrameProbe v9): у обса эта цепочка ~38% кадра, из них ~22% — выделение/освобождение памяти в ntdll.
//
// Исправление: ответ запоминается на 500 мс (номер слота за матч не меняется). Игра получает то же значение.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define FN_SLOT_INDEX 0x00AE6850
static const unsigned char PRO[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};   // push ebp; mov ebp,esp; and esp,-8
typedef int (__cdecl *SlotFn)(void);
static SlotFn g_orig;
static volatile LONG g_calls, g_misses;
static int g_val; static DWORD g_tick, g_ttl = 500; static int g_have;
static wchar_t g_log[MAX_PATH];

static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
static int __cdecl slot_cached(void) {
    InterlockedIncrement(&g_calls);
    DWORD now = GetTickCount();
    if (g_have && now - g_tick < g_ttl) return g_val;
    InterlockedIncrement(&g_misses);
    g_val = g_orig(); g_tick = now; g_have = 1;
    return g_val;
}
static DWORD WINAPI stats(LPVOID _) {                         // раз в минуту: сколько вызовов сэкономили
    for (;;) {
        Sleep(60000);
        LONG c = InterlockedExchange(&g_calls, 0), m = InterlockedExchange(&g_misses, 0);
        if (c) logmsg("last minute: %ld calls, %ld real (%.1f%% saved), slot index %d", c, m, 100.0 * (c - m) / c, g_val);
    }
    return 0;
}
static void install(void) {
    unsigned char *t = (unsigned char *)FN_SLOT_INDEX;
    if (IsBadReadPtr(t, 6) || memcmp(t, PRO, 6)) { logmsg("DOW2.exe is not 3.19 (no match at %08X) — not patching", FN_SLOT_INDEX); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, PRO, 6); tr[6] = 0xE9; *(DWORD *)(tr + 7) = (FN_SLOT_INDEX + 6) - (DWORD)(tr + 11);
    g_orig = (SlotFn)tr;
    DWORD old; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)slot_cached - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), t, 6);
    logmsg("ObsFix v1: observer slot lookup cached for %lu ms (patched %08X)", g_ttl, FN_SLOT_INDEX);
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_log); wcscat(ini, L"ObsFix.ini"); wcscat(g_log, L"ObsFix.log");
    if (!GetPrivateProfileIntW(L"ObsFix", L"Enabled", 1, ini)) { logmsg("disabled in ObsFix.ini"); return TRUE; }
    g_ttl = GetPrivateProfileIntW(L"ObsFix", L"CacheMs", 500, ini);
    install();
    CloseHandle(CreateThread(NULL, 0, stats, NULL, 0, NULL));
    return TRUE;
}
