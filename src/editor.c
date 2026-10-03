/* editor.c - the editor group: tabs, breadcrumbs, the text engine (storage, undo,
 * syntax highlighting, rendering, input), minimap, find/replace and the welcome page. */
#include "xpcode.h"

#define TAB_H     35
#define CRUMB_H   22
#define SB_W      14
#define MM_W      90
#define TABSIZE   4
#define MAXDOCS   64

typedef struct { char *s; int len, cap; } Line;
typedef struct { int line, col; } Pos;
typedef struct { int type; Pos a, b; char *text; int len; int group; } Undo;
enum { U_INS, U_DEL };
enum { K_OTHER, K_TYPE, K_DELETE };

struct Doc {
    int kind;                       /* 0 text, 1 welcome */
    Line *ln; int n, cap;
    char path[MAX_PATH]; int untitled;
    int lang;
    Pos caret, anchor; int wantx;
    int top, left;
    Undo *u; int nu, capu, upos, savepos, group, lastKind; DWORD lastEdit;
    FILETIME mtime;
    unsigned char *st; int stcap, stvalid;
};

static Doc *s_docs[MAXDOCS];
static int s_n, s_act = -1, s_untitledNo;
static RECT s_tabRc[MAXDOCS], s_tabClose[MAXDOCS];
static int s_tabScroll, s_hoverTab = -1, s_hoverClose = -1;
static int s_selecting, s_dragScroll, s_dragMini, s_dragOffset;
static int s_lineCopy; static char *s_lineCopyText;
typedef struct { RECT r; int cmd; } Link;
static Link s_links[24]; static int s_nlinks, s_hoverLink = -1;

/* find widget */
static HWND s_findEdit, s_replEdit;
static int s_findOpen, s_replOpen, s_matchCount, s_matchIndex;
static char s_find[256];
static WNDPROC s_editOrig;
static HBRUSH s_inputBrush;
static RECT s_findRc, s_findBtns[5];   /* prev, next, close, replace, replace all */

static Doc *cur(void) { return s_act >= 0 && s_act < s_n ? s_docs[s_act] : NULL; }
static int is_text(Doc *d) { return d && d->kind == 0; }

/* =========================================================== storage */

static void line_reserve(Line *l, int need)
{
    if (need + 1 > l->cap) {
        int c = l->cap ? l->cap * 2 : 16;
        while (c < need + 1) c *= 2;
        l->s = (char *)realloc(l->s, c); l->cap = c;
    }
}

static void doc_reserve(Doc *d, int need)
{
    if (need > d->cap) {
        int c = d->cap ? d->cap * 2 : 64;
        while (c < need) c *= 2;
        d->ln = (Line *)realloc(d->ln, c * sizeof(Line));
        d->cap = c;
    }
    if (need + 1 > d->stcap) {
        d->stcap = d->cap + 1;
        d->st = (unsigned char *)realloc(d->st, d->stcap);
    }
}

static void doc_insert_lines(Doc *d, int at, int count)
{
    int i;
    doc_reserve(d, d->n + count);
    memmove(d->ln + at + count, d->ln + at, (d->n - at) * sizeof(Line));
    for (i = 0; i < count; i++) { d->ln[at + i].s = NULL; d->ln[at + i].len = d->ln[at + i].cap = 0; line_reserve(&d->ln[at + i], 0); d->ln[at + i].s[0] = 0; }
    d->n += count;
}

static void doc_remove_lines(Doc *d, int at, int count)
{
    int i;
    for (i = 0; i < count; i++) free(d->ln[at + i].s);
    memmove(d->ln + at, d->ln + at + count, (d->n - at - count) * sizeof(Line));
    d->n -= count;
}

static void doc_dirty_from(Doc *d, int line) { if (d->stvalid > line) d->stvalid = line; if (d->stvalid < 0) d->stvalid = 0; }

static void line_insert(Line *l, int col, const char *t, int n)
{
    line_reserve(l, l->len + n);
    memmove(l->s + col + n, l->s + col, l->len - col + 1);
    memcpy(l->s + col, t, n);
    l->len += n;
}

/* insert raw text (with \n line breaks, \r ignored); returns end position */
static Pos raw_insert(Doc *d, Pos p, const char *t, int n)
{
    int i, start = 0, nl = 0; Pos e; char *tail; int tlen;
    for (i = 0; i < n; i++) if (t[i] == '\n') nl++;
    doc_dirty_from(d, p.line);
    if (!nl) {
        char *clean = (char *)malloc(n + 1); int k = 0;
        for (i = 0; i < n; i++) if (t[i] != '\r') clean[k++] = t[i];
        line_insert(&d->ln[p.line], p.col, clean, k);
        free(clean);
        e.line = p.line; e.col = p.col + k;
        return e;
    }
    tlen = d->ln[p.line].len - p.col;
    tail = (char *)malloc(tlen + 1);
    memcpy(tail, d->ln[p.line].s + p.col, tlen);
    d->ln[p.line].len = p.col; d->ln[p.line].s[p.col] = 0;
    doc_insert_lines(d, p.line + 1, nl);
    e.line = p.line;
    for (i = 0; i <= n; i++) {
        if (i == n || t[i] == '\n') {
            int k, segn = i - start; Line *l = &d->ln[e.line];
            for (k = start; k < i; k++) if (t[k] != '\r') { line_reserve(l, l->len + 1); l->s[l->len++] = t[k]; }
            l->s[l->len] = 0;
            (void)segn;
            if (i < n) { e.line++; start = i + 1; }
        }
    }
    e.col = d->ln[e.line].len;
    line_insert(&d->ln[e.line], e.col, tail, tlen);
    free(tail);
    return e;
}

static char *raw_get(Doc *d, Pos a, Pos b, int *outlen)
{
    int size = 0, i, k = 0; char *r;
    for (i = a.line; i <= b.line; i++) size += d->ln[i].len + 1;
    r = (char *)malloc(size + 1);
    for (i = a.line; i <= b.line; i++) {
        int from = i == a.line ? a.col : 0, to = i == b.line ? b.col : d->ln[i].len;
        memcpy(r + k, d->ln[i].s + from, to - from); k += to - from;
        if (i < b.line) r[k++] = '\n';
    }
    r[k] = 0;
    if (outlen) *outlen = k;
    return r;
}

static void raw_delete(Doc *d, Pos a, Pos b)
{
    Line *la = &d->ln[a.line];
    doc_dirty_from(d, a.line);
    if (a.line == b.line) {
        memmove(la->s + a.col, la->s + b.col, la->len - b.col + 1);
        la->len -= b.col - a.col;
        return;
    }
    {
        Line *lb = &d->ln[b.line]; int tl = lb->len - b.col;
        line_reserve(la, a.col + tl);
        memcpy(la->s + a.col, lb->s + b.col, tl);
        la->len = a.col + tl; la->s[la->len] = 0;
        doc_remove_lines(d, a.line + 1, b.line - a.line);
    }
}

static int pos_cmp(Pos a, Pos b) { return a.line != b.line ? a.line - b.line : a.col - b.col; }
static int has_sel(Doc *d) { return pos_cmp(d->caret, d->anchor) != 0; }
static void sel_range(Doc *d, Pos *a, Pos *b)
{
    if (pos_cmp(d->caret, d->anchor) < 0) { *a = d->caret; *b = d->anchor; }
    else { *a = d->anchor; *b = d->caret; }
}

static Doc *doc_alloc(void)
{
    Doc *d = (Doc *)calloc(1, sizeof(Doc));
    doc_reserve(d, 1);
    doc_insert_lines(d, 0, 1);
    d->wantx = -1;
    return d;
}

static void doc_clear_undo(Doc *d)
{
    int i;
    for (i = 0; i < d->nu; i++) free(d->u[i].text);
    d->nu = d->upos = d->savepos = 0;
}

static void doc_free(Doc *d)
{
    int i;
    for (i = 0; i < d->n; i++) free(d->ln[i].s);
    doc_clear_undo(d);
    free(d->u); free(d->ln); free(d->st); free(d);
}

static void doc_set_text(Doc *d, const char *t, int n)
{
    Pos z = { 0, 0 };
    while (d->n > 1) doc_remove_lines(d, d->n - 1, 1);
    d->ln[0].len = 0; d->ln[0].s[0] = 0;
    raw_insert(d, z, t, n);
    d->stvalid = 0; d->st[0] = 0;
}

static int get_mtime(const char *path, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesEx(path, GetFileExInfoStandard, &a)) return 0;
    *ft = a.ftLastWriteTime;
    return 1;
}

/* =========================================================== undo */

static void undo_push(Doc *d, int type, Pos a, Pos b, const char *text, int len)
{
    int i; Undo *u;
    for (i = d->upos; i < d->nu; i++) free(d->u[i].text);
    if (d->savepos > d->upos) d->savepos = -1;
    d->nu = d->upos;
    if (d->nu == d->capu) { d->capu = d->capu ? d->capu * 2 : 64; d->u = (Undo *)realloc(d->u, d->capu * sizeof(Undo)); }
    u = &d->u[d->nu++];
    u->type = type; u->a = a; u->b = b; u->len = len; u->group = d->group;
    u->text = (char *)malloc(len + 1); memcpy(u->text, text, len); u->text[len] = 0;
    d->upos = d->nu;
}

static void ed_begin(Doc *d, int kind)
{
    DWORD now = GetTickCount();
    if (kind == K_OTHER || kind != d->lastKind || now - d->lastEdit > 1500) d->group++;
    d->lastKind = kind; d->lastEdit = now;
}

static Pos ed_insert_at(Doc *d, Pos p, const char *t, int n)
{
    Pos e = raw_insert(d, p, t, n);
    undo_push(d, U_INS, p, e, t, n);
    return e;
}

static void ed_delete_range(Doc *d, Pos a, Pos b)
{
    int len; char *t;
    if (!pos_cmp(a, b)) return;
    t = raw_get(d, a, b, &len);
    raw_delete(d, a, b);
    undo_push(d, U_DEL, a, b, t, len);
    free(t);
}

static int ed_delete_selection(Doc *d)
{
    Pos a, b;
    if (!has_sel(d)) return 0;
    sel_range(d, &a, &b);
    ed_delete_range(d, a, b);
    d->caret = d->anchor = a;
    return 1;
}

static void after_edit(Doc *d);

static void do_undo(Doc *d, int redo)
{
    int g;
    if (!redo) {
        if (!d->upos) return;
        g = d->u[d->upos - 1].group;
        while (d->upos > 0 && d->u[d->upos - 1].group == g) {
            Undo *u = &d->u[--d->upos];
            if (u->type == U_INS) { raw_delete(d, u->a, u->b); d->caret = u->a; }
            else d->caret = raw_insert(d, u->a, u->text, u->len);
        }
    } else {
        if (d->upos >= d->nu) return;
        g = d->u[d->upos].group;
        while (d->upos < d->nu && d->u[d->upos].group == g) {
            Undo *u = &d->u[d->upos++];
            if (u->type == U_INS) d->caret = raw_insert(d, u->a, u->text, u->len);
            else { raw_delete(d, u->a, u->b); d->caret = u->a; }
        }
    }
    d->anchor = d->caret;
    d->group++;
    after_edit(d);
}

/* =========================================================== syntax highlighting */

enum { CL_TEXT, CL_KW, CL_CTRL, CL_TYPE, CL_STR, CL_COM, CL_NUM, CL_PRE, CL_FUNC, CL_VAR, CL_CONST, CL_ESC, CL_TAG, CL_HEAD };
static const COLORREF s_clr[] = {
    HEX(0xD4D4D4), HEX(0x569CD6), HEX(0xC586C0), HEX(0x4EC9B0), HEX(0xCE9178), HEX(0x6A9955),
    HEX(0xB5CEA8), HEX(0xC586C0), HEX(0xDCDCAA), HEX(0x9CDCFE), HEX(0x4FC1FF), HEX(0xD7BA7D),
    HEX(0x569CD6), HEX(0x569CD6)
};

static const char *kw_c_ctrl[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "try", "catch", "throw", NULL };
static const char *kw_c[] = { "struct", "union", "enum", "typedef", "static", "extern", "const", "volatile",
    "register", "sizeof", "inline", "auto", "signed", "unsigned", "void", "char", "short", "int", "long",
    "float", "double", "class", "public", "private", "protected", "new", "delete", "this", "virtual",
    "namespace", "using", "template", "bool", "true", "false", "NULL", "CALLBACK", "WINAPI", NULL };
