// FrameProbe.dll — сэмплирующий профайлер для DOW2.exe (ищем, почему тормозит режим обсервера).
//
// Подключается как остальные DLL Elite (InjectedFiles в Elite.launchconfig). Через 15 с после старта
// каждые ~2 мс на миг останавливает потоки игры, запоминает, где они стоят (EIP) и адреса возврата
// в стеке, и раз в 10 с пишет в FrameProbe.log:
//   - загрузку каждого потока (сколько процентов одного ядра он съел) и где он начался;
//   - самые горячие адреса главного потока (окно игры): «сам» (EIP) и «включая вызовы» (адреса возврата);
//   - разбивку главного потока по модулям (DOW2.exe, d3d9, ntdll = ожидание и т.п.).
// Адреса — модуль+смещение; функции по ним находим в Ghidra.
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define WINDOW_MS   10000
#define PERIOD_MS   20   // 50 замеров в секунду: остановка игрового потока ~20 мкс, т.е. <0.1% его времени
#define MAX_THREADS 64
#define STACK_BYTES 4096
#define HASH_BITS   16
#define HASH_SIZE   (1 << HASH_BITS)

typedef struct { DWORD addr, count; } Slot;
static Slot g_excl[HASH_SIZE], g_incl[HASH_SIZE], g_excl2[HASH_SIZE], g_incl2[HASH_SIZE];
static DWORD g_mainSamples, g_samples2, g_tid2, g_nextTid2;   // 2 — самый загруженный поток после главного
typedef struct { DWORD tid; HANDLE h; DWORD start; ULONGLONG cpu0; DWORD samples; int alive; } Thr;
static Thr g_thr[MAX_THREADS]; static int g_nthr;
static wchar_t g_log[MAX_PATH];
static DWORD g_self;

typedef struct { DWORD base, end; char name[40]; DWORD textBeg, textEnd; } Mod;
static Mod g_mods[256]; static int g_nmods;

