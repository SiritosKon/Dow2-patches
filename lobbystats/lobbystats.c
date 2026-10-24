// LobbyStats.dll - Relic rating right in the DoW2 Retribution / Elite lobby.
//
// How it works: we hook the engine's "call a Flash function" call (FUN_0045db6c, __thiscall
// Invoke(method, flags, GFxValue* args, count)). When the engine sends Receive_PopulatePlayerList
// to the lobby movie, we append the rating to the name in each player slot and replace
// the slot tooltip (m_tooltip field) with "rating / wins / games / hours".
// Data is fetched by a background thread from the stats service (<Server>/stats/api/lobby), which reads the public Relic API.
// A name is first shown without numbers; they appear on the next lobby update (join, ready,
// race change) and immediately in later lobbies (disk cache next to the DLL).
//
// LobbyGuard (host's choice, [Guard] section in LobbyStats.ini, off by default): if a joining
// player fails the rules (rating/games/hours), the bot posts the reason in the lobby chat and after a few
// seconds kicks them with the same function as the host's kick button. DryRun=1 - message only, no kick.
// Players are matched by SteamID64 from the slot data (name is only a fallback).
// The host picks the rules in two dropdowns under "Starting resources" (visible to the host only),
// and also with the !filter command in the lobby chat.
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
static volatile int g_fresh; static volatile int g_uiRefill; static int g_logFs; static int g_pollFrames; static DWORD g_lobbyFsTick; static unsigned char g_refMark[16], g_slotType[16];   // referee marks per lobby slot, slot types (see ref_flag)                                 // new data arrived from the server - recheck the lobby
static int g_showInName = 1, g_stats = 1;                  // g_stats=0: leave names and tooltips alone (the filter still works)

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
        if (got > 0) { parse_lines(resp, 0); save_cache(); g_fresh = 1; }   // data arrived - check the joined players (guard_tick)
        else for (int i = 0; i < n; i++) find_or_add(names[i])->fetched = GetTickCount();   // don't hammer the server on error
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

// ---------- game functions (DOW2.exe 3.19, fixed addresses, no ASLR) ----------
#define G_SESSION      0x01335014     // global network manager; vtbl+0x24 -> match/lobby object
#define FN_INFO_CTOR   0x0040172D     // __stdcall(GameInfo*)      - as in PlayerListPanel::OnKickPlayer
#define FN_INFO_DTOR   0x00401794
#define FN_ST_CTOR     0x0041F1E5     // __stdcall(StationInfo*)
#define FN_ST_DTOR     0x00401A14
#define FN_CHAT_SEND   0x004023DC     // __stdcall(int channel 0=all, WStr*, char) - sends lobby chat text
#define INFO_SLOTS     0x70           // GameInfo: start of the slot array
#define INFO_NSLOTS    0x78           //           number of slots
#define SLOT_SIZE      0xC8           // slot: +8/+0xC - player's SteamID64
typedef void  (__stdcall *Fn1)(void *);
typedef void *(__fastcall *GetMatchFn)(void *self, void *edx);
typedef char  (__stdcall *GetInfoFn)(void *self, void *info);
typedef char  (__stdcall *StationFn)(void *self, DWORD lo, DWORD hi, void *st);
typedef char  (__stdcall *KickFn)(void *self, DWORD lo, DWORD hi, int code, const char *why);
typedef void  (__stdcall *ChatFn)(int channel, void *text, int flag);
typedef struct { wchar_t *start, *finish, *eos; } WStr;          // STLport string: start, end, end of buffer

static int g_hooked;
static void *match_obj(void) {
    void **mgr = *(void ***)G_SESSION; if (!mgr) return NULL;
    return ((GetMatchFn)(*(void ***)mgr)[0x24 / 4])(mgr, NULL);
}
// SteamID64 of each lobby slot by index; returns the number of slots
// slot SteamIDs from the match settings - the way the game itself gets them (PostGameStatsForm canContinue, 0x47F959):
// settings = *(0x1335A78); slots = *(settings+0x928), count = *(settings+0x92C), stride 0xA8; slot: +0 type, +0x58 team, +0x68 SteamID64.
#define G_SETTINGS 0x01335A78
static int slot_ids_settings(unsigned long long *ids, int cap) {
    unsigned char *st = *(unsigned char **)G_SETTINGS; if (!st || IsBadReadPtr(st + 0x928, 8)) return 0;
    unsigned char *slots = *(unsigned char **)(st + 0x928); int n = *(int *)(st + 0x92C);
    if (!slots || n <= 0 || n > 16 || IsBadReadPtr(slots, n * 0xA8)) return 0;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) ids[i] = *(unsigned long long *)(slots + i * 0xA8 + 0x68);
    return n;
}
static int slot_ids_gameinfo(unsigned long long *ids, int cap);
// player IDs by slot POSITION - the way the game itself builds the lobby list (InvokePopulatePlayerList 0x4911C4):
// GameInfo (match vtbl+4): slots *(+0x70), count *(+0x78), stride 0xC8; in the slot +8 = player ID (profile ID, not SteamID; -1 = empty),
// +0x98 = position (the UI receives it as the slot number). Own ID - same as the game: 0x401607 + 0x53A400, ID in the first 8 bytes.
#define FN_LOCAL_CTOR 0x00401607
#define FN_LOCAL_FILL 0x0053A400
#define FN_LOCAL_DTOR 0x00401655
static unsigned long long local_user_id(void) {
    if (!g_hooked) return 0;
    static __attribute__((aligned(16))) unsigned char u[0x800];
    memset(u, 0, sizeof u); ((Fn1)FN_LOCAL_CTOR)(u); ((Fn1)FN_LOCAL_FILL)(u);
    unsigned long long id = *(unsigned long long *)u;
    ((Fn1)FN_LOCAL_DTOR)(u);
    return id == ~0ULL ? 0 : id;
}
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
static int slot_ids(unsigned long long *ids, int cap) {
    int n = slot_ids_settings(ids, cap), good = 0;
    for (int i = 0; i < n; i++) if (ids[i]) good++;          // this may be a game profile ID rather than a SteamID - either works for kicking
    static int logged; if (logged++ < 5) { char b[300]; int o = 0;
        for (int i = 0; i < n && o < 280; i++) o += snprintf(b + o, sizeof b - o, " %llu", ids[i]);
        logmsg("ids from match settings (%d slots):%s", n, n ? b : " none"); }
    if (good) return n;
    memset(ids, 0, sizeof(*ids) * cap);
    return slot_ids_gameinfo(ids, cap);                      // fallback path (as in 0.5)
}
static int slot_ids_gameinfo(unsigned long long *ids, int cap) {
    if (!g_hooked) return 0;
    void *m = match_obj(); if (!m) return 0;
    static __attribute__((aligned(16))) unsigned char info[0x2000];
    memset(info, 0, sizeof info); ((Fn1)FN_INFO_CTOR)(info);
    int n = 0;
    if (((GetInfoFn)(*(void ***)m)[4 / 4])(m, info)) {
        unsigned char *base = *(unsigned char **)(info + INFO_SLOTS);
        n = *(int *)(info + INFO_NSLOTS); if (n < 0 || n > cap) n = n < 0 ? 0 : cap;
        for (int i = 0; i < n; i++) ids[i] = *(unsigned long long *)(base + i * SLOT_SIZE + 8);
    }
    ((Fn1)FN_INFO_DTOR)(info);
    return n;
}
static ChatFn g_chatOrig;                                  // trampoline to the real send (after the hook is installed)
static void chat_send(const wchar_t *text) {
    if (!g_hooked) return;                                   // outside the game (tests) - nowhere to send
    static wchar_t buf[256]; wcsncpy(buf, text, 255); buf[255] = 0;
    WStr s = { buf, buf + wcslen(buf), buf + wcslen(buf) + 1 };
    (g_chatOrig ? g_chatOrig : (ChatFn)FN_CHAT_SEND)(0, &s, 0);
}
// ---------- message to the General channel: the way the game itself does it when you type in the channel tab ----------
// Chat service = *(G_SESSION)->vtbl+0x10(); send to channel = service->vtbl+0x1C(service, channel number, message),
// the message is built by 0x4029C1 (eax = string {start, end}, ecx = result) and freed by 0x537830 (esi = pointer).
// The General channel number is learned when the host types in a regular channel (special 0x10000001..4 - match/party - are skipped).
#define FN_MSG_BUILD 0x004029C1
#define FN_MSG_FREE  0x00537830
typedef char (__stdcall *ChanSendFn)(void *svc, int channel, void *msg);
static ChanSendFn g_chanOrig; static void *g_chatSvc; static int g_general;
static void *chat_service(void) {
    void **mgr = *(void ***)G_SESSION; if (!mgr) return NULL;
    return ((GetMatchFn)(*(void ***)mgr)[0x10 / 4])(mgr, NULL);
}
static wchar_t g_iniPath[MAX_PATH];
static char __stdcall chan_send_hook(void *svc, int channel, void *msg) {
    if (channel && (unsigned)(channel - 0x10000001) > 3 && channel != g_general) {
        g_general = channel;
        wchar_t v[24]; _snwprintf(v, 24, L"%d", channel); WritePrivateProfileStringW(L"Chat", L"GeneralChannel", v, g_iniPath);
        logmsg("chat: general channel = %d (learned from your message)", channel);
    }
    return g_chanOrig(svc, channel, msg);
}
static void hook_chat_service(void) {                         // once, from the main thread
    if (g_chanOrig || !g_hooked) return;
    void *svc = chat_service(); if (!svc) return;
    void **vt = *(void ***)svc; DWORD old;
    if (IsBadReadPtr(vt, 0x20) || !VirtualProtect(&vt[0x1C / 4], 4, PAGE_READWRITE, &old)) return;
    g_chatSvc = svc; g_chanOrig = (ChanSendFn)vt[0x1C / 4]; vt[0x1C / 4] = (void *)chan_send_hook;
    VirtualProtect(&vt[0x1C / 4], 4, old, &old);
    logmsg("chat: channel send hooked, general channel %d%s", g_general, g_general ? "" : " (unknown yet - write anything in General once)");
}
static int general_send(const wchar_t *text) {
    if (!g_hooked || !g_general || !g_chanOrig || !g_chatSvc) return 0;
    static wchar_t buf[256]; wcsncpy(buf, text, 255); buf[255] = 0;
    WStr w = { buf, buf + wcslen(buf), buf + wcslen(buf) + 1 };
    DWORD holder[4] = {0};
    __asm__ volatile("call *%2" : : "a"(&w), "c"(holder), "r"(FN_MSG_BUILD) : "edx", "memory");
    char r = g_chanOrig(g_chatSvc, g_general, holder);
    if (holder[1]) __asm__ volatile("call *%1" : : "S"(holder[0]), "r"(FN_MSG_FREE) : "eax", "ecx", "edx", "memory");
    return r;
}

