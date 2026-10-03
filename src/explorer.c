/* explorer.c - the side bar: file explorer tree, search across files and the run view. */
#include "xpcode.h"

#define HEADER_H  35
#define SECTION_H 22
#define ROW_H     22

typedef struct Node {
    char name[MAX_PATH], path[MAX_PATH];
    int isdir, open, loaded, depth;
    struct Node **kids; int nkids;
} Node;

static Node *s_tree;
static Node **s_rows; static int s_nrows, s_rowsCap;
static char s_selPath[MAX_PATH];
static int s_scroll, s_hover = -1, s_hoverActions, s_actHover = -1;
static HANDLE s_watch = INVALID_HANDLE_VALUE;
static RECT s_actRc[4], s_openBtn, s_runBtn, s_buildBtn;

/* search */
typedef struct { int file; int line, col, len, off; char text[160]; } Hit;
static HWND s_searchEdit; static WNDPROC s_searchOrig; static HBRUSH s_inputBrush;
static char **s_hitFiles; static int s_nhitFiles;
static Hit *s_hits; static int s_nhits, s_hitsCap;
typedef struct { int file, hit; } SRow;     /* hit = -1 for a file header */
static SRow *s_srows; static int s_nsrows;
static char s_query[256]; static int s_searchSel = -1;

/* =========================================================== tree */

static Node *node_new(const char *dir, const char *name, int isdir, int depth)
{
    Node *n = (Node *)calloc(1, sizeof(Node));
    strcpy(n->name, name);
    if (dir) sfmt(n->path, MAX_PATH, "%s\\%s", dir, name); else strcpy(n->path, name);
    n->isdir = isdir; n->depth = depth;
    return n;
}

static void node_free(Node *n)
{
    int i;
    if (!n) return;
    for (i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids); free(n);
}

static int node_cmp(const void *a, const void *b)
{
    const Node *x = *(const Node **)a, *y = *(const Node **)b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;
    return _stricmp(x->name, y->name);
}

static void load_dir(Node *n)
{
    WIN32_FIND_DATA fd; HANDLE h; char pat[MAX_PATH]; int cap = 0;
    n->loaded = 1;
    sfmt(pat, sizeof pat, "%s\\*", n->path);
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (n->nkids == cap) { cap = cap ? cap * 2 : 16; n->kids = (Node **)realloc(n->kids, cap * sizeof(Node *)); }
        n->kids[n->nkids++] = node_new(n->path, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, n->depth + 1);
    } while (FindNextFile(h, &fd));
    FindClose(h);
    qsort(n->kids, n->nkids, sizeof(Node *), node_cmp);
}

static void add_rows(Node *n)
{
    int i;
    for (i = 0; i < n->nkids; i++) {
        if (s_nrows == s_rowsCap) { s_rowsCap = s_rowsCap ? s_rowsCap * 2 : 256; s_rows = (Node **)realloc(s_rows, s_rowsCap * sizeof(Node *)); }
        s_rows[s_nrows++] = n->kids[i];
        if (n->kids[i]->isdir && n->kids[i]->open) add_rows(n->kids[i]);
    }
}

static void rebuild_rows(void)
{
    s_nrows = 0;
    if (s_tree) add_rows(s_tree);
    InvalidateRect(g_hwndSidebar, NULL, FALSE);
}

static void carry_state(Node *fresh, Node *old)
{
    int i, j;
    if (!old->open) return;
    fresh->open = 1;
    load_dir(fresh);
    for (i = 0; i < fresh->nkids; i++)
        if (fresh->kids[i]->isdir)
            for (j = 0; j < old->nkids; j++)
                if (old->kids[j]->isdir && !_stricmp(old->kids[j]->name, fresh->kids[i]->name)) { carry_state(fresh->kids[i], old->kids[j]); break; }
}

void sidebar_refresh(void)
{
    Node *fresh;
    if (!s_tree) return;
    fresh = node_new(NULL, s_tree->path, 1, -1);
    carry_state(fresh, s_tree);
    node_free(s_tree);
    s_tree = fresh;
    rebuild_rows();
}

