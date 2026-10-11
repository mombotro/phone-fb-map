// pdf.h - plain text out of a PDF file, with no libraries. Needs epub.h first (for the inflate code).
//   pdf_text(path, &len)   returns a malloc'ed, NUL-ended ASCII text, or NULL. pdf_error says why.
// How it works: it finds every "N G obj" in the file, opens object streams, walks the page tree in reading order, inflates each
// page's content stream, and collects the text-showing operators (Tj, TJ, ', "). Fonts are decoded with the /ToUnicode map when
// there is one, and with WinAnsi or the /Differences list otherwise. Pages that are only pictures (scans) give no text.
// Not supported: encrypted files, and fonts that have no /ToUnicode map and use two-byte codes.
#ifndef PDF_H
#define PDF_H
#include "epub.h"
#include <math.h>

static char pdf_error[96];
static const void *pdf_memmem(const void *h, size_t hl, const void *n, size_t nl) {      // memmem is not in every C library
    const unsigned char *p = h;
    if (nl == 0 || hl < nl) return NULL;
    for (size_t i = 0; i + nl <= hl; i++) if (p[i] == ((const unsigned char *)n)[0] && !memcmp(p + i, n, nl)) return p + i;
    return NULL;
}

typedef struct { int num; size_t off; int stm, idx; } PdfObj;     // off: file offset of the body, or stm >= 0: inside object stream stm, entry idx
typedef struct { int num; unsigned char *data; size_t len; int first, n; int *onum, *ooff; } PdfStm;  // a decoded object stream
typedef struct { unsigned short *from, *to; int n, cap; int two; int winansi; unsigned short diff[256]; } PdfFont;
typedef struct { int num; PdfFont *f; } PdfFontEnt;
typedef struct { int num; const char *res, *resend; } PdfPage;
typedef struct {
    unsigned char *d; size_t n;
    PdfObj *o; int no, ocap;
    PdfStm *stms; int nstm;
    PdfFontEnt *fonts; int nfonts;
    char *out; size_t olen, ocap2;
    int pages;
    PdfPage *pg; int npg, pgcap; int collect;                    // the pages in reading order
    int img_mode, img_filter;                                  // img_mode: let an image filter (JPEG and so on) pass through; img_filter says which
} Pdf;

// ---------------------------------------------------------------- small text helpers

static int pdf_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static int pdf_delim(int c) { return strchr("()<>[]{}/%", c) != NULL && c != 0; }
static const char *pdf_skip(const char *p, const char *end) {
    while (p < end) {
        if (pdf_ws((unsigned char)*p)) p++;
        else if (*p == '%') { while (p < end && *p != '\n' && *p != '\r') p++; }
        else break;
    }
    return p;
}
static int pdf_cmp_obj(const void *a, const void *b) { const PdfObj *x = a, *y = b; return x->num != y->num ? (x->num < y->num ? -1 : 1) : (x->off < y->off ? -1 : x->off > y->off ? 1 : 0); }
static void pdf_add_obj(Pdf *p, int num, size_t off, int stm, int idx) {
    if (p->no == p->ocap) { p->ocap = p->ocap ? p->ocap * 2 : 1024; p->o = realloc(p->o, (size_t)p->ocap * sizeof *p->o); if (!p->o) { p->no = p->ocap = 0; return; } }
    p->o[p->no].num = num; p->o[p->no].off = off; p->o[p->no].stm = stm; p->o[p->no].idx = idx; p->no++;
}
// A reference "N G R" at p. Returns the object number or -1.
static int pdf_ref_at(const char *p, const char *end) {
    p = pdf_skip(p, end);
    if (p >= end || !isdigit((unsigned char)*p)) return -1;
    char *q; long a = strtol(p, &q, 10); q = (char *)pdf_skip(q, end);
    if (q >= end || !isdigit((unsigned char)*q)) return -1;
    long b = strtol(q, &q, 10); (void)b; q = (char *)pdf_skip(q, end);
    if (q < end && *q == 'R' && (q + 1 >= end || pdf_ws((unsigned char)q[1]) || pdf_delim((unsigned char)q[1]))) return (int)a;
    return -1;
}

// ---------------------------------------------------------------- objects

static PdfObj *pdf_find(Pdf *p, int num) {
    int lo = 0, hi = p->no - 1, at = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (p->o[m].num < num) lo = m + 1; else { if (p->o[m].num == num) at = m; hi = m - 1; } }
    if (at < 0) return NULL;
    while (at + 1 < p->no && p->o[at + 1].num == num) at++;         // the last one in the file wins (later updates)
    return &p->o[at];
}
static const char *pdf_dict_end(const char *p, const char *end);

