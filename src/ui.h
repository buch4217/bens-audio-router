// ui.h - modernes, dark-mode-faehiges UI-Fundament fuer Bens Audio Router
// * Theme: Dunkel / Hell / Auto (folgt Windows), dunkle Titelleiste (DWM), dunkle Popup-Menues
// * Eigene Controls: BARBtn (flacher Button), BARSlider, BARToggle (Schalter), BARPanel (Scroll-Container)
#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

struct Theme {
    bool dark = true;
    COLORREF bg, surface, surfaceHi, press, border, text, dim, accent, accentHi, accentPress, danger, ok, warn;
    COLORREF canvasBg, grid, nodeBody, nodeText, nodeDim;
};
static Theme TH;
static HBRUSH g_brBg = nullptr, g_brSurface = nullptr;
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT g_fUI, g_fBold, g_fMono, g_fSmall;  // Controls (Geraetepixel)
static HFONT g_cUI, g_cBold, g_cMono, g_cSmall;  // Canvas (96-dpi-Einheiten)

static void CreateFonts() {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTW lf = ncm.lfMessageFont;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    g_fUI = CreateFontIndirectW(&lf);
    LOGFONTW lb = lf;
    lb.lfWeight = FW_SEMIBOLD;
    g_fBold = CreateFontIndirectW(&lb);
    LOGFONTW lm = lf;
    wcscpy(lm.lfFaceName, L"Consolas");
    g_fMono = CreateFontIndirectW(&lm);
    LOGFONTW ls = lf;
    ls.lfHeight = (LONG)(lf.lfHeight * 0.88);
    g_fSmall = CreateFontIndirectW(&ls);
    g_cUI = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_cBold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_cMono = CreateFontW(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g_cSmall = CreateFontW(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
}

static bool SystemUsesDark() {
    DWORD v = 1, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

static void SetThemeColors(bool dark) {
    TH.dark = dark;
    if (dark) {
        TH.bg = RGB(24, 26, 32);
        TH.surface = RGB(38, 42, 52);
        TH.surfaceHi = RGB(50, 55, 68);
        TH.press = RGB(30, 33, 41);
        TH.border = RGB(58, 64, 78);
        TH.text = RGB(230, 233, 240);
        TH.dim = RGB(139, 147, 165);
        TH.accent = RGB(76, 141, 255);
        TH.accentHi = RGB(104, 160, 255);
        TH.accentPress = RGB(54, 112, 220);
        TH.danger = RGB(229, 72, 77);
        TH.ok = RGB(63, 185, 122);
        TH.warn = RGB(232, 168, 56);
        TH.canvasBg = RGB(15, 17, 22);
        TH.grid = RGB(36, 40, 50);
        TH.nodeBody = RGB(28, 32, 41);
        TH.nodeText = RGB(235, 240, 248);
        TH.nodeDim = RGB(140, 150, 168);
    } else {
        TH.bg = RGB(240, 242, 246);
        TH.surface = RGB(255, 255, 255);
        TH.surfaceHi = RGB(230, 234, 242);
        TH.press = RGB(214, 220, 232);
        TH.border = RGB(205, 211, 224);
        TH.text = RGB(26, 30, 38);
        TH.dim = RGB(96, 105, 122);
        TH.accent = RGB(0, 103, 224);
        TH.accentHi = RGB(30, 126, 240);
        TH.accentPress = RGB(0, 84, 190);
        TH.danger = RGB(210, 45, 50);
        TH.ok = RGB(22, 150, 90);
        TH.warn = RGB(200, 130, 10);
        TH.canvasBg = RGB(226, 230, 238);
        TH.grid = RGB(206, 212, 224);
        TH.nodeBody = RGB(255, 255, 255);
        TH.nodeText = RGB(26, 30, 38);
        TH.nodeDim = RGB(100, 110, 128);
    }
    if (g_brBg) DeleteObject(g_brBg);
    if (g_brSurface) DeleteObject(g_brSurface);
    g_brBg = CreateSolidBrush(TH.bg);
    g_brSurface = CreateSolidBrush(TH.surface);
}

typedef int(WINAPI* PFN_SetPreferredAppMode)(int);
typedef void(WINAPI* PFN_FlushMenuThemes)();

static void ApplyDarkMenus(bool dark) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    auto setMode = (PFN_SetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (PFN_FlushMenuThemes)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(dark ? 2 : 3);  // ForceDark / ForceLight
    if (flush) flush();
}

static void ApplyWindowChrome(HWND h) {
    BOOL d = TH.dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &d, sizeof(d)))) DwmSetWindowAttribute(h, 19, &d, sizeof(d));
    COLORREF cap = TH.bg, txt = TH.text, bord = TH.border;
    DwmSetWindowAttribute(h, 35, &cap, sizeof(cap));   // Titelleistenfarbe (Win11)
    DwmSetWindowAttribute(h, 36, &txt, sizeof(txt));   // Titeltext
    DwmSetWindowAttribute(h, 34, &bord, sizeof(bord)); // Rahmen
}

