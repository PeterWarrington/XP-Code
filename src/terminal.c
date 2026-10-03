/* terminal.c - the bottom panel: PROBLEMS, OUTPUT and the integrated TERMINAL.
 * Each terminal is a real cmd.exe connected through anonymous pipes, with a hidden
 * console of its own so Ctrl+C can be delivered, all inside a Job Object. */
#include "xpcode.h"

#define HEADER_H   35
#define LIST_W     150
#define MAXTERMS   8
#define SCROLLBACK 5000

typedef struct { char *s; int len, cap; } TLine;
typedef struct { int n; char data[1]; } TermChunk;

typedef struct Term {
    HANDLE hProc, hIn, hOut, hJob, hThread; DWORD pid;
    TLine *lines; int n, cap;
    int col, esc;
    char input[1024]; int inlen, incur;
    char hist[50][256]; int nhist, histpos;
    int top, follow, alive, exitCode, number;
    char cwd[MAX_PATH];
    int selA[2], selB[2], hasSel;
    int tabCycle; char tabBase[256];
} Term;

typedef struct { char file[MAX_PATH]; int line, col, sev; char msg[300]; } Problem;

static Term *s_terms[MAXTERMS]; static int s_nterms, s_cur = -1, s_termCounter;
static int s_tab = 2;                 /* 0 problems, 1 output, 2 terminal */
static RECT s_tabRc[3], s_btnNew, s_btnKill, s_btnClear, s_btnClose;
static int s_hoverBtn = -1, s_hoverRow = -1, s_selecting;
static char *s_out[2000]; static int s_nout, s_outTop;
static Problem *s_probs; static int s_nprobs, s_probScroll;
typedef struct { int file; int prob; } PRow;  /* prob = -1 header */
static PRow *s_prows; static int s_nprows;

static Term *curterm(void) { return s_cur >= 0 && s_cur < s_nterms ? s_terms[s_cur] : NULL; }

/* =========================================================== problems */

int problems_count(int *errors, int *warnings)
{
    int i; *errors = *warnings = 0;
    for (i = 0; i < s_nprobs; i++) { if (s_probs[i].sev) (*warnings)++; else (*errors)++; }
    return s_nprobs;
}

static void problems_rebuild(void)
{
    int i, j, k;
    s_prows = (PRow *)realloc(s_prows, (s_nprobs * 2 + 1) * sizeof(PRow));
    s_nprows = 0;
    for (i = 0; i < s_nprobs; i++) {
        int seen = 0;
        for (j = 0; j < i; j++) if (!_stricmp(s_probs[j].file, s_probs[i].file)) { seen = 1; break; }
        if (seen) continue;
        s_prows[s_nprows].file = i; s_prows[s_nprows].prob = -1; s_nprows++;
        for (k = i; k < s_nprobs; k++) if (!_stricmp(s_probs[k].file, s_probs[i].file)) { s_prows[s_nprows].file = i; s_prows[s_nprows].prob = k; s_nprows++; }
    }
    status_refresh();
    if (s_tab == 0 || 1) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

static void problems_clear(void) { s_nprobs = 0; s_probScroll = 0; problems_rebuild(); }

/* Compilers print paths relative to wherever the command ran, and cmd only reports its new
 * directory after the command finishes, so try the terminal's directory, then the folder root,
 * then any file in the workspace whose path ends with the reported name. */
static void resolve_path(Term *t, const char *f, int n, char *out)
{
    char tmp[MAX_PATH], full[MAX_PATH], **files; int i, nf, tl;
    if (n >= MAX_PATH) n = MAX_PATH - 1;
    memcpy(tmp, f, n); tmp[n] = 0;
    for (i = 0; tmp[i]; i++) if (tmp[i] == '/') tmp[i] = '\\';
    if ((n > 2 && tmp[1] == ':') || tmp[0] == '\\') { strcpy(out, tmp); return; }
    sfmt(out, MAX_PATH, "%s\\%s", t->cwd[0] ? t->cwd : g_root, tmp);
    if (GetFullPathName(out, MAX_PATH, full, NULL)) strcpy(out, full);
    if (file_exists(out) || !g_root[0]) return;
    sfmt(full, MAX_PATH, "%s\\%s", g_root, tmp);
    if (file_exists(full)) { strcpy(out, full); return; }
    nf = sidebar_list_files(&files); tl = (int)strlen(tmp);
    for (i = 0; i < nf; i++) {
        int fl = (int)strlen(files[i]);
        if (fl >= tl && !_stricmp(files[i] + fl - tl, tmp) && (fl == tl || files[i][fl - tl - 1] == '\\')) {
            sfmt(out, MAX_PATH, "%s\\%s", g_root, files[i]); break;
        }
    }
    for (i = 0; i < nf; i++) free(files[i]);
    free(files);
}

static void add_problem(Term *t, const char *file, int flen, int line, int col, int sev, const char *msg)
{
    Problem *p;
    if (s_nprobs >= 500) return;
    s_probs = (Problem *)realloc(s_probs, (s_nprobs + 1) * sizeof(Problem));
    p = &s_probs[s_nprobs++];
    resolve_path(t, file, flen, p->file);
    p->line = line; p->col = col; p->sev = sev;
    sfmt(p->msg, sizeof p->msg, "%s", msg);
    problems_rebuild();
}

/* recognise "file:line: error: msg", "file:line:col: warning: msg", "file(line) : error C1: msg",
 * and Python's '  File "x", line N' */
static int parse_location(const char *s, int *flen, int *line, int *col, int *sev, const char **msg)
{
    const char *p, *q;
    *col = 1; *sev = 0;
    if ((p = strstr(s, "File \"")) != NULL && (q = strstr(p + 6, "\", line ")) != NULL) {
        *flen = (int)(q - p - 6); *line = atoi(q + 8); *msg = "Python exception (see terminal)";
        return (int)(p + 6 - s) + 1;   /* returns 1 + start offset of file name */
    }
    for (p = s + 2; *p; p++) {
        if (*p == ':' && isdigit((unsigned char)p[1])) {
            q = p + 1; *line = atoi(q);
            while (isdigit((unsigned char)*q)) q++;
            if (*q == ':' && isdigit((unsigned char)q[1])) { *col = atoi(q + 1); q++; while (isdigit((unsigned char)*q)) q++; }
            if (*q != ':') continue;
            q++; while (*q == ' ') q++;
            if (!_strnicmp(q, "error", 5)) { *sev = 0; q += 5; }
            else if (!_strnicmp(q, "warning", 7)) { *sev = 1; q += 7; }
            else if (!_strnicmp(q, "fatal error", 11)) { *sev = 0; q += 11; }
            else continue;
            while (*q == ':' || *q == ' ') q++;
            *flen = (int)(p - s); *msg = q;
            return 1;
        }
        if (*p == '(' && isdigit((unsigned char)p[1])) {
            q = p + 1; *line = atoi(q);
            while (isdigit((unsigned char)*q)) q++;
            if (*q != ')') continue;
            q++; while (*q == ' ' || *q == ':') q++;
            if (!_strnicmp(q, "error", 5)) *sev = 0; else if (!_strnicmp(q, "warning", 7)) *sev = 1; else continue;
            while (*q && *q != ':') q++;
            while (*q == ':' || *q == ' ') q++;
            *flen = (int)(p - s); *msg = q;
            return 1;
        }
    }
    return 0;
}

static void scan_problem(Term *t, const char *s)
{
    int flen, line, col, sev, r; const char *msg;
    while (*s == ' ') s++;
    r = parse_location(s, &flen, &line, &col, &sev, &msg);
    if (r) add_problem(t, s + r - 1, flen, line, col, sev, msg);
    else if (s_nprobs && strstr(s_probs[s_nprobs - 1].msg, "Python exception") && strstr(s, "Error") && s[0] != ' ') {
        sfmt(s_probs[s_nprobs - 1].msg, sizeof s_probs[0].msg, "%s", s);
        problems_rebuild();
    }
}

/* =========================================================== output channel */

void panel_output(const char *line)
{
    if (s_nout == 2000) { free(s_out[0]); memmove(s_out, s_out + 1, 1999 * sizeof(char *)); s_nout--; }
    s_out[s_nout++] = _strdup(line);
    if (g_hwndPanel && s_tab == 1) InvalidateRect(g_hwndPanel, NULL, FALSE);
}

/* =========================================================== terminal buffer */

static TLine *last_line(Term *t) { return &t->lines[t->n - 1]; }

static void new_line(Term *t)
{
    if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 256; t->lines = (TLine *)realloc(t->lines, t->cap * sizeof(TLine)); }
    t->lines[t->n].s = (char *)malloc(64); t->lines[t->n].s[0] = 0;
    t->lines[t->n].len = 0; t->lines[t->n].cap = 64;
    t->n++;
    t->col = 0;
    if (t->n > SCROLLBACK) {
        int i, drop = 500;
        for (i = 0; i < drop; i++) free(t->lines[i].s);
        memmove(t->lines, t->lines + drop, (t->n - drop) * sizeof(TLine));
        t->n -= drop; t->top -= drop; if (t->top < 0) t->top = 0;
        t->hasSel = 0;
    }
}