// zlib data (2 header bytes, then deflate): the size is not stored, so grow the buffer until it fits. A damaged tail keeps what was decoded.
static unsigned char *pdf_inflate(const unsigned char *in, size_t inlen, size_t *len) {
    if (inlen < 3) return NULL;
    size_t cap = inlen * 4 + 4096;
    for (int tries = 0; tries < 6 && cap <= (200u << 20); tries++, cap *= 4) {
        unsigned char *out = malloc(cap + 1); if (!out) return NULL;
        size_t got = 0; int r = eb_inflate_ex(in + 2, inlen - 2, out, cap, &got);
        if (r != 0 && got + 300 >= cap) { free(out); continue; }          // ran out of room (a match may not fit): try a bigger buffer
        if (got == 0) { free(out); return NULL; }
        out[got] = 0; *len = got; return out;
    }
    return NULL;
}
static unsigned char *pdf_a85(const unsigned char *in, size_t n, size_t *len) {
    unsigned char *out = malloc(n + 8); if (!out) return NULL;
    size_t o = 0; unsigned long v = 0; int k = 0;
    for (size_t i = 0; i < n; i++) {
        int c = in[i];
        if (pdf_ws(c)) continue;
        if (c == '~') break;
        if (c == 'z' && k == 0) { out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0; continue; }
        if (c < '!' || c > 'u') continue;
        v = v * 85 + (unsigned long)(c - '!');
        if (++k == 5) { out[o++] = (unsigned char)(v >> 24); out[o++] = (unsigned char)(v >> 16); out[o++] = (unsigned char)(v >> 8); out[o++] = (unsigned char)v; v = 0; k = 0; }
    }
    if (k > 1) {
        for (int j = k; j < 5; j++) v = v * 85 + 84;
        for (int j = 0; j < k - 1; j++) out[o++] = (unsigned char)(v >> (24 - 8 * j));
    }
    out[o] = 0; *len = o; return out;
}
static unsigned char *pdf_ahx(const unsigned char *in, size_t n, size_t *len) {
    unsigned char *out = malloc(n / 2 + 2); if (!out) return NULL;
    size_t o = 0; int hi = -1;
    for (size_t i = 0; i < n && in[i] != '>'; i++) {
        if (!isxdigit(in[i])) continue;
        int v = isdigit(in[i]) ? in[i] - '0' : tolower(in[i]) - 'a' + 10;
        if (hi < 0) hi = v; else { out[o++] = (unsigned char)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) out[o++] = (unsigned char)(hi * 16);
    out[o] = 0; *len = o; return out;
}

// Inflate a stream body if its dictionary says FlateDecode. Returns a malloc'ed buffer (NUL-ended) or NULL.
static unsigned char *pdf_stream_data(Pdf *pd, const char *dict, const char *end, size_t *len);

// The text of an object starting at its body. For an object in an object stream this decodes the stream (and keeps it).
static const char *pdf_body(Pdf *p, int num, const char **end) {
    PdfObj *o = pdf_find(p, num); if (!o) return NULL;
    if (o->stm < 0) { *end = (const char *)p->d + p->n; return (const char *)p->d + o->off; }
    for (int i = 0; i < p->nstm; i++) if (p->stms[i].num == o->stm) {
        PdfStm *s = &p->stms[i]; if (o->idx >= s->n) return NULL;
        size_t start = (size_t)s->first + (size_t)s->ooff[o->idx];
        if (start >= s->len) return NULL;
        *end = (const char *)s->data + s->len; return (const char *)s->data + start;
    }
    return NULL;
}

// Find /Key at the top level of the dictionary that begins at p ("<<"). Returns a pointer to its value or NULL.
static const char *pdf_key(const char *p, const char *end, const char *key) {
    p = pdf_skip(p, end);
    if (p + 1 >= end || p[0] != '<' || p[1] != '<') return NULL;
    p += 2; int depth = 1; size_t kl = strlen(key);
    while (p < end && depth > 0) {
        p = pdf_skip(p, end); if (p >= end) break;
        if (p[0] == '<' && p + 1 < end && p[1] == '<') { depth++; p += 2; }
        else if (p[0] == '>' && p + 1 < end && p[1] == '>') { depth--; p += 2; }
        else if (p[0] == '(') { int d = 1; p++; while (p < end && d > 0) { if (*p == '\\') p++; else if (*p == '(') d++; else if (*p == ')') d--; p++; } }
        else if (p[0] == '/') {
            if (depth == 1 && !strncmp(p + 1, key, kl) && (p + 1 + kl >= end || pdf_ws((unsigned char)p[1 + kl]) || pdf_delim((unsigned char)p[1 + kl]))) return pdf_skip(p + 1 + kl, end);
            p++; while (p < end && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++;
        } else p++;
    }
    return NULL;
}
// Where the dictionary beginning at p ends (after the closing ">>"), or NULL.
static const char *pdf_dict_end(const char *p, const char *end) {
    p = pdf_skip(p, end);
    if (p + 1 >= end || p[0] != '<' || p[1] != '<') return NULL;
    p += 2; int depth = 1;
    while (p < end && depth > 0) {
        if (p[0] == '<' && p + 1 < end && p[1] == '<') { depth++; p += 2; }
        else if (p[0] == '>' && p + 1 < end && p[1] == '>') { depth--; p += 2; }
        else if (p[0] == '(') { int d = 1; p++; while (p < end && d > 0) { if (*p == '\\') p++; else if (*p == '(') d++; else if (*p == ')') d--; p++; } }
        else p++;
    }
    return depth == 0 ? p : NULL;
}
static int pdf_int(const char *v, const char *end, int dflt) {   // a direct integer, or a reference to one
    if (!v) return dflt;
    return (v < end && (isdigit((unsigned char)*v) || *v == '-')) ? (int)strtol(v, NULL, 10) : dflt;
}
static const char *pdf_deref(Pdf *p, const char *v, const char **end) {   // if v is a reference, the object's body; else v itself
    if (!v) return NULL;
    int r = pdf_ref_at(v, *end);
    if (r < 0) return v;
    const char *b = pdf_body(p, r, end);
    return b ? pdf_skip(b, *end) : NULL;
}

static unsigned char *pdf_stream_data(Pdf *pd, const char *dict, const char *end, size_t *len) {
    const char *de = pdf_dict_end(dict, end); if (!de) return NULL;
    const char *s = pdf_skip(de, end);
    if (end - s < 6 || strncmp(s, "stream", 6)) return NULL;
    s += 6; if (s < end && *s == '\r') s++; if (s < end && *s == '\n') s++;
    // the length: /Length N, or an indirect reference to N
    long L = -1; const char *lv = pdf_key(dict, end, "Length");
    if (lv) {
        int r = pdf_ref_at(lv, end);
        if (r >= 0) { const char *e2; const char *b = pdf_body(pd, r, &e2); if (b) { b = pdf_skip(b, e2); if (b < e2 && isdigit((unsigned char)*b)) L = strtol(b, NULL, 10); } }
        else if (isdigit((unsigned char)*lv)) L = strtol(lv, NULL, 10);
    }
    const char *dend = (const char *)pd->d + pd->n;
    if (L < 0 || s + L > dend || (s + L + 9 <= dend && !strstr(s + (L > 20 ? L - 20 : 0), "endstream"))) {   // a wrong or missing length: look for the end
        const char *e = NULL; for (const char *q = s; q + 9 <= dend; q++) if (*q == 'e' && !strncmp(q, "endstream", 9)) { e = q; break; }
        if (!e) return NULL; L = e - s; if (L > 0 && s[L - 1] == '\n') L--; if (L > 0 && s[L - 1] == '\r') L--;
    }
    // the filters, in the order they must be undone
    int filt[6], nf = 0;                                            // 1 Flate, 2 ASCII85, 3 ASCIIHex
    pd->img_filter = 0;
    const char *flt = pdf_key(dict, end, "Filter");
    if (flt) {
        const char *fe = end; const char *f = pdf_deref(pd, flt, &fe); if (!f) f = flt;
        const char *stop = f;
        if (*f == '[') { while (stop < fe && *stop != ']') stop++; }
        else { stop = f + 1; while (stop < fe && !pdf_ws((unsigned char)*stop) && !pdf_delim((unsigned char)*stop)) stop++; }
        for (const char *q = f; q < stop && nf < 6; q++) if (*q == '/') {
            if (!strncmp(q, "/FlateDecode", 12) || !strncmp(q, "/Fl", 3)) filt[nf++] = 1;
            else if (!strncmp(q, "/ASCII85Decode", 14) || !strncmp(q, "/A85", 4)) filt[nf++] = 2;
            else if (!strncmp(q, "/ASCIIHexDecode", 15) || !strncmp(q, "/AHx", 4)) filt[nf++] = 3;
            else if (pd->img_mode && (!strncmp(q, "/DCTDecode", 10) || !strncmp(q, "/DCT", 4))) { pd->img_filter = 1; break; }
            else if (pd->img_mode && !strncmp(q, "/JPXDecode", 10)) { pd->img_filter = 2; break; }
            else if (pd->img_mode && (!strncmp(q, "/CCITTFaxDecode", 15) || !strncmp(q, "/CCF", 4))) { pd->img_filter = 3; break; }
            else if (pd->img_mode && !strncmp(q, "/JBIG2Decode", 12)) { pd->img_filter = 4; break; }
            else return NULL;                                       // LZW and so on
        }
    }
    unsigned char *cur = malloc((size_t)L + 1); if (!cur) return NULL;
    memcpy(cur, s, (size_t)L); cur[L] = 0; size_t clen = (size_t)L;
    for (int i = 0; i < nf; i++) {
        unsigned char *nx = NULL; size_t nl = 0;
        if (filt[i] == 1) nx = pdf_inflate(cur, clen, &nl);
        else if (filt[i] == 2) nx = pdf_a85(cur, clen, &nl);
        else nx = pdf_ahx(cur, clen, &nl);
        free(cur); if (!nx) return NULL;
        cur = nx; clen = nl;
    }
    *len = clen; return cur;
}

// ---------------------------------------------------------------- index the objects

static void pdf_index(Pdf *p) {
    const unsigned char *d = p->d; size_t n = p->n;
    for (size_t i = 5; i + 3 <= n; i++) {                          // "N G obj"
        if (d[i] != 'o' || d[i + 1] != 'b' || d[i + 2] != 'j') continue;
        if (i + 3 < n && isalnum(d[i + 3])) continue;               // "object", or endobj has 'd' before
        if (d[i - 1] != ' ' && d[i - 1] != '\n' && d[i - 1] != '\r' && d[i - 1] != '\t') continue;
        size_t j = i - 1; while (j > 0 && (d[j] == ' ')) j--;
        size_t gend = j; while (j > 0 && isdigit(d[j])) j--; if (j == gend || (d[j] != ' ' && d[j] != '\n' && d[j] != '\r')) continue;
        while (j > 0 && (d[j] == ' ' || d[j] == '\n' || d[j] == '\r')) j--;
        size_t nend = j; while (j > 0 && isdigit(d[j])) j--; if (!isdigit(d[j])) j++;
        if (j > nend) continue;
        char tmp[16]; size_t l = nend - j + 1; if (l > 9) continue; memcpy(tmp, d + j, l); tmp[l] = 0;
        pdf_add_obj(p, atoi(tmp), i + 3, -1, 0);
    }
    qsort(p->o, (size_t)p->no, sizeof *p->o, pdf_cmp_obj);
    // object streams
    int base = p->no;
    for (int k = 0; k < base; k++) {
        const char *end = (const char *)d + n, *b = (const char *)d + p->o[k].off;
        b = pdf_skip(b, end);
        const char *ty = pdf_key(b, end, "Type");
        if (!ty || strncmp(ty, "/ObjStm", 7)) continue;
        int N = pdf_int(pdf_key(b, end, "N"), end, 0), first = pdf_int(pdf_key(b, end, "First"), end, 0);
        if (N <= 0 || N > 200000 || first <= 0) continue;
        size_t len = 0; unsigned char *dec = pdf_stream_data(p, b, end, &len); if (!dec) continue;
        p->stms = realloc(p->stms, (size_t)(p->nstm + 1) * sizeof *p->stms);
        PdfStm *s = &p->stms[p->nstm++]; memset(s, 0, sizeof *s);
        s->num = p->o[k].num; s->data = dec; s->len = len; s->first = first; s->n = 0;
        s->onum = malloc((size_t)N * sizeof(int)); s->ooff = malloc((size_t)N * sizeof(int));
        const char *q = (const char *)dec, *qe = (const char *)dec + (size_t)first < (const char *)dec + len ? (const char *)dec + first : (const char *)dec + len;
        for (int i = 0; i < N && s->onum && s->ooff; i++) {
            q = pdf_skip(q, qe); if (q >= qe || !isdigit((unsigned char)*q)) break;
            char *e; long on = strtol(q, &e, 10); q = pdf_skip(e, qe); if (q >= qe || !isdigit((unsigned char)*q)) break;
            long off = strtol(q, &e, 10); q = e;
            s->onum[s->n] = (int)on; s->ooff[s->n] = (int)off; s->n++;
        }
    }
    for (int i = 0; i < p->nstm; i++) for (int k = 0; k < p->stms[i].n; k++) pdf_add_obj(p, p->stms[i].onum[k], 0, p->stms[i].num, k);
    qsort(p->o, (size_t)p->no, sizeof *p->o, pdf_cmp_obj);
}

// ---------------------------------------------------------------- fonts

static unsigned pdf_hexval(const char **pp, const char *end, int *digits) {     // <48656C6C6F> style hex, returns the value, digits = count
    const char *p = *pp; unsigned v = 0; int c = 0;
    if (p < end && *p == '<') p++;
    while (p < end && *p != '>') { if (isxdigit((unsigned char)*p)) { v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : (tolower(*p) - 'a' + 10)); c++; } p++; }
    if (p < end) p++;
    *pp = p; *digits = c; return v;
}
static void pdf_font_map(PdfFont *f, unsigned from, unsigned to) {
    if (from > 0xFFFF) return;
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 256; f->from = realloc(f->from, (size_t)f->cap * 2); f->to = realloc(f->to, (size_t)f->cap * 2); if (!f->from || !f->to) { f->n = f->cap = 0; return; } }
    f->from[f->n] = (unsigned short)from; f->to[f->n] = (unsigned short)(to > 0xFFFF ? '?' : to); f->n++;
}
static void pdf_parse_cmap(PdfFont *f, const char *c, const char *end) {
    for (const char *p = c; p < end; ) {
        const char *bc = strstr(p, "beginbfchar"), *br = strstr(p, "beginbfrange");
        if (!bc && !br) break;
        if (bc && (!br || bc < br)) {
            p = bc + 11;
            while (p < end && strncmp(p, "endbfchar", 9)) {
                p = pdf_skip(p, end); if (p >= end || *p != '<') { if (p < end && strncmp(p, "endbfchar", 9)) p++; continue; }
                int d1, d2; unsigned a = pdf_hexval(&p, end, &d1); p = pdf_skip(p, end); if (p >= end || *p != '<') break;
                unsigned b = pdf_hexval(&p, end, &d2);
                pdf_font_map(f, a, b);
                if (d1 > 2) f->two = 1;
            }
        } else {
            p = br + 12;
            while (p < end && strncmp(p, "endbfrange", 10)) {
                p = pdf_skip(p, end); if (p >= end || *p != '<') { if (p < end && strncmp(p, "endbfrange", 10)) p++; continue; }
                int d1, d2, d3; unsigned lo = pdf_hexval(&p, end, &d1); p = pdf_skip(p, end); if (p >= end || *p != '<') break;
                unsigned hi = pdf_hexval(&p, end, &d2); p = pdf_skip(p, end);
                if (d1 > 2) f->two = 1;
                if (p < end && *p == '[') {
                    p++; for (unsigned c2 = lo; c2 <= hi && p < end; c2++) { p = pdf_skip(p, end); if (p >= end || *p == ']') break; unsigned v = pdf_hexval(&p, end, &d3); pdf_font_map(f, c2, v); }
                    while (p < end && *p != ']') p++; if (p < end) p++;
                } else if (p < end && *p == '<') {
                    unsigned v = pdf_hexval(&p, end, &d3);
                    if (hi - lo > 65535) hi = lo + 65535;
                    for (unsigned c2 = lo; c2 <= hi; c2++) pdf_font_map(f, c2, v + (c2 - lo));
                }
            }
        }
    }
}
static unsigned short pdf_glyph_cp(const char *name, size_t l) {
    static const struct { const char *n; unsigned short c; } g[] = {
        {"space",32},{"exclam",33},{"quotedbl",34},{"numbersign",35},{"dollar",36},{"percent",37},{"ampersand",38},{"quotesingle",39},{"quoteright",0x2019},{"quoteleft",0x2018},
        {"parenleft",40},{"parenright",41},{"asterisk",42},{"plus",43},{"comma",44},{"hyphen",45},{"minus",45},{"period",46},{"slash",47},{"colon",58},{"semicolon",59},{"less",60},
        {"equal",61},{"greater",62},{"question",63},{"at",64},{"bracketleft",91},{"backslash",92},{"bracketright",93},{"underscore",95},{"grave",96},{"braceleft",123},{"bar",124},
        {"braceright",125},{"asciitilde",126},{"bullet",0x2022},{"endash",0x2013},{"emdash",0x2014},{"ellipsis",0x2026},{"quotedblleft",0x201C},{"quotedblright",0x201D},
        {"fi",0xFB01},{"fl",0xFB02},{"ff",0xFB00},{"ffi",0xFB03},{"ffl",0xFB04},{"zero",48},{"one",49},{"two",50},{"three",51},{"four",52},{"five",53},{"six",54},{"seven",55},
        {"eight",56},{"nine",57},{"nbspace",32},{"copyright",0xA9},{"registered",0xAE},{"degree",0xB0},{"eacute",0xE9},{"egrave",0xE8},{"agrave",0xE0},{"aacute",0xE1},{"ccedilla",0xE7},{NULL,0} };
    if (l == 1 && isalpha((unsigned char)name[0])) return (unsigned char)name[0];
    for (int i = 0; g[i].n; i++) if (strlen(g[i].n) == l && !strncmp(g[i].n, name, l)) return g[i].c;
    if (l > 3 && !strncmp(name, "uni", 3)) return (unsigned short)strtoul(name + 3, NULL, 16);
    return 0;
}
static PdfFont *pdf_font(Pdf *p, int num) {
    for (int i = 0; i < p->nfonts; i++) if (p->fonts[i].num == num) return p->fonts[i].f;
    PdfFont *f = calloc(1, sizeof *f); if (!f) return NULL;
    const char *end; const char *b = pdf_body(p, num, &end);
    if (b) {
        b = pdf_skip(b, end);
        const char *st = pdf_key(b, end, "Subtype");
        if (st && !strncmp(st, "/Type0", 6)) f->two = 1;
        const char *tu = pdf_key(b, end, "ToUnicode");
        int r = tu ? pdf_ref_at(tu, end) : -1;
        if (r >= 0) {
            const char *e2; const char *cb = pdf_body(p, r, &e2);
            size_t len = 0; unsigned char *cm = cb ? pdf_stream_data(p, pdf_skip(cb, e2), e2, &len) : NULL;
            if (cm) { pdf_parse_cmap(f, (const char *)cm, (const char *)cm + len); free(cm); }
        }
        for (int c = 0; c < 256; c++) f->diff[c] = 0;
        const char *enc = pdf_key(b, end, "Encoding");
        if (enc) {
            const char *ee = end; const char *ed = pdf_deref(p, enc, &ee);
            if (ed && *ed == '/') { if (!strncmp(ed, "/WinAnsiEncoding", 16)) f->winansi = 1; }
            else if (ed) {
                const char *be = pdf_key(ed, ee, "BaseEncoding"); if (be && !strncmp(be, "/WinAnsiEncoding", 16)) f->winansi = 1;
                const char *df = pdf_key(ed, ee, "Differences");
                if (df) {
                    const char *q = pdf_deref(p, df, &ee); if (q && *q == '[') {
                        q++; int code = 0;
                        while (q < ee && *q != ']') {
                            q = pdf_skip(q, ee);
                            if (isdigit((unsigned char)*q)) { code = (int)strtol(q, (char **)&q, 10); }
                            else if (*q == '/') { const char *s = ++q; while (q < ee && !pdf_ws((unsigned char)*q) && !pdf_delim((unsigned char)*q)) q++; if (code >= 0 && code < 256) f->diff[code] = pdf_glyph_cp(s, (size_t)(q - s)); code++; }
                            else q++;
                        }
                    }
                }
            }
        }
    }
#ifdef PDF_DEBUG
    fprintf(stderr, "font obj %d: map %d two %d body %s\n", num, f->n, f->two, b ? "yes" : "NO");
#endif
    p->fonts = realloc(p->fonts, (size_t)(p->nfonts + 1) * sizeof *p->fonts);
    p->fonts[p->nfonts].num = num; p->fonts[p->nfonts].f = f; p->nfonts++;
    return f;
}
static unsigned pdf_map_code(const PdfFont *f, unsigned code) {
    if (f->n) {
        int lo = 0, hi = f->n - 1;                                   // the map is built in file order, so search linearly for small maps
        for (int i = 0; i < f->n; i++) if (f->from[i] == code) return f->to[i];
        (void)lo; (void)hi;
    }
    if (f->two) return 0;                                            // two-byte codes without a map cannot be decoded
    if (code < 256 && f->diff[code]) return f->diff[code];
    if (code >= 0x80 && code < 0xA0) {                               // Windows-1252 specials
        static const unsigned short w[32] = { 0x20AC,0,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0,0x17D,0,0,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0,0x17E,0x178 };
        return w[code - 0x80];
    }
    return code;
}