static const char *ty_c[] = { "BOOL", "BYTE", "WORD", "DWORD", "UINT", "LONG", "HWND", "HDC", "HFONT", "HBRUSH",
    "HPEN", "HBITMAP", "HICON", "HMENU", "HANDLE", "HINSTANCE", "LPARAM", "WPARAM", "LRESULT", "RECT", "POINT",
    "SIZE", "MSG", "WNDCLASS", "PAINTSTRUCT", "COLORREF", "TCHAR", "LPSTR", "LPCSTR", "FILE", "size_t",
    "Doc", "Line", "Pos", "Undo", "Term", "Node", "INT_PTR", "UINT_PTR", "DWORD_PTR", "va_list", NULL };
static const char *kw_py_ctrl[] = { "if", "elif", "else", "for", "while", "try", "except", "finally", "with",
    "return", "yield", "break", "continue", "pass", "raise", "import", "from", "as", "assert", "await", NULL };
static const char *kw_py[] = { "def", "class", "lambda", "and", "or", "not", "in", "is", "None", "True", "False",
    "global", "nonlocal", "del", "async", "self", "print", NULL };
static const char *kw_js_ctrl[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break",
    "continue", "return", "try", "catch", "finally", "throw", "import", "export", "from", NULL };
static const char *kw_js[] = { "function", "var", "let", "const", "new", "this", "class", "extends", "typeof",
    "instanceof", "null", "undefined", "true", "false", "in", "of", "delete", "void", "async", "await", NULL };
static const char *kw_bat[] = { "echo", "set", "if", "else", "goto", "call", "for", "in", "do", "exit", "not",
    "exist", "defined", "errorlevel", "setlocal", "endlocal", "shift", "pause", "cls", "cd", "copy", "del",
    "start", "mkdir", "md", "rd", "move", "ren", "type", "off", "on", "equ", "neq", "lss", "gtr", NULL };

static int in_list(const char **list, const char *w, int n, int icase)
{
    int i;
    for (i = 0; list[i]; i++)
        if ((int)strlen(list[i]) == n && (icase ? !_strnicmp(list[i], w, n) : !strncmp(list[i], w, n))) return 1;
    return 0;
}

static int is_ident(int c) { return isalnum(c) || c == '_'; }

#define MARK(from, to, cl) do { int _k; if (cls) for (_k = (from); _k < (to); _k++) cls[_k] = (unsigned char)(cl); } while (0)

/* scan one line; returns end-of-line state. state: 0 normal, 1 block comment, 2 py """ , 3 py ''' */
static int hl_line(Doc *d, int li, int state, unsigned char *cls)
{
    const char *s = d->ln[li].s; int n = d->ln[li].len, i = 0, lang = d->lang;
    MARK(0, n, CL_TEXT);
    if (lang == LANG_PLAIN) return 0;
    if (lang == LANG_MD) {
        if (n && s[0] == '#') { MARK(0, n, CL_HEAD); return 0; }
        for (i = 0; i < n; i++) if (s[i] == '`') { int j = i + 1; while (j < n && s[j] != '`') j++; MARK(i, j < n ? j + 1 : n, CL_STR); i = j; }
        else if (s[i] == '*' || (s[i] == '-' && i == 0)) MARK(i, i + 1, CL_KW);
        return 0;
    }
    if (lang == LANG_INI) {
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i < n && (s[i] == ';' || s[i] == '#')) { MARK(i, n, CL_COM); return 0; }
        if (i < n && s[i] == '[') { MARK(i, n, CL_KW); return 0; }
        { const char *eq = memchr(s, '=', n); if (eq) { MARK(i, (int)(eq - s), CL_VAR); MARK((int)(eq - s) + 1, n, CL_STR); } }
        return 0;
    }
    if (lang == LANG_BAT) {
        int j;
        while (i < n && (isspace((unsigned char)s[i]) || s[i] == '@')) i++;
        if ((n - i >= 3 && !_strnicmp(s + i, "rem", 3) && (n - i == 3 || isspace((unsigned char)s[i + 3]))) ||
            (n - i >= 2 && s[i] == ':' && s[i + 1] == ':')) { MARK(i, n, CL_COM); return 0; }
        if (i < n && s[i] == ':') { MARK(i, n, CL_FUNC); return 0; }
        while (i < n) {
            if (s[i] == '%') {
                j = i + 1;
                if (j < n && s[j] == '~') { j++; while (j < n && isalpha((unsigned char)s[j])) j++; if (j < n) j++; }
                else if (j < n && isdigit((unsigned char)s[j])) j++;
                else { while (j < n && s[j] != '%' && !isspace((unsigned char)s[j])) j++; if (j < n && s[j] == '%') j++; }
                MARK(i, j, CL_CONST); i = j;
            } else if (s[i] == '"') {
                j = i + 1; while (j < n && s[j] != '"') j++; if (j < n) j++;
                MARK(i, j, CL_STR); i = j;
            } else if (is_ident((unsigned char)s[i])) {
                j = i; while (j < n && (is_ident((unsigned char)s[j]) || s[j] == '.')) j++;
                if (in_list(kw_bat, s + i, j - i, 1)) MARK(i, j, CL_KW);
                else if (isdigit((unsigned char)s[i])) MARK(i, j, CL_NUM);
                i = j;
            } else if (strchr("|&<>()", s[i])) { MARK(i, i + 1, CL_CTRL); i++; }
            else i++;
        }
        return 0;
    }
    if (lang == LANG_HTML) {
        while (i < n) {
            if (state == 1) {
                const char *e = strstr(s + i, "-->");
                int end = e ? (int)(e - s) + 3 : n;
                MARK(i, end, CL_COM); i = end;
                if (e) state = 0;
                continue;
            }
            if (!strncmp(s + i, "<!--", 4)) { state = 1; continue; }
            if (s[i] == '<') {
                int j = i + 1;
                MARK(i, j, CL_TEXT);
                if (j < n && s[j] == '/') j++;
                { int k = j; while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '-' || s[k] == '!')) k++; MARK(j, k, CL_TAG); j = k; }
                while (j < n && s[j] != '>') {
                    if (s[j] == '"' || s[j] == '\'') { int q = s[j], k = j + 1; while (k < n && s[k] != q) k++; if (k < n) k++; MARK(j, k, CL_STR); j = k; }
                    else if (isalpha((unsigned char)s[j])) { int k = j; while (k < n && (isalnum((unsigned char)s[k]) || s[k] == '-')) k++; MARK(j, k, CL_VAR); j = k; }
                    else j++;
                }
                i = j < n ? j + 1 : n;
            } else if (s[i] == '&') { int j = i; while (j < n && s[j] != ';' && !isspace((unsigned char)s[j])) j++; MARK(i, j < n ? j + 1 : n, CL_ESC); i = j + 1; }
            else i++;
        }
        return state;
    }

    /* C / JS / Python */
    if (lang == LANG_C) {
        int j = 0;
        while (j < n && isspace((unsigned char)s[j])) j++;
        if (state == 0 && j < n && s[j] == '#') {
            int k = j + 1, inc;
            while (k < n && isspace((unsigned char)s[k])) k++;
            while (k < n && isalpha((unsigned char)s[k])) k++;
            MARK(j, k, CL_PRE);
            inc = (k - j >= 8 && strstr(s + j, "include") != NULL);
            if (inc) { MARK(k, n, CL_STR); return 0; }
            i = k;
        }
    }
    while (i < n) {
        int c = (unsigned char)s[i], j;
        if (state == 1) {
            const char *e = strstr(s + i, "*/");
            int end = e ? (int)(e - s) + 2 : n;
            MARK(i, end, CL_COM); i = end;
            if (e) state = 0;
            continue;
        }
        if (state == 2 || state == 3) {
            const char *e = strstr(s + i, state == 2 ? "\"\"\"" : "'''");
            int end = e ? (int)(e - s) + 3 : n;
            MARK(i, end, CL_STR); i = end;
            if (e) state = 0;
            continue;
        }
        if (lang == LANG_PY && c == '#') { MARK(i, n, CL_COM); break; }
        if (lang != LANG_PY && c == '/' && i + 1 < n && s[i + 1] == '/') { MARK(i, n, CL_COM); break; }
        if (lang != LANG_PY && c == '/' && i + 1 < n && s[i + 1] == '*') { state = 1; MARK(i, i + 2, CL_COM); i += 2; continue; }
        if (lang == LANG_PY && (c == '"' || c == '\'') && i + 2 < n && s[i + 1] == c && s[i + 2] == c) {
            state = c == '"' ? 2 : 3; MARK(i, i + 3, CL_STR); i += 3; continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && lang == LANG_JS)) {
            j = i + 1;
            while (j < n && s[j] != c) {
                if (s[j] == '\\' && j + 1 < n) { MARK(j, j + 2, CL_ESC); j += 2; continue; }
                j++;
            }
            if (j < n) j++;
            { int k; if (cls) for (k = i; k < j; k++) if (cls[k] != CL_ESC) cls[k] = CL_STR; }
            i = j; continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < n && isdigit((unsigned char)s[i + 1]))) {
            j = i; while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '.')) j++;
            MARK(i, j, CL_NUM); i = j; continue;
        }
        if (is_ident(c)) {
            int k, allcaps = 1, len;
            j = i; while (j < n && is_ident((unsigned char)s[j])) { if (islower((unsigned char)s[j])) allcaps = 0; j++; }
            len = j - i;
            k = j; while (k < n && s[k] == ' ') k++;
            if (lang == LANG_C && in_list(kw_c_ctrl, s + i, len, 0)) MARK(i, j, CL_CTRL);
            else if (lang == LANG_C && in_list(kw_c, s + i, len, 0)) MARK(i, j, CL_KW);
            else if (lang == LANG_C && in_list(ty_c, s + i, len, 0)) MARK(i, j, CL_TYPE);
            else if (lang == LANG_PY && in_list(kw_py_ctrl, s + i, len, 0)) MARK(i, j, CL_CTRL);
            else if (lang == LANG_PY && in_list(kw_py, s + i, len, 0)) MARK(i, j, CL_KW);
            else if (lang == LANG_JS && in_list(kw_js_ctrl, s + i, len, 0)) MARK(i, j, CL_CTRL);
            else if (lang == LANG_JS && in_list(kw_js, s + i, len, 0)) MARK(i, j, CL_KW);
            else if (k < n && s[k] == '(') MARK(i, j, CL_FUNC);
            else if (allcaps && len > 1 && !isdigit((unsigned char)s[i])) MARK(i, j, CL_CONST);
            else if (len > 2 && s[j - 2] == '_' && s[j - 1] == 't') MARK(i, j, CL_TYPE);
            else if (lang == LANG_PY && i >= 6 && !strncmp(s + i - 6, "class ", 6)) MARK(i, j, CL_TYPE);
            else MARK(i, j, CL_VAR);
            i = j; continue;
        }
        if (lang == LANG_PY && c == '@') { j = i + 1; while (j < n && (is_ident((unsigned char)s[j]) || s[j] == '.')) j++; MARK(i, j, CL_FUNC); i = j; continue; }
        i++;
    }
    return state;
}

static void ensure_states(Doc *d, int upto)
{
    if (upto > d->n - 1) upto = d->n - 1;
    d->st[0] = 0;
    while (d->stvalid < upto) {
        d->st[d->stvalid + 1] = (unsigned char)hl_line(d, d->stvalid, d->st[d->stvalid], NULL);
        d->stvalid++;
    }
}

/* =========================================================== geometry */

static int text_top(void) { return TAB_H + CRUMB_H; }

static int gutter_w(Doc *d)
{
    int digits = 1, n = d->n;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 3) digits = 3;
    return 14 + digits * g_codeCW + 26;
}

static void client_size(int *w, int *h) { RECT r; GetClientRect(g_hwndEditor, &r); *w = r.right; *h = r.bottom; }
static int text_right(void) { int w, h; client_size(&w, &h); return w - SB_W - (g_minimap ? MM_W : 0); }
static int vis_lines(void) { int w, h; client_size(&w, &h); h -= text_top(); return h > 0 ? h / g_codeCH : 1; }
static int vis_cols(Doc *d) { int c = (text_right() - gutter_w(d)) / g_codeCW; return c > 1 ? c : 1; }

static int col_to_vx(Line *l, int col)
{
    int i, vx = 0;
    for (i = 0; i < col && i < l->len; i++) vx = l->s[i] == '\t' ? (vx / TABSIZE + 1) * TABSIZE : vx + 1;
    return vx;
}

static int vx_to_col(Line *l, int vx)
{
    int i, x = 0;
    for (i = 0; i < l->len; i++) {
        int nx = l->s[i] == '\t' ? (x / TABSIZE + 1) * TABSIZE : x + 1;
        if (vx < (x + nx + 1) / 2 + (nx - x > 1 ? 0 : 0) || vx < nx && vx - x < nx - vx) return i;
        x = nx;
    }
    return l->len;
}