static void put_char(Term *t, char c)
{
    TLine *l = last_line(t);
    if (t->col >= l->cap - 1) { l->cap = (t->col + 1) * 2; l->s = (char *)realloc(l->s, l->cap); }
    while (l->len < t->col) l->s[l->len++] = ' ';
    l->s[t->col++] = c;
    if (t->col > l->len) l->len = t->col;
    l->s[l->len] = 0;
}

static int is_prompt(TLine *l)
{
    return l->len >= 3 && isalpha((unsigned char)l->s[0]) && l->s[1] == ':' && l->s[2] == '\\' && l->s[l->len - 1] == '>';
}

static void term_clear_lines(Term *t)
{
    int i;
    for (i = 0; i < t->n; i++) free(t->lines[i].s);
    t->n = 0; t->top = 0; t->hasSel = 0;
    new_line(t);
}

static void term_put(Term *t, const char *d, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (t->esc) {                       /* skip ANSI escape sequences */
            if (t->esc == 1) t->esc = c == '[' ? 2 : 0;
            else if (c >= 0x40 && c <= 0x7E) t->esc = 0;
            continue;
        }
        switch (c) {
        case 27: t->esc = 1; break;
        case '\r': if (i + 1 < n && d[i + 1] == '\n') break; t->col = 0; break;
        case '\n': scan_problem(t, last_line(t)->s); new_line(t); break;
        case '\b': if (t->col > 0) t->col--; break;
        case '\f': term_clear_lines(t); break;
        case '\t': do put_char(t, ' '); while (t->col % 8); break;
        default: if (c >= 32) put_char(t, (char)c); break;
        }
    }
    if (is_prompt(last_line(t))) {
        TLine *l = last_line(t);
        int k = l->len - 1; if (k >= MAX_PATH) k = MAX_PATH - 1;
        memcpy(t->cwd, l->s, k); t->cwd[k] = 0;
    }
}

/* =========================================================== processes */

typedef BOOL (WINAPI *AttachConsoleFn)(DWORD);

