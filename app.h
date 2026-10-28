#pragma once
#include <windows.h>

#define MAX_GAMES   64
#define MAX_MAPS    128

typedef struct { WORD src, dst; } Map;

/* Display modes */
#define DM_NONE        0   /* leave the game window alone        */
#define DM_BORDERLESS  1   /* borderless fullscreen, aspect kept  */
#define DM_WINDOW      2   /* resizable window, aspect kept       */

/* Aspect ratio choices */
#define AR_AUTO   0        /* the game's own (from its original size) */
#define AR_4_3    1
#define AR_16_9   2
#define AR_16_10  3
#define AR_5_4    4

/* How borderless fullscreen gets the picture big */
#define FS_ZOOM    0       /* zoom the picture (works with every game)   */
#define FS_RESIZE  1       /* resize the window; the game must redraw    */

/* Pixel filter for the zoom */
#define FL_SMOOTH  0
#define FL_SHARP   1

/* Scaling */
#define SC_FIT      0      /* as large as possible               */
#define SC_INTEGER  1      /* whole multiples (pixel-perfect)    */

typedef struct {
    WCHAR path[MAX_PATH];
    WCHAR name[128];
    BOOL  enabled;
    BOOL  scanOnly;
    int   nmaps;
    Map   maps[MAX_MAPS];

    /* display */
    int   dispMode;
    int   aspect;
    int   scaling;
    int   fsMethod;
    int   filter;
    BOOL  dpiFix;
    BOOL  dpiFixSet;       /* user (or first mode pick) has decided dpiFix */
    int   baseW, baseH;    /* game's original client size; 0 = unknown     */
    int   winScale;        /* window scale in 1/1000; 0 = not set yet      */
    int   winX, winY;      /* last window position (top-left)             */
    BOOL  hasPos;
} Game;

extern Game             g_games[MAX_GAMES];
extern int              g_ngames;
extern CRITICAL_SECTION g_lock;
extern HINSTANCE        g_hInst;
extern HWND             g_hMain;

const WCHAR *FileNamePart(const WCHAR *p);
BOOL SameText(const WCHAR *a, const WCHAR *b);

/* Finds the library game whose exe is running as `pid`. Returns -1 if none.
 * If outPath is given, receives that game's library path (for stable identity). */
int  FindGameForPid(DWORD pid, WCHAR *outPath);

/* UI notifications posted to g_hMain */
#define WM_UI_GAMEUPDATED  (WM_APP + 13)   /* wParam = game index; display settings changed */

/* Window-mode thread (winmode.c) */
#define WM_WM_RELOAD       (WM_APP + 30)   /* settings changed: re-apply                   */
void  WinMode_Start(void);
void  WinMode_Stop(void);                  /* restores touched windows                     */
void  WinMode_Reload(void);
void  WinMode_SetDpiFix(const WCHAR *exePath, BOOL on);
void  WinMode_AspectOf(const Game *g, int *num, int *den);
