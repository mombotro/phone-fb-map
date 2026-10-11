// epub.h - turn a book file into plain ASCII text, with no libraries. Plain C, no platform code.
//   book_load(path, &len)   returns a malloc'ed, NUL-ended ASCII text of a .txt or .epub file (NULL on failure). The caller frees it.
// An EPUB is a ZIP file of XHTML pages. This reads the ZIP directory, inflates the pages, follows the reading order of the book
// (the "spine" in the OPF file), strips the tags, and turns the characters into ASCII, because the screen font is ASCII.
#ifndef EPUB_H
#define EPUB_H
#include <ctype.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- inflate (RFC 1951), after the structure of zlib's puff.c

typedef struct {
    unsigned char *out; size_t outlen, outcnt;
    const unsigned char *in; size_t inlen, incnt;
    unsigned bitbuf; int bitcnt;
    jmp_buf env;
} EbInfl;
typedef struct { short count[16]; short symbol[320]; } EbHuff;

static int eb_bits(EbInfl *s, int need) {
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) longjmp(s->env, 1);
        val |= (long)s->in[s->incnt++] << s->bitcnt; s->bitcnt += 8;
    }
    s->bitbuf = (unsigned)(val >> need); s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}
static int eb_decode(EbInfl *s, const EbHuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= eb_bits(s, 1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int eb_construct(EbHuff *h, const short *length, int n) {
    for (int len = 0; len <= 15; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
    short offs[16]; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++) if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}
static int eb_codes(EbInfl *s, const EbHuff *lc, const EbHuff *dc) {
    static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
    static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
    static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
    static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
    int sym;
    do {
        sym = eb_decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) { if (s->outcnt >= s->outlen) return -1; s->out[s->outcnt++] = (unsigned char)sym; }
        else if (sym > 256) {
            sym -= 257; if (sym >= 29) return -1;
            int len = lens[sym] + eb_bits(s, lext[sym]);
            sym = eb_decode(s, dc); if (sym < 0 || sym >= 30) return -1;
            size_t dist = (size_t)dists[sym] + eb_bits(s, dext[sym]);
            if (dist > s->outcnt || s->outcnt + len > s->outlen) return -1;
            while (len--) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    } while (sym != 256);
    return 0;
}
static int eb_dynamic(EbInfl *s) {
    static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short lengths[320]; EbHuff lc, dc;
    int nlen = eb_bits(s, 5) + 257, ndist = eb_bits(s, 5) + 1, ncode = eb_bits(s, 4) + 4;
    if (nlen > 286 || ndist > 30) return -1;
    int idx;
    for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (short)eb_bits(s, 3);
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (eb_construct(&lc, lengths, 19) != 0) return -1;
    idx = 0;
    while (idx < nlen + ndist) {
        int sym = eb_decode(s, &lc); if (sym < 0) return -1;
        if (sym < 16) lengths[idx++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return -1; len = lengths[idx - 1]; rep = 3 + eb_bits(s, 2); }
            else if (sym == 17) rep = 3 + eb_bits(s, 3);
            else rep = 11 + eb_bits(s, 7);
            if (idx + rep > nlen + ndist) return -1;
            while (rep--) lengths[idx++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = eb_construct(&lc, lengths, nlen);
    if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return -1;
    err = eb_construct(&dc, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return -1;
    return eb_codes(s, &lc, &dc);
}
// Inflate `in` into `out`. Returns 0 when the stream ended, 1 when `out` was too small (more data would follow), -1 on bad data.
// *produced is the number of bytes written. For a ZIP file outlen is the exact size, and eb_inflate wants all of it.
static int eb_inflate_ex(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen, size_t *produced) {
    EbInfl s; memset(&s, 0, sizeof s);
    s.in = in; s.inlen = inlen; s.out = out; s.outlen = outlen;
    *produced = 0;
    if (setjmp(s.env)) { *produced = s.outcnt; return s.outcnt >= outlen - (outlen > 300 ? 300 : 0) ? 1 : -1; }   // out of input: a damaged tail, or out of room
    int last, err = 0;
    do {
        last = eb_bits(&s, 1);
        int type = eb_bits(&s, 2);
        if (type == 0) {
            s.bitbuf = 0; s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen) { *produced = s.outcnt; return -1; }
            unsigned len = s.in[s.incnt] | (s.in[s.incnt + 1] << 8);
            unsigned nlen = s.in[s.incnt + 2] | (s.in[s.incnt + 3] << 8);
            s.incnt += 4;
            if (len != (~nlen & 0xFFFF) || s.incnt + len > s.inlen) { *produced = s.outcnt; return -1; }
            if (s.outcnt + len > s.outlen) { *produced = s.outcnt; return 1; }
            memcpy(s.out + s.outcnt, s.in + s.incnt, len); s.incnt += len; s.outcnt += len;
        } else if (type == 1) {
            short lengths[320]; EbHuff lc, dc;
            int sym;
            for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
            for (; sym < 256; sym++) lengths[sym] = 9;
            for (; sym < 280; sym++) lengths[sym] = 7;
            for (; sym < 288; sym++) lengths[sym] = 8;
            eb_construct(&lc, lengths, 288);
            for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
            eb_construct(&dc, lengths, 30);
            err = eb_codes(&s, &lc, &dc);
        } else if (type == 2) err = eb_dynamic(&s);
        else { *produced = s.outcnt; return -1; }
        if (err) { *produced = s.outcnt; return s.outcnt >= s.outlen ? 1 : -1; }
    } while (!last);
    *produced = s.outcnt;
    return 0;
}
static int eb_inflate(const unsigned char *in, size_t inlen, unsigned char *out, size_t outlen) {
    size_t got; int r = eb_inflate_ex(in, inlen, out, outlen, &got);
    return (r == 0 && got == outlen) ? 0 : -1;
}

// ---------------------------------------------------------------- ZIP directory

typedef struct { char name[256]; unsigned method; uint32_t csize, usize, off; } EbEntry;
typedef struct { unsigned char *d; size_t n; EbEntry *e; int ne; } EbZip;

static unsigned eb_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t eb_u32(const unsigned char *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static unsigned char *eb_slurp(const char *path, size_t *n, size_t cap) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > cap) { fclose(f); return NULL; }
    unsigned char *b = malloc((size_t)sz + 1); if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f); fclose(f);
    b[got] = 0; *n = got; return b;
}
static int eb_zip_open(EbZip *z, const char *path) {
    memset(z, 0, sizeof *z);
    z->d = eb_slurp(path, &z->n, 120u << 20); if (!z->d || z->n < 22) return -1;
    size_t p = z->n - 22; int found = 0;
    for (size_t back = 0; back < 70000 && p + 4 <= z->n; back++, p--) {
        if (eb_u32(z->d + p) == 0x06054b50) { found = 1; break; }
        if (p == 0) break;
    }
    if (!found) return -1;
    int ne = (int)eb_u16(z->d + p + 10); uint32_t dir = eb_u32(z->d + p + 16);
    z->e = calloc((size_t)ne + 1, sizeof(EbEntry)); if (!z->e) return -1;
    size_t q = dir;
    for (int i = 0; i < ne; i++) {
        if (q + 46 > z->n || eb_u32(z->d + q) != 0x02014b50) break;
        EbEntry *e = &z->e[z->ne];
        e->method = eb_u16(z->d + q + 10); e->csize = eb_u32(z->d + q + 20); e->usize = eb_u32(z->d + q + 24);
        unsigned nl = eb_u16(z->d + q + 28), xl = eb_u16(z->d + q + 30), cl = eb_u16(z->d + q + 32);
        e->off = eb_u32(z->d + q + 42);
        if (q + 46 + nl > z->n) break;
        size_t c = nl < sizeof e->name - 1 ? nl : sizeof e->name - 1;
        memcpy(e->name, z->d + q + 46, c); e->name[c] = 0;
        z->ne++; q += 46 + nl + xl + cl;
    }
    return z->ne ? 0 : -1;
}
static void eb_zip_close(EbZip *z) { free(z->d); free(z->e); memset(z, 0, sizeof *z); }
static int eb_zip_find(const EbZip *z, const char *name) {
    for (int i = 0; i < z->ne; i++) if (strcmp(z->e[i].name, name) == 0) return i;
    for (int i = 0; i < z->ne; i++) if (strcasecmp(z->e[i].name, name) == 0) return i;
    return -1;
}
// The bytes of one file in the ZIP, NUL-ended. Free with free().
static unsigned char *eb_zip_read(const EbZip *z, int i, size_t *len) {
    const EbEntry *e = &z->e[i];
    if (e->off + 30 > z->n || eb_u32(z->d + e->off) != 0x04034b50 || e->usize > (64u << 20)) return NULL;
    size_t start = e->off + 30 + eb_u16(z->d + e->off + 26) + eb_u16(z->d + e->off + 28);
    if (start + e->csize > z->n) return NULL;
    unsigned char *out = malloc((size_t)e->usize + 1); if (!out) return NULL;
    if (e->method == 0) { if (e->csize != e->usize) { free(out); return NULL; } memcpy(out, z->d + start, e->usize); }
    else if (e->method == 8) { if (eb_inflate(z->d + start, e->csize, out, e->usize)) { free(out); return NULL; } }
    else { free(out); return NULL; }
    out[e->usize] = 0; *len = e->usize;
    return out;
}

// ---------------------------------------------------------------- text out of XHTML

// Append the ASCII form of a Unicode code point.
static void eb_put_cp(char **o, unsigned cp) {
    char *p = *o;
    if (cp < 128) { if (cp >= 32 || cp == '\n') *p++ = (char)cp; else *p++ = ' '; }
    else if (cp == 0xA0 || cp == 0x2009 || cp == 0x200A || cp == 0x202F || (cp >= 0x2002 && cp <= 0x2008)) *p++ = ' ';
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) *p++ = '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) *p++ = '"';
    else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2015 || cp == 0x2212) *p++ = '-';
    else if (cp == 0x2026) { *p++ = '.'; *p++ = '.'; *p++ = '.'; }
    else if (cp == 0x2022 || cp == 0xB7) *p++ = '*';
    else if (cp == 0xA9) { *p++ = '('; *p++ = 'c'; *p++ = ')'; }
    else if (cp == 0xAE) { *p++ = '('; *p++ = 'R'; *p++ = ')'; }
    else if (cp == 0x2122) { *p++ = 'T'; *p++ = 'M'; }
    else if (cp == 0xB0) { *p++ = 'o'; }
    else if (cp == 0xB1) { *p++ = '+'; *p++ = '/'; *p++ = '-'; }
    else if (cp == 0xBD) { *p++ = '1'; *p++ = '/'; *p++ = '2'; }
    else if (cp == 0xBC) { *p++ = '1'; *p++ = '/'; *p++ = '4'; }
    else if (cp == 0xBE) { *p++ = '3'; *p++ = '/'; *p++ = '4'; }
    else if (cp == 0xD7) *p++ = 'x';
    else if (cp == 0xF7) *p++ = '/';
    else if (cp == 0xAB) { *p++ = '<'; *p++ = '<'; }
    else if (cp == 0xBB) { *p++ = '>'; *p++ = '>'; }
    else if (cp >= 0xC0 && cp <= 0xFF) {                       // Latin-1 letters lose their marks
        static const char *base = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        char c = base[cp - 0xC0];
        if (cp == 0xC6) { *p++ = 'A'; *p++ = 'E'; } else if (cp == 0xE6) { *p++ = 'a'; *p++ = 'e'; } else *p++ = c;
    }
    else if (cp >= 0x100 && cp <= 0x17F) *p++ = '?';
    else *p++ = '?';
    *o = p;
}
// ASCII-fy a UTF-8 text in place (the result is never longer than 3 bytes per code point of the input... and at most as long as the input).
static size_t eb_utf8_to_ascii(char *s, size_t n) {
    char *o = s; size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        unsigned cp; int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { i++; continue; }
        if (i + extra >= n + 0 && extra) { break; }
        for (int k = 1; k <= extra; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += 1 + extra;
        char tmp[8], *t = tmp; eb_put_cp(&t, cp);
        size_t room = i - (size_t)(o - s);                      // never write past what has been read
        size_t w = (size_t)(t - tmp); if (w > room) w = room;
        memcpy(o, tmp, w); o += w;
    }
    *o = 0; return (size_t)(o - s);
}

