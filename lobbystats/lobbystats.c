// LobbyStats.dll — рейтинг Relic прямо в лобби DoW2 Retribution / Elite.
//
// Как работает: перехватываем вызов движка «вызвать функцию Flash» (FUN_0045db6c, __thiscall
// Invoke(method, flags, GFxValue* args, count)). Когда движок отправляет в ролик лобби
// Receive_PopulatePlayerList, в каждом слоте игрока дописываем рейтинг к нику и подменяем
// всплывающую подсказку слота (поле m_tooltip) на «рейтинг / победы / игры / часы».
// Данные берёт фоновый поток с сервиса статистики (<Server>/stats/api/lobby), который читает публичный API Relic.
// Первый показ ника — без цифр; они появятся при следующем обновлении лобби (вход, готовность,
// смена расы) и сразу в следующих лобби (кэш на диске рядом с DLL).
//
// LobbyGuard (по желанию хоста, секция [Guard] в LobbyStats.ini, по умолчанию выключен): если зашедший
// игрок не проходит правила (рейтинг/игры/часы), бот пишет в общий чат лобби причину и через несколько
// секунд кикает его той же функцией, что и кнопка кика у хоста. DryRun=1 — только сообщение, без кика.
// Игроков ищем по SteamID64 из данных слота (ник — только запасной вариант).
// Правила хост выбирает в двух выпадающих списках под «Starting resources» (видны только хосту),
// а также командой !filter в чате лобби.
//
// Подходит только к DOW2.exe 3.19 (Steam): перед установкой проверяем байты пролога.
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

// адрес сервиса статистики: при сборке -DSTATS_HOST=L\"host\" или в ini [LobbyStats] Server=host
#ifndef STATS_HOST
#define STATS_HOST L""
#endif
static wchar_t g_server[128] = STATS_HOST;
#define SERVER_PATH "/stats/api/lobby"

// GFxValue (Scaleform 2.x в игре): 16 байт. Тип в первом слове, значение с +8.
typedef struct { int type; int pad; union { double num; unsigned char b; const wchar_t *ws; const char *s; } v; } GFxValue;
enum { GFX_BOOL = 2, GFX_NUMBER = 3, GFX_STRING = 4, GFX_WSTRING = 5 };
enum { HEADER_ARGS = 5, SLOT_ARGS = 20, A_HASUSER = 0, A_SLOTINDEX = 3, A_SLOTTYPE = 4, A_NAME = 5, A_TOOLTIP = 19 };
enum { SLOT_LOCAL = 0, SLOT_REMOTE = 1 };

typedef struct { wchar_t name[64]; int known, absent; int rating, wins, losses, hours; DWORD fetched; } Entry;  // absent: server said «no Relic profile»
#define MAX_ENTRIES 256
static Entry g_cache[MAX_ENTRIES]; static int g_ncache;
static CRITICAL_SECTION g_lock; static HANDLE g_wake;
static wchar_t g_pending[16][64]; static int g_npending;
static wchar_t g_dir[MAX_PATH];
static volatile int g_fresh; static volatile int g_uiRefill; static int g_logFs; static int g_pollFrames; static DWORD g_lobbyFsTick;                                 // пришли новые данные с сервера — перепроверить лобби
static int g_showInName = 1, g_stats = 1;                  // g_stats=0: не трогать ники и подсказки (фильтр работает)

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

// ---------- кэш на диске: строки «ник\tрейтинг\tпобеды\tпоражения\tчасы» в UTF-8 ----------
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

// ---------- сеть ----------
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
        Sleep(300);                                  // собираем все ники одного обновления лобби в один запрос
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
        if (got > 0) { parse_lines(resp, 0); save_cache(); g_fresh = 1; }   // данные пришли — проверить зашедших (guard_tick)
        else for (int i = 0; i < n; i++) find_or_add(names[i])->fetched = GetTickCount();   // не долбим сервер при ошибке
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

