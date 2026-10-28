/*
 * Display modes for library games:
 *   - Borderless fullscreen: the game window loses its frame and is sized to
 *     the largest rectangle with the game's aspect ratio (optionally a whole
 *     multiple of its original resolution). Leftover space is covered by a
 *     black backdrop window, so a 4:3 game is pillarboxed, never stretched.
 *   - Resizable window: the game window gets a resize border. Whatever size
 *     the player drags it to is snapped back to the correct aspect ratio and
 *     remembered (scale + position) for next time.
 *
 * No frame capture or re-rendering is involved, so there is zero added input
 * latency: the game draws straight into its own (resized) window.
 *
 * Runs on its own thread, separate from the keyboard hook thread, because
 * changing another process's window can block if that process is busy.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <objbase.h>
#include "app.h"

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);

/* ---- minimal ITaskbarList2 (for MarkFullscreenWindow) ---------------- */
typedef struct TBL2 TBL2;
typedef struct {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(TBL2 *, REFIID, void **);
    ULONG   (STDMETHODCALLTYPE *AddRef)(TBL2 *);
    ULONG   (STDMETHODCALLTYPE *Release)(TBL2 *);
    HRESULT (STDMETHODCALLTYPE *HrInit)(TBL2 *);
    HRESULT (STDMETHODCALLTYPE *AddTab)(TBL2 *, HWND);
    HRESULT (STDMETHODCALLTYPE *DeleteTab)(TBL2 *, HWND);
    HRESULT (STDMETHODCALLTYPE *ActivateTab)(TBL2 *, HWND);
    HRESULT (STDMETHODCALLTYPE *SetActiveAlt)(TBL2 *, HWND);
    HRESULT (STDMETHODCALLTYPE *MarkFullscreenWindow)(TBL2 *, HWND, BOOL);
} TBL2Vtbl;
struct TBL2 { const TBL2Vtbl *lpVtbl; };
static const GUID kCLSID_TaskbarList = { 0x56FDF344, 0xFD6D, 0x11d0, { 0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90 } };
static const GUID kIID_ITaskbarList2 = { 0x602D4995, 0xB13A, 0x429b, { 0xA6, 0x6E, 0x19, 0x35, 0xE4, 0x4F, 0x43, 0x17 } };

/* ---- state (window thread only) ------------------------------------- */
#define MAX_TOUCHED 16

typedef struct {
    HWND     h;
    WCHAR    path[MAX_PATH];  /* library path of the owning game */
    LONG_PTR style, ex;       /* original styles                 */
    RECT     rect;            /* original window rect            */
    int      origCW, origCH;  /* original client size            */
    int      mode;            /* mode currently applied          */
    RECT     applied;         /* window rect we last set         */
    int      appCW, appCH;    /* client size we last set         */
    DWORD    appliedTick;
    int      fixes;           /* game-initiated resizes we followed */
    DWORD    fixTick;
    BOOL     gaveUp;
    BOOL     marked;          /* MarkFullscreenWindow(TRUE) done */
} Touched;

static Touched s_t[MAX_TOUCHED];
static int     s_nt;
static HWND    s_cur;          /* library game window in the foreground */
static DWORD   s_curPid;
static BOOL    s_drag;
static BOOL    s_hotkeys;
static HWND    s_backdrop;
static TBL2   *s_tb;
static HANDLE  s_thread;
static DWORD   s_tid;

/* ---- picture zoom (Windows full-screen magnification, done by DWM) ---- */
typedef BOOL (WINAPI *PFN_MagBool)(void);
typedef BOOL (WINAPI *PFN_MagXform)(float, int, int);
typedef BOOL (WINAPI *PFN_MagSmooth)(BOOL);
static PFN_MagBool   pMagInitialize, pMagUninitialize;
static PFN_MagXform  pMagSetFullscreenTransform;
static PFN_MagSmooth pMagSetFullscreenUseBitmapSmoothing;   /* not on every Windows build */
static BOOL   s_magReady;
static BOOL   s_zoomOn;
static HWND   s_zoomWnd;
static RECT   s_zoomSrc;
static int    s_zoomInteger, s_zoomFilter;
static BOOL   s_clipped;

#define HK_TOGGLE 1
#define HK_UP     2
#define HK_DOWN   3

#define F_BASE   1
#define F_SCALE  2
#define F_POS    4
#define F_MODE   8

/* ---- helpers --------------------------------------------------------- */
static int Iabs(int v) { return v < 0 ? -v : v; }
static BOOL Near(int a, int b) { return Iabs(a - b) <= 2; }

void WinMode_AspectOf(const Game *g, int *num, int *den)
{
    switch (g->aspect) {
    case AR_4_3:   *num = 4;  *den = 3;  return;
    case AR_16_9:  *num = 16; *den = 9;  return;
    case AR_16_10: *num = 16; *den = 10; return;
    case AR_5_4:   *num = 5;  *den = 4;  return;
    }
    if (g->baseW > 0 && g->baseH > 0) { *num = g->baseW; *den = g->baseH; }
    else { *num = 4; *den = 3; }
}