/* Job Object functions are looked up at runtime: TCC's stock kernel32.def predates them. */
typedef HANDLE (WINAPI *CreateJobObjectFn)(LPSECURITY_ATTRIBUTES, LPCSTR);
typedef BOOL (WINAPI *SetInformationJobObjectFn)(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD);
typedef BOOL (WINAPI *AssignProcessToJobObjectFn)(HANDLE, HANDLE);
typedef BOOL (WINAPI *TerminateJobObjectFn)(HANDLE, UINT);
static CreateJobObjectFn pCreateJobObject;
static SetInformationJobObjectFn pSetInformationJobObject;
static AssignProcessToJobObjectFn pAssignProcessToJobObject;
static TerminateJobObjectFn pTerminateJobObject;

static void load_job_api(void)
{
    HMODULE k = GetModuleHandle("kernel32.dll");
    if (pCreateJobObject) return;
    pCreateJobObject = (CreateJobObjectFn)GetProcAddress(k, "CreateJobObjectA");
    pSetInformationJobObject = (SetInformationJobObjectFn)GetProcAddress(k, "SetInformationJobObject");
    pAssignProcessToJobObject = (AssignProcessToJobObjectFn)GetProcAddress(k, "AssignProcessToJobObject");
    pTerminateJobObject = (TerminateJobObjectFn)GetProcAddress(k, "TerminateJobObject");
}

static DWORD WINAPI reader_thread(LPVOID p)
{
    Term *t = (Term *)p; char buf[4096]; DWORD n; HANDLE h = t->hOut;
    while (ReadFile(h, buf, sizeof buf, &n, NULL) && n) {
        TermChunk *c = (TermChunk *)malloc(sizeof(TermChunk) + n);
        c->n = (int)n; memcpy(c->data, buf, n);
        PostMessage(g_hwndPanel, WM_APP_TERMDATA, (WPARAM)t, (LPARAM)c);
    }
    PostMessage(g_hwndPanel, WM_APP_TERMEXIT, (WPARAM)t, 0);
    return 0;
}

static int term_start(Term *t, const char *cwd)
{
    SECURITY_ATTRIBUTES sa; HANDLE inR, inW, outR, outW; STARTUPINFO si; PROCESS_INFORMATION pi;
    char cmd[MAX_PATH + 16], comspec[MAX_PATH]; JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; DWORD tid;
    sa.nLength = sizeof sa; sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return 0;
    if (!CreatePipe(&inR, &inW, &sa, 0)) { CloseHandle(outR); CloseHandle(outW); return 0; }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
    if (!GetEnvironmentVariable("ComSpec", comspec, sizeof comspec)) strcpy(comspec, "cmd.exe");
    sfmt(cmd, sizeof cmd, "\"%s\"", comspec);
    {   /* with no folder open, start in the user's profile like VS Code does */
        static char home[MAX_PATH];
        if (!cwd || !dir_exists(cwd)) cwd = GetEnvironmentVariable("USERPROFILE", home, MAX_PATH) ? home : NULL;
    }
    if (!CreateProcess(NULL, cmd, NULL, NULL, TRUE, CREATE_NEW_CONSOLE, NULL, cwd, &si, &pi)) {
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
        return 0;
    }
    CloseHandle(inR); CloseHandle(outW); CloseHandle(pi.hThread);
    load_job_api();
    t->hJob = pCreateJobObject ? pCreateJobObject(NULL, NULL) : NULL;
    if (t->hJob) {
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        pSetInformationJobObject(t->hJob, JobObjectExtendedLimitInformation, &li, sizeof li);
        pAssignProcessToJobObject(t->hJob, pi.hProcess);
    }
    t->hProc = pi.hProcess; t->pid = pi.dwProcessId; t->hIn = inW; t->hOut = outR;
    t->alive = 1; t->follow = 1;
    if (cwd) sfmt(t->cwd, MAX_PATH, "%s", cwd);
    t->hThread = CreateThread(NULL, 0, reader_thread, t, 0, &tid);
    return 1;
}

static void term_stop(Term *t)
{
    if (t->hJob) { pTerminateJobObject(t->hJob, 1); CloseHandle(t->hJob); t->hJob = NULL; }
    else if (t->hProc) TerminateProcess(t->hProc, 1);
    if (t->hIn) { CloseHandle(t->hIn); t->hIn = NULL; }
    if (t->hProc) { CloseHandle(t->hProc); t->hProc = NULL; }
    if (t->hThread) { CloseHandle(t->hThread); t->hThread = NULL; }
    t->hOut = NULL;   /* the reader thread closes on EOF; handle leaks are harmless here */
    t->alive = 0;
}

static void term_write(Term *t, const char *s, int n)
{
    DWORD wr;
    if (t->alive && t->hIn) WriteFile(t->hIn, s, n, &wr, NULL);
}

static void term_ctrl_c(Term *t)
{
    static AttachConsoleFn attach;
    if (!attach) attach = (AttachConsoleFn)GetProcAddress(GetModuleHandle("kernel32.dll"), "AttachConsole");
    if (!t->alive || !attach) return;
    FreeConsole();
    if (attach(t->pid)) {
        SetConsoleCtrlHandler(NULL, TRUE);
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        Sleep(20);
        FreeConsole();
    }
}

void term_new(const char *cwd)
{
    Term *t;
    if (s_nterms >= MAXTERMS) { out_log("Terminal limit reached (%d)", MAXTERMS); return; }
    t = (Term *)calloc(1, sizeof(Term));
    t->number = ++s_termCounter;
    new_line(t);
    if (!term_start(t, cwd)) {
        put_char(t, '!'); term_put(t, " Could not start cmd.exe", 24);
    }
    s_terms[s_nterms++] = t;
    s_cur = s_nterms - 1;
    s_tab = 2;
    out_log("Started terminal %d (cmd.exe, pid %lu) in %s", t->number, t->pid, cwd ? cwd : "current directory");
    if (g_hwndPanel) { InvalidateRect(g_hwndPanel, NULL, FALSE); if (IsWindowVisible(g_hwndPanel)) SetFocus(g_hwndPanel); }
}

