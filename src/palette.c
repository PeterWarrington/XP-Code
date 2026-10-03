/* palette.c - the quick input widget: Command Palette (>), Quick Open (files),
 * Go to Line (:) and simple text prompts used by the explorer. */
#include "xpcode.h"

#define PAL_W     600
#define ROW_H     24
#define MAXROWS   14
#define INPUT_H   38

enum { M_FILES, M_CMDS, M_LINE, M_PROMPT };

typedef struct { int cmd; const char *name, *key; } Command;
static const Command s_cmds[] = {
    { CMD_NEW_FILE, "File: New File", "Ctrl+N" },
    { CMD_OPEN_FILE, "File: Open File...", "Ctrl+O" },
    { CMD_OPEN_FOLDER, "File: Open Folder...", "Ctrl+K Ctrl+O" },
    { CMD_SAVE, "File: Save", "Ctrl+S" },
    { CMD_SAVE_AS, "File: Save As...", "Ctrl+Shift+S" },
    { CMD_SAVE_ALL, "File: Save All", "Ctrl+K S" },
    { CMD_NEW_FOLDER_ITEM, "File: New Folder...", "" },
    { CMD_CLOSE_EDITOR, "View: Close Editor", "Ctrl+W" },
    { CMD_CLOSE_ALL, "View: Close All Editors", "Ctrl+K W" },
    { CMD_CLOSE_FOLDER, "Workspace: Close Folder", "Ctrl+K F" },
    { CMD_UNDO, "Edit: Undo", "Ctrl+Z" },
    { CMD_REDO, "Edit: Redo", "Ctrl+Y" },
    { CMD_CUT, "Edit: Cut", "Ctrl+X" },
    { CMD_COPY, "Edit: Copy", "Ctrl+C" },
    { CMD_PASTE, "Edit: Paste", "Ctrl+V" },
    { CMD_FIND, "Editor: Find", "Ctrl+F" },
    { CMD_REPLACE, "Editor: Replace", "Ctrl+H" },
    { CMD_TOGGLE_COMMENT, "Editor: Toggle Line Comment", "Ctrl+/" },
    { CMD_SELECT_ALL, "Editor: Select All", "Ctrl+A" },
    { CMD_MOVE_LINE_UP, "Editor: Move Line Up", "Alt+Up" },
    { CMD_MOVE_LINE_DOWN, "Editor: Move Line Down", "Alt+Down" },
    { CMD_COPY_LINE_DOWN, "Editor: Copy Line Down", "Shift+Alt+Down" },
    { CMD_DELETE_LINE, "Editor: Delete Line", "Ctrl+Shift+K" },
    { CMD_GOTO_LINE, "Go to Line/Column...", "Ctrl+G" },
    { CMD_QUICK_OPEN, "Go to File...", "Ctrl+P" },
    { CMD_NEXT_EDITOR, "View: Open Next Editor", "Ctrl+Tab" },
    { CMD_PREV_EDITOR, "View: Open Previous Editor", "Ctrl+Shift+Tab" },
    { CMD_SHOW_EXPLORER, "View: Show Explorer", "Ctrl+Shift+E" },
    { CMD_SHOW_SEARCH, "Search: Find in Files", "Ctrl+Shift+F" },
    { CMD_SHOW_RUN, "View: Show Run", "Ctrl+Shift+D" },
    { CMD_SHOW_PROBLEMS, "View: Show Problems", "Ctrl+Shift+M" },
    { CMD_SHOW_OUTPUT, "View: Show Output", "Ctrl+Shift+U" },
    { CMD_TOGGLE_SIDEBAR, "View: Toggle Primary Side Bar Visibility", "Ctrl+B" },
    { CMD_TOGGLE_PANEL, "View: Toggle Panel Visibility", "Ctrl+J" },
    { CMD_TOGGLE_MINIMAP, "View: Toggle Minimap", "" },
    { CMD_ZOOM_IN, "View: Zoom In", "Ctrl+=" },
    { CMD_ZOOM_OUT, "View: Zoom Out", "Ctrl+-" },
    { CMD_ZOOM_RESET, "View: Reset Zoom", "Ctrl+0" },
    { CMD_REFRESH_EXPLORER, "File: Refresh Explorer", "" },
    { CMD_COLLAPSE_EXPLORER, "File: Collapse Folders in Explorer", "" },
    { CMD_FOCUS_TERMINAL, "Terminal: Focus Terminal", "Ctrl+`" },
    { CMD_NEW_TERMINAL, "Terminal: Create New Terminal", "Ctrl+Shift+`" },
    { CMD_KILL_TERMINAL, "Terminal: Kill the Active Terminal Instance", "" },
    { CMD_CLEAR_TERMINAL, "Terminal: Clear", "Ctrl+L" },
    { CMD_RUN_FILE, "Run: Run Active File", "F5" },
    { CMD_BUILD_TASK, "Tasks: Run Build Task", "Ctrl+Shift+B" },
    { CMD_FOCUS_EDITOR, "View: Focus Active Editor", "" },
    { CMD_WELCOME, "Help: Welcome", "" },
    { CMD_SHORTCUTS, "Help: Keyboard Shortcuts Reference", "" },
    { CMD_ABOUT, "Help: About", "" },
    { CMD_EXIT, "File: Exit", "Alt+F4" },
    { 0, NULL, NULL }
};

