// ReinforceFix.dll - auto-reinforce is lost when a squad goes through an Eldar webway gate (DoW2: Retribution 3.19).
//
// Auto-reinforce ("reinforce overwatch" in the HUD) is client-side: the HUD keeps a list of squad ids and every tick
// FUN_00B26570 walks it and sends a normal reinforce command for each squad. The same walk drops a squad from the
// list when the squad no longer exists OR when it has no entities on the map. A squad loaded into a webway gate
// (hold_ext) keeps its id but has no entities while it is inside, so the walk drops it and it comes out with
// auto-reinforce off.
//
// Fix: a squad with no entities is skipped instead of dropped (6 bytes at 0x00B26646: jump to the loop step).
// Squads that are really gone are still dropped by the first check (the squad lookup fails). Nothing in the
// simulation changes - the reinforce commands themselves are the game's own, so there is no sync impact.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PATCH_AT 0x00B26646
#define LOOP_STEP 0x00B26847
static const unsigned char ORIG[6] = {0x8B, 0x13, 0x89, 0x54, 0x24, 0x14};   // mov edx,[ebx]; mov [esp+14h],edx (then jmp remove)
static wchar_t g_log[MAX_PATH];

static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
static void install(void) {
    unsigned char *t = (unsigned char *)PATCH_AT;
    if (IsBadReadPtr(t, 6) || memcmp(t, ORIG, 6)) { logmsg("DOW2.exe is not 3.19 (no match at %08X) - not patching", PATCH_AT); return; }
    DWORD old; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(DWORD *)(t + 1) = LOOP_STEP - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), t, 6);
    logmsg("ReinforceFix v1: squads inside a webway gate keep auto-reinforce (patched %08X)", PATCH_AT);
}
// one copy per game process: a patch can come both from Elite_DLLs (launcher) and from dow2-patches\ (winmm loader)
static int already_loaded(const char *name) {
    char n[96]; wsprintfA(n, "Local\\dow2patch_%s_%lu", name, GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, n);
    return m && GetLastError() == ERROR_ALREADY_EXISTS;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_log); wcscat(ini, L"ReinforceFix.ini"); wcscat(g_log, L"ReinforceFix.log");
    if (already_loaded("ReinforceFix")) { logmsg("another ReinforceFix copy is already active in this game, this one stays idle"); return TRUE; }
    if (!GetPrivateProfileIntW(L"ReinforceFix", L"Enabled", 1, ini)) { logmsg("disabled in ReinforceFix.ini"); return TRUE; }
    install();
    return TRUE;
}
