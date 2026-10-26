// CGNumbers.dll - control-group numbers above squads in Dawn of War II: Retribution (Elite). SKELETON.
//
// Stage 1 (this file): prove the render path. The game renders through DXVK's d3d9.dll; we hook
// IDirect3DDevice9::EndScene by patching the device vtable, create an ID3DXFont (d3dx9_*.dll is present
// in the prefix), and draw a test string every frame. We also hook SetTransform to log whether the game
// feeds VIEW/PROJECTION matrices (that would give us world->screen cheaply for stage 2).
//
// Stage 2 (later): read each on-screen squad + its control group, project to screen, draw the number.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#define COBJMACROS
#include <d3d9.h>

static wchar_t g_log[MAX_PATH];
static void logmsg(const char *fmt, ...) {
    FILE *f = _wfopen(g_log, L"a"); if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}

// ---- minimal ID3DXFont (avoid needing the d3dx9 headers; we only use a few methods) ----
typedef struct ID3DXFontVtbl ID3DXFontVtbl;
typedef struct ID3DXFont { ID3DXFontVtbl *lpVtbl; } ID3DXFont;
struct ID3DXFontVtbl {
    HRESULT (WINAPI *QueryInterface)(ID3DXFont*, REFIID, void**);
    ULONG   (WINAPI *AddRef)(ID3DXFont*);
    ULONG   (WINAPI *Release)(ID3DXFont*);
    HRESULT (WINAPI *GetDevice)(ID3DXFont*, IDirect3DDevice9**);
    HRESULT (WINAPI *GetDescA)(ID3DXFont*, void*);
    HRESULT (WINAPI *GetDescW)(ID3DXFont*, void*);
    BOOL    (WINAPI *GetTextMetricsA)(ID3DXFont*, TEXTMETRICA*);
    BOOL    (WINAPI *GetTextMetricsW)(ID3DXFont*, TEXTMETRICW*);
    HDC     (WINAPI *GetDC)(ID3DXFont*);
    HRESULT (WINAPI *GetGlyphData)(ID3DXFont*, UINT, IDirect3DTexture9**, RECT*, POINT*);
    HRESULT (WINAPI *PreloadCharacters)(ID3DXFont*, UINT, UINT);
    HRESULT (WINAPI *PreloadGlyphs)(ID3DXFont*, UINT, UINT);
    HRESULT (WINAPI *PreloadTextA)(ID3DXFont*, const char*, INT);
    HRESULT (WINAPI *PreloadTextW)(ID3DXFont*, const wchar_t*, INT);
    INT     (WINAPI *DrawTextA)(ID3DXFont*, void* sprite, const char* str, INT count, RECT* rect, DWORD format, D3DCOLOR color);
    INT     (WINAPI *DrawTextW)(ID3DXFont*, void* sprite, const wchar_t* str, INT count, RECT* rect, DWORD format, D3DCOLOR color);
    HRESULT (WINAPI *OnLostDevice)(ID3DXFont*);
    HRESULT (WINAPI *OnResetDevice)(ID3DXFont*);
};
typedef HRESULT (WINAPI *D3DXCreateFontA_t)(IDirect3DDevice9*, INT height, UINT width, UINT weight,
    UINT miplevels, BOOL italic, DWORD charset, DWORD prec, DWORD quality, DWORD pitchfam,
    const char* face, ID3DXFont** out);
static D3DXCreateFontA_t p_D3DXCreateFontA;

#define DT_LEFT 0x0
#define DT_NOCLIP 0x100