static unsigned long long g_localIdForKick;                  // own ID (from decorate) - checked before kicking
static int station_of(void *m, unsigned long long id, DWORD *lo, DWORD *hi) {
    static __attribute__((aligned(16))) unsigned char st2[0x400];
    memset(st2, 0, sizeof st2); ((Fn1)FN_ST_CTOR)(st2);
    char ok = id ? ((StationFn)(*(void ***)m)[0x18 / 4])(m, (DWORD)id, (DWORD)(id >> 32), st2) : 0;
    *lo = *(DWORD *)st2; *hi = *(DWORD *)(st2 + 4);
    ((Fn1)FN_ST_DTOR)(st2);
    return ok && (*lo & *hi) != 0xFFFFFFFF;
}
static int kick_id(unsigned long long id) {
    void *m = match_obj(); if (!m) return 0;
    // safeguard: never kick our own station, even if the player->ID mapping went wrong somewhere
    DWORD mlo = 0, mhi = 0, tlo = 0, thi = 0;
    if (!g_localIdForKick || id == g_localIdForKick) { logmsg("guard: kick of %llu cancelled - own id unknown or equal (%llu)", id, g_localIdForKick); return 0; }
    int mine = station_of(m, g_localIdForKick, &mlo, &mhi), target = station_of(m, id, &tlo, &thi);
    if (!target || (mine && mlo == tlo && mhi == thi)) {
        logmsg("guard: kick of %llu cancelled - target station %08lX:%08lX, my station %08lX:%08lX (found %d/%d)", id, thi, tlo, mhi, mlo, target, mine);
        return 0;
    }
    static __attribute__((aligned(16))) unsigned char st[0x400];
    memset(st, 0, sizeof st); ((Fn1)FN_ST_CTOR)(st);
    int ok = 0;
    char found = ((StationFn)(*(void ***)m)[0x18 / 4])(m, (DWORD)id, (DWORD)(id >> 32), st);
    logmsg("guard: station lookup for %llu -> %d, station %08lX:%08lX", id, found, *(DWORD *)st, *(DWORD *)(st + 4));
    if (found && (*(DWORD *)st & *(DWORD *)(st + 4)) != 0xFFFFFFFF) {
        ((KickFn)(*(void ***)m)[0x60 / 4])(m, *(DWORD *)st, *(DWORD *)(st + 4), 0, "LobbyGuard");
        ok = 1;
    }
    ((Fn1)FN_ST_DTOR)(st);
    return ok;
}

// ---------- host rules ----------
static struct { int enabled, dryRun, minRating, maxRating, minGames, minHours, maxHours, minWinRate, maxWinRate, kickUnknown, delaySec; } G;
typedef struct { wchar_t key[64]; DWORD lastAction; } Acted;
static Acted g_acted[64]; static int g_nacted;
typedef struct { unsigned long long id; wchar_t text[256]; DWORD due; int kick, general; } Job;
static Job g_jobs[16]; static int g_njobs;

static Acted *acted(const wchar_t *key) {
    for (int i = 0; i < g_nacted; i++) if (!wcscmp(g_acted[i].key, key)) return &g_acted[i];
    if (g_nacted == 64) { memmove(g_acted, g_acted + 1, sizeof(Acted) * 63); g_nacted--; }
    Acted *a = &g_acted[g_nacted++]; memset(a, 0, sizeof *a); wcsncpy(a->key, key, 63); return a;
}
// rejection reason, or NULL if the player passes
static const wchar_t *violation(const Entry *e, wchar_t *why, int cap) {
    if (!e->known) {
        if (G.kickUnknown && e->absent) { _snwprintf(why, cap, L"no Relic stats found"); return why; }   // only when the server answered "not found", never on network errors
        return NULL;
    }
    int games = e->wins + e->losses, wr = games ? (e->wins * 100 + games / 2) / games : 0;
    if (G.minRating && e->rating < G.minRating) _snwprintf(why, cap, L"min rating is %d, yours is %d", G.minRating, e->rating);
    else if (G.maxRating && e->rating > G.maxRating) _snwprintf(why, cap, L"max rating is %d, yours is %d", G.maxRating, e->rating);
    else if (G.minWinRate && games && wr < G.minWinRate) _snwprintf(why, cap, L"min win rate is %d%%, yours is %d%%", G.minWinRate, wr);
    else if (G.maxWinRate && games && wr > G.maxWinRate) _snwprintf(why, cap, L"max win rate is %d%%, yours is %d%%", G.maxWinRate, wr);
    else if (G.minGames && games < G.minGames) _snwprintf(why, cap, L"min games is %d, yours is %d", G.minGames, games);
    else if (G.minHours && e->hours >= 0 && e->hours < G.minHours) _snwprintf(why, cap, L"min hours is %d, yours is %d", G.minHours, e->hours);
    else if (G.maxHours && e->hours > G.maxHours) _snwprintf(why, cap, L"max hours is %d, yours is %d", G.maxHours, e->hours);
    else return NULL;                       // hours hidden (private profile) - don't kick by hours
    why[cap - 1] = 0; return why;
}
static unsigned long long g_localId;                        // the host's own ID - never kicked
static int g_maxPlayers;                                    // player seats (Receive_SetMaxPlayers); positions beyond are observers
static void guard_check(const wchar_t *key, const wchar_t *name, const Entry *e, unsigned long long id) {
    wchar_t why[160];
    if (!violation(e, why, 160)) return;
    Acted *a = acted(key); DWORD now = GetTickCount();
    if (a->lastAction && now - a->lastAction < 2 * 60 * 1000) return;   // don't repeat more than once every 2 minutes
    a->lastAction = now;
    if (g_njobs + 2 > 16) return;
    Job *j = &g_jobs[g_njobs++]; memset(j, 0, sizeof *j); j->due = now;
    if (G.dryRun) _snwprintf(j->text, 255, L"[Bot test] %ls would be kicked: %ls", name, why);
    else _snwprintf(j->text, 255, L"[Bot] %ls kicked by bot: %ls", name, why);
    if (id && id == g_localId) { logmsg("guard: id %llu is the host's own id - slot mapping is wrong, not kicking", id); id = 0; }
    if (!G.dryRun && id) {
        Job *k = &g_jobs[g_njobs++]; memset(k, 0, sizeof *k); k->id = id; k->kick = 1; k->due = now + G.delaySec * 1000;
        if (g_njobs < 16) {                                   // a kicked player won't see the match tab - post the same to the general channel
            Job *g = &g_jobs[g_njobs++]; memset(g, 0, sizeof *g); g->general = 1; g->due = now + G.delaySec * 1000;
            _snwprintf(g->text, 255, L"[Bot] %ls was kicked from the lobby by the host filter: %ls", name, why);
        }
    }
    char n8[200]; WideCharToMultiByte(CP_UTF8, 0, j->text, -1, n8, sizeof n8, NULL, NULL); logmsg("guard: %s", n8);
    if (!G.dryRun && !id) logmsg("guard: no SteamID for this player in the lobby data - cannot kick");
}
// last lobby player list (for rechecking after the rules are changed by a chat command)
typedef struct { wchar_t key[64], name[64]; unsigned long long id; int remote, st, pos, obs; } Seen;
static Seen g_seen[16]; static int g_nseen, g_isHost, g_recheck;

