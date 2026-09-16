/*
 * dirwhale — a tiny zero-dependency disk usage analyzer.
 *
 * Scans a directory tree, aggregates sizes, and shows which entries
 * eat the most space. Works on Windows (MinGW) and POSIX.
 *
 * Usage:
 *   dirwhale [options] [path]
 *
 * Options:
 *   -d, --depth N      show children down to depth N (default 1)
 *   -n, --top N        show only the N biggest entries per level (default 15)
 *   -e, --exclude PAT  skip entries matching wildcard pattern (* and ?),
 *                      may be repeated
 *   -t, --types        show file-type (extension) breakdown
 *   -j, --json FILE    write the scan result to FILE as JSON
 *   -q, --quiet        no scan-progress on stderr
 *   -V, --version      print version and exit
 *   -h, --help         show this help
 *
 * License: MIT
 */

#if !defined(_WIN32)
#  if defined(__APPLE__)
#    define _DARWIN_C_SOURCE
#  else
#    ifndef _DEFAULT_SOURCE
#      define _DEFAULT_SOURCE
#    endif
#    ifndef _POSIX_C_SOURCE
#      define _POSIX_C_SOURCE 200809L
#    endif
#  endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  include <wchar.h>
#  define PATH_SEP '\\'
#  define STDERR_ISATTY() _isatty(_fileno(stderr))
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  define PATH_SEP '/'
#  define STDERR_ISATTY() isatty(STDERR_FILENO)
#endif

#ifdef __MINGW32__
/*
 * MinGW's CRT expands wildcards in argv before main() runs, which would
 * turn `dirwhale -e *` into a pile of positional arguments. We do our own
 * (per-entry, basename-only) matching, so switch CRT globbing off.
 */
int _dowildcard = 0;
#endif

#define DIRWHALE_VERSION "0.2.0"

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct Node {
    char         *name;           /* owned; basename (root: user path)  */
    char         *path;           /* owned; only set on the root node   */
    uint64_t      size;           /* recursive size in bytes            */
    uint64_t      files;          /* recursive file count               */
    int           is_dir;
    struct Node **children;       /* owned array of owned pointers      */
    size_t        n_children;
    size_t        cap_children;
    size_t        n_hidden;       /* pruned siblings not stored         */
    uint64_t      hidden_size;
} Node;

typedef struct ExtStat {
    char    *ext;                 /* owned; "" for extensionless files  */
    uint64_t size;
    uint64_t files;
} ExtStat;

typedef struct StrVec {
    char  **items;
    size_t  len, cap;
} StrVec;

typedef struct ExtVec {
    ExtStat *items;
    size_t   len, cap;
} ExtVec;

typedef struct Options {
    const char  *root;
    int          depth;           /* how deep to print (root children=1)*/
    int          top;             /* entries per level to display       */
    int          types;           /* print extension breakdown          */
    int          quiet;
    const char  *json_path;
    StrVec       excludes;
    ExtVec      *exts;            /* extension stats accumulator        */
    int          root_given;
} Options;

#ifdef _WIN32
typedef struct WPath {
    wchar_t *s;
    size_t   len, cap;
} WPath;
#else
typedef struct PathBuf {
    char   *s;
    size_t  len, cap;
} PathBuf;
#endif

typedef struct ScanCtx {
    const Options *o;
#ifdef _WIN32
    WPath   wp;
    char   *nbuf;                 /* reusable UTF-8 basename buffer     */
    size_t  ncap;
#else
    PathBuf pb;
#endif
} ScanCtx;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void die_oom(void) {
    fprintf(stderr, "dirwhale: out of memory\n");
    exit(1);
}

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) die_oom();
    return q;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) die_oom();
    memcpy(p, s, n);
    return p;
}

static void strvec_push(StrVec *v, const char *s) {
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, v->cap * sizeof(char *));
    }
    v->items[v->len++] = xstrdup(s);
}

/* Wildcard match with * and ? (case-insensitive on Windows) */
static int wc_match(const char *pat, const char *str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (const char *s = str; ; s++) {
                if (wc_match(pat, s)) return 1;
                if (!*s) return 0;
            }
        }
        if (!*str) return 0;
        char a = *pat, b = *str;
#ifdef _WIN32
        a = (char)tolower((unsigned char)a);
        b = (char)tolower((unsigned char)b);