typedef struct { int idx; int score; } Item;    /* idx into s_cmds or s_files */

static HWND s_edit; static WNDPROC s_editOrig; static HBRUSH s_inputBrush;
static int s_mode, s_open, s_sel, s_top;
static Item *s_items; static int s_nitems, s_itemsCap;
static char **s_files; static int s_nfiles;
static char s_text[512], s_placeholder[128];
static InputCallback s_cb; static void *s_cbCtx;
static HWND s_prevFocus;

const char *command_name(int cmd) { int i; for (i = 0; s_cmds[i].name; i++) if (s_cmds[i].cmd == cmd) return s_cmds[i].name; return ""; }
const char *command_key(int cmd) { int i; for (i = 0; s_cmds[i].name; i++) if (s_cmds[i].cmd == cmd) return s_cmds[i].key; return ""; }
int palette_is_open(void) { return s_open; }

static int item_cmp(const void *a, const void *b) { return ((const Item *)b)->score - ((const Item *)a)->score; }

static void push_item(int idx, int score)
{
    if (s_nitems == s_itemsCap) { s_itemsCap = s_itemsCap ? s_itemsCap * 2 : 256; s_items = (Item *)realloc(s_items, s_itemsCap * sizeof(Item)); }
    s_items[s_nitems].idx = idx; s_items[s_nitems].score = score; s_nitems++;
}

static void refilter(void)
{
    const char *q; int i;
    GetWindowText(s_edit, s_text, sizeof s_text);
    if (s_mode == M_FILES && s_text[0] == '>') s_mode = M_CMDS;
    else if (s_mode == M_FILES && s_text[0] == ':') s_mode = M_LINE;
    else if ((s_mode == M_CMDS && s_text[0] != '>') || (s_mode == M_LINE && s_text[0] != ':')) s_mode = M_FILES;
    s_nitems = 0; s_sel = 0; s_top = 0;
    q = s_text;
    while (*q == '>' || *q == ':' || *q == ' ') q++;
    if (s_mode == M_CMDS) {
        for (i = 0; s_cmds[i].name; i++) {
            int sc = fuzzy_score(q, s_cmds[i].name);
            if (sc >= 0) push_item(i, *q ? sc : 1000 - i);
        }
    } else if (s_mode == M_FILES) {
        for (i = 0; i < s_nfiles; i++) {
            int a = fuzzy_score(q, path_name(s_files[i])), b = fuzzy_score(q, s_files[i]);
            int sc = a >= 0 ? a * 2 + 100 : b;
            if (sc >= 0) push_item(i, *q ? sc : -(int)strlen(s_files[i]));
        }
    }
    qsort(s_items, s_nitems, sizeof(Item), item_cmp);
    if (s_nitems > 300) s_nitems = 300;
}

static int visible_count(void)
{
    if (s_mode == M_LINE || s_mode == M_PROMPT) return 1;
    return s_nitems ? (s_nitems < MAXROWS ? s_nitems : MAXROWS) : 1;
}

static void place(void)
{
    RECT ed; int w, h; POINT p;
    GetWindowRect(g_hwndEditor, &ed);
    p.x = ed.left; p.y = ed.top; ScreenToClient(g_hwndMain, &p);
    w = PAL_W; if (w > ed.right - ed.left - 40) w = ed.right - ed.left - 40;
    if (w < 300) w = 300;
    h = INPUT_H + visible_count() * ROW_H + 8;
    SetWindowPos(g_hwndPalette, HWND_TOP, p.x + (ed.right - ed.left - w) / 2, p.y + 4, w, h, SWP_SHOWWINDOW);
    MoveWindow(s_edit, 14, 11, w - 28, 18, TRUE);
}

static void update(void) { refilter(); place(); InvalidateRect(g_hwndPalette, NULL, FALSE); }

static void open_common(int mode, const char *initial)
{
    if (!s_open) s_prevFocus = GetFocus();
    s_open = 1; s_mode = mode;
    if (mode == M_FILES) {
        int i; for (i = 0; i < s_nfiles; i++) free(s_files[i]); free(s_files);
        s_nfiles = sidebar_list_files(&s_files);
    }
    SetWindowText(s_edit, initial);
    SendMessage(s_edit, EM_SETSEL, (WPARAM)strlen(initial), (LPARAM)strlen(initial));
    update();
    SetFocus(s_edit);
}

