/*
 * Key Remapper - per-game keyboard remapping for Windows.
 *
 * Design for low latency:
 *  - A dedicated time-critical input thread owns the low-level keyboard hook.
 *  - The hook is only installed while a library game is in the foreground, so
 *    nothing touches system-wide input the rest of the time.
 *  - The hot path is a single 512-entry table lookup (physical scancode +
 *    extended bit -> target) followed by one SendInput call. No allocation,
 *    no locks, no string work.
 *  - Foreground changes are event driven (SetWinEventHook), with a cheap
 *    250 ms safety re-check.
 *  - Windows power throttling (EcoQoS) is disabled for the process and the
 *    input thread, so the background app is never slowed down while the
 *    game has focus.
 *
 * Built without the C runtime: only kernel32/user32/... are used.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include "resource.h"
#include "theme.h"
#include "app.h"

/* ------------------------------------------------------------------ */
/* Minimal runtime                                                     */
/* ------------------------------------------------------------------ */
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp == sp || n == 0) return d;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}

/* ------------------------------------------------------------------ */
/* Data                                                                */
/* ------------------------------------------------------------------ */
/* Key ids: low byte = scancode, 0x100 = extended (E0) key.
 * Special targets: mouse buttons and "disabled". */
#define KEY_DISABLED   0xFFFF
#define MOUSE_BASE     0x1000
#define MOUSE_LEFT     0x1001
#define MOUSE_RIGHT    0x1002
#define MOUSE_MIDDLE   0x1003
#define MOUSE_X1       0x1004
#define MOUSE_X2       0x1005

#define INJECT_MAGIC   ((ULONG_PTR)0x4B52454DUL) /* tags our own SendInput events */

Game                     g_games[MAX_GAMES];
int                      g_ngames;
CRITICAL_SECTION         g_lock;          /* guards g_games between UI and worker threads */
static volatile LONG     g_paused;
static BOOL              g_autostart;
static WCHAR             g_iniPath[MAX_PATH];
static WCHAR             g_exePath[MAX_PATH];

HINSTANCE        g_hInst;
HWND             g_hMain;
static HANDLE    g_hHookThread;
static DWORD     g_hookTid;
static int       g_sel = -1;
static UINT      g_msgTaskbarCreated, g_msgShow;
static HIMAGELIST g_imgGames;
static BOOL      g_balloonShown;

/* Themed painting: each window paints its background once into a cached
 * bitmap; owner-painted controls copy their backdrop from it. */
typedef struct { HDC dc; HBITMAP bmp, old; int w, h; BOOL dirty; } Surface;
static Surface   g_surfMain, g_surfMap;
static HWND      g_hMapDlg;
static WCHAR     g_statusText[160] = L"Watching for games";
static int       g_statusKind = TH_STATUS_WATCHING;

/* Messages */
#define WM_HT_RELOAD   (WM_APP + 1)  /* thread msg: config changed, re-evaluate        */
#define WM_HT_CAPTURE  (WM_APP + 2)  /* thread msg: wParam = dialog hwnd (0 = cancel)  */
#define WM_HT_CAPDONE  (WM_APP + 3)  /* thread msg: capture finished                   */
#define WM_UI_STATUS   (WM_APP + 10) /* wParam = active game index or -1               */
#define WM_UI_TRAY     (WM_APP + 11)
#define WM_CAP_KEY     (WM_APP + 12) /* wParam = captured key id                       */

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
const WCHAR *FileNamePart(const WCHAR *p)
{
    const WCHAR *r = p;
    for (; *p; ++p) if (*p == L'\\' || *p == L'/') r = p + 1;
    return r;
}

BOOL SameText(const WCHAR *a, const WCHAR *b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

static int HexVal(WCHAR c)
{
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Key naming / VK translation                                         */
/* ------------------------------------------------------------------ */
static WORD VkFromId(WORD id)
{
    WORD s = id & 0xFF;
    BOOL e = (id & 0x100) != 0;
    if (!e) {
        static const BYTE np[13] = { VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_SUBTRACT,
                                     VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, VK_ADD,
                                     VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL };
        if (s >= 0x47 && s <= 0x53) return np[s - 0x47];
        if (s == 0x37) return VK_MULTIPLY;
        if (s == 0x45) return VK_PAUSE;
    } else {
        if (s == 0x45) return VK_NUMLOCK;
        if (s == 0x1C) return VK_RETURN;
        if (s == 0x35) return VK_DIVIDE;
        if (s == 0x37) return VK_SNAPSHOT;
    }
    return (WORD)MapVirtualKeyW(e ? (0xE000u | s) : s, MAPVK_VSC_TO_VK_EX);
}

static void KeyName(WORD id, WCHAR *buf, int cch)
{
    switch (id) {
    case KEY_DISABLED: lstrcpynW(buf, L"(Disabled - block this key)", cch); return;
    case MOUSE_LEFT:   lstrcpynW(buf, L"Mouse Left Button", cch); return;
    case MOUSE_RIGHT:  lstrcpynW(buf, L"Mouse Right Button", cch); return;
    case MOUSE_MIDDLE: lstrcpynW(buf, L"Mouse Middle Button", cch); return;
    case MOUSE_X1:     lstrcpynW(buf, L"Mouse Button 4", cch); return;
    case MOUSE_X2:     lstrcpynW(buf, L"Mouse Button 5", cch); return;
    case 0x045:        lstrcpynW(buf, L"Pause", cch); return;
    case 0x145:        lstrcpynW(buf, L"Num Lock", cch); return;
    }
    LONG lp = (LONG)(((DWORD)(id & 0xFF)) << 16) | ((id & 0x100) ? (1L << 24) : 0);
    if (GetKeyNameTextW(lp, buf, cch) <= 0)
        wsprintfW(buf, L"Key 0x%03X", id);
}

/* ------------------------------------------------------------------ */
/* Input thread (hot path)                                             */
/* ------------------------------------------------------------------ */
static WORD   g_table[512];   /* src id -> target (0 = pass through)        */
static WORD   g_held[512];    /* src id -> target currently held down       */
static WORD   g_vk[512];      /* key id -> virtual key for SendInput         */
static BOOL   g_scanOnly;
static HHOOK  g_kbHook;
static int    g_active = -1;
static HWND   g_capHwnd;
static WORD   g_capSwallowUp;

static void SendTarget(WORD d, BOOL up)
{
    INPUT in;
    memset(&in, 0, sizeof in);
    if (d >= MOUSE_BASE && d <= MOUSE_X2) {
        in.type = INPUT_MOUSE;
        in.mi.dwExtraInfo = INJECT_MAGIC;
        switch (d) {
        case MOUSE_LEFT:   in.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN; break;
        case MOUSE_RIGHT:  in.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN; break;
        case MOUSE_MIDDLE: in.mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN; break;
        case MOUSE_X1:     in.mi.dwFlags = up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN; in.mi.mouseData = XBUTTON1; break;
        case MOUSE_X2:     in.mi.dwFlags = up ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN; in.mi.mouseData = XBUTTON2; break;
        }
    } else {
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = d & 0xFF;
        in.ki.dwFlags = ((d & 0x100) ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
        if (g_scanOnly) in.ki.dwFlags |= KEYEVENTF_SCANCODE;
        else            in.ki.wVk = g_vk[d & 0x1FF];
        in.ki.dwExtraInfo = INJECT_MAGIC;
    }
    SendInput(1, &in, sizeof in);
}

static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode == HC_ACTION) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        if (k->dwExtraInfo != INJECT_MAGIC && k->scanCode != 0) {
            WORD id = (WORD)((k->scanCode & 0xFF) | ((k->flags & LLKHF_EXTENDED) ? 0x100 : 0));
            BOOL up = (k->flags & LLKHF_UP) != 0;

            /* Key-detection mode for the mapping dialog */
            if (g_capSwallowUp) {
                if (up && id == g_capSwallowUp) {
                    g_capSwallowUp = 0;
                    PostThreadMessageW(g_hookTid, WM_HT_CAPDONE, 0, 0);
                }
                return 1;
            }
            if (g_capHwnd) {
                if (!up && !(k->flags & LLKHF_INJECTED)) {
                    PostMessageW(g_capHwnd, WM_CAP_KEY, id, 0);
                    g_capHwnd = NULL;
                    g_capSwallowUp = id;
                }
                return 1;
            }

            if (!(k->flags & LLKHF_INJECTED)) {
                if (up) {
                    WORD d = g_held[id];
                    if (d) {
                        g_held[id] = 0;
                        if (d != KEY_DISABLED) SendTarget(d, TRUE);
                        return 1;
                    }
                } else {
                    WORD d = g_table[id];
                    if (d) {
                        /* Mouse buttons don't auto-repeat; swallow key repeats. */
                        if (g_held[id] && (d == KEY_DISABLED || d >= MOUSE_BASE)) return 1;
                        g_held[id] = d;
                        if (d != KEY_DISABLED) SendTarget(d, FALSE);
                        return 1;
                    }
                }
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wp, lp);
}

static void ReleaseAllHeld(void)
{
    for (int i = 0; i < 512; ++i) {
        WORD d = g_held[i];
        if (d) {
            g_held[i] = 0;
            if (d != KEY_DISABLED) SendTarget(d, TRUE);
        }
    }
}

static void UpdateHookInstall(void)
{
    BOOL need = (g_active >= 0) || g_capHwnd || g_capSwallowUp;
    if (need && !g_kbHook) {
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_hInst, 0);
    } else if (!need && g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = NULL;
    }
}

int FindGameForPid(DWORD pid, WCHAR *outPath)
{
    WCHAR path[1024];
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return -1;
    DWORD n = 1024;
    BOOL ok = QueryFullProcessImageNameW(hp, 0, path, &n);
    CloseHandle(hp);
    if (!ok) return -1;

    int idx = -1, byName = -1;
    const WCHAR *fname = FileNamePart(path);
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ngames; ++i) {
        const Game *g = &g_games[i];
        if (SameText(path, g->path)) { idx = i; break; }
        if (byName < 0 && SameText(fname, FileNamePart(g->path))) byName = i;
    }
    if (idx < 0) idx = byName;
    if (idx >= 0 && outPath) lstrcpynW(outPath, g_games[idx].path, MAX_PATH);
    LeaveCriticalSection(&g_lock);
    return idx;
}