#endif
        if (a != '?' && a != b) return 0;
        pat++; str++;
    }
    return *str == 0;
}

static int excluded(const Options *o, const char *name) {
    for (size_t i = 0; i < o->excludes.len; i++)
        if (wc_match(o->excludes.items[i], name)) return 1;
    return 0;
}

/* Human-readable size: <1KiB in bytes, else KiB/MiB/GiB/TiB */
static void fmt_size(uint64_t bytes, char *buf, size_t bufsz) {
    if (bytes < 1024) { snprintf(buf, bufsz, "%llu B", (unsigned long long)bytes); return; }
    double v; const char *u;
    if (bytes < (1ull<<20))      { v = (double)bytes / 1024.0;                 u = "KiB"; }
    else if (bytes < (1ull<<30)) { v = (double)bytes / (1024.0*1024);          u = "MiB"; }
    else if (bytes < (1ull<<40)) { v = (double)bytes / (1024.0*1024*1024);     u = "GiB"; }
    else                         { v = (double)bytes / (1024.0*1024*1024*1024); u = "TiB"; }
    if (v >= 100)      snprintf(buf, bufsz, "%.0f %s", v, u);
    else if (v >= 10)  snprintf(buf, bufsz, "%.1f %s", v, u);
    else               snprintf(buf, bufsz, "%.2f %s", v, u);
}

/* ------------------------------------------------------------------ */
/* File-type statistics                                                */
/* ------------------------------------------------------------------ */

static const char *path_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    /* treat dotfiles like ".gitignore" as extensionless */
    if (!dot || dot == name) return "";
    return dot + 1;
}

static void extvec_add(ExtVec *v, const char *name, uint64_t size) {
    const char *e = path_ext(name);
    for (size_t i = 0; i < v->len; i++) {
#ifdef _WIN32
        if (_stricmp(v->items[i].ext, e) == 0) {
#else
        if (strcmp(v->items[i].ext, e) == 0) {
#endif
            v->items[i].size += size;
            v->items[i].files++;
            return;
        }
    }
    if (v->len == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 32;
        v->items = xrealloc(v->items, v->cap * sizeof(ExtStat));
    }
    v->items[v->len].ext   = xstrdup(e);
    v->items[v->len].size  = size;
    v->items[v->len].files = 1;
    v->len++;
}

static int ext_cmp(const void *a, const void *b) {
    const ExtStat *x = a, *y = b;
    if (x->size != y->size) return x->size > y->size ? -1 : 1;
    return strcmp(x->ext, y->ext);
}

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

static uint64_t g_files_scanned;
static uint64_t g_errors;         /* entries we could not stat or open  */
static uint64_t g_progress_shown; /* last progress count printed        */
static volatile sig_atomic_t g_abort;
static int g_show_progress;

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    if (g_abort) ExitProcess(130);
    g_abort = 1;
    return TRUE; /* let the scan unwind and print partial results */
}
#else
static void sigint_handler(int sig) {
    (void)sig;
    if (g_abort) _exit(130);
    g_abort = 1;
}
#endif

static void tick_progress(void) {
    if (g_show_progress && g_files_scanned - g_progress_shown >= 4096) {
        g_progress_shown = g_files_scanned;
        fprintf(stderr, "\r  scanned %llu files...",
                (unsigned long long)g_files_scanned);
        fflush(stderr);
    }
}

static Node *node_new(const char *name, int is_dir) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) die_oom();
    n->name   = xstrdup(name);
    n->is_dir = is_dir;
    return n;
}

static void node_add_child(Node *parent, Node *child) {
    if (parent->n_children == parent->cap_children) {
        parent->cap_children = parent->cap_children ? parent->cap_children * 2 : 8;
        parent->children = xrealloc(parent->children,
                                    parent->cap_children * sizeof(Node *));
    }
    parent->children[parent->n_children++] = child;
}

static void node_free(Node *n) {
    for (size_t i = 0; i < n->n_children; i++) node_free(n->children[i]);
    free(n->children);
    free(n->name);
    free(n->path);
    free(n);
}

static int node_cmp_desc(const void *a, const void *b) {
    const Node *const *x = a, *const *y = b;
    if ((*x)->size != (*y)->size) return (*x)->size > (*y)->size ? -1 : 1;
    return strcmp((*x)->name, (*y)->name);
}

static int keep_children(const Options *o, int depth) {
    return o->json_path != NULL || depth < o->depth;
}