/* Copy of the game with this library path; FALSE if it was removed. */
static BOOL GetGame(const WCHAR *path, Game *out)
{
    BOOL ok = FALSE;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ngames; ++i) {
        if (SameText(g_games[i].path, path)) { memcpy(out, &g_games[i], sizeof *out); ok = TRUE; break; }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

/* Writes selected fields back to the library and tells the UI to save. */
static void StoreGame(const Game *src, UINT what)
{
    int idx = -1;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ngames; ++i) {
        if (!SameText(g_games[i].path, src->path)) continue;
        Game *g = &g_games[i];
        if (what & F_BASE)  { g->baseW = src->baseW; g->baseH = src->baseH; }
        if (what & F_SCALE) g->winScale = src->winScale;
        if (what & F_POS)   { g->winX = src->winX; g->winY = src->winY; g->hasPos = src->hasPos; }
        if (what & F_MODE)  g->dispMode = src->dispMode;
        idx = i;
        break;
    }
    LeaveCriticalSection(&g_lock);
    if (idx >= 0 && g_hMain) PostMessageW(g_hMain, WM_UI_GAMEUPDATED, (WPARAM)idx, 0);
}

static Touched *FindTouched(HWND h)
{
    for (int i = 0; i < s_nt; ++i) if (s_t[i].h == h) return &s_t[i];
    return NULL;
}

static void RemoveTouched(Touched *t)
{
    int i = (int)(t - s_t);
    s_t[i] = s_t[--s_nt];
}

static void ClientSize(HWND h, int *w, int *hh)
{
    RECT r;
    GetClientRect(h, &r);
    *w = r.right - r.left;
    *hh = r.bottom - r.top;
}

static void MarkFullscreen(Touched *t, BOOL on)
{
    if (!s_tb || t->marked == on) return;
    s_tb->lpVtbl->MarkFullscreenWindow(s_tb, t->h, on);
    t->marked = on;
}

/* ---- backdrop (black bars) ------------------------------------------ */
static LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r);
        FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void CreateBackdrop(void)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"KeyRemapper.Backdrop";
    RegisterClassW(&wc);
    s_backdrop = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                                 0, 0, 1, 1, NULL, NULL, g_hInst, NULL);
}

static void HideBackdrop(void)
{
    if (s_backdrop && IsWindowVisible(s_backdrop)) ShowWindow(s_backdrop, SW_HIDE);
}

/* Covers the game's monitor, directly behind the game window. Must run in
 * the thread's own (per-monitor) DPI context. */
static void ShowBackdropBehind(HWND game)
{
    if (!s_backdrop) return;
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST), &mi)) return;
    RECT r = mi.rcMonitor;
    SetWindowPos(s_backdrop, game, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
}

/* ---- target computation (in the game's DPI space) ------------------- */
static void MagLoad(void)
{
    HMODULE m = LoadLibraryW(L"Magnification.dll");
    if (!m) return;
    pMagInitialize = (PFN_MagBool)(void *)GetProcAddress(m, "MagInitialize");
    pMagUninitialize = (PFN_MagBool)(void *)GetProcAddress(m, "MagUninitialize");
    pMagSetFullscreenTransform = (PFN_MagXform)(void *)GetProcAddress(m, "MagSetFullscreenTransform");
    pMagSetFullscreenUseBitmapSmoothing = (PFN_MagSmooth)(void *)GetProcAddress(m, "MagSetFullscreenUseBitmapSmoothing");
    s_magReady = pMagInitialize && pMagSetFullscreenTransform && pMagInitialize();
}

static void ZoomOff(void)
{
    if (s_zoomOn && s_magReady) pMagSetFullscreenTransform(1.0f, 0, 0);
    s_zoomOn = FALSE;
    s_zoomWnd = NULL;
    SetRectEmpty(&s_zoomSrc);
    if (s_clipped) { ClipCursor(NULL); s_clipped = FALSE; }
}

/* (Re)computes the zoom from where the game window really is. Runs in the
 * thread's per-monitor DPI context, i.e. physical pixels, like the API. */