// runs on the main thread (from the hook), outside player list processing
static int g_inDecorate;
static void guard_tick(void) {
    if ((g_recheck || g_fresh) && !g_inDecorate) {          // rules changed or data on a joined player arrived - check the lobby
        int rulesChanged = g_recheck;
        g_recheck = 0; g_fresh = 0;
        EnterCriticalSection(&g_lock);
        if (rulesChanged) g_nacted = 0;                        // warn again when the rules change, but not on new data
        if (G.enabled && g_isHost)
            for (int i = 0; i < g_nseen; i++) {
                Entry *e = find(g_seen[i].key);
                if (e && g_seen[i].remote && (e->known || e->absent)) guard_check(g_seen[i].key, g_seen[i].name, e, g_seen[i].id);
            }
        LeaveCriticalSection(&g_lock);
    }
    if (!g_njobs || g_inDecorate) return;
    DWORD now = GetTickCount();
    for (int i = 0; i < g_njobs; ) {
        Job *j = &g_jobs[i];
        if ((int)(now - j->due) < 0) { i++; continue; }
        Job job = *j; memmove(j, j + 1, sizeof(Job) * (g_njobs - i - 1)); g_njobs--;
        if (!g_hooked) continue;
        if (job.kick) logmsg("guard: kick %llu -> %s", job.id, kick_id(job.id) ? "kick sent" : "station not found");
        else if (job.general) { int ok = general_send(job.text); logmsg("guard: notice to general channel %d -> %s", g_general, ok ? "sent" : g_general ? "failed" : "channel unknown"); }
        else chat_send(job.text);
    }
}

// ---------- host commands in the lobby chat: !filter ... ----------
static wchar_t g_ini[MAX_PATH];
static void save_guard(void) {
    wchar_t v[32];
    #define W(k, x) (_snwprintf(v, 32, L"%d", (x)), WritePrivateProfileStringW(L"Guard", L##k, v, g_ini))
    W("Enabled", G.enabled); W("DryRun", G.dryRun); W("MinRating", G.minRating); W("MaxRating", G.maxRating);
    W("MinWinRate", G.minWinRate); W("MaxWinRate", G.maxWinRate); W("MinGames", G.minGames); W("MinHours", G.minHours); W("MaxHours", G.maxHours);
    W("KickUnknown", G.kickUnknown); W("KickDelaySeconds", G.delaySec);
    #undef W
}
static void guard_status(wchar_t *o, int cap) {
    if (!G.enabled) { _snwprintf(o, cap, L"[Bot] Lobby filter is OFF"); o[cap - 1] = 0; return; }
    wchar_t r[200] = L"", part[64]; 
    #define ADD(...) (_snwprintf(part, 64, __VA_ARGS__), part[63] = 0, wcsncat(r, part, 199 - wcslen(r)))
    if (G.minRating && G.maxRating) ADD(L" rating %d-%d,", G.minRating, G.maxRating);
    else if (G.minRating) ADD(L" rating %d+,", G.minRating);
    else if (G.maxRating) ADD(L" rating %d-,", G.maxRating);
    if (G.minWinRate && G.maxWinRate) ADD(L" win rate %d-%d%%,", G.minWinRate, G.maxWinRate);
    else if (G.minWinRate) ADD(L" win rate %d%%+,", G.minWinRate);
    else if (G.maxWinRate) ADD(L" win rate %d%%-,", G.maxWinRate);
    if (G.minGames) ADD(L" games %d+,", G.minGames);
    if (G.minHours) ADD(L" hours %d+,", G.minHours);
    if (G.maxHours) ADD(L" hours %d-,", G.maxHours);
    if (G.kickUnknown) ADD(L" no new accounts,");
    #undef ADD
    size_t n = wcslen(r); if (n && r[n - 1] == L',') r[n - 1] = 0;
    _snwprintf(o, cap, L"[Bot] Lobby filter ON:%ls%ls", n ? r : L" no rules yet", G.dryRun ? L" (test mode, no kicks; host: !filter live)" : L" (kicks on)");
    o[cap - 1] = 0;
}
// number with a sign before or after: 1500 / 1500+ / +1500 -> at least (1); 1500- / -1500 -> at most (2); 1500-2400 -> range (3)
static int parse_lim(const wchar_t *t, int *lo, int *hi) {
    int a, b, n = 0; wchar_t c = 0;
    if (!t || !*t) return 0;
    if (*t == L'+' && swscanf(t + 1, L"%d%n", &a, &n) == 1 && !t[1 + n]) { *lo = a; return 1; }
    if (*t == L'-' && swscanf(t + 1, L"%d%n", &a, &n) == 1 && !t[1 + n]) { *hi = a; return 2; }
    if (swscanf(t, L"%d-%d%n", &a, &b, &n) == 2 && !t[n]) { *lo = a; *hi = b; return 3; }
    if (swscanf(t, L"%d%n", &a, &n) == 1) {
        c = t[n];
        if (!c || (c == L'+' && !t[n + 1])) { *lo = a; return 1; }
        if (c == L'-' && !t[n + 1]) { *hi = a; return 2; }
        if (c == L'%' && (!t[n + 1] || ((t[n + 1] == L'+' || t[n + 1] == L'-') && !t[n + 2]))) { if (t[n + 1] == L'-') { *hi = a; return 2; } *lo = a; return 1; }
    }
    return 0;
}
static void set_lim(int k, int lo, int hi, int *mn, int *mx) {   // a new rule replaces the previous one for this parameter
    if (k == 1) { *mn = lo; *mx = 0; } else if (k == 2) { *mn = 0; *mx = hi; } else if (k == 3) { *mn = lo; *mx = hi; }
}
// "!filter 1500-2400 wr55 g100 h750 live" -> rules; returns 1 if it is a command
static int handle_command(const wchar_t *t, int len) {
    wchar_t s[256]; if (len > 255) len = 255; wcsncpy(s, t, len); s[len] = 0;
    for (wchar_t *c = s; *c; c++) *c = towlower(*c);
    wchar_t *p = s; while (*p == L' ') p++;
    if (wcsncmp(p, L"!filter", 7) && wcsncmp(p, L"!f ", 3) && wcscmp(p, L"!f")) return 0;
    p += (p[2] == L'i') ? 7 : 2;
    int changed = 0, rules = 0, mode = 0;                       // rules: a rule was set; mode: explicit test/live
    for (wchar_t *tok = wcstok(p, L" ,;"); tok; tok = wcstok(NULL, L" ,;")) {
        int a = 0, b = 0, k, lo = 0, hi = 0; (void)b;
        if (!wcscmp(tok, L"off")) { G.enabled = 0; changed = 1; }
        else if (!wcscmp(tok, L"on")) { G.enabled = 1; changed = 1; }
        else if (!wcscmp(tok, L"test")) { G.enabled = 1; G.dryRun = 1; changed = 1; mode = 1; }
        else if (!wcscmp(tok, L"live") || !wcscmp(tok, L"kick")) { G.enabled = 1; G.dryRun = 0; changed = 1; mode = 1; }
        else if (!wcscmp(tok, L"reset")) { G.minRating = G.maxRating = G.minWinRate = G.maxWinRate = G.minGames = G.minHours = G.maxHours = G.kickUnknown = 0; changed = 1; }
        else if (!wcscmp(tok, L"new")) { G.kickUnknown = 0; changed = 1; }
        else if (!wcscmp(tok, L"nonew")) { G.kickUnknown = 1; G.enabled = 1; changed = 1; }
        else if (!wcsncmp(tok, L"wr", 2) && (k = parse_lim(tok + 2, &lo, &hi))) { set_lim(k, lo, hi, &G.minWinRate, &G.maxWinRate); G.enabled = 1; changed = 1; }   // wr55 wr55- wr-45 wr45-60
        else if (tok[0] == L'g' && swscanf(tok + 1, L"%d", &a) == 1) { G.minGames = a; G.enabled = 1; changed = 1; }
        else if (tok[0] == L'h' && (k = parse_lim(tok + 1, &lo, &hi))) { set_lim(k, lo, hi, &G.minHours, &G.maxHours); G.enabled = 1; changed = 1; }        // h1000 h1000- h-1000 h+1000
        else if ((k = parse_lim(tok, &lo, &hi))) { set_lim(k, lo, hi, &G.minRating, &G.maxRating); G.enabled = 1; changed = 1; }                          // 1500 1500+ +1500 1500- -1500 1500-2400
    }
    // rules without the word test = real kick (previously an old DryRun=1 from the ini silently kept test mode)
    for (const wchar_t *c = s; *c; c++) if (iswdigit(*c)) { rules = 1; break; }
    if (rules && !mode && G.enabled) G.dryRun = 0;
    if (changed) { save_guard(); g_recheck = 1; g_uiRefill = 1; }   // the host's menu will show the new values
    wchar_t st[256]; guard_status(st, 256); chat_send(st);
    char u[512]; WideCharToMultiByte(CP_UTF8, 0, st, -1, u, sizeof u, NULL, NULL); logmsg("command: %s", u);
    return 1;
}
static void __stdcall chat_hook(int channel, WStr *text, int flag) {
    if (text && text->start && text->finish > text->start && text->finish - text->start < 1000 && g_isHost && text->start[0] != L'[') {
        if (handle_command(text->start, (int)(text->finish - text->start))) return;   // the command is not passed to chat - the bot's reply goes instead
    }
    g_chatOrig(channel, text, flag);
}