static void finalize_children(Node *dir, const Options *o) {
    if (dir->n_children == 0) return;
    qsort(dir->children, dir->n_children, sizeof(Node *), node_cmp_desc);
    if (o->json_path) return;     /* JSON wants the whole tree */
    if (dir->n_children > (size_t)o->top) {
        for (size_t i = (size_t)o->top; i < dir->n_children; i++) {
            dir->hidden_size += dir->children[i]->size;
            dir->n_hidden++;
            node_free(dir->children[i]);
        }
        dir->n_children = (size_t)o->top;
    }
}

static void add_file(Node *dir, const Options *o, const char *nm,
                     uint64_t fsize, int keep) {
    dir->size  += fsize;
    dir->files += 1;
    g_files_scanned++;
    if (o->exts) extvec_add(o->exts, nm, fsize);
    if (keep) {
        Node *c = node_new(nm, 0);
        c->size  = fsize;
        c->files = 1;
        node_add_child(dir, c);
    }
    tick_progress();
}

static void scan_dir(Node *dir, ScanCtx *ctx, int depth);

static void scan_subdir(Node *dir, ScanCtx *ctx, int depth,
                        const char *nm, int keep) {
    if (keep) {
        Node *c = node_new(nm, 1);
        scan_dir(c, ctx, depth + 1);
        node_add_child(dir, c);
        dir->size  += c->size;
        dir->files += c->files;
    } else {
        Node sub;
        memset(&sub, 0, sizeof sub);
        sub.is_dir = 1;
        scan_dir(&sub, ctx, depth + 1);
        dir->size  += sub.size;
        dir->files += sub.files;
        /* unkept scans never store children, so nothing to free */
    }
}

#ifdef _WIN32

static void wpath_reserve(WPath *p, size_t need) {
    if (need <= p->cap) return;
    size_t ncap = p->cap ? p->cap : 512;
    while (ncap < need) ncap *= 2;
    p->s = xrealloc(p->s, ncap * sizeof(wchar_t));
    p->cap = ncap;
}

static void wpath_set(WPath *p, const wchar_t *src, size_t n) {
    wpath_reserve(p, n + 2);
    memcpy(p->s, src, (n + 1) * sizeof(wchar_t));
    p->len = n;
}

static void wpath_push(WPath *p, const wchar_t *name) {
    size_t nl = wcslen(name);
    int sep = p->len > 0 && p->s[p->len - 1] != L'\\';
    wpath_reserve(p, p->len + (size_t)sep + nl + 2);
    if (sep) p->s[p->len++] = L'\\';
    memcpy(p->s + p->len, name, (nl + 1) * sizeof(wchar_t));
    p->len += nl;
}

static void wpath_pop(WPath *p, size_t mark) {
    p->len = mark;
    p->s[p->len] = L'\0';
}

static char *wide_to_utf8(const wchar_t *w) {
    if (!w) return xstrdup("");
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return xstrdup("");
    char *s = malloc((size_t)n);
    if (!s) die_oom();
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static wchar_t *to_wide(const char *s, UINT cp) {
    int n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t)n * sizeof(wchar_t));
    if (!w) die_oom();
    MultiByteToWideChar(cp, 0, s, -1, w, n);
    return w;
}

static const char *wname_utf8(ScanCtx *ctx, const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    if ((size_t)n > ctx->ncap) {
        ctx->ncap = (size_t)n + 64;
        ctx->nbuf = xrealloc(ctx->nbuf, ctx->ncap);
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, ctx->nbuf, n, NULL, NULL);
    return ctx->nbuf;
}