static void update_caret(void)
{
    Doc *d = cur();
    if (GetFocus() != g_hwndEditor) return;
    if (!is_text(d)) { SetCaretPos(-100, -100); return; }
    {
        int x = gutter_w(d) + (col_to_vx(&d->ln[d->caret.line], d->caret.col) - d->left) * g_codeCW;
        int y = text_top() + (d->caret.line - d->top) * g_codeCH;
        if (x < gutter_w(d) || x > text_right()) x = -100;
        SetCaretPos(x, y + 1);
    }
}

static void clamp_scroll(Doc *d)
{
    if (d->top > d->n - 1) d->top = d->n - 1;
    if (d->top < 0) d->top = 0;
    if (d->left < 0) d->left = 0;
}

static void ensure_visible(Doc *d)
{
    int vis = vis_lines(), cols = vis_cols(d), vx;
    if (text_right() - gutter_w(d) < 10 * g_codeCW) return;   /* not laid out yet */
    if (d->caret.line < d->top) d->top = d->caret.line;
    if (d->caret.line >= d->top + vis) d->top = d->caret.line - vis + 1;
    vx = col_to_vx(&d->ln[d->caret.line], d->caret.col);
    if (vx < d->left) d->left = vx > 4 ? vx - 4 : 0;
    if (vx >= d->left + cols - 1) d->left = vx - cols + 6;
    clamp_scroll(d);
}

static void redraw(void) { InvalidateRect(g_hwndEditor, NULL, FALSE); update_caret(); }

static void caret_moved(Doc *d, int keepWant)
{
    if (!keepWant) d->wantx = -1;
    ensure_visible(d);
    redraw();
    status_refresh();
}

static void after_edit(Doc *d)
{
    static int lastDirty = -1;
    int dirty = d->upos != d->savepos;
    d->wantx = -1;
    ensure_visible(d);
    redraw();
    status_refresh();
    if (dirty != lastDirty) { lastDirty = dirty; app_update_title(); }
    InvalidateRect(g_hwndEditor, NULL, FALSE);
}

/* =========================================================== find */

static int find_in_line(Line *l, int from, const char *q, int qn)
{
    int i;
    if (qn == 0) return -1;
    for (i = from; i + qn <= l->len; i++)
        if (!_strnicmp(l->s + i, q, qn)) return i;
    return -1;
}

static void find_count(Doc *d)
{
    int i, qn = (int)strlen(s_find), c; Pos a, b;
    s_matchCount = 0; s_matchIndex = 0;
    if (!is_text(d) || !qn) return;
    sel_range(d, &a, &b);
    for (i = 0; i < d->n; i++) {
        c = 0;
        while ((c = find_in_line(&d->ln[i], c, s_find, qn)) >= 0) {
            s_matchCount++;
            if (i == a.line && c == a.col) s_matchIndex = s_matchCount;
            c += qn;
        }
    }
}

static int find_step(Doc *d, int dir, int fromSelStart)
{
    int qn = (int)strlen(s_find), i, line, col, c, k;
    Pos a, b;
    if (!is_text(d) || !qn) return 0;
    sel_range(d, &a, &b);
    if (dir > 0) {
        line = fromSelStart ? a.line : b.line; col = fromSelStart ? a.col : b.col;
        for (k = 0; k <= d->n; k++) {
            i = (line + k) % d->n;
            c = find_in_line(&d->ln[i], k == 0 ? col : 0, s_find, qn);
            if (c >= 0) { d->anchor.line = i; d->anchor.col = c; d->caret.line = i; d->caret.col = c + qn; goto found; }
        }
    } else {
        line = a.line; col = a.col;
        for (k = 0; k <= d->n; k++) {
            int best = -1;
            i = ((line - k) % d->n + d->n) % d->n;
            c = 0;
            while ((c = find_in_line(&d->ln[i], c, s_find, qn)) >= 0) {
                if (k == 0 && c >= col) break;
                best = c; c++;
            }
            if (best >= 0) { d->anchor.line = i; d->anchor.col = best; d->caret.line = i; d->caret.col = best + qn; goto found; }
        }
    }
    find_count(d); redraw();
    return 0;
found:
    {
        int vis = vis_lines();
        if (d->caret.line < d->top || d->caret.line >= d->top + vis) d->top = d->caret.line - vis / 3;
        clamp_scroll(d);
    }
    find_count(d);
    caret_moved(d, 0);
    return 1;
}

static void layout_find(void)
{
    int w, h, x, y, fw = 430;
    client_size(&w, &h);
    x = w - SB_W - (g_minimap ? MM_W : 0) - fw - 10; if (x < 40) x = 40;
    y = text_top();
    SetRect(&s_findRc, x, y, x + fw, y + (s_replOpen ? 64 : 34));
    if (s_findEdit) {
        MoveWindow(s_findEdit, x + 30, y + 7, 220, 18, TRUE);
        ShowWindow(s_findEdit, s_findOpen ? SW_SHOW : SW_HIDE);
        MoveWindow(s_replEdit, x + 30, y + 37, 220, 18, TRUE);
        ShowWindow(s_replEdit, s_findOpen && s_replOpen ? SW_SHOW : SW_HIDE);
    }
    SetRect(&s_findBtns[0], x + 336, y + 6, x + 358, y + 28);
    SetRect(&s_findBtns[1], x + 360, y + 6, x + 382, y + 28);
    SetRect(&s_findBtns[2], x + 400, y + 6, x + 422, y + 28);
    SetRect(&s_findBtns[3], x + 262, y + 36, x + 318, y + 58);
    SetRect(&s_findBtns[4], x + 322, y + 36, x + 390, y + 58);
}

static void find_close(void)
{
    s_findOpen = 0; layout_find(); SetFocus(g_hwndEditor); redraw();
}

static void replace_one(Doc *d)
{
    char rep[256]; Pos a, b; int len;
    char *t;
    if (!is_text(d) || !s_find[0]) return;
    GetWindowText(s_replEdit, rep, sizeof rep);
    if (has_sel(d)) {
        sel_range(d, &a, &b);
        t = raw_get(d, a, b, &len);
        if (!_stricmp(t, s_find)) {
            ed_begin(d, K_OTHER);
            ed_delete_selection(d);
            d->caret = d->anchor = ed_insert_at(d, d->caret, rep, (int)strlen(rep));
            after_edit(d);
        }
        free(t);
    }
    find_step(d, 1, 0);
}

static void replace_all(Doc *d)
{
    char rep[256]; int i, c, qn = (int)strlen(s_find), rn, count = 0;
    if (!is_text(d) || !qn) return;
    GetWindowText(s_replEdit, rep, sizeof rep); rn = (int)strlen(rep);
    ed_begin(d, K_OTHER);
    for (i = 0; i < d->n; i++) {
        c = 0;
        while ((c = find_in_line(&d->ln[i], c, s_find, qn)) >= 0) {
            Pos a, b; a.line = b.line = i; a.col = c; b.col = c + qn;
            ed_delete_range(d, a, b);
            ed_insert_at(d, a, rep, rn);
            c += rn; count++;
        }
    }
    if (d->caret.col > d->ln[d->caret.line].len) d->caret.col = d->ln[d->caret.line].len;
    d->anchor = d->caret;
    after_edit(d); find_count(d);
    out_log("Replaced %d occurrence(s) of '%s'", count, s_find);
}

static LRESULT CALLBACK FindEditProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    Doc *d = cur();
    if (m == WM_KEYDOWN) {
        int shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wp == VK_RETURN) {
            if (w == s_replEdit) { if (ctrl) replace_all(d); else replace_one(d); }
            else find_step(d, shift ? -1 : 1, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) { find_close(); return 0; }
        if (wp == VK_TAB) { SetFocus(w == s_findEdit && s_replOpen ? s_replEdit : s_findEdit); return 0; }
        if (wp == VK_F3) { find_step(d, shift ? -1 : 1, 0); return 0; }
        if (ctrl && wp == 'A') { SendMessage(w, EM_SETSEL, 0, -1); return 0; }
        if (ctrl && (wp == 'F' || wp == 'H')) {
            if (wp == 'H' && !s_replOpen) { s_replOpen = 1; layout_find(); redraw(); }
            SendMessage(w, EM_SETSEL, 0, -1); return 0;
        }
    }
    if (m == WM_CHAR && (wp == 13 || wp == 27 || wp == 9 || wp == 1)) return 0;
    return CallWindowProc(s_editOrig, w, m, wp, lp);
}

void editor_find_show(int replace)
{
    Doc *d = cur();
    if (!is_text(d)) return;
    s_findOpen = 1; if (replace) s_replOpen = 1;
    if (has_sel(d)) {
        Pos a, b; sel_range(d, &a, &b);
        if (a.line == b.line && b.col - a.col < 200) {
            char *t = raw_get(d, a, b, NULL); SetWindowText(s_findEdit, t); free(t);
        }
    }
    layout_find();
    SetFocus(replace ? s_replEdit : s_findEdit);
    SendMessage(replace ? s_replEdit : s_findEdit, EM_SETSEL, 0, -1);
    find_count(d);
    redraw();
}

/* =========================================================== editing operations */

static void type_text(Doc *d, const char *t, int n, int kind)
{
    ed_begin(d, kind);
    ed_delete_selection(d);
    d->caret = d->anchor = ed_insert_at(d, d->caret, t, n);
    after_edit(d);
}

static int leading_ws(Line *l) { int i = 0; while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++; return i; }

static void do_enter(Doc *d)
{
    Line *l = &d->ln[d->caret.line];
    char buf[512]; int ind = leading_ws(l), n, p, extra = 0, between = 0;
    if (ind > d->caret.col) ind = d->caret.col;
    if (ind > 400) ind = 400;
    p = d->caret.col - 1;
    while (p >= 0 && l->s[p] == ' ') p--;
    if (p >= 0 && (l->s[p] == '{' || l->s[p] == '[' || l->s[p] == '(' || (d->lang == LANG_PY && l->s[p] == ':'))) extra = 1;
    if (extra && d->caret.col < l->len && p >= 0 &&
        ((l->s[p] == '{' && l->s[d->caret.col] == '}') || (l->s[p] == '(' && l->s[d->caret.col] == ')') ||
         (l->s[p] == '[' && l->s[d->caret.col] == ']'))) between = 1;
    n = 0; buf[n++] = '\n';
    memcpy(buf + n, l->s, ind); n += ind;
    if (extra) { memcpy(buf + n, "    ", 4); n += 4; }
    ed_begin(d, K_OTHER);
    ed_delete_selection(d);
    d->caret = d->anchor = ed_insert_at(d, d->caret, buf, n);
    if (between) {
        Pos keep = d->caret;
        ed_insert_at(d, d->caret, buf, n - 4);
        d->caret = d->anchor = keep;
    }
    after_edit(d);
}

static void do_backspace(Doc *d, int word)
{
    Pos a = d->caret;
    if (ed_delete_selection(d)) { after_edit(d); return; }
    if (a.col == 0 && a.line == 0) return;
    ed_begin(d, K_DELETE);
    if (a.col == 0) { a.line--; a.col = d->ln[a.line].len; }
    else {
        Line *l = &d->ln[a.line];
        if (word) {
            while (a.col > 0 && l->s[a.col - 1] == ' ') a.col--;
            if (a.col > 0 && is_ident((unsigned char)l->s[a.col - 1])) while (a.col > 0 && is_ident((unsigned char)l->s[a.col - 1])) a.col--;
            else if (a.col > 0 && a.col == d->caret.col) a.col--;
        } else if (leading_ws(l) >= a.col && l->s[a.col - 1] == ' ') {
            int target = ((a.col - 1) / TABSIZE) * TABSIZE;
            while (a.col > target && l->s[a.col - 1] == ' ') a.col--;
        } else {
            static const char *pairs = "()[]{}\"\"''";
            const char *q;
            for (q = pairs; *q; q += 2)
                if (l->s[a.col - 1] == q[0] && a.col < l->len && l->s[a.col] == q[1]) {
                    Pos e = d->caret; e.col++; ed_delete_range(d, d->caret, e); break;
                }
            a.col--;
        }
    }
    ed_delete_range(d, a, d->caret);
    d->caret = d->anchor = a;
    after_edit(d);
}

static void do_delete(Doc *d, int word)
{
    Pos b = d->caret; Line *l = &d->ln[b.line];
    if (ed_delete_selection(d)) { after_edit(d); return; }
    if (b.col >= l->len) { if (b.line >= d->n - 1) return; b.line++; b.col = 0; }
    else if (word) { while (b.col < l->len && is_ident((unsigned char)l->s[b.col])) b.col++; while (b.col < l->len && l->s[b.col] == ' ') b.col++; if (b.col == d->caret.col) b.col++; }
    else b.col++;
    ed_begin(d, K_DELETE);
    ed_delete_range(d, d->caret, b);
    d->anchor = d->caret;
    after_edit(d);
}

