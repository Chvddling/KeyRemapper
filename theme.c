/*
 * Glossy Vista-style painting. Shapes use the GDI+ flat API for anti-aliasing
 * and alpha; text uses plain GDI (ClearType) on the same opaque DC.
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include "theme.h"

/* ---- GDI+ flat API (declared here so we stay plain C) ---------------- */
typedef float REAL;
typedef DWORD ARGB;
typedef struct { REAL X, Y, Width, Height; } GpRectF;
typedef struct GpGraphics GpGraphics;
typedef struct GpPath GpPath;
typedef struct GpBrush GpBrush;
typedef struct GpPen GpPen;
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GdiplusStartupInput;

int  WINAPI GdiplusStartup(ULONG_PTR *, const GdiplusStartupInput *, void *);
void WINAPI GdiplusShutdown(ULONG_PTR);
int  WINAPI GdipCreateFromHDC(HDC, GpGraphics **);
int  WINAPI GdipDeleteGraphics(GpGraphics *);
int  WINAPI GdipSetSmoothingMode(GpGraphics *, int);
int  WINAPI GdipSetClipRect(GpGraphics *, REAL, REAL, REAL, REAL, int);
int  WINAPI GdipResetClip(GpGraphics *);
int  WINAPI GdipCreatePath(int, GpPath **);
int  WINAPI GdipDeletePath(GpPath *);
int  WINAPI GdipAddPathArc(GpPath *, REAL, REAL, REAL, REAL, REAL, REAL);
int  WINAPI GdipAddPathLine(GpPath *, REAL, REAL, REAL, REAL);
int  WINAPI GdipAddPathRectangle(GpPath *, REAL, REAL, REAL, REAL);
int  WINAPI GdipAddPathEllipse(GpPath *, REAL, REAL, REAL, REAL);
int  WINAPI GdipClosePathFigure(GpPath *);
int  WINAPI GdipCreateLineBrushFromRect(const GpRectF *, ARGB, ARGB, int, int, GpBrush **);
int  WINAPI GdipCreateSolidFill(ARGB, GpBrush **);
int  WINAPI GdipCreatePathGradientFromPath(const GpPath *, GpBrush **);
int  WINAPI GdipSetPathGradientCenterColor(GpBrush *, ARGB);
int  WINAPI GdipSetPathGradientSurroundColorsWithCount(GpBrush *, const ARGB *, INT *);
int  WINAPI GdipDeleteBrush(GpBrush *);
int  WINAPI GdipFillPath(GpGraphics *, GpBrush *, GpPath *);
int  WINAPI GdipCreatePen1(ARGB, REAL, int, GpPen **);
int  WINAPI GdipSetPenLineJoin(GpPen *, int);
int  WINAPI GdipSetPenStartCap(GpPen *, int);
int  WINAPI GdipSetPenEndCap(GpPen *, int);
int  WINAPI GdipDeletePen(GpPen *);
int  WINAPI GdipDrawPath(GpGraphics *, GpPen *, GpPath *);
int  WINAPI GdipDrawLine(GpGraphics *, GpPen *, REAL, REAL, REAL, REAL);

#define A(a, rgb) ((((ARGB)(a)) << 24) | ((ARGB)(rgb) & 0xFFFFFF))
#define O(rgb)    A(0xFF, rgb)

static ULONG_PTR s_token;

void Th_Init(void)
{
    GdiplusStartupInput in = { 1, NULL, FALSE, TRUE };
    GdiplusStartup(&s_token, &in, NULL);
}

void Th_Shutdown(void)
{
    if (s_token) GdiplusShutdown(s_token);
    s_token = 0;
}

/* ---- primitives ------------------------------------------------------ */
static GpGraphics *G(HDC dc)
{
    GpGraphics *g = NULL;
    GdipCreateFromHDC(dc, &g);
    if (g) GdipSetSmoothingMode(g, 4 /* AntiAlias */);
    return g;
}