// ---------------------------------------------------------------- the text of a page

typedef struct { char *b; size_t n, cap; } PdfBuf;
static void pdf_put(PdfBuf *o, int c) {
    if (o->n + 8 >= o->cap) { o->cap = o->cap ? o->cap * 2 : 1 << 16; o->b = realloc(o->b, o->cap); if (!o->b) { o->n = o->cap = 0; return; } }
    o->b[o->n++] = (char)c;
}
static void pdf_put_cp(PdfBuf *o, unsigned cp) {
    char tmp[8], *t = tmp;
    if (cp == 0xFB01) { pdf_put(o, 'f'); pdf_put(o, 'i'); return; }
    if (cp == 0xFB02) { pdf_put(o, 'f'); pdf_put(o, 'l'); return; }
    if (cp == 0xFB00) { pdf_put(o, 'f'); pdf_put(o, 'f'); return; }
    if (cp == 0xFB03) { pdf_put(o, 'f'); pdf_put(o, 'f'); pdf_put(o, 'i'); return; }
    if (cp == 0xFB04) { pdf_put(o, 'f'); pdf_put(o, 'f'); pdf_put(o, 'l'); return; }
    if (cp == 0 || cp == 0xFFFD) return;
    eb_put_cp(&t, cp);
    for (char *q = tmp; q < t; q++) pdf_put(o, *q);
}
static int pdf_last(const PdfBuf *o) { return o->n ? (unsigned char)o->b[o->n - 1] : '\n'; }
static void pdf_space(PdfBuf *o) { int c = pdf_last(o); if (c != ' ' && c != '\n') pdf_put(o, ' '); }
static void pdf_newline(PdfBuf *o, int blank) {
    while (o->n && o->b[o->n - 1] == ' ') o->n--;
    if (o->n && o->b[o->n - 1] != '\n') pdf_put(o, '\n');
    if (blank && o->n >= 2 && o->b[o->n - 2] != '\n') pdf_put(o, '\n');
}