/* Library game for `pid` that has remapping switched on, or -1. */
static int MatchProcess(DWORD pid)
{
    int idx = FindGameForPid(pid, NULL);
    if (idx < 0) return -1;
    EnterCriticalSection(&g_lock);
    BOOL on = idx < g_ngames && g_games[idx].enabled && g_games[idx].nmaps > 0;
    LeaveCriticalSection(&g_lock);
    return on ? idx : -1;
}

static void Activate(int idx)
{
    ReleaseAllHeld();
    memset(g_table, 0, sizeof g_table);
    g_scanOnly = FALSE;
    if (idx >= 0) {
        EnterCriticalSection(&g_lock);
        if (idx < g_ngames) {
            const Game *g = &g_games[idx];
            for (int i = 0; i < g->nmaps; ++i) g_table[g->maps[i].src & 0x1FF] = g->maps[i].dst;
            g_scanOnly = g->scanOnly;
        } else {
            idx = -1;
        }
        LeaveCriticalSection(&g_lock);
    }
    int prev = g_active;
    g_active = idx;
    UpdateHookInstall();
    (void)prev;
    PostMessageW(g_hMain, WM_UI_STATUS, (WPARAM)idx, 0);
}

static void EvaluateForeground(BOOL force)
{
    static HWND  lastHwnd;
    static DWORD lastPid;
    static int   lastPidIdx = -1;

    HWND h = GetForegroundWindow();
    if (!force && h == lastHwnd) return;
    lastHwnd = h;

    int idx = -1;
    if (h && !g_paused) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid && pid != GetCurrentProcessId()) {
            if (!force && pid == lastPid) {
                idx = lastPidIdx;
            } else {
                idx = MatchProcess(pid);
                lastPid = pid;
                lastPidIdx = idx;
            }
        }
    }
    if (force || idx != g_active) Activate(idx);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK h, DWORD ev, HWND hwnd, LONG obj, LONG child,
                                  DWORD tid, DWORD time)
{
    (void)h; (void)ev; (void)hwnd; (void)obj; (void)child; (void)tid; (void)time;
    EvaluateForeground(FALSE);
}

static void DisableThreadThrottling(void)
{
    THREAD_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;
    SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &st, sizeof st);
}

static DWORD WINAPI InputThread(LPVOID param)
{
    MSG m;
    PeekMessageW(&m, NULL, WM_USER, WM_USER, PM_NOREMOVE); /* create the queue */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    DisableThreadThrottling();

    for (int i = 0; i < 512; ++i) g_vk[i] = VkFromId((WORD)i);

    HWINEVENTHOOK e1 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                                       WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK e2 = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, NULL,
                                       WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    UINT_PTR timer = SetTimer(NULL, 0, 250, NULL);
    SetEvent((HANDLE)param);

    EvaluateForeground(TRUE);

    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.hwnd == NULL) {
            switch (m.message) {
            case WM_HT_RELOAD:
                EvaluateForeground(TRUE);
                continue;
            case WM_HT_CAPTURE:
                g_capHwnd = (HWND)m.wParam;
                if (!g_capHwnd) g_capSwallowUp = 0;
                UpdateHookInstall();
                continue;
            case WM_HT_CAPDONE:
                UpdateHookInstall();
                continue;
            case WM_TIMER:
                EvaluateForeground(FALSE);
                continue;
            }
        }
        DispatchMessageW(&m);
    }

    KillTimer(NULL, timer);
    ReleaseAllHeld();
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (e1) UnhookWinEvent(e1);
    if (e2) UnhookWinEvent(e2);
    return 0;
}

static void PostReload(void) { PostThreadMessageW(g_hookTid, WM_HT_RELOAD, 0, 0); }

/* ------------------------------------------------------------------ */
/* Config (INI next to the exe)                                        */
/* ------------------------------------------------------------------ */
static void LoadConfig(void)
{
    static WCHAR buf[MAX_MAPS * 12 + 16];
    WCHAR sec[32];
    g_ngames = 0;
    int n = (int)GetPrivateProfileIntW(L"General", L"Games", 0, g_iniPath);
    g_autostart = GetPrivateProfileIntW(L"General", L"Autostart", 0, g_iniPath) != 0;
    if (n > MAX_GAMES) n = MAX_GAMES;
    for (int i = 0; i < n; ++i) {
        Game *g = &g_games[g_ngames];
        memset(g, 0, sizeof *g);
        wsprintfW(sec, L"Game%d", i);
        GetPrivateProfileStringW(sec, L"Path", L"", g->path, MAX_PATH, g_iniPath);
        if (!g->path[0]) continue;
        GetPrivateProfileStringW(sec, L"Name", FileNamePart(g->path), g->name, 128, g_iniPath);
        g->enabled  = GetPrivateProfileIntW(sec, L"Enabled", 1, g_iniPath) != 0;
        g->scanOnly = GetPrivateProfileIntW(sec, L"ScanOnly", 0, g_iniPath) != 0;
        g->dispMode = (int)GetPrivateProfileIntW(sec, L"DisplayMode", DM_NONE, g_iniPath);
        g->aspect   = (int)GetPrivateProfileIntW(sec, L"Aspect", AR_AUTO, g_iniPath);
        g->scaling  = (int)GetPrivateProfileIntW(sec, L"Scaling", SC_FIT, g_iniPath);
        g->fsMethod = GetPrivateProfileIntW(sec, L"FsMethod", FS_ZOOM, g_iniPath) == FS_RESIZE ? FS_RESIZE : FS_ZOOM;
        g->filter   = GetPrivateProfileIntW(sec, L"Filter", FL_SMOOTH, g_iniPath) == FL_SHARP ? FL_SHARP : FL_SMOOTH;
        g->dpiFix   = GetPrivateProfileIntW(sec, L"DpiFix", 0, g_iniPath) != 0;
        g->dpiFixSet = GetPrivateProfileIntW(sec, L"DpiFixSet", 0, g_iniPath) != 0;
        g->baseW    = (int)GetPrivateProfileIntW(sec, L"BaseW", 0, g_iniPath);
        g->baseH    = (int)GetPrivateProfileIntW(sec, L"BaseH", 0, g_iniPath);
        g->winScale = (int)GetPrivateProfileIntW(sec, L"WinScale", 0, g_iniPath);
        g->hasPos   = GetPrivateProfileIntW(sec, L"HasPos", 0, g_iniPath) != 0;
        g->winX     = (int)GetPrivateProfileIntW(sec, L"WinX", 0, g_iniPath);
        g->winY     = (int)GetPrivateProfileIntW(sec, L"WinY", 0, g_iniPath);
        if (g->dispMode < DM_NONE || g->dispMode > DM_WINDOW) g->dispMode = DM_NONE;
        if (g->aspect < AR_AUTO || g->aspect > AR_5_4) g->aspect = AR_AUTO;
        if (g->scaling != SC_INTEGER) g->scaling = SC_FIT;
        if (g->baseW < 64 || g->baseH < 48) g->baseW = g->baseH = 0;
        GetPrivateProfileStringW(sec, L"Maps", L"", buf, sizeof buf / sizeof buf[0], g_iniPath);
        /* format: SSS:DDDD,SSS:DDDD (hex) */
        const WCHAR *p = buf;
        while (*p && g->nmaps < MAX_MAPS) {
            DWORD a = 0, b = 0; int v;
            while (*p == L' ' || *p == L',') ++p;
            if (!*p) break;
            while ((v = HexVal(*p)) >= 0) { a = a * 16 + (DWORD)v; ++p; }
            if (*p != L':') { while (*p && *p != L',') ++p; continue; }
            ++p;
            while ((v = HexVal(*p)) >= 0) { b = b * 16 + (DWORD)v; ++p; }
            if (a && a < 0x200 && b) {
                g->maps[g->nmaps].src = (WORD)a;
                g->maps[g->nmaps].dst = (WORD)b;
                g->nmaps++;
            }
        }
        g_ngames++;
    }
}