static void plog(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fclose(f);
}
static void add(Slot *t, DWORD addr) {
    DWORD h = (addr * 2654435761u) >> (32 - HASH_BITS);
    for (int i = 0; i < HASH_SIZE; i++, h = (h + 1) & (HASH_SIZE - 1)) {
        if (t[h].addr == addr) { t[h].count++; return; }
        if (!t[h].addr) { t[h].addr = addr; t[h].count = 1; return; }
    }
}
static void load_mods(void) {
    HMODULE ms[256]; DWORD need; g_nmods = 0;
    if (!EnumProcessModules(GetCurrentProcess(), ms, sizeof ms, &need)) return;
    int n = need / sizeof(HMODULE); if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi; if (!GetModuleInformation(GetCurrentProcess(), ms[i], &mi, sizeof mi)) continue;
        Mod *m = &g_mods[g_nmods++]; m->base = (DWORD)mi.lpBaseOfDll; m->end = m->base + mi.SizeOfImage;
        char full[MAX_PATH]; GetModuleFileNameA(ms[i], full, MAX_PATH);
        char *s = strrchr(full, '\\'); strncpy(m->name, s ? s + 1 : full, 39); m->name[39] = 0;
        IMAGE_DOS_HEADER *dh = (IMAGE_DOS_HEADER *)m->base; IMAGE_NT_HEADERS *nh = (IMAGE_NT_HEADERS *)(m->base + dh->e_lfanew);
        IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nh); m->textBeg = m->textEnd = 0;
        for (int k = 0; k < nh->FileHeader.NumberOfSections; k++)
            if (sh[k].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                DWORD b = m->base + sh[k].VirtualAddress, e = b + sh[k].Misc.VirtualSize;
                if (!m->textBeg || b < m->textBeg) m->textBeg = b; if (e > m->textEnd) m->textEnd = e;
            }
    }
}
static Mod *mod_of(DWORD a) { for (int i = 0; i < g_nmods; i++) if (a >= g_mods[i].base && a < g_mods[i].end) return &g_mods[i]; return NULL; }
static int in_text(DWORD a) { for (int i = 0; i < g_nmods; i++) if (a >= g_mods[i].textBeg && a < g_mods[i].textEnd) return 1; return 0; }
static void fmt_addr(DWORD a, char *o, int cap) {
    Mod *m = mod_of(a);
    if (!m) snprintf(o, cap, "%08lX", a);
    else if (!_stricmp(m->name, "DOW2.exe")) snprintf(o, cap, "DOW2.exe!%08lX", a);       // exe без ASLR: адрес как в Ghidra
    else snprintf(o, cap, "%s+%lX", m->name, a - m->base);
}
// похоже ли значение со стека на адрес возврата (перед ним инструкция call)
static int is_ret(DWORD r) {
    if (!in_text(r)) return 0;
    unsigned char b[7];
    if (!ReadProcessMemory(GetCurrentProcess(), (void *)(r - 7), b, 7, NULL)) return 0;
    if (b[2] == 0xE8) return 1;                                   // call rel32
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return 1;          // call [disp32] / [reg+disp32]
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10) return 1;          // call [reg+disp8]
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return 1;          // call reg / [reg]
    return 0;
}
static DWORD main_tid(void) {
    HWND w = NULL; DWORD best = 0;
    while ((w = FindWindowExW(NULL, w, NULL, NULL))) {
        DWORD pid, tid = GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId() && IsWindowVisible(w)) { best = tid; break; }
    }
    return best;
}
typedef LONG (WINAPI *NtQIT)(HANDLE, int, PVOID, ULONG, PULONG);
static void refresh_threads(void) {
    for (int i = 0; i < g_nthr; i++) g_thr[i].alive = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); if (s == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te = { sizeof te };
    static NtQIT q; if (!q) q = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == g_self) continue;
        int i; for (i = 0; i < g_nthr; i++) if (g_thr[i].tid == te.th32ThreadID) break;
        if (i == g_nthr) {
            if (g_nthr == MAX_THREADS) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            Thr *t = &g_thr[g_nthr++]; memset(t, 0, sizeof *t); t->tid = te.th32ThreadID; t->h = h;
            if (q) q(h, 9, &t->start, sizeof t->start, NULL);            // ThreadQuerySetWin32StartAddress
            FILETIME a, b, k, u; if (GetThreadTimes(h, &a, &b, &k, &u)) t->cpu0 = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
        }
        g_thr[i].alive = 1;
    }
    CloseHandle(s);
}
// ---------- точные счётчики: перехват таблицы импорта DOW2.exe 3.19 (код игры не меняется) ----------
#define IAT_SHARK_UPDATE 0x00F88068   // shark::update(float dt) — ровно раз в кадр
#define IAT_SIM_PRE      0x00F88154   // World::SimulatePre  (__thiscall) — начало тика симуляции
#define IAT_SIM_POST     0x00F88100   // World::SimulatePost (__thiscall) — конец тика
#define IAT_GAMETICKS    0x00F885C4   // SimDebug::GetGameTicks() — номер игрового тика
#define IAT_TRACEF       0x00F87074   // dbInternal::TimeStampedTracef(fmt, ...) — отладочный вывод игры (Debug.dll)
typedef void (__stdcall *UpdFn)(float);
typedef void (__fastcall *ThisFn)(void *, void *);
typedef long (__stdcall *TicksFn)(void);
static UpdFn o_upd; static ThisFn o_pre, o_post; static TicksFn f_ticks;
static CRITICAL_SECTION g_cs; static LARGE_INTEGER g_qf;
static struct { LONG frames, ticks, maxTicksFrame, ticksFrame, hist[6]; double ftSum, ftMax, simSum, simMax, dtSum; LONGLONG lastQ, preQ; } C;
static double ms_since(LONGLONG q0, LONGLONG q1) { return (q1 - q0) * 1000.0 / g_qf.QuadPart; }
static volatile LONG g_ticksNow;                            // номер игрового тика, читается в главном потоке
static volatile DWORD g_gameTid;                            // поток, который рисует кадры (игровой цикл), а не поток окна
static void __stdcall hk_update(float dt) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    if (!g_gameTid) g_gameTid = GetCurrentThreadId();
    EnterCriticalSection(&g_cs);
    if (C.lastQ) {
        double ft = ms_since(C.lastQ, q.QuadPart); C.ftSum += ft; if (ft > C.ftMax) C.ftMax = ft;
        C.hist[ft < 16.7 ? 0 : ft < 25 ? 1 : ft < 33.4 ? 2 : ft < 50 ? 3 : ft < 100 ? 4 : 5]++;
    }
    C.lastQ = q.QuadPart; C.frames++; C.dtSum += dt;
    if (C.ticksFrame > C.maxTicksFrame) C.maxTicksFrame = C.ticksFrame;
    C.ticksFrame = 0;
    LeaveCriticalSection(&g_cs);
    o_upd(dt);
}
static void __fastcall hk_pre(void *self, void *edx) {
    LARGE_INTEGER q; QueryPerformanceCounter(&q); C.preQ = q.QuadPart;
    o_pre(self, edx);
}
static void __fastcall hk_post(void *self, void *edx) {
    o_post(self, edx);
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    EnterCriticalSection(&g_cs);
    if (C.preQ) { double t = ms_since(C.preQ, q.QuadPart); C.simSum += t; if (t > C.simMax) C.simMax = t; }
    C.ticks++; C.ticksFrame++;
    LeaveCriticalSection(&g_cs);
}
static int iat_hook(DWORD slot, const char *dll, void *fn, void **orig) {
    HMODULE m = GetModuleHandleA(dll); MODULEINFO mi;
    if (!m || !GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi) || IsBadReadPtr((void *)slot, 4)) return 0;
    DWORD cur = *(DWORD *)slot;
    if (cur < (DWORD)mi.lpBaseOfDll || cur >= (DWORD)mi.lpBaseOfDll + mi.SizeOfImage) {
        // слот уже перехвачен другой DLL (например LobbyStats) — встаём цепочкой, если там исполняемый код
        MEMORY_BASIC_INFORMATION mb;
        if (!VirtualQuery((void *)cur, &mb, sizeof mb) || !(mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
        HMODULE exe = GetModuleHandleA(NULL); MODULEINFO me;               // но только в самой игре (DOW2.exe)
        if (!GetModuleInformation(GetCurrentProcess(), exe, &me, sizeof me) || slot < (DWORD)me.lpBaseOfDll || slot >= (DWORD)me.lpBaseOfDll + me.SizeOfImage) return 0;
        char nm[MAX_PATH]; GetModuleFileNameA(exe, nm, MAX_PATH); if (!strstr(nm, "DOW2") && !strstr(nm, "dow2")) return 0;
    }
    DWORD old; if (!VirtualProtect((void *)slot, 4, PAGE_READWRITE, &old)) return 0;
    *orig = (void *)cur; *(DWORD *)slot = (DWORD)fn;
    VirtualProtect((void *)slot, 4, old, &old);
    return 1;
}
// Копия отладочного вывода игры в FrameProbe_trace.log: PartyProgresser / PostGameStatsForm (кнопка Continue),
// сообщения сети и синхронизации (обсервер). Ограничение 30 МБ.
typedef void (__cdecl *TraceFn)(const char *, ...);
static TraceFn o_trace; static CRITICAL_SECTION g_tcs; static wchar_t g_tlog[MAX_PATH]; static LONG g_tbytes;
// форматируем только нужные строки (пати/Continue/сессия/вход): разбирать каждую отладочную строку игры рискованно
static int trace_wanted(const char *f) {
    static const char *keys[] = {"Party", "PostGame", "Postgame", "Continue", "Transition", "Session", "Join", "Advertis", "Lobby", "%s", NULL};
    if (!f) return 0;
    for (int i = 0; keys[i]; i++) if (strstr(f, keys[i])) return 1;
    return 0;
}
static void __cdecl hk_trace(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    if (!trace_wanted(fmt)) {                                   // не наше — просто отдать игре как есть
        char pass[1024]; int pn = _vsnprintf(pass, sizeof pass - 1, fmt, a); va_end(a);
        if (pn < 0 || pn > (int)sizeof pass - 1) pn = sizeof pass - 1; pass[pn] = 0;
        o_trace("%s", pass); return;
    }
    char buf[1024];
    int n = fmt ? _vsnprintf(buf, sizeof buf - 1, fmt, a) : 0; va_end(a);
    if (n < 0 || n > (int)sizeof buf - 1) n = sizeof buf - 1; buf[n] = 0;
    if (g_tbytes < 30 << 20) {
        EnterCriticalSection(&g_tcs);
        FILE *f = _wfopen(g_tlog, L"a");
        if (f) { SYSTEMTIME t; GetLocalTime(&t);
            g_tbytes += fprintf(f, "%02d:%02d:%02d.%03d [%lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), buf); fclose(f); }
        LeaveCriticalSection(&g_tcs);
    }
    o_trace("%s", buf);                                         // игре — та же строка, уже отформатированная
}
// ---------- кнопка Continue на экране итогов: что видит игра в момент нажатия ----------
#define FN_ONCLICKNEXT 0x00480A71                   // PostGameStatsForm::OnClickNext (__fastcall, ecx = форма)
static const unsigned char NEXT_PRO[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
#define G_SESSION_PTR  0x01335014
#define G_SETTINGS_PTR 0x01335A78
typedef char  (__fastcall *BoolVFn)(void *self, void *edx);
typedef void *(__fastcall *PtrVFn)(void *self, void *edx);
void *g_nextTramp;                                   // используется из asm-заглушки
static void tlog(const char *fmt, ...) {
    char buf[600]; va_list a; va_start(a, fmt); _vsnprintf(buf, sizeof buf - 1, fmt, a); va_end(a); buf[sizeof buf - 1] = 0;
    EnterCriticalSection(&g_tcs);
    FILE *f = _wfopen(g_tlog, L"a");
    if (f) { SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d.%03d [FrameProbe] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf); fclose(f); }
    LeaveCriticalSection(&g_tcs);
}
// только чтение памяти с проверкой: вызывать функции объекта матча нельзя — после реплея матча нет, и игра падала (v7)
static int rd_ok(const void *p, int n) { return p && !IsBadReadPtr(p, n); }
void __stdcall on_click_next(unsigned char *form) {
    tlog("OnClickNext: clicked, form=%p", form);
    int mode = -1, party = -1; void *mgr = NULL, *mvt = NULL;
    if (rd_ok((void *)G_SESSION_PTR, 4)) { mgr = *(void **)G_SESSION_PTR; if (rd_ok(mgr, 4)) mvt = *(void **)mgr; }
    unsigned char *st = rd_ok((void *)G_SETTINGS_PTR, 4) ? *(unsigned char **)G_SETTINGS_PTR : NULL;
    if (rd_ok(st, 0xEB4)) { mode = *(int *)(st + 0x118); party = st[0xEB0]; }
    int ok = rd_ok(form, 0x68);
    tlog("OnClickNext: canContinue(+64)=%d busy(+65)=%d (+66)=%d | session=%p vtbl=%p | mode(+118)=0x%X party(+EB0)=%d",
         ok ? form[0x64] : -1, ok ? form[0x65] : -1, ok ? form[0x66] : -1, mgr, mvt, mode, party);
}
__asm__(".globl _next_stub\n_next_stub:\n"
        "  pushal\n  pushl %ecx\n  call _on_click_next@4\n  popal\n"
        "  jmp *_g_nextTramp\n");
void next_stub(void);
static void install_next_hook(void) {
    unsigned char *t = (unsigned char *)FN_ONCLICKNEXT;
    if (IsBadReadPtr(t, 6) || memcmp(t, NEXT_PRO, 6)) { tlog("OnClickNext hook: prologue mismatch, skipped"); return; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, NEXT_PRO, 6); tr[6] = 0xE9; *(DWORD *)(tr + 7) = (FN_ONCLICKNEXT + 6) - (DWORD)(tr + 11);
    g_nextTramp = tr;
    DWORD old; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(DWORD *)(t + 1) = (DWORD)next_stub - (DWORD)(t + 5); t[5] = 0x90;
    VirtualProtect(t, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), t, 6);
    tlog("OnClickNext hook installed");
}

static int g_hooks; static LONG g_gt0;
static int g_optObserver = 1, g_optContinue = 1;           // FrameProbe.ini [Probe] Observer= / Continue=
static void install_counters(void) {
    InitializeCriticalSection(&g_cs); QueryPerformanceFrequency(&g_qf);
    if (g_optObserver) g_hooks |= iat_hook(IAT_SHARK_UPDATE, "Shark.dll", hk_update, (void **)&o_upd) ? 1 : 0;
    if (g_optObserver && iat_hook(IAT_SIM_PRE, "SimEngine.dll", hk_pre, (void **)&o_pre)) {
        if (iat_hook(IAT_SIM_POST, "SimEngine.dll", hk_post, (void **)&o_post)) g_hooks |= 2;
        else { DWORD old; VirtualProtect((void *)IAT_SIM_PRE, 4, PAGE_READWRITE, &old); *(void **)IAT_SIM_PRE = o_pre; VirtualProtect((void *)IAT_SIM_PRE, 4, old, &old); }
    }
    InitializeCriticalSection(&g_tcs);
    wcscpy(g_tlog, g_log); wchar_t *dot = wcsrchr(g_tlog, L'.'); if (dot) wcscpy(dot, L"_trace.log");
    // копию отладочного вывода игры больше не делаем: он и так в warnings.txt, а запись файла из сетевого потока его тормозила
    (void)hk_trace;
    if (g_optContinue) { g_hooks |= 8; install_next_hook(); }
    HMODULE se = GetModuleHandleA("SimEngine.dll");
    // SimDebug::GetGameTicks больше не вызываем: вызов вне симуляции ронял игру на первом же отчёте (v5)
    if (0 && g_optObserver && se && !IsBadReadPtr((void *)IAT_GAMETICKS, 4)) {
        DWORD cur = *(DWORD *)IAT_GAMETICKS; MODULEINFO mi;
        if (GetModuleInformation(GetCurrentProcess(), se, &mi, sizeof mi) && cur >= (DWORD)mi.lpBaseOfDll && cur < (DWORD)mi.lpBaseOfDll + mi.SizeOfImage) { f_ticks = (TicksFn)cur; g_hooks |= 4; }
    }
    plog("counters: frames %s, sim ticks %s, game ticks %s, game trace %s\n", g_hooks & 1 ? "on" : "off (not DOW2 3.19?)", g_hooks & 2 ? "on" : "off", g_hooks & 4 ? "on" : "off", g_hooks & 8 ? "Continue hook only (game trace is in warnings.txt)" : "off");
}
static void report_counters(DWORD ms) {
    if (!g_hooks) return;
    EnterCriticalSection(&g_cs);
    typeof(C) c = C; LONGLONG lastQ = C.lastQ; memset(&C, 0, sizeof C); C.lastQ = lastQ;
    LeaveCriticalSection(&g_cs);
    double sec = ms / 1000.0;
    if (g_hooks & 1) {
        LONG n = c.frames > 1 ? c.frames : 1, h = 0; for (int i = 0; i < 6; i++) h += c.hist[i]; if (!h) h = 1;
        plog("FRAMES: %.1f fps, avg %.1f ms, worst %.1f ms, game dt sum %.2f s | <16ms %ld%%  16-25 %ld%%  25-33 %ld%%  33-50 %ld%%  50-100 %ld%%  >100 %ld%%\n",
             c.frames / sec, c.ftSum / n, c.ftMax, c.dtSum, c.hist[0] * 100 / h, c.hist[1] * 100 / h, c.hist[2] * 100 / h, c.hist[3] * 100 / h, c.hist[4] * 100 / h, c.hist[5] * 100 / h);
    }
    if (g_hooks & 2)
        plog("SIM: %ld ticks (%.1f/s), avg %.2f ms, worst %.2f ms, max %ld ticks in one frame, sim = %.0f%% of real time\n",
             c.ticks, c.ticks / sec, c.ticks ? c.simSum / c.ticks : 0, c.simMax, c.maxTicksFrame, c.simSum / ms * 100);
    if (g_hooks & 4) { LONG gt = g_ticksNow; if (g_gt0) plog("GAME TICKS: +%ld in %.1f s (%.2f per real second)\n", gt - g_gt0, sec, (gt - g_gt0) / sec); g_gt0 = gt; }
}

static int cmp_slot(const void *a, const void *b) { return (int)((const Slot *)b)->count - (int)((const Slot *)a)->count; }
static void report(DWORD mainTid, DWORD ms) {
    SYSTEMTIME st; GetLocalTime(&st);
    plog("\n===== %02d:%02d:%02d  window %lu ms, main thread %lu, main samples %lu =====\n", st.wHour, st.wMinute, st.wSecond, ms, mainTid, g_mainSamples);
    report_counters(ms);
    plog("threads (CPU %% of one core in this window):\n");
    for (int i = 0; i < g_nthr; i++) {
        Thr *t = &g_thr[i]; if (!t->alive) continue;
        FILETIME a, b, k, u; ULONGLONG c = t->cpu0;
        if (GetThreadTimes(t->h, &a, &b, &k, &u)) c = ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
        double pct = (c - t->cpu0) / 100.0 / ms; t->cpu0 = c;   // (1 tick FILETIME = 100 ns)
        static double best; if (i == 0) best = 0;
        if (t->tid != mainTid && pct > best) { best = pct; g_nextTid2 = t->tid; }
        if (pct < 2 && t->tid != mainTid) continue;
        char sa[64]; fmt_addr(t->start, sa, sizeof sa);
        plog("  tid %6lu %5.1f%%  start %s%s\n", t->tid, pct, sa, t->tid == mainTid ? "   <- main (game window)" : "");
    }
    static Slot tmp[HASH_SIZE]; int n = 0;
    // по модулям
    typedef struct { const char *name; DWORD c; } MC; MC mc[64]; int nmc = 0;
    for (int i = 0; i < HASH_SIZE; i++) if (g_excl[i].addr) {
        Mod *m = mod_of(g_excl[i].addr); const char *nm = m ? m->name : "?"; int j;
        for (j = 0; j < nmc; j++) if (mc[j].name == nm) break;
        if (j == nmc && nmc < 64) { mc[nmc].name = nm; mc[nmc++].c = 0; }
        if (j < 64) mc[j].c += g_excl[i].count;
        tmp[n++] = g_excl[i];
    }
    plog("main thread by module:");
    for (int i = 0; i < nmc; i++) if (mc[i].c * 100 >= g_mainSamples + 1) plog("  %s %lu%%", mc[i].name, mc[i].c * 100 / (g_mainSamples ? g_mainSamples : 1));
    plog("\nmain thread hottest (self):\n");
    qsort(tmp, n, sizeof(Slot), cmp_slot);
    for (int i = 0; i < n && i < 40; i++) { char s[64]; fmt_addr(tmp[i].addr, s, sizeof s); plog("  %5.1f%%  %s\n", tmp[i].count * 100.0 / g_mainSamples, s); }
    n = 0; for (int i = 0; i < HASH_SIZE; i++) if (g_incl[i].addr) tmp[n++] = g_incl[i];
    qsort(tmp, n, sizeof(Slot), cmp_slot);
    plog("main thread hottest (including callees, return addresses):\n");
    for (int i = 0; i < n && i < 80; i++) { char s[64]; fmt_addr(tmp[i].addr, s, sizeof s); plog("  %5.1f%%  %s\n", tmp[i].count * 100.0 / g_mainSamples, s); }
    if (g_samples2) {
        plog("second busiest thread %lu, samples %lu — hottest (self):\n", g_tid2, g_samples2);
        n = 0; for (int i = 0; i < HASH_SIZE; i++) if (g_excl2[i].addr) tmp[n++] = g_excl2[i];
        qsort(tmp, n, sizeof(Slot), cmp_slot);
        for (int i = 0; i < n && i < 25; i++) { char s[64]; fmt_addr(tmp[i].addr, s, sizeof s); plog("  %5.1f%%  %s\n", tmp[i].count * 100.0 / g_samples2, s); }
        plog("second thread (including callees):\n");
        n = 0; for (int i = 0; i < HASH_SIZE; i++) if (g_incl2[i].addr) tmp[n++] = g_incl2[i];
        qsort(tmp, n, sizeof(Slot), cmp_slot);
        for (int i = 0; i < n && i < 40; i++) { char s[64]; fmt_addr(tmp[i].addr, s, sizeof s); plog("  %5.1f%%  %s\n", tmp[i].count * 100.0 / g_samples2, s); }
    }
    memset(g_excl, 0, sizeof g_excl); memset(g_incl, 0, sizeof g_incl); g_mainSamples = 0;
    memset(g_excl2, 0, sizeof g_excl2); memset(g_incl2, 0, sizeof g_incl2); g_samples2 = 0; g_tid2 = g_nextTid2;
}
static DWORD WINAPI sampler(LPVOID _) {
    Sleep(5000);
    timeBeginPeriod(1);
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_log); wchar_t *dt = wcsrchr(ini, L'.'); if (dt) wcscpy(dt, L".ini");
    g_optObserver = GetPrivateProfileIntW(L"Probe", L"Observer", 1, ini);
    g_optContinue = GetPrivateProfileIntW(L"Probe", L"Continue", 1, ini);
    load_mods(); refresh_threads(); install_counters();
    plog("FrameProbe v9\n");
    plog("options: observer logs %s, Continue logs %s\n", g_optObserver ? "on" : "off", g_optContinue ? "on" : "off");
    if (!g_optObserver) return 0;                              // без замеров обсервера сэмплер не нужен
    int sampler = GetPrivateProfileIntW(L"Probe", L"Sampler", 1, ini);   // 0 — только счётчики кадров, игровой поток не останавливается
    plog("sampler: %s\n", sampler ? "on, every 20 ms" : "off");
    plog("FrameProbe started: %d modules, %d threads\n", g_nmods, g_nthr);
    static unsigned char stack[STACK_BYTES];
    DWORD t0 = GetTickCount(), lastRefresh = t0, mainTid = main_tid();
    for (;;) {
        Sleep(PERIOD_MS);
        DWORD now = GetTickCount();
        if (now - lastRefresh > 1000) { refresh_threads(); lastRefresh = now; DWORD m = g_gameTid ? g_gameTid : main_tid(); if (m) mainTid = m; }
        if (now - t0 >= WINDOW_MS) { load_mods(); report(mainTid, now - t0); t0 = now; }
        for (int i = 0; sampler && i < g_nthr; i++) {
            Thr *t = &g_thr[i]; if (!t->alive || t->tid != mainTid) continue;   // только главный поток: останавливать сетевой нельзя (рвёт соединение)
            int second = t->tid != mainTid;
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(t->h) == (DWORD)-1) continue;
            SIZE_T got = 0; int ok = GetThreadContext(t->h, &c);
            if (ok) ReadProcessMemory(GetCurrentProcess(), (void *)c.Esp, stack, STACK_BYTES, &got);
            ResumeThread(t->h);                       // дальше — без остановленного потока
            if (!ok) continue;
            Slot *ex = second ? g_excl2 : g_excl, *in = second ? g_incl2 : g_incl;
            if (second) g_samples2++; else g_mainSamples++;
            add(ex, c.Eip);
            DWORD seen[24]; int ns = 0;
            for (SIZE_T o = 0; o + 4 <= got && ns < 24; o += 4) {
                DWORD v = *(DWORD *)(stack + o); int dup = 0;
                if (!is_ret(v)) continue;
                for (int k = 0; k < ns; k++) if (seen[k] == v) dup = 1;
                if (!dup) { seen[ns++] = v; add(in, v); }
            }
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wcscat(g_log, L"FrameProbe.log");
    HANDLE th = CreateThread(NULL, 0, sampler, NULL, CREATE_SUSPENDED, &g_self);
    SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL); ResumeThread(th); CloseHandle(th);
    return TRUE;
}