void sidebar_set_root(const char *root)
{
    node_free(s_tree); s_tree = NULL;
    if (s_watch != INVALID_HANDLE_VALUE) { FindCloseChangeNotification(s_watch); s_watch = INVALID_HANDLE_VALUE; }
    s_scroll = 0; s_selPath[0] = 0;
    if (root && root[0]) {
        s_tree = node_new(NULL, root, 1, -1);
        s_tree->open = 1;
        load_dir(s_tree);
        s_watch = FindFirstChangeNotification(root, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    }
    rebuild_rows();
}

void sidebar_tick(void)
{
    if (s_watch != INVALID_HANDLE_VALUE && WaitForSingleObject(s_watch, 0) == WAIT_OBJECT_0) {
        sidebar_refresh();
        FindNextChangeNotification(s_watch);
    }
}

static void collapse(Node *n) { int i; for (i = 0; i < n->nkids; i++) { n->kids[i]->open = 0; collapse(n->kids[i]); } }
void sidebar_collapse_all(void) { if (s_tree) { collapse(s_tree); s_scroll = 0; rebuild_rows(); } }

static int visible_rows(void)
{
    RECT r; GetClientRect(g_hwndSidebar, &r);
    return (r.bottom - HEADER_H - SECTION_H) / ROW_H;
}

static int row_of_path(const char *p)
{
    int i;
    for (i = 0; i < s_nrows; i++) if (!_stricmp(s_rows[i]->path, p)) return i;
    return -1;
}

static void scroll_into_view(int row)
{
    int vis = visible_rows();
    if (row < 0 || vis <= 0) return;
    if (row < s_scroll) s_scroll = row;
    if (row >= s_scroll + vis) s_scroll = row - vis + 1;
}

void sidebar_reveal(const char *path)
{
    Node *n = s_tree; char rel[MAX_PATH], *seg, *next;
    if (!n || !path_rel(path, rel, sizeof rel)) return;
    seg = rel;
    while (seg && n) {
        int i; Node *found = NULL;
        next = strchr(seg, '\\');
        if (next) *next++ = 0;
        if (!n->loaded) load_dir(n);
        for (i = 0; i < n->nkids; i++) if (!_stricmp(n->kids[i]->name, seg)) { found = n->kids[i]; break; }
        if (!found) break;
        if (next && found->isdir) found->open = 1;
        n = found; seg = next;
    }
    strcpy(s_selPath, path);
    rebuild_rows();
    scroll_into_view(row_of_path(path));
}

static void collect(const char *dir, const char *rel, char ***out, int *n, int *cap)
{
    WIN32_FIND_DATA fd; HANDLE h; char pat[MAX_PATH];
    sfmt(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[MAX_PATH], r[MAX_PATH];
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        sfmt(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (rel[0]) sfmt(r, sizeof r, "%s\\%s", rel, fd.cFileName); else sfmt(r, sizeof r, "%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!_stricmp(fd.cFileName, "node_modules")) continue;
            if (*n < 5000) collect(full, r, out, n, cap);
        } else if (*n < 5000) {
            if (*n == *cap) { *cap = *cap ? *cap * 2 : 256; *out = (char **)realloc(*out, *cap * sizeof(char *)); }
            (*out)[(*n)++] = _strdup(r);
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

int sidebar_list_files(char ***out)
{
    int n = 0, cap = 0;
    *out = NULL;
    if (g_root[0]) collect(g_root, "", out, &n, &cap);
    return n;
}

/* =========================================================== file operations */

static Node *sel_node(void) { int r = row_of_path(s_selPath); return r >= 0 ? s_rows[r] : NULL; }

static void target_dir(char *out)
{
    Node *n = sel_node();
    if (!n) { strcpy(out, g_root); return; }
    strcpy(out, n->path);
    if (!n->isdir) *(char *)path_name(out) = 0, out[strlen(out) - 1] = 0;
}

static void create_cb(const char *text, void *ctx)
{
    char dir[MAX_PATH], full[MAX_PATH];
    int folder = (int)(INT_PTR)ctx;
    if (!text[0]) return;
    target_dir(dir);
    sfmt(full, sizeof full, "%s\\%s", dir, text);
    if (folder) {
        if (!CreateDirectory(full, NULL)) { MessageBox(g_hwndMain, "Could not create the folder.", APP_NAME, MB_ICONERROR); return; }
        out_log("Created folder %s", full);
        sidebar_refresh(); sidebar_reveal(full);
    } else {
        HANDLE h = CreateFile(full, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) { MessageBox(g_hwndMain, "Could not create the file (does it already exist?).", APP_NAME, MB_ICONERROR); return; }
        CloseHandle(h);
        out_log("Created file %s", full);
        sidebar_refresh();
        editor_open(full);
    }
}

void sidebar_new_item(int folder)
{
    if (!g_root[0]) { app_command(CMD_OPEN_FOLDER); return; }
    palette_prompt(folder ? "New folder name" : "New file name", "", create_cb, (void *)(INT_PTR)folder);
}

static void rename_cb(const char *text, void *ctx)
{
    Node *n = sel_node(); char to[MAX_PATH], dir[MAX_PATH];
    if (!n || !text[0]) return;
    strcpy(dir, n->path); *(char *)path_name(dir) = 0;
    sfmt(to, sizeof to, "%s%s", dir, text);
    if (!MoveFile(n->path, to)) { MessageBox(g_hwndMain, "Rename failed.", APP_NAME, MB_ICONERROR); return; }
    out_log("Renamed %s -> %s", n->path, to);
    strcpy(s_selPath, to);
    sidebar_refresh();
}

static void delete_sel(void)
{
    Node *n = sel_node(); char msg[MAX_PATH + 100], from[MAX_PATH + 2]; SHFILEOPSTRUCT op;
    if (!n) return;
    sfmt(msg, sizeof msg, "Are you sure you want to delete '%s'%s?\n\nYou can restore it from the Recycle Bin.",
         n->name, n->isdir ? " and its contents" : "");
    if (MessageBox(g_hwndMain, msg, APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES) return;
    memset(from, 0, sizeof from); strcpy(from, n->path);
    memset(&op, 0, sizeof op);
    op.hwnd = g_hwndMain; op.wFunc = FO_DELETE; op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperation(&op) == 0) out_log("Deleted %s", from);
    sidebar_refresh();
}

static void tree_context(int x, int y)
{
    HMENU m = CreatePopupMenu(); POINT pt; int c; Node *n = sel_node(); char dir[MAX_PATH];
    AppendMenu(m, MF_STRING, 1, "New File...");
    AppendMenu(m, MF_STRING, 2, "New Folder...");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 3, "Reveal in Windows Explorer");
    AppendMenu(m, MF_STRING, 4, "Open in Integrated Terminal");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING | (n ? 0 : MF_GRAYED), 5, "Copy Path");
    AppendMenu(m, MF_STRING | (n ? 0 : MF_GRAYED), 6, "Copy Relative Path");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING | (n ? 0 : MF_GRAYED), 7, "Rename...\tF2");
    AppendMenu(m, MF_STRING | (n ? 0 : MF_GRAYED), 8, "Delete\tDel");
    pt.x = x; pt.y = y; ClientToScreen(g_hwndSidebar, &pt);
    c = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, g_hwndSidebar, NULL);
    DestroyMenu(m);
    target_dir(dir);
    switch (c) {
    case 1: sidebar_new_item(0); break;
    case 2: sidebar_new_item(1); break;
    case 3: {
        char a[MAX_PATH + 16];
        if (n) sfmt(a, sizeof a, "/select,\"%s\"", n->path); else sfmt(a, sizeof a, "\"%s\"", g_root);
        ShellExecute(NULL, "open", "explorer.exe", a, NULL, SW_SHOWNORMAL); break; }
    case 4: g_panelVisible = 1; app_layout(); term_new(dir); break;
    case 5: case 6: {
        char rel[MAX_PATH]; const char *t = n->path; HGLOBAL h; char *p;
        if (c == 6) { path_rel(n->path, rel, sizeof rel); t = rel; }
        h = GlobalAlloc(GMEM_MOVEABLE, strlen(t) + 1); p = (char *)GlobalLock(h); strcpy(p, t); GlobalUnlock(h);
        if (OpenClipboard(g_hwndSidebar)) { EmptyClipboard(); SetClipboardData(CF_TEXT, h); CloseClipboard(); }
        break; }
    case 7: palette_prompt("New name", n->name, rename_cb, NULL); break;
    case 8: delete_sel(); break;
    }
}

/* =========================================================== search */

static void search_clear(void)
{
    int i;
    for (i = 0; i < s_nhitFiles; i++) free(s_hitFiles[i]);
    free(s_hitFiles); s_hitFiles = NULL; s_nhitFiles = 0;
    s_nhits = 0; s_nsrows = 0; s_searchSel = -1; s_scroll = 0;
}

static void run_search(void)
{
    char **files; int nf, i, qn;
    HCURSOR old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    GetWindowText(s_searchEdit, s_query, sizeof s_query);
    search_clear();
    qn = (int)strlen(s_query);
    if (!qn || !g_root[0]) { InvalidateRect(g_hwndSidebar, NULL, FALSE); SetCursor(old); return; }
    nf = sidebar_list_files(&files);
    for (i = 0; i < nf && s_nhits < 2000; i++) {
        char full[MAX_PATH], *t, *p, *line; int len, ln = 0, fileIdx = -1, k, bin = 0;
        sfmt(full, sizeof full, "%s\\%s", g_root, files[i]);
        t = read_file(full, &len);
        if (!t) continue;
        if (len > 2 * 1024 * 1024) { free(t); continue; }
        for (k = 0; k < len && k < 4000; k++) if (!t[k]) { bin = 1; break; }
        if (bin) { free(t); continue; }
        line = t;
        while (line && *line) {
            int ll; char *e = strchr(line, '\n');
            ll = e ? (int)(e - line) : (int)strlen(line);
            for (p = line; p + qn <= line + ll; p++) {
                if (!_strnicmp(p, s_query, qn)) {
                    Hit *h; int c = (int)(p - line), from = c > 24 ? c - 24 : 0, n, pre = 0;
                    if (fileIdx < 0) {
                        s_hitFiles = (char **)realloc(s_hitFiles, (s_nhitFiles + 1) * sizeof(char *));
                        fileIdx = s_nhitFiles; s_hitFiles[s_nhitFiles++] = _strdup(files[i]);
                    }
                    if (s_nhits == s_hitsCap) { s_hitsCap = s_hitsCap ? s_hitsCap * 2 : 256; s_hits = (Hit *)realloc(s_hits, s_hitsCap * sizeof(Hit)); }
                    h = &s_hits[s_nhits++];
                    h->file = fileIdx; h->line = ln; h->col = c; h->len = qn;
                    if (from > 0) {   /* start the snippet on a word boundary, marked with an ellipsis */
                        while (from < c && line[from - 1] != ' ' && line[from - 1] != '\t') from++;
                        memcpy(h->text, "\x85", 1); pre = 1;
                    }
                    while (from < c && (line[from] == ' ' || line[from] == '\t')) from++;
                    n = ll - from; if (n > 150) n = 150;
                    memcpy(h->text + pre, line + from, n); n += pre; h->text[n] = 0;
                    for (k = 0; k < n; k++) if (h->text[k] == '\t' || h->text[k] == '\r') h->text[k] = ' ';
                    h->off = c - from + pre;   /* match offset within the snippet */
                    break;
                }
            }
            ln++;
            line = e ? e + 1 : NULL;
        }
        free(t);
    }
    for (i = 0; i < nf; i++) free(files[i]);
    free(files);
    /* flatten into rows */
    s_srows = (SRow *)realloc(s_srows, (s_nhits + s_nhitFiles + 1) * sizeof(SRow));
    {
        int f, h; s_nsrows = 0;
        for (f = 0; f < s_nhitFiles; f++) {
            s_srows[s_nsrows].file = f; s_srows[s_nsrows].hit = -1; s_nsrows++;
            for (h = 0; h < s_nhits; h++) if (s_hits[h].file == f) { s_srows[s_nsrows].file = f; s_srows[s_nsrows].hit = h; s_nsrows++; }
        }
    }
    out_log("Search '%s': %d results in %d files", s_query, s_nhits, s_nhitFiles);
    SetCursor(old);
    InvalidateRect(g_hwndSidebar, NULL, FALSE);
}

static void open_hit(int row)
{
    SRow *r; char full[MAX_PATH];
    if (row < 0 || row >= s_nsrows) return;
    r = &s_srows[row];
    sfmt(full, sizeof full, "%s\\%s", g_root, s_hitFiles[r->file]);
    if (!editor_open(full)) return;
    if (r->hit >= 0) editor_goto(s_hits[r->hit].line + 1, s_hits[r->hit].col + 1, s_hits[r->hit].len);
}

static LRESULT CALLBACK SearchEditProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN && wp == VK_RETURN) { run_search(); return 0; }
    if (m == WM_KEYDOWN && wp == VK_DOWN && s_nsrows) { s_searchSel = 0; SetFocus(g_hwndSidebar); InvalidateRect(g_hwndSidebar, NULL, FALSE); return 0; }
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { SetWindowText(w, ""); search_clear(); InvalidateRect(g_hwndSidebar, NULL, FALSE); return 0; }
    if (m == WM_KEYDOWN && wp == 'A' && GetKeyState(VK_CONTROL) < 0) { SendMessage(w, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_CHAR && (wp == 13 || wp == 27 || wp == 1)) return 0;
    return CallWindowProc(s_searchOrig, w, m, wp, lp);
}