// ---- hook state ----
typedef HRESULT (WINAPI *EndScene_t)(IDirect3DDevice9*);
typedef HRESULT (WINAPI *SetTransform_t)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
typedef HRESULT (WINAPI *SetVSConstF_t)(IDirect3DDevice9*, UINT, const float*, UINT);
static EndScene_t     o_EndScene;
static SetTransform_t o_SetTransform;
static SetVSConstF_t  o_SetVSConstF;
// capture candidate view-proj: log the first few large (>=4 vec4) uploads at low registers per frame
static int g_vsLogged;
static int looks_persp(const float *m) {
    // perspective proj/VP: _44 ~ 0 and a z->w term (_34 or _43 col) ~ +-1; and it's not the 2D [.,.,1,0]/[.,.,.,1] pattern
    float w = m[15];
    int zrow = (m[2]!=0.0f || m[6]!=0.0f || m[8]!=0.0f || m[9]!=0.0f); // real 3x3 rotation content
    return (w > -0.1f && w < 0.1f) && zrow && (m[11]*m[11] + m[14]*m[14] > 0.25f);
}
static HRESULT WINAPI hk_SetVSConstF(IDirect3DDevice9 *dev, UINT reg, const float *data, UINT n) {
    if (data && n >= 4 && reg <= 64 && g_vsLogged < 24 && looks_persp(data)) {
        const float *m = data;
        logmsg("VSConst PERSP? reg=%u n=%u | %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.1f %.1f %.1f %.3f",
            reg, n, m[0],m[1],m[2],m[3], m[4],m[5],m[6],m[7], m[8],m[9],m[10],m[11], m[12],m[13],m[14],m[15]);
        g_vsLogged++;
    }
    return o_SetVSConstF(dev, reg, data, n);
}
static ID3DXFont *g_font;       // big (debug header)
static ID3DXFont *g_fontNum;    // small (control-group number inside the squad banner)
static IDirect3DDevice9 *g_dev;
static LONG g_frames;
static int g_loggedProj;
static D3DMATRIX g_view, g_proj; static int g_haveView, g_haveProj;
static D3DMATRIX g_wview, g_wproj; static int g_haveWView, g_haveWProj;   // world (perspective) camera

// ---- engine world camera via sp::View import hooks (DOW2.exe 3.19 IAT slots) ----
#define IAT_GetProjMat 0x00F88EB0
#define IAT_GetViewMat 0x00F88EB4
#define IAT_ViewSetup  0x00F88EB8
typedef void (__attribute__((fastcall)) *GetMat_t)(void *thisp, void *edx, float *out16);
typedef void (__attribute__((fastcall)) *Setup_t)(void *thisp, void *edx, void *cam, void *rect, float f);
static GetMat_t o_GetProjMat, o_GetViewMat;
static Setup_t  o_ViewSetup;
static float g_mproj[16], g_mview[16], g_vp[16]; static volatile LONG g_haveVP; static int g_vpLogged;
#define ENT_POS_OFF 0x7C      // entity world position (x,y,z) float triplet
#define G_WORLD 0x01335720    // global: contents -> world/game singleton
// control-group number for a squad entity, or -1 if the squad is in no group.
// CGM = *(*(*G_WORLD + 8) + 0x20); assignments vector [CGM+0x380 .. CGM+0x384), 12-byte {squadID, group}.
// Control group of a squad entity, or -1. Must be called on the sim thread (world globals are live there).
// CGM = *(*(*G_WORLD+8)+0x20); control groups = vector at CGM+0x374 (10 entries x 0xc {begin,end,cap}),
// each a list of 4-byte squad handles; the squad's handle is Entity+0x2c. Group index == the Ctrl+N number.
static int cg_for_entity(void *ent) {
    if (IsBadReadPtr(ent, 0x30)) return -1;
    DWORD handle = *(DWORD *)((char *)ent + 0x2c);
    if (!handle) return -1;
    if (IsBadReadPtr((void *)G_WORLD, 4)) return -1;
    DWORD g = *(DWORD *)G_WORLD; if (!g || IsBadReadPtr((void *)(g + 8), 4)) return -1;
    DWORD a = *(DWORD *)(g + 8); if (!a || IsBadReadPtr((void *)(a + 0x20), 4)) return -1;
    DWORD cgm = *(DWORD *)(a + 0x20); if (!cgm || IsBadReadPtr((void *)(cgm + 0x378), 4)) return -1;
    DWORD vb = *(DWORD *)(cgm + 0x374), ve = *(DWORD *)(cgm + 0x378);
    if (!vb || ve < vb || (ve - vb) > 0x200 || IsBadReadPtr((void *)vb, ve - vb)) return -1;
    int gi = 0;
    for (DWORD q = vb; q + 12 <= ve; q += 12, gi++) {
        DWORD lb = *(DWORD *)q, le = *(DWORD *)(q + 4);
        if (le <= lb || (le - lb) > 0x400 || IsBadReadPtr((void *)lb, le - lb)) continue;
        for (DWORD h = lb; h + 4 <= le; h += 4)
            if (*(DWORD *)h == handle) return gi;
    }
    return -1;
}
static int project_world(float x, float y, float z, int W, int H, float *sx, float *sy) {
    float *m = g_vp;
    float cx = x*m[0] + y*m[4] + z*m[8]  + m[12];
    float cy = x*m[1] + y*m[5] + z*m[9]  + m[13];
    float cw = x*m[3] + y*m[7] + z*m[11] + m[15];
    if (cw <= 0.001f) return 0;               // behind camera
    *sx = (cx/cw * 0.5f + 0.5f) * W;
    *sy = (0.5f - cy/cw * 0.5f) * H;
    return 1;
}
#define MAXENT 512
static void *g_ents[MAXENT]; static volatile LONG g_entCount; static int g_entDumped;
static float g_entPos[MAXENT][3]; static int g_entGroup[MAXENT];   // filled on the sim thread (in the decorator tap)
static int cg_for_entity(void *ent);                                // fwd
// sp::View::Setup runs every frame for each pass (shadows + main scene). After Setup we ask the view for its
// projection; the MAIN gameplay view is the one whose projection is perspective (out[15] ~ 0). Capture its VP.
static void __attribute__((fastcall)) hk_ViewSetup(void *thisp, void *edx, void *cam, void *rect, float f) {
    o_ViewSetup(thisp, edx, cam, rect, f);
    if (!o_GetProjMat || !o_GetViewMat) return;
    float pm[16]; o_GetProjMat(thisp, 0, pm);
    if (pm[15] > -0.01f && pm[15] < 0.01f) {            // perspective -> gameplay camera
        float vm[16]; o_GetViewMat(thisp, 0, vm);
        memcpy(g_mproj, pm, 64); memcpy(g_mview, vm, 64);
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++)   // VP = View * Proj (row-major)
            g_vp[r*4+c] = vm[r*4+0]*pm[0*4+c] + vm[r*4+1]*pm[1*4+c] + vm[r*4+2]*pm[2*4+c] + vm[r*4+3]*pm[3*4+c];
        InterlockedExchange(&g_haveVP, 1);
        if (g_vpLogged < 3) {
            logmsg("MAIN VP proj: %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f / %.4f %.4f %.4f %.4f",
                pm[0],pm[1],pm[2],pm[3], pm[4],pm[5],pm[6],pm[7], pm[8],pm[9],pm[10],pm[11], pm[12],pm[13],pm[14],pm[15]);
            logmsg("MAIN VP view: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f",
                vm[0],vm[1],vm[2],vm[3], vm[4],vm[5],vm[6],vm[7], vm[8],vm[9],vm[10],vm[11], vm[12],vm[13],vm[14],vm[15]);
            g_vpLogged++;
        }
    }
}
static int install_view_hooks(void) {
    unsigned char *pj = (unsigned char *)IAT_GetProjMat, *pv = (unsigned char *)IAT_GetViewMat, *ps = (unsigned char *)IAT_ViewSetup;
    if (IsBadReadPtr(pj, 4) || IsBadReadPtr(pv, 4) || IsBadReadPtr(ps, 4)) { logmsg("IAT not readable - not 3.19?"); return 0; }
    o_GetViewMat = *(GetMat_t *)pv;                 // originals, kept (read-only)
    o_GetProjMat = *(GetMat_t *)pj;
    o_ViewSetup  = *(Setup_t  *)ps;
    DWORD old; VirtualProtect(ps, 4, PAGE_READWRITE, &old);
    *(void **)ps = (void *)hk_ViewSetup;            // hook only Setup
    VirtualProtect(ps, 4, old, &old);
    logmsg("view hook: Setup orig=%p GetProj=%p GetView=%p", (void*)o_ViewSetup, (void*)o_GetProjMat, (void*)o_GetViewMat);
    return 1;
}