// ---------- host filter right in the "Game Setup" panel: two dropdowns under "Starting resources" ----------
// Scaleform in the game is 2.x: the public Invoke passes only numbers and strings to the movie, while AddDropDownBox needs
// a live clip. So we call AS functions the same way Invoke does internally (push values onto the AS
// stack and call the function by path), except the clip argument is resolved by path to a real object.
// The list clip is a copy of resources_dropdown_mc (Receive_DuplicateMovieClip, already in the movie), so Elite
// files are not modified. The selection arrives as a regular fscommand "<panel>.OnSelectDropDownItem" "index;item" -
// we hook the engine's fscommand handler and don't pass our indices to the game.
#define FN_STR_NEW   0x00DB64D0     // ASString from char*: __thiscall(string manager, const char*)
#define FN_STR_FREE  0x00DB62D0     // __thiscall(string node), when the refcount (+0xC) reaches 0
#define FN_RESOLVE   0x00DF9520     // path -> value: __thiscall(env, node**, ASValue*, 0,0,0,0)
#define FN_AS_CALL   0x00DD27E0     // call a function by path: __thiscall(root clip, path, ASValue* result, n)
#define FN_STK_GROW  0x00DB7950     // __thiscall(stack env+8)
#define FN_STK_POP   0x00DB7BE0     // __thiscall(stack, n) - pops n values and frees them
#define FN_VAL_FREE  0x00DBA000     // __thiscall(ASValue*) for types >= 5
#define FN_FSCMD     0x00681AB0     // SFMovie::FSCommandHandler::Callback(movie, command, arguments)
static const unsigned char FSCMD_PRO[7] = {0x51, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14};
typedef struct { unsigned char b[16]; } ASValue;            // type in the first byte, value at +4
typedef void *(__fastcall *StrNewFn)(void *mgr, void *edx, const char *s);
typedef void  (__fastcall *ThisFn)(void *self, void *edx);
typedef void *(__fastcall *EnvFn)(void *sprite, void *edx);
typedef char  (__fastcall *ResolveFn)(void *env, void *edx, void **node, ASValue *out, int, int, int, int);
typedef char  (__fastcall *AsCallFn)(void *sprite, void *edx, const char *path, ASValue *res, int n);
typedef void  (__fastcall *PopFn)(void *stk, void *edx, int n);
typedef char  (__fastcall *GetVarFn)(void *movie, void *edx, GFxValue *out, const char *path);   // GFxMovie vtbl+0x30
typedef void  (__fastcall *FsCmdFn)(void *self, void *edx, void *movie, const char *cmd, const char *args);

enum { AV_UNDEF, AV_NUM, AV_BOOL, AV_STR, AV_PATH };
typedef struct { int kind; double num; const char *s; } Arg;

static void *as_str(void *movie, const char *s) {
    void *mgr = (char *)*(void **)((char *)movie + 0x5C4) + 0x238;
    int *n = ((StrNewFn)FN_STR_NEW)(mgr, NULL, s); n[3]++; return n;
}
static void as_str_release(void *n) { if (--((int *)n)[3] == 0) ((ThisFn)FN_STR_FREE)(n, NULL); }

// method - full path of the function; AV_PATH arguments are passed as objects by path
static int as_call(void *movie, const char *method, const Arg *args, int n, double *ret) {
    void *sprite = *(void **)((char *)movie + 0x24); if (!sprite) return 0;
    void *env = ((EnvFn)(*(void ***)sprite)[0x5C / 4])(sprite, NULL);
    char *stk = (char *)env + 8;                             // {top, bottom, limit}
    for (int i = n - 1; i >= 0; i--) {                       // like Invoke: last argument first
        *(char **)stk += 16;
        if (*(char **)stk >= *(char **)(stk + 8)) ((ThisFn)FN_STK_GROW)(stk, NULL);
        unsigned char *v = *(unsigned char **)stk; if (!v) continue;
        memset(v, 0, 16);
        if (args[i].kind == AV_NUM) { v[0] = 3; memcpy(v + 4, &args[i].num, 8); }
        else if (args[i].kind == AV_BOOL) { v[0] = 2; v[4] = args[i].num != 0; }
        else if (args[i].kind == AV_STR) { v[0] = 5; *(void **)(v + 4) = as_str(movie, args[i].s); }
        else if (args[i].kind == AV_PATH) {
            void *node = as_str(movie, args[i].s);
            ((ResolveFn)FN_RESOLVE)(env, NULL, &node, (ASValue *)v, 0, 0, 0, 0);
            as_str_release(node);
        }
    }
    ASValue res; memset(&res, 0, sizeof res);
    char ok = ((AsCallFn)FN_AS_CALL)(sprite, NULL, method, &res, n);
    ((PopFn)FN_STK_POP)(stk, NULL, n);
    if (ret) { *ret = 0; if (res.b[0] == 3) memcpy(ret, res.b + 4, 8); else if (res.b[0] == 2) *ret = res.b[4]; }
    if (res.b[0] >= 5) ((ThisFn)FN_VAL_FREE)(&res, NULL);
    return ok;
}
static int as_get(void *movie, const char *path, GFxValue *v) {
    memset(v, 0, sizeof *v);
    return ((GetVarFn)(*(void ***)movie)[0x30 / 4])(movie, NULL, v, path) != 0;
}
static int as_num(void *movie, const char *path, double *out) {
    GFxValue v; if (!as_get(movie, path, &v) || v.type != GFX_NUMBER) return 0;
    *out = v.v.num; return 1;
}
// panel function with simple arguments: as_fn(movie, "Receive_SetDropDownVisible", "nb", 5.0, 1)
// sig: n - number (double), b - bool (int), s - string, p - clip by path, u - undefined
static char g_panel[256];                                    // absolute panel path, e.g. _level0.xxx
static int as_fn(void *movie, const char *fn, const char *sig, ...) {
    Arg a[24]; int n = 0; va_list ap; va_start(ap, sig);
    for (const char *c = sig; *c && n < 24; c++, n++) {
        a[n].kind = *c == 'n' ? AV_NUM : *c == 'b' ? AV_BOOL : *c == 'p' ? AV_PATH : *c == 'u' ? AV_UNDEF : AV_STR;
        a[n].num = 0; a[n].s = NULL;
        if (*c == 'n') a[n].num = va_arg(ap, double); else if (*c == 'b') a[n].num = va_arg(ap, int);
        else if (*c != 'u') a[n].s = va_arg(ap, const char *);
    }
    va_end(ap);
    char m[400]; snprintf(m, sizeof m, "%s.%s", g_panel, fn);
    return as_call(movie, m, a, n, NULL);
}

#define NBOX 2
static void *g_uiMovie; static int g_resOpen = -1;
static int g_base = -1, g_ui = 1, g_uiStep = 52, g_uiOpenUp = 1; static double g_ddY[2];
static const struct { const char *t; int min, max; } RATING[] = {
    {"Rating: off", 0, 0}, {"Rating 1000+", 1000, 0}, {"Rating 2000+", 2000, 0}, {"Rating 2500+", 2500, 0},
    {"Rating 1400-", 0, 1400}, {"Rating 1000-", 0, 1000},
};
static const struct { const char *t; int games, hours, wr, unknown, maxh, maxwr; } EXP[] = {
    {"Hours / win rate: off", 0, 0, 0, 0}, {"Hours 1000+", 0, 1000, 0, 0}, {"Hours 1000-", 0, 0, 0, 0, 1000},
    {"Win rate 45%+", 0, 0, 45, 0}, {"Win rate 45%-", 0, 0, 0, 0, 0, 45}, {"Win rate 50%+", 0, 0, 50, 0},
};
#define COUNT(a) (int)(sizeof a / sizeof a[0])
static const char *TIP = "LobbyStats host filter (only you see it): players outside the limits get a chat message "
                         "and are kicked a few seconds later. Rating, win rate and games are Relic stats, hours come from Steam.";