static void sel_lines(Doc *d, int *l1, int *l2)
{
    Pos a, b; sel_range(d, &a, &b);
    *l1 = a.line; *l2 = b.line;
    if (b.line > a.line && b.col == 0) (*l2)--;
}

static void indent_lines(Doc *d, int out)
{
    int l1, l2, i;
    sel_lines(d, &l1, &l2);
    ed_begin(d, K_OTHER);
    for (i = l1; i <= l2; i++) {
        Pos p; p.line = i; p.col = 0;
        if (!out) {
            if (d->ln[i].len == 0) continue;
            ed_insert_at(d, p, "    ", 4);
            if (d->caret.line == i && d->caret.col > 0) d->caret.col += 4;
            if (d->anchor.line == i && d->anchor.col > 0) d->anchor.col += 4;
        } else {
            Pos e = p; int k = 0;
            while (k < TABSIZE && k < d->ln[i].len && d->ln[i].s[k] == ' ') k++;
            if (k == 0 && d->ln[i].len && d->ln[i].s[0] == '\t') k = 1;
            if (!k) continue;
            e.col = k; ed_delete_range(d, p, e);
            if (d->caret.line == i) d->caret.col = d->caret.col > k ? d->caret.col - k : 0;
            if (d->anchor.line == i) d->anchor.col = d->anchor.col > k ? d->anchor.col - k : 0;
        }
    }
    after_edit(d);
}

static void toggle_comment(Doc *d)
{
    const char *tok = d->lang == LANG_PY || d->lang == LANG_INI ? "#" : d->lang == LANG_BAT ? "REM" :
                      (d->lang == LANG_C || d->lang == LANG_JS) ? "//" : NULL;
    int l1, l2, i, all = 1, minind = 9999, tn;
    if (!tok) return;
    tn = (int)strlen(tok);
    sel_lines(d, &l1, &l2);
    for (i = l1; i <= l2; i++) {
        Line *l = &d->ln[i]; int w = leading_ws(l);
        if (w == l->len) continue;
        if (w < minind) minind = w;
        if (_strnicmp(l->s + w, tok, tn)) all = 0;
    }
    if (minind == 9999) return;
    ed_begin(d, K_OTHER);
    for (i = l1; i <= l2; i++) {
        Line *l = &d->ln[i]; int w = leading_ws(l), delta;
        Pos p; p.line = i;
        if (w == l->len) continue;
        if (all) {
            Pos e; p.col = w; e.line = i; e.col = w + tn;
            if (e.col < l->len && l->s[e.col] == ' ') e.col++;
            delta = -(e.col - p.col);
            ed_delete_range(d, p, e);
        } else {
            char t[8]; sfmt(t, sizeof t, "%s ", tok);
            p.col = minind; ed_insert_at(d, p, t, tn + 1); delta = tn + 1;
        }
        if (d->caret.line == i && d->caret.col >= p.col) d->caret.col += delta;
        if (d->anchor.line == i && d->anchor.col >= p.col) d->anchor.col += delta;
        if (d->caret.col < 0) d->caret.col = 0;
        if (d->anchor.col < 0) d->anchor.col = 0;
    }
    after_edit(d);
}

static void move_lines(Doc *d, int dir, int copy)
{
    int l1, l2, len; Pos a, b; char *block, *buf; int blen;
    sel_lines(d, &l1, &l2);
    a.line = l1; a.col = 0; b.line = l2; b.col = d->ln[l2].len;
    block = raw_get(d, a, b, &blen);
    ed_begin(d, K_OTHER);
    if (copy) {
        buf = (char *)malloc(blen + 2); buf[0] = '\n'; memcpy(buf + 1, block, blen);
        ed_insert_at(d, b, buf, blen + 1);
        if (dir > 0) { d->caret.line += l2 - l1 + 1; d->anchor.line += l2 - l1 + 1; }
        free(buf);
    } else if (dir < 0 && l1 > 0) {
        char *prev = raw_get(d, (Pos){ l1 - 1, 0 }, (Pos){ l1 - 1, d->ln[l1 - 1].len }, &len);
        Pos s = { l1 - 1, 0 };
        buf = (char *)malloc(blen + len + 2);
        memcpy(buf, block, blen); buf[blen] = '\n'; memcpy(buf + blen + 1, prev, len);
        ed_delete_range(d, s, b);
        ed_insert_at(d, s, buf, blen + len + 1);
        d->caret.line--; d->anchor.line--;
        free(buf); free(prev);
    } else if (dir > 0 && l2 < d->n - 1) {
        char *next = raw_get(d, (Pos){ l2 + 1, 0 }, (Pos){ l2 + 1, d->ln[l2 + 1].len }, &len);
        Pos e = { l2 + 1, d->ln[l2 + 1].len };
        buf = (char *)malloc(blen + len + 2);
        memcpy(buf, next, len); buf[len] = '\n'; memcpy(buf + len + 1, block, blen);
        ed_delete_range(d, a, e);
        ed_insert_at(d, a, buf, blen + len + 1);
        d->caret.line++; d->anchor.line++;
        free(buf); free(next);
    }
    free(block);
    after_edit(d);
}

static void delete_lines(Doc *d)
{
    int l1, l2; Pos a, b;
    sel_lines(d, &l1, &l2);
    ed_begin(d, K_OTHER);
    if (l2 + 1 < d->n) { a.line = l1; a.col = 0; b.line = l2 + 1; b.col = 0; }
    else if (l1 > 0) { a.line = l1 - 1; a.col = d->ln[l1 - 1].len; b.line = l2; b.col = d->ln[l2].len; }
    else { a.line = 0; a.col = 0; b.line = l2; b.col = d->ln[l2].len; }
    ed_delete_range(d, a, b);
    d->caret.line = a.line < d->n - 1 && l2 + 1 < d->n + (l2 - l1) ? l1 : a.line;
    if (d->caret.line >= d->n) d->caret.line = d->n - 1;
    d->caret.col = 0; if (d->caret.col > d->ln[d->caret.line].len) d->caret.col = 0;
    d->anchor = d->caret;
    after_edit(d);
}

/* =========================================================== clipboard */

static void copy_text(const char *t, int n)
{
    HGLOBAL h; char *p; int i, k = 0, extra = 0;
    for (i = 0; i < n; i++) if (t[i] == '\n') extra++;
    h = GlobalAlloc(GMEM_MOVEABLE, n + extra + 1);
    p = (char *)GlobalLock(h);
    for (i = 0; i < n; i++) { if (t[i] == '\n') p[k++] = '\r'; p[k++] = t[i]; }
    p[k] = 0;
    GlobalUnlock(h);
    if (OpenClipboard(g_hwndEditor)) { EmptyClipboard(); SetClipboardData(CF_TEXT, h); CloseClipboard(); }
    else GlobalFree(h);
}

static void do_copy(Doc *d, int cut)
{
    Pos a, b; int n; char *t;
    if (has_sel(d)) {
        sel_range(d, &a, &b);
        t = raw_get(d, a, b, &n);
        copy_text(t, n); free(t);
        s_lineCopy = 0;
        if (cut) { ed_begin(d, K_OTHER); ed_delete_selection(d); after_edit(d); }
    } else {
        Line *l = &d->ln[d->caret.line];
        t = (char *)malloc(l->len + 2); memcpy(t, l->s, l->len); t[l->len] = '\n'; t[l->len + 1] = 0;
        copy_text(t, l->len + 1);
        free(s_lineCopyText); s_lineCopyText = t; s_lineCopy = 1;
        if (cut) delete_lines(d);
    }
}

static void do_paste(Doc *d)
{
    HANDLE h; char *p, *t; int n;
    if (!OpenClipboard(g_hwndEditor)) return;
    h = GetClipboardData(CF_TEXT);
    if (h && (p = (char *)GlobalLock(h)) != NULL) {
        n = (int)strlen(p);
        t = (char *)malloc(n + 1); memcpy(t, p, n + 1);
        GlobalUnlock(h);
        CloseClipboard();
        {   /* strip CR */
            int i, k = 0; for (i = 0; i < n; i++) if (t[i] != '\r') t[k++] = t[i]; t[k] = 0; n = k;
        }
        if (!has_sel(d) && s_lineCopy && s_lineCopyText && !strcmp(t, s_lineCopyText)) {
            Pos s = { d->caret.line, 0 };
            ed_begin(d, K_OTHER);
            ed_insert_at(d, s, t, n);
            d->caret.line++; d->anchor = d->caret;
            after_edit(d);
        } else type_text(d, t, n, K_OTHER);
        free(t);
        return;
    }
    CloseClipboard();
}

/* =========================================================== navigation */

static void move_to(Doc *d, Pos p, int extend)
{
    d->caret = p;
    if (!extend) d->anchor = p;
}

static Pos word_left(Doc *d, Pos p)
{
    Line *l = &d->ln[p.line];
    if (p.col == 0) { if (p.line > 0) { p.line--; p.col = d->ln[p.line].len; } return p; }
    while (p.col > 0 && isspace((unsigned char)l->s[p.col - 1])) p.col--;
    if (p.col > 0 && is_ident((unsigned char)l->s[p.col - 1])) while (p.col > 0 && is_ident((unsigned char)l->s[p.col - 1])) p.col--;
    else if (p.col > 0) p.col--;
    return p;
}

static Pos word_right(Doc *d, Pos p)
{
    Line *l = &d->ln[p.line];
    if (p.col >= l->len) { if (p.line < d->n - 1) { p.line++; p.col = 0; } return p; }
    while (p.col < l->len && isspace((unsigned char)l->s[p.col])) p.col++;
    if (p.col < l->len && is_ident((unsigned char)l->s[p.col])) while (p.col < l->len && is_ident((unsigned char)l->s[p.col])) p.col++;
    else if (p.col < l->len) p.col++;
    return p;
}

static void vertical(Doc *d, int delta, int extend)
{
    Pos p = d->caret;
    if (d->wantx < 0) d->wantx = col_to_vx(&d->ln[p.line], p.col);
    p.line += delta;
    if (p.line < 0) { p.line = 0; p.col = 0; d->wantx = 0; }
    else if (p.line >= d->n) { p.line = d->n - 1; p.col = d->ln[p.line].len; }
    else p.col = vx_to_col(&d->ln[p.line], d->wantx);
    move_to(d, p, extend);
    caret_moved(d, 1);
}

static void select_word_at(Doc *d, Pos p)
{
    Line *l = &d->ln[p.line]; int a = p.col, b = p.col;
    while (a > 0 && is_ident((unsigned char)l->s[a - 1])) a--;
    while (b < l->len && is_ident((unsigned char)l->s[b])) b++;
    d->anchor.line = d->caret.line = p.line; d->anchor.col = a; d->caret.col = b;
}