static void ensure_font(IDirect3DDevice9 *dev) {
    if (!p_D3DXCreateFontA || g_fontNum) return;
    HRESULT hr = p_D3DXCreateFontA(dev, 13, 0, FW_BOLD, 1, FALSE, DEFAULT_CHARSET,   // small bold number inside the banner
        OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Arial", &g_fontNum);
    logmsg("font -> hr=0x%08lx %p", (unsigned long)hr, (void*)g_fontNum);
}

static int g_projLogged;
static HRESULT WINAPI hk_SetTransform(IDirect3DDevice9 *dev, D3DTRANSFORMSTATETYPE st, const D3DMATRIX *m) {
    if (m) {
        if (st == D3DTS_VIEW)       { g_view = *m; g_haveView = 1; }
        if (st == D3DTS_PROJECTION) {
            g_proj = *m; g_haveProj = 1;
            // diagnostic: perspective proj has _44~0 and |_34|~1; ortho (UI) has _44~1
            int persp = (m->_44 > -0.01f && m->_44 < 0.01f);
            if (g_projLogged < 12)
                logmsg("PROJ %s: _11=%.5f _22=%.5f _34=%.3f _43=%.3f _44=%.3f", persp ? "PERSPECTIVE" : "ortho",
                       m->_11, m->_22, m->_34, m->_43, m->_44), g_projLogged++;
            if (persp) { g_wproj = *m; g_haveWProj = 1; g_wview = g_view; g_haveWView = g_haveView; }
        }
    }
    return o_SetTransform(dev, st, m);
}

static HRESULT WINAPI hk_EndScene(IDirect3DDevice9 *dev) {
    g_dev = dev;
    InterlockedIncrement(&g_frames);
    ensure_font(dev);
    LONG ents = InterlockedExchange(&g_entCount, 0);    // squads collected during the frame just rendered
    if (ents > MAXENT) ents = MAXENT;
    // The squad banner sits above the unit. We project a point raised in WORLD space (perspective-correct,
    // so the offset is stable at any depth), then nudge in screen space to land inside the banner box.
    const float WORLD_UP = 4.0f; // raise to ~torso/banner height (world units)
    const int BANNER_DY = 31;    // screen nudge up so the number sits inside the banner box (px)
    const int BOX_HALF = 17;     // right edge of the number inside the box (px from centre)
    const D3DCOLOR COL = D3DCOLOR_XRGB(255, 255, 255);   // white
    if (g_haveVP && g_fontNum) {
        D3DVIEWPORT9 vp; int W = 1280, H = 800;
        if (SUCCEEDED(IDirect3DDevice9_GetViewport(dev, &vp)) && vp.Width > 0) { W = vp.Width; H = vp.Height; }
        for (LONG i = 0; i < ents; i++) {
            int cg = g_entGroup[i];                      // control group (resolved on the sim thread), -1 = none
            if (cg < 0) continue;                         // only squads that are in a control group
            float sx, sy;
            if (!project_world(g_entPos[i][0], g_entPos[i][1] + WORLD_UP, g_entPos[i][2], W, H, &sx, &sy)) continue;
            if (sx < -50 || sx > W + 50 || sy < -50 || sy > H + 50) continue;
            char n[8]; wsprintfA(n, "%d", cg);
            int bx = (int)sx, by = (int)sy - BANNER_DY;   // banner centre
            RECT nr = { bx + BOX_HALF - 15, by, bx + BOX_HALF, by + 14 }; // top-right inside the box
            g_fontNum->lpVtbl->DrawTextA(g_fontNum, NULL, n, -1, &nr, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOCLIP, COL);
        }
    }
    return o_EndScene(dev);
}

// ---- decorator Update tap: collect the Entity of every on-screen squad each frame ----
// FUN_00b24370(decorator* ecx, Entity* [esp+4], int*, int*) runs once per visible squad.
#define FN_DECOR_UPDATE 0x00B24370
static const unsigned char DEC_PRO[6] = {0x55,0x8b,0xec,0x83,0xe4,0xf8};  // push ebp; mov ebp,esp; and esp,-8
void *cg_tramp;                                   // trampoline (orig prologue + jmp back)
void __attribute__((cdecl)) cg_record(void *ent) {
    // runs on the sim thread (inside the decorator update), where the world globals are live
    LONG i = InterlockedIncrement(&g_entCount) - 1;
    if (i < 0 || i >= MAXENT || !ent) return;
    g_ents[i] = ent;
    g_entGroup[i] = -1;
    if (!IsBadReadPtr(ent, ENT_POS_OFF + 12)) {
        const float *pos = (const float *)((char *)ent + ENT_POS_OFF);
        g_entPos[i][0] = pos[0]; g_entPos[i][1] = pos[1]; g_entPos[i][2] = pos[2];
        g_entGroup[i] = cg_for_entity(ent);              // resolve control group HERE (sim thread)
    }
}
__asm__(
".text\n"
".globl _cg_tap\n"
"_cg_tap:\n"
"  pushal\n"
"  pushfl\n"
"  movl 0x28(%esp), %eax\n"      // Entity = original [esp+4]
"  pushl %eax\n"
"  call _cg_record\n"
"  addl $4, %esp\n"
"  popfl\n"
"  popal\n"
"  jmp *_cg_tramp\n"
);
extern void cg_tap(void);
static int install_decor_hook(void) {
    unsigned char *t = (unsigned char *)FN_DECOR_UPDATE;
    if (IsBadReadPtr(t, 6) || memcmp(t, DEC_PRO, 6)) { logmsg("decor Update prologue mismatch - skip"); return 0; }
    unsigned char *tr = VirtualAlloc(NULL, 16, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tr, DEC_PRO, 6); tr[6] = 0xE9; *(DWORD*)(tr+7) = (FN_DECOR_UPDATE+6) - (DWORD)(tr+11);
    cg_tramp = tr;
    DWORD old; VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &old);
    t[0] = 0xE9; *(DWORD*)(t+1) = (DWORD)cg_tap - (DWORD)(t+5); t[5] = 0x90;
    VirtualProtect(t, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), t, 6);
    logmsg("decor Update hook installed (tap=%p tramp=%p)", (void*)cg_tap, cg_tramp);
    return 1;
}