static int eb_tag_is(const char *t, const char *name) {          // t points after '<' (and after '/' for closing tags)
    size_t l = strlen(name);
    return strncasecmp(t, name, l) == 0 && !isalnum((unsigned char)t[l]);
}
static int eb_is_block(const char *t) {
    static const char *b[] = { "p", "div", "br", "h1", "h2", "h3", "h4", "h5", "h6", "li", "tr", "blockquote", "section", "hr", "title", "dt", "dd", "pre", "table", "ul", "ol", NULL };
    for (int i = 0; b[i]; i++) if (eb_tag_is(t, b[i])) return 1;
    return 0;
}
// Strip the tags from an XHTML page and append the text to out. Returns the new end. `out` must have room for 1 byte per input byte + 4.
static char *eb_html_text(const char *h, size_t n, char *out) {
    const char *end = h + n; char *o = out; int skip = 0;
    while (h < end) {
        if (*h == '<') {
            const char *t = h + 1; int closing = 0;
            if (t < end && *t == '/') { closing = 1; t++; }
            if (end - h > 3 && !strncmp(h, "<!--", 4)) { const char *e = strstr(h + 4, "-->"); h = e ? e + 3 : end; continue; }
            if (!closing && (eb_tag_is(t, "script") || eb_tag_is(t, "style") || eb_tag_is(t, "head"))) skip = 1;
            if (closing && (eb_tag_is(t, "script") || eb_tag_is(t, "style") || eb_tag_is(t, "head"))) skip = 0;
            else if (!skip && eb_is_block(t) && o > out && o[-1] != '\n') { *o++ = '\n'; if (closing || eb_tag_is(t, "p") || eb_tag_is(t, "h1") || eb_tag_is(t, "h2") || eb_tag_is(t, "h3")) *o++ = '\n'; }
            while (h < end && *h != '>') h++;
            if (h < end) h++;
        } else if (skip) h++;
        else if (*h == '&') {
            const char *e = h + 1; while (e < end && e - h < 10 && *e != ';') e++;
            unsigned cp = 0; int ok = 0;
            if (e < end && *e == ';') {
                if (h[1] == '#') cp = (h[2] == 'x' || h[2] == 'X') ? (unsigned)strtoul(h + 3, NULL, 16) : (unsigned)strtoul(h + 2, NULL, 10), ok = cp != 0;
                else {
                    static const struct { const char *n; unsigned c; } ent[] = { {"amp",'&'},{"lt",'<'},{"gt",'>'},{"quot",'"'},{"apos",'\''},{"nbsp",' '},
                        {"mdash",0x2014},{"ndash",0x2013},{"hellip",0x2026},{"lsquo",0x2018},{"rsquo",0x2019},{"ldquo",0x201C},{"rdquo",0x201D},{NULL,0} };
                    for (int i = 0; ent[i].n; i++) if ((size_t)(e - h - 1) == strlen(ent[i].n) && !strncmp(h + 1, ent[i].n, e - h - 1)) { cp = ent[i].c; ok = 1; break; }
                }
            }
            if (ok) { char tmp[8], *t = tmp; eb_put_cp(&t, cp); memcpy(o, tmp, (size_t)(t - tmp)); o += t - tmp; h = e + 1; }
            else *o++ = *h++;
        } else {
            unsigned char c = (unsigned char)*h++;
            if (c == '\r') continue;
            if (c == '\n' || c == '\t') c = ' ';
            if (c == ' ' && (o == out || o[-1] == ' ' || o[-1] == '\n')) continue;       // collapse white space
            *o++ = (char)c;
        }
    }
    return o;
}