void term_kill(void)
{
    Term *t = curterm(); int i;
    if (!t) return;
    term_stop(t);
    for (i = 0; i < t->n; i++) free(t->lines[i].s);
    free(t->lines);
    memmove(s_terms + s_cur, s_terms + s_cur + 1, (s_nterms - s_cur - 1) * sizeof(Term *));
    s_nterms--;
    free(t);
    if (s_cur >= s_nterms) s_cur = s_nterms - 1;
    out_log("Terminal killed");
    InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_shutdown(void)
{
    int i;
    for (i = 0; i < s_nterms; i++) term_stop(s_terms[i]);
}

void term_clear(void)
{
    Term *t = curterm(); TLine keep; int i;
    if (!t) return;
    keep = *last_line(t);
    for (i = 0; i < t->n - 1; i++) free(t->lines[i].s);
    t->lines[0] = keep; t->n = 1; t->top = 0; t->hasSel = 0;
    InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_send_line(const char *cmd)
{
    Term *t = curterm();
    if (!t || !t->alive) { term_new(g_root[0] ? g_root : NULL); t = curterm(); }
    if (!t) return;
    problems_clear();
    term_write(t, cmd, (int)strlen(cmd));
    term_write(t, "\r\n", 2);
    t->follow = 1;
    s_tab = 2;
    InvalidateRect(g_hwndPanel, NULL, FALSE);
}

void term_focus(void)
{
    if (!g_panelVisible) return;
    if (s_tab == 2 && !curterm()) term_new(g_root[0] ? g_root : NULL);
    SetFocus(g_hwndPanel);
}

void panel_show_tab(int tab)
{
    s_tab = tab;
    if (tab == 2 && !curterm()) term_new(g_root[0] ? g_root : NULL);
    InvalidateRect(g_hwndPanel, NULL, FALSE);
}

/* =========================================================== geometry */

static int content_w(void)
{
    RECT r; GetClientRect(g_hwndPanel, &r);
    return r.right - (s_tab == 2 && s_nterms > 1 ? LIST_W : 0) - 12;
}
static int vis_rows(void) { RECT r; GetClientRect(g_hwndPanel, &r); return (r.bottom - HEADER_H - 4) / g_codeCH; }

static void follow_bottom(Term *t)
{
    int vis = vis_rows();
    if (t->follow) { t->top = t->n - vis; if (t->top < 0) t->top = 0; }
}

static void place_caret(void)
{
    Term *t = curterm();
    if (GetFocus() != g_hwndPanel) return;
    if (s_tab != 2 || !t) { SetCaretPos(-100, -100); return; }
    {
        int row = t->n - 1 - t->top, x = 12 + (last_line(t)->len + t->incur) * g_codeCW;
        SetCaretPos(x, HEADER_H + 4 + row * g_codeCH + 1);
    }
}

/* =========================================================== painting */

static void paint_header(HDC dc, int w)
{
    static const char *names[3] = { "PROBLEMS", "OUTPUT", "TERMINAL" };
    int i, x = 20, e, wn;
    fill(dc, 0, 0, w, HEADER_H, C_EDITOR_BG);
    problems_count(&e, &wn);
    for (i = 0; i < 3; i++) {
        int tw = text_w(dc, g_fUISmall, names[i], -1), extra = 0;
        COLORREF c = s_tab == i ? HEX(0xE7E7E7) : HEX(0x969696);
        text_at(dc, x, 11, names[i], -1, c, g_fUISmall);
        if (i == 0 && e + wn) {
            char b[16]; int bw; sfmt(b, sizeof b, "%d", e + wn); bw = text_w(dc, g_fUISmall, b, -1) + 10;
            fill(dc, x + tw + 6, 9, bw, 16, HEX(0x4D4D4D)); text_at(dc, x + tw + 11, 10, b, -1, C_TEXT_BRIGHT, g_fUISmall);
            extra = bw + 6;
        }
        if (s_tab == i) hline(dc, x, 29, tw + extra, HEX(0xE7E7E7));
        SetRect(&s_tabRc[i], x - 8, 0, x + tw + extra + 8, HEADER_H);
        x += tw + extra + 28;
    }
    SetRect(&s_btnClose, w - 30, 7, w - 8, 29);
    SetRect(&s_btnKill, w - 56, 7, w - 34, 29);
    SetRect(&s_btnClear, w - 82, 7, w - 60, 29);
    SetRect(&s_btnNew, w - 108, 7, w - 86, 29);
    if (s_tab == 2) {
        if (s_hoverBtn == 0) fill_rc(dc, &s_btnNew, HEX(0x3A3D41));
        if (s_hoverBtn == 1) fill_rc(dc, &s_btnKill, HEX(0x3A3D41));
        if (s_hoverBtn == 3) fill_rc(dc, &s_btnClear, HEX(0x3A3D41));
        draw_icon(dc, ICON_PLUS, s_btnNew.left + 11, 18, 14, C_SIDEBAR_TEXT);
        draw_icon(dc, ICON_TRASH, s_btnKill.left + 11, 18, 14, C_SIDEBAR_TEXT);
        draw_icon(dc, ICON_COLLAPSE, s_btnClear.left + 11, 18, 14, C_SIDEBAR_TEXT);
        if (curterm()) {
            char b[64]; sfmt(b, sizeof b, "%d: cmd", curterm()->number);
            text_at(dc, s_btnNew.left - 10 - text_w(dc, g_fUI, b, -1), 10, b, -1, C_SIDEBAR_TEXT, g_fUI);
        }
    }
    if (s_hoverBtn == 2) fill_rc(dc, &s_btnClose, HEX(0x3A3D41));
    draw_icon(dc, ICON_CLOSE, s_btnClose.left + 11, 18, 14, C_SIDEBAR_TEXT);
}

static int sel_contains(Term *t, int line, int col)
{
    int a0 = t->selA[0], a1 = t->selA[1], b0 = t->selB[0], b1 = t->selB[1];
    if (!t->hasSel) return 0;
    if (a0 > b0 || (a0 == b0 && a1 > b1)) { int x = a0, y = a1; a0 = b0; a1 = b1; b0 = x; b1 = y; }
    if (line < a0 || line > b0) return 0;
    if (line == a0 && col < a1) return 0;
    if (line == b0 && col >= b1) return 0;
    return 1;
}

static void paint_terminal(HDC dc, int w, int h)
{
    Term *t = curterm(); int i, vis = vis_rows() + 1, y = HEADER_H + 4, cw = content_w();
    if (!t) {
        text_at(dc, 20, HEADER_H + 10, "No terminal is running. Press Ctrl+Shift+` to start one.", -1, C_TEXT_MUTED, g_fUI);
        return;
    }
    follow_bottom(t);
    SelectObject(dc, g_fCode);
    for (i = 0; i < vis && t->top + i < t->n; i++, y += g_codeCH) {
        int li = t->top + i; TLine *l = &t->lines[li]; COLORREF c = HEX(0xCCCCCC);
        if (t->hasSel) {
            int k, start = -1;
            for (k = 0; k <= l->len + 1; k++) {
                int in = k <= l->len && sel_contains(t, li, k);
                if (in && start < 0) start = k;
                if (!in && start >= 0) { fill(dc, 12 + start * g_codeCW, y, (k - start) * g_codeCW, g_codeCH, C_SELECTION); start = -1; }
            }
        }
        if (strstr(l->s, ": error") || strstr(l->s, "Error:") || strstr(l->s, "Traceback")) c = C_ERROR;
        else if (strstr(l->s, ": warning")) c = C_WARNING;
        text_at(dc, 12, y + 2, l->s, l->len, c, g_fCode);
        if (li == t->n - 1 && t->inlen) text_at(dc, 12 + l->len * g_codeCW, y + 2, t->input, t->inlen, HEX(0xFFFFFF), g_fCode);
    }
    if (!t->alive) {
        char b[96]; sfmt(b, sizeof b, "The terminal process exited (code %d). Press Enter to restart it.", t->exitCode);
        if (y < h) { fill(dc, 12, y + 4, text_w(dc, g_fUI, b, -1) + 16, 20, HEX(0x3A3D41)); text_at(dc, 20, y + 7, b, -1, C_SIDEBAR_TEXT, g_fUI); }
    }
    /* scrollbar */
    if (t->n > vis_rows()) {
        int th = h - HEADER_H, thumb = th * vis_rows() / t->n, ty;
        if (thumb < 20) thumb = 20;
        ty = HEADER_H + (th - thumb) * t->top / (t->n - vis_rows() > 0 ? t->n - vis_rows() : 1);
        fill(dc, 12 + cw, ty, 10, thumb, HEX(0x424242));
    }
    /* terminal list */
    if (s_nterms > 1) {
        int x = w - LIST_W;
        vline(dc, x, HEADER_H, h - HEADER_H, C_BORDER);
        for (i = 0; i < s_nterms; i++) {
            int ry = HEADER_H + i * 22; char b[32];
            if (i == s_cur) fill(dc, x + 1, ry, LIST_W - 1, 22, C_LIST_ACTIVE);
            else if (i == s_hoverRow) fill(dc, x + 1, ry, LIST_W - 1, 22, C_LIST_HOVER);
            draw_icon(dc, ICON_TERMINAL, x + 18, ry + 11, 14, s_terms[i]->alive ? C_SIDEBAR_TEXT : C_TEXT_DIM);
            sfmt(b, sizeof b, "%d: cmd%s", s_terms[i]->number, s_terms[i]->alive ? "" : " (exited)");
            text_at(dc, x + 32, ry + 4, b, -1, C_SIDEBAR_TEXT, g_fUI);
        }
    }
}

static void paint_output(HDC dc, int w, int h)
{
    int vis = vis_rows(), i, y = HEADER_H + 4;
    s_outTop = s_nout - vis; if (s_outTop < 0) s_outTop = 0;
    for (i = s_outTop; i < s_nout && y < h; i++, y += g_codeCH)
        text_at(dc, 12, y + 2, s_out[i], -1, HEX(0xCCCCCC), g_fCode);
    (void)w;
}

static void paint_problems(HDC dc, int w, int h)
{
    int i, y = HEADER_H + 2;
    if (!s_nprobs) { text_at(dc, 20, HEADER_H + 8, "No problems have been detected in the workspace.", -1, C_SIDEBAR_TEXT, g_fUI); return; }
    for (i = s_probScroll; i < s_nprows && y < h; i++, y += 22) {
        PRow *r = &s_prows[i];
        if (i == s_hoverRow) fill(dc, 0, y, w, 22, C_LIST_HOVER);
        if (r->prob < 0) {
            Problem *p = &s_probs[r->file]; char rel[MAX_PATH], b[16]; int cnt = 0, k, x;
            for (k = 0; k < s_nprobs; k++) if (!_stricmp(s_probs[k].file, p->file)) cnt++;
            draw_icon(dc, ICON_CHEVRON_DOWN, 14, y + 11, 14, C_SIDEBAR_TEXT);
            file_badge(dc, path_name(p->file), 0, 0, 24, y + 11);
            text_at(dc, 44, y + 4, path_name(p->file), -1, C_SIDEBAR_TEXT, g_fUI);
            x = 52 + text_w(dc, g_fUI, path_name(p->file), -1);
            path_rel(p->file, rel, sizeof rel); *(char *)path_name(rel) = 0;
            text_at(dc, x, y + 4, rel, -1, C_TEXT_MUTED, g_fUISmall);
            x += text_w(dc, g_fUISmall, rel, -1) + 8;
            sfmt(b, sizeof b, "%d", cnt);
            fill(dc, x, y + 3, text_w(dc, g_fUISmall, b, -1) + 10, 16, HEX(0x4D4D4D));
            text_at(dc, x + 5, y + 4, b, -1, C_SIDEBAR_TEXT, g_fUISmall);
        } else {
            Problem *p = &s_probs[r->prob]; char loc[48]; int x;
            draw_icon(dc, p->sev ? ICON_WARNING : ICON_ERROR, 44, y + 11, 14, p->sev ? C_WARNING : C_ERROR);
            text_at(dc, 60, y + 4, p->msg, -1, C_SIDEBAR_TEXT, g_fUI);
            x = 68 + text_w(dc, g_fUI, p->msg, -1);
            sfmt(loc, sizeof loc, "[Ln %d, Col %d]", p->line, p->col);
            text_at(dc, x, y + 4, loc, -1, C_TEXT_MUTED, g_fUI);
        }
    }
}

static void panel_paint(HWND w)
{
    PAINTSTRUCT ps; RECT rc; HDC dc = BeginPaint(w, &ps), mem; HBITMAP bmp, ob;
    GetClientRect(w, &rc);
    mem = CreateCompatibleDC(dc); bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = (HBITMAP)SelectObject(mem, bmp);
    SetBkMode(mem, TRANSPARENT);
    fill_rc(mem, &rc, C_EDITOR_BG);
    paint_header(mem, rc.right);
    if (s_tab == 0) paint_problems(mem, rc.right, rc.bottom);
    else if (s_tab == 1) paint_output(mem, rc.right, rc.bottom);
    else paint_terminal(mem, rc.right, rc.bottom);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(w, &ps);
    place_caret();
}

/* =========================================================== input */

static void input_insert(Term *t, const char *s, int n)
{
    if (t->inlen + n >= (int)sizeof t->input - 1) n = (int)sizeof t->input - 1 - t->inlen;
    if (n <= 0) return;
    memmove(t->input + t->incur + n, t->input + t->incur, t->inlen - t->incur);
    memcpy(t->input + t->incur, s, n);
    t->inlen += n; t->incur += n;
    t->follow = 1; t->tabCycle = 0;
}

static void input_set(Term *t, const char *s)
{
    int n = (int)strlen(s); if (n > (int)sizeof t->input - 1) n = (int)sizeof t->input - 1;
    memcpy(t->input, s, n); t->inlen = t->incur = n;
}

static void submit(Term *t)
{
    char line[1100]; int atPrompt = is_prompt(last_line(t));
    if (!t->alive) {
        term_stop(t); new_line(t);
        if (term_start(t, t->cwd[0] ? t->cwd : g_root)) out_log("Restarted terminal %d", t->number);
        return;
    }
    memcpy(line, t->input, t->inlen); line[t->inlen] = 0;
    if (t->inlen) {
        if (!t->nhist || strcmp(t->hist[t->nhist - 1], line)) {
            if (t->nhist == 50) { memmove(t->hist, t->hist + 1, 49 * sizeof t->hist[0]); t->nhist--; }
            sfmt(t->hist[t->nhist++], 256, "%s", line);
        }
    }
    t->histpos = t->nhist;
    if (atPrompt) problems_clear();
    else { term_put(t, line, t->inlen); term_put(t, "\r\n", 2); }   /* programs don't echo stdin */
    if (!_stricmp(line, "cls")) term_clear_lines(t);
    term_write(t, line, t->inlen);
    term_write(t, "\r\n", 2);
    t->inlen = t->incur = 0; t->follow = 1; t->hasSel = 0;
}

static void tab_complete(Term *t)
{
    char pat[MAX_PATH * 2], word[256]; int start = t->incur, i, idx = 0; WIN32_FIND_DATA fd; HANDLE h;
    while (start > 0 && t->input[start - 1] != ' ' && t->input[start - 1] != '"') start--;
    if (!t->tabCycle) { memcpy(t->tabBase, t->input + start, t->incur - start); t->tabBase[t->incur - start] = 0; }
    if (t->tabBase[1] == ':') sfmt(pat, sizeof pat, "%s*", t->tabBase);
    else sfmt(pat, sizeof pat, "%s\\%s*", t->cwd[0] ? t->cwd : ".", t->tabBase);
    h = FindFirstFile(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (idx++ == t->tabCycle) {
            const char *slash = strrchr(t->tabBase, '\\');
            int keep = slash ? (int)(slash - t->tabBase) + 1 : 0;
            sfmt(word, sizeof word, "%.*s%s%s", keep, t->tabBase, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "\\" : "");
            t->inlen = start; t->incur = start;
            for (i = 0; word[i] && t->inlen < (int)sizeof t->input - 1; i++) { t->input[t->inlen++] = word[i]; }
            t->incur = t->inlen;
            t->tabCycle++;
            FindClose(h);
            return;
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
    t->tabCycle = 0;
}

static void copy_selection(Term *t)
{
    int a0 = t->selA[0], a1 = t->selA[1], b0 = t->selB[0], b1 = t->selB[1], i, n = 0, cap = 4096;
    char *buf = (char *)malloc(cap); HGLOBAL hg; char *p;
    if (a0 > b0 || (a0 == b0 && a1 > b1)) { int x = a0, y = a1; a0 = b0; a1 = b1; b0 = x; b1 = y; }
    for (i = a0; i <= b0 && i < t->n; i++) {
        TLine *l = &t->lines[i]; int from = i == a0 ? a1 : 0, to = i == b0 ? b1 : l->len;
        if (from > l->len) from = l->len;
        if (to > l->len) to = l->len;
        if (n + (to - from) + 3 > cap) { cap = (n + to - from + 3) * 2; buf = (char *)realloc(buf, cap); }
        memcpy(buf + n, l->s + from, to - from); n += to - from;
        if (i < b0) { buf[n++] = '\r'; buf[n++] = '\n'; }
    }
    buf[n] = 0;
    hg = GlobalAlloc(GMEM_MOVEABLE, n + 1); p = (char *)GlobalLock(hg); memcpy(p, buf, n + 1); GlobalUnlock(hg);
    if (OpenClipboard(g_hwndPanel)) { EmptyClipboard(); SetClipboardData(CF_TEXT, hg); CloseClipboard(); }
    free(buf);
    t->hasSel = 0;
}

static void paste(Term *t)
{
    HANDLE h; char *p;
    if (!OpenClipboard(g_hwndPanel)) return;
    if ((h = GetClipboardData(CF_TEXT)) != NULL && (p = (char *)GlobalLock(h)) != NULL) {
        char *copy = _strdup(p), *s = copy, *nl;
        GlobalUnlock(h); CloseClipboard();
        while ((nl = strchr(s, '\n')) != NULL) {       /* multi-line paste: submit each line */
            int n = (int)(nl - s); if (n && s[n - 1] == '\r') n--;
            input_insert(t, s, n); submit(t);
            s = nl + 1;
        }
        input_insert(t, s, (int)strlen(s));
        free(copy);
        return;
    }
    CloseClipboard();
}

static int term_key(Term *t, int vk)
{
    int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    switch (vk) {
    case VK_RETURN: submit(t); return 1;
    case VK_BACK:
        if (t->incur > 0) { memmove(t->input + t->incur - 1, t->input + t->incur, t->inlen - t->incur); t->incur--; t->inlen--; }
        t->tabCycle = 0; return 1;
    case VK_DELETE:
        if (t->incur < t->inlen) { memmove(t->input + t->incur, t->input + t->incur + 1, t->inlen - t->incur - 1); t->inlen--; }
        return 1;
    case VK_LEFT: if (t->incur > 0) t->incur--; return 1;
    case VK_RIGHT: if (t->incur < t->inlen) t->incur++; return 1;
    case VK_HOME: if (ctrl) { t->top = 0; t->follow = 0; } else t->incur = 0; return 1;
    case VK_END: if (ctrl) t->follow = 1; else t->incur = t->inlen; return 1;
    case VK_UP:
        if (t->histpos > 0) { t->histpos--; input_set(t, t->hist[t->histpos]); }
        return 1;
    case VK_DOWN:
        if (t->histpos < t->nhist - 1) { t->histpos++; input_set(t, t->hist[t->histpos]); }
        else { t->histpos = t->nhist; input_set(t, ""); }
        return 1;
    case VK_PRIOR: t->top -= vis_rows(); if (t->top < 0) t->top = 0; t->follow = 0; return 1;
    case VK_NEXT:
        t->top += vis_rows();
        if (t->top >= t->n - vis_rows()) t->follow = 1;
        return 1;
    case VK_ESCAPE: t->inlen = t->incur = 0; t->hasSel = 0; return 1;
    case VK_TAB: if (!ctrl) { tab_complete(t); return 1; } return 0;
    case VK_INSERT: if (shift) { paste(t); return 1; } if (ctrl && t->hasSel) { copy_selection(t); return 1; } return 0;
    }
    if (ctrl) switch (vk) {
    case 'C':
        if (t->hasSel) copy_selection(t);
        else { t->inlen = t->incur = 0; term_ctrl_c(t); }   /* cmd prints the ^C itself */
        return 1;
    case 'V': paste(t); return 1;
    case 'L': term_clear(); return 1;
    case 'A': t->selA[0] = 0; t->selA[1] = 0; t->selB[0] = t->n - 1; t->selB[1] = last_line(t)->len; t->hasSel = 1; return 1;
    }
    return 0;
}

static void term_hit(Term *t, int x, int y, int *line, int *col)
{
    *line = t->top + (y - HEADER_H - 4) / g_codeCH;
    if (*line < 0) *line = 0;
    if (*line >= t->n) *line = t->n - 1;
    *col = (x - 12 + g_codeCW / 2) / g_codeCW;
    if (*col < 0) *col = 0;
}

static void open_location_at(Term *t, int line)
{
    int flen, ln, col, sev, r; const char *msg, *s = t->lines[line].s; char path[MAX_PATH];
    while (*s == ' ') s++;
    r = parse_location(s, &flen, &ln, &col, &sev, &msg);
    if (!r) return;
    resolve_path(t, s + r - 1, flen, path);
    if (file_exists(path) && editor_open(path)) editor_goto(ln, col, 0);
}

static LRESULT CALLBACK PanelProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    Term *t = curterm();
    switch (m) {
    case WM_APP_TERMDATA: {
        Term *src = (Term *)wp; TermChunk *c = (TermChunk *)lp; int i, ok = 0;
        for (i = 0; i < s_nterms; i++) if (s_terms[i] == src) ok = 1;
        if (ok) { term_put(src, c->data, c->n); if (src == t || s_tab == 2) InvalidateRect(w, NULL, FALSE); }
        free(c);
        return 0;
    }
    case WM_APP_TERMEXIT: {
        Term *src = (Term *)wp; int i;
        for (i = 0; i < s_nterms; i++) if (s_terms[i] == src && src->alive) {
            DWORD code = 0;
            if (src->hProc) GetExitCodeProcess(src->hProc, &code);
            src->exitCode = (int)code;
            term_stop(src);
            out_log("Terminal %d exited with code %d", src->number, src->exitCode);
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    }
    case WM_PAINT: panel_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(w, NULL, FALSE); return 0;
    case WM_SETFOCUS: CreateCaret(w, NULL, 2, g_codeCH - 2); ShowCaret(w); place_caret(); InvalidateRect(w, NULL, FALSE); return 0;
    case WM_KILLFOCUS: DestroyCaret(); return 0;
    case WM_KEYDOWN:
        if (s_tab == 2 && t && term_key(t, (int)wp)) { InvalidateRect(w, NULL, FALSE); return 0; }
        break;
    case WM_CHAR:
        if (s_tab == 2 && t && wp >= 32) { char c = (char)wp; t->hasSel = 0; input_insert(t, &c, 1); InvalidateRect(w, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i; POINT pt; RECT rc;
        pt.x = x; pt.y = y; GetClientRect(w, &rc);
        SetFocus(w);
        for (i = 0; i < 3; i++) if (PtInRect(&s_tabRc[i], pt)) { panel_show_tab(i); return 0; }
        if (PtInRect(&s_btnClose, pt)) { app_command(CMD_TOGGLE_PANEL); return 0; }
        if (s_tab == 2) {
            if (PtInRect(&s_btnNew, pt)) { app_command(CMD_NEW_TERMINAL); return 0; }
            if (PtInRect(&s_btnKill, pt)) { term_kill(); return 0; }
            if (PtInRect(&s_btnClear, pt)) { term_clear(); return 0; }
            if (s_nterms > 1 && x >= rc.right - LIST_W) {
                i = (y - HEADER_H) / 22;
                if (y >= HEADER_H && i < s_nterms) { s_cur = i; InvalidateRect(w, NULL, FALSE); }
                return 0;
            }
            if (t && y > HEADER_H) {
                int line, col; term_hit(t, x, y, &line, &col);
                if (GetKeyState(VK_CONTROL) < 0) { open_location_at(t, line); return 0; }
                if (m == WM_LBUTTONDBLCLK) {
                    TLine *l = &t->lines[line]; int a = col, b = col;
                    while (a > 0 && a <= l->len && !isspace((unsigned char)l->s[a - 1])) a--;
                    while (b < l->len && !isspace((unsigned char)l->s[b])) b++;
                    t->selA[0] = t->selB[0] = line; t->selA[1] = a; t->selB[1] = b; t->hasSel = b > a;
                } else {
                    t->selA[0] = t->selB[0] = line; t->selA[1] = t->selB[1] = col; t->hasSel = 0;
                    s_selecting = 1; SetCapture(w);
                }
                InvalidateRect(w, NULL, FALSE);
            }
        } else if (s_tab == 0 && y > HEADER_H) {
            int r = s_probScroll + (y - HEADER_H - 2) / 22;
            if (r >= 0 && r < s_nprows && s_prows[r].prob >= 0) {
                Problem *p = &s_probs[s_prows[r].prob];
                if (editor_open(p->file)) editor_goto(p->line, p->col, 0);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), hb = -1, hr = -1; POINT pt; RECT rc; TRACKMOUSEEVENT tm;
        pt.x = x; pt.y = y; GetClientRect(w, &rc);
        if (s_selecting && t) {
            int line, col; term_hit(t, x, y, &line, &col);
            t->selB[0] = line; t->selB[1] = col;
            t->hasSel = t->selA[0] != line || t->selA[1] != col;
            if (y < HEADER_H && t->top > 0) { t->top--; t->follow = 0; }
            InvalidateRect(w, NULL, FALSE);
            return 0;
        }
        if (PtInRect(&s_btnNew, pt)) hb = 0; else if (PtInRect(&s_btnKill, pt)) hb = 1;
        else if (PtInRect(&s_btnClose, pt)) hb = 2; else if (PtInRect(&s_btnClear, pt)) hb = 3;
        if (s_tab == 2 && s_nterms > 1 && x >= rc.right - LIST_W && y >= HEADER_H) hr = (y - HEADER_H) / 22;
        if (s_tab == 0 && y > HEADER_H) hr = s_probScroll + (y - HEADER_H - 2) / 22;
        if (hb != s_hoverBtn || hr != s_hoverRow) { s_hoverBtn = hb; s_hoverRow = hr; InvalidateRect(w, NULL, FALSE); }
        tm.cbSize = sizeof tm; tm.dwFlags = TME_LEAVE; tm.hwndTrack = w; tm.dwHoverTime = 0; TrackMouseEvent(&tm);
        return 0;
    }
    case WM_MOUSELEAVE: s_hoverBtn = s_hoverRow = -1; InvalidateRect(w, NULL, FALSE); return 0;
    case WM_LBUTTONUP: if (s_selecting) { s_selecting = 0; ReleaseCapture(); } return 0;
    case WM_RBUTTONDOWN:
        SetFocus(w);
        if (s_tab == 2 && t) { if (t->hasSel) copy_selection(t); else paste(t); InvalidateRect(w, NULL, FALSE); }
        return 0;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp) / 40;
        if (s_tab == 2 && t) {
            t->top -= d; t->follow = 0;
            if (t->top < 0) t->top = 0;
            if (t->top >= t->n - vis_rows()) { t->follow = 1; }
        } else if (s_tab == 0) {
            s_probScroll -= d; if (s_probScroll > s_nprows - 1) s_probScroll = s_nprows - 1; if (s_probScroll < 0) s_probScroll = 0;
        }
        InvalidateRect(w, NULL, FALSE);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && s_tab == 2) {
            POINT pt; RECT rc; GetCursorPos(&pt); ScreenToClient(w, &pt); GetClientRect(w, &rc);
            if (pt.y > HEADER_H && pt.x < rc.right - (s_nterms > 1 ? LIST_W : 0)) { SetCursor(LoadCursor(NULL, IDC_IBEAM)); return TRUE; }
        }
        break;
    }
    return DefWindowProc(w, m, wp, lp);
}

void panel_register(void)
{
    WNDCLASS wc; memset(&wc, 0, sizeof wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = PanelProc; wc.hInstance = g_hinst; wc.lpszClassName = "XPPanel";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClass(&wc);
}