static int wpath_from_utf8_root(WPath *p, const char *utf8) {
    wchar_t *w = to_wide(utf8, CP_UTF8);
    if (!w) w = to_wide(utf8, CP_ACP);
    if (!w) return -1;

    wchar_t full[32768];
    DWORD n = GetFullPathNameW(w, 32768, full, NULL);
    free(w);
    if (n == 0 || n >= 32768) return -1;

    if (full[0] == L'\\' && full[1] == L'\\' && full[2] == L'?' && full[3] == L'\\') {
        wpath_set(p, full, wcslen(full));
    } else if (full[0] == L'\\' && full[1] == L'\\') {
        /* \\server\share -> \\?\UNC\server\share */
        size_t rest = wcslen(full + 2);
        wpath_reserve(p, 8 + rest + 2);
        memcpy(p->s, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
        memcpy(p->s + 8, full + 2, (rest + 1) * sizeof(wchar_t));
        p->len = 8 + rest;
    } else {
        size_t fl = wcslen(full);
        wpath_reserve(p, 4 + fl + 2);
        memcpy(p->s, L"\\\\?\\", 4 * sizeof(wchar_t));
        memcpy(p->s + 4, full, (fl + 1) * sizeof(wchar_t));
        p->len = 4 + fl;
    }
    return 0;
}

static void win_perror(const char *path) {
    DWORD err = GetLastError();
    wchar_t wmsg[256];
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, err, 0, wmsg, 256, NULL);
    while (n && (wmsg[n - 1] == L'\r' || wmsg[n - 1] == L'\n' || wmsg[n - 1] == L' '))
        wmsg[--n] = 0;
    if (n) {
        char *msg = wide_to_utf8(wmsg);
        fprintf(stderr, "dirwhale: cannot access '%s': %s\n", path, msg);
        free(msg);
    } else {
        fprintf(stderr, "dirwhale: cannot access '%s': error %lu\n",
                path, (unsigned long)err);
    }
}

static void scan_dir(Node *dir, ScanCtx *ctx, int depth) {
    const Options *o = ctx->o;
    int keep = keep_children(o, depth);
    WIN32_FIND_DATAW fd;
    size_t mark = ctx->wp.len;
    wpath_push(&ctx->wp, L"*");
    HANDLE h = FindFirstFileW(ctx->wp.s, &fd);
    wpath_pop(&ctx->wp, mark);
    if (h == INVALID_HANDLE_VALUE) { g_errors++; return; }

    do {
        if (g_abort) break;
        const wchar_t *wn = fd.cFileName;
        if (wn[0] == L'.' && (wn[1] == 0 || (wn[1] == L'.' && wn[2] == 0)))
            continue;
        const char *nm = wname_utf8(ctx, wn);
        if (excluded(o, nm)) continue;

        DWORD attr = fd.dwFileAttributes;
        int is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        int is_reparse = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        uint64_t fsize = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;

        if (is_dir && !is_reparse) {
            size_t m = ctx->wp.len;
            wpath_push(&ctx->wp, wn);
            scan_subdir(dir, ctx, depth, nm, keep);
            wpath_pop(&ctx->wp, m);
        } else if (is_dir) {
            /* child junction/symlink-dir: show as empty dir, do not recurse */
            if (keep) node_add_child(dir, node_new(nm, 1));
        } else {
            add_file(dir, o, nm, fsize, keep);
        }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    finalize_children(dir, o);
}

#else /* POSIX */

static void pb_reserve(PathBuf *p, size_t need) {
    if (need <= p->cap) return;
    size_t ncap = p->cap ? p->cap : 256;
    while (ncap < need) ncap *= 2;
    p->s = xrealloc(p->s, ncap);
    p->cap = ncap;
}

static void pb_init(PathBuf *p, const char *root) {
    size_t n = strlen(root);
    while (n > 1 && (root[n - 1] == '/' || root[n - 1] == '\\')) n--;
    pb_reserve(p, n + 256);
    memcpy(p->s, root, n);
    p->s[n] = 0;
    p->len = n;
}

static void pb_push(PathBuf *p, const char *name) {
    size_t nl = strlen(name);
    int sep = p->len > 0 && p->s[p->len - 1] != PATH_SEP;
    pb_reserve(p, p->len + (size_t)sep + nl + 1);
    if (sep) p->s[p->len++] = PATH_SEP;
    memcpy(p->s + p->len, name, nl + 1);
    p->len += nl;
}

static void pb_pop(PathBuf *p, size_t mark) {
    p->len = mark;
    p->s[p->len] = 0;
}

static void scan_dir(Node *dir, ScanCtx *ctx, int depth) {
    const Options *o = ctx->o;
    int keep = keep_children(o, depth);
    DIR *d = opendir(ctx->pb.s);
    if (!d) { g_errors++; return; }

    struct dirent *de;
    while ((de = readdir(d)) && !g_abort) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (excluded(o, nm)) continue;

        int is_dir = 0;
        uint64_t fsize = 0;
#ifdef DT_DIR
        if (de->d_type == DT_DIR) is_dir = 1;
#endif
        if (!is_dir) {
            struct stat st;
            int st_ok;
#if defined(AT_SYMLINK_NOFOLLOW)
            st_ok = fstatat(dirfd(d), nm, &st, AT_SYMLINK_NOFOLLOW) == 0;
#else
            {
                size_t m = ctx->pb.len;
                pb_push(&ctx->pb, nm);
                st_ok = lstat(ctx->pb.s, &st) == 0;
                pb_pop(&ctx->pb, m);
            }
#endif
            if (!st_ok) { g_errors++; continue; }
            if (S_ISDIR(st.st_mode)) is_dir = 1;
            else fsize = (uint64_t)st.st_size;
        }

        if (is_dir) {
            size_t m = ctx->pb.len;
            pb_push(&ctx->pb, nm);
            scan_subdir(dir, ctx, depth, nm, keep);
            pb_pop(&ctx->pb, m);
        } else {
            add_file(dir, o, nm, fsize, keep);
        }
    }
    closedir(d);
    finalize_children(dir, o);
}