/* =========================================================== painting */

static void paint_header(HDC dc, int w, const char *title)
{
    fill(dc, 0, 0, w, HEADER_H, C_SIDEBAR_BG);
    text_at(dc, 20, 12, title, -1, HEX(0xBBBBBB), g_fUISmall);
}

static void paint_scrollbar(HDC dc, int w, int y0, int h, int total, int vis, int pos)
{
    int th, ty;
    if (total <= vis || h <= 0) return;
    th = h * vis / total; if (th < 20) th = 20;
    ty = y0 + (h - th) * pos / (total - vis);
    fill(dc, w - 10, ty, 10, th, HEX(0x424242));
}

static void paint_explorer(HDC dc, int w, int h)
{
    int focused = GetFocus() == g_hwndSidebar, i, y;
    paint_header(dc, w, "EXPLORER");
    if (!g_root[0]) {
        const char *msg1 = "You have not yet opened a folder.";
        text_at(dc, 20, HEADER_H + 14, msg1, -1, C_SIDEBAR_TEXT, g_fUI);
        SetRect(&s_openBtn, 20, HEADER_H + 44, w - 20, HEADER_H + 72);
        fill_rc(dc, &s_openBtn, s_hover == -2 ? HEX(0x1177BB) : HEX(0x0E639C));
        text_at(dc, (w - text_w(dc, g_fUI, "Open Folder", -1)) / 2, HEADER_H + 51, "Open Folder", -1, C_TEXT_BRIGHT, g_fUI);
        text_at(dc, 20, HEADER_H + 90, "Opening a folder will show its files here", -1, C_TEXT_MUTED, g_fUI);
        text_at(dc, 20, HEADER_H + 106, "and start the terminal inside it.", -1, C_TEXT_MUTED, g_fUI);
        return;
    }
    /* section header with actions */
    {
        char up[MAX_PATH]; int k; const char *nm = path_name(g_root);
        static const int icons[4] = { ICON_NEW_FILE, ICON_NEW_FOLDER, ICON_REFRESH, ICON_COLLAPSE };
        sfmt(up, sizeof up, "%s", nm[0] ? nm : g_root); CharUpper(up);
        draw_icon(dc, ICON_CHEVRON_DOWN, 10, HEADER_H + SECTION_H / 2, 14, C_SIDEBAR_TEXT);
        text_ellipsis(dc, 22, HEADER_H + 4, w - 22 - (s_hoverActions ? 100 : 6), up, C_SIDEBAR_TEXT, g_fUIBold);
        for (k = 0; k < 4; k++) {
            SetRect(&s_actRc[k], w - 96 + k * 22, HEADER_H + 1, w - 76 + k * 22, HEADER_H + 21);
            if (!s_hoverActions) continue;
            if (s_actHover == k) fill_rc(dc, &s_actRc[k], HEX(0x3A3D41));
            draw_icon(dc, icons[k], s_actRc[k].left + 10, HEADER_H + 11, 14, C_SIDEBAR_TEXT);
        }
    }
    y = HEADER_H + SECTION_H;
    for (i = s_scroll; i < s_nrows && y < h; i++, y += ROW_H) {
        Node *n = s_rows[i]; int x = 12 + n->depth * 12, sel = !_stricmp(n->path, s_selPath);
        if (sel) {
            fill(dc, 0, y, w, ROW_H, focused ? C_LIST_FOCUS : C_LIST_ACTIVE);
            if (focused) frame(dc, 0, y, w, ROW_H, C_ACCENT);
        } else if (i == s_hover) fill(dc, 0, y, w, ROW_H, C_LIST_HOVER);
        { int g; for (g = 0; g < n->depth; g++) vline(dc, 12 + g * 12 + 8, y, ROW_H, HEX(0x3B3B3B)); }
        file_badge(dc, n->name, n->isdir, n->open, x, y + ROW_H / 2);
        text_ellipsis(dc, x + 20, y + 4, w - x - 26, n->name, sel && focused ? C_TEXT_BRIGHT : C_SIDEBAR_TEXT, g_fUI);
    }
    paint_scrollbar(dc, w, HEADER_H + SECTION_H, h - HEADER_H - SECTION_H, s_nrows, visible_rows(), s_scroll);
}

