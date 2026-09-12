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
 *   -h, --help         show this help
 *
 * License: MIT
 */

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
#  define PATH_SEP '\\'
#else
#  include <unistd.h>
#  define PATH_SEP '/'
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>

#ifdef __MINGW32__
/*
 * MinGW's CRT expands wildcards in argv before main() runs, which would
 * turn `dirwhale -e *` into a pile of positional arguments. We do our own
 * (per-entry, basename-only) matching, so switch CRT globbing off.
 */
int _dowildcard = 0;
#endif

/* ------------------------------------------------------------------ */
/* Data structures                                                     */
/* ------------------------------------------------------------------ */

typedef struct Node {
    char        *name;      /* owned; basename without parent path      */
    char        *path;      /* owned; full path                         */
    uint64_t     size;      /* recursive size in bytes                  */
    uint64_t     files;     /* recursive file count                     */
    int          is_dir;
    struct Node **children; /* owned array of owned pointers            */
    size_t       n_children;
} Node;

typedef struct ExtStat {
    char    *ext;           /* owned; "" for extensionless files        */
    uint64_t size;
    uint64_t files;
} ExtStat;

typedef struct StrVec {
    char  **items;
    size_t  len, cap;
} StrVec;

typedef struct Options {
    const char  *root;
    int          depth;     /* how deep to print (root children = 1)    */
    int          top;       /* entries per level to display             */
    int          types;     /* print extension breakdown                */
    const char  *json_path;
    StrVec       excludes;
    struct ExtVec *exts;    /* extension stats accumulator               */
    int          root_given;
} Options;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "dirwhale: out of memory\n"); exit(1); }
    return q;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) { fprintf(stderr, "dirwhale: out of memory\n"); exit(1); }
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

static char *to_utf8(const char *s);   /* defined below; converts output on Windows */

/* Human-readable size: <1KiB in bytes, else KiB/MiB/GiB/TiB */
static void fmt_size(uint64_t bytes, char *buf, size_t bufsz) {
    if (bytes < 1024) { snprintf(buf, bufsz, "%llu B", (unsigned long long)bytes); return; }
    double v; const char *u;
    if (bytes < (1ull<<20))      { v = bytes / 1024.0;           u = "KiB"; }
    else if (bytes < (1ull<<30)) { v = bytes / (1024.0*1024);    u = "MiB"; }
    else if (bytes < (1ull<<40)) { v = bytes / (1024.0*1024*1024); u = "GiB"; }
    else                         { v = bytes / (1024.0*1024*1024*1024); u = "TiB"; }
    if (v >= 100)      snprintf(buf, bufsz, "%.0f %s", v, u);
    else if (v >= 10)  snprintf(buf, bufsz, "%.1f %s", v, u);
    else               snprintf(buf, bufsz, "%.2f %s", v, u);
}

/* ------------------------------------------------------------------ */
/* File-type statistics                                                */
/* ------------------------------------------------------------------ */

typedef struct ExtVec {
    ExtStat *items;
    size_t   len, cap;
} ExtVec;

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
static uint64_t g_errors;      /* entries we could not stat or open    */
static uint64_t g_progress_shown; /* last progress count printed       */
static volatile sig_atomic_t g_abort;
static int g_show_progress;

#ifdef _WIN32
static BOOL WINAPI ctrl_handler(DWORD type) {
    (void)type;
    if (g_abort) {
        /* second Ctrl+C: force exit */
        ExitProcess(130);
    }
    g_abort = 1;
    return TRUE; /* let the scan unwind and print partial results */
}
#else
#  include <signal.h>
static void sigint_handler(int sig) {
    (void)sig;
    if (g_abort) _exit(130);
    g_abort = 1;
}
#endif

/*
 * Portable stat wrapper.
 * - Windows: _stat64 gives a 64-bit st_size (the plain struct stat in
 *   MinGW-w64 defaults to a 32-bit off_t and overflows past 2/4 GiB).
 * - POSIX:   lstat, so symlinks are not followed (avoids recursive
 *   scans of symlinked directory trees).
 */
#ifdef _WIN32
typedef struct __stat64 Stat;
static int do_stat(const char *path, Stat *st) { return _stat64(path, st); }
static int entry_is_reparse(const char *path) {
    /* dir symlinks/junctions are reparse points; do not recurse into them */
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}
#else
typedef struct stat Stat;
static int do_stat(const char *path, Stat *st) { return lstat(path, st); }
static int entry_is_reparse(const char *path) { (void)path; return 0; }
#endif
#define STAT_ISDIR(st) S_ISDIR((st).st_mode)