static int rating_sel(void) {
    for (int i = 0; i < COUNT(RATING); i++) if (RATING[i].min == G.minRating && RATING[i].max == G.maxRating) return i;
    return -1;
}
static int exp_sel(void) {
    for (int i = 0; i < COUNT(EXP); i++)
        if (EXP[i].games == G.minGames && EXP[i].hours == G.minHours && EXP[i].wr == G.minWinRate && EXP[i].maxwr == G.maxWinRate && EXP[i].maxh == G.maxHours && EXP[i].unknown == G.kickUnknown) return i;
    return -1;
}
// fill the lists from the current rules; rules from ini/command that match no item - "Custom" item
static void ui_fill(void *movie) {
    char custom[160], p[400];
    for (int b = 0; b < NBOX; b++) {
        Arg a[24]; int n = 0, cnt = b ? COUNT(EXP) : COUNT(RATING), sel = b ? exp_sel() : rating_sel();
        a[n++] = (Arg){AV_NUM, g_base + b, 0}; a[n++] = (Arg){AV_STR, 0, ""};
        for (int i = 0; i < cnt; i++) a[n++] = (Arg){AV_STR, 0, b ? EXP[i].t : RATING[i].t};
        if (sel < 0) {
            int o = 0; custom[0] = 0;
            #define PART(...) (o += snprintf(custom + o, sizeof custom - o, __VA_ARGS__), o = o < (int)sizeof custom ? o : (int)sizeof custom - 1)
            if (!b) { PART("Rating"); if (G.minRating && G.maxRating) PART(" %d-%d", G.minRating, G.maxRating);
                      else if (G.minRating) PART(" %d+", G.minRating); else if (G.maxRating) PART(" %d-", G.maxRating); }
            else {
                if (G.minHours) PART("%sHours %d+", o ? ", " : "", G.minHours); if (G.maxHours) PART("%sHours %d-", o ? ", " : "", G.maxHours);
                if (G.minWinRate && G.maxWinRate) PART("%sWin rate %d-%d%%", o ? ", " : "", G.minWinRate, G.maxWinRate);
                else if (G.minWinRate) PART("%sWin rate %d%%+", o ? ", " : "", G.minWinRate); else if (G.maxWinRate) PART("%sWin rate %d%%-", o ? ", " : "", G.maxWinRate);
                if (G.minGames) PART("%sGames %d+", o ? ", " : "", G.minGames);
                if (G.kickUnknown) PART("%sno new accounts", o ? ", " : "");
            }
            #undef PART
            a[n++] = (Arg){AV_STR, 0, custom}; sel = cnt;
        }
        snprintf(p, sizeof p, "%s.Receive_PopulateDropDownList", g_panel); as_call(movie, p, a, n, NULL);
        as_fn(movie, "Receive_SetDropDownSelection", "nn", (double)(g_base + b), (double)sel);
        as_fn(movie, "Receive_SetDropDownTooltip", "ns", (double)(g_base + b), TIP);
        as_fn(movie, "Receive_SetDropDownVisible", "nb", (double)(g_base + b), g_isHost);
        double h; char nm[64];
        snprintf(nm, sizeof nm, "lsf_dd%d", b); as_fn(movie, "Receive_SetVisible", "sb", nm, g_isHost);   // the clip itself: host only                                // near the bottom of the screen the list opens upward
        snprintf(p, sizeof p, "%s.lsf_dd%d.dropdown_list_mc._height", g_panel, b);
        snprintf(nm, sizeof nm, "lsf_dd%d.dropdown_list_mc", b);
        if (g_uiOpenUp && as_num(movie, p, &h) && h > 0) as_fn(movie, "Receive_SetMovieClipY", "sn", nm, -h - 2);
    }
}
// called after the game has filled "Starting resources" (index 4) in panel self
static void ui_setup(void *self) {
    if (!g_ui || IsBadReadPtr(self, 0x2C)) return;
    char *holder = *(char **)((char *)self + 0x28); if (!holder || IsBadReadPtr(holder, 8)) return;
    void *movie = *(void **)(holder + 4); if (!movie) return;
    char rel[256], p[400]; char *b = *(char **)((char *)self + 4), *e = *(char **)((char *)self + 8);
    if (!b || e <= b || e - b > 250) return;
    memcpy(rel, b, e - b); rel[e - b] = 0;
    GFxValue v; double y0, nb, d;
    if (as_get(movie, rel, &v) && v.type == GFX_STRING && v.v.s && strlen(v.v.s) < 250) strcpy(g_panel, v.v.s); else strcpy(g_panel, rel);
    snprintf(p, sizeof p, "%s.resources_dropdown_mc._y", g_panel);
    if (!as_num(movie, p, &y0)) return;                       // wrong panel
    snprintf(p, sizeof p, "%s.generic_dropdown0_mc._y", g_panel);
    if (!as_num(movie, p, &d)) return;
    snprintf(p, sizeof p, "%s.lsf_dd0._y", g_panel);
    if (!as_num(movie, p, &d)) {                              // this panel doesn't have our lists yet
        snprintf(p, sizeof p, "%s.g_dropDownBoxes.length", g_panel);
        if (!as_num(movie, p, &nb)) { logmsg("ui: g_dropDownBoxes not found in %s", g_panel); g_base = -1; return; }
        g_base = (int)nb;
        memset(g_refMark, 0, sizeof g_refMark);              // new lobby - no referees yet
        for (int k = 0; k < NBOX; k++) {                      // create the upper one first: the lower one's list opens over it
            char nm[32], path[300]; snprintf(nm, sizeof nm, "lsf_dd%d", k); snprintf(path, sizeof path, "%s.%s", g_panel, nm);
            as_fn(movie, "Receive_DuplicateMovieClip", "ss", "resources_dropdown_mc", nm);
            as_fn(movie, "Receive_SetVisible", "sb", nm, 1);
            as_fn(movie, "Receive_SetMovieClipY", "sn", nm, y0 + g_uiStep * (k + 1));
            g_ddY[k] = y0 + g_uiStep * (k + 1);
            as_fn(movie, "AddDropDownBox", "punb", path, 2.0, 1);   // FLAG_HIDE_UP_WHEN_OVER, wide
        }
        snprintf(p, sizeof p, "%s.g_dropDownBoxes.length", g_panel);
        double after = 0; as_num(movie, p, &after);
        logmsg("ui: filter lists added to %s as dropdowns %d..%d (boxes now %d), y=%.0f", g_panel, g_base, g_base + NBOX - 1, (int)after, y0);
        if ((int)after != g_base + NBOX) { logmsg("ui: AddDropDownBox did not register, giving up"); g_base = -1; return; }
    }
    if (g_base >= 0) {
        g_uiMovie = movie; g_resOpen = -1;
        ui_fill(movie);
        // our lists go on top (lower above upper: its list opens upward over the upper one);
        // they don't cover the open "Starting resources" list - while it is open, ours are hidden (ui_poll)
        int ok0 = as_fn(movie, "lsf_dd0.swapDepths", "n", 16001.0);
        int ok1 = as_fn(movie, "lsf_dd1.swapDepths", "n", 16002.0);
        static int logged; if (logged++ < 3) logmsg("ui: filter lists raised to the top (swapDepths %d %d)", ok0, ok1);
    }
}
// player became/stopped being host - show/hide; movie - the shared frontend movie (one for all panels)
// every frame: while the "Starting resources" list is open, our lists are hidden (otherwise they lie on top of it)
// movie - live pointer from the current game call (fscommand); a saved pointer is not used: after leaving
// the lobby it may point to a deleted object (this is why 0.6.5-0.6.6 crashed when hosting again)
static void ui_poll(void *movie) {
    if (g_base < 0 || !movie || !g_panel[0]) return;
    char p[400]; GFxValue v; snprintf(p, sizeof p, "%s.resources_dropdown_mc.dropdown_list_mc._visible", g_panel);
    if (!as_get(movie, p, &v)) return;                         // no panel (not our movie, or left the lobby)
    g_uiMovie = movie; g_lobbyFsTick = GetTickCount();
    if (g_uiRefill) { g_uiRefill = 0; ui_fill(movie); g_resOpen = -1; }   // rules changed by a chat command
    int open = v.type == GFX_BOOL && v.v.b;
    if (g_logFs) { static int n; if (n++ < 300) logmsg("poll: resources list _visible type=%d val=%d -> open=%d (was %d)", v.type, v.type == GFX_BOOL ? v.v.b : -1, open, g_resOpen); }
    if (open == g_resOpen) return;
    g_resOpen = open;
    // hiding via _visible doesn't work (the box stays on screen) - move the clips off screen and back
    for (int b = 0; b < NBOX; b++) {
        char nm[16]; snprintf(nm, sizeof nm, "lsf_dd%d", b);
        if (!g_ddY[b]) { char p2[400]; snprintf(p2, sizeof p2, "%s.%s._y", g_panel, nm); as_num(g_uiMovie, p2, &g_ddY[b]); }
        as_fn(g_uiMovie, "Receive_SetMovieClipY", "sn", nm, open ? -5000.0 : g_ddY[b]);
    }
}
// the player list lives next to the settings panel: ...panel_mc.player_list_panel_mc
static int pl_path(char *out, size_t n) {
    const char *q = strstr(g_panel, "game_options_panel_mc"); if (!q) return 0;
    snprintf(out, n, "%.*splayer_list_panel_mc", (int)(q - g_panel), g_panel); return 1;
}
static int as_bool(void *movie, const char *path) { GFxValue v; return as_get(movie, path, &v) && v.type == GFX_BOOL && v.v.b; }
// row index in the observer list -> lobby slot (ConvertObserverIndexToSlotIndex in player_list_panel)
static int obs_slot(void *movie, const char *pl, int i) {
    char p[400]; double per = 3, maxp = 6;
    snprintf(p, sizeof p, "%s.g_playersPerTeam", pl); as_num(movie, p, &per);
    snprintf(p, sizeof p, "%s.g_scenarioMaxPlayers", pl); as_num(movie, p, &maxp);
    snprintf(p, sizeof p, "%s.g_isFFASolo", pl); int ffa = as_bool(movie, p);
    snprintf(p, sizeof p, "%s.g_isFFATeam", pl); ffa |= as_bool(movie, p);
    if (ffa) return (int)maxp + i;
    int half = (8 - (int)per * 2) / 2;
    return i >= half ? i - half + 4 + (int)per : i + (int)per;
}
// the frontend movie is kept alive while we use it from the frame hook (one reference, dropped when the lobby is gone)
static void *g_refMovie;
static void ref_drop(void) {
    if (!g_refMovie) return;
    LONG *rc = (LONG *)((char *)g_refMovie + 4);
    if (*rc > 1) InterlockedDecrement(rc);                  // if we are the last owner, leak it rather than free it here
    g_refMovie = NULL;
}
static void ref_hold(void *movie) {
    if (movie == g_refMovie) return;
    ref_drop();
    if (IsBadWritePtr((char *)movie + 4, 4)) return;
    LONG rc = *(LONG *)((char *)movie + 4);
    if (rc <= 0 || rc > 10000) { static int n; if (n++ < 3) logmsg("referee: unexpected movie refcount %ld, button off", rc); return; }
    InterlockedIncrement((LONG *)((char *)movie + 4)); g_refMovie = movie;
    logmsg("referee: holding the lobby movie (refcount was %ld)", rc);
}
// hammer look: the button clip has state layers (up / toggled_on / over / down) that nothing switches now
static void ref_paint(void *movie, const char *row, int on) {
    static const char *layer[] = {"up", "over", "down", "toggled_on"};
    char p[420];
    for (int k = 0; k < 4; k++) {
        snprintf(p, sizeof p, "%s.ref_btn.%s", row, layer[k]);
        as_fn(movie, "Receive_SetVisible", "sb", p, k == 0 ? !on : k == 3 ? on : 0);
    }
}
static void ref_show(void *movie) {
    char pl[300], p[400]; if (!pl_path(pl, sizeof pl)) return;
    snprintf(p, sizeof p, "%s.observer_list_panel_mc.observer_00_mc._y", pl);
    double d; if (!as_num(movie, p, &d)) return;
    for (int i = 0; i < 6; i++) {
        char row[360]; snprintf(row, sizeof row, "%s.observer_list_panel_mc.observer_%02d_mc", pl, i);
        snprintf(p, sizeof p, "%s.ref_btn", row);
        as_fn(movie, "Receive_SetVisible", "sb", p, g_isHost);
        int slot = obs_slot(movie, pl, i);
        if (g_isHost) ref_paint(movie, row, slot >= 0 && slot < 16 && g_refMark[slot]);
    }
    if (g_isHost) ref_hold(movie); else ref_drop();
}
// frame hook: a left click on a visible hammer toggles the mark of that observer slot
static void ref_click_poll(void) {
    static int down;
    int now = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0, press = now && !down;
    down = now;
    if (!press || !g_refMovie || !g_isHost || !g_panel[0]) return;
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); if (pid != GetCurrentProcessId()) return;
    void *movie = g_refMovie; char pl[300], p[400], row[360];
    if (!pl_path(pl, sizeof pl)) return;
    snprintf(p, sizeof p, "%s.observer_list_panel_mc.observer_00_mc._y", pl);
    double d; if (!as_num(movie, p, &d)) { ref_drop(); return; }   // lobby is gone
    for (int i = 0; i < 6; i++) {
        double x, y, w, h;
        snprintf(row, sizeof row, "%s.observer_list_panel_mc.observer_%02d_mc", pl, i);
        snprintf(p, sizeof p, "%s._visible", row); if (!as_bool(movie, p)) continue;
        snprintf(p, sizeof p, "%s.ref_btn._visible", row); if (!as_bool(movie, p)) continue;
        snprintf(p, sizeof p, "%s.ref_btn._xmouse", row); if (!as_num(movie, p, &x)) continue;
        snprintf(p, sizeof p, "%s.ref_btn._ymouse", row); if (!as_num(movie, p, &y)) continue;
        snprintf(p, sizeof p, "%s.ref_btn._width", row); if (!as_num(movie, p, &w)) continue;
        snprintf(p, sizeof p, "%s.ref_btn._height", row); if (!as_num(movie, p, &h)) continue;
        if (g_logFs) logmsg("referee: row %d mouse %.0f,%.0f size %.0fx%.0f", i, x, y, w, h);
        if (x < 0 || y < 0 || x > w || y > h) continue;
        int slot = obs_slot(movie, pl, i);
        if (slot < 0 || slot > 15 || g_slotType[slot] != 5) { logmsg("referee: row %d (slot %d) has nobody in it", i, slot); return; }
        g_refMark[slot] ^= 1;
        snprintf(p, sizeof p, "%s.referee_txt", row);
        as_fn(movie, "Receive_SetVisible", "sb", p, g_refMark[slot]);
        ref_paint(movie, row, g_refMark[slot]);
        logmsg("referee: slot %d %s", slot, g_refMark[slot] ? "marked" : "unmarked");
        return;
    }
}
static void ui_host_changed(void *self) {
    if (g_base < 0 || !g_panel[0] || IsBadReadPtr(self, 0x2C)) return;
    char *holder = *(char **)((char *)self + 0x28); if (!holder || IsBadReadPtr(holder, 8)) return;
    void *movie = *(void **)(holder + 4); if (!movie) return;
    char p[400]; double d; snprintf(p, sizeof p, "%s.lsf_dd0._y", g_panel);
    if (!as_num(movie, p, &d)) { g_base = -1; return; }       // panel is gone
    for (int b = 0; b < NBOX; b++) {
        char nm[16]; snprintf(nm, sizeof nm, "lsf_dd%d", b);
        as_fn(movie, "Receive_SetDropDownVisible", "nb", (double)(g_base + b), g_isHost);
        as_fn(movie, "Receive_SetVisible", "sb", nm, g_isHost);
    }
    ref_show(movie);
}
static void ui_select(int box, int item) {
    if (box == 0 && item < COUNT(RATING)) { G.minRating = RATING[item].min; G.maxRating = RATING[item].max; }
    else if (box == 1 && item < COUNT(EXP)) {
        G.minGames = EXP[item].games; G.minHours = EXP[item].hours; G.minWinRate = EXP[item].wr; G.maxWinRate = EXP[item].maxwr; G.maxHours = EXP[item].maxh; G.kickUnknown = EXP[item].unknown;
    } else return;                                            // "Custom" - leave as is
    G.enabled = G.minRating || G.maxRating || G.minGames || G.minHours || G.maxHours || G.minWinRate || G.maxWinRate || G.kickUnknown;
    G.dryRun = 0;                                             // picked in the menu - so kick for real (test mode: ini or !filter test)
    save_guard(); g_recheck = 1;
    wchar_t st[256]; guard_status(st, 256); chat_send(st);
    char u[512]; WideCharToMultiByte(CP_UTF8, 0, st, -1, u, sizeof u, NULL, NULL); logmsg("ui: box %d item %d -> %s", box, item, u);
}
static FsCmdFn g_fsOrig;
static void __fastcall fs_hook(void *self, void *edx, void *movie, const char *cmd, const char *args) {
    // UI click: the "Starting resources" list opens only AFTER the click sound - check the next 30 frames
    if (cmd && args && movie && g_base >= 0 && !strcmp(cmd, "OnPlaySound") && strstr(args, "click")) { g_uiMovie = movie; g_pollFrames = 30; }
    if (g_logFs && cmd) { static int n; if (n++ < 400) logmsg("fs: %s (%s)", cmd, args ? args : ""); }   // debug: which events the UI sends
    if (g_base >= 0 && movie && cmd && strstr(cmd, "OnSelectDropDownItem") == NULL) ui_poll(movie);
    if (g_base >= 0 && cmd && args) {
        const char *dot = strrchr(cmd, '.');
        int sel = dot && !strcmp(dot, ".OnSelectDropDownItem"), roll = dot && !strcmp(dot, ".OnRollOverDropDownItem");
        int idx, item;
        if ((sel || roll) && sscanf(args, "%d;%d", &idx, &item) == 2 && idx >= g_base && idx < g_base + NBOX) {
            size_t pl = strlen(g_panel);
            if ((size_t)(dot - cmd) == pl && !strncmp(cmd, g_panel, pl)) {
                if (sel) ui_select(idx - g_base, item);
                return;                                       // our list - not passed to the game
            }
            static int warned; if (!warned++) logmsg("ui: %s from %s, our panel is %s - passing to the game", dot + 1, cmd, g_panel);
        }
    }
    g_fsOrig(self, edx, movie, cmd, args);
    if (g_uiRefill && g_base >= 0 && movie) ui_poll(movie);   // a chat command was applied inside this call - update the menu right away
}

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
    g_isHost = isHost; g_nseen = 0;
    unsigned long long ids[32] = {0}; int nids = ids_by_position(ids, 32) ? 32 : 0;
    // own ID - from the slot the game marked as "own" (type 0); calling local_user_id() from 0.6.8 crashed the game for a user
    g_localId = 0;
    for (unsigned i = 0; i < (count - HEADER_ARGS) / SLOT_ARGS && i < 16; i++) {
        GFxValue *a0 = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a0[A_SLOTTYPE].type == GFX_NUMBER && (int)a0[A_SLOTTYPE].v.num == SLOT_LOCAL && a0[A_SLOTINDEX].type == GFX_NUMBER) {
            int p0 = (int)a0[A_SLOTINDEX].v.num; if (p0 >= 0 && p0 < nids && ids[p0]) g_localId = ids[p0];
        }
    }
    {   // identical IDs at different positions = mapping is broken -> kicking disabled
        int dup = 0;
        for (int x = 0; x < nids; x++) for (int y = x + 1; y < nids; y++) if (ids[x] && ids[x] == ids[y]) dup = 1;
        g_localIdForKick = dup ? 0 : g_localId;
        if (dup) { static int w; if (w++ < 3) logmsg("guard: duplicate ids in lobby data - kicks disabled for safety"); }
    }
    unsigned slots = (count - HEADER_ARGS) / SLOT_ARGS; int want = 0;
    EnterCriticalSection(&g_lock);
    for (unsigned i = 0; i < slots && i < 16; i++) {
        GFxValue *a = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a[A_HASUSER].type != GFX_BOOL || !a[A_HASUSER].v.b) continue;
        if (a[A_SLOTTYPE].type != GFX_NUMBER) continue;
        int st = (int)a[A_SLOTTYPE].v.num;
        // slot type: 0 own, 1 other player; observers may have another - rating is shown to everyone alive, filter applies to players only
        if (a[A_NAME].type != GFX_WSTRING || !a[A_NAME].v.ws || !a[A_NAME].v.ws[0]) continue;
        if (a[A_TOOLTIP].type != GFX_WSTRING) continue;          // layout isn't what we expect - leave it alone
        const wchar_t *name = a[A_NAME].v.ws;
        unsigned long long id = 0;
        int si = a[A_SLOTINDEX].type == GFX_NUMBER ? (int)a[A_SLOTINDEX].v.num : -1;
        if (si >= 0 && si < nids && ids[si]) id = ids[si];       // player ID by slot position (for kicking)
        int observer = g_maxPlayers > 0 && si >= g_maxPlayers;  // observer: position beyond the player seats
        wchar_t key[64];
        if ((id >> 32) == 0x01100001) _snwprintf(key, 63, L"/steam/%llu", id); else wcsncpy(key, name, 63);   // stats are looked up by SteamID or by name
        key[63] = 0;
        if (g_nseen < 16) {
            Seen *sn = &g_seen[g_nseen++]; wcsncpy(sn->key, key, 63); sn->key[63] = 0; wcsncpy(sn->name, name, 63); sn->name[63] = 0;
            sn->id = id; sn->remote = st == SLOT_REMOTE && !observer && id != g_localId;   // rechecking - players only
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
        if (G.enabled && isHost && st == SLOT_REMOTE && !observer && id != g_localId && (e->known || e->absent)) guard_check(key, name, e, id);
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
    {   // lobby roster changed - log who has a SteamID (kicking is impossible without it)
        static unsigned long long lastSig; unsigned long long sig = nids * 1315423911ULL + g_nseen;
        for (int k = 0; k < g_nseen; k++) sig = sig * 31 + g_seen[k].id + g_seen[k].remote * 7;
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
    int players = 0, resources = 0;
    if (method && args && !strcmp(method, "Receive_PopulatePlayerList")) {
        if (count < 4096 && !IsBadReadPtr(args, count * sizeof(GFxValue))) {
            g_inDecorate = 1; decorate(args, count); g_inDecorate = 0; players = 1;
        }
    }
    // "Starting resources" (5th list of the settings panel) is filled - so the panel is ready, add the filter
    if (method && args && count >= 1 && !strcmp(method, "Receive_SetMaxPlayers") && !IsBadReadPtr(args, sizeof(GFxValue)) && args[0].type == GFX_NUMBER)
        g_maxPlayers = (int)args[0].v.num;
    if (method && args && count >= 2 && !strcmp(method, "Receive_PopulateDropDownList") && !IsBadReadPtr(args, sizeof(GFxValue))
        && args[0].type == GFX_NUMBER && args[0].v.num == 4) resources = 1;
    char r = g_orig(self, edx, method, flags, args, count);
    if (resources) ui_setup(self);
    if (players) ui_host_changed(self);
    guard_tick();
    return r;
}

// once per frame (main thread): bot tasks run on time, not only on UI updates
#define IAT_SHARK_UPDATE 0x00F88068
typedef void (__stdcall *UpdFn)(float);
static UpdFn g_updOrig;
static void __stdcall frame_hook(float dt) {
    g_updOrig(dt);
    if (g_logFs) { static int n; if (!(n++ % 500)) logmsg("frame hook: call %d, poll frames left %d", n, g_pollFrames); }
    if (!g_inDecorate) { hook_chat_service(); guard_tick(); }
    // the !filter command comes from the chat movie - update the menu via the lobby movie if it is alive (lobby action < 20 s ago)
    if (g_uiRefill && g_uiMovie && g_base >= 0 && GetTickCount() - g_lobbyFsTick < 20000 && !g_pollFrames) g_pollFrames = 1;
    if (g_pollFrames > 0 && g_uiMovie) { g_pollFrames--; ui_poll(g_uiMovie); }
    ref_click_poll();
}
static void hook_frame(void) {
    HMODULE m = GetModuleHandleA("Shark.dll"); MODULEINFO mi; DWORD old;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || IsBadReadPtr((void *)IAT_SHARK_UPDATE, 4)) return;
    DWORD cur = *(DWORD *)IAT_SHARK_UPDATE;
    if (cur < (DWORD)mi.lpBaseOfDll || cur >= (DWORD)mi.lpBaseOfDll + mi.SizeOfImage) { logmsg("frame hook: IAT slot is not Shark.dll (already hooked by another DLL?) - chaining"); }
    if (!VirtualProtect((void *)IAT_SHARK_UPDATE, 4, PAGE_READWRITE, &old)) return;
    g_updOrig = (UpdFn)cur; *(DWORD *)IAT_SHARK_UPDATE = (DWORD)frame_hook;
    VirtualProtect((void *)IAT_SHARK_UPDATE, 4, old, &old);
    logmsg("frame hook installed");
}