// A value of an attribute (name="value") inside a tag that starts at tag. Copies up to n-1 characters. Returns 1 when found.
static int eb_attr(const char *tag, const char *name, char *val, size_t n) {
    const char *end = strchr(tag, '>'); if (!end) return 0;
    size_t l = strlen(name);
    for (const char *p = tag; p < end; p++) {
        if ((p == tag || isspace((unsigned char)p[-1])) && !strncasecmp(p, name, l) && p[l] == '=' && (p[l + 1] == '"' || p[l + 1] == '\'')) {
            char q = p[l + 1]; const char *s = p + l + 2, *e = memchr(s, q, (size_t)(end - s)); if (!e) return 0;
            size_t c = (size_t)(e - s) < n - 1 ? (size_t)(e - s) : n - 1; memcpy(val, s, c); val[c] = 0; return 1;
        }
    }
    return 0;
}
static void eb_unescape_url(char *s) {                           // %20 and friends
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
        else if (*s == '#') break;
        else *o++ = *s;
    }
    *o = 0;
}

typedef struct { char id[96], href[256]; } EbItem;

// All the readable text of an EPUB, in reading order.
static char *eb_epub_text(const char *path, size_t *len) {
    EbZip z; if (eb_zip_open(&z, path)) return NULL;
    int order[4096], norder = 0; char *result = NULL;
    size_t clen = 0; unsigned char *cont = NULL;
    int ci = eb_zip_find(&z, "META-INF/container.xml");
    if (ci >= 0) cont = eb_zip_read(&z, ci, &clen);
    char opf_path[300] = "";
    if (cont) { const char *rf = strstr((char *)cont, "<rootfile "); if (rf) eb_attr(rf, "full-path", opf_path, sizeof opf_path); }
    free(cont);
    char dir[300] = ""; { char *sl = strrchr(opf_path, '/'); if (sl) { memcpy(dir, opf_path, (size_t)(sl - opf_path + 1)); dir[sl - opf_path + 1] = 0; } }
    int oi = opf_path[0] ? eb_zip_find(&z, opf_path) : -1;
    if (oi >= 0) {
        size_t ol = 0; char *opf = (char *)eb_zip_read(&z, oi, &ol);
        if (opf) {
            EbItem *items = calloc(2048, sizeof(EbItem)); int ni = 0;
            for (char *p = opf; (p = strstr(p, "<item")) && ni < 2048; p++) {
                if (!isspace((unsigned char)p[5])) continue;
                if (eb_attr(p, "id", items[ni].id, sizeof items[ni].id) && eb_attr(p, "href", items[ni].href, sizeof items[ni].href)) ni++;
            }
            for (char *p = opf; (p = strstr(p, "<itemref")) && norder < 4096; p++) {
                char idref[96]; if (!eb_attr(p, "idref", idref, sizeof idref)) continue;
                for (int k = 0; k < ni; k++) if (!strcmp(items[k].id, idref)) {
                    char full[600]; char href[256]; snprintf(href, sizeof href, "%s", items[k].href); eb_unescape_url(href);
                    snprintf(full, sizeof full, "%s%s", dir, href);
                    int zi = eb_zip_find(&z, full); if (zi >= 0) order[norder++] = zi;
                    break;
                }
            }
            free(items); free(opf);
        }
    }
    if (!norder) {   // no usable OPF: every page in file order
        for (int i = 0; i < z.ne && norder < 4096; i++) {
            size_t l = strlen(z.e[i].name);
            if ((l > 5 && !strcasecmp(z.e[i].name + l - 5, ".html")) || (l > 6 && !strcasecmp(z.e[i].name + l - 6, ".xhtml")) || (l > 4 && !strcasecmp(z.e[i].name + l - 4, ".htm"))) order[norder++] = i;
        }
    }
    size_t cap = 1 << 16, used = 0; result = malloc(cap);
    for (int k = 0; k < norder && result; k++) {
        size_t pl = 0; unsigned char *page = eb_zip_read(&z, order[k], &pl); if (!page) continue;
        if (used + pl + 8 > cap) { while (used + pl + 8 > cap) cap *= 2; char *nr = realloc(result, cap); if (!nr) { free(page); free(result); result = NULL; break; } result = nr; }
        char *e = eb_html_text((const char *)page, pl, result + used);
        used = (size_t)(e - result);
        if (used && result[used - 1] != '\n') result[used++] = '\n';
        result[used++] = '\n';                                     // a blank line between pages
        free(page);
    }
    eb_zip_close(&z);
    if (!result) return NULL;
    result[used] = 0; *len = used;
    return result;
}

// ---------------------------------------------------------------- the public call

static int eb_has_ext(const char *n, const char *e) { size_t a = strlen(n), b = strlen(e); return a > b && !strcasecmp(n + a - b, e); }

// A .txt, .md, or .epub file as ASCII text. Long runs of blank lines are shortened to one blank line.
static char *book_load(const char *path, size_t *len) {
    char *t = NULL; size_t n = 0;
    if (eb_has_ext(path, ".epub")) t = eb_epub_text(path, &n);
    else t = (char *)eb_slurp(path, &n, 64u << 20);
    if (!t) return NULL;
    n = eb_utf8_to_ascii(t, n);
    for (size_t i = 0; i < n; i++) if (t[i] == '\r') t[i] = ' ';
    char *o = t; int nl = 0;                                       // compact blank lines, drop tabs
    for (size_t i = 0; i < n; i++) {
        char c = t[i]; if (c == '\t') c = ' ';
        if (c == '\n') { if (++nl > 2) continue; } else nl = 0;
        *o++ = c;
    }
    *o = 0; *len = (size_t)(o - t);
    return t;
}
#endif