static Node *node_new(const char *path, const char *name, int is_dir) {
    Node *n = calloc(1, sizeof(Node));
    if (!n) { fprintf(stderr, "dirwhale: out of memory\n"); exit(1); }
    n->path   = xstrdup(path);
    n->name   = xstrdup(name);
    n->is_dir = is_dir;
    return n;
}

static void node_add_child(Node *parent, Node *child) {
    parent->children = xrealloc(parent->children,
                                (parent->n_children + 1) * sizeof(Node *));
    parent->children[parent->n_children++] = child;
}

static void node_free(Node *n) {
    for (size_t i = 0; i < n->n_children; i++) node_free(n->children[i]);
    free(n->children);
    free(n->name);
    free(n->path);
    free(n);
}

/* Build the full tree under `dir`; returns recursive byte size. */
static uint64_t scan_dir(Node *dir, const Options *o) {
    DIR *d = opendir(dir->path);
    if (!d) { g_errors++; return 0; }
    struct dirent *de;
    while ((de = readdir(d)) && !g_abort) {
        const char *nm = de->d_name;
        if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
        if (excluded(o, nm)) continue;   /* before stat: no syscall wasted,
                                            no misleading error count */

        size_t plen = strlen(dir->path) + 1 + strlen(nm) + 1;
        char *full = malloc(plen);
        if (!full) { fprintf(stderr, "dirwhale: out of memory\n"); exit(1); }
        snprintf(full, plen, "%s%c%s", dir->path, PATH_SEP, nm);

        Stat st;
        int is_dir = 0;
        uint64_t fsize = 0;
        if (do_stat(full, &st) != 0) {
            /* broken link, permission denied, path too long, ... */
            g_errors++;
            free(full);
            continue;
        }
        /* never recurse into reparse points (dir symlinks/junctions on
           Windows) or POSIX symlinks: lstat already keeps POSIX ones out
           of S_ISDIR, the reparse check catches the Windows ones */
        if (STAT_ISDIR(st) && !entry_is_reparse(full)) is_dir = 1;
        else fsize = (uint64_t)st.st_size;

        Node *child = node_new(full, nm, is_dir);
        if (is_dir) {
            child->size = scan_dir(child, o);
        } else {
            child->size = fsize;
            child->files = 1;
            g_files_scanned++;
            if (o->exts) extvec_add(o->exts, nm, fsize);
        }
        free(full);
        node_add_child(dir, child);
        dir->size += child->size;
        dir->files += child->files;

        /* monotonic progress ticks; g_files_scanned itself can be read
           slightly out of order between threads in theory, so only print
           when the count has moved on since the last tick */
        if (g_show_progress && g_files_scanned - g_progress_shown >= 4096) {
            g_progress_shown = g_files_scanned;
            fprintf(stderr, "\r  scanned %llu files...",
                    (unsigned long long)g_files_scanned);
        }
    }
    closedir(d);
    return dir->size;
}

/* ------------------------------------------------------------------ */
/* Printing                                                            */
/* ------------------------------------------------------------------ */

static int node_cmp_desc(const void *a, const void *b) {
    const Node *const *x = a, *const *y = b;
    if ((*x)->size != (*y)->size) return (*x)->size > (*y)->size ? -1 : 1;
    return strcmp((*x)->name, (*y)->name);
}

static void print_tree(const Node *n, const Options *o, int depth, int is_last,
                       const char *prefix) {
    char sz[32];
    fmt_size(n->size, sz, sizeof sz);
    if (depth == 0) {
        char *up = to_utf8(n->path);
        printf("%s [%s, %llu file%s]\n", up, sz,
               (unsigned long long)n->files, n->files == 1 ? "" : "s");
        free(up);
    } else {
        char *un = to_utf8(n->name);
        printf("%s%s- %s%s  (%s, %llu file%s)\n",
               prefix, is_last ? "`" : "|",
               un, n->is_dir ? "/" : "", sz,
               (unsigned long long)n->files, n->files == 1 ? "" : "s");
        free(un);
    }

    if (!n->is_dir || depth >= o->depth || n->n_children == 0) return;

    Node **kids = xrealloc(NULL, n->n_children * sizeof(Node *));
    memcpy(kids, n->children, n->n_children * sizeof(Node *));
    qsort(kids, n->n_children, sizeof(Node *), node_cmp_desc);

    size_t shown = n->n_children < (size_t)o->top ? n->n_children : (size_t)o->top;
    size_t hidden = n->n_children - shown;
    uint64_t hidden_size = 0;
    for (size_t i = shown; i < n->n_children; i++) hidden_size += kids[i]->size;

    char next_prefix[4096];
    snprintf(next_prefix, sizeof next_prefix, "%s%s", prefix, is_last ? "  " : "| ");

    char hsz[32];
    for (size_t i = 0; i < shown; i++)
        print_tree(kids[i], o, depth + 1, i == shown - 1 && hidden == 0,
                   next_prefix);
    if (hidden > 0) {
        fmt_size(hidden_size, hsz, sizeof hsz);
        printf("%s`- ... and %llu more entr%s (%s)\n",
               next_prefix, (unsigned long long)hidden,
               hidden == 1 ? "y" : "ies", hsz);
    }
    free(kids);
}