static void WriteInt(const WCHAR *sec, const WCHAR *key, int v)
{
    WCHAR num[16];
    wsprintfW(num, L"%d", v);
    WritePrivateProfileStringW(sec, key, num, g_iniPath);
}

static void SaveConfig(void)
{
    static WCHAR buf[MAX_MAPS * 12 + 16];
    static Game snap[MAX_GAMES];
    WCHAR sec[32], num[16];
    int ngames;
    EnterCriticalSection(&g_lock);   /* worker threads may update display fields */
    ngames = g_ngames;
    memcpy(snap, g_games, sizeof(Game) * (size_t)ngames);
    LeaveCriticalSection(&g_lock);
    /* Start a fresh UTF-16 file so the profile APIs keep Unicode paths intact. */
    HANDLE f = CreateFileW(g_iniPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w; WCHAR bom = 0xFEFF;
        WriteFile(f, &bom, 2, &w, NULL);
        CloseHandle(f);
    }
    wsprintfW(num, L"%d", ngames);
    WritePrivateProfileStringW(L"General", L"Games", num, g_iniPath);
    WritePrivateProfileStringW(L"General", L"Autostart", g_autostart ? L"1" : L"0", g_iniPath);
    for (int i = 0; i < ngames; ++i) {
        const Game *g = &snap[i];
        wsprintfW(sec, L"Game%d", i);
        WritePrivateProfileStringW(sec, L"Path", g->path, g_iniPath);
        WritePrivateProfileStringW(sec, L"Name", g->name, g_iniPath);
        WritePrivateProfileStringW(sec, L"Enabled", g->enabled ? L"1" : L"0", g_iniPath);
        WritePrivateProfileStringW(sec, L"ScanOnly", g->scanOnly ? L"1" : L"0", g_iniPath);
        buf[0] = 0;
        int len = 0;
        for (int j = 0; j < g->nmaps; ++j)
            len += wsprintfW(buf + len, j ? L",%03X:%04X" : L"%03X:%04X", g->maps[j].src, g->maps[j].dst);
        WritePrivateProfileStringW(sec, L"Maps", buf, g_iniPath);
        WriteInt(sec, L"DisplayMode", g->dispMode);
        WriteInt(sec, L"Aspect", g->aspect);
        WriteInt(sec, L"Scaling", g->scaling);
        WriteInt(sec, L"FsMethod", g->fsMethod);
        WriteInt(sec, L"Filter", g->filter);
        WriteInt(sec, L"DpiFix", g->dpiFix);
        WriteInt(sec, L"DpiFixSet", g->dpiFixSet);
        WriteInt(sec, L"BaseW", g->baseW);
        WriteInt(sec, L"BaseH", g->baseH);
        WriteInt(sec, L"WinScale", g->winScale);
        WriteInt(sec, L"HasPos", g->hasPos);
        WriteInt(sec, L"WinX", g->winX);
        WriteInt(sec, L"WinY", g->winY);
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, g_iniPath); /* flush */
}

/* ------------------------------------------------------------------ */
/* Autostart via Task Scheduler (elevated apps can't use the Run key)  */
/* ------------------------------------------------------------------ */
static DWORD RunHidden(WCHAR *cmd)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi; DWORD code = 1;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return code;
}

static void AppendXmlEscaped(WCHAR *dst, const WCHAR *s)
{
    WCHAR *d = dst + lstrlenW(dst);
    for (; *s; ++s) {
        const WCHAR *rep = NULL;
        if (*s == L'&') rep = L"&amp;"; else if (*s == L'<') rep = L"&lt;"; else if (*s == L'>') rep = L"&gt;";
        if (rep) { lstrcpyW(d, rep); d += lstrlenW(rep); }
        else *d++ = *s;
    }
    *d = 0;
}

static BOOL SetAutostart(BOOL on)
{
    static WCHAR cmd[1024];
    if (!on) {
        lstrcpyW(cmd, L"schtasks.exe /Delete /F /TN \"KeyRemapper\"");
        RunHidden(cmd);
        return TRUE;
    }
    static WCHAR xml[4096], tmp[MAX_PATH], user[256], dom[256];
    GetTempPathW(MAX_PATH, tmp);
    lstrcatW(tmp, L"KeyRemapperTask.xml");
    user[0] = dom[0] = 0;
    GetEnvironmentVariableW(L"USERNAME", user, 256);
    GetEnvironmentVariableW(L"USERDOMAIN", dom, 256);

    lstrcpyW(xml,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Starts Key Remapper in the tray at logon.</Description></RegistrationInfo>\r\n"
        L"<Triggers><LogonTrigger><Enabled>true</Enabled><UserId>");
    if (dom[0]) { AppendXmlEscaped(xml, dom); lstrcatW(xml, L"\\"); }
    AppendXmlEscaped(xml, user);
    lstrcatW(xml,
        L"</UserId></LogonTrigger></Triggers>\r\n"
        L"<Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>false</StartWhenAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>4</Priority></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>");
    AppendXmlEscaped(xml, g_exePath);
    lstrcatW(xml, L"</Command><Arguments>/tray</Arguments></Exec></Actions>\r\n</Task>\r\n");

    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD w; WCHAR bom = 0xFEFF;
    WriteFile(f, &bom, 2, &w, NULL);
    WriteFile(f, xml, (DWORD)lstrlenW(xml) * 2, &w, NULL);
    CloseHandle(f);

    wsprintfW(cmd, L"schtasks.exe /Create /F /TN \"KeyRemapper\" /XML \"%s\"", tmp);
    DWORD rc = RunHidden(cmd);
    DeleteFileW(tmp);
    return rc == 0;
}

/* ------------------------------------------------------------------ */
/* Tray                                                                */
/* ------------------------------------------------------------------ */
static void TrayInit(NOTIFYICONDATAW *nid)
{
    memset(nid, 0, sizeof *nid);
    nid->cbSize = sizeof *nid;
    nid->hWnd = g_hMain;
    nid->uID = 1;
}