static void ZoomUpdate(void)
{
    if (!s_zoomOn) return;
    if (!s_zoomWnd || !IsWindow(s_zoomWnd) || IsIconic(s_zoomWnd)) { ZoomOff(); return; }

    RECT r;
    GetWindowRect(s_zoomWnd, &r);
    int sw = r.right - r.left, sh = r.bottom - r.top;
    if (sw < 16 || sh < 16) return;

    if (!EqualRect(&r, &s_zoomSrc)) {
        POINT o = { 0, 0 };
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        int mw = mi.rcMonitor.right - mi.rcMonitor.left, mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        float sx = (float)mw / (float)sw, sy = (float)mh / (float)sh;
        float z = sx < sy ? sx : sy;
        if (s_zoomInteger && z >= 1.0f) z = (float)(int)z;
        if (z < 1.0f) z = 1.0f;

        /* centre the game in the magnified view (offsets: unmagnified px from the primary's corner) */
        float vw = (float)mw / z, vh = (float)mh / z;
        int xo = r.left - (int)((vw - (float)sw) * 0.5f + 0.5f);
        int yo = r.top - (int)((vh - (float)sh) * 0.5f + 0.5f);
        if (s_magReady) pMagSetFullscreenTransform(z, xo, yo);
        s_zoomSrc = r;
    }

    /* keep the (real) mouse on the game so the magnified cursor matches clicks */
    RECT c;
    if (!GetClipCursor(&c) || !EqualRect(&c, &r)) { ClipCursor(&r); s_clipped = TRUE; }
}

static void ZoomOn(HWND h, const Game *g)
{
    if (!s_magReady) return;
    if (s_zoomWnd != h || s_zoomInteger != (g->scaling == SC_INTEGER)) SetRectEmpty(&s_zoomSrc);
    s_zoomWnd = h;
    s_zoomOn = TRUE;
    s_zoomInteger = g->scaling == SC_INTEGER;
    if (pMagSetFullscreenUseBitmapSmoothing && s_zoomFilter != g->filter) {
        pMagSetFullscreenUseBitmapSmoothing(g->filter == FL_SMOOTH);
        s_zoomFilter = g->filter;
    }
    ZoomUpdate();
}

static void BorderlessTarget(const Game *g, const RECT *mon, RECT *client)
{
    int num, den;
    WinMode_AspectOf(g, &num, &den);
    int mw = mon->right - mon->left, mh = mon->bottom - mon->top;
    int W = 0, H = 0;

    if (g->scaling == SC_INTEGER && g->baseH > 0) {
        int k = mh / g->baseH;
        while (k > 1 && MulDiv(g->baseH * k, num, den) > mw) k--;
        if (k >= 1 && MulDiv(g->baseH * k, num, den) <= mw) { H = g->baseH * k; W = MulDiv(H, num, den); }
    }
    if (!W) {
        H = mh;
        W = MulDiv(H, num, den);
        if (W > mw) { W = mw; H = MulDiv(W, den, num); }
    }
    client->left = mon->left + (mw - W) / 2;
    client->top = mon->top + (mh - H) / 2;
    client->right = client->left + W;
    client->bottom = client->top + H;
}

/* Only these frame bits are ever changed; everything else (visible,
 * disabled while the game shows a modal dialog, ...) is left to the game. */
#define FRAME_STYLE   ((LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MAXIMIZEBOX | WS_MINIMIZEBOX))
#define FRAME_EXSTYLE ((LONG_PTR)(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE))

static LONG_PTR BorderlessStyle(LONG_PTR s) { return s & ~FRAME_STYLE; }
static LONG_PTR BorderlessEx(LONG_PTR e)    { return e & ~FRAME_EXSTYLE; }
static LONG_PTR WindowStyle(LONG_PTR s)     { return (s & ~(LONG_PTR)WS_MAXIMIZEBOX) | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX; }

/* Sets only the frame bits; returns TRUE if anything changed. */
static BOOL SetFrameBits(HWND h, LONG_PTR wantStyle, LONG_PTR wantEx)
{
    BOOL changed = FALSE;
    LONG_PTR cs = GetWindowLongPtrW(h, GWL_STYLE), ce = GetWindowLongPtrW(h, GWL_EXSTYLE);
    LONG_PTR ns = (cs & ~FRAME_STYLE) | (wantStyle & FRAME_STYLE);
    LONG_PTR ne = (ce & ~FRAME_EXSTYLE) | (wantEx & FRAME_EXSTYLE);
    if (ns != cs) { SetWindowLongPtrW(h, GWL_STYLE, ns); changed = TRUE; }
    if (ne != ce) { SetWindowLongPtrW(h, GWL_EXSTYLE, ne); changed = TRUE; }
    return changed;
}

static void Frame(HWND h, LONG_PTR st, LONG_PTR ex, RECT *r)
{
    st = (GetWindowLongPtrW(h, GWL_STYLE) & ~FRAME_STYLE) | (st & FRAME_STYLE);
    ex = (GetWindowLongPtrW(h, GWL_EXSTYLE) & ~FRAME_EXSTYLE) | (ex & FRAME_EXSTYLE);
    UINT dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96;
    AdjustWindowRectExForDpi(r, (DWORD)st, GetMenu(h) != NULL, (DWORD)ex, dpi);
}