// type: 0 Edit, 1 Combo, 2 Scroll/Liste
static void ThemeNative(HWND h, int type) {
    if (TH.dark) SetWindowTheme(h, type == 2 ? L"DarkMode_Explorer" : L"DarkMode_CFD", nullptr);
    else SetWindowTheme(h, type == 2 ? L"Explorer" : nullptr, nullptr);
}

static void RoundFill(HDC dc, RECT r, int rad, COLORREF fill, COLORREF line) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void FillRectColor(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// ---------------------------------------------------------------- Button

enum { BTN_ACCENT = 1, BTN_DANGER = 2 };
static const UINT BTM_SETFLAGS = WM_USER + 20;

struct BtnData {
    bool hover = false, down = false, track = false, focus = false;
    int flags = 0;
};

static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    BtnData* d = (BtnData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        d = new BtnData();
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        return DefWindowProcW(h, m, w, l);
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case BTM_SETFLAGS:
        d->flags = (int)w;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_GETDLGCODE:
        return DLGC_BUTTON;
    case WM_SETFOCUS: d->focus = true; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_KILLFOCUS: d->focus = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ENABLE:
    case WM_SETTEXT:
        DefWindowProcW(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!d->track) {
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            d->track = true;
        }
        if (!d->hover) { d->hover = true; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSELEAVE:
        d->hover = false;
        d->track = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        if (IsWindowEnabled(h)) { SetFocus(h); SetCapture(h); d->down = true; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (d->down) {
            ReleaseCapture();
            d->down = false;
            InvalidateRect(h, nullptr, FALSE);
            RECT rc;
            GetClientRect(h, &rc);
            POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (PtInRect(&rc, pt))
                PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_SPACE || w == VK_RETURN) {
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bm = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bm);
        FillRectColor(dc, rc, TH.bg);
        bool en = IsWindowEnabled(h);
        COLORREF fill, line, txt;
        if (d->flags & BTN_ACCENT) {
            fill = !en ? TH.surface : d->down ? TH.accentPress : d->hover ? TH.accentHi : TH.accent;
            line = fill;
            txt = en ? RGB(255, 255, 255) : TH.dim;
        } else {
            fill = !en ? TH.bg : d->down ? TH.press : d->hover ? TH.surfaceHi : TH.surface;
            line = d->focus ? TH.accent : TH.border;
            txt = !en ? TH.dim : (d->flags & BTN_DANGER) ? TH.danger : TH.text;
        }
        RECT rr = rc;
        rr.right--;
        rr.bottom--;
        RoundFill(dc, rr, S(6), fill, line);
        wchar_t t[128];
        GetWindowTextW(h, t, 128);
        SelectObject(dc, (d->flags & BTN_ACCENT) ? g_fBold : g_fUI);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, txt);
        DrawTextW(dc, t, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bm);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- Slider

static const UINT SLM_SETRANGE = WM_USER + 30, SLM_SETPOS = WM_USER + 31, SLM_GETPOS = WM_USER + 32, SLM_SETTICK = WM_USER + 33;
static const int SLN_CHANGING = 0x1001, SLN_DONE = 0x1002;

struct SlData {
    int mn = 0, mx = 100, pos = 0, tick = 0;
    bool hover = false, drag = false, track = false;
};

static int SlFromX(HWND h, SlData* d, int x) {
    RECT rc;
    GetClientRect(h, &rc);
    int mg = S(9);
    double t = (double)(x - mg) / (double)std::max(1, (int)(rc.right - 2 * mg));
    t = std::max(0.0, std::min(1.0, t));
    return d->mn + (int)std::lround(t * (d->mx - d->mn));
}

static void SlNotify(HWND h, int code) {
    PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), code), (LPARAM)h);
}

