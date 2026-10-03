/* main.c - application shell: main window, layout, activity bar, status bar,
 * icons, commands, keyboard shortcuts and session persistence. */
#include "xpcode.h"

HINSTANCE g_hinst;
HWND g_hwndMain, g_hwndActivity, g_hwndSidebar, g_hwndEditor, g_hwndPanel, g_hwndStatus, g_hwndPalette;
HFONT g_fUI, g_fUIBold, g_fUISmall, g_fCode, g_fTitle, g_fHeading, g_fCodeSmall;
int g_codeCW, g_codeCH, g_codeSize = 14;
char g_root[MAX_PATH];
int g_sidebarVisible = 1, g_panelVisible = 1, g_sideView = 0, g_minimap = 1;
char g_exeDir[MAX_PATH];

static int s_sideW = 250, s_panelH = 230;
static int s_drag;               /* 0 none, 1 sidebar sash, 2 panel sash */
static RECT s_sashSide, s_sashPanel;
static char s_ini[MAX_PATH];
static int s_chordK;             /* Ctrl+K pressed, waiting for second key */

#define ACTIVITY_W 48
#define STATUS_H   22
#define SASH       3

/* ================================================================== utils */

void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r; SetRect(&r, x, y, x + w, y + h);
    SetBkColor(dc, c);
    ExtTextOut(dc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
}