/* Largest window scale (1/1000) that still fits the monitor's work area. */
static int MaxScale(HWND h, const Game *g, LONG_PTR st, LONG_PTR ex, const RECT *work)
{
    int num, den;
    WinMode_AspectOf(g, &num, &den);
    RECT z = { 0, 0, 0, 0 };
    Frame(h, st, ex, &z);
    int fw = z.right - z.left, fh = z.bottom - z.top;
    int maxH = (work->bottom - work->top) - fh;
    int maxW = (work->right - work->left) - fw;
    if (maxH < 1 || maxW < 1 || g->baseH <= 0) return 1000;
    int byH = MulDiv(maxH, 1000, g->baseH);
    int byW = MulDiv(maxW, 1000 * den, num * g->baseH);
    int s = byH < byW ? byH : byW;
    if (g->scaling == SC_INTEGER && s >= 1000) s = (s / 1000) * 1000;
    return s < 100 ? 100 : s;
}

static int ClampScale(HWND h, const Game *g, int s, LONG_PTR st, LONG_PTR ex, const RECT *work)
{
    int mx = MaxScale(h, g, st, ex, work);
    if (g->scaling == SC_INTEGER) s = ((s + 500) / 1000) * 1000;
    if (s < 250) s = (g->scaling == SC_INTEGER) ? 1000 : 250;
    if (s > mx) s = mx;
    return s;
}

static void WindowClientSize(const Game *g, int s, int *W, int *H)
{
    int num, den;
    WinMode_AspectOf(g, &num, &den);
    *H = MulDiv(g->baseH, s, 1000);
    *W = MulDiv(*H, num, den);
}