static LRESULT CALLBACK SliderProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    SlData* d = (SlData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        d = new SlData();
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        return DefWindowProcW(h, m, w, l);
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case SLM_SETRANGE: d->mn = (int)w; d->mx = (int)l; d->pos = std::max(d->mn, std::min(d->mx, d->pos)); InvalidateRect(h, nullptr, FALSE); return 0;
    case SLM_SETPOS: d->pos = std::max(d->mn, std::min(d->mx, (int)w)); InvalidateRect(h, nullptr, FALSE); return 0;
    case SLM_GETPOS: return d->pos;
    case SLM_SETTICK: d->tick = (int)w; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!d->track) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); d->track = true; }
        if (!d->hover) { d->hover = true; InvalidateRect(h, nullptr, FALSE); }
        if (d->drag) {
            int p = SlFromX(h, d, GET_X_LPARAM(l));
            if (p != d->pos) { d->pos = p; InvalidateRect(h, nullptr, FALSE); SlNotify(h, SLN_CHANGING); }
        }
        return 0;
    case WM_MOUSELEAVE: d->hover = false; d->track = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        if (IsWindowEnabled(h)) {
            SetFocus(h);
            SetCapture(h);
            d->drag = true;
            int p = SlFromX(h, d, GET_X_LPARAM(l));
            d->pos = p;
            InvalidateRect(h, nullptr, FALSE);
            SlNotify(h, SLN_CHANGING);
        }
        return 0;
    case WM_LBUTTONUP:
        if (d->drag) { d->drag = false; ReleaseCapture(); InvalidateRect(h, nullptr, FALSE); SlNotify(h, SLN_DONE); }
        return 0;
    case WM_KEYDOWN: {
        int step = (GetKeyState(VK_CONTROL) & 0x8000) ? 10 : 1;
        int range = d->mx - d->mn;
        int np = d->pos;
        if (w == VK_LEFT || w == VK_DOWN) np -= step;
        else if (w == VK_RIGHT || w == VK_UP) np += step;
        else if (w == VK_PRIOR) np += std::max(1, range / 10);
        else if (w == VK_NEXT) np -= std::max(1, range / 10);
        else if (w == VK_HOME) np = d->mn;
        else if (w == VK_END) np = d->mx;
        else break;
        np = std::max(d->mn, std::min(d->mx, np));
        if (np != d->pos) { d->pos = np; InvalidateRect(h, nullptr, FALSE); SlNotify(h, SLN_CHANGING); SlNotify(h, SLN_DONE); }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bm = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bm);
        FillRectColor(dc, rc, TH.bg);
        bool en = IsWindowEnabled(h);
        int mg = S(9), cy = rc.bottom / 2, th = S(4);
        int x0 = mg, x1 = rc.right - mg;
        RECT groove = {x0, cy - th / 2, x1, cy + th / 2 + 1};
        RoundFill(dc, groove, th / 2, en ? TH.surfaceHi : TH.surface, en ? TH.surfaceHi : TH.surface);
        double range = (double)std::max(1, d->mx - d->mn);
        int px = x0 + (int)((d->pos - d->mn) / range * (x1 - x0));
        int zx = x0;
        if (d->mn < 0 && d->mx > 0) zx = x0 + (int)((0 - d->mn) / range * (x1 - x0));
        RECT fill = {std::min(zx, px), cy - th / 2, std::max(zx, px) + 1, cy + th / 2 + 1};
        if (fill.right > fill.left) RoundFill(dc, fill, th / 2, en ? TH.accent : TH.border, en ? TH.accent : TH.border);
        if (d->mn < 0 && d->mx > 0) {
            HPEN zp = CreatePen(PS_SOLID, 1, TH.dim);
            HGDIOBJ oz = SelectObject(dc, zp);
            MoveToEx(dc, zx, cy - S(7), nullptr);
            LineTo(dc, zx, cy + S(8));
            SelectObject(dc, oz);
            DeleteObject(zp);
        }
        int r = S(d->hover || d->drag ? 8 : 7);
        HBRUSH tb = CreateSolidBrush(en ? (d->drag ? TH.accentPress : TH.accent) : TH.border);
        HPEN tp = CreatePen(PS_SOLID, S(2), TH.bg);
        HGDIOBJ otb = SelectObject(dc, tb), otp = SelectObject(dc, tp);
        Ellipse(dc, px - r, cy - r, px + r + 1, cy + r + 1);
        SelectObject(dc, otb);
        SelectObject(dc, otp);
        DeleteObject(tb);
        DeleteObject(tp);
        if (GetFocus() == h) {
            HPEN fp = CreatePen(PS_DOT, 1, TH.accent);
            HGDIOBJ of = SelectObject(dc, fp), onb = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, rc.right, rc.bottom);
            SelectObject(dc, of);
            SelectObject(dc, onb);
            DeleteObject(fp);
        }
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bm);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- Toggle