typedef struct { int type; double num; char *s; size_t sl; char op[8]; } PdfTok;       // type: 0 end, 1 number, 2 string, 3 name, 4 operator, 5 array start, 6 array end
static int pdf_next_tok(const char **pp, const char *end, PdfTok *t, char *sbuf, size_t scap) {
    const char *p = pdf_skip(*pp, end);
    t->s = sbuf; t->sl = 0;
    if (p >= end) { *pp = p; t->type = 0; return 0; }
    unsigned char c = (unsigned char)*p;
    if (c == '(') {
        int depth = 1; p++;
        while (p < end && depth > 0) {
            unsigned char ch = (unsigned char)*p++;
            if (ch == '\\' && p < end) {
                unsigned char e = (unsigned char)*p++;
                if (e == 'n') ch = '\n'; else if (e == 'r') ch = '\r'; else if (e == 't') ch = '\t'; else if (e == 'b') ch = 8; else if (e == 'f') ch = 12;
                else if (e >= '0' && e <= '7') { int v = e - '0'; for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++) v = v * 8 + (*p++ - '0'); ch = (unsigned char)v; }
                else if (e == '\r' || e == '\n') { if (e == '\r' && p < end && *p == '\n') p++; continue; }
                else ch = e;
            } else if (ch == '(') depth++;
            else if (ch == ')') { if (--depth == 0) break; }
            if (t->sl + 1 < scap) sbuf[t->sl++] = (char)ch;
        }
        *pp = p; t->type = 2; return 1;
    }
    if (c == '<' && p + 1 < end && p[1] == '<') { int d = 1; p += 2; while (p + 1 < end && d > 0) { if (p[0] == '<' && p[1] == '<') { d++; p += 2; } else if (p[0] == '>' && p[1] == '>') { d--; p += 2; } else p++; } *pp = p; return pdf_next_tok(pp, end, t, sbuf, scap); }
    if (c == '<') {
        p++; int hi = -1;
        while (p < end && *p != '>') {
            if (isxdigit((unsigned char)*p)) { int v = isdigit((unsigned char)*p) ? *p - '0' : tolower(*p) - 'a' + 10; if (hi < 0) hi = v; else { if (t->sl + 1 < scap) sbuf[t->sl++] = (char)(hi * 16 + v); hi = -1; } }
            p++;
        }
        if (hi >= 0 && t->sl + 1 < scap) sbuf[t->sl++] = (char)(hi * 16);
        if (p < end) p++;
        *pp = p; t->type = 2; return 1;
    }
    if (c == '[') { *pp = p + 1; t->type = 5; return 1; }
    if (c == ']') { *pp = p + 1; t->type = 6; return 1; }
    if (c == '/') { p++; const char *s = p; while (p < end && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++; size_t l = (size_t)(p - s); if (l >= scap) l = scap - 1; memcpy(sbuf, s, l); sbuf[l] = 0; t->sl = l; *pp = p; t->type = 3; return 1; }
    if (isdigit(c) || c == '-' || c == '+' || c == '.') { char *e; t->num = strtod(p, &e); if (e == p) e = (char *)p + 1; *pp = e; t->type = 1; return 1; }
    if (c == '>' || c == '{' || c == '}' || c == ')') { *pp = p + 1; return pdf_next_tok(pp, end, t, sbuf, scap); }
    { const char *s = p; while (p < end && !pdf_ws((unsigned char)*p) && !pdf_delim((unsigned char)*p)) p++; if (p == s) p++; size_t l = (size_t)(p - s); if (l > 7) l = 7; memcpy(t->op, s, l); t->op[l] = 0; *pp = p; t->type = 4; return 1; }
}

static void pdf_show(PdfBuf *o, const PdfFont *f, const char *s, size_t n) {
    if (!f) return;
    if (f->two || (f->n && f->from && n >= 2 && f->two)) { for (size_t i = 0; i + 1 < n; i += 2) pdf_put_cp(o, pdf_map_code(f, ((unsigned char)s[i] << 8) | (unsigned char)s[i + 1])); }
    else for (size_t i = 0; i < n; i++) pdf_put_cp(o, pdf_map_code(f, (unsigned char)s[i]));
}

typedef struct { char name[32]; int num; } PdfFontRef;

static void pdf_page_text(Pdf *p, const char *res, const char *resend, const char *cont, const char *cend, PdfBuf *o) {
    // fonts of this page: name -> object number
    PdfFontRef fr[64]; int nfr = 0;
    if (res) {
        const char *re = resend; const char *rd = pdf_deref(p, res, &re);
        const char *fd = rd ? pdf_key(rd, re, "Font") : NULL;
        if (fd) {
            const char *fe = re; const char *fdict = pdf_deref(p, fd, &fe);
            if (fdict && fdict[0] == '<') {
                const char *q = fdict + 2;
                while (q < fe && nfr < 64) {
                    q = pdf_skip(q, fe); if (q >= fe || (q[0] == '>' && q[1] == '>')) break;
                    if (*q == '/') {
                        const char *s = ++q; while (q < fe && !pdf_ws((unsigned char)*q) && !pdf_delim((unsigned char)*q)) q++;
                        size_t l = (size_t)(q - s); if (l > 31) l = 31; memcpy(fr[nfr].name, s, l); fr[nfr].name[l] = 0;
                        int r = pdf_ref_at(q, fe); if (r >= 0) { fr[nfr].num = r; nfr++; }
                    } else q++;
                }
            }
        }
    }
#ifdef PDF_DEBUG
    fprintf(stderr, "  fonts on page: %d\n", nfr);
#endif
    PdfFont *font = NULL; double size = 12, lx = 0, ly = 0, lead = 0, last_y = 1e9, last_x = 0; int have_line = 0, moved = 0;
    char sbuf[8192]; PdfTok t; double stack[8]; int ns = 0; PdfBuf arr = {0}; (void)arr;
    char strs[16][1]; (void)strs;
    // operand storage: numbers and the last string/array text
    const char *c = cont;
    PdfBuf tmp = {0};                                                // text of the current operand (a string or array) decoded with the current font
    int in_arr = 0;
    while (pdf_next_tok(&c, cend, &t, sbuf, sizeof sbuf)) {
        if (t.type == 1) { if (in_arr) { if (t.num < -120 && tmp.n) { pdf_put(&tmp, ' '); } } else if (ns < 8) stack[ns++] = t.num; continue; }
        if (t.type == 5) { in_arr = 1; tmp.n = 0; continue; }
        if (t.type == 6) { in_arr = 0; continue; }
        if (t.type == 2) { pdf_show(&tmp, font, t.s, t.sl); continue; }
        if (t.type == 3) {
            // a name operand: either a font name (before Tf) or something else; remember the last font name
            int found = 0; for (int i = 0; i < nfr; i++) if (!strcmp(fr[i].name, t.s)) { font = pdf_font(p, fr[i].num); found = 1; break; }
            (void)found; ns = 0; continue;
        }
        if (t.type != 4) continue;
        const char *op = t.op;
        #define NEWLINE_IF_MOVED() do { \
            if (have_line && fabs(ly - last_y) > size * 0.4) { pdf_newline(o, fabs(ly - last_y) > size * 2.2); moved = 0; } \
            else if (have_line && moved) pdf_space(o); \
            have_line = 1; last_y = ly; moved = 0; } while (0)
        if (!strcmp(op, "BT")) { lx = ly = 0; ns = 0; tmp.n = 0; }
        else if (!strcmp(op, "Tf")) { if (ns >= 1) size = fabs(stack[ns - 1]) > 0.1 ? fabs(stack[ns - 1]) : size; ns = 0; tmp.n = 0; }
        else if (!strcmp(op, "Td") || !strcmp(op, "TD")) { if (ns >= 2) { lx += stack[ns - 2]; ly += stack[ns - 1]; if (op[1] == 'D') lead = -stack[ns - 1]; if (fabs(stack[ns - 1]) < 1e-6 && stack[ns - 2] != 0) moved = 1; } ns = 0; tmp.n = 0; }
        else if (!strcmp(op, "Tm")) { if (ns >= 6) { if (fabs(stack[5] - ly) < size * 0.4 && stack[4] != lx) moved = 1; lx = stack[4]; ly = stack[5]; } ns = 0; tmp.n = 0; }
        else if (!strcmp(op, "TL")) { if (ns >= 1) lead = stack[ns - 1]; ns = 0; }
        else if (!strcmp(op, "T*")) { ly -= lead ? lead : size * 1.2; tmp.n = 0; }
        else if (!strcmp(op, "Tj") || !strcmp(op, "TJ")) { NEWLINE_IF_MOVED(); for (size_t i = 0; i < tmp.n; i++) pdf_put(o, (unsigned char)tmp.b[i]); tmp.n = 0; ns = 0; }
        else if (!strcmp(op, "'") || !strcmp(op, "\"")) { ly -= lead ? lead : size * 1.2; NEWLINE_IF_MOVED(); for (size_t i = 0; i < tmp.n; i++) pdf_put(o, (unsigned char)tmp.b[i]); tmp.n = 0; ns = 0; }
        else if (!strcmp(op, "BI")) {                                  // an inline image: skip to EI
            while (c + 2 < cend) { if (pdf_ws((unsigned char)c[0]) && c[1] == 'E' && c[2] == 'I' && (c + 3 >= cend || pdf_ws((unsigned char)c[3]))) { c += 3; break; } c++; }
            ns = 0;
        }
        else if (!strcmp(op, "ET")) { ns = 0; tmp.n = 0; }
        else { ns = 0; if (strcmp(op, "q") && strcmp(op, "Q")) tmp.n = 0; }
        #undef NEWLINE_IF_MOVED
        (void)lx;
    }
    free(tmp.b);
    pdf_newline(o, 1);
}