static GpPath *RRect(REAL x, REAL y, REAL w, REAL h, REAL r)
{
    GpPath *p = NULL;
    GdipCreatePath(0, &p);
    if (!p) return NULL;
    REAL d = r * 2;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 1.0f) {
        GdipAddPathRectangle(p, x, y, w, h);
        return p;
    }
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

static GpPath *EllipsePath(REAL x, REAL y, REAL w, REAL h)
{
    GpPath *p = NULL;
    GdipCreatePath(0, &p);
    if (p) GdipAddPathEllipse(p, x, y, w, h);
    return p;
}

/* Vertical gradient c0 (at y0) -> c1 (at y1) over a path. */
static void FillV(GpGraphics *g, GpPath *p, REAL x, REAL y0, REAL w, REAL y1, ARGB c0, ARGB c1)
{
    if (!g || !p || y1 <= y0) return;
    GpRectF r = { x - 1, y0 - 0.5f, w + 2, (y1 - y0) + 1.0f };
    GpBrush *b = NULL;
    GdipCreateLineBrushFromRect(&r, c0, c1, 1 /* vertical */, 3 /* TileFlipXY */, &b);
    if (b) { GdipFillPath(g, b, p); GdipDeleteBrush(b); }
}

/* Classic two-band gloss: a lighter top half and a darker bottom half. */
static void FillGloss(GpGraphics *g, GpPath *p, REAL x, REAL y, REAL w, REAL h,
                      ARGB t0, ARGB t1, ARGB b0, ARGB b1)
{
    REAL mid = y + (REAL)(int)(h * 0.5f);
    GdipSetClipRect(g, x - 1, y - 1, w + 2, mid - y + 1, 0);
    FillV(g, p, x, y, w, mid, t0, t1);
    GdipSetClipRect(g, x - 1, mid, w + 2, y + h - mid + 1, 0);
    FillV(g, p, x, mid, w, y + h, b0, b1);
    GdipResetClip(g);
}

static void FillSolid(GpGraphics *g, GpPath *p, ARGB c)
{
    GpBrush *b = NULL;
    if (!g || !p) return;
    GdipCreateSolidFill(c, &b);
    if (b) { GdipFillPath(g, b, p); GdipDeleteBrush(b); }
}

static void Stroke(GpGraphics *g, GpPath *p, ARGB c, REAL width)
{
    GpPen *pen = NULL;
    if (!g || !p) return;
    GdipCreatePen1(c, width, 0, &pen);
    if (pen) { GdipDrawPath(g, pen, p); GdipDeletePen(pen); }
}

static void HLine(GpGraphics *g, REAL x0, REAL x1, REAL y, ARGB c)
{
    GpPen *pen = NULL;
    GdipCreatePen1(c, 1.0f, 0, &pen);
    if (pen) { GdipDrawLine(g, pen, x0, y, x1, y); GdipDeletePen(pen); }
}

static void Glow(GpGraphics *g, REAL cx, REAL cy, REAL rx, REAL ry, ARGB center)
{
    GpPath *p = EllipsePath(cx - rx, cy - ry, rx * 2, ry * 2);
    GpBrush *b = NULL;
    if (!p) return;
    GdipCreatePathGradientFromPath(p, &b);
    if (b) {
        ARGB edge = center & 0x00FFFFFF;
        INT n = 1;
        GdipSetPathGradientCenterColor(b, center);
        GdipSetPathGradientSurroundColorsWithCount(b, &edge, &n);
        GdipFillPath(g, b, p);
        GdipDeleteBrush(b);
    }
    GdipDeletePath(p);
}

static void Del(GpPath *p) { if (p) GdipDeletePath(p); }

