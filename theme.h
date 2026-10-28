#pragma once
#include <windows.h>

/* Glossy "Vista style" painting helpers (GDI+ flat API, anti-aliased). */

#define TH_HOT        0x01
#define TH_PRESSED    0x02
#define TH_FOCUS      0x04
#define TH_DISABLED   0x08
#define TH_DEFAULT    0x10
#define TH_CHECKED    0x20
#define TH_SHOWFOCUS  0x40

#define TH_STATUS_WATCHING 0
#define TH_STATUS_ACTIVE   1
#define TH_STATUS_PAUSED   2

typedef struct {
    RECT         header;
    RECT         cards[3];
    const WCHAR *cardTitle[3];
    int          ncards;
    int          cardHeaderH;
    int          footerTop;
    HINSTANCE    hInst;
    int          iconId;
    const WCHAR *title;
    const WCHAR *subtitle;
    const WCHAR *status;
    int          statusKind;
} ThMain;

void Th_Init(void);
void Th_Shutdown(void);
void Th_PaintMain(HDC dc, int w, int h, const ThMain *m);
void Th_PaintDialog(HDC dc, int w, int h, int footerTop, const RECT *instrRect, const WCHAR *instruction);
void Th_DrawButton(HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT flags);
void Th_DrawCheckbox(HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT flags);