static void paint_search(HDC dc, int w, int h)
{
    RECT er; int y, i; char buf[128];
    paint_header(dc, w, "SEARCH");
    SetRect(&er, 12, HEADER_H + 4, w - 12, HEADER_H + 28);
    fill_rc(dc, &er, C_INPUT_BG);
    if (GetFocus() == s_searchEdit) frame(dc, er.left, er.top, er.right - er.left, er.bottom - er.top, C_ACCENT);
    y = HEADER_H + 36;
    if (!g_root[0]) { text_at(dc, 20, y, "Open a folder to search its files.", -1, C_TEXT_MUTED, g_fUI); return; }
    if (s_query[0]) {
        if (s_nhits) sfmt(buf, sizeof buf, "%d result%s in %d file%s%s", s_nhits, s_nhits == 1 ? "" : "s", s_nhitFiles,
                          s_nhitFiles == 1 ? "" : "s", s_nhits >= 2000 ? " (limited)" : "");
        else sfmt(buf, sizeof buf, "No results found.");
        text_at(dc, 20, y, buf, -1, C_TEXT_MUTED, g_fUI);
    } else text_at(dc, 20, y, "Type a term and press Enter.", -1, C_TEXT_MUTED, g_fUI);
    y += 22;
    for (i = s_scroll; i < s_nsrows && y < h; i++, y += ROW_H) {
        SRow *r = &s_srows[i];
        if (i == s_searchSel) fill(dc, 0, y, w, ROW_H, GetFocus() == g_hwndSidebar ? C_LIST_FOCUS : C_LIST_ACTIVE);
        else if (i == s_hover) fill(dc, 0, y, w, ROW_H, C_LIST_HOVER);
        if (r->hit < 0) {
            const char *rel = s_hitFiles[r->file], *nm = path_name(rel); int cnt = 0, k, x;
            for (k = 0; k < s_nhits; k++) if (s_hits[k].file == r->file) cnt++;
            draw_icon(dc, ICON_CHEVRON_DOWN, 12, y + ROW_H / 2, 14, C_SIDEBAR_TEXT);
            file_badge(dc, nm, 0, 0, 20, y + ROW_H / 2);
            text_at(dc, 40, y + 4, nm, -1, C_SIDEBAR_TEXT, g_fUI);
            x = 46 + text_w(dc, g_fUI, nm, -1);
            if (nm != rel) { char dir[MAX_PATH]; sfmt(dir, sizeof dir, "%.*s", (int)(nm - rel - 1), rel); text_ellipsis(dc, x, y + 4, w - x - 40, dir, C_TEXT_MUTED, g_fUISmall); }
            sfmt(buf, sizeof buf, "%d", cnt);
            { int bw = text_w(dc, g_fUISmall, buf, -1) + 10; fill(dc, w - bw - 14, y + 3, bw, 16, HEX(0x4D4D4D)); text_at(dc, w - bw - 9, y + 4, buf, -1, C_SIDEBAR_TEXT, g_fUISmall); }
        } else {
            Hit *ht = &s_hits[r->hit]; int off = ht->off, x = 40, len = (int)strlen(ht->text);
            int pre = text_w(dc, g_fUI, ht->text, off < len ? off : len);
            if (off < len) {
                int mw = text_w(dc, g_fUI, ht->text + off, ht->len < len - off ? ht->len : len - off);
                fill(dc, x + pre, y + 3, mw, ROW_H - 6, HEX(0x623315));
            }
            text_ellipsis(dc, x, y + 4, w - x - 10, ht->text, C_SIDEBAR_TEXT, g_fUI);
        }
    }
    paint_scrollbar(dc, w, HEADER_H + 58, h - HEADER_H - 58, s_nsrows, (h - HEADER_H - 58) / ROW_H, s_scroll);
}