/* ---- apply / restore -------------------------------------------------- */
static DPI_AWARENESS_CONTEXT EnterGameDpi(HWND h)
{
    DPI_AWARENESS_CONTEXT c = GetWindowDpiAwarenessContext(h);
    return SetThreadDpiAwarenessContext(c ? c : DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

static void Restore(Touched *t)
{
    if (IsWindow(t->h)) {
        DPI_AWARENESS_CONTEXT old = EnterGameDpi(t->h);
        MarkFullscreen(t, FALSE);
        SetFrameBits(t->h, t->style, t->ex);
        if (!IsRectEmpty(&t->rect))
            SetWindowPos(t->h, NULL, t->rect.left, t->rect.top, t->rect.right - t->rect.left, t->rect.bottom - t->rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
        else
            SetWindowPos(t->h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
        SetThreadDpiAwarenessContext(old);
    }
    if (t->h == s_cur) HideBackdrop();
    if (t->h == s_zoomWnd) ZoomOff();
    RemoveTouched(t);
}

/* Applies the game's display mode to window t (thread is in game DPI space). */
static void ApplyIn(Touched *t, Game *g)
{
    HWND h = t->h;
    if (IsIconic(h)) return;

    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return;
    if (IsZoomed(h)) ShowWindowAsync(h, SW_RESTORE);

    LONG_PTR st, ex;
    RECT client;
    RECT cur;
    GetWindowRect(h, &cur);

    if (g->dispMode == DM_BORDERLESS && g->fsMethod == FS_ZOOM) {
        /* the game keeps drawing at its own resolution; DWM zooms the picture */
        st = BorderlessStyle(t->style);
        ex = BorderlessEx(t->ex);
        POINT o = { 0, 0 };
        MONITORINFO pm;
        pm.cbSize = sizeof pm;
        if (!GetMonitorInfoW(MonitorFromPoint(o, MONITOR_DEFAULTTOPRIMARY), &pm)) return;
        int mw = pm.rcMonitor.right - pm.rcMonitor.left, mh = pm.rcMonitor.bottom - pm.rcMonitor.top;
        client.left = pm.rcMonitor.left + (mw - g->baseW) / 2;
        client.top = pm.rcMonitor.top + (mh - g->baseH) / 2;
        client.right = client.left + g->baseW;
        client.bottom = client.top + g->baseH;
    } else if (g->dispMode == DM_BORDERLESS) {
        st = BorderlessStyle(t->style);
        ex = BorderlessEx(t->ex);
        BorderlessTarget(g, &mi.rcMonitor, &client);
    } else {
        st = WindowStyle(t->style);
        ex = t->ex;
        int s = g->winScale;
        if (s <= 0) s = (g->baseH > 0 && t->origCH > 0) ? MulDiv(t->origCH, 1000, g->baseH) : 1000;
        int s2 = ClampScale(h, g, s, st, ex, &mi.rcWork);
        if (s2 != g->winScale) { g->winScale = s2; StoreGame(g, F_SCALE); }

        int W, H;
        WindowClientSize(g, g->winScale, &W, &H);
        RECT fr = { 0, 0, W, H };
        Frame(h, st, ex, &fr);
        int ww = fr.right - fr.left, wh = fr.bottom - fr.top;

        int x, y;
        if (g->hasPos) {
            POINT c = { g->winX + ww / 2, g->winY + wh / 2 };
            if (MonitorFromPoint(c, MONITOR_DEFAULTTONULL)) { x = g->winX; y = g->winY; }
            else { x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2; y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2; }
        } else {
            /* keep the window centred where it is */
            x = (cur.left + cur.right) / 2 - ww / 2;
            y = (cur.top + cur.bottom) / 2 - wh / 2;
        }
        /* keep it on the work area */
        if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
        if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;

        client.left = x - fr.left;
        client.top = y - fr.top;
        client.right = client.left + W;
        client.bottom = client.top + H;
    }

    RECT wr = client;
    Frame(h, st, ex, &wr);

    BOOL styleChanged = SetFrameBits(h, st, ex);
    if (styleChanged || !EqualRect(&cur, &wr)) {
        SetWindowPos(h, NULL, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS |
                     (styleChanged ? SWP_FRAMECHANGED : 0));
        t->appliedTick = GetTickCount();
    }
    t->applied = wr;
    t->appCW = client.right - client.left;
    t->appCH = client.bottom - client.top;
    t->mode = g->dispMode;

    MarkFullscreen(t, g->dispMode == DM_BORDERLESS);
}

static void Apply(Touched *t, Game *g)
{
    if (t->gaveUp || g->baseW <= 0 || g->baseH <= 0) return;   /* wait until we know its size */
    DPI_AWARENESS_CONTEXT old = EnterGameDpi(t->h);
    ApplyIn(t, g);
    BOOL bars = FALSE;
    if (g->dispMode == DM_BORDERLESS && !IsIconic(t->h)) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        if (GetMonitorInfoW(MonitorFromWindow(t->h, MONITOR_DEFAULTTONEAREST), &mi))
            bars = !EqualRect(&mi.rcMonitor, &t->applied);
    }
    SetThreadDpiAwarenessContext(old);

    BOOL front = t->h == s_cur && GetForegroundWindow() == t->h;
    if (front && bars) ShowBackdropBehind(t->h);
    else if (t->h == s_cur) HideBackdrop();

    if (front && g->dispMode == DM_BORDERLESS && g->fsMethod == FS_ZOOM && !IsIconic(t->h)) ZoomOn(t->h, g);
    else if (s_zoomWnd == t->h || (t->h == s_cur && s_zoomOn)) ZoomOff();
}

static void DetectBase(Touched *t, Game *g);

/* Records a window the first time we see it and works out the game's
 * original resolution (base size). */
static Touched *Touch(HWND h, const WCHAR *path, Game *g)
{
    Touched *t = FindTouched(h);
    if (t) return t;

    /* forget dead windows first */
    for (int i = s_nt - 1; i >= 0; --i) if (!IsWindow(s_t[i].h)) RemoveTouched(&s_t[i]);
    if (s_nt >= MAX_TOUCHED) RemoveTouched(&s_t[0]);

    t = &s_t[s_nt++];
    memset(t, 0, sizeof *t);
    t->h = h;
    lstrcpynW(t->path, path, MAX_PATH);

    DPI_AWARENESS_CONTEXT old = EnterGameDpi(h);
    t->style = GetWindowLongPtrW(h, GWL_STYLE);
    t->ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    SetThreadDpiAwarenessContext(old);
    DetectBase(t, g);
    return t;
}

/* Reads the untouched window's size and decides the game's base size.
 * Retried until the window has a real size (e.g. it started minimized). */
static void DetectBase(Touched *t, Game *g)
{
    HWND h = t->h;
    if (t->mode != DM_NONE || IsIconic(h)) return;
    DPI_AWARENESS_CONTEXT old = EnterGameDpi(h);
    GetWindowRect(h, &t->rect);
    ClientSize(h, &t->origCW, &t->origCH);

    int cw = t->origCW, ch = t->origCH;
    if (cw >= 64 && ch >= 48) {
        BOOL newBase = FALSE;
        if (g->baseW <= 0 || g->baseH <= 0) {
            newBase = TRUE;
        } else if (!(Near(cw, g->baseW) && Near(ch, g->baseH))) {
            /* Is this a size we set earlier (game remembered it)? Then keep base. */
            MONITORINFO mi;
            mi.cbSize = sizeof mi;
            BOOL known = FALSE;
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) {
                RECT bt;
                BorderlessTarget(g, &mi.rcMonitor, &bt);
                if (Near(cw, bt.right - bt.left) && Near(ch, bt.bottom - bt.top)) known = TRUE;
                if (!known && g->winScale > 0) {
                    int W, H;
                    WindowClientSize(g, g->winScale, &W, &H);
                    if (Near(cw, W) && Near(ch, H)) known = TRUE;
                }
            }
            newBase = !known;
        }
        if (newBase) {
            g->baseW = cw;
            g->baseH = ch;
            StoreGame(g, F_BASE);
        }
    }
    SetThreadDpiAwarenessContext(old);
}

/* ---- hotkeys --------------------------------------------------------- */
static void SetHotkeys(BOOL on)
{
    if (on == s_hotkeys) return;
    if (on) {
        RegisterHotKey(NULL, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_RETURN);
        RegisterHotKey(NULL, HK_UP, MOD_CONTROL | MOD_ALT, VK_PRIOR);
        RegisterHotKey(NULL, HK_DOWN, MOD_CONTROL | MOD_ALT, VK_NEXT);
    } else {
        UnregisterHotKey(NULL, HK_TOGGLE);
        UnregisterHotKey(NULL, HK_UP);
        UnregisterHotKey(NULL, HK_DOWN);
    }
    s_hotkeys = on;
}

static int StepScale(int s, int dir, BOOL integer)
{
    static const int steps[] = { 250, 333, 500, 667, 750, 1000, 1250, 1500, 1750, 2000, 2250, 2500,
                                 3000, 3500, 4000, 4500, 5000, 6000, 7000, 8000 };
    const int n = (int)(sizeof steps / sizeof steps[0]);
    if (integer) {
        int k = (s + 500) / 1000;
        k += dir;
        return k < 1 ? 1000 : k * 1000;
    }
    if (dir > 0) {
        for (int i = 0; i < n; ++i) if (steps[i] > s + 5) return steps[i];
        return steps[n - 1];
    }
    for (int i = n - 1; i >= 0; --i) if (steps[i] < s - 5) return steps[i];
    return steps[0];
}

/* ---- evaluation ------------------------------------------------------ */
static BOOL SuitableWindow(HWND h)
{
    if (!IsWindowVisible(h)) return FALSE;
    if (GetWindow(h, GW_OWNER)) return FALSE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return FALSE;
    WCHAR cls[16];
    if (GetClassNameW(h, cls, 16) && SameText(cls, L"#32770")) return FALSE;
    if (IsIconic(h)) return TRUE;
    int w, hh;
    ClientSize(h, &w, &hh);
    return w >= 160 && hh >= 120;
}

static void Deactivate(void)
{
    ZoomOff();
    HideBackdrop();
    SetHotkeys(FALSE);
    s_cur = NULL;
    s_curPid = 0;
    s_drag = FALSE;
}

/* Drops changes on windows whose game no longer wants them. */
static void SweepTouched(void)
{
    Game g;
    for (int i = s_nt - 1; i >= 0; --i) {
        Touched *t = &s_t[i];
        if (!IsWindow(t->h)) { if (t->h == s_cur) HideBackdrop(); RemoveTouched(t); continue; }
        if (!GetGame(t->path, &g) || g.dispMode == DM_NONE) Restore(t);
    }
}

static void Evaluate(BOOL reload)
{
    if (reload) {
        SweepTouched();
        for (int i = 0; i < s_nt; ++i) s_t[i].gaveUp = FALSE;
    }

    HWND fg = GetForegroundWindow();
    HWND h = fg ? GetAncestor(fg, GA_ROOT) : NULL;
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    if (!h || !pid || pid == GetCurrentProcessId()) { Deactivate(); return; }

    WCHAR path[MAX_PATH];
    int idx = FindGameForPid(pid, path);
    if (idx < 0) { Deactivate(); return; }

    if (!SuitableWindow(h)) {
        /* a dialog or splash of the same game: leave things as they are */
        if (pid == s_curPid) { HideBackdrop(); return; }
        Deactivate();
        return;
    }

    Game g;
    if (!GetGame(path, &g)) { Deactivate(); return; }

    if (h != s_cur) s_drag = FALSE;
    s_cur = h;
    s_curPid = pid;
    SetHotkeys(TRUE);

    if (g.dispMode == DM_NONE) {
        Touched *t = FindTouched(h);
        if (t) Restore(t);
        HideBackdrop();
        return;
    }

    Touched *t = Touch(h, path, &g);
    if (g.baseH <= 0) DetectBase(t, &g);
    if (reload && (g.baseW <= 0 || g.baseH <= 0) && t->origCW >= 64) {
        /* "Forget size" was pressed: the window's original size is the base */
        g.baseW = t->origCW;
        g.baseH = t->origCH;
        StoreGame(&g, F_BASE);
    }
    Apply(t, &g);
}

/* Player finished dragging/resizing the game window. */
static void OnMoveSizeEnd(HWND h)
{
    Touched *t = FindTouched(h);
    Game g;
    if (!t || !GetGame(t->path, &g) || g.dispMode != DM_WINDOW || g.baseH <= 0) return;

    DPI_AWARENESS_CONTEXT old = EnterGameDpi(h);
    RECT cur;
    GetWindowRect(h, &cur);
    int cw, ch;
    ClientSize(h, &cw, &ch);

    g.winX = cur.left;
    g.winY = cur.top;
    g.hasPos = TRUE;
    UINT what = F_POS;

    if (!(Near(cw, t->appCW) && Near(ch, t->appCH))) {
        int num, den;
        WinMode_AspectOf(&g, &num, &den);
        int pw = t->appCW > 0 ? t->appCW : 1, ph = t->appCH > 0 ? t->appCH : 1;
        int dw = MulDiv(Iabs(cw - pw), 1000, pw), dh = MulDiv(Iabs(ch - ph), 1000, ph);
        int H = (dw >= dh) ? MulDiv(cw, den, num) : ch;   /* follow the edge the player dragged most */
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        g.winScale = ClampScale(h, &g, MulDiv(H, 1000, g.baseH), WindowStyle(t->style), t->ex, &mi.rcWork);
        what |= F_SCALE;
    }
    SetThreadDpiAwarenessContext(old);

    StoreGame(&g, what);
    t->gaveUp = FALSE;
    Apply(t, &g);
}

static void OnHotkey(int id)
{
    Touched *t;
    Game g;
    if (!s_cur) return;
    WCHAR path[MAX_PATH];
    if (FindGameForPid(s_curPid, path) < 0 || !GetGame(path, &g)) return;

    if (id == HK_TOGGLE) {
        g.dispMode = (g.dispMode == DM_BORDERLESS) ? DM_WINDOW : DM_BORDERLESS;
        StoreGame(&g, F_MODE);
        t = Touch(s_cur, path, &g);
        t->gaveUp = FALSE;
        Apply(t, &g);
        return;
    }
    if (g.dispMode != DM_WINDOW || g.baseH <= 0) return;
    t = Touch(s_cur, path, &g);
    int s = g.winScale > 0 ? g.winScale : 1000;
    g.winScale = StepScale(s, id == HK_UP ? 1 : -1, g.scaling == SC_INTEGER);
    /* keep the window centred while it grows/shrinks */
    g.hasPos = FALSE;
    StoreGame(&g, F_SCALE | F_POS);
    t->gaveUp = FALSE;
    Apply(t, &g);
    /* remember the new position too */
    Game g2;
    if (GetGame(path, &g2)) {
        g2.winX = t->applied.left;
        g2.winY = t->applied.top;
        g2.hasPos = TRUE;
        StoreGame(&g2, F_POS);
    }
}

/* Periodic check: game moved/resized itself, window died, bars z-order. */
static void OnTimer(void)
{
    if (s_zoomOn) {
        if (GetForegroundWindow() != s_zoomWnd) ZoomOff();
        else ZoomUpdate();
    }
    if (!s_cur) return;
    if (!IsWindow(s_cur)) { Touched *t = FindTouched(s_cur); if (t) RemoveTouched(t); Deactivate(); return; }
    Touched *t = FindTouched(s_cur);
    if (!t || t->gaveUp || s_drag || IsIconic(s_cur)) return;
    if (GetTickCount() - t->appliedTick < 700) return;

    Game g;
    if (!GetGame(t->path, &g) || g.dispMode == DM_NONE) return;

    DPI_AWARENESS_CONTEXT old = EnterGameDpi(s_cur);
    RECT cur;
    GetWindowRect(s_cur, &cur);
    int cw, ch;
    ClientSize(s_cur, &cw, &ch);
    SetThreadDpiAwarenessContext(old);

    if (!EqualRect(&cur, &t->applied)) {
        if (Near(cw, t->appCW) && Near(ch, t->appCH)) {
            /* only moved */
            if (g.dispMode == DM_WINDOW) {
                t->applied = cur;
                g.winX = cur.left; g.winY = cur.top; g.hasPos = TRUE;
                StoreGame(&g, F_POS);
                return;
            }
        } else if (cw >= 64 && ch >= 48) {
            /* the game changed its own size (e.g. new resolution in its options) */
            DWORD now = GetTickCount();
            if (now - t->fixTick > 10000) { t->fixes = 0; t->fixTick = now; }
            if (++t->fixes > 4) { t->gaveUp = TRUE; return; }   /* it keeps fighting us */
            g.baseW = cw;
            g.baseH = ch;
            StoreGame(&g, F_BASE);
        }
        Apply(t, &g);
        return;
    }

    /* keep the black bars directly behind the game (above the taskbar) */
    if (s_backdrop && IsWindowVisible(s_backdrop) && GetForegroundWindow() == s_cur) {
        HWND below = GetWindow(s_cur, GW_HWNDNEXT);
        while (below && below != s_backdrop && !IsWindowVisible(below)) below = GetWindow(below, GW_HWNDNEXT);
        if (below != s_backdrop) ShowBackdropBehind(s_cur);
    }
}

#define WM_WM_EVENT (WM_APP + 31)

/* Window events can arrive while this thread waits inside a cross-process
 * call (re-entrancy). So the callback only queues; the loop does the work. */
static void CALLBACK WinEvent(HWINEVENTHOOK hk, DWORD ev, HWND hwnd, LONG obj, LONG child, DWORD tid, DWORD time)
{
    (void)hk; (void)child; (void)tid; (void)time;
    if (obj != OBJID_WINDOW) return;
    PostThreadMessageW(s_tid, WM_WM_EVENT, ev, (LPARAM)hwnd);
}

static void HandleEvent(DWORD ev, HWND hwnd)
{
    switch (ev) {
    case EVENT_SYSTEM_MOVESIZESTART:
        if (hwnd && hwnd == s_cur) s_drag = TRUE;
        break;
    case EVENT_SYSTEM_MOVESIZEEND:
        if (hwnd && hwnd == s_cur) { s_drag = FALSE; OnMoveSizeEnd(hwnd); }
        break;
    default:
        Evaluate(FALSE);
        break;
    }
}

static DWORD WINAPI WinModeThread(LPVOID param)
{
    MSG m;
    s_tid = GetCurrentThreadId();
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(CoCreateInstance(&kCLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &kIID_ITaskbarList2, (void **)&s_tb)) && s_tb)
        s_tb->lpVtbl->HrInit(s_tb);
    CreateBackdrop();
    MagLoad();
    s_zoomFilter = -1;

    HWINEVENTHOOK e1 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK e2 = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, NULL, WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK e3 = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL, WinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    UINT_PTR timer = SetTimer(NULL, 0, 100, NULL);
    SetEvent((HANDLE)param);

    Evaluate(TRUE);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.hwnd == NULL) {
            if (m.message == WM_WM_RELOAD) { Evaluate(TRUE); continue; }
            if (m.message == WM_WM_EVENT)  { HandleEvent((DWORD)m.wParam, (HWND)m.lParam); continue; }
            if (m.message == WM_HOTKEY)    { OnHotkey((int)m.wParam); continue; }
            if (m.message == WM_TIMER)     { OnTimer(); continue; }
        }
        DispatchMessageW(&m);
    }

    KillTimer(NULL, timer);
    if (e1) UnhookWinEvent(e1);
    if (e2) UnhookWinEvent(e2);
    if (e3) UnhookWinEvent(e3);
    SetHotkeys(FALSE);
    ZoomOff();
    if (s_magReady && pMagUninitialize) pMagUninitialize();
    while (s_nt > 0) Restore(&s_t[s_nt - 1]);   /* put every game window back */
    if (s_backdrop) DestroyWindow(s_backdrop);
    if (s_tb) s_tb->lpVtbl->Release(s_tb);
    CoUninitialize();
    return 0;
}