/* ------------------------------------------------------------------ */
/* Output encoding                                                     */
/* ------------------------------------------------------------------ */

/*
 * Windows gives us filenames in the ANSI code page (e.g. GBK on zh-CN
 * systems); the console (CP 65001) and JSON both expect UTF-8. Convert
 * at output time only — the raw bytes are still needed for opendir/stat.
 * POSIX filenames are already byte strings treated as-is.
 */
#ifdef _WIN32
static char *to_utf8(const char *s) {
    int wlen = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (wlen <= 0) return xstrdup(s);
    wchar_t *w = malloc((size_t)wlen * sizeof(wchar_t));
    if (!w) { fprintf(stderr, "dirwhale: out of memory\n"); exit(1); }
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (ulen <= 0) { free(w); return xstrdup(s); }
    char *u = malloc((size_t)ulen);
    if (!u) { fprintf(stderr, "dirwhale: out of memory\n"); exit(1); }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, ulen, NULL, NULL);
    free(w);
    return u;
}
#else
static char *to_utf8(const char *s) { return xstrdup(s); }
#endif

/* ------------------------------------------------------------------ */
/* JSON export                                                         */
/* ------------------------------------------------------------------ */

/*
 * Escape s as a JSON string, guaranteeing valid UTF-8 output: on Windows
 * to_utf8() already re-encodes from the ANSI code page; on POSIX filenames
 * are raw bytes, so invalid UTF-8 sequences are replaced with U+FFFD
 * instead of producing an unparseable JSON file.
 */
static void json_escape(const char *s, FILE *f) {
    char *u = to_utf8(s);
    const unsigned char *p = (const unsigned char *)u;
    fputc('"', f);
    while (*p) {
        /* try to decode one UTF-8 sequence; len = 0 means invalid */
        int len = 0;
        unsigned char c = p[0];
        if (c < 0x80) {
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) {
            if (p[1] && (p[1] & 0xC0) == 0x80) len = 2;   /* p[1] may be the NUL */
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
    free(u);
}

static void json_tree(const Node *n, FILE *f) {
    fprintf(f, "{\"name\":");
    json_escape(n->name, f);
    fprintf(f, ",\"path\":");
    json_escape(n->path, f);
    fprintf(f, ",\"is_dir\":%s,\"size\":%llu,\"files\":%llu",
            n->is_dir ? "true" : "false",
            (unsigned long long)n->size, (unsigned long long)n->files);
    if (n->n_children) {
        fprintf(f, ",\"children\":[");
        for (size_t i = 0; i < n->n_children; i++) {
            if (i) fputc(',', f);
            json_tree(n->children[i], f);
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
        "  -h, --help         show this help\n", prog);
}

static int parse_int(const char *s, int *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) return 0;
    *out = (int)v;
    return 1;
}

int main(int argc, char **argv) {
    Options o = {0};
    o.root  = ".";
    o.depth = 1;
    o.top   = 15;
    ExtVec  exts = {0};

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
    SetConsoleOutputCP(65001); /* UTF-8 output */
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sigint_handler);
#endif
    g_show_progress = 1;

    Stat root_st;
    if (do_stat(o.root, &root_st) != 0) {
        fprintf(stderr, "dirwhale: cannot access '%s': %s\n", o.root, strerror(errno));
        return 1;
    }

    /* resolve the root to an absolute-ish path for nicer display */
    Node *root = node_new(o.root, o.root, STAT_ISDIR(root_st));
    if (STAT_ISDIR(root_st)) {
        fprintf(stderr, "scanning %s ...\n", o.root);
        scan_dir(root, &o);
    } else {
        /* a single file given as the argument: report just that file */
        root->size = (uint64_t)root_st.st_size;
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
        size_t shown = exts.len < 10 ? exts.len : 10;
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
            json_tree(root, f);
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
    return 0;
}