// Walk the page tree in order.
static void pdf_page_do_text(Pdf *p, int num, const char *r, const char *re, PdfBuf *o);
static void pdf_walk(Pdf *p, int num, const char *inherited_res, const char *inh_end, PdfBuf *o, int depth) {
    if (depth > 40 || p->pages > 20000) return;
    const char *end; const char *b = pdf_body(p, num, &end); if (!b) return;
    b = pdf_skip(b, end);
    const char *kids = pdf_key(b, end, "Kids");
    const char *res = pdf_key(b, end, "Resources");
    const char *r = res ? res : inherited_res, *re = res ? end : inh_end;
    if (kids) {
        const char *ke = end; const char *k = pdf_deref(p, kids, &ke);
        if (k && *k == '[') { k++; while (k < ke && *k != ']') { int r2 = pdf_ref_at(k, ke); if (r2 < 0) break; pdf_walk(p, r2, r, re, o, depth + 1);
            while (k < ke && *k != 'R') k++; if (k < ke) k++; k = pdf_skip(k, ke); } }
        return;
    }
    const char *ty = pdf_key(b, end, "Type");
    if (ty && !strncmp(ty, "/Pages", 6)) return;
    p->pages++;
    if (p->collect) {                                              // only list the pages
        if (p->npg == p->pgcap) { p->pgcap = p->pgcap ? p->pgcap * 2 : 256; p->pg = realloc(p->pg, (size_t)p->pgcap * sizeof *p->pg); if (!p->pg) { p->npg = p->pgcap = 0; return; } }
        p->pg[p->npg].num = num; p->pg[p->npg].res = r; p->pg[p->npg].resend = re; p->npg++;
        return;
    }
    pdf_page_do_text(p, num, r, re, o);
}

// The text of one page: its content streams joined, then run through the text operators.
static void pdf_page_do_text(Pdf *p, int num, const char *r, const char *re, PdfBuf *o) {
    const char *end; const char *b = pdf_body(p, num, &end); if (!b) return;
    b = pdf_skip(b, end);
    const char *cont = pdf_key(b, end, "Contents"); if (!cont) return;
#ifdef PDF_DEBUG
    fprintf(stderr, "page obj %d contents %.20s\n", num, cont);
#endif
    int refs[256], nrefs = 0;
    const char *ce = end; const char *cd = cont;
    int single = pdf_ref_at(cd, ce);
    if (single >= 0) {
        const char *e2; const char *body = pdf_body(p, single, &e2);
        if (body && pdf_skip(body, e2)[0] == '[') { cd = pdf_skip(body, e2); ce = e2; single = -1; }       // the reference points to an array
        else refs[nrefs++] = single;
    }
    if (single < 0 && cd && *cd == '[') { const char *k = cd + 1; while (k < ce && *k != ']' && nrefs < 256) { int r2 = pdf_ref_at(k, ce); if (r2 < 0) break; refs[nrefs++] = r2; while (k < ce && *k != 'R') k++; if (k < ce) k++; k = pdf_skip(k, ce); } }
    // join the content streams (an operator may not be split across them, so a space joins them)
    size_t total = 0; unsigned char *parts[256] = {0}; size_t lens[256] = {0};
    for (int i = 0; i < nrefs; i++) {
        const char *e2; const char *body = pdf_body(p, refs[i], &e2); if (!body) continue;
        parts[i] = pdf_stream_data(p, pdf_skip(body, e2), e2, &lens[i]); total += lens[i] + 1;
    }
    unsigned char *all = malloc(total + 1);
    if (all) {
        size_t at = 0; for (int i = 0; i < nrefs; i++) if (parts[i]) { memcpy(all + at, parts[i], lens[i]); at += lens[i]; all[at++] = '\n'; }
        all[at] = 0;
#ifdef PDF_DEBUG
        fprintf(stderr, "  content %zu bytes\n", at); if (getenv("PDF_DUMP")) { fwrite(all, 1, at, stderr); fprintf(stderr, "\n=====\n"); }
#endif
        pdf_page_text(p, r, re, (const char *)all, (const char *)all + at, o);
        free(all);
    }
    for (int i = 0; i < nrefs; i++) free(parts[i]);
}

// ---------------------------------------------------------------- tidy up: join the lines of a paragraph

static size_t pdf_reflow(char *t, size_t n) {
    // find the typical long line
    size_t maxl = 0, cur = 0; for (size_t i = 0; i <= n; i++) { if (i == n || t[i] == '\n') { if (cur > maxl && cur < 200) maxl = cur; cur = 0; } else cur++; }
    char *o = t; size_t i = 0;
    while (i < n) {
        size_t e = i; while (e < n && t[e] != '\n') e++;
        size_t len = e - i;
        memmove(o, t + i, len); o += len;
        if (e < n) {
            size_t ne = e + 1; int next_blank = ne >= n || t[ne] == '\n';
            char last = len ? t[e - 1] : '\n';
            int long_line = maxl && len > maxl * 6 / 10;
            if (len && !next_blank && long_line && last != '.' && last != '!' && last != '?' && last != ':' && last != '"') {
                if (last == '-' && ne < n && islower((unsigned char)t[ne]) && len > 1 && isalpha((unsigned char)t[e - 2])) o--;      // hyphenated word: join without the hyphen
                else *o++ = ' ';
            } else *o++ = '\n';
        }
        i = e + 1;
    }
    *o = 0; return (size_t)(o - t);
}

// ---------------------------------------------------------------- the document: open once, then ask for text or page pictures

typedef Pdf PdfDoc;
static void pdf_doc_close(PdfDoc *d) {
    if (!d) return;
    for (int i = 0; i < d->nstm; i++) { free(d->stms[i].data); free(d->stms[i].onum); free(d->stms[i].ooff); }
    for (int i = 0; i < d->nfonts; i++) { free(d->fonts[i].f->from); free(d->fonts[i].f->to); free(d->fonts[i].f); }
    free(d->stms); free(d->fonts); free(d->o); free(d->pg); free(d->d); free(d);
}
static PdfDoc *pdf_doc_open(const char *path) {
    pdf_error[0] = 0;
    Pdf *pd = calloc(1, sizeof *pd); if (!pd) return NULL;
    pd->d = eb_slurp(path, &pd->n, 200u << 20);
    if (!pd->d || pd->n < 20) { snprintf(pdf_error, sizeof pdf_error, "cannot read the file"); pdf_doc_close(pd); return NULL; }
    if (!pdf_memmem(pd->d, pd->n < 1024 ? pd->n : 1024, "%PDF", 4)) { snprintf(pdf_error, sizeof pdf_error, "not a PDF file"); pdf_doc_close(pd); return NULL; }
    {   // encrypted?
        const char *tail = (const char *)pd->d + (pd->n > 4096 ? pd->n - 4096 : 0);
        if (pdf_memmem(tail, (size_t)((const char *)pd->d + pd->n - tail), "/Encrypt", 8)) { snprintf(pdf_error, sizeof pdf_error, "this PDF is locked"); pdf_doc_close(pd); return NULL; }
    }
    pdf_index(pd);
    // the root of the page tree: /Root in the trailer or xref stream, else the Pages node without a parent
    int pages_root = -1;
    {
        const char *last = NULL; for (const char *q = (const char *)pd->d; q + 5 <= (const char *)pd->d + pd->n; ) { const char *f = pdf_memmem(q, (size_t)((const char *)pd->d + pd->n - q), "/Root", 5); if (!f) break; last = f; q = f + 5; }
        if (last) { int cat = pdf_ref_at(last + 5, (const char *)pd->d + pd->n);
            if (cat >= 0) { const char *e; const char *b = pdf_body(pd, cat, &e); if (b) { const char *pv = pdf_key(pdf_skip(b, e), e, "Pages"); if (pv) pages_root = pdf_ref_at(pv, e); } } }
        if (pages_root < 0) for (int i = 0; i < pd->no; i++) {
            const char *e; const char *b = pdf_body(pd, pd->o[i].num, &e); if (!b) continue; b = pdf_skip(b, e);
            const char *ty = pdf_key(b, e, "Type"); if (ty && !strncmp(ty, "/Pages", 6) && !pdf_key(b, e, "Parent")) { pages_root = pd->o[i].num; break; }
        }
    }
    pd->collect = 1;
    if (pages_root >= 0) pdf_walk(pd, pages_root, NULL, NULL, NULL, 0);
    pd->collect = 0;
    if (!pd->npg) { snprintf(pdf_error, sizeof pdf_error, "no pages found"); pdf_doc_close(pd); return NULL; }
    return pd;
}
static int pdf_doc_npages(const PdfDoc *d) { return d->npg; }