// Referee. The observer rows in the lobby Flash still have the referee button (orange hammer, ref_btn) and
// the "Referee" label (referee_txt), but both are hidden and the button has no handler. The engine side is
// cut too: InvokePopulatePlayerList pushes m_isReferee as a constant false (xor al,al @ 0x4917C6) and the
// slot's referee byte (+0x54) is never set. We show the button to the host, catch clicks on it ourselves and
// keep the mark per slot; the patched push makes the list draw the label. The mark is local to the host.
#define REF_PATCH 0x004917C6
static const unsigned char REF_ORIG[10] = {0x32, 0xC0, 0x8D, 0x4D, 0xCC, 0xE8, 0xB5, 0xFF, 0xFC, 0xFF};
__attribute__((used)) int ref_flag(const int *slot) {
    if (IsBadReadPtr(slot, 0xA0)) return 0;
    unsigned idx = slot[0x98 / 4] & 15;
    g_slotType[idx] = (unsigned char)slot[0];
    if (slot[0] != 5) g_refMark[idx] = 0;                    // 5 = observer slot with a user in it
    return g_refMark[idx];
}
void ref_stub(void);
__asm__(".text\n_ref_stub:\n"
        "pushl %ebx\n call _ref_flag\n addl $4, %esp\n"
        "leal -0x34(%ebp), %ecx\n movl $0x461785, %edx\n call *%edx\n"
        "movl $0x4917D0, %edx\n jmp *%edx\n");