static void paint_run(HDC dc, int w)
{
    int y = HEADER_H + 14;
    paint_header(dc, w, "RUN");
    SetRect(&s_runBtn, 20, y, w - 20, y + 28);
    fill_rc(dc, &s_runBtn, s_hover == -3 ? HEX(0x1177BB) : HEX(0x0E639C));
    text_at(dc, (w - text_w(dc, g_fUI, "Run Active File", -1)) / 2, y + 7, "Run Active File", -1, C_TEXT_BRIGHT, g_fUI);
    y += 38;
    SetRect(&s_buildBtn, 20, y, w - 20, y + 28);
    fill_rc(dc, &s_buildBtn, s_hover == -4 ? HEX(0x45494E) : HEX(0x3A3D41));
    text_at(dc, (w - text_w(dc, g_fUI, "Run Build Task", -1)) / 2, y + 7, "Run Build Task", -1, C_TEXT_BRIGHT, g_fUI);
    y += 46;
    text_at(dc, 20, y, "F5 runs the active file in the terminal:", -1, C_TEXT_MUTED, g_fUI); y += 20;
    text_at(dc, 28, y, ".c   compile with tcc, then run", -1, C_TEXT_MUTED, g_fUI); y += 18;
    text_at(dc, 28, y, ".py  python -u", -1, C_TEXT_MUTED, g_fUI); y += 18;
    text_at(dc, 28, y, ".bat call", -1, C_TEXT_MUTED, g_fUI); y += 18;
    text_at(dc, 28, y, ".js  cscript", -1, C_TEXT_MUTED, g_fUI); y += 28;
    text_at(dc, 20, y, "Ctrl+Shift+B runs build.bat in the folder.", -1, C_TEXT_MUTED, g_fUI);
}