static HFONT MakeFont(int px, int weight)
{
    if (px < 8) px = 8;
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

static void Text(HDC dc, const WCHAR *s, RECT r, HFONT f, COLORREF c, UINT fmt)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

/* Light footer / command bar, shared by both windows. */
static void Footer(GpGraphics *g, int w, int top, int h)
{
    if (top >= h) return;
    GpPath *p = RRect(0, (REAL)top, (REAL)w, (REAL)(h - top), 0);
    FillGloss(g, p, 0, (REAL)top, (REAL)w, (REAL)(h - top),
              O(0xFBFDFF), O(0xEEF3F9), O(0xE3EBF4), O(0xD7E1EE));
    Del(p);
    HLine(g, 0, (REAL)w, top + 0.5f, O(0xA9C3DF));
    HLine(g, 0, (REAL)w, top + 1.5f, A(0xD0, 0xFFFFFF));
}

/* ---- main window ----------------------------------------------------- */
void Th_PaintMain(HDC dc, int w, int h, const ThMain *m)
{
    const int H = m->header.bottom;
    const REAL fw = (REAL)w;
    const int chH = m->cardHeaderH;

    HFONT fTitle = MakeFont(H * 30 / 100, FW_SEMIBOLD);
    HFONT fSub   = MakeFont(H * 19 / 100, FW_NORMAL);
    HFONT fPill  = MakeFont(H * 20 / 100, FW_SEMIBOLD);
    HFONT fCard  = MakeFont(chH * 54 / 100, FW_SEMIBOLD);

    /* Measure the status text so the pill can hug it. */
    const int pad = H * 28 / 100;
    const int ph = H * 44 / 100;
    const int orb = ph * 52 / 100;
    SIZE ts = { 0, 0 };
    {
        HGDIOBJ o = SelectObject(dc, fPill);
        GetTextExtentPoint32W(dc, m->status, lstrlenW(m->status), &ts);
        SelectObject(dc, o);
    }
    int maxText = w * 46 / 100;
    if (ts.cx > maxText) ts.cx = maxText;
    const int pw = ph * 45 / 100 + orb + ph * 35 / 100 + ts.cx + ph * 60 / 100;
    const int px = w - pad - pw, py = (H - ph) / 2;

    GpGraphics *g = G(dc);
    GpPath *p;

    /* body */
    p = RRect(0, (REAL)H, fw, (REAL)(m->footerTop - H), 0);
    FillV(g, p, 0, (REAL)H, fw, (REAL)m->footerTop, O(0xF3F7FC), O(0xDAE6F3));
    Del(p);
    Glow(g, fw * 0.12f, (REAL)m->footerTop, fw * 0.62f, (m->footerTop - H) * 0.55f, A(0xA0, 0xFFFFFF));
    Glow(g, fw * 0.92f, (REAL)H, fw * 0.36f, (m->footerTop - H) * 0.40f, A(0x55, 0xC4E4FF));

    /* header: dark glass */
    p = RRect(0, 0, fw, (REAL)H, 0);
    FillGloss(g, p, 0, 0, fw, (REAL)H, O(0x4B535E), O(0x30363F), O(0x1B1F25), O(0x101317));
    Del(p);
    Glow(g, fw * 0.80f, H * 1.05f, fw * 0.30f, H * 0.85f, A(0xA8, 0x2F95E6));
    Glow(g, fw * 0.58f, H * 1.15f, fw * 0.17f, H * 0.60f, A(0x70, 0x34C7B5));
    Glow(g, pad + H * 0.29f, H * 0.5f, H * 0.55f, H * 0.55f, A(0x38, 0xBFE6FF));
    p = RRect(0, 0, fw, H * 0.5f, 0);
    FillV(g, p, 0, 0, fw, H * 0.5f, A(0x2C, 0xFFFFFF), A(0x0A, 0xFFFFFF));
    Del(p);
    HLine(g, 0, fw, 0.5f, A(0x38, 0xFFFFFF));
    HLine(g, 0, fw, H - 1.5f, O(0x07090B));
    HLine(g, 0, fw, H - 0.5f, A(0xC8, 0x5FB4F0));

    /* status pill */
    {
        static const ARGB orbCols[3][2] = {
            { O(0x9BD8FF), O(0x1C67BE) },   /* watching: blue  */
            { O(0x9BF07A), O(0x1A8E1A) },   /* active:   green */
            { O(0xFFDA7A), O(0xCC7608) },   /* paused:   amber */
        };
        int k = (m->statusKind >= 0 && m->statusKind <= 2) ? m->statusKind : 0;
        p = RRect((REAL)px, (REAL)py, (REAL)pw, (REAL)ph, ph * 0.5f);
        FillGloss(g, p, (REAL)px, (REAL)py, (REAL)pw, (REAL)ph,
                  A(0x34, 0xFFFFFF), A(0x1A, 0xFFFFFF), A(0x08, 0xFFFFFF), A(0x16, 0xFFFFFF));
        Del(p);
        p = RRect(px + 0.5f, py + 0.5f, pw - 1.0f, ph - 1.0f, ph * 0.5f);
        Stroke(g, p, A(0x60, 0xFFFFFF), 1.0f);
        Del(p);

        REAL ox = px + ph * 0.45f, oy = py + (ph - orb) * 0.5f;
        Glow(g, ox + orb * 0.5f, oy + orb * 0.5f, orb * 1.15f, orb * 1.15f, (orbCols[k][0] & 0xFFFFFF) | 0x80000000);
        p = EllipsePath(ox, oy, (REAL)orb, (REAL)orb);
        FillV(g, p, ox, oy, (REAL)orb, oy + orb, orbCols[k][0], orbCols[k][1]);
        Stroke(g, p, A(0xA0, (orbCols[k][1] >> 1) & 0x7F7F7F), 1.0f);
        Del(p);
        p = EllipsePath(ox + orb * 0.18f, oy + orb * 0.07f, orb * 0.64f, orb * 0.46f);
        FillV(g, p, ox, oy + orb * 0.07f, (REAL)orb, oy + orb * 0.53f, A(0xE0, 0xFFFFFF), A(0x18, 0xFFFFFF));
        Del(p);
    }

    /* cards */
    for (int i = 0; i < m->ncards; ++i) {
        const RECT *c = &m->cards[i];
        REAL x = (REAL)c->left, y = (REAL)c->top, cw = (REAL)(c->right - c->left), ch = (REAL)(c->bottom - c->top);
        REAL r = chH * 0.30f;
        p = RRect(x - 1, y, cw + 2, ch + 3, r + 1); FillSolid(g, p, A(0x0C, 0x1E395B)); Del(p);
        p = RRect(x, y + 1, cw, ch + 1, r);         FillSolid(g, p, A(0x10, 0x1E395B)); Del(p);
        p = RRect(x, y, cw, ch, r);
        FillSolid(g, p, O(0xFFFFFF));
        FillGloss(g, p, x, y, cw, (REAL)chH, O(0xFFFFFF), O(0xF1F6FC), O(0xE2ECF8), O(0xD4E3F4));
        Del(p);
        HLine(g, x, x + cw, y + chH - 0.5f, O(0xB4CBE5));
        p = RRect(x + 0.5f, y + 0.5f, cw - 1, ch - 1, r);
        Stroke(g, p, O(0x9DB9D9), 1.0f);
        Del(p);
    }

    Footer(g, w, m->footerTop, h);
    if (g) GdipDeleteGraphics(g);

    /* ---- text & icon on top ---- */
    {
        int sz = H * 58 / 100;
        HICON ic = (HICON)LoadImageW(m->hInst, MAKEINTRESOURCEW(m->iconId), IMAGE_ICON, sz, sz, 0);
        if (ic) { DrawIconEx(dc, pad, (H - sz) / 2, ic, sz, sz, 0, NULL, DI_NORMAL); DestroyIcon(ic); }

        int tx = pad + sz + pad * 6 / 10;
        RECT r = { tx + 1, 1, px - pad, H * 57 / 100 + 1 };
        Text(dc, m->title, r, fTitle, RGB(0, 0, 0), DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS);
        OffsetRect(&r, -1, -1);
        Text(dc, m->title, r, fTitle, RGB(255, 255, 255), DT_SINGLELINE | DT_BOTTOM | DT_END_ELLIPSIS);
        RECT s = { tx, H * 58 / 100, px - pad, H };
        Text(dc, m->subtitle, s, fSub, RGB(0xA4, 0xB8, 0xCE), DT_SINGLELINE | DT_TOP | DT_END_ELLIPSIS);

        RECT t = { px + ph * 45 / 100 + orb + ph * 35 / 100, py, px + pw - ph * 50 / 100, py + ph };
        Text(dc, m->status, t, fPill, RGB(255, 255, 255), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    for (int i = 0; i < m->ncards; ++i) {
        RECT r = m->cards[i];
        r.left += chH * 55 / 100;
        r.bottom = r.top + chH;
        Text(dc, m->cardTitle[i], r, fCard, RGB(0x1E, 0x39, 0x5B), DT_SINGLELINE | DT_VCENTER);
    }

    DeleteObject(fTitle); DeleteObject(fSub); DeleteObject(fPill); DeleteObject(fCard);
}

/* ---- secondary dialogs: white content + glossy footer ---------------- */
void Th_PaintDialog(HDC dc, int w, int h, int footerTop, const RECT *instr, const WCHAR *instruction)
{
    GpGraphics *g = G(dc);
    GpPath *p = RRect(0, 0, (REAL)w, (REAL)footerTop, 0);
    FillSolid(g, p, O(0xFFFFFF));
    Del(p);
    Footer(g, w, footerTop, h);
    if (g) GdipDeleteGraphics(g);

    if (instr && instruction) {
        HFONT f = MakeFont((instr->bottom - instr->top) * 80 / 100, FW_NORMAL);
        Text(dc, instruction, *instr, f, RGB(0x00, 0x33, 0x99), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        DeleteObject(f);
    }
}

/* ---- push button ----------------------------------------------------- */
void Th_DrawButton(HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT fl)
{
    REAL x = (REAL)rc->left, y = (REAL)rc->top, w = (REAL)(rc->right - rc->left), h = (REAL)(rc->bottom - rc->top);
    REAL r = h * 0.14f;
    if (r < 2.5f) r = 2.5f;
    ARGB border, t0, t1, b0, b1, inner;

    if (fl & TH_DISABLED) {
        border = O(0xADB2B5); t0 = t1 = b0 = b1 = O(0xF4F4F4); inner = A(0x00, 0xFFFFFF);
    } else if (fl & TH_PRESSED) {
        border = O(0x2C628B); t0 = O(0xE5F4FC); t1 = O(0xC4E5F6); b0 = O(0x98D1EF); b1 = O(0x68B3DB); inner = A(0x50, 0x9ED5F2);
    } else if (fl & TH_HOT) {
        border = O(0x3C7FB1); t0 = O(0xEAF6FD); t1 = O(0xD9F0FC); b0 = O(0xBEE6FD); b1 = O(0xA7D9F5); inner = A(0xC0, 0xFFFFFF);
    } else if (fl & TH_DEFAULT) {
        border = O(0x3C7FB1); t0 = O(0xF2F7FC); t1 = O(0xE6EFF8); b0 = O(0xD3E3F2); b1 = O(0xC4D9EE); inner = A(0xC0, 0xA6D8F8);
    } else {
        border = O(0x707070); t0 = O(0xF2F2F2); t1 = O(0xEBEBEB); b0 = O(0xDDDDDD); b1 = O(0xCFCFCF); inner = A(0xB8, 0xFFFFFF);
    }

    GpGraphics *g = G(dc);
    GpPath *p = RRect(x, y, w, h, r);
    FillGloss(g, p, x, y, w, h, t0, t1, b0, b1);
    Del(p);
    p = RRect(x + 1.5f, y + 1.5f, w - 3, h - 3, r - 1);
    Stroke(g, p, inner, 1.0f);
    Del(p);
    p = RRect(x + 0.5f, y + 0.5f, w - 1, h - 1, r);
    Stroke(g, p, border, 1.0f);
    Del(p);
    if (g) GdipDeleteGraphics(g);

    RECT tr = *rc;
    Text(dc, text, tr, font, (fl & TH_DISABLED) ? RGB(0x83, 0x83, 0x83) : RGB(0, 0, 0),
         DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);

    if ((fl & TH_FOCUS) && (fl & TH_SHOWFOCUS)) {
        RECT f = *rc;
        InflateRect(&f, -3, -3);
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        DrawFocusRect(dc, &f);
    }
}

/* ---- check box ------------------------------------------------------- */
void Th_DrawCheckbox(HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT fl)
{
    int hgt = rc->bottom - rc->top;
    int s = hgt * 13 / 16;
    if (s < 11) s = 11;
    if (s > hgt) s = hgt;
    REAL bx = (REAL)rc->left, by = (REAL)(rc->top + (hgt - s) / 2), fs = (REAL)s;

    ARGB border, i0, i1, mark;
    if (fl & TH_DISABLED)      { border = O(0xBCBCBC); i0 = O(0xEAEAEA); i1 = O(0xF6F6F6); mark = O(0xA2A2A2); }
    else if (fl & TH_PRESSED)  { border = O(0x2C628B); i0 = O(0x8CCBEF); i1 = O(0xD7EFFB); mark = O(0x1F3F7A); }
    else if (fl & TH_HOT)      { border = O(0x3C7FB1); i0 = O(0xB4E1FC); i1 = O(0xEAF7FE); mark = O(0x22468A); }
    else                       { border = O(0x8E8F8F); i0 = O(0xD2D5DA); i1 = O(0xF7F8F9); mark = O(0x2E4580); }

    GpGraphics *g = G(dc);
    GpPath *p = RRect(bx, by, fs, fs, 1.5f);
    FillSolid(g, p, O(0xFFFFFF));
    Del(p);
    REAL in = fs * 0.2f;
    p = RRect(bx + in, by + in, fs - 2 * in, fs - 2 * in, 1.0f);
    FillV(g, p, bx + in, by + in, fs - 2 * in, by + fs - in, i0, i1);
    Del(p);
    p = RRect(bx + 0.5f, by + 0.5f, fs - 1, fs - 1, 1.5f);
    Stroke(g, p, border, 1.0f);
    Del(p);

    if (fl & TH_CHECKED) {
        GpPen *pen = NULL;
        GdipCreatePen1(mark, fs * 0.15f, 0, &pen);
        if (pen) {
            GdipSetPenLineJoin(pen, 2);
            GdipSetPenStartCap(pen, 2);
            GdipSetPenEndCap(pen, 2);
            GpPath *ck = NULL;
            GdipCreatePath(0, &ck);
            if (ck) {
                GdipAddPathLine(ck, bx + fs * 0.24f, by + fs * 0.52f, bx + fs * 0.43f, by + fs * 0.71f);
                GdipAddPathLine(ck, bx + fs * 0.43f, by + fs * 0.71f, bx + fs * 0.78f, by + fs * 0.28f);
                GdipDrawPath(g, pen, ck);
                GdipDeletePath(ck);
            }
            GdipDeletePen(pen);
        }
    }
    if (g) GdipDeleteGraphics(g);

    RECT tr = *rc;
    tr.left += s + s * 45 / 100;
    COLORREF tc = (fl & TH_DISABLED) ? RGB(0x8D, 0x8D, 0x8D) : RGB(0x1B, 0x1B, 0x1B);
    Text(dc, text, tr, font, tc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if ((fl & TH_FOCUS) && (fl & TH_SHOWFOCUS)) {
        RECT f = tr;
        HGDIOBJ o = SelectObject(dc, font);
        DrawTextW(dc, text, -1, &f, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        SelectObject(dc, o);
        int dy = ((tr.bottom - tr.top) - (f.bottom - f.top)) / 2;
        OffsetRect(&f, 0, dy);
        InflateRect(&f, 2, 0);
        if (f.right > rc->right) f.right = rc->right;
        SetTextColor(dc, RGB(0, 0, 0));
        SetBkColor(dc, RGB(255, 255, 255));
        DrawFocusRect(dc, &f);
    }
}