static void hook_referee(void) {
    unsigned char *t = (unsigned char *)REF_PATCH;
    if (IsBadReadPtr(t, 10) || memcmp(t, REF_ORIG, 10)) { logmsg("referee flag: code not recognised"); return; }
    DWORD o; VirtualProtect(t, 10, PAGE_EXECUTE_READWRITE, &o);
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)ref_stub - (DWORD)(t + 5); memset(t + 5, 0x90, 5);
    VirtualProtect(t, 10, o, &o); FlushInstructionCache(GetCurrentProcess(), t, 10);
    logmsg("referee button restored for the host");
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
#ifdef LITE
    // lite build: only the stats tooltip - no filter, kick, chat commands or new menus
    logmsg("hooked Invoke at %08X (lite: stats tooltip only, no filter/kick/chat/menus)", HOOK_ADDR);
    return;
#endif
    hook_frame();
    hook_referee();
    logmsg("hooked Invoke at %08X", HOOK_ADDR);
    static const unsigned char CHAT_PRO[5] = {0xB8, 0xD9, 0xEC, 0xF0, 0x00};   // mov eax, 0x00F0ECD9 @ 0x4023DC
    unsigned char *ct = (unsigned char *)FN_CHAT_SEND;
    if (!IsBadReadPtr(ct, 5) && !memcmp(ct, CHAT_PRO, 5)) {
        unsigned char *t2 = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t2, CHAT_PRO, 5); t2[5] = 0xE9; *(DWORD *)(t2 + 6) = (FN_CHAT_SEND + 5) - (DWORD)(t2 + 10);
        g_chatOrig = (ChatFn)t2;
        DWORD o2; VirtualProtect(ct, 5, PAGE_EXECUTE_READWRITE, &o2);
        ct[0] = 0xE9; *(DWORD *)(ct + 1) = (DWORD)chat_hook - (DWORD)(ct + 5);
        VirtualProtect(ct, 5, o2, &o2); FlushInstructionCache(GetCurrentProcess(), ct, 5);
        logmsg("hooked lobby chat at %08X (host commands: !filter)", FN_CHAT_SEND);
    }
    unsigned char *ft = (unsigned char *)FN_FSCMD;
    if (g_ui && !IsBadReadPtr(ft, 7) && !memcmp(ft, FSCMD_PRO, 7)) {
        unsigned char *t3 = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        memcpy(t3, FSCMD_PRO, 7); t3[7] = 0xE9; *(DWORD *)(t3 + 8) = (FN_FSCMD + 7) - (DWORD)(t3 + 12);
        g_fsOrig = (FsCmdFn)t3;
        DWORD o3; VirtualProtect(ft, 7, PAGE_EXECUTE_READWRITE, &o3);
        ft[0] = 0xE9; *(DWORD *)(ft + 1) = (DWORD)fs_hook - (DWORD)(ft + 5); ft[5] = 0x90; ft[6] = 0x90;
        VirtualProtect(ft, 7, o3, &o3); FlushInstructionCache(GetCurrentProcess(), ft, 7);
        logmsg("hooked fscommand at %08X (filter lists in Game Setup)", FN_FSCMD);
    } else if (g_ui) { g_ui = 0; logmsg("fscommand handler not recognised, no filter lists in Game Setup"); }
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
    wchar_t *ini = g_ini; _snwprintf(ini, MAX_PATH, L"%lsLobbyStats.ini", g_dir);
    g_showInName = GetPrivateProfileIntW(L"LobbyStats", L"ShowRatingInName", 1, ini);
    g_stats       = GetPrivateProfileIntW(L"LobbyStats", L"Stats", 1, ini);
    GetPrivateProfileStringW(L"LobbyStats", L"Server", STATS_HOST, g_server, 128, ini);
    if (!g_server[0]) { g_stats = 0; }                        // stats service not set - there will be no numbers
    G.enabled     = GetPrivateProfileIntW(L"Guard", L"Enabled", 0, ini);
    G.dryRun      = GetPrivateProfileIntW(L"Guard", L"DryRun", 1, ini);
    G.minRating   = GetPrivateProfileIntW(L"Guard", L"MinRating", 0, ini);
    G.maxRating   = GetPrivateProfileIntW(L"Guard", L"MaxRating", 0, ini);
    G.minWinRate  = GetPrivateProfileIntW(L"Guard", L"MinWinRate", 0, ini);
    G.maxWinRate  = GetPrivateProfileIntW(L"Guard", L"MaxWinRate", 0, ini);
    G.minGames    = GetPrivateProfileIntW(L"Guard", L"MinGames", 0, ini);
    G.minHours    = GetPrivateProfileIntW(L"Guard", L"MinHours", 0, ini);
    G.maxHours    = GetPrivateProfileIntW(L"Guard", L"MaxHours", 0, ini);
    G.kickUnknown = GetPrivateProfileIntW(L"Guard", L"KickUnknown", 0, ini);
    G.delaySec    = GetPrivateProfileIntW(L"Guard", L"KickDelaySeconds", 5, ini);
    g_ui          = GetPrivateProfileIntW(L"UI", L"FilterLists", 1, ini);
    g_logFs       = GetPrivateProfileIntW(L"UI", L"LogFsCommands", 0, ini);
    g_general     = GetPrivateProfileIntW(L"Chat", L"GeneralChannel", 0, ini);
    wcscpy(g_iniPath, ini);
    g_uiStep      = GetPrivateProfileIntW(L"UI", L"FilterListsStep", 52, ini);
    g_uiOpenUp    = GetPrivateProfileIntW(L"UI", L"FilterListsOpenUp", 1, ini);