void fill_rc(HDC dc, const RECT *r, COLORREF c)
{
    SetBkColor(dc, c);
    ExtTextOut(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}

void hline(HDC dc, int x, int y, int w, COLORREF c) { fill(dc, x, y, w, 1, c); }
void vline(HDC dc, int x, int y, int h, COLORREF c) { fill(dc, x, y, 1, h, c); }

void frame(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    hline(dc, x, y, w, c); hline(dc, x, y + h - 1, w, c);
    vline(dc, x, y, h, c); vline(dc, x + w - 1, y, h, c);
}

void text_at(HDC dc, int x, int y, const char *s, int n, COLORREF c, HFONT f)
{
    HFONT old = (HFONT)SelectObject(dc, f);
    if (n < 0) n = (int)strlen(s);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOut(dc, x, y, s, n);
    SelectObject(dc, old);
}

int text_w(HDC dc, HFONT f, const char *s, int n)
{
    SIZE sz; HFONT old = (HFONT)SelectObject(dc, f);
    if (n < 0) n = (int)strlen(s);
    GetTextExtentPoint32(dc, s, n, &sz);
    SelectObject(dc, old);
    return sz.cx;
}

void text_ellipsis(HDC dc, int x, int y, int maxw, const char *s, COLORREF c, HFONT f)
{
    char buf[512]; int n = (int)strlen(s);
    if (maxw <= 0) return;
    if (text_w(dc, f, s, n) <= maxw) { text_at(dc, x, y, s, n, c, f); return; }
    if (n > 500) n = 500;
    while (n > 0) {
        memcpy(buf, s, n); strcpy(buf + n, "...");
        if (text_w(dc, f, buf, n + 3) <= maxw) break;
        n--;
    }
    text_at(dc, x, y, buf, -1, c, f);
}

int sfmt(char *buf, int n, const char *f, ...)
{
    va_list ap; int r;
    va_start(ap, f);
    r = _vsnprintf(buf, n, f, ap);
    va_end(ap);
    buf[n - 1] = 0;
    if (r < 0 || r >= n) r = (int)strlen(buf);
    return r;
}

const char *path_name(const char *p)
{
    const char *s = p + strlen(p);
    while (s > p && s[-1] != '\\' && s[-1] != '/') s--;
    return s;
}

const char *path_ext(const char *p)
{
    const char *n = path_name(p), *d = strrchr(n, '.');
    return d ? d + 1 : "";
}

int path_rel(const char *full, char *out, int n)
{
    int rl = (int)strlen(g_root);
    if (rl && _strnicmp(full, g_root, rl) == 0 && (full[rl] == '\\' || full[rl] == '/')) {
        sfmt(out, n, "%s", full + rl + 1);
        return 1;
    }
    sfmt(out, n, "%s", full);
    return 0;
}

int file_exists(const char *p)
{
    DWORD a = GetFileAttributes(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int dir_exists(const char *p)
{
    DWORD a = GetFileAttributes(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int lang_from_path(const char *p)
{
    const char *e = path_ext(p);
    if (!_stricmp(e, "c") || !_stricmp(e, "h") || !_stricmp(e, "cpp") || !_stricmp(e, "hpp") ||
        !_stricmp(e, "cc") || !_stricmp(e, "java") || !_stricmp(e, "cs")) return LANG_C;
    if (!_stricmp(e, "py") || !_stricmp(e, "pyw")) return LANG_PY;
    if (!_stricmp(e, "js") || !_stricmp(e, "json") || !_stricmp(e, "ts")) return LANG_JS;
    if (!_stricmp(e, "bat") || !_stricmp(e, "cmd")) return LANG_BAT;
    if (!_stricmp(e, "ini") || !_stricmp(e, "cfg") || !_stricmp(e, "inf")) return LANG_INI;
    if (!_stricmp(e, "html") || !_stricmp(e, "htm") || !_stricmp(e, "xml")) return LANG_HTML;
    if (!_stricmp(e, "md")) return LANG_MD;
    return LANG_PLAIN;
}

const char *lang_name(int lang)
{
    static const char *names[] = { "Plain Text", "C", "Python", "JavaScript", "Batch", "Ini", "HTML", "Markdown" };
    return names[lang];
}

char *read_file(const char *path, int *len)
{
    HANDLE h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0; char *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    buf = (char *)malloc(size + 1);
    if (buf && !ReadFile(h, buf, size, &got, NULL)) got = 0;
    CloseHandle(h);
    if (!buf) return NULL;
    buf[got] = 0;
    if (len) *len = (int)got;
    return buf;
}

int fuzzy_score(const char *pat, const char *s)
{
    int score = 0, run = 0, pi = 0, i, n = (int)strlen(s), plen = (int)strlen(pat);
    if (!plen) return 1;
    for (i = 0; i < n && pi < plen; i++) {
        if (tolower((unsigned char)s[i]) == tolower((unsigned char)pat[pi])) {
            int bonus = 1 + run * 3;
            if (i == 0 || strchr("\\/_-. ", s[i - 1])) bonus += 6;
            score += bonus; run++; pi++;
        } else run = 0;
    }
    if (pi < plen) return -1;
    return score * 100 / (n + 10) + score;
}

void out_log(const char *f, ...)
{
    char buf[1024]; va_list ap; SYSTEMTIME t; char line[1100];
    va_start(ap, f); _vsnprintf(buf, sizeof buf, f, ap); va_end(ap); buf[sizeof buf - 1] = 0;
    GetLocalTime(&t);
    sfmt(line, sizeof line, "[%02d:%02d:%02d] %s", t.wHour, t.wMinute, t.wSecond, buf);
    panel_output(line);
}

/* ================================================================== icons */

static int IX, IY, IS;
#define GX(v) (IX + (((v) - 8) * IS) / 16)
#define GY(v) (IY + (((v) - 8) * IS) / 16)
static void L(HDC dc, int x1, int y1, int x2, int y2) { MoveToEx(dc, GX(x1), GY(y1), NULL); LineTo(dc, GX(x2), GY(y2)); }
static void PL(HDC dc, const int *pts, int n)
{
    int i; MoveToEx(dc, GX(pts[0]), GY(pts[1]), NULL);
    for (i = 1; i < n; i++) LineTo(dc, GX(pts[i * 2]), GY(pts[i * 2 + 1]));
}
static void R(HDC dc, int x1, int y1, int x2, int y2) { Rectangle(dc, GX(x1), GY(y1), GX(x2) + 1, GY(y2) + 1); }
static void E(HDC dc, int x1, int y1, int x2, int y2) { Ellipse(dc, GX(x1), GY(y1), GX(x2) + 1, GY(y2) + 1); }

void draw_icon(HDC dc, int icon, int cx, int cy, int sz, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, sz >= 24 ? 2 : 1, c), op = (HPEN)SelectObject(dc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    HBRUSH solid = NULL;
    IX = cx; IY = cy; IS = sz;
    switch (icon) {
    case ICON_FILES: {
        int p[] = { 5,4, 10,4, 13,7, 13,15, 5,15, 5,4 }; PL(dc, p, 6);
        L(dc, 10, 4, 10, 7); L(dc, 10, 7, 13, 7);
        L(dc, 2, 12, 2, 1); L(dc, 2, 1, 9, 1);
        break; }
    case ICON_SEARCH:
        E(dc, 5, 1, 15, 11); L(dc, 6, 10, 1, 15); L(dc, 7, 10, 2, 15); break;
    case ICON_RUN: {
        int p[] = { 4,2, 13,8, 4,14, 4,2 }; PL(dc, p, 4);
        break; }
    case ICON_GEAR: {
        static const int d[8][2] = { {10,0},{7,7},{0,10},{-7,7},{-10,0},{-7,-7},{0,-10},{7,-7} };
        int i; E(dc, 5, 5, 11, 11);
        for (i = 0; i < 8; i++) {
            MoveToEx(dc, cx + d[i][0] * IS * 5 / 160, cy + d[i][1] * IS * 5 / 160, NULL);
            LineTo(dc, cx + d[i][0] * IS * 7 / 160, cy + d[i][1] * IS * 7 / 160);
        }
        break; }
    case ICON_CHEVRON_RIGHT: { int p[] = { 6,4, 10,8, 6,12 }; PL(dc, p, 3); break; }
    case ICON_CHEVRON_DOWN:  { int p[] = { 4,6, 8,10, 12,6 }; PL(dc, p, 3); break; }
    case ICON_CHEVRON_UP:    { int p[] = { 4,10, 8,6, 12,10 }; PL(dc, p, 3); break; }
    case ICON_CLOSE: L(dc, 4, 4, 12, 12); L(dc, 12, 4, 4, 12); break;
    case ICON_PLUS: L(dc, 8, 3, 8, 13); L(dc, 3, 8, 13, 8); break;
    case ICON_TRASH: {
        int p[] = { 4,5, 5,14, 11,14, 12,5 }; PL(dc, p, 4);
        L(dc, 2, 4, 14, 4); L(dc, 6, 4, 6, 2); L(dc, 6, 2, 10, 2); L(dc, 10, 2, 10, 4);
        L(dc, 7, 7, 7, 12); L(dc, 9, 7, 9, 12); break; }
    case ICON_NEW_FILE: {
        int p[] = { 9,14, 2,14, 2,1, 8,1, 11,4, 11,8 }; PL(dc, p, 6);
        L(dc, 13, 9, 13, 15); L(dc, 10, 12, 16, 12); break; }
    case ICON_NEW_FOLDER: {
        int p[] = { 9,13, 1,13, 1,3, 6,3, 7,5, 14,5, 14,8 }; PL(dc, p, 7);
        L(dc, 13, 9, 13, 15); L(dc, 10, 12, 16, 12); break; }
    case ICON_REFRESH:
        Arc(dc, GX(2), GY(2), GX(14), GY(14), GX(13), GY(3), GX(14), GY(8));
        L(dc, 9, 3, 13, 3); L(dc, 13, 3, 13, 0); break;
    case ICON_COLLAPSE:
        R(dc, 2, 4, 12, 14); L(dc, 4, 2, 14, 2); L(dc, 14, 2, 14, 12); L(dc, 5, 9, 10, 9); break;
    case ICON_ERROR:
        E(dc, 1, 1, 15, 15); L(dc, 5, 5, 11, 11); L(dc, 11, 5, 5, 11); break;
    case ICON_WARNING: {
        int p[] = { 8,1, 15,14, 1,14, 8,1 }; PL(dc, p, 4);
        L(dc, 8, 5, 8, 10); L(dc, 8, 11, 8, 13); break; }
    case ICON_DOT:
        solid = CreateSolidBrush(c); SelectObject(dc, solid); E(dc, 4, 4, 12, 12); break;
    case ICON_SPLIT: R(dc, 1, 2, 15, 14); L(dc, 8, 2, 8, 14); break;
    case ICON_ELLIPSIS:
        solid = CreateSolidBrush(c); SelectObject(dc, solid);
        E(dc, 2, 7, 4, 9); E(dc, 7, 7, 9, 9); E(dc, 12, 7, 14, 9); break;
    case ICON_ARROW_UP:   L(dc, 8, 14, 8, 2); L(dc, 3, 7, 8, 2); L(dc, 8, 2, 13, 7); break;
    case ICON_ARROW_DOWN: L(dc, 8, 2, 8, 14); L(dc, 3, 9, 8, 14); L(dc, 8, 14, 13, 9); break;
    case ICON_TERMINAL:
        R(dc, 1, 2, 15, 14); { int p[] = { 4,6, 6,8, 4,10 }; PL(dc, p, 3); } L(dc, 8, 10, 12, 10); break;
    case ICON_PLAY: {
        POINT pt[3]; solid = CreateSolidBrush(c); SelectObject(dc, solid);
        pt[0].x = GX(4); pt[0].y = GY(2); pt[1].x = GX(13); pt[1].y = GY(8); pt[2].x = GX(4); pt[2].y = GY(14);
        Polygon(dc, pt, 3); break; }
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
    if (solid) DeleteObject(solid);
}

/* small coloured file-type badge, like the Seti icon theme */
void file_badge(HDC dc, const char *name, int isdir, int open, int x, int y)
{
    const char *e = path_ext(name), *t = NULL; COLORREF c = HEX(0x9DA5B4);
    if (isdir) { draw_icon(dc, open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, x + 8, y, 16, C_SIDEBAR_TEXT); return; }
    if (!_stricmp(e, "c"))        { t = "C";  c = HEX(0x519ABA); }
    else if (!_stricmp(e, "h"))   { t = "h";  c = HEX(0xA074C4); }
    else if (!_stricmp(e, "cpp")) { t = "C+"; c = HEX(0x519ABA); }
    else if (!_stricmp(e, "py"))  { t = "py"; c = HEX(0xE8C547); }
    else if (!_stricmp(e, "js"))  { t = "JS"; c = HEX(0xCBCB41); }
    else if (!_stricmp(e, "json")){ t = "{}"; c = HEX(0xCBCB41); }
    else if (!_stricmp(e, "bat") || !_stricmp(e, "cmd")) { t = ">"; c = HEX(0x8DC149); }
    else if (!_stricmp(e, "md"))  { t = "M";  c = HEX(0x519ABA); }
    else if (!_stricmp(e, "ini") || !_stricmp(e, "cfg")) { t = "*"; c = HEX(0x6D8086); }
    else if (!_stricmp(e, "exe") || !_stricmp(e, "dll")) { t = "#"; c = HEX(0x9F9F9F); }
    else if (!_stricmp(e, "html") || !_stricmp(e, "htm")) { t = "<>"; c = HEX(0xE37933); }
    if (t) {
        text_at(dc, x + 8 - text_w(dc, g_fUIBold, t, -1) / 2, y - 7, t, -1, c, g_fUIBold);
    } else {
        hline(dc, x + 4, y - 3, 9, c); hline(dc, x + 4, y, 9, c); hline(dc, x + 4, y + 3, 6, c);
    }
}

/* ================================================================== activity bar */

static int s_actHover = -1;
static const int s_actIcons[] = { ICON_FILES, ICON_SEARCH, ICON_RUN };
#define NVIEWS 3

static int activity_hit(int y, int h)
{
    if (y < NVIEWS * 48) return y / 48;
    if (y >= h - 48) return 6;           /* gear */
    return -1;
}

static LRESULT CALLBACK ActivityProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; RECT rc; HDC dc = BeginPaint(w, &ps), mem; HBITMAP bmp; int i;
        GetClientRect(w, &rc);
        mem = CreateCompatibleDC(dc); bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        SelectObject(mem, bmp);
        fill_rc(mem, &rc, C_ACTIVITY_BG);
        for (i = 0; i < NVIEWS; i++) {
            int active = g_sidebarVisible && g_sideView == i;
            COLORREF c = active || s_actHover == i ? C_TEXT_BRIGHT : HEX(0x858585);
            if (active) fill(mem, 0, i * 48, 2, 48, C_TEXT_BRIGHT);
            draw_icon(mem, s_actIcons[i], 24, i * 48 + 24, 24, c);
        }
        draw_icon(mem, ICON_GEAR, 24, rc.bottom - 24, 24, s_actHover == 6 ? C_TEXT_BRIGHT : HEX(0x858585));
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        RECT rc; int h; TRACKMOUSEEVENT t;
        GetClientRect(w, &rc); h = activity_hit(GET_Y_LPARAM(lp), rc.bottom);
        if (h != s_actHover) { s_actHover = h; InvalidateRect(w, NULL, FALSE); }
        t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = w; t.dwHoverTime = 0; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: s_actHover = -1; InvalidateRect(w, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        RECT rc; int h; GetClientRect(w, &rc); h = activity_hit(GET_Y_LPARAM(lp), rc.bottom);
        if (h >= 0 && h < NVIEWS) {
            if (g_sidebarVisible && g_sideView == h) app_command(CMD_TOGGLE_SIDEBAR);
            else app_command(CMD_SHOW_EXPLORER + h);
        } else if (h == 6) {
            HMENU menu = CreatePopupMenu(); POINT pt; int cmd;
            AppendMenu(menu, MF_STRING, CMD_PALETTE, "Command Palette...\tCtrl+Shift+P");
            AppendMenu(menu, MF_STRING, CMD_SHORTCUTS, "Keyboard Shortcuts");
            AppendMenu(menu, MF_SEPARATOR, 0, NULL);
            AppendMenu(menu, MF_STRING, CMD_ZOOM_IN, "Zoom In\tCtrl+=");
            AppendMenu(menu, MF_STRING, CMD_ZOOM_OUT, "Zoom Out\tCtrl+-");
            AppendMenu(menu, MF_STRING | (g_minimap ? MF_CHECKED : 0), CMD_TOGGLE_MINIMAP, "Minimap");
            AppendMenu(menu, MF_SEPARATOR, 0, NULL);
            AppendMenu(menu, MF_STRING, CMD_ABOUT, "About " APP_NAME);
            pt.x = 48; pt.y = rc.bottom - 10; ClientToScreen(w, &pt);
            cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_BOTTOMALIGN, pt.x, pt.y, 0, w, NULL);
            DestroyMenu(menu);
            if (cmd) app_command(cmd);
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(w, m, wp, lp);
}

void activity_register(void)
{
    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = ActivityProc; wc.hInstance = g_hinst; wc.lpszClassName = "XPActivity";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}

/* ================================================================== status bar */

typedef struct { RECT r; int cmd; } StatusItem;
static StatusItem s_items[12];
static int s_nitems, s_statusHover = -1;

static int status_item(HDC dc, int *x, int right, const char *text, int icon, int cmd, int hover)
{
    int w = text_w(dc, g_fUI, text, -1) + 16 + (icon >= 0 ? 18 : 0), ix;
    StatusItem *it = &s_items[s_nitems];
    if (right) *x -= w;
    SetRect(&it->r, *x, 0, *x + w, STATUS_H);
    it->cmd = cmd;
    if (hover == s_nitems && cmd) fill_rc(dc, &it->r, HEX(0x1F8AD2));
    ix = *x + 8;
    if (icon >= 0) { draw_icon(dc, icon, ix + 7, STATUS_H / 2, 14, C_TEXT_BRIGHT); ix += 18; }
    text_at(dc, ix, 4, text, -1, C_TEXT_BRIGHT, g_fUI);
    if (!right) *x += w;
    s_nitems++;
    return w;
}

static LRESULT CALLBACK StatusProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; RECT rc; HDC dc = BeginPaint(w, &ps), mem; HBITMAP bmp;
        int x, line, col, lang, has, nl, errs, warns; char buf[128];
        GetClientRect(w, &rc);
        mem = CreateCompatibleDC(dc); bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        SelectObject(mem, bmp);
        fill_rc(mem, &rc, g_root[0] ? C_STATUS_BG : C_STATUS_NOFOLDER);
        s_nitems = 0;
        x = 0;
        x = 6;
        if (g_root[0]) status_item(mem, &x, 0, path_name(g_root), ICON_FILES, CMD_SHOW_EXPLORER, s_statusHover);
        problems_count(&errs, &warns);
        sfmt(buf, sizeof buf, "%d   ", errs);
        {
            int x0 = x; status_item(mem, &x, 0, "             ", -1, CMD_SHOW_PROBLEMS, s_statusHover);
            draw_icon(mem, ICON_ERROR, x0 + 15, STATUS_H / 2, 13, C_TEXT_BRIGHT);
            sfmt(buf, sizeof buf, "%d", errs); text_at(mem, x0 + 25, 4, buf, -1, C_TEXT_BRIGHT, g_fUI);
            draw_icon(mem, ICON_WARNING, x0 + 48, STATUS_H / 2, 13, C_TEXT_BRIGHT);
            sfmt(buf, sizeof buf, "%d", warns); text_at(mem, x0 + 58, 4, buf, -1, C_TEXT_BRIGHT, g_fUI);
        }
        x = rc.right;
        x -= 6;
        editor_status(&line, &col, &lang, &has, &nl);
        if (has) {
            status_item(mem, &x, 1, lang_name(lang), -1, CMD_NONE, s_statusHover);
            status_item(mem, &x, 1, "CRLF", -1, CMD_NONE, s_statusHover);
            status_item(mem, &x, 1, "Windows 1252", -1, CMD_NONE, s_statusHover);
            status_item(mem, &x, 1, "Spaces: 4", -1, CMD_NONE, s_statusHover);
            sfmt(buf, sizeof buf, "Ln %d, Col %d", line, col);
            status_item(mem, &x, 1, buf, -1, CMD_GOTO_LINE, s_statusHover);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        DeleteObject(bmp); DeleteDC(mem);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int i, h = -1; POINT p; TRACKMOUSEEVENT t;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        for (i = 0; i < s_nitems; i++) if (PtInRect(&s_items[i].r, p) && s_items[i].cmd) h = i;
        if (h != s_statusHover) { s_statusHover = h; InvalidateRect(w, NULL, FALSE); }
        t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = w; t.dwHoverTime = 0; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: s_statusHover = -1; InvalidateRect(w, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        int i; POINT p; p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        for (i = 0; i < s_nitems; i++) if (PtInRect(&s_items[i].r, p) && s_items[i].cmd) { app_command(s_items[i].cmd); break; }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(w, m, wp, lp);
}

void status_register(void)
{
    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = StatusProc; wc.hInstance = g_hinst; wc.lpszClassName = "XPStatus";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}

void status_refresh(void) { if (g_hwndStatus) InvalidateRect(g_hwndStatus, NULL, FALSE); }

/* ================================================================== fonts */

static HFONT mkfont(const char *face, int h, int weight)
{
    return CreateFont(-h, 0, 0, 0, weight, 0, 0, 0, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_DONTCARE, face);
}

static void make_code_font(void)
{
    HDC dc; TEXTMETRIC tm; HFONT old;
    if (g_fCode) DeleteObject(g_fCode);
    if (g_fCodeSmall) DeleteObject(g_fCodeSmall);
    g_fCode = mkfont("Lucida Console", g_codeSize, FW_NORMAL);
    g_fCodeSmall = mkfont("Lucida Console", 11, FW_NORMAL);
    dc = GetDC(NULL); old = (HFONT)SelectObject(dc, g_fCode);
    GetTextMetrics(dc, &tm);
    g_codeCW = tm.tmAveCharWidth; g_codeCH = tm.tmHeight + 5;
    SelectObject(dc, old); ReleaseDC(NULL, dc);
}

static void make_fonts(void)
{
    g_fUI = CreateFont(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Tahoma");
    g_fUIBold = CreateFont(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Tahoma");
    g_fUISmall = CreateFont(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Tahoma");
    g_fTitle = CreateFont(-46, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Trebuchet MS");
    g_fHeading = CreateFont(-19, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Trebuchet MS");
    make_code_font();
}

/* ================================================================== layout */

void app_layout(void)
{
    RECT rc; int W, H, x, edH, sw = 0;
    if (!g_hwndMain) return;
    GetClientRect(g_hwndMain, &rc); W = rc.right; H = rc.bottom;
    if (s_sideW > W - 300) s_sideW = W - 300;
    if (s_sideW < 170) s_sideW = 170;
    if (s_panelH > H - 150) s_panelH = H - 150;
    if (s_panelH < 80) s_panelH = 80;
    MoveWindow(g_hwndStatus, 0, H - STATUS_H, W, STATUS_H, TRUE);
    MoveWindow(g_hwndActivity, 0, 0, ACTIVITY_W, H - STATUS_H, TRUE);
    x = ACTIVITY_W;
    if (g_sidebarVisible) {
        sw = s_sideW;
        MoveWindow(g_hwndSidebar, x, 0, sw, H - STATUS_H, TRUE);
        ShowWindow(g_hwndSidebar, SW_SHOW);
        SetRect(&s_sashSide, x + sw, 0, x + sw + SASH, H - STATUS_H);
        x += sw + SASH;
    } else {
        ShowWindow(g_hwndSidebar, SW_HIDE);
        SetRectEmpty(&s_sashSide);
    }
    edH = H - STATUS_H;
    if (g_panelVisible) {
        edH -= s_panelH + SASH;
        MoveWindow(g_hwndPanel, x, edH + SASH, W - x, s_panelH, TRUE);
        ShowWindow(g_hwndPanel, SW_SHOW);
        SetRect(&s_sashPanel, x, edH, W, edH + SASH);
    } else {
        ShowWindow(g_hwndPanel, SW_HIDE);
        SetRectEmpty(&s_sashPanel);
    }
    MoveWindow(g_hwndEditor, x, 0, W - x, edH, TRUE);
    if (palette_is_open()) {
        int pw = 600; if (pw > W - x - 40) pw = W - x - 40;
        SetWindowPos(g_hwndPalette, HWND_TOP, x + (W - x - pw) / 2, 6, pw, 0, SWP_NOSIZE);
    }
    InvalidateRect(g_hwndMain, NULL, FALSE);
    InvalidateRect(g_hwndActivity, NULL, FALSE);
}

void app_update_title(void)
{
    char t[MAX_PATH * 2]; int i = editor_active_index();
    const char *folder = g_root[0] ? path_name(g_root) : NULL;
    if (i >= 0) {
        char label[MAX_PATH]; editor_doc_label(i, label, sizeof label);
        sfmt(t, sizeof t, "%s%s%s%s - " APP_NAME, editor_doc_dirty(i) ? "\x95 " : "", label,
             folder ? " - " : "", folder ? folder : "");
    } else if (folder) sfmt(t, sizeof t, "%s - " APP_NAME, folder);
    else sfmt(t, sizeof t, APP_NAME);
    SetWindowText(g_hwndMain, t);
    status_refresh();
}

/* ================================================================== session */

static void save_session(void)
{
    char key[32], buf[32]; int i, n = 0;
    WINDOWPLACEMENT wp; wp.length = sizeof wp;
    GetWindowPlacement(g_hwndMain, &wp);
    WritePrivateProfileString("session", "folder", g_root, s_ini);
    WritePrivateProfileString("session", NULL, NULL, s_ini);
    WritePrivateProfileString("session", "folder", g_root, s_ini);
    for (i = 0; i < editor_count(); i++) {
        const char *p = editor_doc_path(i);
        if (!file_exists(p)) continue;
        sfmt(key, sizeof key, "file%d", n++);
        WritePrivateProfileString("session", key, p, s_ini);
    }
    sfmt(buf, sizeof buf, "%d", n); WritePrivateProfileString("session", "files", buf, s_ini);
    sfmt(buf, sizeof buf, "%d", editor_active_index()); WritePrivateProfileString("session", "active", buf, s_ini);
#define PUTI(k, v) sfmt(buf, sizeof buf, "%d", (v)); WritePrivateProfileString("ui", k, buf, s_ini)
    PUTI("sidebar", g_sidebarVisible); PUTI("panel", g_panelVisible); PUTI("sideW", s_sideW);
    PUTI("panelH", s_panelH); PUTI("font", g_codeSize); PUTI("minimap", g_minimap);
    PUTI("max", wp.showCmd == SW_SHOWMAXIMIZED);
    PUTI("x", wp.rcNormalPosition.left); PUTI("y", wp.rcNormalPosition.top);
    PUTI("w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    PUTI("h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
}

static void restore_session_files(void)
{
    char key[32], p[MAX_PATH]; int i, n = GetPrivateProfileInt("session", "files", 0, s_ini);
    int act = GetPrivateProfileInt("session", "active", -1, s_ini);
    for (i = 0; i < n; i++) {
        sfmt(key, sizeof key, "file%d", i);
        GetPrivateProfileString("session", key, "", p, sizeof p, s_ini);
        if (p[0] && file_exists(p)) editor_open(p);
    }
    if (act >= 0 && act < editor_count()) editor_activate(act);
}

void app_open_folder(const char *path)
{
    char full[MAX_PATH];
    if (path && path[0]) {
        GetFullPathName(path, MAX_PATH, full, NULL);
        { int n = (int)strlen(full); if (n > 3 && full[n - 1] == '\\') full[n - 1] = 0; }
        strcpy(g_root, full);
        SetCurrentDirectory(g_root);
        out_log("Opened folder %s", g_root);
    } else g_root[0] = 0;
    sidebar_set_root(g_root);
    app_update_title();
    InvalidateRect(g_hwndEditor, NULL, FALSE);
}

static int CALLBACK browse_cb(HWND w, UINT m, LPARAM lp, LPARAM data)
{
    if (m == BFFM_INITIALIZED && g_root[0]) SendMessage(w, BFFM_SETSELECTION, TRUE, (LPARAM)g_root);
    return 0;
}

static void open_folder_dialog(void)
{
    BROWSEINFO bi; LPITEMIDLIST pidl; char path[MAX_PATH];
    memset(&bi, 0, sizeof bi);
    bi.hwndOwner = g_hwndMain; bi.lpszTitle = "Open Folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE; bi.lpfn = browse_cb;
    pidl = SHBrowseForFolder(&bi);
    if (pidl && SHGetPathFromIDList(pidl, path)) app_open_folder(path);
    if (pidl) CoTaskMemFree(pidl);
}

static void open_file_dialog(void)
{
    OPENFILENAME of; char buf[MAX_PATH * 8];
    memset(&of, 0, sizeof of); buf[0] = 0;
    of.lStructSize = sizeof of; of.hwndOwner = g_hwndMain;
    of.lpstrFilter = "All Files (*.*)\0*.*\0C/C++ Files\0*.c;*.h;*.cpp\0Python\0*.py\0Batch\0*.bat;*.cmd\0";
    of.lpstrFile = buf; of.nMaxFile = sizeof buf; of.lpstrInitialDir = g_root[0] ? g_root : NULL;
    of.Flags = OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
    if (GetOpenFileName(&of)) {
        char *p = buf + strlen(buf) + 1;
        if (!*p) editor_open(buf);
        else for (; *p; p += strlen(p) + 1) {
            char full[MAX_PATH]; sfmt(full, sizeof full, "%s\\%s", buf, p); editor_open(full);
        }
    }
}

/* ================================================================== commands */

static void run_in_terminal(const char *cmd)
{
    g_panelVisible = 1; app_layout();
    panel_show_tab(2);
    term_send_line(cmd);
}

static void run_active_file(int buildOnly)
{
    int i = editor_active_index(); const char *p, *e; char dir[MAX_PATH], cmd[MAX_PATH * 3];
    if (i < 0) { out_log("Nothing to run: no active editor"); return; }
    if (!editor_save(i, 0)) return;
    p = editor_doc_path(i); e = path_ext(p);
    strcpy(dir, p); *(char *)path_name(dir) = 0;
    if (strlen(dir) > 3) dir[strlen(dir) - 1] = 0;
    if (!_stricmp(e, "c")) {
        char exe[MAX_PATH]; strcpy(exe, path_name(p)); exe[strlen(exe) - 2] = 0;
        if (buildOnly) sfmt(cmd, sizeof cmd, "cd /d \"%s\" && tcc \"%s\" -o \"%s.exe\"", dir, path_name(p), exe);
        else sfmt(cmd, sizeof cmd, "cd /d \"%s\" && tcc \"%s\" -o \"%s.exe\" && \"%s.exe\"", dir, path_name(p), exe, exe);
    } else if (!_stricmp(e, "py")) sfmt(cmd, sizeof cmd, "cd /d \"%s\" && python -u \"%s\"", dir, path_name(p));
    else if (!_stricmp(e, "bat") || !_stricmp(e, "cmd")) sfmt(cmd, sizeof cmd, "cd /d \"%s\" && call \"%s\"", dir, path_name(p));
    else if (!_stricmp(e, "js")) sfmt(cmd, sizeof cmd, "cd /d \"%s\" && cscript //nologo \"%s\"", dir, path_name(p));
    else if (!_stricmp(e, "html") || !_stricmp(e, "htm")) sfmt(cmd, sizeof cmd, "start \"\" \"%s\"", p);
    else { out_log("Don't know how to run %s files", e); panel_show_tab(1); return; }
    out_log("Run: %s", cmd);
    run_in_terminal(cmd);
}

static void build_task(void)
{
    char bat[MAX_PATH], cmd[MAX_PATH * 2];
    editor_save_all();
    if (g_root[0]) {
        sfmt(bat, sizeof bat, "%s\\build.bat", g_root);
        if (file_exists(bat)) {
            sfmt(cmd, sizeof cmd, "cd /d \"%s\" && call build.bat", g_root);
            out_log("Build task: %s", cmd);
            run_in_terminal(cmd);
            return;
        }
    }
    run_active_file(1);
}

static void zoom(int delta)
{
    g_codeSize = delta ? g_codeSize + delta : 14;
    if (g_codeSize < 8) g_codeSize = 8;
    if (g_codeSize > 32) g_codeSize = 32;
    make_code_font();
    editor_font_changed();
    InvalidateRect(g_hwndPanel, NULL, FALSE);
}

static void show_shortcuts(void)
{
    static const int cmds[] = { CMD_PALETTE, CMD_QUICK_OPEN, CMD_GOTO_LINE, CMD_NEW_FILE, CMD_OPEN_FILE,
        CMD_OPEN_FOLDER, CMD_SAVE, CMD_SAVE_AS, CMD_CLOSE_EDITOR, CMD_UNDO, CMD_REDO, CMD_FIND, CMD_REPLACE,
        CMD_TOGGLE_COMMENT, CMD_SELECT_ALL, CMD_MOVE_LINE_UP, CMD_MOVE_LINE_DOWN, CMD_COPY_LINE_DOWN,
        CMD_DELETE_LINE, CMD_TOGGLE_SIDEBAR, CMD_TOGGLE_PANEL, CMD_SHOW_EXPLORER, CMD_SHOW_SEARCH,
        CMD_FOCUS_TERMINAL, CMD_NEW_TERMINAL, CMD_RUN_FILE, CMD_BUILD_TASK, CMD_NEXT_EDITOR, CMD_PREV_EDITOR,
        CMD_ZOOM_IN, CMD_ZOOM_OUT, 0 };
    char *buf = (char *)malloc(8192), line[256]; int i, n = 0;
    n += sfmt(buf + n, 8192 - n, "XP Code - Keyboard Shortcuts\r\n============================\r\n\r\n");
    for (i = 0; cmds[i]; i++) {
        sfmt(line, sizeof line, "%-36s %s\r\n", command_name(cmds[i]), command_key(cmds[i]));
        n += sfmt(buf + n, 8192 - n, "%s", line);
    }
    {   /* write to a temp file so it opens as a normal tab */
        char tmp[MAX_PATH], path[MAX_PATH]; HANDLE h; DWORD wr;
        GetTempPath(MAX_PATH, tmp); sfmt(path, sizeof path, "%sKeyboard Shortcuts.txt", tmp);
        h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) { WriteFile(h, buf, n, &wr, NULL); CloseHandle(h); editor_open(path); }
    }
    free(buf);
}

void app_command(int cmd)
{
    switch (cmd) {
    case CMD_NEW_FILE: editor_new_untitled(); break;
    case CMD_OPEN_FILE: open_file_dialog(); break;
    case CMD_OPEN_FOLDER: open_folder_dialog(); break;
    case CMD_CLOSE_FOLDER: if (editor_close_all()) app_open_folder(NULL); break;
    case CMD_SAVE: if (editor_active_index() >= 0) editor_save(editor_active_index(), 0); break;
    case CMD_SAVE_AS: if (editor_active_index() >= 0) editor_save(editor_active_index(), 1); break;
    case CMD_SAVE_ALL: editor_save_all(); break;
    case CMD_CLOSE_EDITOR: if (editor_active_index() >= 0) editor_close(editor_active_index()); break;
    case CMD_CLOSE_ALL: editor_close_all(); break;
    case CMD_EXIT: PostMessage(g_hwndMain, WM_CLOSE, 0, 0); break;
    case CMD_PALETTE: palette_open(1); break;
    case CMD_QUICK_OPEN: palette_open(0); break;
    case CMD_GOTO_LINE: palette_open(2); break;
    case CMD_SHOW_EXPLORER: case CMD_SHOW_SEARCH: case CMD_SHOW_RUN:
        g_sidebarVisible = 1; sidebar_set_view(cmd - CMD_SHOW_EXPLORER); app_layout(); break;
    case CMD_TOGGLE_SIDEBAR: g_sidebarVisible = !g_sidebarVisible; app_layout(); break;
    case CMD_TOGGLE_PANEL:
        g_panelVisible = !g_panelVisible; app_layout();
        if (g_panelVisible) term_focus(); else editor_focus();
        break;
    case CMD_FOCUS_TERMINAL:
        if (g_panelVisible && GetFocus() == g_hwndPanel) { g_panelVisible = 0; app_layout(); editor_focus(); }
        else { g_panelVisible = 1; app_layout(); panel_show_tab(2); term_focus(); }
        break;
    case CMD_NEW_TERMINAL: g_panelVisible = 1; app_layout(); term_new(g_root[0] ? g_root : NULL); break;
    case CMD_NEW_FOLDER_ITEM: sidebar_new_item(1); break;
    case CMD_KILL_TERMINAL: term_kill(); break;
    case CMD_CLEAR_TERMINAL: term_clear(); break;
    case CMD_SHOW_PROBLEMS: g_panelVisible = 1; app_layout(); panel_show_tab(0); break;
    case CMD_SHOW_OUTPUT: g_panelVisible = 1; app_layout(); panel_show_tab(1); break;
    case CMD_TOGGLE_MINIMAP: g_minimap = !g_minimap; InvalidateRect(g_hwndEditor, NULL, FALSE); break;
    case CMD_ZOOM_IN: zoom(1); break;
    case CMD_ZOOM_OUT: zoom(-1); break;
    case CMD_ZOOM_RESET: zoom(0); break;
    case CMD_RUN_FILE: run_active_file(0); break;
    case CMD_BUILD_TASK: build_task(); break;
    case CMD_REFRESH_EXPLORER: sidebar_refresh(); break;
    case CMD_COLLAPSE_EXPLORER: sidebar_collapse_all(); break;
    case CMD_SHORTCUTS: show_shortcuts(); break;
    case CMD_WELCOME: editor_command(CMD_WELCOME); break;
    case CMD_FOCUS_EDITOR: editor_focus(); break;
    case CMD_ABOUT:
        {
            MSGBOXPARAMS mb; memset(&mb, 0, sizeof mb);
            mb.cbSize = sizeof mb; mb.hwndOwner = g_hwndMain; mb.hInstance = g_hinst;
            mb.lpszText = APP_NAME " " APP_VERSION "\n\nA Visual Studio Code style editor written in plain Win32 C.\n"
                          "No Chromium, no Electron - just GDI, USER32 and cmd.exe.\n\nBuilt with Tiny C Compiler on Windows XP.";
            mb.lpszCaption = "About " APP_NAME; mb.dwStyle = MB_USERICON; mb.lpszIcon = MAKEINTRESOURCE(IDI_APP);
            MessageBoxIndirect(&mb);
        }
        break;
    default: editor_command(cmd); break;
    }
}

/* global keyboard shortcuts; returns 1 if handled */
int app_hotkey(MSG *m)
{
    int ctrl, shift, alt, vk;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return 0;
    ctrl = GetKeyState(VK_CONTROL) < 0; shift = GetKeyState(VK_SHIFT) < 0; alt = GetKeyState(VK_MENU) < 0;
    vk = (int)m->wParam;
    if (palette_is_open() && vk != VK_F1) return 0;
    if (s_chordK) {
        if (vk == VK_CONTROL || vk == VK_SHIFT) return 1;
        s_chordK = 0;
        if (vk == 'O') { app_command(CMD_OPEN_FOLDER); return 1; }
        if (vk == 'S') { app_command(CMD_SAVE_ALL); return 1; }
        if (vk == 'W') { app_command(CMD_CLOSE_ALL); return 1; }
        if (vk == 'F') { app_command(CMD_CLOSE_FOLDER); return 1; }
        return 1;
    }
    if (vk == VK_F1 || (ctrl && shift && vk == 'P')) { app_command(CMD_PALETTE); return 1; }
    if (vk == VK_F5) { app_command(CMD_RUN_FILE); return 1; }
    if (alt) return 0;
    if (!ctrl) return 0;
    if (vk == VK_OEM_3 || vk == VK_OEM_8 || vk == VK_OEM_7) {
        app_command(shift ? CMD_NEW_TERMINAL : CMD_FOCUS_TERMINAL); return 1;
    }
    if (shift) {
        switch (vk) {
        case 'E': app_command(CMD_SHOW_EXPLORER); return 1;
        case 'F': app_command(CMD_SHOW_SEARCH); return 1;
        case 'D': app_command(CMD_SHOW_RUN); return 1;
        case 'B': app_command(CMD_BUILD_TASK); return 1;
        case 'S': app_command(CMD_SAVE_AS); return 1;
        case 'M': app_command(CMD_SHOW_PROBLEMS); return 1;
        case 'U': app_command(CMD_SHOW_OUTPUT); return 1;
        case VK_TAB: app_command(CMD_PREV_EDITOR); return 1;
        }
        return 0;
    }
    switch (vk) {
    case 'P': case 'E': app_command(CMD_QUICK_OPEN); return 1;
    case 'G': app_command(CMD_GOTO_LINE); return 1;
    case 'B': app_command(CMD_TOGGLE_SIDEBAR); return 1;
    case 'J': app_command(CMD_TOGGLE_PANEL); return 1;
    case 'N': app_command(CMD_NEW_FILE); return 1;
    case 'O': app_command(CMD_OPEN_FILE); return 1;
    case 'S': app_command(CMD_SAVE); return 1;
    case 'W': case VK_F4: app_command(CMD_CLOSE_EDITOR); return 1;
    case 'K': s_chordK = 1; return 1;
    case VK_TAB: case VK_NEXT: app_command(CMD_NEXT_EDITOR); return 1;
    case VK_PRIOR: app_command(CMD_PREV_EDITOR); return 1;
    case VK_OEM_PLUS: case VK_ADD: app_command(CMD_ZOOM_IN); return 1;
    case VK_OEM_MINUS: case VK_SUBTRACT: app_command(CMD_ZOOM_OUT); return 1;
    case '0': case VK_NUMPAD0: app_command(CMD_ZOOM_RESET); return 1;
    }
    return 0;
}

/* ================================================================== main window */

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), f = CreatePopupMenu(), e = CreatePopupMenu(), s = CreatePopupMenu(),
          v = CreatePopupMenu(), g = CreatePopupMenu(), r = CreatePopupMenu(), t = CreatePopupMenu(),
          h = CreatePopupMenu();
    AppendMenu(f, MF_STRING, CMD_NEW_FILE, "&New File\tCtrl+N");
    AppendMenu(f, MF_STRING, CMD_OPEN_FILE, "&Open File...\tCtrl+O");
    AppendMenu(f, MF_STRING, CMD_OPEN_FOLDER, "Open &Folder...\tCtrl+K Ctrl+O");
    AppendMenu(f, MF_SEPARATOR, 0, NULL);
    AppendMenu(f, MF_STRING, CMD_SAVE, "&Save\tCtrl+S");
    AppendMenu(f, MF_STRING, CMD_SAVE_AS, "Save &As...\tCtrl+Shift+S");
    AppendMenu(f, MF_STRING, CMD_SAVE_ALL, "Save A&ll\tCtrl+K S");
    AppendMenu(f, MF_SEPARATOR, 0, NULL);
    AppendMenu(f, MF_STRING, CMD_CLOSE_EDITOR, "Close &Editor\tCtrl+W");
    AppendMenu(f, MF_STRING, CMD_CLOSE_FOLDER, "Close Fol&der\tCtrl+K F");
    AppendMenu(f, MF_SEPARATOR, 0, NULL);
    AppendMenu(f, MF_STRING, CMD_EXIT, "E&xit");
    AppendMenu(e, MF_STRING, CMD_UNDO, "&Undo\tCtrl+Z");
    AppendMenu(e, MF_STRING, CMD_REDO, "&Redo\tCtrl+Y");
    AppendMenu(e, MF_SEPARATOR, 0, NULL);
    AppendMenu(e, MF_STRING, CMD_CUT, "Cu&t\tCtrl+X");
    AppendMenu(e, MF_STRING, CMD_COPY, "&Copy\tCtrl+C");
    AppendMenu(e, MF_STRING, CMD_PASTE, "&Paste\tCtrl+V");
    AppendMenu(e, MF_SEPARATOR, 0, NULL);
    AppendMenu(e, MF_STRING, CMD_FIND, "&Find\tCtrl+F");
    AppendMenu(e, MF_STRING, CMD_REPLACE, "&Replace\tCtrl+H");
    AppendMenu(e, MF_STRING, CMD_SHOW_SEARCH, "Find in Files\tCtrl+Shift+F");
    AppendMenu(e, MF_SEPARATOR, 0, NULL);
    AppendMenu(e, MF_STRING, CMD_TOGGLE_COMMENT, "Toggle Line &Comment\tCtrl+/");
    AppendMenu(s, MF_STRING, CMD_SELECT_ALL, "Select &All\tCtrl+A");
    AppendMenu(s, MF_SEPARATOR, 0, NULL);
    AppendMenu(s, MF_STRING, CMD_COPY_LINE_DOWN, "Copy Line &Down\tShift+Alt+Down");
    AppendMenu(s, MF_STRING, CMD_MOVE_LINE_UP, "Move Line &Up\tAlt+Up");
    AppendMenu(s, MF_STRING, CMD_MOVE_LINE_DOWN, "Move Line Do&wn\tAlt+Down");
    AppendMenu(s, MF_STRING, CMD_DELETE_LINE, "Delete &Line\tCtrl+Shift+K");
    AppendMenu(v, MF_STRING, CMD_PALETTE, "&Command Palette...\tCtrl+Shift+P");
    AppendMenu(v, MF_STRING, CMD_QUICK_OPEN, "&Open View... (Quick Open)\tCtrl+P");
    AppendMenu(v, MF_SEPARATOR, 0, NULL);
    AppendMenu(v, MF_STRING, CMD_SHOW_EXPLORER, "&Explorer\tCtrl+Shift+E");
    AppendMenu(v, MF_STRING, CMD_SHOW_SEARCH, "&Search\tCtrl+Shift+F");
    AppendMenu(v, MF_STRING, CMD_SHOW_RUN, "&Run\tCtrl+Shift+D");
    AppendMenu(v, MF_SEPARATOR, 0, NULL);
    AppendMenu(v, MF_STRING, CMD_SHOW_PROBLEMS, "&Problems\tCtrl+Shift+M");
    AppendMenu(v, MF_STRING, CMD_SHOW_OUTPUT, "&Output\tCtrl+Shift+U");
    AppendMenu(v, MF_STRING, CMD_FOCUS_TERMINAL, "&Terminal\tCtrl+`");
    AppendMenu(v, MF_SEPARATOR, 0, NULL);
    AppendMenu(v, MF_STRING, CMD_TOGGLE_SIDEBAR, "Toggle Side &Bar\tCtrl+B");
    AppendMenu(v, MF_STRING, CMD_TOGGLE_PANEL, "Toggle Pa&nel\tCtrl+J");
    AppendMenu(v, MF_STRING, CMD_TOGGLE_MINIMAP, "Toggle &Minimap");
    AppendMenu(v, MF_SEPARATOR, 0, NULL);
    AppendMenu(v, MF_STRING, CMD_ZOOM_IN, "Zoom &In\tCtrl+=");
    AppendMenu(v, MF_STRING, CMD_ZOOM_OUT, "Zoom O&ut\tCtrl+-");
    AppendMenu(v, MF_STRING, CMD_ZOOM_RESET, "Reset &Zoom\tCtrl+0");
    AppendMenu(g, MF_STRING, CMD_QUICK_OPEN, "Go to &File...\tCtrl+P");
    AppendMenu(g, MF_STRING, CMD_GOTO_LINE, "Go to &Line...\tCtrl+G");
    AppendMenu(g, MF_SEPARATOR, 0, NULL);
    AppendMenu(g, MF_STRING, CMD_NEXT_EDITOR, "&Next Editor\tCtrl+Tab");
    AppendMenu(g, MF_STRING, CMD_PREV_EDITOR, "&Previous Editor\tCtrl+Shift+Tab");
    AppendMenu(r, MF_STRING, CMD_RUN_FILE, "&Run Active File\tF5");
    AppendMenu(r, MF_STRING, CMD_BUILD_TASK, "Run &Build Task\tCtrl+Shift+B");
    AppendMenu(t, MF_STRING, CMD_NEW_TERMINAL, "&New Terminal\tCtrl+Shift+`");
    AppendMenu(t, MF_STRING, CMD_KILL_TERMINAL, "&Kill Terminal");
    AppendMenu(t, MF_STRING, CMD_CLEAR_TERMINAL, "&Clear Terminal");
    AppendMenu(t, MF_SEPARATOR, 0, NULL);
    AppendMenu(t, MF_STRING, CMD_BUILD_TASK, "Run &Build Task...\tCtrl+Shift+B");
    AppendMenu(t, MF_STRING, CMD_RUN_FILE, "Run &Active File\tF5");
    AppendMenu(h, MF_STRING, CMD_WELCOME, "&Welcome");
    AppendMenu(h, MF_STRING, CMD_SHORTCUTS, "&Keyboard Shortcuts Reference");
    AppendMenu(h, MF_SEPARATOR, 0, NULL);
    AppendMenu(h, MF_STRING, CMD_ABOUT, "&About");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)f, "&File");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)e, "&Edit");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)s, "&Selection");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)v, "&View");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)g, "&Go");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)r, "&Run");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)t, "&Terminal");
    AppendMenu(bar, MF_POPUP, (UINT_PTR)h, "&Help");
    return bar;
}

