// LobbyStats.dll - Relic rating right in the DoW2 Retribution / Elite lobby.
//
// How it works: we hook the engine's "call a Flash function" call (FUN_0045db6c, __thiscall
// Invoke(method, flags, GFxValue* args, count)). When the engine sends Receive_PopulatePlayerList
// to the lobby movie, we append the rating to the name in each player slot and replace
// the slot tooltip (m_tooltip field) with "rating / wins / games / hours".
// Data is fetched by a background thread from the stats service (<Server>/stats/api/lobby), which reads the public Relic API.
// A name is first shown without numbers; they appear on the next lobby update (join, ready,
// race change) and immediately in later lobbies (disk cache next to the DLL).
// Players are matched by SteamID64 from the slot data (name is only a fallback).
// It only reads the lobby: no kicking, no chat commands, no extra menus.
//
// Works only with DOW2.exe 3.19 (Steam): the prologue bytes are checked before installing.
#include <windows.h>
#include <winhttp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>

#define HOOK_ADDR   0x0045DB6C
#define HOOK_RESUME 0x0045DB71
static const unsigned char PROLOGUE[5] = {0xB8, 0x59, 0x70, 0xEF, 0x00};   // mov eax, 0x00EF7059

// stats service address: at build time -DSTATS_HOST=L\"host\" or in ini [LobbyStats] Server=host
#ifndef STATS_HOST
#define STATS_HOST L""
#endif
static wchar_t g_server[128] = STATS_HOST;
#define SERVER_PATH "/stats/api/lobby"

// GFxValue (Scaleform 2.x in the game): 16 bytes. Type in the first word, value at +8.
typedef struct { int type; int pad; union { double num; unsigned char b; const wchar_t *ws; const char *s; } v; } GFxValue;
enum { GFX_BOOL = 2, GFX_NUMBER = 3, GFX_STRING = 4, GFX_WSTRING = 5 };
enum { HEADER_ARGS = 5, SLOT_ARGS = 20, A_HASUSER = 0, A_SLOTINDEX = 3, A_SLOTTYPE = 4, A_NAME = 5, A_TOOLTIP = 19 };
enum { SLOT_LOCAL = 0, SLOT_REMOTE = 1 };

typedef struct { wchar_t name[64]; int known, absent; int rating, wins, losses, hours; DWORD fetched; } Entry;  // absent: server said "no Relic profile"
#define MAX_ENTRIES 256
static Entry g_cache[MAX_ENTRIES]; static int g_ncache;
static CRITICAL_SECTION g_lock; static HANDLE g_wake;
static wchar_t g_pending[16][64]; static int g_npending;
static wchar_t g_dir[MAX_PATH];
static int g_showInName = 1, g_stats = 1;                  // g_stats=0: leave names and tooltips alone