void palette_open(int mode)
{
    s_cb = NULL;
    open_common(mode, mode == 1 ? ">" : mode == 2 ? ":" : "");
}

void palette_prompt(const char *placeholder, const char *initial, InputCallback cb, void *ctx)
{
    sfmt(s_placeholder, sizeof s_placeholder, "%s", placeholder);
    s_cb = cb; s_cbCtx = ctx;
    open_common(M_PROMPT, initial);
    SendMessage(s_edit, EM_SETSEL, 0, -1);
}

void palette_close(void)
{
    if (!s_open) return;
    s_open = 0;
    ShowWindow(g_hwndPalette, SW_HIDE);
    if (s_prevFocus && IsWindow(s_prevFocus) && IsWindowVisible(s_prevFocus)) SetFocus(s_prevFocus);
    else editor_focus();
}

static void pal_accept(void)
{
    int mode = s_mode;
    char text[512];
    strcpy(text, s_text);
    if (mode == M_PROMPT) {
        InputCallback cb = s_cb; void *ctx = s_cbCtx;
        palette_close();
        if (cb) cb(text, ctx);
        return;
    }
    if (mode == M_LINE) {
        int line = atoi(text + 1), col = 1; const char *c = strchr(text + 1, ':');
        if (c) col = atoi(c + 1);
        palette_close();
        if (line > 0) editor_goto(line, col, 0);
        return;
    }
    if (s_sel < 0 || s_sel >= s_nitems) { palette_close(); return; }
    if (mode == M_CMDS) {
        int cmd = s_cmds[s_items[s_sel].idx].cmd;
        palette_close();
        app_command(cmd);
    } else {
        char full[MAX_PATH];
        sfmt(full, sizeof full, "%s\\%s", g_root, s_files[s_items[s_sel].idx]);
        palette_close();
        editor_open(full);
        editor_focus();
    }
}

static void move_sel(int d)
{
    if (!s_nitems) return;
    s_sel += d;
    if (s_sel < 0) s_sel = s_nitems - 1;
    if (s_sel >= s_nitems) s_sel = 0;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + MAXROWS) s_top = s_sel - MAXROWS + 1;
    InvalidateRect(g_hwndPalette, NULL, FALSE);
}

static LRESULT CALLBACK PalEditProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN) {
        switch (wp) {
        case VK_ESCAPE: palette_close(); return 0;
        case VK_RETURN: pal_accept(); return 0;
        case VK_UP: move_sel(-1); return 0;
        case VK_DOWN: move_sel(1); return 0;
        case VK_PRIOR: move_sel(-MAXROWS); return 0;
        case VK_NEXT: move_sel(MAXROWS); return 0;
        }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessage(w, EM_SETSEL, 0, -1); return 0; }
        if (wp == 'P' && GetKeyState(VK_CONTROL) < 0) { move_sel(GetKeyState(VK_SHIFT) < 0 ? 0 : 1); return 0; }
    }
    if (m == WM_CHAR && (wp == 13 || wp == 27 || wp == 1 || wp == 16)) return 0;
    if (m == WM_KILLFOCUS) PostMessage(g_hwndPalette, WM_APP + 10, 0, 0);
    return CallWindowProc(s_editOrig, w, m, wp, lp);
}

/* draw text with fuzzy-matched characters highlighted */
static void draw_match(HDC dc, int x, int y, const char *s, const char *q, COLORREF c, COLORREF hl, HFONT f)
{
    int i, n = (int)strlen(s), qi = 0, ql = (int)strlen(q), cx = x;
    text_at(dc, x, y, s, n, c, f);
    if (!ql) return;
    for (i = 0; i < n && qi < ql; i++) {
        if (tolower((unsigned char)s[i]) == tolower((unsigned char)q[qi])) {
            text_at(dc, cx, y, s + i, 1, hl, g_fUIBold == f ? f : g_fUIBold);
            qi++;
        }
        cx += text_w(dc, f, s + i, 1);
    }
}