static int editor_keydown(Doc *d, int vk)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    Pos p = d->caret;
    Line *l = &d->ln[p.line];
    if (alt && !ctrl) {
        if (vk == VK_UP) { move_lines(d, -1, shift); return 1; }
        if (vk == VK_DOWN) { move_lines(d, 1, shift); return 1; }
        return 0;
    }
    switch (vk) {
    case VK_LEFT:
        if (!shift && has_sel(d) && !ctrl) { Pos a, b; sel_range(d, &a, &b); move_to(d, a, 0); }
        else if (ctrl) move_to(d, word_left(d, p), shift);
        else { if (p.col > 0) p.col--; else if (p.line > 0) { p.line--; p.col = d->ln[p.line].len; } move_to(d, p, shift); }
        caret_moved(d, 0); return 1;
    case VK_RIGHT:
        if (!shift && has_sel(d) && !ctrl) { Pos a, b; sel_range(d, &a, &b); move_to(d, b, 0); }
        else if (ctrl) move_to(d, word_right(d, p), shift);
        else { if (p.col < l->len) p.col++; else if (p.line < d->n - 1) { p.line++; p.col = 0; } move_to(d, p, shift); }
        caret_moved(d, 0); return 1;
    case VK_UP:
        if (ctrl) { d->top--; clamp_scroll(d); redraw(); return 1; }
        vertical(d, -1, shift); return 1;
    case VK_DOWN:
        if (ctrl) { d->top++; clamp_scroll(d); redraw(); return 1; }
        vertical(d, 1, shift); return 1;
    case VK_PRIOR: d->top -= vis_lines(); clamp_scroll(d); vertical(d, -vis_lines(), shift); return 1;
    case VK_NEXT:  d->top += vis_lines(); clamp_scroll(d); vertical(d, vis_lines(), shift); return 1;
    case VK_HOME:
        if (ctrl) { p.line = 0; p.col = 0; }
        else { int w = leading_ws(l); p.col = p.col == w ? 0 : w; }
        move_to(d, p, shift); caret_moved(d, 0); return 1;
    case VK_END:
        if (ctrl) { p.line = d->n - 1; }
        p.col = d->ln[p.line].len;
        move_to(d, p, shift); caret_moved(d, 0); return 1;
    case VK_BACK: do_backspace(d, ctrl); return 1;
    case VK_DELETE:
        if (shift && !ctrl) { do_copy(d, 1); return 1; }
        do_delete(d, ctrl); return 1;
    case VK_RETURN:
        if (ctrl) {
            Pos e = { p.line, 0 };
            if (shift) { ed_begin(d, K_OTHER); ed_insert_at(d, e, "\n", 1); d->caret = d->anchor = e; after_edit(d); }
            else { e.col = l->len; d->caret = d->anchor = e; do_enter(d); }
            return 1;
        }
        do_enter(d); return 1;
    case VK_TAB:
        if (ctrl) return 0;
        if (shift) { indent_lines(d, 1); return 1; }
        if (has_sel(d) && d->caret.line != d->anchor.line) { indent_lines(d, 0); return 1; }
        { int vx = col_to_vx(l, p.col), k = TABSIZE - vx % TABSIZE; type_text(d, "    ", k, K_TYPE); }
        return 1;
    case VK_ESCAPE:
        if (s_findOpen) { find_close(); return 1; }
        d->anchor = d->caret; redraw(); return 1;
    case VK_INSERT:
        if (ctrl) { do_copy(d, 0); return 1; }
        if (shift) { do_paste(d); return 1; }
        return 0;
    case VK_F3: if (!s_find[0]) editor_find_show(0); else find_step(d, shift ? -1 : 1, 0); return 1;
    }
    if (!ctrl) return 0;
    switch (vk) {
    case 'Z': do_undo(d, shift); return 1;
    case 'Y': do_undo(d, 1); return 1;
    case 'X': do_copy(d, 1); return 1;
    case 'C': do_copy(d, 0); return 1;
    case 'V': do_paste(d); return 1;
    case 'A': d->anchor.line = 0; d->anchor.col = 0; d->caret.line = d->n - 1; d->caret.col = d->ln[d->n - 1].len; caret_moved(d, 0); return 1;
    case 'F': editor_find_show(0); return 1;
    case 'H': editor_find_show(1); return 1;
    case 'L': d->anchor.line = p.line; d->anchor.col = 0;
              if (p.line + 1 < d->n) { d->caret.line = p.line + 1; d->caret.col = 0; } else d->caret.col = l->len;
              caret_moved(d, 0); return 1;
    case 'D': select_word_at(d, p); caret_moved(d, 0); return 1;
    case 'K': if (shift) { delete_lines(d); return 1; } return 0;
    case VK_OEM_2: case VK_DIVIDE: toggle_comment(d); return 1;
    case VK_OEM_6: indent_lines(d, 0); return 1;
    case VK_OEM_4: indent_lines(d, 1); return 1;
    }
    return 0;
}

static void editor_char(Doc *d, int ch)
{
    char c = (char)ch, two[2];
    Line *l = &d->ln[d->caret.line];
    int next = d->caret.col < l->len ? (unsigned char)l->s[d->caret.col] : 0;
    if (ch < 32 && ch != 9) return;
    if (ch == 9) return;
    /* typing over an auto-inserted closer */
    if (!has_sel(d) && (c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && next == (unsigned char)c) {
        d->caret.col++; d->anchor = d->caret; caret_moved(d, 0); return;
    }
    if ((c == '(' || c == '[' || c == '{' || ((c == '"' || c == '\'') && d->lang != LANG_PLAIN && d->lang != LANG_MD)) &&
        (next == 0 || isspace(next) || strchr(")]};,", next))) {
        char close = c == '(' ? ')' : c == '[' ? ']' : c == '{' ? '}' : c;
        int prev = d->caret.col > 0 ? (unsigned char)l->s[d->caret.col - 1] : 0;
        if (!((c == '"' || c == '\'') && is_ident(prev))) {
            if (has_sel(d) && d->caret.line == d->anchor.line) {   /* wrap selection */
                Pos a, b; char *t; int n; char *buf;
                sel_range(d, &a, &b); t = raw_get(d, a, b, &n);
                buf = (char *)malloc(n + 3); buf[0] = c; memcpy(buf + 1, t, n); buf[n + 1] = close;
                type_text(d, buf, n + 2, K_OTHER); free(buf); free(t);
                return;
            }
            two[0] = c; two[1] = close;
            type_text(d, two, 2, K_TYPE);
            d->caret.col--; d->anchor = d->caret; caret_moved(d, 0);
            return;
        }
    }
    type_text(d, &c, 1, K_TYPE);
    if (c == '}') {   /* outdent a lone closing brace */
        Line *ln = &d->ln[d->caret.line]; int w = leading_ws(ln);
        if (w == ln->len - 1 && w >= TABSIZE && d->caret.line > 0) {
            Pos a = { d->caret.line, 0 }, b = { d->caret.line, TABSIZE };
            ed_delete_range(d, a, b); d->caret.col -= TABSIZE; d->anchor = d->caret; after_edit(d);
        }
    }
}

/* =========================================================== documents */

static Doc *find_doc(const char *path, int *idx)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (s_docs[i]->path[0] && !_stricmp(s_docs[i]->path, path)) { if (idx) *idx = i; return s_docs[i]; }
    return NULL;
}

static void doc_label(Doc *d, char *buf, int n)
{
    if (d->kind == 1) sfmt(buf, n, "Welcome");
    else if (d->path[0]) sfmt(buf, n, "%s", path_name(d->path));
    else sfmt(buf, n, "Untitled-%d", d->untitled);
}

static void add_doc(Doc *d)
{
    if (s_n >= MAXDOCS) { editor_close(0); }
    s_docs[s_n++] = d;
    editor_activate(s_n - 1);
}

void editor_activate(int idx)
{
    if (idx < 0 || idx >= s_n) return;
    s_act = idx;
    if (is_text(cur())) { ensure_visible(cur()); find_count(cur()); }
    app_update_title();
    redraw();
    if (cur() && cur()->path[0]) sidebar_reveal(cur()->path);
}

Doc *editor_open(const char *path)
{
    char full[MAX_PATH]; int idx, len, i; char *t; Doc *d;
    GetFullPathName(path, MAX_PATH, full, NULL);
    if ((d = find_doc(full, &idx)) != NULL) { editor_activate(idx); return d; }
    t = read_file(full, &len);
    if (!t) { out_log("Could not open %s", full); MessageBox(g_hwndMain, "The file could not be opened.", APP_NAME, MB_ICONERROR); return NULL; }
    for (i = 0; i < len && i < 8000; i++) if (!t[i]) {
        char msg[MAX_PATH + 100];
        free(t);
        sfmt(msg, sizeof msg, "%s\n\nThe file is not displayed in the editor because it is either binary or uses an unsupported text encoding.", path_name(full));
        MessageBox(g_hwndMain, msg, APP_NAME, MB_ICONINFORMATION);
        return NULL;
    }
    d = doc_alloc();
    strcpy(d->path, full);
    d->lang = lang_from_path(full);
    doc_set_text(d, t, len);
    free(t);
    get_mtime(full, &d->mtime);
    add_doc(d);
    out_log("Opened %s (%d lines)", full, d->n);
    return d;
}

void editor_new_untitled(void)
{
    Doc *d = doc_alloc();
    d->untitled = ++s_untitledNo;
    add_doc(d);
    SetFocus(g_hwndEditor);
}

static int write_doc(Doc *d, const char *path)
{
    HANDLE h; DWORD wr; int i; BOOL ok = TRUE;
    h = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    for (i = 0; i < d->n && ok; i++) {
        if (d->ln[i].len) ok = WriteFile(h, d->ln[i].s, d->ln[i].len, &wr, NULL);
        if (i < d->n - 1) ok = ok && WriteFile(h, "\r\n", 2, &wr, NULL);
    }
    CloseHandle(h);
    return ok;
}

int editor_save(int idx, int saveas)
{
    Doc *d; char path[MAX_PATH];
    if (idx < 0 || idx >= s_n) return 0;
    d = s_docs[idx];
    if (d->kind != 0) return 1;
    if (!d->path[0] || saveas) {
        OPENFILENAME of; memset(&of, 0, sizeof of);
        if (d->path[0]) strcpy(path, d->path); else sfmt(path, sizeof path, "Untitled-%d.txt", d->untitled);
        of.lStructSize = sizeof of; of.hwndOwner = g_hwndMain;
        of.lpstrFilter = "All Files (*.*)\0*.*\0C Source (*.c)\0*.c\0Python (*.py)\0*.py\0Batch (*.bat)\0*.bat\0";
        of.lpstrFile = path; of.nMaxFile = MAX_PATH; of.lpstrInitialDir = g_root[0] ? g_root : NULL;
        of.Flags = OFN_OVERWRITEPROMPT | OFN_EXPLORER;
        if (!GetSaveFileName(&of)) return 0;
    } else strcpy(path, d->path);
    if (!write_doc(d, path)) {
        MessageBox(g_hwndMain, "Failed to save the file.", APP_NAME, MB_ICONERROR);
        return 0;
    }
    if (strcmp(d->path, path)) { strcpy(d->path, path); d->lang = lang_from_path(path); d->stvalid = 0; sidebar_refresh(); }
    d->savepos = d->upos;
    get_mtime(path, &d->mtime);
    out_log("Saved %s", path);
    app_update_title();
    redraw();
    return 1;
}

void editor_save_all(void)
{
    int i;
    for (i = 0; i < s_n; i++) if (s_docs[i]->kind == 0 && s_docs[i]->upos != s_docs[i]->savepos && s_docs[i]->path[0]) editor_save(i, 0);
}

int editor_close(int idx)
{
    Doc *d;
    if (idx < 0 || idx >= s_n) return 1;
    d = s_docs[idx];
    if (d->kind == 0 && d->upos != d->savepos) {
        char msg[MAX_PATH + 128], name[MAX_PATH]; int r;
        doc_label(d, name, sizeof name);
        sfmt(msg, sizeof msg, "Do you want to save the changes you made to %s?\n\nYour changes will be lost if you don't save them.", name);
        editor_activate(idx);
        r = MessageBox(g_hwndMain, msg, APP_NAME, MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !editor_save(idx, 0)) return 0;
    }
    doc_free(d);
    memmove(s_docs + idx, s_docs + idx + 1, (s_n - idx - 1) * sizeof(Doc *));
    s_n--;
    if (s_act >= s_n || s_act > idx) s_act--;
    if (s_act < 0 && s_n) s_act = 0;
    app_update_title();
    redraw();
    return 1;
}

int editor_close_all(void)
{
    while (s_n) if (!editor_close(s_n - 1)) return 0;
    return 1;
}

int editor_count(void) { return s_n; }
int editor_active_index(void) { Doc *d = cur(); return d && d->kind == 0 ? s_act : -1; }
const char *editor_doc_path(int idx) { return idx >= 0 && idx < s_n ? s_docs[idx]->path : ""; }
void editor_doc_label(int idx, char *buf, int n) { if (idx >= 0 && idx < s_n) doc_label(s_docs[idx], buf, n); else buf[0] = 0; }
int editor_doc_dirty(int idx) { return idx >= 0 && idx < s_n && s_docs[idx]->kind == 0 && s_docs[idx]->upos != s_docs[idx]->savepos; }
void editor_focus(void) { SetFocus(g_hwndEditor); }

void editor_status(int *line, int *col, int *lang, int *hasdoc, int *nlines)
{
    Doc *d = cur();
    *hasdoc = is_text(d);
    if (!*hasdoc) { *line = *col = *lang = *nlines = 0; return; }
    *line = d->caret.line + 1;
    *col = col_to_vx(&d->ln[d->caret.line], d->caret.col) + 1;
    *lang = d->lang; *nlines = d->n;
}

void editor_goto(int line, int col, int sellen)
{
    Doc *d = cur();
    if (!is_text(d)) return;
    if (line < 1) line = 1;
    if (line > d->n) line = d->n;
    d->anchor.line = d->caret.line = line - 1;
    if (col < 1) col = 1;
    if (col - 1 > d->ln[line - 1].len) col = d->ln[line - 1].len + 1;
    d->anchor.col = col - 1;
    d->caret.col = col - 1 + sellen;
    if (d->caret.col > d->ln[line - 1].len) d->caret.col = d->ln[line - 1].len;
    {
        int vis = vis_lines();
        if (d->caret.line < d->top || d->caret.line >= d->top + vis) d->top = d->caret.line - vis / 3;
        clamp_scroll(d);
    }
    SetFocus(g_hwndEditor);
    caret_moved(d, 0);
}