// All the text of the document. NULL when it has none.
static char *pdf_doc_text(PdfDoc *d, size_t *len) {
    PdfBuf out = {0};
    for (int k = 0; k < d->npg; k++) pdf_page_do_text(d, d->pg[k].num, d->pg[k].res, d->pg[k].resend, &out);
    if (!out.n) { snprintf(pdf_error, sizeof pdf_error, "no text found (a scanned PDF?)"); free(out.b); return NULL; }
    out.b[out.n] = 0;
    *len = pdf_reflow(out.b, out.n);
    return out.b;
}
static char *pdf_text(const char *path, size_t *len) {
    PdfDoc *d = pdf_doc_open(path); if (!d) return NULL;
    char *t = pdf_doc_text(d, len);
    pdf_doc_close(d);
    return t;
}

// ---------------------------------------------------------------- page pictures (for scanned PDFs)
// The largest picture on a page is shown. JPEG pictures (the usual scan format) and plain pixel data (Flate, with or without PNG
// predictors) are supported. JPEG 2000, JBIG2, and CCITT fax pictures are not. Needs stb_image.h before this file for JPEG.
#ifdef STBI_VERSION
static void pdf_unpredict(unsigned char *d, size_t *len, int predictor, int colors, int bpc, int columns) {
    if (predictor < 10) return;
    int bpp = (colors * bpc + 7) / 8; size_t rowb = ((size_t)columns * colors * bpc + 7) / 8;
    if (!rowb) return;
    size_t rows = *len / (rowb + 1), o = 0;
    unsigned char *prev = calloc(1, rowb);
    for (size_t r = 0; r < rows; r++) {
        unsigned char *src = d + r * (rowb + 1), ft = src[0], *cur = d + o;
        for (size_t i = 0; i < rowb; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0, x = src[1 + i], v;
            switch (ft) {
                case 1: v = x + a; break;
                case 2: v = x + b; break;
                case 3: v = x + ((a + b) >> 1); break;
                case 4: { int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c); v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c); break; }
                default: v = x;
            }
            cur[i] = (unsigned char)v;
        }
        memcpy(prev, cur, rowb); o += rowb;
    }
    free(prev); *len = o;
}
// Find the picture objects of a page, return the body of the largest. *w and *h are its size.
static const char *pdf_biggest_image(Pdf *p, const PdfPage *pg, const char **iend, int *w, int *h) {
    if (!pg->res) return NULL;
    const char *re = pg->resend; const char *rd = pdf_deref(p, pg->res, &re); if (!rd) return NULL;
    const char *xo = pdf_key(rd, re, "XObject"); if (!xo) return NULL;
    const char *xe = re; const char *xd = pdf_deref(p, xo, &xe); if (!xd || xd[0] != '<') return NULL;
    const char *best = NULL, *bend = NULL; long bestarea = 0; int bw = 0, bh = 0;
    for (const char *q = xd + 2; q < xe; ) {
        q = pdf_skip(q, xe); if (q >= xe || (q[0] == '>' && q[1] == '>')) break;
        if (*q != '/') { q++; continue; }
        q++; while (q < xe && !pdf_ws((unsigned char)*q) && !pdf_delim((unsigned char)*q)) q++;
        int r = pdf_ref_at(q, xe); if (r < 0) continue;
        const char *e2; const char *b = pdf_body(p, r, &e2); if (!b) continue; b = pdf_skip(b, e2);
        const char *st = pdf_key(b, e2, "Subtype"); if (!st || strncmp(st, "/Image", 6)) continue;
        int iw = pdf_int(pdf_key(b, e2, "Width"), e2, 0), ih = pdf_int(pdf_key(b, e2, "Height"), e2, 0);
        if (iw > 0 && ih > 0 && (long)iw * ih > bestarea) { bestarea = (long)iw * ih; best = b; bend = e2; bw = iw; bh = ih; }
    }
    if (best) { *iend = bend; *w = bw; *h = bh; }
    return best;
}
// The pixels (0xRRGGBB) of one picture object, at its own size. NULL with pdf_error when it cannot be read.
static uint32_t *pdf_decode_image(Pdf *p, const char *b, const char *iend, int *piw, int *pih) {
    int iw = pdf_int(pdf_key(b, iend, "Width"), iend, 0), ih = pdf_int(pdf_key(b, iend, "Height"), iend, 0);
    if (iw <= 0 || ih <= 0 || (long)iw * ih > 90000000L) { snprintf(pdf_error, sizeof pdf_error, "the picture is too big"); return NULL; }
    p->img_mode = 1;
    size_t len = 0; unsigned char *raw = pdf_stream_data(p, b, iend, &len); int filt = p->img_filter; p->img_mode = 0;
    if (!raw) { snprintf(pdf_error, sizeof pdf_error, "cannot read this picture"); return NULL; }
    uint32_t *rgb = NULL;
    if (filt == 1) {                                               // JPEG
        int w, h, c; unsigned char *im = stbi_load_from_memory(raw, (int)len, &w, &h, &c, 3);
        if (!im) { snprintf(pdf_error, sizeof pdf_error, "cannot decode the JPEG picture"); free(raw); return NULL; }
        rgb = malloc((size_t)w * h * 4);
        if (rgb) for (long i = 0; i < (long)w * h; i++) rgb[i] = ((uint32_t)im[i * 3] << 16) | ((uint32_t)im[i * 3 + 1] << 8) | im[i * 3 + 2];
        stbi_image_free(im); iw = w; ih = h;
    } else if (filt) {
        snprintf(pdf_error, sizeof pdf_error, filt == 2 ? "this scan uses JPEG 2000, which is not supported" : filt == 3 ? "this scan uses CCITT fax coding, which is not supported" : "this scan uses JBIG2, which is not supported");
        free(raw); return NULL;
    } else {                                                       // plain samples
        int bpc = pdf_int(pdf_key(b, iend, "BitsPerComponent"), iend, 8), colors = 1, indexed = 0, hival = 0; const unsigned char *pal = NULL; unsigned char *palbuf = NULL;
        const char *cs = pdf_key(b, iend, "ColorSpace"); const char *ce = iend;
        if (cs) { cs = pdf_deref(p, cs, &ce); if (!cs) cs = pdf_key(b, iend, "ColorSpace"); }
        if (cs) {
            if (!strncmp(cs, "/DeviceRGB", 10) || !strncmp(cs, "/CalRGB", 7)) colors = 3;
            else if (!strncmp(cs, "/DeviceCMYK", 11)) colors = 4;
            else if (*cs == '[') {
                const char *q = pdf_skip(cs + 1, ce);
                if (!strncmp(q, "/Indexed", 8) || !strncmp(q, "/I ", 3)) {
                    indexed = 1; q = pdf_skip(q + (q[2] == 'n' ? 8 : 2), ce);
                    if (*q == '/') { while (q < ce && !pdf_ws((unsigned char)*q) && *q != '[' && *q != '<' && *q != '(') { q++; if (q < ce && *q == '/') break; } }
                    q = pdf_skip(q, ce);
                    if (*q == '/') { q++; while (q < ce && !pdf_ws((unsigned char)*q) && !pdf_delim((unsigned char)*q)) q++; }
                    else if (*q == '[') { int d = 1; q++; while (q < ce && d) { if (*q == '[') d++; else if (*q == ']') d--; q++; } }
                    else if (isdigit((unsigned char)*q)) { int r2 = pdf_ref_at(q, ce); while (q < ce && *q != 'R') q++; if (q < ce) q++; (void)r2; }
                    q = pdf_skip(q, ce); hival = (int)strtol(q, (char **)&q, 10); q = pdf_skip(q, ce);
                    if (*q == '<') { const char *h = q + 1; size_t pn = 0; palbuf = malloc(1024); int hi = -1; while (h < ce && *h != '>' && pn < 768) { if (isxdigit((unsigned char)*h)) { int v = isdigit((unsigned char)*h) ? *h - '0' : tolower(*h) - 'a' + 10; if (hi < 0) hi = v; else { palbuf[pn++] = (unsigned char)(hi * 16 + v); hi = -1; } } h++; } pal = palbuf; }
                    else if (*q == '(') { palbuf = malloc(1024); q++; size_t pn = 0; while (q < ce && *q != ')' && pn < 768) { palbuf[pn++] = (unsigned char)*q++; } pal = palbuf; }
                    else { int r2 = pdf_ref_at(q, ce); if (r2 >= 0) { const char *e3; const char *pb2 = pdf_body(p, r2, &e3); size_t pl = 0; unsigned char *pd2 = pb2 ? pdf_stream_data(p, pdf_skip(pb2, e3), e3, &pl) : NULL; if (pd2) { palbuf = malloc(1024); memcpy(palbuf, pd2, pl < 768 ? pl : 768); pal = palbuf; free(pd2); } } }
                    colors = 1;
                } else if (!strncmp(q, "/ICCBased", 9)) {
                    int r2 = pdf_ref_at(q + 9, ce);
                    if (r2 >= 0) { const char *e3; const char *pb2 = pdf_body(p, r2, &e3); if (pb2) { int n = pdf_int(pdf_key(pdf_skip(pb2, e3), e3, "N"), e3, 3); colors = n == 1 ? 1 : n == 4 ? 4 : 3; } }
                    else colors = 3;
                } else if (!strncmp(q, "/CalRGB", 7) || !strncmp(q, "/Lab", 4)) colors = 3;
            }
        }
        const char *dp = pdf_key(b, iend, "DecodeParms"); int pred = 1, pcols = iw, pcolors = colors, pbpc = bpc;
        if (dp) { const char *de = iend; const char *dd = pdf_deref(p, dp, &de); if (dd && *dd == '[') { dd = pdf_skip(dd + 1, de); } if (dd && dd[0] == '<') {
            pred = pdf_int(pdf_key(dd, de, "Predictor"), de, 1); pcols = pdf_int(pdf_key(dd, de, "Columns"), de, iw); pcolors = pdf_int(pdf_key(dd, de, "Colors"), de, colors); pbpc = pdf_int(pdf_key(dd, de, "BitsPerComponent"), de, bpc); } }
        pdf_unpredict(raw, &len, pred, pcolors, pbpc, pcols);
        int invert = 0; const char *dc = pdf_key(b, iend, "Decode"); if (dc && *dc == '[' && !indexed) { const char *q = pdf_skip(dc + 1, iend); if (*q == '1') invert = 1; }
        size_t rowb = ((size_t)iw * colors * bpc + 7) / 8;
        if (rowb * (size_t)ih > len + rowb || (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8)) { snprintf(pdf_error, sizeof pdf_error, "this picture format is not supported"); free(raw); free(palbuf); return NULL; }
        rgb = malloc((size_t)iw * ih * 4);
        if (rgb) for (int y = 0; y < ih; y++) {
            const unsigned char *row = raw + (size_t)y * rowb;
            if ((size_t)y * rowb + rowb > len) { for (int x = 0; x < iw; x++) rgb[(size_t)y * iw + x] = 0xFFFFFF; continue; }
            for (int x = 0; x < iw; x++) {
                int r = 0, g = 0, bl = 0;
                if (colors == 1) {
                    int v;
                    if (bpc == 8) v = row[x]; else { int per = 8 / bpc, sh = 8 - bpc * (x % per + 1); v = (row[x / per] >> sh) & ((1 << bpc) - 1); }
                    if (indexed && pal && v <= hival && v * 3 + 2 < 768) { r = pal[v * 3]; g = pal[v * 3 + 1]; bl = pal[v * 3 + 2]; }
                    else { int mx = (1 << bpc) - 1; v = v * 255 / mx; if (invert) v = 255 - v; r = g = bl = v; }
                } else if (colors == 3) { r = row[x * 3]; g = row[x * 3 + 1]; bl = row[x * 3 + 2]; if (invert) { r = 255 - r; g = 255 - g; bl = 255 - bl; } }
                else { int c = row[x * 4], m = row[x * 4 + 1], yy = row[x * 4 + 2], k = row[x * 4 + 3]; r = (255 - c) * (255 - k) / 255; g = (255 - m) * (255 - k) / 255; bl = (255 - yy) * (255 - k) / 255; }
                rgb[(size_t)y * iw + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
            }
        }
        free(palbuf);
    }
    free(raw);
    if (!rgb) { snprintf(pdf_error, sizeof pdf_error, "out of memory"); return NULL; }
    *piw = iw; *pih = ih;
    return rgb;
}

// Scale a picture to w2 x h2: a box average when it gets smaller, repeated pixels when it gets bigger.
static uint32_t *pdf_scale(const uint32_t *rgb, int iw, int ih, int w2, int h2) {
    if (w2 < 1) w2 = 1; if (h2 < 1) h2 = 1;
    uint32_t *out = malloc((size_t)w2 * h2 * 4); if (!out) return NULL;
    for (int y = 0; y < h2; y++) {
        int sy0 = (int)((long)y * ih / h2), sy1 = (int)((long)(y + 1) * ih / h2); if (sy1 <= sy0) sy1 = sy0 + 1; if (sy1 > ih) sy1 = ih;
        for (int x = 0; x < w2; x++) {
            int sx0 = (int)((long)x * iw / w2), sx1 = (int)((long)(x + 1) * iw / w2); if (sx1 <= sx0) sx1 = sx0 + 1; if (sx1 > iw) sx1 = iw;
            long ar = 0, ag = 0, ab = 0, cnt = 0;
            for (int yy = sy0; yy < sy1; yy++) for (int xx = sx0; xx < sx1; xx++) { uint32_t c = rgb[(size_t)yy * iw + xx]; ar += (c >> 16) & 255; ag += (c >> 8) & 255; ab += c & 255; cnt++; }
            out[(size_t)y * w2 + x] = ((uint32_t)(ar / cnt) << 16) | ((uint32_t)(ag / cnt) << 8) | (uint32_t)(ab / cnt);
        }
    }
    return out;
}

// ---- compose a page from its pictures. The content stream says where each one goes (cm sets the placement, Do draws).
static int pdf_page_attr_box(Pdf *p, const PdfPage *pg, const char *key, double *v) {
    int num = pg->num;
    for (int depth = 0; depth < 10 && num >= 0; depth++) {
        const char *end; const char *b = pdf_body(p, num, &end); if (!b) return 0;
        b = pdf_skip(b, end);
        const char *k = pdf_key(b, end, key);
        if (k) {
            const char *ke = end; const char *a = pdf_deref(p, k, &ke);
            if (a && *a == '[') { const char *q = a + 1; for (int i = 0; i < 4; i++) { q = pdf_skip(q, ke); char *e; v[i] = strtod(q, &e); if (e == q) return 0; q = e; } return 1; }
            return 0;
        }
        const char *par = pdf_key(b, end, "Parent"); num = par ? pdf_ref_at(par, end) : -1;
    }
    return 0;
}
static int pdf_page_rotate(Pdf *p, const PdfPage *pg) {
    int num = pg->num;
    for (int depth = 0; depth < 10 && num >= 0; depth++) {
        const char *end; const char *b = pdf_body(p, num, &end); if (!b) return 0;
        b = pdf_skip(b, end);
        const char *k = pdf_key(b, end, "Rotate");
        if (k) { int r = pdf_int(k, end, 0) % 360; return r < 0 ? r + 360 : r; }
        const char *par = pdf_key(b, end, "Parent"); num = par ? pdf_ref_at(par, end) : -1;
    }
    return 0;
}
static int pdf_xobject_ref(Pdf *p, const PdfPage *pg, const char *name) {
    if (!pg->res) return -1;
    const char *re = pg->resend; const char *rd = pdf_deref(p, pg->res, &re); if (!rd) return -1;
    const char *xo = pdf_key(rd, re, "XObject"); if (!xo) return -1;
    const char *xe = re; const char *xd = pdf_deref(p, xo, &xe); if (!xd || xd[0] != '<') return -1;
    const char *k = pdf_key(xd, xe, name); return k ? pdf_ref_at(k, xe) : -1;
}
typedef struct { double a, b, c, d, e, f; } PdfMat;
static PdfMat pdf_mat_mul(PdfMat m, PdfMat c) {                           // m applied first, then c (PDF: new CTM = m x CTM)
    PdfMat r = { m.a * c.a + m.b * c.c, m.a * c.b + m.b * c.d, m.c * c.a + m.d * c.c, m.c * c.b + m.d * c.d,
                 m.e * c.a + m.f * c.c + c.e, m.e * c.b + m.f * c.d + c.f };
    return r;
}
// Draw one picture onto the canvas. The picture is the unit square mapped by m (PDF user space, origin at the bottom left of the box).
static void pdf_place(uint32_t *cv, int cw, int ch, double bx0, double by1, double sc, PdfMat m, const uint32_t *img, int iw, int ih, const unsigned char *alpha, int aw, int ah) {
    double du = sqrt(m.a * m.a + m.b * m.b) * sc, dv = sqrt(m.c * m.c + m.d * m.d) * sc;        // size on the canvas, in pixels
    int tw = (int)(du + 0.5), th = (int)(dv + 0.5); if (tw < 1) tw = 1; if (th < 1) th = 1;
    if (tw > 6000 || th > 6000) return;
    uint32_t *si = pdf_scale(img, iw, ih, tw, th); if (!si) return;
    unsigned char *sa = NULL;
    if (alpha) { uint32_t *tmp = malloc((size_t)aw * ah * 4); if (tmp) { for (long i = 0; i < (long)aw * ah; i++) tmp[i] = alpha[i] * 0x010101u; uint32_t *ta = pdf_scale(tmp, aw, ah, tw, th); free(tmp);
        if (ta) { sa = malloc((size_t)tw * th); if (sa) for (long i = 0; i < (long)tw * th; i++) sa[i] = (unsigned char)(ta[i] & 255); free(ta); } } }
    // the four corners on the canvas give the bounding box
    double cx[4], cy[4]; int k = 0;
    for (int vi = 0; vi < 2; vi++) for (int ui = 0; ui < 2; ui++) { double ux = m.a * ui + m.c * vi + m.e, uy = m.b * ui + m.d * vi + m.f; cx[k] = (ux - bx0) * sc; cy[k] = (by1 - uy) * sc; k++; }
    double minx = cx[0], maxx = cx[0], miny = cy[0], maxy = cy[0];
    for (int i = 1; i < 4; i++) { if (cx[i] < minx) minx = cx[i]; if (cx[i] > maxx) maxx = cx[i]; if (cy[i] < miny) miny = cy[i]; if (cy[i] > maxy) maxy = cy[i]; }
    int x0 = (int)floor(minx), x1 = (int)ceil(maxx), y0 = (int)floor(miny), y1 = (int)ceil(maxy);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > cw) x1 = cw; if (y1 > ch) y1 = ch;
    double det = m.a * m.d - m.b * m.c;
    if (fabs(det) > 1e-9) for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        double ux = x / sc + bx0 + 0.5 / sc, uy = by1 - (y + 0.5) / sc - 0 ;                          // canvas pixel -> user space
        ux = (x + 0.5) / sc + bx0; uy = by1 - (y + 0.5) / sc;
        double dx = ux - m.e, dy = uy - m.f;
        double u = (dx * m.d - dy * m.c) / det, v = (dy * m.a - dx * m.b) / det;                      // user space -> the unit square
        if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
        int sx = (int)(u * tw), sy = (int)((1.0 - v) * th);                                           // image rows run top to bottom
        if (sx >= tw) sx = tw - 1; if (sy >= th) sy = th - 1; if (sy < 0) sy = 0;
        uint32_t c = si[(size_t)sy * tw + sx]; int a = sa ? sa[(size_t)sy * tw + sx] : 255;
        if (a == 255) cv[(size_t)y * cw + x] = c;
        else if (a) {
            uint32_t d = cv[(size_t)y * cw + x];
            int r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255, g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255, bl = ((c & 255) * a + (d & 255) * (255 - a)) / 255;
            cv[(size_t)y * cw + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
        }
    }
    free(si); free(sa);
}
// The picture of one page, maxw pixels wide. Draws every picture on the page where the page puts it (a soft mask makes parts clear).
// Vector drawings and text are not drawn. NULL with pdf_error when the page has no picture.
static uint32_t *pdf_doc_image(PdfDoc *p, int page, int maxw, int *ow, int *oh) {
    if (page < 0 || page >= p->npg) return NULL;
    const PdfPage *pg = &p->pg[page];
    double box[4] = { 0, 0, 612, 792 };
    if (!pdf_page_attr_box(p, pg, "CropBox", box) && !pdf_page_attr_box(p, pg, "MediaBox", box)) { box[0] = 0; box[1] = 0; box[2] = 612; box[3] = 792; }
    double bw = fabs(box[2] - box[0]), bh = fabs(box[3] - box[1]); if (bw < 1 || bh < 1) { bw = 612; bh = 792; }
    double bx0 = fmin(box[0], box[2]), by1 = fmax(box[1], box[3]);
    int rot = pdf_page_rotate(p, pg);
    int cwid = (rot == 90 || rot == 270) ? (int)(bh * maxw / bw + 0.5) : maxw;                      // size before the page rotation is applied
    double sc = (rot == 90 || rot == 270) ? (double)maxw / bh : (double)maxw / bw;
    (void)cwid;
    int cw = (int)(bw * sc + 0.5), ch = (int)(bh * sc + 0.5); if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if ((long)cw * ch > 40000000L) { snprintf(pdf_error, sizeof pdf_error, "the page is too big"); return NULL; }
    uint32_t *cv = malloc((size_t)cw * ch * 4); if (!cv) { snprintf(pdf_error, sizeof pdf_error, "out of memory"); return NULL; }
    for (long i = 0; i < (long)cw * ch; i++) cv[i] = 0xFFFFFF;
    // run the page content for q, Q, cm, Do
    size_t total = 0; unsigned char *all = NULL;
    {
        const char *end; const char *b = pdf_body(p, pg->num, &end);
        const char *cont = b ? pdf_key(pdf_skip(b, end), end, "Contents") : NULL;
        int refs[256], nrefs = 0;
        if (cont) {
            const char *cd = cont, *ce = end; int single = pdf_ref_at(cd, ce);
            if (single >= 0) { const char *e2; const char *body = pdf_body(p, single, &e2); if (body && pdf_skip(body, e2)[0] == '[') { cd = pdf_skip(body, e2); ce = e2; single = -1; } else refs[nrefs++] = single; }
            if (single < 0 && cd && *cd == '[') { const char *k = cd + 1; while (k < ce && *k != ']' && nrefs < 256) { int r2 = pdf_ref_at(k, ce); if (r2 < 0) break; refs[nrefs++] = r2; while (k < ce && *k != 'R') k++; if (k < ce) k++; k = pdf_skip(k, ce); } }
        }
        unsigned char *parts[256] = {0}; size_t lens[256] = {0};
        for (int i = 0; i < nrefs; i++) { const char *e2; const char *body = pdf_body(p, refs[i], &e2); if (!body) continue; parts[i] = pdf_stream_data(p, pdf_skip(body, e2), e2, &lens[i]); total += lens[i] + 1; }
        all = malloc(total + 1);
        if (all) { size_t at = 0; for (int i = 0; i < nrefs; i++) if (parts[i]) { memcpy(all + at, parts[i], lens[i]); at += lens[i]; all[at++] = '\n'; } all[at] = 0; total = at; }
        for (int i = 0; i < nrefs; i++) free(parts[i]);
    }
    int placed = 0, firsterr = 0; char lasterr[96] = "";
    if (all) {
        PdfMat stack[32], cur = { 1, 0, 0, 1, 0, 0 }; int sp = 0;
        char sbuf[4096]; PdfTok t; double nums[8]; int nn = 0; char lastname[64] = "";
        const char *c = (const char *)all, *cend = (const char *)all + total;
        while (pdf_next_tok(&c, cend, &t, sbuf, sizeof sbuf)) {
            if (t.type == 1) { if (nn < 8) nums[nn++] = t.num; continue; }
            if (t.type == 3) { snprintf(lastname, sizeof lastname, "%s", t.s); continue; }
            if (t.type != 4) { continue; }
            if (!strcmp(t.op, "q")) { if (sp < 32) stack[sp++] = cur; nn = 0; }
            else if (!strcmp(t.op, "Q")) { if (sp > 0) cur = stack[--sp]; nn = 0; }
            else if (!strcmp(t.op, "cm")) { if (nn >= 6) { PdfMat m = { nums[nn - 6], nums[nn - 5], nums[nn - 4], nums[nn - 3], nums[nn - 2], nums[nn - 1] }; cur = pdf_mat_mul(m, cur); } nn = 0; }
            else if (!strcmp(t.op, "Do")) {
                int r = pdf_xobject_ref(p, pg, lastname); nn = 0;
                if (r < 0) continue;
                const char *e2; const char *ib = pdf_body(p, r, &e2); if (!ib) continue; ib = pdf_skip(ib, e2);
                const char *st = pdf_key(ib, e2, "Subtype"); if (!st || strncmp(st, "/Image", 6)) continue;
                int iw = 0, ih = 0; pdf_error[0] = 0;
                uint32_t *img = pdf_decode_image(p, ib, e2, &iw, &ih);
                if (!img) { if (!firsterr) { firsterr = 1; snprintf(lasterr, sizeof lasterr, "%s", pdf_error); } continue; }
                unsigned char *alpha = NULL; int aw = 0, ah = 0;
                const char *sm = pdf_key(ib, e2, "SMask"); int smr = sm ? pdf_ref_at(sm, e2) : -1;
                if (smr >= 0) {
                    const char *e3; const char *sb = pdf_body(p, smr, &e3);
                    if (sb) { uint32_t *g = pdf_decode_image(p, pdf_skip(sb, e3), e3, &aw, &ah);
                        if (g) { alpha = malloc((size_t)aw * ah); if (alpha) for (long i = 0; i < (long)aw * ah; i++) alpha[i] = (unsigned char)(g[i] & 255); free(g); } }
                }
                pdf_place(cv, cw, ch, bx0, by1, sc, cur, img, iw, ih, alpha, aw, ah);
                placed++; free(img); free(alpha);
            }
            else nn = 0;
        }
        free(all);
    }
    if (!placed) {
        free(cv); snprintf(pdf_error, sizeof pdf_error, "%s", lasterr[0] ? lasterr : "this page has no picture");
        return NULL;
    }
    // rotate the finished page
    if (rot) {
        int nw = (rot == 90 || rot == 270) ? ch : cw, nh = (rot == 90 || rot == 270) ? cw : ch;
        uint32_t *rv = malloc((size_t)nw * nh * 4);
        if (rv) {
            for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
                int nx, ny;
                if (rot == 90) { nx = ch - 1 - y; ny = x; } else if (rot == 180) { nx = cw - 1 - x; ny = ch - 1 - y; } else { nx = y; ny = cw - 1 - x; }
                rv[(size_t)ny * nw + nx] = cv[(size_t)y * cw + x];
            }
            free(cv); cv = rv; cw = nw; ch = nh;
        }
    }
    *ow = cw; *oh = ch;
    return cv;
}
#endif

#endif