#ifdef LITE
    G.enabled = 0; g_ui = 0;                                  // no filter or menu at all in this build
    g_showInName = GetPrivateProfileIntW(L"LobbyStats", L"ShowRatingInName", 1, ini);   // rating next to the name - on by default
#endif
    InitializeCriticalSection(&g_lock); g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
#ifdef LITE
    logmsg("LobbyStats Lite v0.6.17");
#else
    logmsg("LobbyStats v0.6.20");
#endif
    if (already_loaded("LobbyStats")) { logmsg("another LobbyStats copy is already active in this game, this one stays idle"); return TRUE; }
    load_cache(); install();
    if (G.enabled) logmsg("guard: on, dry-run=%d, rating %d..%d, win rate %d..%d%%, min games=%d hours=%d, kick unknown=%d, delay=%ds",
                          G.dryRun, G.minRating, G.maxRating, G.minWinRate, G.maxWinRate, G.minGames, G.minHours, G.kickUnknown, G.delaySec);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}

// for testing outside the game: run the substitution and rules on a fake argument set
__declspec(dllexport) int LobbyStats_Test(GFxValue *args, unsigned count) { decorate(args, count); return g_ncache; }
__declspec(dllexport) void LobbyStats_SetGuard(int enabled, int dry, int minRating, int minGames, int minHours, int kickUnknown) {
    G.enabled = enabled; G.dryRun = dry; G.minRating = minRating; G.minGames = minGames; G.minHours = minHours; G.kickUnknown = kickUnknown; G.delaySec = 5;
}
__declspec(dllexport) void LobbyStats_SetGuard2(int maxRating, int minWinRate, int maxWinRate) { G.maxRating = maxRating; G.minWinRate = minWinRate; G.maxWinRate = maxWinRate; }
__declspec(dllexport) int LobbyStats_Jobs(wchar_t *out, int cap) {
    int o = 0; out[0] = 0;
    for (int i = 0; i < g_njobs; i++) o += _snwprintf(out + o, cap - o, L"%ls%ls\n", g_jobs[i].kick ? L"KICK " : L"CHAT ", g_jobs[i].kick ? L"" : g_jobs[i].text);
    return g_njobs;
}

__declspec(dllexport) int LobbyStats_Command(const wchar_t *t, wchar_t *status, int cap) {
    g_isHost = 1; int r = handle_command(t, (int)wcslen(t)); guard_status(status, cap); return r;
}
// for testing outside the game: picking an item in a filter list, and which item the lists would show for the current rules
__declspec(dllexport) void LobbyStats_UiSelect(int box, int item, wchar_t *status, int cap, int *selRating, int *selExp) {
    ui_select(box, item); guard_status(status, cap); *selRating = rating_sel(); *selExp = exp_sel();
}