void editor_check_disk(void)
{
    int i;
    for (i = 0; i < s_n; i++) {
        Doc *d = s_docs[i]; FILETIME ft;
        if (d->kind || !d->path[0] || !get_mtime(d->path, &ft)) continue;
        if (CompareFileTime(&ft, &d->mtime) != 0) {
            d->mtime = ft;
            if (d->upos == d->savepos) {
                int len; char *t = read_file(d->path, &len);
                if (t) {
                    Pos c = d->caret; int top = d->top;
                    doc_set_text(d, t, len); free(t);
                    doc_clear_undo(d);
                    if (c.line >= d->n) c.line = d->n - 1;
                    if (c.col > d->ln[c.line].len) c.col = d->ln[c.line].len;
                    d->caret = d->anchor = c; d->top = top; clamp_scroll(d);
                    out_log("Reloaded %s (changed on disk)", d->path);
                    if (i == s_act) redraw();
                }
            }
        }
    }
}

void editor_font_changed(void)
{
    if (GetFocus() == g_hwndEditor) { DestroyCaret(); CreateCaret(g_hwndEditor, NULL, 2, g_codeCH - 2); ShowCaret(g_hwndEditor); }
    if (cur() && is_text(cur())) ensure_visible(cur());
    redraw();
}

void editor_command(int cmd)
{
    Doc *d = cur();
    if (cmd == CMD_WELCOME) {
        int i;
        for (i = 0; i < s_n; i++) if (s_docs[i]->kind == 1) { editor_activate(i); return; }
        d = doc_alloc(); d->kind = 1; add_doc(d);
        return;
    }
    if (cmd == CMD_NEXT_EDITOR) { if (s_n) editor_activate((s_act + 1) % s_n); return; }
    if (cmd == CMD_PREV_EDITOR) { if (s_n) editor_activate((s_act + s_n - 1) % s_n); return; }
    if (!is_text(d)) return;
    switch (cmd) {
    case CMD_UNDO: do_undo(d, 0); break;
    case CMD_REDO: do_undo(d, 1); break;
    case CMD_CUT: do_copy(d, 1); break;
    case CMD_COPY: do_copy(d, 0); break;
    case CMD_PASTE: do_paste(d); break;
    case CMD_FIND: editor_find_show(0); break;
    case CMD_REPLACE: editor_find_show(1); break;
    case CMD_TOGGLE_COMMENT: toggle_comment(d); break;
    case CMD_SELECT_ALL: editor_keydown(d, 0); d->anchor.line = 0; d->anchor.col = 0; d->caret.line = d->n - 1; d->caret.col = d->ln[d->n - 1].len; caret_moved(d, 0); break;
    case CMD_MOVE_LINE_UP: move_lines(d, -1, 0); break;
    case CMD_MOVE_LINE_DOWN: move_lines(d, 1, 0); break;
    case CMD_COPY_LINE_DOWN: move_lines(d, 1, 1); break;
    case CMD_DELETE_LINE: delete_lines(d); break;
    }
}

/* =========================================================== painting */

static unsigned char *s_cls; static int s_clsCap;

static COLORREF dim(COLORREF c, int pct)
{
    return RGB((GetRValue(c) * pct + 0x1E * (100 - pct)) / 100, (GetGValue(c) * pct + 0x1E * (100 - pct)) / 100,
               (GetBValue(c) * pct + 0x1E * (100 - pct)) / 100);
}

static int match_bracket(Doc *d, Pos p, Pos *out)
{
    static const char *open = "([{", *close = ")]}";
    Line *l = &d->ln[p.line]; int c, dir, depth = 0, li, ci, tries = 0; char o, cl;
    const char *k;
    if (p.col < l->len && strchr("()[]{}", l->s[p.col])) c = p.col;
    else if (p.col > 0 && strchr("()[]{}", l->s[p.col - 1])) c = p.col - 1;
    else return 0;
    if ((k = strchr(open, l->s[c])) != NULL) { dir = 1; o = *k; cl = close[k - open]; }
    else { k = strchr(close, l->s[c]); dir = -1; cl = *k; o = open[k - close]; }
    li = p.line; ci = c;
    while (tries++ < 20000) {
        char ch = d->ln[li].s[ci];
        if (ch == o) depth += dir; else if (ch == cl) depth -= dir;
        if (depth == 0) { out[0].line = p.line; out[0].col = c; out[1].line = li; out[1].col = ci; return 1; }
        ci += dir;
        while (ci < 0 || ci >= d->ln[li].len) {
            li += dir;
            if (li < 0 || li >= d->n) return 0;
            ci = dir > 0 ? 0 : d->ln[li].len - 1;
        }
    }
    return 0;
}

static void paint_line_text(HDC dc, Doc *d, int li, int x0, int y, int cols)
{
    Line *l = &d->ln[li]; int i, vx = 0, runStart = 0, runVx = 0, cl;
    char buf[1024]; int bn = 0;
    if (s_clsCap < l->len + 1) { s_clsCap = l->len + 256; s_cls = (unsigned char *)realloc(s_cls, s_clsCap); }
    hl_line(d, li, d->st[li], s_cls);
    SelectObject(dc, g_fCode);
    cl = l->len ? s_cls[0] : 0;
    for (i = 0; i <= l->len; i++) {
        if (i == l->len || s_cls[i] != cl || bn > 1000) {
            if (bn) {
                int sx = runVx - d->left, k = 0;
                if (sx < 0) { k = -sx; sx = 0; }
                if (k < bn && sx < cols) {
                    SetTextColor(dc, s_clr[cl]);
                    TextOut(dc, x0 + sx * g_codeCW, y, buf + k, bn - k);
                }
            }
            if (i == l->len) break;
            cl = s_cls[i]; bn = 0; runStart = i; runVx = vx;
        }
        if (l->s[i] == '\t') {
            int nx = (vx / TABSIZE + 1) * TABSIZE;
            while (vx < nx) { buf[bn++] = ' '; vx++; }
        } else { buf[bn++] = l->s[i]; vx++; }
    }
    (void)runStart;
}

static void paint_minimap(HDC dc, Doc *d, int x, int y0, int h)
{
    int rows = h / 2, mtop = 0, i, vis = vis_lines();
    if (d->n > rows) {
        int maxTop = d->n - vis; if (maxTop < 1) maxTop = 1;
        mtop = (int)((double)d->top / maxTop * (d->n - rows));
        if (mtop < 0) mtop = 0;
        if (mtop > d->n - rows) mtop = d->n - rows;
    }
    fill(dc, x, y0 + (d->top - mtop) * 2, MM_W, vis * 2, HEX(0x2F2F2F));
    ensure_states(d, mtop + rows);
    for (i = 0; i < rows && mtop + i < d->n; i++) {
        Line *l = &d->ln[mtop + i]; int k, vx = 0, start = -1, sc = 0;
        if (s_clsCap < l->len + 1) { s_clsCap = l->len + 256; s_cls = (unsigned char *)realloc(s_cls, s_clsCap); }
        hl_line(d, mtop + i, d->st[mtop + i], s_cls);
        for (k = 0; k <= l->len && vx < MM_W - 4; k++) {
            int ws = k == l->len || l->s[k] == ' ' || l->s[k] == '\t';
            if (start >= 0 && (ws || s_cls[k] != sc)) {
                fill(dc, x + 2 + start, y0 + i * 2, vx - start, 2, dim(s_clr[sc], 55));
                start = -1;
            }
            if (k == l->len) break;
            if (!ws && start < 0) { start = vx; sc = s_cls[k]; }
            vx = l->s[k] == '\t' ? (vx / TABSIZE + 1) * TABSIZE : vx + 1;
        }
    }
}

static void paint_welcome(HDC dc, int x0, int y0, int w, int h)
{
    int lx = x0 + w / 10, y = y0 + h / 8, rx = x0 + w / 2 + 20, i;
    static const struct { const char *label; int cmd; } start[] = {
        { "New File...", CMD_NEW_FILE }, { "Open File...", CMD_OPEN_FILE }, { "Open Folder...", CMD_OPEN_FOLDER },
        { "Run Build Task", CMD_BUILD_TASK }, { "New Terminal", CMD_NEW_TERMINAL } };
    static const struct { const char *title, *desc, *key; int cmd; } cards[] = {
        { "Command Palette", "Find and run every command in XP Code.", "Ctrl+Shift+P", CMD_PALETTE },
        { "Integrated Terminal", "A real cmd.exe, right inside the editor.", "Ctrl+`", CMD_FOCUS_TERMINAL },
        { "Quick Open", "Jump to any file in the folder by name.", "Ctrl+P", CMD_QUICK_OPEN },
        { "Run and Build", "F5 compiles C with TCC, runs Python and batch.", "F5", CMD_RUN_FILE },
        { "Keyboard Shortcuts", "Learn the keys that make you fast.", "", CMD_SHORTCUTS } };
    s_nlinks = 0;
    fill(dc, x0, y0, w, h, C_EDITOR_BG);
    text_at(dc, lx, y, "XP Code", -1, HEX(0xCCCCCC), g_fTitle);
    y += 58;
    text_at(dc, lx, y, "A code editor in plain Win32", -1, HEX(0x9D9D9D), g_fHeading);
    y += 60;
    text_at(dc, lx, y, "Start", -1, HEX(0xCCCCCC), g_fHeading);
    y += 34;
    for (i = 0; i < 5; i++) {
        Link *k = &s_links[s_nlinks];
        int tw = text_w(dc, g_fUI, start[i].label, -1);
        SetRect(&k->r, lx, y, lx + 22 + tw, y + 20); k->cmd = start[i].cmd;
        draw_icon(dc, i == 0 ? ICON_NEW_FILE : i == 1 ? ICON_FILES : i == 2 ? ICON_NEW_FOLDER : i == 3 ? ICON_PLAY : ICON_TERMINAL,
                  lx + 7, y + 9, 14, C_LINK);
        text_at(dc, lx + 22, y + 2, start[i].label, -1, s_hoverLink == s_nlinks ? HEX(0x6CB2FF) : C_LINK, g_fUI);
        if (s_hoverLink == s_nlinks) hline(dc, lx + 22, y + 16, tw, HEX(0x6CB2FF));
        s_nlinks++;
        y += 26;
    }
    y += 24;
    text_at(dc, lx, y, "Recent", -1, HEX(0xCCCCCC), g_fHeading);
    y += 34;
    text_at(dc, lx, y, g_root[0] ? g_root : "No folder open", -1, C_TEXT_MUTED, g_fUI);

    y = y0 + h / 8 + 118;
    text_at(dc, rx, y, "Learn", -1, HEX(0xCCCCCC), g_fHeading);
    y += 34;
    for (i = 0; i < 5; i++) {
        Link *k = &s_links[s_nlinks]; int cw = w / 2 - 80; if (cw > 460) cw = 460;
        SetRect(&k->r, rx, y, rx + cw, y + 58); k->cmd = cards[i].cmd;
        fill_rc(dc, &k->r, s_hoverLink == s_nlinks ? HEX(0x2D2D30) : HEX(0x252526));
        if (s_hoverLink == s_nlinks) frame(dc, k->r.left, k->r.top, cw, 58, C_ACCENT);
        draw_icon(dc, i == 0 ? ICON_SEARCH : i == 1 ? ICON_TERMINAL : i == 2 ? ICON_FILES : i == 3 ? ICON_RUN : ICON_GEAR,
                  rx + 22, y + 29, 18, C_LINK);
        text_at(dc, rx + 44, y + 12, cards[i].title, -1, HEX(0xE7E7E7), g_fUIBold);
        text_at(dc, rx + 44, y + 31, cards[i].desc, -1, C_TEXT_MUTED, g_fUI);
        if (cards[i].key[0]) {
            int kw = text_w(dc, g_fUISmall, cards[i].key, -1);
            fill(dc, rx + cw - kw - 22, y + 10, kw + 12, 18, HEX(0x3A3A3A));
            text_at(dc, rx + cw - kw - 16, y + 12, cards[i].key, -1, HEX(0xCCCCCC), g_fUISmall);
        }
        s_nlinks++;
        y += 68;
    }
}