#endif /* POSIX */

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

static void print_tree(const Node *n, const Options *o, int depth, int is_last,
                       const char *prefix) {
    char sz[32];
    fmt_size(n->size, sz, sizeof sz);
    if (depth == 0) {
        const char *label = n->path ? n->path : n->name;
        printf("%s [%s, %llu file%s]\n", label, sz,
               (unsigned long long)n->files, n->files == 1 ? "" : "s");
    } else {
        printf("%s%s- %s%s  (%s, %llu file%s)\n",
               prefix, is_last ? "`" : "|",
               n->name, n->is_dir ? "/" : "", sz,
               (unsigned long long)n->files, n->files == 1 ? "" : "s");
    }

    if (!n->is_dir || depth >= o->depth) return;

    size_t shown = n->n_children;
    if (shown > (size_t)o->top) shown = (size_t)o->top;

    size_t hidden = n->n_hidden;
    uint64_t hidden_size = n->hidden_size;
    for (size_t i = shown; i < n->n_children; i++) {
        hidden++;
        hidden_size += n->children[i]->size;
    }
    if (shown == 0 && hidden == 0) return;

    char next_prefix[4096];
    snprintf(next_prefix, sizeof next_prefix, "%s%s", prefix, is_last ? "  " : "| ");

    for (size_t i = 0; i < shown; i++)
        print_tree(n->children[i], o, depth + 1, i == shown - 1 && hidden == 0,
                   next_prefix);
    if (hidden > 0) {
        char hsz[32];
        fmt_size(hidden_size, hsz, sizeof hsz);
        printf("%s`- ... and %llu more entr%s (%s)\n",
               next_prefix, (unsigned long long)hidden,
               hidden == 1 ? "y" : "ies", hsz);
    }
}

/* ------------------------------------------------------------------ */
/* JSON export                                                         */
/* ------------------------------------------------------------------ */

static char *path_join(const char *parent, const char *name) {
    size_t lp = strlen(parent), ln = strlen(name);
    int sep = lp > 0 && parent[lp - 1] != '/' && parent[lp - 1] != '\\';
    char *s = malloc(lp + (size_t)sep + ln + 1);
    if (!s) die_oom();
    memcpy(s, parent, lp);
    if (sep) s[lp] = PATH_SEP;
    memcpy(s + lp + (size_t)sep, name, ln + 1);
    return s;
}

/*
 * Escape s as a JSON string, guaranteeing valid UTF-8 output: on Windows
 * names are already UTF-8; on POSIX filenames are raw bytes, so invalid
 * UTF-8 sequences are replaced with U+FFFD.
 */