static void paint(HWND w)
{
    PAINTSTRUCT ps; RECT rc; HDC dc = BeginPaint(w, &ps), mem; HBITMAP bmp, ob; int i, y;
    const char *q = s_text; char buf[256];
    while (*q == '>' || *q == ':' || *q == ' ') q++;
    GetClientRect(w, &rc);
    mem = CreateCompatibleDC(dc); bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = (HBITMAP)SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);
    fill_rc(mem, &rc, C_WIDGET_BG);
    frame(mem, 0, 0, rc.right, rc.bottom, HEX(0x454545));
    fill(mem, 8, 6, rc.right - 16, 28, C_INPUT_BG);
    frame(mem, 8, 6, rc.right - 16, 28, C_ACCENT);
    y = INPUT_H;
    if (s_mode == M_PROMPT) {
        text_at(mem, 16, y + 5, s_placeholder, -1, C_SIDEBAR_TEXT, g_fUI);
        text_at(mem, 16 + text_w(mem, g_fUI, s_placeholder, -1) + 12, y + 5, "(press 'Enter' to confirm or 'Escape' to cancel)", -1, C_TEXT_MUTED, g_fUI);
    } else if (s_mode == M_LINE) {
        int line, col, lang, has, nl;
        editor_status(&line, &col, &lang, &has, &nl);
        if (!has) sfmt(buf, sizeof buf, "Open a text editor first to go to a line.");
        else if (atoi(q) > 0) sfmt(buf, sizeof buf, "Go to line %d.", atoi(q));
        else sfmt(buf, sizeof buf, "Current Line: %d, Character: %d. Type a line number between 1 and %d to navigate to.", line, col, nl);
        fill(mem, 1, y, rc.right - 2, ROW_H, HEX(0x04395E));
        text_at(mem, 16, y + 5, buf, -1, C_SIDEBAR_TEXT, g_fUI);
    } else if (!s_nitems) {
        text_at(mem, 16, y + 5, s_mode == M_CMDS ? "No matching commands" : (g_root[0] ? "No matching results" : "Open a folder to search for files by name"), -1, C_SIDEBAR_TEXT, g_fUI);
    } else {
        for (i = s_top; i < s_nitems && i < s_top + MAXROWS; i++, y += ROW_H) {
            int sel = i == s_sel;
            if (sel) fill(mem, 1, y, rc.right - 2, ROW_H, HEX(0x04395E));
            if (s_mode == M_CMDS) {
                const Command *c = &s_cmds[s_items[i].idx];
                draw_match(mem, 16, y + 5, c->name, q, C_SIDEBAR_TEXT, HEX(0x2AAAFF), g_fUI);
                if (c->key[0]) {
                    int kw = text_w(mem, g_fUISmall, c->key, -1);
                    fill(mem, rc.right - kw - 26, y + 3, kw + 12, 18, HEX(0x3A3A3A));
                    hline(mem, rc.right - kw - 26, y + 20, kw + 12, HEX(0x2A2A2A));
                    text_at(mem, rc.right - kw - 20, y + 5, c->key, -1, C_SIDEBAR_TEXT, g_fUISmall);
                }
            } else {
                const char *rel = s_files[s_items[i].idx], *nm = path_name(rel);
                file_badge(mem, nm, 0, 0, 12, y + ROW_H / 2);
                draw_match(mem, 34, y + 5, nm, q, C_SIDEBAR_TEXT, HEX(0x2AAAFF), g_fUI);
                if (nm != rel) {
                    sfmt(buf, sizeof buf, "%.*s", (int)(nm - rel - 1), rel);
                    text_ellipsis(mem, 44 + text_w(mem, g_fUI, nm, -1), y + 6, rc.right - 60 - text_w(mem, g_fUI, nm, -1), buf, C_TEXT_MUTED, g_fUISmall);
                }
            }
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(w, &ps);
}

static LRESULT CALLBACK PaletteProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        s_inputBrush = CreateSolidBrush(C_INPUT_BG);
        s_edit = CreateWindowEx(0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, w, (HMENU)1, g_hinst, NULL);
        SendMessage(s_edit, WM_SETFONT, (WPARAM)g_fUI, 0);
        s_editOrig = (WNDPROC)SetWindowLongPtr(s_edit, GWLP_WNDPROC, (LONG_PTR)PalEditProc);
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT_BRIGHT); SetBkColor((HDC)wp, C_INPUT_BG);
        return (LRESULT)s_inputBrush;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && s_open) update();
        return 0;
    case WM_APP + 10:
        if (s_open && GetFocus() != s_edit) { s_open = 0; ShowWindow(w, SW_HIDE); }
        return 0;
    case WM_PAINT: paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int y = GET_Y_LPARAM(lp), r;
        if (y >= INPUT_H && (s_mode == M_FILES || s_mode == M_CMDS)) {
            r = s_top + (y - INPUT_H) / ROW_H;
            if (r < s_nitems && r != s_sel) { s_sel = r; InvalidateRect(w, NULL, FALSE); }
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (GET_Y_LPARAM(lp) >= INPUT_H) pal_accept();
        else SetFocus(s_edit);
        return 0;
    case WM_MOUSEWHEEL: move_sel(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -3 : 3); return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

void palette_register(void)
{
    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = PaletteProc; wc.hInstance = g_hinst; wc.lpszClassName = "XPPalette";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}