static void sidebar_paint(HWND w)
{
    PAINTSTRUCT ps; RECT rc; HDC dc = BeginPaint(w, &ps), mem; HBITMAP bmp, ob;
    GetClientRect(w, &rc);
    mem = CreateCompatibleDC(dc); bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = (HBITMAP)SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);
    fill_rc(mem, &rc, C_SIDEBAR_BG);
    switch (g_sideView) {
    case 0: paint_explorer(mem, rc.right, rc.bottom); break;
    case 1: paint_search(mem, rc.right, rc.bottom); break;
    case 2: paint_run(mem, rc.right); break;
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(w, &ps);
}

void sidebar_set_view(int view)
{
    g_sideView = view;
    s_scroll = 0; s_hover = -1;
    ShowWindow(s_searchEdit, view == 1 ? SW_SHOW : SW_HIDE);
    InvalidateRect(g_hwndSidebar, NULL, FALSE);
    if (view == 1) { SetFocus(s_searchEdit); SendMessage(s_searchEdit, EM_SETSEL, 0, -1); }
    else if (view == 0) SetFocus(g_hwndSidebar);
}

/* =========================================================== input */

static int row_at(int y)
{
    int top = g_sideView == 0 ? HEADER_H + SECTION_H : HEADER_H + 58;
    if (y < top) return -1;
    return s_scroll + (y - top) / ROW_H;
}