static LRESULT CALLBACK MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_SIZE: app_layout(); return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == 0 && LOWORD(wp) >= 100) { app_command(LOWORD(wp)); return 0; }
        break;
    case WM_TIMER: editor_check_disk(); sidebar_tick(); return 0;
    case WM_ACTIVATEAPP: if (wp) editor_check_disk(); return 0;
    case WM_SETFOCUS: editor_focus(); return 0;
    case WM_DROPFILES: {
        HDROP d = (HDROP)wp; char p[MAX_PATH]; UINT i, n = DragQueryFile(d, 0xFFFFFFFF, NULL, 0);
        for (i = 0; i < n; i++) {
            DragQueryFile(d, i, p, MAX_PATH);
            if (dir_exists(p)) app_open_folder(p); else editor_open(p);
        }
        DragFinish(d);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT p; GetCursorPos(&p); ScreenToClient(w, &p);
        if (PtInRect(&s_sashSide, p)) { SetCursor(LoadCursor(NULL, IDC_SIZEWE)); return TRUE; }
        if (PtInRect(&s_sashPanel, p)) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT p; p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        if (PtInRect(&s_sashSide, p)) s_drag = 1;
        else if (PtInRect(&s_sashPanel, p)) s_drag = 2;
        if (s_drag) SetCapture(w);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s_drag) {
            RECT rc; GetClientRect(w, &rc);
            if (s_drag == 1) s_sideW = GET_X_LPARAM(lp) - ACTIVITY_W;
            else s_panelH = rc.bottom - STATUS_H - GET_Y_LPARAM(lp);
            app_layout();
            UpdateWindow(g_hwndEditor);
        }
        return 0;
    case WM_LBUTTONUP: if (s_drag) { s_drag = 0; ReleaseCapture(); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
        if (!IsRectEmpty(&s_sashSide)) fill_rc(dc, &s_sashSide, C_SIDEBAR_BG);
        if (!IsRectEmpty(&s_sashPanel)) {
            fill_rc(dc, &s_sashPanel, C_EDITOR_BG);
            hline(dc, s_sashPanel.left, s_sashPanel.bottom - 1, s_sashPanel.right - s_sashPanel.left, C_BORDER);
        }
        EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        save_session();
        if (!editor_close_all()) return 0;
        term_shutdown();
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

/* Make compilers and interpreters reachable from the terminal and F5 even when they are not on the
 * system PATH: anything listed under [tools] path= in the settings file, then common install
 * locations of Tiny C Compiler and Python. */
static void prepend_path(char *np, const char *path, const char *dir)
{
    char probe[MAX_PATH];
    if (!dir_exists(dir) || strstr(path, dir) || strstr(np, dir)) return;
    sfmt(probe, sizeof probe, "%s\\tcc.exe", dir);
    if (!file_exists(probe)) { sfmt(probe, sizeof probe, "%s\\python.exe", dir); if (!file_exists(probe)) return; }
    if (strlen(np) + strlen(dir) + 2 >= 32000) return;
    strcat(np, dir); strcat(np, ";");
}

static void add_tools_to_path(void)
{
    char *path = (char *)malloc(32768), *np = (char *)malloc(32768), extra[2048], dir[MAX_PATH], pf[MAX_PATH];
    char *tok; WIN32_FIND_DATA fd; HANDLE h;
    GetEnvironmentVariable("PATH", path, 32768);
    np[0] = 0;
    GetPrivateProfileString("tools", "path", "", extra, sizeof extra, s_ini);
    for (tok = strtok(extra, ";"); tok; tok = strtok(NULL, ";")) prepend_path(np, path, tok);
    if (!GetEnvironmentVariable("ProgramFiles", pf, sizeof pf)) strcpy(pf, "C:\\Program Files");
    sfmt(dir, sizeof dir, "%stcc", g_exeDir); prepend_path(np, path, dir);
    sfmt(dir, sizeof dir, "%s\\tcc", pf); prepend_path(np, path, dir);
    prepend_path(np, path, "C:\\tcc");
    prepend_path(np, path, "C:\\dev\\tools\\tcc");
    h = FindFirstFile("C:\\Python*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { sfmt(dir, sizeof dir, "C:\\%s", fd.cFileName); prepend_path(np, path, dir); } while (FindNextFile(h, &fd));
        FindClose(h);
    }
    strcat(np, path);
    SetEnvironmentVariable("PATH", np);
    free(path); free(np);
}

/* settings live in %APPDATA%\XP Code\xpcode.ini so the program folder can stay read-only */
static void locate_settings(void)
{
    char dir[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, dir))) {
        sfmt(s_ini, sizeof s_ini, "%s\\XP Code", dir);
        CreateDirectory(s_ini, NULL);
        strcat(s_ini, "\\xpcode.ini");
    } else sfmt(s_ini, sizeof s_ini, "%sxpcode.ini", g_exeDir);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmdline, int show)
{
    WNDCLASS wc; MSG msg; INITCOMMONCONTROLSEX icc; char arg[MAX_PATH]; int x, y, cw, ch, mx;
    g_hinst = hi;
    icc.dwSize = sizeof icc; icc.dwICC = ICC_WIN95_CLASSES; InitCommonControlsEx(&icc);
    CoInitialize(NULL);
    GetModuleFileName(NULL, g_exeDir, MAX_PATH); *(char *)path_name(g_exeDir) = 0;
    locate_settings();
    add_tools_to_path();

    g_sidebarVisible = GetPrivateProfileInt("ui", "sidebar", 1, s_ini);
    g_panelVisible = GetPrivateProfileInt("ui", "panel", 1, s_ini);
    s_sideW = GetPrivateProfileInt("ui", "sideW", 250, s_ini);
    s_panelH = GetPrivateProfileInt("ui", "panelH", 230, s_ini);
    g_codeSize = GetPrivateProfileInt("ui", "font", 14, s_ini);
    g_minimap = GetPrivateProfileInt("ui", "minimap", 1, s_ini);
    g_sideView = 0;
    make_fonts();

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = MainProc; wc.hInstance = hi; wc.lpszClassName = "XPCodeMain";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(hi, MAKEINTRESOURCE(IDI_APP));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClass(&wc);
    activity_register(); status_register(); sidebar_register(); editor_register();
    panel_register(); palette_register();

    cw = GetPrivateProfileInt("ui", "w", 1280, s_ini); ch = GetPrivateProfileInt("ui", "h", 860, s_ini);
    x = GetPrivateProfileInt("ui", "x", (GetSystemMetrics(SM_CXSCREEN) - cw) / 2, s_ini);
    y = GetPrivateProfileInt("ui", "y", (GetSystemMetrics(SM_CYSCREEN) - ch) / 2 - 20, s_ini);
    mx = GetPrivateProfileInt("ui", "max", 0, s_ini);
    g_hwndMain = CreateWindowEx(WS_EX_ACCEPTFILES, "XPCodeMain", APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                x, y, cw, ch, NULL, build_menu(), hi, NULL);
    {
        HICON smallIcon = (HICON)LoadImage(hi, MAKEINTRESOURCE(IDI_APP), IMAGE_ICON, 16, 16, 0);
        if (smallIcon) SendMessage(g_hwndMain, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    }
#define CHILD (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN)
    g_hwndActivity = CreateWindow("XPActivity", "", CHILD, 0, 0, 0, 0, g_hwndMain, NULL, hi, NULL);
    g_hwndSidebar = CreateWindow("XPSidebar", "", CHILD, 0, 0, 0, 0, g_hwndMain, NULL, hi, NULL);
    g_hwndEditor = CreateWindow("XPEditor", "", CHILD, 0, 0, 0, 0, g_hwndMain, NULL, hi, NULL);
    g_hwndPanel = CreateWindow("XPPanel", "", CHILD, 0, 0, 0, 0, g_hwndMain, NULL, hi, NULL);
    g_hwndStatus = CreateWindow("XPStatus", "", CHILD, 0, 0, 0, 0, g_hwndMain, NULL, hi, NULL);
    g_hwndPalette = CreateWindow("XPPalette", "", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, g_hwndMain, NULL, hi, NULL);

    /* command line: a folder or a file; otherwise restore the last session */
    {   /* parse GetCommandLine ourselves: skip the program name, unquote the first argument */
        const char *c = GetCommandLine(); int n;
        if (*c == '"') { c++; while (*c && *c != '"') c++; if (*c) c++; }
        else while (*c && *c != ' ') c++;
        while (*c == ' ') c++;
        sfmt(arg, sizeof arg, "%s", *c == '"' ? c + 1 : c);
        n = (int)strlen(arg);
        while (n && (arg[n - 1] == ' ' || arg[n - 1] == '"')) arg[--n] = 0;
        (void)cmdline;
    }
    if (arg[0] && dir_exists(arg)) app_open_folder(arg);
    else if (arg[0] && file_exists(arg)) { app_open_folder(NULL); editor_open(arg); }
    else {
        char last[MAX_PATH];
        GetPrivateProfileString("session", "folder", "", last, sizeof last, s_ini);
        app_open_folder(dir_exists(last) ? last : NULL);
        restore_session_files();
    }
    if (editor_count() == 0) editor_command(CMD_WELCOME);
    term_new(g_root[0] ? g_root : NULL);
    out_log(APP_NAME " " APP_VERSION " started");

    ShowWindow(g_hwndMain, mx ? SW_SHOWMAXIMIZED : show);
    app_layout();
    SetTimer(g_hwndMain, 1, 1000, NULL);
    editor_focus();

    while (GetMessage(&msg, NULL, 0, 0)) {
        if (app_hotkey(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