static void json_escape(const char *s, FILE *f) {
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', f);
    while (*p) {
        int len = 0;
        unsigned char c = p[0];
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (p[1] && (p[1] & 0xC0) == 0x80) len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            if (p[1] && (p[1] & 0xC0) == 0x80 && p[2] && (p[2] & 0xC0) == 0x80) {
                unsigned cp = ((c & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
                if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) len = 3;
            }
        } else if (c >= 0xF0 && c <= 0xF4) {
            if (p[1] && (p[1] & 0xC0) == 0x80 && p[2] && (p[2] & 0xC0) == 0x80 &&
                p[3] && (p[3] & 0xC0) == 0x80) {
                unsigned cp = ((c & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
                              ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
                if (cp >= 0x10000 && cp <= 0x10FFFF) len = 4;
            }
        }
        if (len == 0) {
            fputs("\\ufffd", f);
            p++;
            continue;
        }
        for (int i = 0; i < len; i++, p++) {
            unsigned char b = *p;
            if (b == '"' || b == '\\') { fputc('\\', f); fputc(b, f); }
            else if (b < 0x20) fprintf(f, "\\u%04x", b);
            else fputc(b, f);
        }
    }
    fputc('"', f);
}

static void json_tree(const Node *n, FILE *f, const char *path) {
    fprintf(f, "{\"name\":");
    json_escape(n->name, f);
    fprintf(f, ",\"path\":");
    json_escape(path, f);
    fprintf(f, ",\"is_dir\":%s,\"size\":%llu,\"files\":%llu",
            n->is_dir ? "true" : "false",
            (unsigned long long)n->size, (unsigned long long)n->files);
    if (n->n_children) {
        fprintf(f, ",\"children\":[");
        for (size_t i = 0; i < n->n_children; i++) {
            if (i) fputc(',', f);
            char *cp = path_join(path, n->children[i]->name);
            json_tree(n->children[i], f, cp);
            free(cp);
        }
        fputc(']', f);
    }
    fputc('}', f);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(const char *prog) {
    fprintf(stderr,
        "dirwhale — zero-dependency disk usage analyzer\n\n"
        "usage: %s [options] [path]\n"
        "  -d, --depth N      show children down to depth N (default 1)\n"
        "  -n, --top N        biggest N entries per level (default 15)\n"
        "  -e, --exclude PAT  skip wildcard pattern, may repeat\n"
        "  -t, --types        show file-type breakdown\n"
        "  -j, --json FILE    also write results as JSON\n"
        "  -q, --quiet        no scan-progress on stderr\n"
        "  -V, --version      print version and exit\n"
        "  -h, --help         show this help\n", prog);
}

static int parse_int(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) return 0;
    *out = (int)v;
    return 1;
}

#ifdef _WIN32
static char **win_utf8_argv(int *argc, char **fallback) {
    int n = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &n);
    if (wargv) {
        char **av = xrealloc(NULL, (size_t)(n + 1) * sizeof(char *));
        for (int i = 0; i < n; i++) av[i] = wide_to_utf8(wargv[i]);
        av[n] = NULL;
        LocalFree(wargv);
        *argc = n;
        return av;
    }
    /* ACP fallback so the rest of the program can assume UTF-8 */
    char **av = xrealloc(NULL, (size_t)(*argc + 1) * sizeof(char *));
    for (int i = 0; i < *argc; i++) {
        wchar_t *w = to_wide(fallback[i], CP_ACP);
        av[i] = w ? wide_to_utf8(w) : xstrdup(fallback[i]);
        free(w);
    }
    av[*argc] = NULL;
    return av;
}
#endif

int main(int argc, char **argv) {
    Options o = {0};
    o.root  = ".";
    o.depth = 1;
    o.top   = 15;
    ExtVec  exts = {0};
#ifdef _WIN32
    char **utf8argv = NULL;
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    utf8argv = win_utf8_argv(&argc, argv);
    if (utf8argv) argv = utf8argv;
#endif

    /* needs the next argv slot to exist; guards atoi(NULL)/NULL deref */
    #define NEED_ARG() do { \
        if (i + 1 >= argc) { \
            fprintf(stderr, "dirwhale: option %s needs an argument\n", a); \
            return 1; \
        } \
    } while (0)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(argv[0]); return 0; }
        else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("dirwhale %s\n", DIRWHALE_VERSION);
            return 0;
        }
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) o.quiet = 1;
        else if (strcmp(a, "-d") == 0 || strcmp(a, "--depth") == 0) {
            NEED_ARG(); i++;
            if (!parse_int(argv[i], &o.depth)) {
                fprintf(stderr, "dirwhale: %s expects a number, got '%s'\n", a, argv[i]);
                return 1;
            }
        }
        else if (strcmp(a, "-n") == 0 || strcmp(a, "--top") == 0) {
            NEED_ARG(); i++;
            if (!parse_int(argv[i], &o.top)) {
                fprintf(stderr, "dirwhale: %s expects a number, got '%s'\n", a, argv[i]);
                return 1;
            }
        }
        else if (strcmp(a, "-e") == 0 || strcmp(a, "--exclude") == 0) { NEED_ARG(); strvec_push(&o.excludes, argv[++i]); }
        else if (strcmp(a, "-t") == 0 || strcmp(a, "--types") == 0) o.types = 1;
        else if (strcmp(a, "-j") == 0 || strcmp(a, "--json") == 0)  { NEED_ARG(); o.json_path = argv[++i]; }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "dirwhale: unknown option %s\n", a); usage(argv[0]); return 1; }
        else if (o.root_given) { fprintf(stderr, "dirwhale: multiple paths given ('%s' and '%s')\n", o.root, a); return 1; }
        else { o.root = a; o.root_given = 1; }
    }
    #undef NEED_ARG
    if (o.depth < 0) o.depth = 0;
    if (o.depth > 64) o.depth = 64;   /* keeps prefix buffers and the stack sane */
    if (o.top < 1)   o.top = 1;
    if (o.types) o.exts = &exts;      /* only collect stats when they'll be shown */

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sigint_handler);
#endif
    g_show_progress = !o.quiet && STDERR_ISATTY();

    ScanCtx ctx = {0};
    ctx.o = &o;
    Node *root = NULL;
    int root_is_dir = 0;
    uint64_t root_fsize = 0;