void WinMode_Start(void)
{
    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_thread = CreateThread(NULL, 0, WinModeThread, ready, 0, &s_tid);
    if (s_thread) WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
}

void WinMode_Stop(void)
{
    if (!s_thread) return;
    PostThreadMessageW(s_tid, WM_QUIT, 0, 0);
    WaitForSingleObject(s_thread, 3000);
    CloseHandle(s_thread);
    s_thread = NULL;
}

void WinMode_Reload(void)
{
    if (s_tid) PostThreadMessageW(s_tid, WM_WM_RELOAD, 0, 0);
}

/* ---- Windows "high DPI scaling override" for the game exe ------------ */
static BOOL IsDpiToken(const WCHAR *t, int n)
{
    static const WCHAR *names[] = { L"HIGHDPIAWARE", L"DPIUNAWARE", L"GDIDPISCALING" };
    for (int i = 0; i < 3; ++i) {
        int l = lstrlenW(names[i]);
        if (l == n && CompareStringOrdinal(t, n, names[i], l, TRUE) == CSTR_EQUAL) return TRUE;
    }
    return FALSE;
}

void WinMode_SetDpiFix(const WCHAR *exePath, BOOL on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                        0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;

    WCHAR val[1024], out[1100];
    DWORD type = 0, sz = sizeof val - sizeof(WCHAR);
    memset(val, 0, sizeof val);
    if (RegQueryValueExW(k, exePath, NULL, &type, (BYTE *)val, &sz) != ERROR_SUCCESS || type != REG_SZ) val[0] = 0;

    int ol = 0, tokens = 0;
    out[0] = 0;
    const WCHAR *p = val;
    while (*p) {
        while (*p == L' ') ++p;
        const WCHAR *s = p;
        while (*p && *p != L' ') ++p;
        int n = (int)(p - s);
        if (n == 0) break;
        if ((n == 1 && s[0] == L'~') || IsDpiToken(s, n)) continue;
        if (ol + n + 2 >= 1000) break;
        out[ol++] = L' ';
        for (int i = 0; i < n; ++i) out[ol++] = s[i];
        out[ol] = 0;
        tokens++;
    }
    if (on) {
        lstrcpyW(out + ol, L" HIGHDPIAWARE");
        ol += 13;
        tokens++;
    }

    if (tokens == 0) {
        RegDeleteValueW(k, exePath);
    } else {
        WCHAR final[1104];
        lstrcpyW(final, L"~");
        lstrcatW(final, out);
        RegSetValueExW(k, exePath, 0, REG_SZ, (const BYTE *)final, (DWORD)(lstrlenW(final) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(k);
}