// patch one COM vtable slot, return the previous pointer
static void *vt_hook(void **vtbl, int index, void *fn) {
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    void *prev = vtbl[index];
    vtbl[index] = fn;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return prev;
}

// create a throwaway device to read the device vtable, then hook the shared vtable slots
static int install_d3d_hooks(void) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9) { logmsg("d3d9.dll not loaded yet"); return 0; }
    IDirect3D9 *(WINAPI *pCreate)(UINT) = (void*)GetProcAddress(d3d9, "Direct3DCreate9");
    if (!pCreate) { logmsg("no Direct3DCreate9"); return 0; }
    IDirect3D9 *d3d = pCreate(D3D_SDK_VERSION);
    if (!d3d) { logmsg("Direct3DCreate9 failed"); return 0; }

    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "cgnprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("cgnprobe", "cgn", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = hwnd;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_NOWINDOWCHANGES, &pp, &dev);
    if (FAILED(hr) || !dev) {
        logmsg("temp CreateDevice failed hr=0x%08lx", (unsigned long)hr);
        IDirect3D9_Release(d3d); if (hwnd) DestroyWindow(hwnd); return 0;
    }
    void **vtbl = *(void***)dev;                 // IDirect3DDevice9 vtable (shared by the real device)
    o_EndScene     = (EndScene_t)     vt_hook(vtbl, 42, (void*)hk_EndScene);
    o_SetTransform = (SetTransform_t) vt_hook(vtbl, 44, (void*)hk_SetTransform);
    (void)hk_SetVSConstF; (void)o_SetVSConstF;   // VS-const capture retired; world VP comes from sp::View hooks
    logmsg("vtable hooked: EndScene42=%p SetTransform44=%p", (void*)o_EndScene, (void*)o_SetTransform);

    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    if (hwnd) DestroyWindow(hwnd);
    return 1;
}