static void paint_watermark(HDC dc, int w, int h)
{
    static const struct { const char *k, *v; } rows[] = {
        { "Show All Commands", "Ctrl+Shift+P" }, { "Go to File", "Ctrl+P" }, { "Find in Files", "Ctrl+Shift+F" },
        { "Toggle Terminal", "Ctrl+`" }, { "Run Active File", "F5" } };
    int i, cx = w / 2, y = h / 2 - 150;
    HPEN pen = CreatePen(PS_SOLID, 22, HEX(0x2B2B2B)), op;
    fill(dc, 0, 0, w, h, C_EDITOR_BG);
    op = (HPEN)SelectObject(dc, pen);
    MoveToEx(dc, cx - 60, y - 10, NULL); LineTo(dc, cx, y + 50); LineTo(dc, cx - 60, y + 110);
    MoveToEx(dc, cx + 20, y + 110, NULL); LineTo(dc, cx + 80, y + 110);
    SelectObject(dc, op); DeleteObject(pen);
    y += 170;
    for (i = 0; i < 5; i++) {
        text_at(dc, cx - 10 - text_w(dc, g_fUI, rows[i].k, -1), y, rows[i].k, -1, HEX(0x6E6E6E), g_fUI);
        text_at(dc, cx + 10, y, rows[i].v, -1, HEX(0x6E6E6E), g_fUI);
        y += 24;
    }
}

static void paint_tabs(HDC dc, int w)
{
    int i, x, total = 0;
    char name[MAX_PATH];
    fill(dc, 0, 0, w, TAB_H, C_TABSTRIP_BG);
    for (i = 0; i < s_n; i++) {
        doc_label(s_docs[i], name, sizeof name);
        total = text_w(dc, g_fUI, name, -1) + 70;
        s_tabRc[i].left = i ? s_tabRc[i - 1].right : 0;
        s_tabRc[i].right = s_tabRc[i].left + total;
    }
    if (s_act >= 0) {
        if (s_tabRc[s_act].right - s_tabScroll > w - 40) s_tabScroll = s_tabRc[s_act].right - w + 40;
        if (s_tabRc[s_act].left - s_tabScroll < 0) s_tabScroll = s_tabRc[s_act].left;
    }
    if (s_tabScroll < 0) s_tabScroll = 0;
    for (i = 0; i < s_n; i++) {
        Doc *d = s_docs[i]; int active = i == s_act, dirty = editor_doc_dirty(i), tw;
        RECT *r = &s_tabRc[i];
        r->left -= s_tabScroll; r->right -= s_tabScroll; r->top = 0; r->bottom = TAB_H;
        x = r->left; tw = r->right - r->left;
        doc_label(d, name, sizeof name);
        fill(dc, x, 0, tw, TAB_H, active ? C_EDITOR_BG : C_TAB_INACTIVE);
        vline(dc, r->right - 1, 0, TAB_H, C_TABSTRIP_BG);
        if (active && GetFocus() == g_hwndEditor) hline(dc, x, 0, tw - 1, C_ACCENT);
        if (d->kind == 1) draw_icon(dc, ICON_FILES, x + 18, TAB_H / 2, 14, C_LINK);
        else file_badge(dc, name, 0, 0, x + 10, TAB_H / 2);
        text_at(dc, x + 30, 10, name, -1, active ? C_TEXT_BRIGHT : C_TEXT_MUTED, g_fUI);
        SetRect(&s_tabClose[i], r->right - 30, 8, r->right - 10, 28);
        if (s_hoverClose == i) fill_rc(dc, &s_tabClose[i], HEX(0x404040));
        if (dirty && s_hoverClose != i) draw_icon(dc, ICON_DOT, r->right - 20, TAB_H / 2, 14, active ? C_TEXT_BRIGHT : C_TEXT_MUTED);
        else if (active || s_hoverTab == i) draw_icon(dc, ICON_CLOSE, r->right - 20, TAB_H / 2, 14, active ? C_TEXT_BRIGHT : C_TEXT_MUTED);
    }
}

static void paint_breadcrumbs(HDC dc, Doc *d, int w)
{
    char rel[MAX_PATH], *seg, *next; int x = 16;
    fill(dc, 0, TAB_H, w, CRUMB_H, C_EDITOR_BG);
    if (!d->path[0]) return;
    path_rel(d->path, rel, sizeof rel);
    seg = rel;
    while (seg) {
        next = strchr(seg, '\\');
        if (next) *next++ = 0;
        if (!next) { file_badge(dc, seg, 0, 0, x - 2, TAB_H + CRUMB_H / 2); x += 18; }
        text_at(dc, x, TAB_H + 4, seg, -1, HEX(0xA9A9A9), g_fUI);
        x += text_w(dc, g_fUI, seg, -1) + 6;
        if (next) { draw_icon(dc, ICON_CHEVRON_RIGHT, x + 3, TAB_H + CRUMB_H / 2, 12, HEX(0xA9A9A9)); x += 14; }
        seg = next;
    }
}

static void paint_find(HDC dc)
{
    char buf[64]; RECT er;
    if (!s_findOpen) return;
    fill_rc(dc, &s_findRc, C_WIDGET_BG);
    hline(dc, s_findRc.left, s_findRc.bottom, s_findRc.right - s_findRc.left, HEX(0x111111));
    vline(dc, s_findRc.left, s_findRc.top, s_findRc.bottom - s_findRc.top, C_ACCENT);
    draw_icon(dc, s_replOpen ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT, s_findRc.left + 15, s_findRc.top + 17, 14, C_SIDEBAR_TEXT);
    SetRect(&er, s_findRc.left + 26, s_findRc.top + 4, s_findRc.left + 254, s_findRc.top + 28);
    fill_rc(dc, &er, C_INPUT_BG);
    if (GetFocus() == s_findEdit) frame(dc, er.left, er.top, er.right - er.left, er.bottom - er.top, C_ACCENT);
    if (!s_find[0]) sfmt(buf, sizeof buf, "No results");
    else if (!s_matchCount) sfmt(buf, sizeof buf, "No results");
    else if (s_matchIndex) sfmt(buf, sizeof buf, "%d of %d", s_matchIndex, s_matchCount);
    else sfmt(buf, sizeof buf, "? of %d", s_matchCount);
    text_at(dc, s_findRc.left + 262, s_findRc.top + 10, buf, -1, s_find[0] && !s_matchCount ? C_ERROR : C_SIDEBAR_TEXT, g_fUI);
    draw_icon(dc, ICON_ARROW_UP, (s_findBtns[0].left + s_findBtns[0].right) / 2, s_findRc.top + 17, 14, C_SIDEBAR_TEXT);
    draw_icon(dc, ICON_ARROW_DOWN, (s_findBtns[1].left + s_findBtns[1].right) / 2, s_findRc.top + 17, 14, C_SIDEBAR_TEXT);
    draw_icon(dc, ICON_CLOSE, (s_findBtns[2].left + s_findBtns[2].right) / 2, s_findRc.top + 17, 14, C_SIDEBAR_TEXT);
    if (s_replOpen) {
        SetRect(&er, s_findRc.left + 26, s_findRc.top + 34, s_findRc.left + 254, s_findRc.top + 58);
        fill_rc(dc, &er, C_INPUT_BG);
        if (GetFocus() == s_replEdit) frame(dc, er.left, er.top, er.right - er.left, er.bottom - er.top, C_ACCENT);
        fill_rc(dc, &s_findBtns[3], HEX(0x3A3D41)); fill_rc(dc, &s_findBtns[4], HEX(0x3A3D41));
        text_at(dc, s_findBtns[3].left + 6, s_findBtns[3].top + 4, "Replace", -1, C_SIDEBAR_TEXT, g_fUI);
        text_at(dc, s_findBtns[4].left + 6, s_findBtns[4].top + 4, "Replace All", -1, C_SIDEBAR_TEXT, g_fUI);
    }
}

static void paint_text(HDC dc, Doc *d, int w, int h)
{
    int y0 = text_top(), gw = gutter_w(d), xr = text_right(), vis = vis_lines() + 1, cols = vis_cols(d) + 1, i;
    int focused = GetFocus() == g_hwndEditor || GetFocus() == s_findEdit || GetFocus() == s_replEdit;
    Pos sa, sb, br[2]; int hasBr, qn = (int)strlen(s_find);
    char num[16];
    sel_range(d, &sa, &sb);
    hasBr = !has_sel(d) && match_bracket(d, d->caret, br);
    ensure_states(d, d->top + vis);
    fill(dc, 0, y0, w, h - y0, C_EDITOR_BG);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < vis; i++) {
        int li = d->top + i, y = y0 + i * g_codeCH; Line *l;
        if (li >= d->n) break;
        l = &d->ln[li];
        if (li == d->caret.line && !has_sel(d)) frame(dc, gw - 4, y, xr - gw + 4, g_codeCH, HEX(0x282828));
        /* selection */
        if (li >= sa.line && li <= sb.line && has_sel(d)) {
            int a = li == sa.line ? col_to_vx(l, sa.col) : 0;
            int b = li == sb.line ? col_to_vx(l, sb.col) : col_to_vx(l, l->len) + 1;
            a -= d->left; b -= d->left; if (a < 0) a = 0;
            if (b > a) fill(dc, gw + a * g_codeCW, y, (b - a) * g_codeCW, g_codeCH, focused ? C_SELECTION : C_SEL_INACTIVE);
        }
        /* find matches */
        if (s_findOpen && qn) {
            int c = 0;
            while ((c = find_in_line(l, c, s_find, qn)) >= 0) {
                int a = col_to_vx(l, c) - d->left, b = col_to_vx(l, c + qn) - d->left;
                int current = li == sa.line && c == sa.col && has_sel(d);
                if (b > 0) { if (a < 0) a = 0; fill(dc, gw + a * g_codeCW, y, (b - a) * g_codeCW, g_codeCH, current ? HEX(0x515C6A) : HEX(0x623315)); }
                c += qn;
            }
        }
        /* indent guides */
        {
            int lead = col_to_vx(l, leading_ws(l)), g;
            if (leading_ws(l) == l->len && li > 0) lead = col_to_vx(&d->ln[li - 1], leading_ws(&d->ln[li - 1]));
            for (g = 0; g < lead; g += TABSIZE) if (g - d->left >= 0) vline(dc, gw + (g - d->left) * g_codeCW, y, g_codeCH, HEX(0x404040));
        }
        /* bracket match boxes */
        if (hasBr) {
            int k;
            for (k = 0; k < 2; k++) if (br[k].line == li) {
                int a = col_to_vx(l, br[k].col) - d->left;
                if (a >= 0) frame(dc, gw + a * g_codeCW, y, g_codeCW + 1, g_codeCH, HEX(0x888888));
            }
        }
        /* line number */
        sfmt(num, sizeof num, "%d", li + 1);
        SelectObject(dc, g_fCode);
        SetTextColor(dc, li == d->caret.line ? HEX(0xC6C6C6) : HEX(0x858585));
        {
            SIZE sz; GetTextExtentPoint32(dc, num, (int)strlen(num), &sz);
            TextOut(dc, gw - 26 - sz.cx, y + 2, num, (int)strlen(num));
        }
        /* text */
        {
            int saved = SaveDC(dc);
            IntersectClipRect(dc, gw, y, xr, y + g_codeCH);
            paint_line_text(dc, d, li, gw, y + 2, cols);
            RestoreDC(dc, saved);
        }
    }
    if (d->left > 0) fill(dc, gw - 6, y0, 6, h - y0, HEX(0x161616));
    /* minimap + scrollbar */
    if (g_minimap) {
        fill(dc, xr, y0, MM_W, h - y0, C_EDITOR_BG);
        paint_minimap(dc, d, xr, y0, h - y0);
    }
    {
        int sx = w - SB_W, th = h - y0, total = d->n + vis_lines() - 1, thumbH, thumbY;
        fill(dc, sx, y0, SB_W, th, C_EDITOR_BG);
        vline(dc, sx, y0, th, HEX(0x2B2B2B));
        if (total < 1) total = 1;
        thumbH = th * vis_lines() / total; if (thumbH < 20) thumbH = 20;
        thumbY = y0 + (th - thumbH) * d->top / (total - vis_lines() + 1 > 0 ? total - vis_lines() + 1 : 1);
        if (d->n > 1) fill(dc, sx + 1, thumbY, SB_W - 1, thumbH, s_dragScroll ? HEX(0x5F5F5F) : HEX(0x424242));
        /* overview ruler: caret line */
        fill(dc, sx + 1, y0 + th * d->caret.line / (d->n > 1 ? d->n : 1), SB_W - 1, 2, HEX(0xA0A0A0));
    }
}

static void editor_paint(HWND w)
{
    PAINTSTRUCT ps; RECT rc; HDC dc = BeginPaint(w, &ps), mem; HBITMAP bmp, ob;
    Doc *d = cur();
    GetClientRect(w, &rc);
    mem = CreateCompatibleDC(dc); bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = (HBITMAP)SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);
    if (!d) paint_watermark(mem, rc.right, rc.bottom);
    else {
        paint_tabs(mem, rc.right);
        if (d->kind == 1) paint_welcome(mem, 0, TAB_H, rc.right, rc.bottom - TAB_H);
        else {
            paint_breadcrumbs(mem, d, rc.right);
            paint_text(mem, d, rc.right, rc.bottom);
            paint_find(mem);
        }
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(w, &ps);
}