static void TrayAdd(void)
{
    NOTIFYICONDATAW nid; TrayInit(&nid);
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_UI_TRAY;
    nid.hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    lstrcpynW(nid.szTip, L"Key Remapper", 128);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TraySetTip(const WCHAR *tip)
{
    NOTIFYICONDATAW nid; TrayInit(&nid);
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, tip, 128);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayBalloon(const WCHAR *text)
{
    NOTIFYICONDATAW nid; TrayInit(&nid);
    nid.uFlags = NIF_INFO;
    lstrcpynW(nid.szInfoTitle, L"Key Remapper", 64);
    lstrcpynW(nid.szInfo, text, 256);
    nid.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayRemove(void)
{
    NOTIFYICONDATAW nid; TrayInit(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */
static void ShowMain(void)
{
    ShowWindow(g_hMain, SW_SHOW);
    if (IsIconic(g_hMain)) ShowWindow(g_hMain, SW_RESTORE);
    SetForegroundWindow(g_hMain);
}

static void HideMain(void)
{
    ShowWindow(g_hMain, SW_HIDE);
    if (!g_balloonShown) {
        g_balloonShown = TRUE;
        TrayBalloon(L"Still running in the tray. Your games will be remapped automatically.");
    }
}

static void UpdatePauseUi(void)
{
    SetDlgItemTextW(g_hMain, IDC_PAUSE, g_paused ? L"Resume" : L"Pause");
}

static void InvalidateHeader(void);

static void SetStatus(int idx)
{
    if (g_paused) {
        lstrcpyW(g_statusText, L"Paused  \x2022  Ctrl+Shift+F12");
        g_statusKind = TH_STATUS_PAUSED;
    } else if (idx >= 0 && idx < g_ngames) {
        wsprintfW(g_statusText, L"Active: %s", g_games[idx].name);
        g_statusKind = TH_STATUS_ACTIVE;
    } else {
        lstrcpyW(g_statusText, L"Watching for games");
        g_statusKind = TH_STATUS_WATCHING;
    }
    InvalidateHeader();

    WCHAR tip[128];
    if (g_paused) lstrcpyW(tip, L"Key Remapper - paused");
    else if (idx >= 0 && idx < g_ngames) { lstrcpyW(tip, L"Key Remapper - active: "); lstrcatW(tip, g_games[idx].name); tip[127] = 0; }
    else lstrcpyW(tip, L"Key Remapper - watching");
    TraySetTip(tip);
}

static void RefreshDisplay(void);
static void Commit(void);

static void RefreshMaps(void)
{
    HWND lv = GetDlgItem(g_hMain, IDC_MAPS);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    BOOL has = g_sel >= 0 && g_sel < g_ngames;
    if (has) {
        const Game *g = &g_games[g_sel];
        WCHAR name[128];
        for (int i = 0; i < g->nmaps; ++i) {
            LVITEMW it; memset(&it, 0, sizeof it);
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = i;
            KeyName(g->maps[i].src, name, 128);
            it.pszText = name;
            it.lParam = i;
            SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
            KeyName(g->maps[i].dst, name, 128);
            it.mask = LVIF_TEXT; it.iSubItem = 1; it.pszText = name;
            SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        }
        SetDlgItemTextW(g_hMain, IDC_GAMEPATH, g->path);
        CheckDlgButton(g_hMain, IDC_ENABLED, g->enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hMain, IDC_SCANONLY, g->scanOnly ? BST_CHECKED : BST_UNCHECKED);
    } else {
        SetDlgItemTextW(g_hMain, IDC_GAMEPATH, L"Select or add a game on the left.");
        CheckDlgButton(g_hMain, IDC_ENABLED, BST_UNCHECKED);
        CheckDlgButton(g_hMain, IDC_SCANONLY, BST_UNCHECKED);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    RefreshDisplay();
    static const int ids[] = { IDC_ENABLED, IDC_MAPS, IDC_ADDMAP, IDC_EDITMAP, IDC_REMMAP, IDC_SCANONLY, IDC_REMGAME, IDC_COPYFROM,
                               IDC_DISPMODE, IDC_DPIFIX, IDC_RESETSIZE };
    for (int i = 0; i < (int)(sizeof ids / sizeof ids[0]); ++i) EnableWindow(GetDlgItem(g_hMain, ids[i]), has);
}

static void RefreshDisplay(void)
{
    static WCHAR info[256];
    BOOL has = g_sel >= 0 && g_sel < g_ngames;
    Game g;
    memset(&g, 0, sizeof g);
    if (has) {
        EnterCriticalSection(&g_lock);
        memcpy(&g, &g_games[g_sel], sizeof g);
        LeaveCriticalSection(&g_lock);
    }
    SendDlgItemMessageW(g_hMain, IDC_DISPMODE, CB_SETCURSEL, has ? g.dispMode : -1, 0);
    SendDlgItemMessageW(g_hMain, IDC_ASPECT, CB_SETCURSEL, has ? g.aspect : -1, 0);
    SendDlgItemMessageW(g_hMain, IDC_SCALING, CB_SETCURSEL, has ? g.scaling : -1, 0);
    SendDlgItemMessageW(g_hMain, IDC_METHOD, CB_SETCURSEL, has ? g.fsMethod : -1, 0);
    SendDlgItemMessageW(g_hMain, IDC_FILTER, CB_SETCURSEL, has ? g.filter : -1, 0);
    {
        /* only offer what applies to the chosen mode */
        BOOL bl = has && g.dispMode == DM_BORDERLESS;
        BOOL zoom = bl && g.fsMethod == FS_ZOOM;
        EnableWindow(GetDlgItem(g_hMain, IDC_METHOD), bl);
        EnableWindow(GetDlgItem(g_hMain, IDC_FILTER), zoom);
        EnableWindow(GetDlgItem(g_hMain, IDC_ASPECT), has && g.dispMode != DM_NONE && !zoom);
        EnableWindow(GetDlgItem(g_hMain, IDC_SCALING), has && g.dispMode != DM_NONE);
    }
    CheckDlgButton(g_hMain, IDC_DPIFIX, has && g.dpiFix ? BST_CHECKED : BST_UNCHECKED);

    if (!has) {
        info[0] = 0;
    } else {
        int len;
        if (g.baseW > 0) len = wsprintfW(info, L"Original size %d\x00D7%d", g.baseW, g.baseH);
        else len = wsprintfW(info, L"Original size: found when the game runs");
        if (g.dispMode == DM_WINDOW && g.winScale > 0)
            len += wsprintfW(info + len, L"  \x2022  window %d.%02d\x00D7", g.winScale / 1000, (g.winScale % 1000) / 10);
        wsprintfW(info + len, L"\nCtrl+Alt+Enter: fullscreen \x2194 window  \x2022  Ctrl+Alt+PgUp/PgDn: size");
    }
    SetDlgItemTextW(g_hMain, IDC_DISPINFO, info);
}

static void InitDisplayCombos(void)
{
    static const WCHAR *modes[] = { L"Leave as is", L"Borderless fullscreen", L"Resizable window (keeps aspect)" };
    static const WCHAR *aspects[] = { L"Game's own", L"4:3", L"16:9", L"16:10", L"5:4" };
    static const WCHAR *scal[] = { L"Best fit", L"Whole multiples" };
    static const WCHAR *meth[] = { L"Zoom the picture (any game, no lag)", L"Resize the window (game redraws)" };
    static const WCHAR *filt[] = { L"Smooth", L"Sharp pixels" };
    for (int i = 0; i < 2; ++i) SendDlgItemMessageW(g_hMain, IDC_METHOD, CB_ADDSTRING, 0, (LPARAM)meth[i]);
    for (int i = 0; i < 2; ++i) SendDlgItemMessageW(g_hMain, IDC_FILTER, CB_ADDSTRING, 0, (LPARAM)filt[i]);
    for (int i = 0; i < 3; ++i) SendDlgItemMessageW(g_hMain, IDC_DISPMODE, CB_ADDSTRING, 0, (LPARAM)modes[i]);
    for (int i = 0; i < 5; ++i) SendDlgItemMessageW(g_hMain, IDC_ASPECT, CB_ADDSTRING, 0, (LPARAM)aspects[i]);
    for (int i = 0; i < 2; ++i) SendDlgItemMessageW(g_hMain, IDC_SCALING, CB_ADDSTRING, 0, (LPARAM)scal[i]);
}

/* A display setting was changed in the UI. */
static void OnDisplayChanged(int ctl)
{
    if (g_sel < 0 || g_sel >= g_ngames) return;
    WCHAR path[MAX_PATH];
    BOOL setDpi = FALSE, dpiOn = FALSE;

    EnterCriticalSection(&g_lock);
    Game *g = &g_games[g_sel];
    lstrcpynW(path, g->path, MAX_PATH);
    switch (ctl) {
    case IDC_DISPMODE: {
        int m = (int)SendDlgItemMessageW(g_hMain, IDC_DISPMODE, CB_GETCURSEL, 0, 0);
        if (m < DM_NONE || m > DM_WINDOW) m = DM_NONE;
        /* first time a display mode is picked: also turn on sharp high-DPI output */
        if (m != DM_NONE && !g->dpiFixSet) { g->dpiFix = TRUE; g->dpiFixSet = TRUE; setDpi = TRUE; dpiOn = TRUE; }
        g->dispMode = m;
        break;
    }
    case IDC_ASPECT: {
        int a = (int)SendDlgItemMessageW(g_hMain, IDC_ASPECT, CB_GETCURSEL, 0, 0);
        g->aspect = (a >= AR_AUTO && a <= AR_5_4) ? a : AR_AUTO;
        break;
    }
    case IDC_METHOD:
        g->fsMethod = SendDlgItemMessageW(g_hMain, IDC_METHOD, CB_GETCURSEL, 0, 0) == FS_RESIZE ? FS_RESIZE : FS_ZOOM;
        break;
    case IDC_FILTER:
        g->filter = SendDlgItemMessageW(g_hMain, IDC_FILTER, CB_GETCURSEL, 0, 0) == FL_SHARP ? FL_SHARP : FL_SMOOTH;
        break;
    case IDC_SCALING:
        g->scaling = SendDlgItemMessageW(g_hMain, IDC_SCALING, CB_GETCURSEL, 0, 0) == SC_INTEGER ? SC_INTEGER : SC_FIT;
        break;
    case IDC_DPIFIX:
        g->dpiFix = IsDlgButtonChecked(g_hMain, IDC_DPIFIX) == BST_CHECKED;
        g->dpiFixSet = TRUE;
        setDpi = TRUE; dpiOn = g->dpiFix;
        break;
    case IDC_RESETSIZE:
        g->baseW = g->baseH = 0;
        g->winScale = 0;
        g->hasPos = FALSE;
        break;
    }
    LeaveCriticalSection(&g_lock);

    if (setDpi) WinMode_SetDpiFix(path, dpiOn);
    Commit();
    RefreshDisplay();
}

static void RefreshGames(void)
{
    HWND lv = GetDlgItem(g_hMain, IDC_GAMES);
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
    ImageList_RemoveAll(g_imgGames);
    for (int i = 0; i < g_ngames; ++i) {
        SHFILEINFOW sfi; memset(&sfi, 0, sizeof sfi);
        int img = -1;
        if (SHGetFileInfoW(g_games[i].path, 0, &sfi, sizeof sfi, SHGFI_ICON | SHGFI_SMALLICON) && sfi.hIcon) {
            img = ImageList_AddIcon(g_imgGames, sfi.hIcon);
            DestroyIcon(sfi.hIcon);
        }
        LVITEMW it; memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem = i;
        it.pszText = g_games[i].name;
        it.iImage = img;
        it.lParam = i;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    }
    SendMessageW(lv, LVM_SETCOLUMNWIDTH, 0, LVSCW_AUTOSIZE_USEHEADER);
    if (g_sel >= g_ngames) g_sel = g_ngames - 1;
    if (g_sel >= 0) {
        LVITEMW it; memset(&it, 0, sizeof it);
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(lv, LVM_SETITEMSTATE, g_sel, (LPARAM)&it);
        SendMessageW(lv, LVM_ENSUREVISIBLE, g_sel, FALSE);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    RefreshMaps();
}

static void Commit(void)
{
    SaveConfig();
    PostReload();
    WinMode_Reload();
}

/* ---- Themed painting ---- */
static RECT DluRect(HWND dlg, int x, int y, int w, int h)
{
    RECT r = { x, y, x + w, y + h };
    MapDialogRect(dlg, &r);
    return r;
}

static Surface *SurfaceFor(HWND dlg)
{
    if (dlg && dlg == g_hMain) return &g_surfMain;
    if (dlg && dlg == g_hMapDlg) return &g_surfMap;
    return NULL;
}

static void MainHeaderRect(RECT *r)
{
    GetClientRect(g_hMain, r);
    r->bottom = DluRect(g_hMain, 0, 0, 1, 38).bottom;
}

static void PaintSurface(HWND dlg, HDC dc, int w, int h)
{
    if (dlg == g_hMain) {
        ThMain m;
        memset(&m, 0, sizeof m);
        MainHeaderRect(&m.header);
        m.cards[0] = DluRect(dlg, 7, 46, 150, 288);
        m.cards[1] = DluRect(dlg, 164, 46, 249, 154);
        m.cards[2] = DluRect(dlg, 164, 208, 249, 126);
        m.cardTitle[0] = L"Game library";
        m.cardTitle[1] = L"Key bindings";
        m.cardTitle[2] = L"Display";
        m.ncards = 3;
        m.cardHeaderH = DluRect(dlg, 0, 0, 1, 15).bottom;
        m.footerTop = DluRect(dlg, 0, 342, 1, 1).top;
        m.hInst = g_hInst;
        m.iconId = IDI_APP;
        m.title = L"Key Remapper";
        m.subtitle = L"Per-game key bindings and display modes \x2022 kicks in when the game is in front";
        m.status = g_statusText;
        m.statusKind = g_statusKind;
        Th_PaintMain(dc, w, h, &m);
    } else {
        RECT instr = DluRect(dlg, 7, 5, 236, 14);
        Th_PaintDialog(dc, w, h, DluRect(dlg, 0, 84, 1, 1).top, &instr, L"Choose a key and what the game should get");
    }
}

static Surface *EnsureSurface(HWND dlg)
{
    Surface *s = SurfaceFor(dlg);
    if (!s) return NULL;
    RECT rc;
    GetClientRect(dlg, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return s->dc ? s : NULL;
    if (!s->dc || s->w != w || s->h != h) {
        if (s->dc) { SelectObject(s->dc, s->old); DeleteObject(s->bmp); DeleteDC(s->dc); }
        HDC screen = GetDC(dlg);
        s->dc = CreateCompatibleDC(screen);
        s->bmp = CreateCompatibleBitmap(screen, w, h);
        ReleaseDC(dlg, screen);
        s->old = SelectObject(s->dc, s->bmp);
        s->w = w; s->h = h;
        s->dirty = TRUE;
    }
    if (s->dirty) {
        PaintSurface(dlg, s->dc, w, h);
        s->dirty = FALSE;
    }
    return s;
}

static void FreeSurface(Surface *s)
{
    if (s->dc) { SelectObject(s->dc, s->old); DeleteObject(s->bmp); DeleteDC(s->dc); }
    memset(s, 0, sizeof *s);
}

static void InvalidateHeader(void)
{
    if (!g_hMain) return;
    RECT r;
    MainHeaderRect(&r);
    g_surfMain.dirty = TRUE;
    InvalidateRect(g_hMain, &r, FALSE);
}

/* Custom-draw for push buttons and check boxes (keeps native behaviour,
 * hot-tracking and keyboard handling; we only replace the pixels). */
static LRESULT DrawButtonCD(NMCUSTOMDRAW *cd)
{
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND b = cd->hdr.hwndFrom, parent = GetParent(b);
    Surface *s = EnsureSurface(parent);
    if (!s) return CDRF_DODEFAULT;

    RECT rc;
    GetClientRect(b, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return CDRF_SKIPDEFAULT;

    HDC mem = CreateCompatibleDC(cd->hdc);
    HBITMAP bmp = CreateCompatibleBitmap(cd->hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT pt = { 0, 0 };
    MapWindowPoints(b, parent, &pt, 1);
    BitBlt(mem, 0, 0, w, h, s->dc, pt.x, pt.y, SRCCOPY);

    WCHAR text[128];
    GetWindowTextW(b, text, 128);
    HFONT font = (HFONT)SendMessageW(b, WM_GETFONT, 0, 0);
    LONG style = GetWindowLongW(b, GWL_STYLE);
    LRESULT st = SendMessageW(b, BM_GETSTATE, 0, 0);
    UINT fl = 0;
    if (st & BST_HOT) fl |= TH_HOT;
    if (st & BST_PUSHED) fl |= TH_PRESSED;
    if (st & BST_FOCUS) fl |= TH_FOCUS;
    if ((st & BST_CHECKED) == BST_CHECKED) fl |= TH_CHECKED;
    if (!IsWindowEnabled(b)) fl |= TH_DISABLED;
    if (!(SendMessageW(b, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) fl |= TH_SHOWFOCUS;

    LONG type = style & BS_TYPEMASK;
    if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) {
        Th_DrawCheckbox(mem, &rc, text, font, fl);
    } else {
        if (type == BS_DEFPUSHBUTTON) fl |= TH_DEFAULT;
        Th_DrawButton(mem, &rc, text, font, fl);
    }

    BitBlt(cd->hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    return CDRF_SKIPDEFAULT;
}

static BOOL IsButtonClass(HWND h)
{
    WCHAR cls[16];
    return GetClassNameW(h, cls, 16) && SameText(cls, L"Button");
}

/* Shared message handling for the themed dialogs. Returns TRUE if handled. */
static BOOL ThemedDlgMsg(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR *ret)
{
    switch (msg) {
    case WM_ERASEBKGND:
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        *ret = TRUE;
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(dlg, &ps);
        Surface *s = EnsureSurface(dlg);
        if (s) BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                      ps.rcPaint.bottom - ps.rcPaint.top, s->dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(dlg, &ps);
        *ret = TRUE;
        return TRUE;
    }
    case WM_SIZE:
    case WM_DPICHANGED:
    case WM_THEMECHANGED: {
        Surface *s = SurfaceFor(dlg);
        if (s) s->dirty = TRUE;
        InvalidateRect(dlg, NULL, FALSE);
        return FALSE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int cid = GetDlgCtrlID((HWND)lp);
        BOOL path = cid == IDC_GAMEPATH || cid == IDC_DISPINFO;
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, path ? RGB(0x4D, 0x60, 0x82) : RGB(0x1B, 0x1B, 0x1B));
        *ret = (INT_PTR)GetStockObject(WHITE_BRUSH);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->code == NM_CUSTOMDRAW && IsButtonClass(h->hwndFrom)) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, DrawButtonCD((NMCUSTOMDRAW *)lp));
            *ret = TRUE;
            return TRUE;
        }
        return FALSE;
    }
    }
    return FALSE;
}

static void StyleCaption(HWND dlg)
{
    BOOL dark = TRUE;
    COLORREF cap = RGB(0x4B, 0x53, 0x5E), txt = RGB(255, 255, 255);
    DwmSetWindowAttribute(dlg, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    DwmSetWindowAttribute(dlg, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
    DwmSetWindowAttribute(dlg, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof txt);
}

/* ---- Mapping dialog ---- */
typedef struct { Map m; int capField; } MapDlgState;

static void AddComboItem(HWND cb, WORD id)
{
    WCHAR name[128];
    KeyName(id, name, 128);
    LRESULT i = SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)name);
    if (i >= 0) SendMessageW(cb, CB_SETITEMDATA, (WPARAM)i, id);
}

static void FillKeyCombo(HWND cb, BOOL target)
{
    static const WORD ext[] = { 0x11C, 0x11D, 0x135, 0x137, 0x138, 0x145, 0x147, 0x148, 0x149, 0x14B,
                                0x14D, 0x14F, 0x150, 0x151, 0x152, 0x153, 0x15B, 0x15C, 0x15D };
    WCHAR name[128];
    SendMessageW(cb, WM_SETREDRAW, FALSE, 0);
    if (target) {
        AddComboItem(cb, KEY_DISABLED);
        AddComboItem(cb, MOUSE_LEFT);  AddComboItem(cb, MOUSE_RIGHT); AddComboItem(cb, MOUSE_MIDDLE);
        AddComboItem(cb, MOUSE_X1);    AddComboItem(cb, MOUSE_X2);
    }
    for (WORD s = 0x01; s <= 0x58; ++s) {
        if (s == 0x54 || s == 0x55) continue;
        LONG lp = (LONG)((DWORD)s << 16);
        if (s != 0x45 && GetKeyNameTextW(lp, name, 128) <= 0) continue;
        AddComboItem(cb, s);
    }
    for (int i = 0; i < (int)(sizeof ext / sizeof ext[0]); ++i) AddComboItem(cb, ext[i]);
    for (WORD s = 0x64; s <= 0x6E; ++s) AddComboItem(cb, s); /* F13-F23 */
    AddComboItem(cb, 0x76);                                    /* F24 */
    SendMessageW(cb, WM_SETREDRAW, TRUE, 0);
}

static void SelectComboId(HWND cb, WORD id)
{
    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; ++i) {
        if ((WORD)SendMessageW(cb, CB_GETITEMDATA, i, 0) == id) { SendMessageW(cb, CB_SETCURSEL, i, 0); return; }
    }
    AddComboItem(cb, id);
    SendMessageW(cb, CB_SETCURSEL, n, 0);
}

static WORD ComboId(HWND cb)
{
    LRESULT i = SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0) return 0;
    return (WORD)SendMessageW(cb, CB_GETITEMDATA, (WPARAM)i, 0);
}

static void StopCapture(HWND dlg, MapDlgState *st)
{
    if (st->capField) {
        PostThreadMessageW(g_hookTid, WM_HT_CAPTURE, 0, 0);
        SetDlgItemTextW(dlg, IDC_SRCCAP, L"Detect");
        SetDlgItemTextW(dlg, IDC_DSTCAP, L"Detect");
        st->capField = 0;
    }
}

static INT_PTR CALLBACK MapDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    MapDlgState *st = (MapDlgState *)GetWindowLongPtrW(dlg, DWLP_USER);
    INT_PTR tr;
    if (msg != WM_INITDIALOG && ThemedDlgMsg(dlg, msg, wp, lp, &tr)) return tr;
    switch (msg) {
    case WM_DESTROY:
        FreeSurface(&g_surfMap);
        g_hMapDlg = NULL;
        return FALSE;
    case WM_INITDIALOG:
        g_hMapDlg = dlg;
        g_surfMap.dirty = TRUE;
        st = (MapDlgState *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)st);
        FillKeyCombo(GetDlgItem(dlg, IDC_SRC), FALSE);
        FillKeyCombo(GetDlgItem(dlg, IDC_DST), TRUE);
        if (st->m.src) SelectComboId(GetDlgItem(dlg, IDC_SRC), st->m.src);
        if (st->m.dst) SelectComboId(GetDlgItem(dlg, IDC_DST), st->m.dst);
        return TRUE;

    case WM_CAP_KEY:
        if (st && st->capField) {
            int field = st->capField;
            st->capField = 0;
            SetDlgItemTextW(dlg, IDC_SRCCAP, L"Detect");
            SetDlgItemTextW(dlg, IDC_DSTCAP, L"Detect");
            SelectComboId(GetDlgItem(dlg, field == 1 ? IDC_SRC : IDC_DST), (WORD)wp);
            SetDlgItemTextW(dlg, IDC_HINT, L"Tip: click Detect, then press the key on your keyboard.");
            SetFocus(GetDlgItem(dlg, field == 1 ? IDC_DSTCAP : IDOK));
        }
        return TRUE;

    case WM_COMMAND:
        if (!st) break;
        switch (LOWORD(wp)) {
        case IDC_SRCCAP:
        case IDC_DSTCAP: {
            int field = LOWORD(wp) == IDC_SRCCAP ? 1 : 2;
            if (st->capField == field) { StopCapture(dlg, st); return TRUE; }
            StopCapture(dlg, st);
            st->capField = field;
            SetDlgItemTextW(dlg, LOWORD(wp), L"Press a key...");
            SetDlgItemTextW(dlg, IDC_HINT, L"Press the key now. (Click the button again to cancel.)");
            PostThreadMessageW(g_hookTid, WM_HT_CAPTURE, (WPARAM)dlg, 0);
            return TRUE;
        }
        case IDOK: {
            WORD s = ComboId(GetDlgItem(dlg, IDC_SRC));
            WORD d = ComboId(GetDlgItem(dlg, IDC_DST));
            if (!s || !d) {
                MessageBoxW(dlg, L"Choose both the key you press and what it should send.", L"Key mapping", MB_ICONINFORMATION);
                return TRUE;
            }
            if (s == d) {
                MessageBoxW(dlg, L"The key already sends itself - pick a different target.", L"Key mapping", MB_ICONINFORMATION);
                return TRUE;
            }
            StopCapture(dlg, st);
            st->m.src = s; st->m.dst = d;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            StopCapture(dlg, st);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static int SelectedMap(void)
{
    return (int)SendDlgItemMessageW(g_hMain, IDC_MAPS, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void EditMapping(int mi)
{
    if (g_sel < 0) return;
    Game *g = &g_games[g_sel];
    MapDlgState st; memset(&st, 0, sizeof st);
    if (mi >= 0 && mi < g->nmaps) st.m = g->maps[mi];
    else if (g->nmaps >= MAX_MAPS) { MessageBoxW(g_hMain, L"Too many mappings for this game.", L"Key Remapper", MB_ICONWARNING); return; }

    if (DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAP), g_hMain, MapDlgProc, (LPARAM)&st) != IDOK) return;

    EnterCriticalSection(&g_lock);
    /* one mapping per source key: drop any other mapping with the same source */
    for (int i = 0; i < g->nmaps; ++i) {
        if (i != mi && g->maps[i].src == st.m.src) {
            memmove(&g->maps[i], &g->maps[i + 1], (size_t)(g->nmaps - i - 1) * sizeof(Map));
            g->nmaps--;
            if (mi > i) mi--;
            i--;
        }
    }
    if (mi >= 0 && mi < g->nmaps) g->maps[mi] = st.m;
    else g->maps[g->nmaps++] = st.m;
    LeaveCriticalSection(&g_lock);
    Commit();
    RefreshMaps();
}

static void RemoveMapping(void)
{
    int mi = SelectedMap();
    if (g_sel < 0 || mi < 0) return;
    Game *g = &g_games[g_sel];
    EnterCriticalSection(&g_lock);
    memmove(&g->maps[mi], &g->maps[mi + 1], (size_t)(g->nmaps - mi - 1) * sizeof(Map));
    g->nmaps--;
    LeaveCriticalSection(&g_lock);
    Commit();
    RefreshMaps();
}

/* ---- Copy bindings between games ---- */
#define COPY_CANCEL   0
#define COPY_REPLACE  1
#define COPY_MERGE    2
#define COPY_SCRATCH  3
#define COPY_NONE     4   /* no other game has bindings */

static void ApplyCopy(int dst, int src, BOOL replace)
{
    if (dst < 0 || src < 0 || dst >= g_ngames || src >= g_ngames || dst == src) return;
    EnterCriticalSection(&g_lock);
    Game *d = &g_games[dst];
    const Game *s = &g_games[src];
    if (replace) {
        d->nmaps = 0;
        d->scanOnly = s->scanOnly;
    }
    for (int i = 0; i < s->nmaps; ++i) {
        int j;
        for (j = 0; j < d->nmaps; ++j) if (d->maps[j].src == s->maps[i].src) break;
        if (j < d->nmaps) d->maps[j].dst = s->maps[i].dst;
        else if (d->nmaps < MAX_MAPS) d->maps[d->nmaps++] = s->maps[i];
    }
    LeaveCriticalSection(&g_lock);
    Commit();
    RefreshMaps();
}

/* Asks which game to copy from (and, if needed, replace vs. merge).
 * newGame: offers "start from scratch" instead of replace/merge. */
static void ChooseCopySource(int target, BOOL newGame, int *src, int *action)
{
    static TASKDIALOG_BUTTON radios[MAX_GAMES];
    static WCHAR labels[MAX_GAMES][192];
    static WCHAR content[300];
    int n = 0;

    *src = -1;
    *action = COPY_CANCEL;
    for (int i = 0; i < g_ngames; ++i) {
        if (i == target || g_games[i].nmaps == 0) continue;
        wsprintfW(labels[n], L"%s   (%d binding%s)", g_games[i].name, g_games[i].nmaps,
                  g_games[i].nmaps == 1 ? L"" : L"s");
        radios[n].nButtonID = 1000 + i;
        radios[n].pszButtonText = labels[n];
        n++;
    }
    if (n == 0) {
        *action = newGame ? COPY_SCRATCH : COPY_NONE;
        return;
    }

    TASKDIALOG_BUTTON buttons[2];
    int nb = 0;
    BOOL targetHasMaps = g_games[target].nmaps > 0;
    if (newGame) {
        buttons[nb].nButtonID = 100; buttons[nb++].pszButtonText = L"Copy bindings from the selected game\nYou can still change them afterwards.";
        buttons[nb].nButtonID = 102; buttons[nb++].pszButtonText = L"Start from scratch\nSet up this game's keys one by one.";
        wsprintfW(content, L"%s was added to the library. Pick a game to reuse its key bindings:", g_games[target].name);
    } else if (targetHasMaps) {
        buttons[nb].nButtonID = 100; buttons[nb++].pszButtonText = L"Replace this game's bindings\nRemoves the current ones and uses the copied ones.";
        buttons[nb].nButtonID = 101; buttons[nb++].pszButtonText = L"Add to this game's bindings\nKeeps the current ones. Copied keys win where both use the same key.";
        wsprintfW(content, L"Copy key bindings into %s from:", g_games[target].name);
    } else {
        buttons[nb].nButtonID = 100; buttons[nb++].pszButtonText = L"Copy bindings\nYou can still change them afterwards.";
        wsprintfW(content, L"Copy key bindings into %s from:", g_games[target].name);
    }

    TASKDIALOGCONFIG c;
    memset(&c, 0, sizeof c);
    c.cbSize = sizeof c;
    c.hwndParent = g_hMain;
    c.hInstance = g_hInst;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW | TDF_USE_HICON_MAIN;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = L"Key Remapper";
    c.hMainIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    c.pszMainInstruction = newGame ? L"Start with bindings from another game?" : L"Copy bindings from another game";
    c.pszContent = content;
    c.cButtons = (UINT)nb;
    c.pButtons = buttons;
    c.nDefaultButton = 100;
    c.cRadioButtons = (UINT)n;
    c.pRadioButtons = radios;
    c.nDefaultRadioButton = radios[0].nButtonID;

    int btn = 0, radio = 0;
    HRESULT hr = TaskDialogIndirect(&c, &btn, &radio, NULL);
    if (c.hMainIcon) DestroyIcon(c.hMainIcon);
    if (FAILED(hr)) return;

    if (btn == 102) { *action = COPY_SCRATCH; return; }
    if (btn != 100 && btn != 101) return;
    if (radio < 1000 || radio - 1000 >= g_ngames) return;
    *src = radio - 1000;
    *action = (btn == 101) ? COPY_MERGE : COPY_REPLACE;
}

static void CopyFromOtherGame(void)
{
    if (g_sel < 0) return;
    int src, action;
    ChooseCopySource(g_sel, FALSE, &src, &action);
    if (action == COPY_NONE)
        MessageBoxW(g_hMain, L"No other game in the library has key bindings yet.", L"Key Remapper", MB_ICONINFORMATION);
    else if (action == COPY_REPLACE || action == COPY_MERGE)
        ApplyCopy(g_sel, src, action == COPY_REPLACE);
}

static void AddGame(void)
{
    static WCHAR file[MAX_PATH];
    if (g_ngames >= MAX_GAMES) { MessageBoxW(g_hMain, L"The library is full.", L"Key Remapper", MB_ICONWARNING); return; }
    file[0] = 0;
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hMain;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select the game's .exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn)) return;

    for (int i = 0; i < g_ngames; ++i) {
        if (SameText(g_games[i].path, file)) { g_sel = i; RefreshGames(); return; }
    }
    EnterCriticalSection(&g_lock);
    Game *g = &g_games[g_ngames];
    memset(g, 0, sizeof *g);
    lstrcpynW(g->path, file, MAX_PATH);
    lstrcpynW(g->name, FileNamePart(file), 128);
    g->enabled = TRUE;
    g_sel = g_ngames++;
    LeaveCriticalSection(&g_lock);
    Commit();
    RefreshGames();

    int src, action;
    ChooseCopySource(g_sel, TRUE, &src, &action);
    if (action == COPY_REPLACE || action == COPY_MERGE) ApplyCopy(g_sel, src, TRUE);
    else if (action == COPY_SCRATCH) EditMapping(-1);
}

static void RemoveGame(void)
{
    if (g_sel < 0) return;
    static WCHAR q[300];
    wsprintfW(q, L"Remove %s and its mappings from the library?", g_games[g_sel].name);
    if (MessageBoxW(g_hMain, q, L"Key Remapper", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    EnterCriticalSection(&g_lock);
    memmove(&g_games[g_sel], &g_games[g_sel + 1], (size_t)(g_ngames - g_sel - 1) * sizeof(Game));
    g_ngames--;
    LeaveCriticalSection(&g_lock);
    if (g_sel >= g_ngames) g_sel = g_ngames - 1;
    Commit();
    RefreshGames();
}

static void TogglePause(void)
{
    InterlockedExchange(&g_paused, !g_paused);
    UpdatePauseUi();
    PostReload();
    SetStatus(-1);
}

static void ShowTrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open Key Remapper");
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), IDM_PAUSE, L"Pause remapping\tCtrl+Shift+F12");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    SetMenuDefaultItem(m, IDM_OPEN, FALSE);
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hMain);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hMain, NULL);
    PostMessageW(g_hMain, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void InitListColumns(void)
{
    HWND games = GetDlgItem(g_hMain, IDC_GAMES), maps = GetDlgItem(g_hMain, IDC_MAPS);
    LVCOLUMNW c; memset(&c, 0, sizeof c);
    RECT r;

    SetWindowTheme(games, L"Explorer", NULL);   /* Vista-style glassy selection */
    SetWindowTheme(maps, L"Explorer", NULL);
    SendMessageW(games, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    GetClientRect(games, &r);
    c.mask = LVCF_WIDTH | LVCF_TEXT; c.cx = r.right - r.left; c.pszText = L"Game";
    SendMessageW(games, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
    g_imgGames = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_COLOR32 | ILC_MASK, 8, 8);
    SendMessageW(games, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)g_imgGames);

    SendMessageW(maps, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    GetClientRect(maps, &r);
    int w = (r.right - r.left) - GetSystemMetrics(SM_CXVSCROLL);
    c.cx = w / 2; c.pszText = L"When I press";
    SendMessageW(maps, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
    c.cx = w - w / 2; c.pszText = L"The game gets";
    SendMessageW(maps, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
}

static INT_PTR CALLBACK MainDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_msgTaskbarCreated && msg) { TrayAdd(); SetStatus(-1); return TRUE; }
    if (msg == g_msgShow && msg) { ShowMain(); return TRUE; }

    INT_PTR tr;
    if (msg != WM_INITDIALOG && ThemedDlgMsg(dlg, msg, wp, lp, &tr)) return tr;

    switch (msg) {
    case WM_INITDIALOG: {
        g_hMain = dlg;
        g_surfMain.dirty = TRUE;
        StyleCaption(dlg);
        HICON big = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                      GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        HICON sm = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)sm);
        InitListColumns();
        InitDisplayCombos();
        CheckDlgButton(dlg, IDC_AUTOSTART, g_autostart ? BST_CHECKED : BST_UNCHECKED);
        if (g_ngames > 0) g_sel = 0;
        RefreshGames();
        TrayAdd();
        RegisterHotKey(dlg, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_F12);
        UpdatePauseUi();
        SetStatus(-1);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 0);   /* never leak a stale custom-draw result */
        if (h->idFrom == IDC_GAMES && h->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *n = (NMLISTVIEW *)lp;
            if ((n->uChanged & LVIF_STATE) && (n->uNewState & LVIS_SELECTED) && !(n->uOldState & LVIS_SELECTED)) {
                g_sel = n->iItem;
                RefreshMaps();
            }
        } else if (h->idFrom == IDC_MAPS && h->code == NM_DBLCLK) {
            int mi = SelectedMap();
            EditMapping(mi >= 0 ? mi : -1);
        } else if (h->idFrom == IDC_MAPS && h->code == LVN_KEYDOWN) {
            if (((NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) RemoveMapping();
        } else if (h->idFrom == IDC_GAMES && h->code == LVN_KEYDOWN) {
            if (((NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) RemoveGame();
        }
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ADDGAME:  AddGame(); return TRUE;
        case IDC_REMGAME:  RemoveGame(); return TRUE;
        case IDC_ADDMAP:   EditMapping(-1); return TRUE;
        case IDC_EDITMAP: { int mi = SelectedMap(); if (mi >= 0) EditMapping(mi); return TRUE; }
        case IDC_REMMAP:   RemoveMapping(); return TRUE;
        case IDC_COPYFROM: CopyFromOtherGame(); return TRUE;
        case IDC_DISPMODE:
        case IDC_ASPECT:
        case IDC_SCALING:
        case IDC_METHOD:
        case IDC_FILTER:
            if (HIWORD(wp) == CBN_SELCHANGE) OnDisplayChanged(LOWORD(wp));
            return TRUE;
        case IDC_DPIFIX:
        case IDC_RESETSIZE:
            OnDisplayChanged(LOWORD(wp));
            return TRUE;
        case IDC_ENABLED:
        case IDC_SCANONLY:
            if (g_sel >= 0) {
                EnterCriticalSection(&g_lock);
                g_games[g_sel].enabled  = IsDlgButtonChecked(dlg, IDC_ENABLED) == BST_CHECKED;
                g_games[g_sel].scanOnly = IsDlgButtonChecked(dlg, IDC_SCANONLY) == BST_CHECKED;
                LeaveCriticalSection(&g_lock);
                Commit();
            }
            return TRUE;
        case IDC_AUTOSTART: {
            BOOL on = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
            HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
            BOOL ok = SetAutostart(on);
            SetCursor(old);
            if (!ok) {
                MessageBoxW(dlg, L"Couldn't create the startup task.", L"Key Remapper", MB_ICONWARNING);
                CheckDlgButton(dlg, IDC_AUTOSTART, BST_UNCHECKED);
                on = FALSE;
            }
            g_autostart = on;
            SaveConfig();
            return TRUE;
        }
        case IDC_PAUSE:
        case IDM_PAUSE:    TogglePause(); return TRUE;
        case IDC_HIDE:
        case IDCANCEL:     HideMain(); return TRUE;  /* Esc / close -> tray */
        case IDM_OPEN:     ShowMain(); return TRUE;
        case IDM_EXIT:     DestroyWindow(dlg); return TRUE;
        }
        break;

    case WM_HOTKEY:
        if (wp == 1) {
            TogglePause();
            TrayBalloon(g_paused ? L"Remapping paused." : L"Remapping resumed.");
        }
        return TRUE;

    case WM_UI_STATUS:
        SetStatus((int)wp);
        return TRUE;

    case WM_UI_GAMEUPDATED:
        SaveConfig();
        if ((int)wp == g_sel) RefreshDisplay();
        return TRUE;

    case WM_UI_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: ShowMain(); break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:   ShowTrayMenu(); break;
        }
        return TRUE;

    case WM_CLOSE:
        HideMain();
        return TRUE;

    case WM_DESTROY:
        UnregisterHotKey(dlg, 1);
        TrayRemove();
        FreeSurface(&g_surfMain);
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */
static void DisableProcessThrottling(void)
{
    PROCESS_POWER_THROTTLING_STATE st;
    memset(&st, 0, sizeof st);
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
}

static BOOL HasArg(const WCHAR *cmdline, const WCHAR *arg)
{
    int n = lstrlenW(arg);
    for (const WCHAR *p = cmdline; *p; ++p)
        if (CompareStringOrdinal(p, n, arg, n, TRUE) == CSTR_EQUAL) return TRUE;
    return FALSE;
}

static int AppMain(void)
{
    g_hInst = GetModuleHandleW(NULL);
    g_msgShow = RegisterWindowMessageW(L"KeyRemapper.Show");
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Local\\KeyRemapper.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"#32770", L"Key Remapper");
        if (other) PostMessageW(other, g_msgShow, 0, 0);
        return 0;
    }

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    DisableProcessThrottling();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    lstrcpyW(g_iniPath, g_exePath);
    {
        int n = lstrlenW(g_iniPath);
        while (n > 0 && g_iniPath[n - 1] != L'.' && g_iniPath[n - 1] != L'\\') --n;
        if (n > 0 && g_iniPath[n - 1] == L'.') g_iniPath[n - 1] = 0;
        lstrcatW(g_iniPath, L".ini");
    }

    InitializeCriticalSection(&g_lock);
    LoadConfig();
    Th_Init();

    g_hMain = CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainDlgProc, 0);
    if (!g_hMain) return 1;

    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_hHookThread = CreateThread(NULL, 64 * 1024, InputThread, ready, 0, &g_hookTid);
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
    WinMode_Start();

    const WCHAR *cmd = GetCommandLineW();
    if (!HasArg(cmd, L"/tray") || g_ngames == 0) ShowMain();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hMain, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    WinMode_Stop();   /* puts game windows back the way they were */
    PostThreadMessageW(g_hookTid, WM_QUIT, 0, 0);
    WaitForSingleObject(g_hHookThread, 2000);
    if (mtx) CloseHandle(mtx);
    return 0;
}

void __stdcall WinMainCRTStartup(void)
{
    ExitProcess((UINT)AppMain());
}