static const UINT TGM_SET = WM_USER + 40, TGM_GET = WM_USER + 41;
struct TgData {
    bool on = false, hover = false, track = false;
};

static LRESULT CALLBACK ToggleProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    TgData* d = (TgData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_NCCREATE:
        d = new TgData();
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        return DefWindowProcW(h, m, w, l);
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case TGM_SET: d->on = w != 0; InvalidateRect(h, nullptr, FALSE); return 0;
    case TGM_GET: return d->on ? 1 : 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETDLGCODE: return DLGC_BUTTON;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SETTEXT:
        DefWindowProcW(h, m, w, l);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (!d->track) { TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); d->track = true; }
        if (!d->hover) { d->hover = true; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_MOUSELEAVE: d->hover = false; d->track = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN:
        if (IsWindowEnabled(h)) {
            SetFocus(h);
            d->on = !d->on;
            InvalidateRect(h, nullptr, FALSE);
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_SPACE && IsWindowEnabled(h)) {
            d->on = !d->on;
            InvalidateRect(h, nullptr, FALSE);
            PostMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(h), BN_CLICKED), (LPARAM)h);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bm = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(dc, bm);
        FillRectColor(dc, rc, TH.bg);
        bool en = IsWindowEnabled(h);
        int tw = S(34), th = S(18), cy = rc.bottom / 2;
        RECT tr = {0, cy - th / 2, tw, cy + th / 2};
        COLORREF tc = d->on ? (en ? (d->hover ? TH.accentHi : TH.accent) : TH.border) : (d->hover ? TH.surfaceHi : TH.surface);
        RoundFill(dc, tr, th / 2, tc, d->on ? tc : TH.border);
        int kr = th / 2 - S(3);
        int kx = d->on ? tw - th / 2 : th / 2;
        HBRUSH kb = CreateSolidBrush(d->on ? RGB(255, 255, 255) : TH.dim);
        HPEN kp = CreatePen(PS_NULL, 0, 0);
        HGDIOBJ okb = SelectObject(dc, kb), okp = SelectObject(dc, kp);
        Ellipse(dc, kx - kr, cy - kr, kx + kr + 1, cy + kr + 1);
        SelectObject(dc, okb);
        SelectObject(dc, okp);
        DeleteObject(kb);
        DeleteObject(kp);
        wchar_t t[160];
        GetWindowTextW(h, t, 160);
        RECT lr = {tw + S(8), 0, rc.right, rc.bottom};
        SelectObject(dc, g_fUI);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, en ? TH.text : TH.dim);
        DrawTextW(dc, t, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (GetFocus() == h) {
            HPEN fp = CreatePen(PS_DOT, 1, TH.accent);
            HGDIOBJ of = SelectObject(dc, fp), onb = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, 0, 0, rc.right, rc.bottom);
            SelectObject(dc, of);
            SelectObject(dc, onb);
            DeleteObject(fp);
        }
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bm);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------- Panel (Scroll-Container fuer den Inspector)

static LRESULT CALLBACK PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, g_brBg);
        return 1;
    }
    case WM_COMMAND:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return SendMessageW(GetParent(h), m, w, l);
    case WM_MOUSEWHEEL:
        return SendMessageW(GetParent(h), m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

static void RegisterModernControls(HINSTANCE hi) {
    WNDCLASSEXW c = {};
    c.cbSize = sizeof(c);
    c.hInstance = hi;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.lpfnWndProc = BtnProc;
    c.lpszClassName = L"BARBtn";
    RegisterClassExW(&c);
    c.lpfnWndProc = SliderProc;
    c.lpszClassName = L"BARSlider";
    RegisterClassExW(&c);
    c.lpfnWndProc = ToggleProc;
    c.lpszClassName = L"BARToggle";
    c.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&c);
    c.lpfnWndProc = PanelProc;
    c.lpszClassName = L"BARPanel";
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&c);
}