#ifdef _WIN32
    if (wpath_from_utf8_root(&ctx.wp, o.root) != 0) {
        win_perror(o.root);
        return 1;
    }
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (!GetFileAttributesExW(ctx.wp.s, GetFileExInfoStandard, &ad)) {
        win_perror(o.root);
        free(ctx.wp.s);
        return 1;
    }
    root_is_dir = (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    root_fsize  = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
#else
    pb_init(&ctx.pb, o.root);
    struct stat root_st;
    if (lstat(ctx.pb.s, &root_st) != 0) {
        fprintf(stderr, "dirwhale: cannot access '%s': %s\n", o.root, strerror(errno));
        free(ctx.pb.s);
        return 1;
    }
    root_is_dir = S_ISDIR(root_st.st_mode);
    root_fsize  = (uint64_t)root_st.st_size;
#endif

    root = node_new(o.root, root_is_dir);
    root->path = xstrdup(o.root);
    if (root_is_dir) {
        if (g_show_progress) {
            fprintf(stderr, "scanning %s ...\n", o.root);
            fflush(stderr);
        }
        scan_dir(root, &ctx, 0);
    } else {
        root->size  = root_fsize;
        root->files = 1;
        g_files_scanned = 1;
    }
    if (g_show_progress)
        fprintf(stderr, "\r  scanned %llu files.      \n",
                (unsigned long long)g_files_scanned);

    print_tree(root, &o, 0, 1, "");

    if (g_errors)
        printf("\nwarning: %llu entr%s could not be read (permissions, broken "
               "links, ...) and were skipped\n",
               (unsigned long long)g_errors, g_errors == 1 ? "y" : "ies");

    if (o.types) {
        qsort(exts.items, exts.len, sizeof(ExtStat), ext_cmp);
        printf("\nfile types:\n");
        size_t shown = exts.len < (size_t)o.top ? exts.len : (size_t)o.top;
        for (size_t i = 0; i < shown; i++) {
            char sz[32];
            fmt_size(exts.items[i].size, sz, sizeof sz);
            printf("  %-12s %s  (%llu file%s)\n",
                   exts.items[i].ext[0] ? exts.items[i].ext : "(none)",
                   sz, (unsigned long long)exts.items[i].files,
                   exts.items[i].files == 1 ? "" : "s");
        }
    }

    if (o.json_path) {
        FILE *f = fopen(o.json_path, "wb");
        if (!f) {
            perror("dirwhale: cannot open json output");
        } else {
            json_tree(root, f, o.root);
            fputc('\n', f);
            if (ferror(f) || fclose(f) != 0)
                perror("dirwhale: error writing json output");
            else
                printf("\nJSON written to %s\n", o.json_path);
        }
    }

    node_free(root);
    for (size_t i = 0; i < exts.len; i++) free(exts.items[i].ext);
    free(exts.items);
    for (size_t i = 0; i < o.excludes.len; i++) free(o.excludes.items[i]);
    free(o.excludes.items);
#ifdef _WIN32
    free(ctx.wp.s);
    free(ctx.nbuf);
    if (utf8argv) {
        for (int i = 0; i < argc; i++) free(utf8argv[i]);
        free(utf8argv);
    }
#else
    free(ctx.pb.s);
#endif
    return 0;
}