static void activate_row(int r, int keyboard)
{
    Node *n;
    if (r < 0 || r >= s_nrows) return;
    n = s_rows[r];
    strcpy(s_selPath, n->path);
    if (n->isdir) {
        n->open = !n->open;
        if (n->open && !n->loaded) load_dir(n);
        rebuild_rows();
    } else {
        editor_open(n->path);
        if (keyboard) editor_focus();
    }
    InvalidateRect(g_hwndSidebar, NULL, FALSE);
}

static void tree_key(int vk)
{
    int r = row_of_path(s_selPath); Node *n = r >= 0 ? s_rows[r] : NULL;
    switch (vk) {
    case VK_DOWN: r = r < s_nrows - 1 ? r + 1 : r; if (r < 0 && s_nrows) r = 0; break;
    case VK_UP: r = r > 0 ? r - 1 : 0; break;
    case VK_HOME: r = 0; break;
    case VK_END: r = s_nrows - 1; break;
    case VK_RIGHT: if (n && n->isdir && !n->open) { activate_row(r, 1); return; } if (n && n->isdir && r + 1 < s_nrows) r++; break;
    case VK_LEFT:
        if (n && n->isdir && n->open) { activate_row(r, 1); return; }
        if (n) { int k; for (k = r - 1; k >= 0; k--) if (s_rows[k]->depth < n->depth) { r = k; break; } }
        break;
    case VK_RETURN: case VK_SPACE: activate_row(r, 1); return;
    case VK_F2: if (n) palette_prompt("New name", n->name, rename_cb, NULL); return;
    case VK_DELETE: delete_sel(); return;
    default: return;
    }
    if (r >= 0 && r < s_nrows) { strcpy(s_selPath, s_rows[r]->path); scroll_into_view(r); InvalidateRect(g_hwndSidebar, NULL, FALSE); }
}