static DWORD WINAPI worker(LPVOID _) {
    // load d3dx9 for the font (try the versions present in the prefix)
    const char *vers[] = { "d3dx9_43.dll", "d3dx9_42.dll", "d3dx9_41.dll", "d3dx9_40.dll", "d3dx9_38.dll", "d3dx9_35.dll", "d3dx9_34.dll", "d3dx9_31.dll", "d3dx9_30.dll", NULL };
    for (int i = 0; vers[i]; i++) {
        HMODULE h = LoadLibraryA(vers[i]);
        if (h) { p_D3DXCreateFontA = (D3DXCreateFontA_t)GetProcAddress(h, "D3DXCreateFontA");
                 if (p_D3DXCreateFontA) { logmsg("using %s", vers[i]); break; } }
    }
    if (!p_D3DXCreateFontA) logmsg("WARNING: no d3dx9 D3DXCreateFontA found - text will not draw");

    install_view_hooks();   // IAT is populated by the time we run; capture the gameplay camera matrices
    install_decor_hook();   // tap the squad decorator update to collect on-screen squad entities
    // wait for d3d9.dll + a device to exist, then hook
    for (int i = 0; i < 120; i++) {
        if (GetModuleHandleA("d3d9.dll") && install_d3d_hooks()) { logmsg("hooks installed after %d s", i); return 0; }
        Sleep(1000);
    }
    logmsg("gave up waiting for d3d9.dll");
    return 0;
}

static int already_loaded(const char *name) {
    char n[96]; wsprintfA(n, "Local\\dow2patch_%s_%lu", name, GetCurrentProcessId());
    HANDLE m = CreateMutexA(NULL, FALSE, n);
    return m && GetLastError() == ERROR_ALREADY_EXISTS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID _) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    GetModuleFileNameW(h, g_log, MAX_PATH); wchar_t *sl = wcsrchr(g_log, L'\\'); if (sl) sl[1] = 0; else g_log[0] = 0;
    wchar_t ini[MAX_PATH]; wcscpy(ini, g_log); wcscat(ini, L"CGNumbers.ini"); wcscat(g_log, L"CGNumbers.log");
    if (already_loaded("CGNumbers")) return TRUE;
    if (!GetPrivateProfileIntW(L"CGNumbers", L"Enabled", 1, ini)) { logmsg("disabled in CGNumbers.ini"); return TRUE; }
    logmsg("CGNumbers v1 attach");
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}