/* =========================================================== mouse */

static Pos hit_pos(Doc *d, int x, int y)
{
    Pos p; int vx;
    p.line = d->top + (y - text_top()) / g_codeCH;
    if (y < text_top()) p.line = d->top - 1;
    if (p.line < 0) p.line = 0;
    if (p.line >= d->n) p.line = d->n - 1;
    vx = (x - gutter_w(d) + g_codeCW / 2) / g_codeCW + d->left;
    if (vx < 0) vx = 0;
    p.col = vx_to_col(&d->ln[p.line], vx);
    return p;
}

static void scroll_to_y(Doc *d, int y)
{
    int w, h, th, total, vis = vis_lines();
    client_size(&w, &h);
    th = h - text_top(); total = d->n;
    d->top = (int)((double)(y - text_top() - s_dragOffset) / th * total);
    clamp_scroll(d); redraw();
}

static void mini_to_y(Doc *d, int y)
{
    int w, h, rows, mtop = 0, vis = vis_lines();
    client_size(&w, &h);
    rows = (h - text_top()) / 2;
    if (d->n > rows) {
        int maxTop = d->n - vis; if (maxTop < 1) maxTop = 1;
        mtop = (int)((double)d->top / maxTop * (d->n - rows));
    }
    d->top = mtop + (y - text_top()) / 2 - vis / 2;
    clamp_scroll(d); redraw();
}

static void tab_context(int i, int x, int y)
{
    HMENU m = CreatePopupMenu(); POINT pt; int c;
    AppendMenu(m, MF_STRING, 1, "Close\tCtrl+W");
    AppendMenu(m, MF_STRING, 2, "Close Others");
    AppendMenu(m, MF_STRING, 3, "Close All");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, 4, "Copy Path");
    AppendMenu(m, MF_STRING, 5, "Reveal in Windows Explorer");
    pt.x = x; pt.y = y; ClientToScreen(g_hwndEditor, &pt);
    c = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, g_hwndEditor, NULL);
    DestroyMenu(m);
    if (c == 1) editor_close(i);
    else if (c == 2) { Doc *keep = s_docs[i]; int k; for (k = s_n - 1; k >= 0; k--) if (s_docs[k] != keep && !editor_close(k)) break; }
    else if (c == 3) editor_close_all();
    else if (c == 4 && s_docs[i]->path[0]) copy_text(s_docs[i]->path, (int)strlen(s_docs[i]->path));
    else if (c == 5 && s_docs[i]->path[0]) { char a[MAX_PATH + 16]; sfmt(a, sizeof a, "/select,\"%s\"", s_docs[i]->path); ShellExecute(NULL, "open", "explorer.exe", a, NULL, SW_SHOWNORMAL); }
}

static void text_context(int x, int y)
{
    HMENU m = CreatePopupMenu(); POINT pt; int c;
    AppendMenu(m, MF_STRING, CMD_CUT, "Cut\tCtrl+X");
    AppendMenu(m, MF_STRING, CMD_COPY, "Copy\tCtrl+C");
    AppendMenu(m, MF_STRING, CMD_PASTE, "Paste\tCtrl+V");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, CMD_TOGGLE_COMMENT, "Toggle Line Comment\tCtrl+/");
    AppendMenu(m, MF_STRING, CMD_FIND, "Find\tCtrl+F");
    AppendMenu(m, MF_SEPARATOR, 0, NULL);
    AppendMenu(m, MF_STRING, CMD_RUN_FILE, "Run Active File\tF5");
    AppendMenu(m, MF_STRING, CMD_PALETTE, "Command Palette...\tCtrl+Shift+P");
    pt.x = x; pt.y = y; ClientToScreen(g_hwndEditor, &pt);
    c = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, g_hwndEditor, NULL);
    DestroyMenu(m);
    if (c) app_command(c);
}

static void editor_mousedown(HWND w, int x, int y, int dbl)
{
    Doc *d = cur(); int i, ww, hh;
    POINT pt; pt.x = x; pt.y = y;
    client_size(&ww, &hh);
    SetFocus(w);
    if (!d) return;
    if (y < TAB_H) {
        for (i = 0; i < s_n; i++) {
            if (PtInRect(&s_tabClose[i], pt)) { editor_close(i); return; }
            if (PtInRect(&s_tabRc[i], pt)) { editor_activate(i); return; }
        }
        return;
    }
    if (d->kind == 1) {
        for (i = 0; i < s_nlinks; i++) if (PtInRect(&s_links[i].r, pt)) { app_command(s_links[i].cmd); return; }
        return;
    }
    if (y < text_top()) return;
    if (s_findOpen && PtInRect(&s_findRc, pt)) {
        if (PtInRect(&s_findBtns[0], pt)) find_step(d, -1, 0);
        else if (PtInRect(&s_findBtns[1], pt)) find_step(d, 1, 0);
        else if (PtInRect(&s_findBtns[2], pt)) find_close();
        else if (s_replOpen && PtInRect(&s_findBtns[3], pt)) replace_one(d);
        else if (s_replOpen && PtInRect(&s_findBtns[4], pt)) replace_all(d);
        else if (x < s_findRc.left + 26) { s_replOpen = !s_replOpen; layout_find(); redraw(); }
        return;
    }
    if (x >= ww - SB_W) {
        int th = hh - text_top(), total = d->n + vis_lines() - 1, thumbH, thumbY;
        thumbH = th * vis_lines() / (total ? total : 1); if (thumbH < 20) thumbH = 20;
        thumbY = text_top() + (th - thumbH) * d->top / (total - vis_lines() + 1 > 0 ? total - vis_lines() + 1 : 1);
        if (y >= thumbY && y < thumbY + thumbH) s_dragOffset = y - thumbY;
        else { s_dragOffset = thumbH / 2; scroll_to_y(d, y); }
        s_dragScroll = 1; SetCapture(w);
        return;
    }
    if (g_minimap && x >= text_right()) { s_dragMini = 1; SetCapture(w); mini_to_y(d, y); return; }
    {
        Pos p = hit_pos(d, x, y);
        if (x < gutter_w(d) - 10) {   /* gutter: select line */
            d->anchor.line = p.line; d->anchor.col = 0;
            if (p.line + 1 < d->n) { d->caret.line = p.line + 1; d->caret.col = 0; } else d->caret.col = d->ln[p.line].len;
        } else if (dbl) select_word_at(d, p);
        else move_to(d, p, GetKeyState(VK_SHIFT) < 0);
        s_selecting = 1; SetCapture(w);
        caret_moved(d, 0);
    }
}

/* =========================================================== window proc */

static LRESULT CALLBACK EditorProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    Doc *d = cur();
    switch (m) {
    case WM_CREATE:
        s_inputBrush = CreateSolidBrush(C_INPUT_BG);
        s_findEdit = CreateWindowEx(0, "EDIT", "", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, w, (HMENU)1, g_hinst, NULL);
        s_replEdit = CreateWindowEx(0, "EDIT", "", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 0, 0, w, (HMENU)2, g_hinst, NULL);
        SendMessage(s_findEdit, WM_SETFONT, (WPARAM)g_fUI, 0);
        SendMessage(s_replEdit, WM_SETFONT, (WPARAM)g_fUI, 0);
        s_editOrig = (WNDPROC)SetWindowLongPtr(s_findEdit, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
        SetWindowLongPtr(s_replEdit, GWLP_WNDPROC, (LONG_PTR)FindEditProc);
        return 0;
    case WM_SIZE:
        layout_find();
        if (d && is_text(d)) {
            if (col_to_vx(&d->ln[d->caret.line], d->caret.col) < vis_cols(d) - 1) d->left = 0;
            ensure_visible(d);
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_SIDEBAR_TEXT); SetBkColor((HDC)wp, C_INPUT_BG);
        return (LRESULT)s_inputBrush;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == s_findEdit) {
            GetWindowText(s_findEdit, s_find, sizeof s_find);
            if (d && is_text(d)) { if (s_find[0]) find_step(d, 1, 1); else { find_count(d); redraw(); } }
        }
        if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_PAINT: editor_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS:
        CreateCaret(w, NULL, 2, g_codeCH - 2); update_caret(); ShowCaret(w);
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_KILLFOCUS: DestroyCaret(); InvalidateRect(w, NULL, FALSE); return 0;
    case WM_KEYDOWN:
        if (is_text(d) && editor_keydown(d, (int)wp)) return 0;
        break;
    case WM_SYSKEYDOWN:
        if (is_text(d) && (wp == VK_UP || wp == VK_DOWN) && editor_keydown(d, (int)wp)) return 0;
        break;
    case WM_CHAR:
        if (is_text(d)) editor_char(d, (int)wp);
        return 0;
    case WM_LBUTTONDOWN: editor_mousedown(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0); return 0;
    case WM_LBUTTONDBLCLK: editor_mousedown(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 1); return 0;
    case WM_MBUTTONDOWN: {
        POINT pt; int i; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        for (i = 0; i < s_n; i++) if (PtInRect(&s_tabRc[i], pt)) { editor_close(i); break; }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        POINT pt; int i; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        SetFocus(w);
        if (pt.y < TAB_H) { for (i = 0; i < s_n; i++) if (PtInRect(&s_tabRc[i], pt)) { tab_context(i, pt.x, pt.y); break; } }
        else if (is_text(d) && pt.y > text_top()) {
            Pos p = hit_pos(d, pt.x, pt.y), a, b; sel_range(d, &a, &b);
            if (!has_sel(d) || pos_cmp(p, a) < 0 || pos_cmp(p, b) > 0) { move_to(d, p, 0); caret_moved(d, 0); }
            text_context(pt.x, pt.y);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i, ht = -1, hc = -1, hl = -1; POINT pt; TRACKMOUSEEVENT t;
        pt.x = x; pt.y = y;
        if (s_selecting && is_text(d)) {
            Pos p = hit_pos(d, x, y);
            if (y < text_top() && d->top > 0) { d->top--; }
            d->caret = p; caret_moved(d, 0);
            return 0;
        }
        if (s_dragScroll && is_text(d)) { scroll_to_y(d, y); return 0; }
        if (s_dragMini && is_text(d)) { mini_to_y(d, y); return 0; }
        if (y < TAB_H) for (i = 0; i < s_n; i++) { if (PtInRect(&s_tabRc[i], pt)) ht = i; if (PtInRect(&s_tabClose[i], pt)) hc = i; }
        if (d && d->kind == 1) for (i = 0; i < s_nlinks; i++) if (PtInRect(&s_links[i].r, pt)) hl = i;
        if (ht != s_hoverTab || hc != s_hoverClose || hl != s_hoverLink) {
            s_hoverTab = ht; s_hoverClose = hc; s_hoverLink = hl; InvalidateRect(w, NULL, FALSE);
        }
        t.cbSize = sizeof t; t.dwFlags = TME_LEAVE; t.hwndTrack = w; t.dwHoverTime = 0; TrackMouseEvent(&t);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && is_text(d)) {
            POINT pt; GetCursorPos(&pt); ScreenToClient(w, &pt);
            if (pt.y > text_top() && pt.x > gutter_w(d) - 10 && pt.x < text_right() && !(s_findOpen && PtInRect(&s_findRc, pt))) {
                SetCursor(LoadCursor(NULL, IDC_IBEAM)); return TRUE;
            }
        }
        if (LOWORD(lp) == HTCLIENT && d && d->kind == 1 && s_hoverLink >= 0) { SetCursor(LoadCursor(NULL, IDC_HAND)); return TRUE; }
        break;
    case WM_MOUSELEAVE:
        if (s_hoverTab >= 0 || s_hoverClose >= 0 || s_hoverLink >= 0) { s_hoverTab = s_hoverClose = s_hoverLink = -1; InvalidateRect(w, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        if (s_selecting || s_dragScroll || s_dragMini) { s_selecting = s_dragScroll = s_dragMini = 0; ReleaseCapture(); redraw(); }
        return 0;
    case WM_MOUSEWHEEL:
        if (is_text(d)) {
            int delta = GET_WHEEL_DELTA_WPARAM(wp) / 40;
            if (GetKeyState(VK_CONTROL) < 0) { app_command(delta > 0 ? CMD_ZOOM_IN : CMD_ZOOM_OUT); return 0; }
            if (GetKeyState(VK_SHIFT) < 0) d->left -= delta * 4; else d->top -= delta;
            clamp_scroll(d); redraw();
        }
        return 0;
    }
    return DefWindowProc(w, m, wp, lp);
}

void editor_register(void)
{
    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = EditorProc; wc.hInstance = g_hinst; wc.lpszClassName = "XPEditor";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}