static LRESULT CALLBACK SidebarProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        s_inputBrush = CreateSolidBrush(C_INPUT_BG);
        s_searchEdit = CreateWindowEx(0, "EDIT", "", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, w, (HMENU)1, g_hinst, NULL);
        SendMessage(s_searchEdit, WM_SETFONT, (WPARAM)g_fUI, 0);
        s_searchOrig = (WNDPROC)SetWindowLongPtr(s_searchEdit, GWLP_WNDPROC, (LONG_PTR)SearchEditProc);
        return 0;
    case WM_SIZE:
        MoveWindow(s_searchEdit, 18, HEADER_H + 9, LOWORD(lp) - 36, 16, TRUE);
        if (s_scroll > s_nrows - visible_rows()) s_scroll = s_nrows - visible_rows();
        if (s_scroll < 0) s_scroll = 0;
        scroll_into_view(row_of_path(s_selPath));
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_SIDEBAR_TEXT); SetBkColor((HDC)wp, C_INPUT_BG);
        return (LRESULT)s_inputBrush;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_PAINT: sidebar_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(w, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), r, k; POINT pt; pt.x = x; pt.y = y;
        SetFocus(w);
        if (g_sideView == 0) {
            if (!g_root[0]) { if (PtInRect(&s_openBtn, pt)) app_command(CMD_OPEN_FOLDER); return 0; }
            if (y >= HEADER_H && y < HEADER_H + SECTION_H) {
                for (k = 0; k < 4; k++) if (PtInRect(&s_actRc[k], pt)) {
                    if (k == 0) sidebar_new_item(0); else if (k == 1) sidebar_new_item(1);
                    else if (k == 2) sidebar_refresh(); else sidebar_collapse_all();
                    return 0;
                }
                return 0;
            }
            r = row_at(y);
            if (r >= 0 && r < s_nrows) activate_row(r, 0);
        } else if (g_sideView == 1) {
            r = row_at(y);
            if (r >= 0 && r < s_nsrows) { s_searchSel = r; open_hit(r); InvalidateRect(w, NULL, FALSE); }
        } else if (g_sideView == 2) {
            if (PtInRect(&s_runBtn, pt)) app_command(CMD_RUN_FILE);
            else if (PtInRect(&s_buildBtn, pt)) app_command(CMD_BUILD_TASK);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        if (g_sideView == 0 && g_root[0]) {
            int r = row_at(GET_Y_LPARAM(lp));
            SetFocus(w);
            if (r >= 0 && r < s_nrows) strcpy(s_selPath, s_rows[r]->path); else s_selPath[0] = 0;
            InvalidateRect(w, NULL, FALSE);
            tree_context(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        }
        return 0;
    case WM_MOUSEMOVE: {
        int y = GET_Y_LPARAM(lp), h = row_at(y), ha = -1, k; POINT pt; TRACKMOUSEEVENT t;
        pt.x = GET_X_LPARAM(lp); pt.y = y;
        if (g_sideView == 0 && !g_root[0] && PtInRect(&s_openBtn, pt)) h = -2;
        if (g_sideView == 2) h = PtInRect(&s_runBtn, pt) ? -3 : PtInRect(&s_buildBtn, pt) ? -4 : -1;
        for (k = 0; k < 4; k++) if (PtInRect(&s_actRc[k], pt)) ha = k;
        if (h != s_hover || !s_hoverActions || ha != s_actHover) { s_hover = h; s_hoverActions = 1; s_actHover = ha; InvalidateRect(w, NULL, FALSE); }
        t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = w; t.dwHoverTime = 0; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: s_hover = -1; s_hoverActions = 0; s_actHover = -1; InvalidateRect(w, NULL, FALSE); return 0;
    case WM_MOUSEWHEEL: {
        int total = g_sideView == 0 ? s_nrows : s_nsrows;
        s_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 40;
        if (s_scroll > total - visible_rows()) s_scroll = total - visible_rows();
        if (s_scroll < 0) s_scroll = 0;
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (g_sideView == 0) tree_key((int)wp);
        else if (g_sideView == 1) {
            if (wp == VK_DOWN && s_searchSel < s_nsrows - 1) s_searchSel++;
            else if (wp == VK_UP) { if (s_searchSel > 0) s_searchSel--; else { SetFocus(s_searchEdit); } }
            else if (wp == VK_RETURN) open_hit(s_searchSel);
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProc(w, m, wp, lp);
}

void sidebar_register(void)
{
    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = SidebarProc; wc.hInstance = g_hinst; wc.lpszClassName = "XPSidebar";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}