static void logmsg(const char *fmt, ...) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%lsLobbyStats.log", g_dir);
    FILE *f = _wfopen(p, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

static Entry *find(const wchar_t *name) {
    for (int i = 0; i < g_ncache; i++) if (!wcscmp(g_cache[i].name, name)) return &g_cache[i];
    return NULL;
}
static Entry *find_or_add(const wchar_t *name) {
    Entry *e = find(name); if (e) return e;
    if (g_ncache == MAX_ENTRIES) { memmove(g_cache, g_cache + 1, sizeof(Entry) * (MAX_ENTRIES - 1)); g_ncache--; }
    e = &g_cache[g_ncache++]; memset(e, 0, sizeof *e); wcsncpy(e->name, name, 63); return e;
}

// ---------- disk cache: lines "name\trating\twins\tlosses\thours" in UTF-8 ----------
static void cache_path(wchar_t *p) { _snwprintf(p, MAX_PATH, L"%lsLobbyStats.cache", g_dir); }
static void parse_lines(char *buf, int from_disk) {
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *f[6] = {0}; int n = 0; char *p = line;
        while (n < 6) { f[n++] = p; p = strchr(p, '\t'); if (!p) break; *p++ = 0; }
        if (n < 2 || !*f[0]) continue;
        wchar_t name[64]; if (!MultiByteToWideChar(CP_UTF8, 0, f[0], -1, name, 64)) continue;
        Entry *e = find_or_add(name);
        e->fetched = from_disk ? 0 : GetTickCount();
        if (n >= 4 && strcmp(f[1], "-")) {
            e->known = 1; e->absent = 0; e->rating = atoi(f[1]); e->wins = atoi(f[2]); e->losses = atoi(f[3]);
            e->hours = (n >= 5 && strcmp(f[4], "-")) ? atoi(f[4]) : -1;
        } else if (!from_disk) { e->known = 0; e->absent = 1; }
    }
}
static void load_cache(void) {
    wchar_t p[MAX_PATH]; cache_path(p); FILE *f = _wfopen(p, L"rb"); if (!f) return;
    static char buf[64 * 1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    parse_lines(buf, 1);
}
static void save_cache(void) {
    wchar_t p[MAX_PATH]; cache_path(p); FILE *f = _wfopen(p, L"wb"); if (!f) return;
    for (int i = 0; i < g_ncache; i++) if (g_cache[i].known) {
        char name[256]; WideCharToMultiByte(CP_UTF8, 0, g_cache[i].name, -1, name, sizeof name, NULL, NULL);
        fprintf(f, "%s\t%d\t%d\t%d\t%d\n", name, g_cache[i].rating, g_cache[i].wins, g_cache[i].losses, g_cache[i].hours);
    }
    fclose(f);
}

// ---------- network ----------
static void urlenc(const wchar_t *w, char *out, int cap) {
    char u[256]; WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    int o = 0;
    for (unsigned char *c = (unsigned char *)u; *c && o < cap - 4; c++) {
        if (isalnum(*c) || strchr("-_.~", *c)) out[o++] = *c;
        else o += sprintf(out + o, "%%%02X", *c);
    }
    out[o] = 0;
}
static int http_get(const char *pathq, char *out, int cap) {
    int got = 0; wchar_t wpath[4096]; MultiByteToWideChar(CP_UTF8, 0, pathq, -1, wpath, 4096);
    HINTERNET s = WinHttpOpen(L"LobbyStats/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!s) return -1;
    WinHttpSetTimeouts(s, 5000, 5000, 15000, 30000);
    HINTERNET c = WinHttpConnect(s, g_server, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", wpath, NULL, NULL, NULL, WINHTTP_FLAG_SECURE) : NULL;
    if (r && WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL)) {
        DWORD n;
        while (got < cap - 1 && WinHttpReadData(r, out + got, cap - 1 - got, &n) && n) got += n;
    } else got = -1;
    if (got >= 0) out[got] = 0;
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return got;
}
static DWORD WINAPI worker(LPVOID _) {
    static char resp[64 * 1024], q[4096];
    for (;;) {
        WaitForSingleObject(g_wake, INFINITE);
        Sleep(300);                                  // collect all names from one lobby update into a single request
        wchar_t names[16][64]; int n;
        EnterCriticalSection(&g_lock);
        n = g_npending; memcpy(names, g_pending, sizeof(wchar_t) * 64 * n); g_npending = 0;
        LeaveCriticalSection(&g_lock);
        if (!n) continue;
        int o = sprintf(q, "%s?", SERVER_PATH);
        for (int i = 0; i < n; i++) { char e[400]; urlenc(names[i], e, sizeof e); o += snprintf(q + o, sizeof q - o, "%sn=%s", i ? "&" : "", e); }
        int got = http_get(q, resp, sizeof resp);
        logmsg("lookup %d names -> %d bytes", n, got);
        EnterCriticalSection(&g_lock);
        if (got > 0) { parse_lines(resp, 0); save_cache(); }
        else for (int i = 0; i < n; i++) find_or_add(names[i])->fetched = GetTickCount();   // don't hammer the server on error
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

// ---------- game functions (DOW2.exe 3.19, fixed addresses, no ASLR) ----------
#define G_SESSION      0x01335014     // global network manager; vtbl+0x24 -> match/lobby object
#define FN_INFO_CTOR   0x0040172D     // __stdcall(GameInfo*)
#define FN_INFO_DTOR   0x00401794
#define FN_ST_CTOR     0x0041F1E5     // __stdcall(StationInfo*)
#define FN_ST_DTOR     0x00401A14
#define INFO_SLOTS     0x70           // GameInfo: start of the slot array
#define INFO_NSLOTS    0x78           //           number of slots
#define SLOT_SIZE      0xC8           // slot: +8/+0xC - player's SteamID64
typedef void  (__stdcall *Fn1)(void *);
typedef void *(__fastcall *GetMatchFn)(void *self, void *edx);
typedef char  (__stdcall *GetInfoFn)(void *self, void *info);

static int g_hooked;
static void *match_obj(void) {
    void **mgr = *(void ***)G_SESSION; if (!mgr) return NULL;
    return ((GetMatchFn)(*(void ***)mgr)[0x24 / 4])(mgr, NULL);
}
// player IDs by slot POSITION - the way the game itself builds the lobby list (InvokePopulatePlayerList 0x4911C4):
// GameInfo (match vtbl+4): slots *(+0x70), count *(+0x78), stride 0xC8; in the slot +8 = player ID (profile ID, not SteamID; -1 = empty),
// +0x98 = position (the UI receives it as the slot number). Own ID - same as the game: 0x401607 + 0x53A400, ID in the first 8 bytes.
static int ids_by_position(unsigned long long *ids, int cap) {
    memset(ids, 0, sizeof(*ids) * cap);
    if (!g_hooked) return 0;
    void *m = match_obj(); if (!m) return 0;
    static __attribute__((aligned(16))) unsigned char info[0x2000];
    memset(info, 0, sizeof info); ((Fn1)FN_INFO_CTOR)(info);
    int n = 0, found = 0;
    if (((GetInfoFn)(*(void ***)m)[4 / 4])(m, info)) {
        unsigned char *base = *(unsigned char **)(info + INFO_SLOTS);
        n = *(int *)(info + INFO_NSLOTS);
        if (base && n > 0 && n <= 64 && !IsBadReadPtr(base, n * SLOT_SIZE))
            for (int i = 0; i < n; i++) {
                unsigned char *sl = base + i * SLOT_SIZE;
                unsigned long long id = *(unsigned long long *)(sl + 8); int pos = *(int *)(sl + 0x98);
                if (id != ~0ULL && id && pos >= 0 && pos < cap) { ids[pos] = id; found++; }
            }
    }
    ((Fn1)FN_INFO_DTOR)(info);
    return found;
}

typedef struct { wchar_t key[64], name[64]; unsigned long long id; int st, pos, obs; } Seen;
static Seen g_seen[16]; static int g_nseen;
static unsigned long long g_localId;                        // own ID (from the slot the game marked as own)
static int g_maxPlayers;                                    // player seats; positions beyond are observers

// ---------- hook ----------
typedef char (__fastcall *InvokeFn)(void *self, void *edx, const char *method, int flags, GFxValue *args, unsigned count);
static InvokeFn g_orig;
static wchar_t g_nameBuf[16][128], g_tipBuf[16][320];

static void decorate(GFxValue *args, unsigned count) {
    if (count < HEADER_ARGS + SLOT_ARGS || (count - HEADER_ARGS) % SLOT_ARGS) {
        static int warned; if (!warned++) logmsg("PopulatePlayerList: unexpected arg count %u, not touching", count);
        return;
    }
    int isHost = args[1].type == GFX_BOOL && args[1].v.b;
    g_nseen = 0;
    unsigned long long ids[32] = {0}; int nids = ids_by_position(ids, 32) ? 32 : 0;
    // own ID - from the slot the game marked as "own" (type 0); calling local_user_id() from 0.6.8 crashed the game for a user
    g_localId = 0;
    for (unsigned i = 0; i < (count - HEADER_ARGS) / SLOT_ARGS && i < 16; i++) {
        GFxValue *a0 = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a0[A_SLOTTYPE].type == GFX_NUMBER && (int)a0[A_SLOTTYPE].v.num == SLOT_LOCAL && a0[A_SLOTINDEX].type == GFX_NUMBER) {
            int p0 = (int)a0[A_SLOTINDEX].v.num; if (p0 >= 0 && p0 < nids && ids[p0]) g_localId = ids[p0];
        }
    }
    unsigned slots = (count - HEADER_ARGS) / SLOT_ARGS; int want = 0;
    EnterCriticalSection(&g_lock);
    for (unsigned i = 0; i < slots && i < 16; i++) {
        GFxValue *a = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a[A_HASUSER].type != GFX_BOOL || !a[A_HASUSER].v.b) continue;
        if (a[A_SLOTTYPE].type != GFX_NUMBER) continue;
        int st = (int)a[A_SLOTTYPE].v.num;
        // slot type: 0 own, 1 other player; observers may have another - rating is shown to everyone
        if (a[A_NAME].type != GFX_WSTRING || !a[A_NAME].v.ws || !a[A_NAME].v.ws[0]) continue;
        if (a[A_TOOLTIP].type != GFX_WSTRING) continue;          // layout isn't what we expect - leave it alone
        const wchar_t *name = a[A_NAME].v.ws;
        unsigned long long id = 0;
        int si = a[A_SLOTINDEX].type == GFX_NUMBER ? (int)a[A_SLOTINDEX].v.num : -1;
        if (si >= 0 && si < nids && ids[si]) id = ids[si];       // player ID by slot position
        int observer = g_maxPlayers > 0 && si >= g_maxPlayers;  // observer: position beyond the player seats
        wchar_t key[64];
        if ((id >> 32) == 0x01100001) _snwprintf(key, 63, L"/steam/%llu", id); else wcsncpy(key, name, 63);   // stats are looked up by SteamID or by name
        key[63] = 0;
        if (g_nseen < 16) {
            Seen *sn = &g_seen[g_nseen++]; wcsncpy(sn->key, key, 63); sn->key[63] = 0; wcsncpy(sn->name, name, 63); sn->name[63] = 0;
            sn->id = id;
            sn->st = st; sn->pos = si; sn->obs = observer;
        }
        Entry *e = find(key);
        DWORD now = GetTickCount();
        if (!e || !e->fetched || now - e->fetched > 10 * 60 * 1000) {          // no data or older than 10 minutes
            int dup = 0;
            for (int k = 0; k < g_npending; k++) if (!wcscmp(g_pending[k], key)) dup = 1;
            if (!dup && g_npending < 16) { wcsncpy(g_pending[g_npending++], key, 63); want = 1; }
            if (!e) { e = find_or_add(key); e->fetched = 0; }
        }
        if (!e->known || !g_stats) continue;
        int games = e->wins + e->losses, wr = games ? (e->wins * 100 + games / 2) / games : 0;
        if (g_showInName) {
            _snwprintf(g_nameBuf[i], 127, L"%ls  [%d]", name, e->rating); g_nameBuf[i][127] = 0;
            a[A_NAME].v.ws = g_nameBuf[i];
        }
        wchar_t hours[48] = L"";
        if (e->hours >= 0) _snwprintf(hours, 47, L"\nSteam hours: %d", e->hours);
        _snwprintf(g_tipBuf[i], 319, L"%ls\nRelic rating: %d\nWin rate: %d%%  (%d W / %d L)\nGames: %d%ls",
                   name, e->rating, wr, e->wins, e->losses, games, hours);
        g_tipBuf[i][319] = 0;
        a[A_TOOLTIP].type = GFX_WSTRING; a[A_TOOLTIP].v.ws = g_tipBuf[i];
    }
    {   // lobby roster changed - log who is there
        static unsigned long long lastSig; unsigned long long sig = nids * 1315423911ULL + g_nseen;
        for (int k = 0; k < g_nseen; k++) sig = sig * 31 + g_seen[k].id;
        if (sig != lastSig) {
            lastSig = sig;
            logmsg("lobby: host=%d, max players=%d, my id=%llu, people=%d", isHost, g_maxPlayers, g_localId, g_nseen);
            for (int k = 0; k < g_nseen; k++) {
                char n8[200]; WideCharToMultiByte(CP_UTF8, 0, g_seen[k].name, -1, n8, sizeof n8, NULL, NULL);
                logmsg("lobby:   %s type=%d pos=%d%s id=%llu%s", n8, g_seen[k].st, g_seen[k].pos, g_seen[k].obs ? " OBSERVER" : "",
                       g_seen[k].id, g_seen[k].id && g_seen[k].id == g_localId ? " (me)" : "");
            }
        }
    }
    LeaveCriticalSection(&g_lock);
    if (want) SetEvent(g_wake);
}

static char __fastcall hook(void *self, void *edx, const char *method, int flags, GFxValue *args, unsigned count) {
    if (method && args && !strcmp(method, "Receive_PopulatePlayerList") && count < 4096 && !IsBadReadPtr(args, count * sizeof(GFxValue)))
        decorate(args, count);
    if (method && args && count >= 1 && !strcmp(method, "Receive_SetMaxPlayers") && !IsBadReadPtr(args, sizeof(GFxValue)) && args[0].type == GFX_NUMBER)
        g_maxPlayers = (int)args[0].v.num;
    return g_orig(self, edx, method, flags, args, count);
}

static void install(void) {
    unsigned char *target = (unsigned char *)HOOK_ADDR;
    if (IsBadReadPtr(target, 5) || memcmp(target, PROLOGUE, 5)) { logmsg("DOW2.exe version mismatch at %08X, not hooking", HOOK_ADDR); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, PROLOGUE, 5); tr[5] = 0xE9; *(DWORD *)(tr + 6) = HOOK_RESUME - (DWORD)(tr + 10);
    g_orig = (InvokeFn)tr;
    DWORD old; VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9; *(DWORD *)(target + 1) = (DWORD)hook - (DWORD)(target + 5);
    VirtualProtect(target, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), target, 5);
    g_hooked = 1;
    logmsg("hooked Invoke at %08X", HOOK_ADDR);
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
    GetModuleFileNameW(h, g_dir, MAX_PATH); wchar_t *sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0; else g_dir[0] = 0;
    wchar_t ini[MAX_PATH]; _snwprintf(ini, MAX_PATH, L"%lsLobbyStats.ini", g_dir);
    g_showInName = GetPrivateProfileIntW(L"LobbyStats", L"ShowRatingInName", 1, ini);
    g_stats       = GetPrivateProfileIntW(L"LobbyStats", L"Stats", 1, ini);
    GetPrivateProfileStringW(L"LobbyStats", L"Server", STATS_HOST, g_server, 128, ini);
    if (!g_server[0]) { g_stats = 0; }                        // stats service not set - there will be no numbers
    InitializeCriticalSection(&g_lock); g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    logmsg("LobbyStats v0.7.0");
    if (already_loaded("LobbyStats")) { logmsg("another LobbyStats copy is already active in this game, this one stays idle"); return TRUE; }
    load_cache(); install();
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}

// for testing outside the game: run the substitution on a fake argument set
__declspec(dllexport) int LobbyStats_Test(GFxValue *args, unsigned count) { decorate(args, count); return g_ncache; }