// ---------- функции игры (DOW2.exe 3.19, адреса фиксированы, ASLR нет) ----------
#define G_SESSION      0x01335014     // глобальный менеджер сети; vtbl+0x24 -> объект матча/лобби
#define FN_INFO_CTOR   0x0040172D     // __stdcall(GameInfo*)      — как в PlayerListPanel::OnKickPlayer
#define FN_INFO_DTOR   0x00401794
#define FN_ST_CTOR     0x0041F1E5     // __stdcall(StationInfo*)
#define FN_ST_DTOR     0x00401A14
#define FN_CHAT_SEND   0x004023DC     // __stdcall(int channel 0=все, WStr*, char) — отправка текста чата лобби
#define INFO_SLOTS     0x70           // GameInfo: начало массива слотов
#define INFO_NSLOTS    0x78           //           число слотов
#define SLOT_SIZE      0xC8           // слот: +8/+0xC — SteamID64 игрока
typedef void  (__stdcall *Fn1)(void *);
typedef void *(__fastcall *GetMatchFn)(void *self, void *edx);
typedef char  (__stdcall *GetInfoFn)(void *self, void *info);
typedef char  (__stdcall *StationFn)(void *self, DWORD lo, DWORD hi, void *st);
typedef char  (__stdcall *KickFn)(void *self, DWORD lo, DWORD hi, int code, const char *why);
typedef void  (__stdcall *ChatFn)(int channel, void *text, int flag);
typedef struct { wchar_t *start, *finish, *eos; } WStr;          // строка STLport: начало, конец, конец буфера

static int g_hooked;
static void *match_obj(void) {
    void **mgr = *(void ***)G_SESSION; if (!mgr) return NULL;
    return ((GetMatchFn)(*(void ***)mgr)[0x24 / 4])(mgr, NULL);
}
// SteamID64 каждого слота лобби по индексу; возвращает число слотов
// SteamID слотов из настроек матча — так их берёт сама игра (PostGameStatsForm canContinue, 0x47F959):
// settings = *(0x1335A78); слоты = *(settings+0x928), число = *(settings+0x92C), шаг 0xA8; слот: +0 тип, +0x58 команда, +0x68 SteamID64.
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
// ID игроков по ПОЗИЦИИ слота — как строит список лобби сама игра (InvokePopulatePlayerList 0x4911C4):
// GameInfo (матч vtbl+4): слоты *(+0x70), число *(+0x78), шаг 0xC8; в слоте +8 = ID игрока (ID профиля, не SteamID; -1 = пусто),
// +0x98 = позиция (её интерфейс получает как номер слота). Свой ID — как у игры: 0x401607 + 0x53A400, ID в первых 8 байтах.
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
    for (int i = 0; i < n; i++) if (ids[i]) good++;          // это может быть и не SteamID, а ID профиля игры — кику подходит любой
    static int logged; if (logged++ < 5) { char b[300]; int o = 0;
        for (int i = 0; i < n && o < 280; i++) o += snprintf(b + o, sizeof b - o, " %llu", ids[i]);
        logmsg("ids from match settings (%d slots):%s", n, n ? b : " none"); }
    if (good) return n;
    memset(ids, 0, sizeof(*ids) * cap);
    return slot_ids_gameinfo(ids, cap);                      // запасной путь (как в 0.5)
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
static ChatFn g_chatOrig;                                  // трамплин к настоящей отправке (после установки перехвата)
static void chat_send(const wchar_t *text) {
    if (!g_hooked) return;                                   // вне игры (тесты) — некуда отправлять
    static wchar_t buf[256]; wcsncpy(buf, text, 255); buf[255] = 0;
    WStr s = { buf, buf + wcslen(buf), buf + wcslen(buf) + 1 };
    (g_chatOrig ? g_chatOrig : (ChatFn)FN_CHAT_SEND)(0, &s, 0);
}
// ---------- сообщение в общий канал (General): как это делает сама игра, когда вы пишете во вкладке канала ----------
// Чат-сервис = *(G_SESSION)->vtbl+0x10(); отправка в канал = сервис->vtbl+0x1C(сервис, номер канала, сообщение),
// сообщение собирает 0x4029C1 (eax = строка {начало, конец}, ecx = результат), освобождает 0x537830 (esi = указатель).
// Номер канала General узнаём, когда хост сам пишет в обычный канал (особые 0x10000001..4 — матч/пати — пропускаем).
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
static void hook_chat_service(void) {                         // один раз, из главного потока
    if (g_chanOrig || !g_hooked) return;
    void *svc = chat_service(); if (!svc) return;
    void **vt = *(void ***)svc; DWORD old;
    if (IsBadReadPtr(vt, 0x20) || !VirtualProtect(&vt[0x1C / 4], 4, PAGE_READWRITE, &old)) return;
    g_chatSvc = svc; g_chanOrig = (ChanSendFn)vt[0x1C / 4]; vt[0x1C / 4] = (void *)chan_send_hook;
    VirtualProtect(&vt[0x1C / 4], 4, old, &old);
    logmsg("chat: channel send hooked, general channel %d%s", g_general, g_general ? "" : " (unknown yet — write anything in General once)");
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

static unsigned long long g_localIdForKick;                  // свой ID (из decorate) — для проверки перед киком
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
    // страховка: не кикать свою станцию, даже если сопоставление игрок→ID где-то ошиблось
    DWORD mlo = 0, mhi = 0, tlo = 0, thi = 0;
    if (!g_localIdForKick || id == g_localIdForKick) { logmsg("guard: kick of %llu cancelled — own id unknown or equal (%llu)", id, g_localIdForKick); return 0; }
    int mine = station_of(m, g_localIdForKick, &mlo, &mhi), target = station_of(m, id, &tlo, &thi);
    if (!target || (mine && mlo == tlo && mhi == thi)) {
        logmsg("guard: kick of %llu cancelled — target station %08lX:%08lX, my station %08lX:%08lX (found %d/%d)", id, thi, tlo, mhi, mlo, target, mine);
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

// ---------- правила хоста ----------
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
// причина отказа или NULL, если игрок проходит
static const wchar_t *violation(const Entry *e, wchar_t *why, int cap) {
    if (!e->known) {
        if (G.kickUnknown && e->absent) { _snwprintf(why, cap, L"no Relic stats found"); return why; }   // only when the server answered «not found», never on network errors
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
    else return NULL;                       // часы скрыты (закрытый профиль) — по часам не кикаем
    why[cap - 1] = 0; return why;
}
static unsigned long long g_localId;                        // ID самого хоста — его никогда не кикаем
static int g_maxPlayers;                                    // мест для игроков (Receive_SetMaxPlayers); позиции дальше — обсерверы
static void guard_check(const wchar_t *key, const wchar_t *name, const Entry *e, unsigned long long id) {
    wchar_t why[160];
    if (!violation(e, why, 160)) return;
    Acted *a = acted(key); DWORD now = GetTickCount();
    if (a->lastAction && now - a->lastAction < 2 * 60 * 1000) return;   // не повторяем чаще раза в 2 минуты
    a->lastAction = now;
    if (g_njobs + 2 > 16) return;
    Job *j = &g_jobs[g_njobs++]; memset(j, 0, sizeof *j); j->due = now;
    if (G.dryRun) _snwprintf(j->text, 255, L"[Bot test] %ls would be kicked: %ls", name, why);
    else _snwprintf(j->text, 255, L"[Bot] %ls kicked by bot: %ls", name, why);
    if (id && id == g_localId) { logmsg("guard: id %llu is the host's own id — slot mapping is wrong, not kicking", id); id = 0; }
    if (!G.dryRun && id) {
        Job *k = &g_jobs[g_njobs++]; memset(k, 0, sizeof *k); k->id = id; k->kick = 1; k->due = now + G.delaySec * 1000;
        if (g_njobs < 16) {                                   // кикнутый вкладку матча уже не увидит — то же в общий канал
            Job *g = &g_jobs[g_njobs++]; memset(g, 0, sizeof *g); g->general = 1; g->due = now + G.delaySec * 1000;
            _snwprintf(g->text, 255, L"[Bot] %ls was kicked from the lobby by the host filter: %ls", name, why);
        }
    }
    char n8[200]; WideCharToMultiByte(CP_UTF8, 0, j->text, -1, n8, sizeof n8, NULL, NULL); logmsg("guard: %s", n8);
    if (!G.dryRun && !id) logmsg("guard: no SteamID for this player in the lobby data — cannot kick");
}
// последний список игроков лобби (для повторной проверки после смены правил командой в чате)
typedef struct { wchar_t key[64], name[64]; unsigned long long id; int remote, st, pos, obs; } Seen;
static Seen g_seen[16]; static int g_nseen, g_isHost, g_recheck;

// выполняется в главном потоке (из перехвата), вне обработки списка игроков
static int g_inDecorate;
static void guard_tick(void) {
    if ((g_recheck || g_fresh) && !g_inDecorate) {          // правила поменяли или пришли данные о зашедшем — проверяем лобби
        int rulesChanged = g_recheck;
        g_recheck = 0; g_fresh = 0;
        EnterCriticalSection(&g_lock);
        if (rulesChanged) g_nacted = 0;                        // при смене правил предупреждаем заново, при новых данных — нет
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

// ---------- команды хоста в чате лобби: !filter ... ----------
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
// число со знаком спереди или сзади: 1500 / 1500+ / +1500 → не меньше (1); 1500- / -1500 → не больше (2); 1500-2400 → диапазон (3)
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
static void set_lim(int k, int lo, int hi, int *mn, int *mx) {   // одно правило заменяет прежнее по этому параметру
    if (k == 1) { *mn = lo; *mx = 0; } else if (k == 2) { *mn = 0; *mx = hi; } else if (k == 3) { *mn = lo; *mx = hi; }
}
// "!filter 1500-2400 wr55 g100 h750 live" → правила; возвращает 1, если это команда
static int handle_command(const wchar_t *t, int len) {
    wchar_t s[256]; if (len > 255) len = 255; wcsncpy(s, t, len); s[len] = 0;
    for (wchar_t *c = s; *c; c++) *c = towlower(*c);
    wchar_t *p = s; while (*p == L' ') p++;
    if (wcsncmp(p, L"!filter", 7) && wcsncmp(p, L"!f ", 3) && wcscmp(p, L"!f")) return 0;
    p += (p[2] == L'i') ? 7 : 2;
    int changed = 0, rules = 0, mode = 0;                       // rules: задали правило; mode: явно test/live
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
    // правила без слова test = настоящий кик (раньше старый DryRun=1 из ini молча оставлял тестовый режим)
    for (const wchar_t *c = s; *c; c++) if (iswdigit(*c)) { rules = 1; break; }
    if (rules && !mode && G.enabled) G.dryRun = 0;
    if (changed) { save_guard(); g_recheck = 1; g_uiRefill = 1; }   // меню у хоста покажет новые значения
    wchar_t st[256]; guard_status(st, 256); chat_send(st);
    char u[512]; WideCharToMultiByte(CP_UTF8, 0, st, -1, u, sizeof u, NULL, NULL); logmsg("command: %s", u);
    return 1;
}
static void __stdcall chat_hook(int channel, WStr *text, int flag) {
    if (text && text->start && text->finish > text->start && text->finish - text->start < 1000 && g_isHost && text->start[0] != L'[') {
        if (handle_command(text->start, (int)(text->finish - text->start))) return;   // команду в чат не пропускаем — вместо неё ответ бота
    }
    g_chatOrig(channel, text, flag);
}

// ---------- фильтр хоста прямо в панели «Game Setup»: два выпадающих списка под «Starting resources» ----------
// Scaleform в игре 2.x: через публичный Invoke в ролик уходят только числа и строки, а AddDropDownBox нужен
// живой клип. Поэтому вызываем AS-функции так же, как это делает сам Invoke внутри (кладём значения на стек
// AS и зовём функцию по пути), только аргумент-клип достаём по пути настоящим объектом.
// Клип списка — копия resources_dropdown_mc (Receive_DuplicateMovieClip, уже есть в ролике), поэтому файлы
// Elite не меняются. Выбор приходит обычным fscommand "<панель>.OnSelectDropDownItem" "индекс;пункт" —
// перехватываем обработчик fscommand движка и свои индексы игре не передаём.
#define FN_STR_NEW   0x00DB64D0     // ASString из char*: __thiscall(менеджер строк, const char*)
#define FN_STR_FREE  0x00DB62D0     // __thiscall(узел строки), когда счётчик (+0xC) дошёл до 0
#define FN_RESOLVE   0x00DF9520     // путь -> значение: __thiscall(env, узел**, ASValue*, 0,0,0,0)
#define FN_AS_CALL   0x00DD27E0     // вызов функции по пути: __thiscall(корневой клип, путь, ASValue* рез., n)
#define FN_STK_GROW  0x00DB7950     // __thiscall(стек env+8)
#define FN_STK_POP   0x00DB7BE0     // __thiscall(стек, n) — снимает n значений и освобождает их
#define FN_VAL_FREE  0x00DBA000     // __thiscall(ASValue*) для типов >= 5
#define FN_FSCMD     0x00681AB0     // SFMovie::FSCommandHandler::Callback(movie, команда, аргументы)
static const unsigned char FSCMD_PRO[7] = {0x51, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14};
typedef struct { unsigned char b[16]; } ASValue;            // тип в первом байте, значение с +4
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

// method — полный путь функции; аргументы AV_PATH передаются как объекты по пути
static int as_call(void *movie, const char *method, const Arg *args, int n, double *ret) {
    void *sprite = *(void **)((char *)movie + 0x24); if (!sprite) return 0;
    void *env = ((EnvFn)(*(void ***)sprite)[0x5C / 4])(sprite, NULL);
    char *stk = (char *)env + 8;                             // {вершина, низ, предел}
    for (int i = n - 1; i >= 0; i--) {                       // как Invoke: последний аргумент первым
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
// функция панели с простыми аргументами: as_fn(movie, "Receive_SetDropDownVisible", "nb", 5.0, 1)
// sig: n — число (double), b — bool (int), s — строка, p — клип по пути, u — undefined
static char g_panel[256];                                    // абсолютный путь панели, напр. _level0.xxx
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
// заполнить списки по текущим правилам; правила из ini/команды, которых нет среди пунктов, — пункт «Custom»
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
        snprintf(nm, sizeof nm, "lsf_dd%d", b); as_fn(movie, "Receive_SetVisible", "sb", nm, g_isHost);   // сам клип: только хосту                                // у нижнего края экрана список раскрываем вверх
        snprintf(p, sizeof p, "%s.lsf_dd%d.dropdown_list_mc._height", g_panel, b);
        snprintf(nm, sizeof nm, "lsf_dd%d.dropdown_list_mc", b);
        if (g_uiOpenUp && as_num(movie, p, &h) && h > 0) as_fn(movie, "Receive_SetMovieClipY", "sn", nm, -h - 2);
    }
}
// вызывается после того, как игра заполнила «Starting resources» (индекс 4) в панели self
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
    if (!as_num(movie, p, &y0)) return;                       // не та панель
    snprintf(p, sizeof p, "%s.generic_dropdown0_mc._y", g_panel);
    if (!as_num(movie, p, &d)) return;
    snprintf(p, sizeof p, "%s.lsf_dd0._y", g_panel);
    if (!as_num(movie, p, &d)) {                              // в этой панели ещё нет наших списков
        snprintf(p, sizeof p, "%s.g_dropDownBoxes.length", g_panel);
        if (!as_num(movie, p, &nb)) { logmsg("ui: g_dropDownBoxes not found in %s", g_panel); g_base = -1; return; }
        g_base = (int)nb;
        for (int k = 0; k < NBOX; k++) {                      // верхний создаём первым: список нижнего раскрывается поверх него
            char nm[32], path[300]; snprintf(nm, sizeof nm, "lsf_dd%d", k); snprintf(path, sizeof path, "%s.%s", g_panel, nm);
            as_fn(movie, "Receive_DuplicateMovieClip", "ss", "resources_dropdown_mc", nm);
            as_fn(movie, "Receive_SetVisible", "sb", nm, 1);
            as_fn(movie, "Receive_SetMovieClipY", "sn", nm, y0 + g_uiStep * (k + 1));
            g_ddY[k] = y0 + g_uiStep * (k + 1);
            as_fn(movie, "AddDropDownBox", "punb", path, 2.0, 1);   // FLAG_HIDE_UP_WHEN_OVER, широкий
        }
        snprintf(p, sizeof p, "%s.g_dropDownBoxes.length", g_panel);
        double after = 0; as_num(movie, p, &after);
        logmsg("ui: filter lists added to %s as dropdowns %d..%d (boxes now %d), y=%.0f", g_panel, g_base, g_base + NBOX - 1, (int)after, y0);
        if ((int)after != g_base + NBOX) { logmsg("ui: AddDropDownBox did not register, giving up"); g_base = -1; return; }
    }
    if (g_base >= 0) {
        g_uiMovie = movie; g_resOpen = -1;
        ui_fill(movie);
        // наши списки — на самый верх (нижний выше верхнего: его список раскрывается вверх поверх верхнего);
        // раскрытый список «Starting resources» они не закрывают — пока он открыт, наши спрятаны (ui_poll)
        int ok0 = as_fn(movie, "lsf_dd0.swapDepths", "n", 16001.0);
        int ok1 = as_fn(movie, "lsf_dd1.swapDepths", "n", 16002.0);
        static int logged; if (logged++ < 3) logmsg("ui: filter lists raised to the top (swapDepths %d %d)", ok0, ok1);
    }
}
// игрок стал/перестал быть хостом — показать/скрыть; movie — общий ролик фронтенда (у панелей он один)
// каждый кадр: пока раскрыт список «Starting resources», наши списки спрятаны (иначе они лежат поверх него)
// movie — живой указатель из текущего вызова игры (fscommand), сохранённый указатель не используем: после выхода
// из лобби он может указывать на удалённый объект (из-за этого 0.6.5–0.6.6 вылетала при повторном хосте)
static void ui_poll(void *movie) {
    if (g_base < 0 || !movie || !g_panel[0]) return;
    char p[400]; GFxValue v; snprintf(p, sizeof p, "%s.resources_dropdown_mc.dropdown_list_mc._visible", g_panel);
    if (!as_get(movie, p, &v)) return;                         // панели нет (не наш ролик или ушли из лобби)
    g_uiMovie = movie; g_lobbyFsTick = GetTickCount();
    if (g_uiRefill) { g_uiRefill = 0; ui_fill(movie); g_resOpen = -1; }   // правила поменяли командой в чате
    int open = v.type == GFX_BOOL && v.v.b;
    if (g_logFs) { static int n; if (n++ < 300) logmsg("poll: resources list _visible type=%d val=%d -> open=%d (was %d)", v.type, v.type == GFX_BOOL ? v.v.b : -1, open, g_resOpen); }
    if (open == g_resOpen) return;
    g_resOpen = open;
    // спрятать через _visible не выходит (плашка остаётся на экране) — уносим клипы за край экрана и возвращаем
    for (int b = 0; b < NBOX; b++) {
        char nm[16]; snprintf(nm, sizeof nm, "lsf_dd%d", b);
        if (!g_ddY[b]) { char p2[400]; snprintf(p2, sizeof p2, "%s.%s._y", g_panel, nm); as_num(g_uiMovie, p2, &g_ddY[b]); }
        as_fn(g_uiMovie, "Receive_SetMovieClipY", "sn", nm, open ? -5000.0 : g_ddY[b]);
    }
}
static void ui_host_changed(void *self) {
    if (g_base < 0 || !g_panel[0] || IsBadReadPtr(self, 0x2C)) return;
    char *holder = *(char **)((char *)self + 0x28); if (!holder || IsBadReadPtr(holder, 8)) return;
    void *movie = *(void **)(holder + 4); if (!movie) return;
    char p[400]; double d; snprintf(p, sizeof p, "%s.lsf_dd0._y", g_panel);
    if (!as_num(movie, p, &d)) { g_base = -1; return; }       // панели уже нет
    for (int b = 0; b < NBOX; b++) {
        char nm[16]; snprintf(nm, sizeof nm, "lsf_dd%d", b);
        as_fn(movie, "Receive_SetDropDownVisible", "nb", (double)(g_base + b), g_isHost);
        as_fn(movie, "Receive_SetVisible", "sb", nm, g_isHost);
    }
}
static void ui_select(int box, int item) {
    if (box == 0 && item < COUNT(RATING)) { G.minRating = RATING[item].min; G.maxRating = RATING[item].max; }
    else if (box == 1 && item < COUNT(EXP)) {
        G.minGames = EXP[item].games; G.minHours = EXP[item].hours; G.minWinRate = EXP[item].wr; G.maxWinRate = EXP[item].maxwr; G.maxHours = EXP[item].maxh; G.kickUnknown = EXP[item].unknown;
    } else return;                                            // «Custom» — оставить как есть
    G.enabled = G.minRating || G.maxRating || G.minGames || G.minHours || G.maxHours || G.minWinRate || G.maxWinRate || G.kickUnknown;
    G.dryRun = 0;                                             // выбрал в меню — значит кикать (тестовый режим: ini или !filter test)
    save_guard(); g_recheck = 1;
    wchar_t st[256]; guard_status(st, 256); chat_send(st);
    char u[512]; WideCharToMultiByte(CP_UTF8, 0, st, -1, u, sizeof u, NULL, NULL); logmsg("ui: box %d item %d -> %s", box, item, u);
}
static FsCmdFn g_fsOrig;
static void __fastcall fs_hook(void *self, void *edx, void *movie, const char *cmd, const char *args) {
    // клик в интерфейсе: список «Starting resources» раскрывается уже ПОСЛЕ звука клика — следующие 30 кадров проверяем
    if (cmd && args && movie && g_base >= 0 && !strcmp(cmd, "OnPlaySound") && strstr(args, "click")) { g_uiMovie = movie; g_pollFrames = 30; }
    if (g_logFs && cmd) { static int n; if (n++ < 400) logmsg("fs: %s (%s)", cmd, args ? args : ""); }   // отладка: какие события шлёт интерфейс
    if (g_base >= 0 && movie && cmd && strstr(cmd, "OnSelectDropDownItem") == NULL) ui_poll(movie);
    if (g_base >= 0 && cmd && args) {
        const char *dot = strrchr(cmd, '.');
        int sel = dot && !strcmp(dot, ".OnSelectDropDownItem"), roll = dot && !strcmp(dot, ".OnRollOverDropDownItem");
        int idx, item;
        if ((sel || roll) && sscanf(args, "%d;%d", &idx, &item) == 2 && idx >= g_base && idx < g_base + NBOX) {
            size_t pl = strlen(g_panel);
            if ((size_t)(dot - cmd) == pl && !strncmp(cmd, g_panel, pl)) {
                if (sel) ui_select(idx - g_base, item);
                return;                                       // наш список — игре не передаём
            }
            static int warned; if (!warned++) logmsg("ui: %s from %s, our panel is %s — passing to the game", dot + 1, cmd, g_panel);
        }
    }
    g_fsOrig(self, edx, movie, cmd, args);
    if (g_uiRefill && g_base >= 0 && movie) ui_poll(movie);   // команду в чате применили внутри этого вызова — меню сразу
}

// ---------- перехват ----------
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
    // свой ID — по слоту, который игра пометила как «свой» (тип 0); вызов local_user_id() из 0.6.8 ронял игру у клиента
    g_localId = 0;
    for (unsigned i = 0; i < (count - HEADER_ARGS) / SLOT_ARGS && i < 16; i++) {
        GFxValue *a0 = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a0[A_SLOTTYPE].type == GFX_NUMBER && (int)a0[A_SLOTTYPE].v.num == SLOT_LOCAL && a0[A_SLOTINDEX].type == GFX_NUMBER) {
            int p0 = (int)a0[A_SLOTINDEX].v.num; if (p0 >= 0 && p0 < nids && ids[p0]) g_localId = ids[p0];
        }
    }
    {   // одинаковые ID у разных позиций = сопоставление сломано → кик запрещён
        int dup = 0;
        for (int x = 0; x < nids; x++) for (int y = x + 1; y < nids; y++) if (ids[x] && ids[x] == ids[y]) dup = 1;
        g_localIdForKick = dup ? 0 : g_localId;
        if (dup) { static int w; if (w++ < 3) logmsg("guard: duplicate ids in lobby data — kicks disabled for safety"); }
    }
    unsigned slots = (count - HEADER_ARGS) / SLOT_ARGS; int want = 0;
    EnterCriticalSection(&g_lock);
    for (unsigned i = 0; i < slots && i < 16; i++) {
        GFxValue *a = args + HEADER_ARGS + i * SLOT_ARGS;
        if (a[A_HASUSER].type != GFX_BOOL || !a[A_HASUSER].v.b) continue;
        if (a[A_SLOTTYPE].type != GFX_NUMBER) continue;
        int st = (int)a[A_SLOTTYPE].v.num;
        // тип слота: 0 свой, 1 чужой игрок; у обсерверов может быть другой — рейтинг показываем всем живым, фильтр — только игрокам
        if (a[A_NAME].type != GFX_WSTRING || !a[A_NAME].v.ws || !a[A_NAME].v.ws[0]) continue;
        if (a[A_TOOLTIP].type != GFX_WSTRING) continue;          // раскладка не та, что ждём — не трогаем
        const wchar_t *name = a[A_NAME].v.ws;
        unsigned long long id = 0;
        int si = a[A_SLOTINDEX].type == GFX_NUMBER ? (int)a[A_SLOTINDEX].v.num : -1;
        if (si >= 0 && si < nids && ids[si]) id = ids[si];       // ID игрока по позиции слота (для кика)
        int observer = g_maxPlayers > 0 && si >= g_maxPlayers;  // обсервер: позиция за местами игроков
        wchar_t key[64];
        if ((id >> 32) == 0x01100001) _snwprintf(key, 63, L"/steam/%llu", id); else wcsncpy(key, name, 63);   // статистику ищем по SteamID или по нику
        key[63] = 0;
        if (g_nseen < 16) {
            Seen *sn = &g_seen[g_nseen++]; wcsncpy(sn->key, key, 63); sn->key[63] = 0; wcsncpy(sn->name, name, 63); sn->name[63] = 0;
            sn->id = id; sn->remote = st == SLOT_REMOTE && !observer && id != g_localId;   // повторная проверка — только игроки
            sn->st = st; sn->pos = si; sn->obs = observer;
        }
        Entry *e = find(key);
        DWORD now = GetTickCount();
        if (!e || !e->fetched || now - e->fetched > 10 * 60 * 1000) {          // нет данных или старше 10 минут
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
    {   // состав лобби поменялся — записать, у кого есть SteamID (без него кик невозможен)
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
    // «Starting resources» (5-й список панели настроек) заполнен — значит, панель готова, добавляем фильтр
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

// раз в кадр (главный поток): задачи бота выполняются вовремя, а не только при обновлении интерфейса
#define IAT_SHARK_UPDATE 0x00F88068
typedef void (__stdcall *UpdFn)(float);
static UpdFn g_updOrig;
static void __stdcall frame_hook(float dt) {
    g_updOrig(dt);
    if (g_logFs) { static int n; if (!(n++ % 500)) logmsg("frame hook: call %d, poll frames left %d", n, g_pollFrames); }
    if (!g_inDecorate) { hook_chat_service(); guard_tick(); }
    // команда !filter из чата приходит от ролика чата — меню обновляем роликом лобби, если он живой (действие в лобби < 20 с назад)
    if (g_uiRefill && g_uiMovie && g_base >= 0 && GetTickCount() - g_lobbyFsTick < 20000 && !g_pollFrames) g_pollFrames = 1;
    if (g_pollFrames > 0 && g_uiMovie) { g_pollFrames--; ui_poll(g_uiMovie); }
}
static void hook_frame(void) {
    HMODULE m = GetModuleHandleA("Shark.dll"); MODULEINFO mi; DWORD old;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || IsBadReadPtr((void *)IAT_SHARK_UPDATE, 4)) return;
    DWORD cur = *(DWORD *)IAT_SHARK_UPDATE;
    if (cur < (DWORD)mi.lpBaseOfDll || cur >= (DWORD)mi.lpBaseOfDll + mi.SizeOfImage) { logmsg("frame hook: IAT slot is not Shark.dll (already hooked by another DLL?) — chaining"); }
    if (!VirtualProtect((void *)IAT_SHARK_UPDATE, 4, PAGE_READWRITE, &old)) return;
    g_updOrig = (UpdFn)cur; *(DWORD *)IAT_SHARK_UPDATE = (DWORD)frame_hook;
    VirtualProtect((void *)IAT_SHARK_UPDATE, 4, old, &old);
    logmsg("frame hook installed");
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
    // облегчённая сборка: только подсказка со статистикой — без фильтра, кика, команд в чате и новых меню
    logmsg("hooked Invoke at %08X (lite: stats tooltip only, no filter/kick/chat/menus)", HOOK_ADDR);
    return;
#endif
    hook_frame();
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

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_dir, MAX_PATH); wchar_t *sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0; else g_dir[0] = 0;
    wchar_t *ini = g_ini; _snwprintf(ini, MAX_PATH, L"%lsLobbyStats.ini", g_dir);
    g_showInName = GetPrivateProfileIntW(L"LobbyStats", L"ShowRatingInName", 1, ini);
    g_stats       = GetPrivateProfileIntW(L"LobbyStats", L"Stats", 1, ini);
    GetPrivateProfileStringW(L"LobbyStats", L"Server", STATS_HOST, g_server, 128, ini);
    if (!g_server[0]) { g_stats = 0; }                        // сервис статистики не задан — цифр не будет
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
    G.enabled = 0; g_ui = 0;                                  // фильтра и меню в этой сборке нет вовсе
    g_showInName = GetPrivateProfileIntW(L"LobbyStats", L"ShowRatingInName", 1, ini);   // рейтинг рядом с ником — по умолчанию
#endif
    InitializeCriticalSection(&g_lock); g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
#ifdef LITE
    logmsg("LobbyStats Lite v0.6.17");
#else
    logmsg("LobbyStats v0.6.17");
#endif
    load_cache(); install();
    if (G.enabled) logmsg("guard: on, dry-run=%d, rating %d..%d, win rate %d..%d%%, min games=%d hours=%d, kick unknown=%d, delay=%ds",
                          G.dryRun, G.minRating, G.maxRating, G.minWinRate, G.maxWinRate, G.minGames, G.minHours, G.kickUnknown, G.delaySec);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}

// для проверки вне игры: прогнать подмену и правила на поддельном наборе аргументов
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
// для проверки вне игры: выбор пункта в списке фильтра и какой пункт показали бы списки при текущих правилах
__declspec(dllexport) void LobbyStats_UiSelect(int box, int item, wchar_t *status, int cap, int *selRating, int *selExp) {
    ui_select(box, item); guard_status(status, cap); *selRating = rating_sel(); *selExp = exp_sel();
}
