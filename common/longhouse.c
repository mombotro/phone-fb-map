// longhouse.c - a Longhouse chat client for the framebuffer phones. Text only for now.
// usage: longhouse [seconds] [dir]      (default 86400 seconds, dir <data>/data/longhouse)
//
// Longhouse is a private family chat mesh. Each household runs a home-node. This app talks to the home-node over plain HTTP on the
// home network (GET /conversations, GET and POST /conversations/:id/messages, Bearer token). It never needs HTTPS: the Mac signs in
// (tools/longhouse_login.py) and puts two files on the phone, in the data folder:
//   session.json    {"home":"http://192.168.1.20:8787","token":"...","personId":"...","displayName":"..."}
//   directory.json  {"names":{"<person id>":"<display name>", ...}}
// The token lasts 24 hours. When it expires, the app says so, and you run the sign-in tool again.
// Picture thumbnails, live updates (/live), and sign-in on the phone are not here yet.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS                                    // iOS 7 has no thread-local storage
#include "third_party/stb_image.h"
#include "lhnet.h"
#include <pthread.h>
#include <sys/stat.h>
#ifdef __ANDROID__
#include <sys/syscall.h>               // rename() fails on Android 4.1 with this toolkit: call the kernel
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_DIR "/var/root/data/longhouse"
#else
#define DEFAULT_DIR "/data/local/tmp/data/longhouse"
#endif

// ---------------------------------------------------------------- JSON (just enough)

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct JN { char t; char *s; double n; struct JN **v; char **k; int len, cap; } JN;

static JN *jnew(char t) { JN *j = calloc(1, sizeof *j); j->t = t; return j; }
static void jfree(JN *j) {
    if (!j) return;
    for (int i = 0; i < j->len; i++) { jfree(j->v[i]); if (j->k) free(j->k[i]); }
    free(j->v); free(j->k); free(j->s); free(j);
}
static void jpush(JN *p, char *key, JN *c) {
    if (p->len == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->v = realloc(p->v, p->cap * sizeof *p->v);
        p->k = realloc(p->k, p->cap * sizeof *p->k);
    }
    p->v[p->len] = c; p->k[p->len] = key; p->len++;
}
static const char *jws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }

static void put_utf8(char **o, unsigned c) {
    if (c < 0x80) *(*o)++ = (char)c;
    else if (c < 0x800) { *(*o)++ = (char)(0xC0 | (c >> 6)); *(*o)++ = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { *(*o)++ = (char)(0xE0 | (c >> 12)); *(*o)++ = (char)(0x80 | ((c >> 6) & 63)); *(*o)++ = (char)(0x80 | (c & 63)); }
    else { *(*o)++ = (char)(0xF0 | (c >> 18)); *(*o)++ = (char)(0x80 | ((c >> 12) & 63)); *(*o)++ = (char)(0x80 | ((c >> 6) & 63)); *(*o)++ = (char)(0x80 | (c & 63)); }
}
static int hex4(const char *p) { int v = 0; for (int i = 0; i < 4; i++) { if (!isxdigit((unsigned char)p[i])) return -1; v = v * 16 + (isdigit((unsigned char)p[i]) ? p[i] - '0' : (tolower(p[i]) - 'a' + 10)); } return v; }

static char *jstring(const char **pp) {                       // *pp is just after the opening quote; returns a malloc'ed UTF-8 string
    const char *p = *pp;
    size_t cap = 16; char *out = malloc(cap), *o = out;
    while (*p && *p != '"') {
        if ((size_t)(o - out) + 8 > cap) { size_t n = o - out; cap *= 2; out = realloc(out, cap); o = out + n; }
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n': *o++ = '\n'; p++; break; case 't': *o++ = '\t'; p++; break; case 'r': *o++ = '\r'; p++; break;
                case 'b': *o++ = '\b'; p++; break; case 'f': *o++ = '\f'; p++; break;
                case 'u': {
                    int c = hex4(p + 1); p += 5;
                    if (c >= 0xD800 && c < 0xDC00 && p[0] == '\\' && p[1] == 'u') { int d = hex4(p + 2); if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); p += 6; } }
                    put_utf8(&o, c < 0 ? '?' : (unsigned)c); break;
                }
                default: if (*p) { *o++ = *p++; }
            }
        } else *o++ = *p++;
    }
    *o = 0; if (*p == '"') p++;
    *pp = p; return out;
}

static JN *jvalue(const char **pp, int depth) {
    const char *p = jws(*pp);
    if (depth > 40) return NULL;
    JN *j = NULL;
    if (*p == '{') {
        j = jnew(J_OBJ); p = jws(p + 1);
        while (*p && *p != '}') {
            if (*p != '"') { jfree(j); return NULL; }
            p++; char *key = jstring(&p); p = jws(p);
            if (*p != ':') { free(key); jfree(j); return NULL; }
            p++;                                                        // step past the colon
            JN *c = jvalue(&p, depth + 1); if (!c) { free(key); jfree(j); return NULL; }
            jpush(j, key, c); p = jws(p); if (*p == ',') p = jws(p + 1);
        }
        if (*p == '}') p++;
    } else if (*p == '[') {
        j = jnew(J_ARR); p = jws(p + 1);
        while (*p && *p != ']') {
            JN *c = jvalue(&p, depth + 1); if (!c) { jfree(j); return NULL; }
            jpush(j, NULL, c); p = jws(p); if (*p == ',') p = jws(p + 1);
        }
        if (*p == ']') p++;
    } else if (*p == '"') { j = jnew(J_STR); p++; j->s = jstring(&p); }
    else if (!strncmp(p, "true", 4)) { j = jnew(J_BOOL); j->n = 1; p += 4; }
    else if (!strncmp(p, "false", 5)) { j = jnew(J_BOOL); p += 5; }
    else if (!strncmp(p, "null", 4)) { j = jnew(J_NULL); p += 4; }
    else if (*p == '-' || isdigit((unsigned char)*p)) { char *e; j = jnew(J_NUM); j->n = strtod(p, &e); p = e; }
    else return NULL;
    *pp = p; return j;
}
static JN *jparse(const char *text) { const char *p = text; return jvalue(&p, 0); }
static JN *jget(const JN *o, const char *key) {
    if (!o || o->t != J_OBJ) return NULL;
    for (int i = 0; i < o->len; i++) if (!strcmp(o->k[i], key)) return o->v[i];
    return NULL;
}
static const char *jstr(const JN *j) { return j && j->t == J_STR && j->s ? j->s : ""; }

// ---------------------------------------------------------------- HTTP (plain, one request per connection)

static char g_err[200];
typedef struct { char host[64]; int port; char token[1024]; char person[96]; char name[64]; char home[96]; char home_id[96]; char sig_host[64]; char sig_path[96]; char sig_ip[48]; char refresh[80]; } Cfg;
static unsigned long g_node_epoch, g_node_epoch_at;      // the home-node's clock (HTTP Date header), for the TLS certificate dates
static char g_dir[256];                                   // the data folder (session.json and directory.json)
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;   // guards the token and the name list while a thread renews them
static unsigned long tls_epoch(void) { return g_node_epoch ? g_node_epoch + ((unsigned long)time(NULL) - g_node_epoch_at) : (unsigned long)time(NULL); }
static Cfg g_cfg;
static int g_have_cfg;

static int parse_home(const char *url, Cfg *c) {                  // "http://1.2.3.4:8787"
    if (strncmp(url, "http://", 7)) return -1;
    const char *h = url + 7; const char *colon = strchr(h, ':'); const char *slash = strchr(h, '/');
    size_t hl = colon && (!slash || colon < slash) ? (size_t)(colon - h) : (slash ? (size_t)(slash - h) : strlen(h));
    if (hl == 0 || hl >= sizeof c->host) return -1;
    memcpy(c->host, h, hl); c->host[hl] = 0;
    c->port = colon && (!slash || colon < slash) ? atoi(colon + 1) : 80;
    return 0;
}

static int connect_to(const char *host, int port) {
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) != 1) { snprintf(g_err, sizeof g_err, "home address must be an IPv4 number: %s", host); return -1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr = a;
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    if (r < 0 && errno == EINPROGRESS) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w); struct timeval tv = { 4, 0 };
        if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) { close(fd); snprintf(g_err, sizeof g_err, "cannot reach %s (is the phone on the home Wi-Fi?)", host); return -1; }
        int e = 0; socklen_t el = sizeof e; getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
        if (e) { close(fd); snprintf(g_err, sizeof g_err, "cannot reach %s (%s)", host, strerror(e)); return -1; }
    } else if (r < 0) { close(fd); snprintf(g_err, sizeof g_err, "connect: %s", strerror(errno)); return -1; }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv = { 8, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static char *dechunk(const char *in, size_t n, size_t *outn) {   // decode "Transfer-Encoding: chunked"
    char *out = malloc(n + 1); size_t o = 0; const char *p = in, *end = in + n;
    while (p < end) {
        char *e; unsigned long len = strtoul(p, &e, 16);
        while (e < end && *e != '\n') e++;
        if (e >= end) break;
        p = e + 1;
        if (len == 0) break;
        if (p + len > end) len = (unsigned long)(end - p);
        memcpy(out + o, p, len); o += len; p += len;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
    out[o] = 0; *outn = o; return out;
}

// Returns the HTTP status (0 on a network error). *out is a malloc'ed, NUL-terminated body.
static int http_do_raw(const char *method, const char *path, const char *body, char **out) {
    *out = NULL; g_err[0] = 0;
    if (!g_have_cfg) { snprintf(g_err, sizeof g_err, "no session: run tools/longhouse_login.py on the Mac"); return 0; }
    int fd = connect_to(g_cfg.host, g_cfg.port);
    if (fd < 0) return 0;
    size_t bl = body ? strlen(body) : 0;
    char tok[1024]; pthread_mutex_lock(&g_cfg_lock); snprintf(tok, sizeof tok, "%s", g_cfg.token); pthread_mutex_unlock(&g_cfg_lock);
    char *req = malloc(bl + 1400);
    int rl = snprintf(req, bl + 1400, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nAuthorization: Bearer %s\r\nAccept: application/json\r\n"
                      "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                      method, path, g_cfg.host, g_cfg.port, tok, bl, body ? body : "");
    if (send(fd, req, (size_t)rl, 0) != rl) { snprintf(g_err, sizeof g_err, "send failed"); free(req); close(fd); return 0; }
    free(req);
    size_t cap = 16384, n = 0; char *buf = malloc(cap);
    for (;;) {
        if (n + 8192 > cap) { if (cap > (6u << 20)) break; cap *= 2; buf = realloc(buf, cap); }
        ssize_t r = recv(fd, buf + n, cap - n - 1, 0);
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd); buf[n] = 0;
    int status = 0; if (n > 12 && !strncmp(buf, "HTTP/", 5)) status = atoi(buf + 9);
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!status || !hdr_end) { snprintf(g_err, sizeof g_err, "no answer from the home-node"); free(buf); return 0; }
    *hdr_end = 0; char *b = hdr_end + 4; size_t bn = n - (size_t)(b - buf);
    for (char *h = buf; *h; h++) if (h[0] == '\n' && !strncasecmp(h + 1, "date:", 5)) {            // "Date: Sun, 11 Oct 2026 05:35:23 GMT"
        static const char *mon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        int d, y, hh, mm, ss; char ms[8] = "";
        if (sscanf(h + 6, " %*3s, %d %3s %d %d:%d:%d", &d, ms, &y, &hh, &mm, &ss) == 6)
            for (int i = 0; i < 12; i++) if (!strcmp(ms, mon[i])) {
                struct tm t; memset(&t, 0, sizeof t); t.tm_mday = d; t.tm_mon = i; t.tm_year = y - 1900; t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
                g_node_epoch = (unsigned long)timegm(&t); g_node_epoch_at = (unsigned long)time(NULL);
            }
        break;
    }
    int chunked = 0; for (char *h = buf; *h; h++) if (!strncasecmp(h, "transfer-encoding: chunked", 26)) { chunked = 1; break; }
    if (chunked) { size_t cn; char *d = dechunk(b, bn, &cn); *out = d; }
    else { *out = malloc(bn + 1); memcpy(*out, b, bn); (*out)[bn] = 0; }
    free(buf);
    if (status == 401 || status == 403) snprintf(g_err, sizeof g_err, "session expired or refused (%d): sign in again on the Mac", status);
    else if (status >= 400) snprintf(g_err, sizeof g_err, "the home-node answered %d", status);
    return status;
}

static void json_escape(const char *s, char *out, size_t n);
static void ascii_clean(char *s);
// ---------------------------------------------------------------- the phone signs itself in (renews its session)

static long jwt_exp(const char *tok) {                          // the middle part of a token is base64url JSON with "exp" (seconds since 1970)
    const char *d1 = strchr(tok, '.'); if (!d1) return 0;
    const char *d2 = strchr(d1 + 1, '.'); if (!d2) return 0;
    size_t n = (size_t)(d2 - d1 - 1); if (n == 0 || n > 600) return 0;
    char tmp[640]; for (size_t i = 0; i < n; i++) { char ch = d1[1 + i]; tmp[i] = ch == '-' ? '+' : (ch == '_' ? '/' : ch); } tmp[n] = 0;
    unsigned char out[480]; size_t on = lh_b64_decode(tmp, n, out); out[on < 479 ? on : 479] = 0;
    const char *e = strstr((char *)out, "\"exp\":"); return e ? atol(e + 6) : 0;
}

static void save_session(void) {                                 // write session.json back, so a renewed token survives a restart
    char path[300], tmp[310]; snprintf(path, sizeof path, "%s/session.json", g_dir); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "{\"home\":\"%s\",\"signaling\":\"https://%s%s\",\"signalingIp\":\"%s\",\"token\":\"%s\",\"refreshSecret\":\"%s\",\"personId\":\"%s\",\"homeId\":\"%s\",\"displayName\":\"%s\"}\n",
            g_cfg.home, g_cfg.sig_host, g_cfg.sig_path, g_cfg.sig_ip, g_cfg.token, g_cfg.refresh, g_cfg.person, g_cfg.home_id, g_cfg.name);
    fclose(f); chmod(tmp, 0600);
#ifdef __ANDROID__
    syscall(__NR_rename, tmp, path);
#else
    rename(tmp, path);
#endif
}

static long g_exp;                                                 // when the current token ends
static int refresh_token(void) {                                   // 0 on success
    if (!g_cfg.refresh[0] || !g_cfg.sig_host[0]) { snprintf(g_err, sizeof g_err, "session ended: sign in again on the Mac"); return -1; }
    char path[200], body[400], *out = NULL;
    snprintf(path, sizeof path, "%s/session/refresh", g_cfg.sig_path);
    snprintf(body, sizeof body, "{\"kind\":\"person\",\"id\":\"%s\",\"refreshSecret\":\"%s\"}", g_cfg.person, g_cfg.refresh);
    int st = lh_https(g_cfg.sig_host, "POST", path, NULL, body, tls_epoch(), &out);
    if (st != 200) {
        if (!st) snprintf(g_err, sizeof g_err, "renewing the session failed: %s", lh_err);
        else if (st == 401 || st == 403) snprintf(g_err, sizeof g_err, "the seat was revoked or replaced: sign in again on the Mac");
        else snprintf(g_err, sizeof g_err, "renewing the session failed (%d)", st);
        free(out); return -1;
    }
    JN *j = jparse(out); free(out);
    const char *tok = jstr(jget(j, "token"));
    if (!tok[0]) { jfree(j); snprintf(g_err, sizeof g_err, "renewing the session: unexpected answer"); return -1; }
    pthread_mutex_lock(&g_cfg_lock); snprintf(g_cfg.token, sizeof g_cfg.token, "%s", tok); pthread_mutex_unlock(&g_cfg_lock);
    JN *ex = jget(j, "exp"); g_exp = ex && ex->t == J_NUM ? (long)ex->n : jwt_exp(tok);
    jfree(j); save_session(); return 0;
}

static int token_needs_renewal(void) { long now = (long)tls_epoch(); return g_exp && g_exp - now < 3600; }     // less than an hour left


static int http_do(const char *method, const char *path, const char *body, char **out) {
    if (token_needs_renewal()) refresh_token();                  // under an hour left: renew now (a failure is retried below if the home refuses)
    int st = http_do_raw(method, path, body, out);
    if ((st == 401 || st == 403) && g_cfg.refresh[0]) { free(*out); *out = NULL; if (refresh_token() == 0) st = http_do_raw(method, path, body, out); }
    return st;
}

// ---------------------------------------------------------------- data

typedef struct { char id[96]; char kind[8]; char title[100]; char members[16][96]; int nmem; } Conv;
typedef struct { char id[96]; char sender[96]; char *body; char when[24]; char iso[32]; int deleted, edited, picture, has_thumb; } Msg;
#define MAXCONV 64
#define MAXMSG 200
static Conv g_conv[MAXCONV]; static int g_nconv;
static Msg g_msg[MAXMSG]; static int g_nmsg;
typedef struct { char id[96]; char name[64]; } Name;
static Name g_names[256]; static int g_nnames;

static const char *name_of(const char *id) {
    pthread_mutex_lock(&g_cfg_lock);
    for (int i = 0; i < g_nnames; i++) if (!strcmp(g_names[i].id, id)) { static char found[64]; snprintf(found, sizeof found, "%s", g_names[i].name); pthread_mutex_unlock(&g_cfg_lock); return found; }
    pthread_mutex_unlock(&g_cfg_lock);
    static char tmp[16]; snprintf(tmp, sizeof tmp, "%.8s", id); return tmp;
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (2 << 20)) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1); n = (long)fread(b, 1, (size_t)n, f); b[n] = 0; fclose(f); return b;
}

static void load_config(const char *dir) {
    char path[300]; snprintf(path, sizeof path, "%s/session.json", dir);
    char *t = read_all(path); JN *j = t ? jparse(t) : NULL; free(t);
    g_have_cfg = 0;
    if (j) {
        snprintf(g_cfg.token, sizeof g_cfg.token, "%s", jstr(jget(j, "token")));
        snprintf(g_cfg.person, sizeof g_cfg.person, "%s", jstr(jget(j, "personId")));
        snprintf(g_cfg.name, sizeof g_cfg.name, "%s", jstr(jget(j, "displayName")));
        snprintf(g_cfg.home, sizeof g_cfg.home, "%s", jstr(jget(j, "home")));
        snprintf(g_cfg.home_id, sizeof g_cfg.home_id, "%s", jstr(jget(j, "homeId")));
        snprintf(g_cfg.refresh, sizeof g_cfg.refresh, "%s", jstr(jget(j, "refreshSecret")));
        const char *sig = jstr(jget(j, "signaling")); if (!sig[0]) sig = "https://longhouse.tech/longhouse";      // "https://host/path"
        g_cfg.sig_host[0] = 0; g_cfg.sig_path[0] = 0;
        snprintf(g_cfg.sig_ip, sizeof g_cfg.sig_ip, "%s", jstr(jget(j, "signalingIp")));
        snprintf(lh_ip_override, sizeof lh_ip_override, "%s", g_cfg.sig_ip);
        if (!strncmp(sig, "https://", 8)) {
            const char *h = sig + 8, *sl = strchr(h, '/'); size_t hl = sl ? (size_t)(sl - h) : strlen(h);
            if (hl < sizeof g_cfg.sig_host) { memcpy(g_cfg.sig_host, h, hl); g_cfg.sig_host[hl] = 0; }
            snprintf(g_cfg.sig_path, sizeof g_cfg.sig_path, "%s", sl ? sl : "");
        }
        g_have_cfg = g_cfg.token[0] && g_cfg.person[0] && parse_home(g_cfg.home, &g_cfg) == 0;
        jfree(j);
    }
    snprintf(path, sizeof path, "%s/directory.json", dir);
    t = read_all(path); j = t ? jparse(t) : NULL; free(t);
    g_nnames = 0;
    JN *names = jget(j, "names");
    if (names && names->t == J_OBJ) for (int i = 0; i < names->len && g_nnames < 256; i++) {
        snprintf(g_names[g_nnames].id, sizeof g_names[0].id, "%s", names->k[i]);
        snprintf(g_names[g_nnames].name, sizeof g_names[0].name, "%s", jstr(names->v[i])); g_nnames++;
    }
    jfree(j);
}

static void ascii_clean(char *s) {                                  // the font is ASCII only: one "?" for each non-ASCII character
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        if (*r < 0x80) { if (*r == '\r') { r++; continue; } *w++ = *r == '\t' ? ' ' : *r; r++; }
        else { *w++ = '?'; r++; while ((*r & 0xC0) == 0x80) r++; }
    }
    *w = 0;
}

static int fetch_conversations(void) {
    char *body; int st = http_do("GET", "/conversations", NULL, &body);
    if (st != 200) { free(body); return -1; }
    JN *j = jparse(body); free(body);
    JN *arr = jget(j, "conversations");
    if (!arr || arr->t != J_ARR) { jfree(j); snprintf(g_err, sizeof g_err, "unexpected answer"); return -1; }
    g_nconv = 0;
    for (int i = 0; i < arr->len && g_nconv < MAXCONV; i++) {
        JN *c = arr->v[i]; Conv *o = &g_conv[g_nconv];
        snprintf(o->id, sizeof o->id, "%s", jstr(jget(c, "id"))); snprintf(o->kind, sizeof o->kind, "%s", jstr(jget(c, "kind")));
        JN *mem = jget(c, "memberIds"); o->nmem = 0;
        if (mem && mem->t == J_ARR) for (int m = 0; m < mem->len && o->nmem < 16; m++) snprintf(o->members[o->nmem++], 96, "%s", jstr(mem->v[m]));
        const char *title = jstr(jget(c, "title"));
        if (title[0]) snprintf(o->title, sizeof o->title, "%s", title);
        else {                                                         // a DM has no title: show the other person
            o->title[0] = 0;
            for (int m = 0; m < o->nmem; m++) if (strcmp(o->members[m], g_cfg.person)) {
                size_t l = strlen(o->title); snprintf(o->title + l, sizeof o->title - l, "%s%s", l ? ", " : "", name_of(o->members[m]));
            }
            if (!o->title[0]) snprintf(o->title, sizeof o->title, "(chat)");
        }
        ascii_clean(o->title); g_nconv++;
    }
    jfree(j); return 0;
}

static void free_messages(void) { for (int i = 0; i < g_nmsg; i++) free(g_msg[i].body); g_nmsg = 0; }

static void format_time(const char *iso, char *out, size_t n) {       // "2026-10-11T05:35:23.385Z" (UTC) -> local "14:03" or "10-10 14:03"
    struct tm t; memset(&t, 0, sizeof t);
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) { snprintf(out, n, "?"); return; }
    t.tm_year -= 1900; t.tm_mon -= 1;
    time_t ts = timegm(&t); struct tm lt; localtime_r(&ts, &lt);
    time_t nowt = time(NULL); struct tm ln; localtime_r(&nowt, &ln);
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) strftime(out, n, "%H:%M", &lt); else strftime(out, n, "%m-%d %H:%M", &lt);
}

static int msg_cmp(const void *a, const void *b) { return strcmp(((const Msg *)a)->iso, ((const Msg *)b)->iso); }

static int fetch_messages(const char *conv_id) {
    char path[200]; snprintf(path, sizeof path, "/conversations/%s/messages", conv_id);
    char *body; int st = http_do("GET", path, NULL, &body);
    if (st != 200) { free(body); return -1; }
    JN *j = jparse(body); free(body);
    JN *arr = jget(j, "messages");
    if (!arr || arr->t != J_ARR) { jfree(j); snprintf(g_err, sizeof g_err, "unexpected answer"); return -1; }
    free_messages();
    int start = arr->len > MAXMSG ? arr->len - MAXMSG : 0;                // keep the newest messages
    for (int i = start; i < arr->len; i++) {
        JN *m = arr->v[i]; Msg *o = &g_msg[g_nmsg];
        snprintf(o->id, sizeof o->id, "%s", jstr(jget(m, "id"))); snprintf(o->sender, sizeof o->sender, "%s", jstr(jget(m, "senderId")));
        snprintf(o->iso, sizeof o->iso, "%s", jstr(jget(m, "createdAt"))); format_time(o->iso, o->when, sizeof o->when);
        JN *del = jget(m, "deletedAt"), *ed = jget(m, "editedAt"), *media = jget(m, "media"), *ht = jget(m, "hasThumb");
        o->deleted = del && del->t == J_STR; o->edited = ed && ed->t == J_STR;
        o->has_thumb = ht && ht->t == J_BOOL && ht->n != 0;
        o->picture = o->has_thumb || (media && media->t == J_OBJ);
        const char *b = o->deleted ? "(deleted)" : jstr(jget(m, "body"));
        size_t bl = strlen(b); o->body = malloc(bl + 24); strcpy(o->body, b);
        if (o->picture && !o->deleted) {
            JN *k = media && media->t == J_OBJ ? jget(media, "kind") : NULL;
            strcat(o->body, bl ? "\n" : ""); strcat(o->body, k && !strcmp(jstr(k), "video") ? "[video]" : (k && !strcmp(jstr(k), "file") ? "[file]" : "[picture]"));
        }
        ascii_clean(o->body); g_nmsg++;
    }
    jfree(j);
    qsort(g_msg, (size_t)g_nmsg, sizeof g_msg[0], msg_cmp);
    return 0;
}

static void json_escape(const char *s, char *out, size_t n) {
    size_t o = 0;
    for (; *s && o + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32 || c >= 127) { out[o++] = ' '; }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static int send_message(const char *conv_id, const char *text) {
    char path[200]; snprintf(path, sizeof path, "/conversations/%s/messages", conv_id);
    char esc[8400], body[8600]; json_escape(text, esc, sizeof esc);
    snprintf(body, sizeof body, "{\"body\":\"%s\"}", esc);
    char *out; int st = http_do("POST", path, body, &out); free(out);
    return st == 200 ? 0 : -1;
}

static int refresh_roster(void) {                                  // fetch the people's names from the signaling server and keep them in directory.json
    char path[200], *out = NULL; snprintf(path, sizeof path, "%s/roster", g_cfg.sig_path);
    char tok[1024]; pthread_mutex_lock(&g_cfg_lock); snprintf(tok, sizeof tok, "%s", g_cfg.token); pthread_mutex_unlock(&g_cfg_lock);
    int st = lh_https(g_cfg.sig_host, "GET", path, tok, NULL, tls_epoch(), &out);
    if (st != 200) { free(out); return -1; }
    JN *j = jparse(out); free(out);
    JN *persons = jget(j, "persons"); if (!persons || persons->t != J_ARR) { jfree(j); return -1; }
    pthread_mutex_lock(&g_cfg_lock);
    g_nnames = 0;
    for (int i = 0; i < persons->len && g_nnames < 256; i++) {
        snprintf(g_names[g_nnames].id, sizeof g_names[0].id, "%s", jstr(jget(persons->v[i], "id")));
        snprintf(g_names[g_nnames].name, sizeof g_names[0].name, "%s", jstr(jget(persons->v[i], "displayName")));
        ascii_clean(g_names[g_nnames].name); g_nnames++;
    }
    pthread_mutex_unlock(&g_cfg_lock);
    jfree(j);
    char dpath[300], tmp[310]; snprintf(dpath, sizeof dpath, "%s/directory.json", g_dir); snprintf(tmp, sizeof tmp, "%s.tmp", dpath);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs("{\"names\":{", f);
        for (int i = 0; i < g_nnames; i++) { char en[140]; json_escape(g_names[i].name, en, sizeof en); fprintf(f, "%s\"%s\":\"%s\"", i ? "," : "", g_names[i].id, en); }
        fputs("}}\n", f); fclose(f);
#ifdef __ANDROID__
        syscall(__NR_rename, tmp, dpath);
#else
        rename(tmp, dpath);
#endif
    }
    return 0;
}

// Runs once at start-up in a thread: learn the home-node's clock, renew the session if it is old, refresh the names.
static void *boot_worker(void *arg) {
    (void)arg;
    char *o = NULL; http_do_raw("GET", "/health", NULL, &o); free(o);     // the Date header gives a good clock for the certificate check
    if (!g_exp || token_needs_renewal()) refresh_token();
    refresh_roster();
    return NULL;
}

// ---------------------------------------------------------------- pictures (thumbnails) in a background thread

#define MAXPIC 12
#define PIC_MAX 260                                              // a thumbnail is shown at most this many pixels on its long edge
typedef struct { char id[96]; int state; unsigned char *rgb; int w, h; char err[80]; } Pic;      // state: 1 waiting, 2 ready, 3 failed
static Pic g_pic[MAXPIC]; static int g_picn;
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pc = PTHREAD_COND_INITIALIZER;
static volatile int g_pic_quit;

static Pic *pic_find(const char *id) { for (int i = 0; i < g_picn; i++) if (!strcmp(g_pic[i].id, id)) return &g_pic[i]; return NULL; }

static void pic_request(const char *id) {                       // main thread: ask for a thumbnail once
    pthread_mutex_lock(&g_pm);
    if (!pic_find(id)) {
        if (g_picn == MAXPIC) { free(g_pic[0].rgb); memmove(&g_pic[0], &g_pic[1], (MAXPIC - 1) * sizeof g_pic[0]); g_picn--; }   // forget the oldest
        Pic *p = &g_pic[g_picn++]; memset(p, 0, sizeof *p); snprintf(p->id, sizeof p->id, "%s", id); p->state = 1;
        pthread_cond_signal(&g_pc);
    }
    pthread_mutex_unlock(&g_pm);
}

static void *pic_worker(void *arg) {
    (void)arg;
    LhConn *c = malloc(sizeof *c); memset(c, 0, sizeof *c); c->fd = -1;
    int rpc = 0; time_t last_use = 0;
    while (!g_pic_quit) {
        char id[96] = "";
        pthread_mutex_lock(&g_pm);
        for (;;) {
            for (int i = g_picn - 1; i >= 0; i--) if (g_pic[i].state == 1) { snprintf(id, sizeof id, "%s", g_pic[i].id); break; }   // newest first
            if (id[0] || g_pic_quit) break;
            struct timeval tv; gettimeofday(&tv, NULL); struct timespec ts = { tv.tv_sec + 5, tv.tv_usec * 1000 };       // clock_gettime does not exist on iOS 7
            pthread_cond_timedwait(&g_pc, &g_pm, &ts);
            if (c->open && time(NULL) - last_use > 45) { lh_close(c); }                             // idle: let go of the connection
        }
        pthread_mutex_unlock(&g_pm);
        if (g_pic_quit) break;
        unsigned char *jpg = NULL; size_t jl = 0; char err[80] = "";
        for (int attempt = 0; attempt < 2 && !jpg; attempt++) {                                    // a second try reconnects
            if (!c->open) {
                lh_err[0] = 0;
                if (lh_tls_open(c, g_cfg.sig_host, 443, tls_epoch())) { snprintf(err, sizeof err, "%s", lh_err); break; }
                char tok[1024]; pthread_mutex_lock(&g_cfg_lock); snprintf(tok, sizeof tok, "%s", g_cfg.token); pthread_mutex_unlock(&g_cfg_lock);
                char path[1300]; snprintf(path, sizeof path, "%s/ws?token=%s", g_cfg.sig_path, tok);
                if (lh_ws_open(c, g_cfg.sig_host, path)) {
                    snprintf(err, sizeof err, "%s", lh_err); lh_close(c);
                    if (strstr(err, " 401") && attempt == 0 && refresh_token() == 0) continue;                // the token ended: renew and try again
                    break;
                }
            }
            jpg = lh_get_thumbnail(c, g_cfg.home_id, id, ++rpc, &jl);
            if (!jpg) { snprintf(err, sizeof err, "%s", lh_err); lh_close(c); }
        }
        last_use = time(NULL);
        unsigned char *rgb = NULL; int w = 0, h = 0;
        if (jpg) {
            int iw, ih, ch; unsigned char *full = stbi_load_from_memory(jpg, (int)jl, &iw, &ih, &ch, 3); free(jpg);
            if (full) {                                                                              // scale down (nearest) to at most PIC_MAX
                int lg = iw > ih ? iw : ih; w = iw * (lg > PIC_MAX ? PIC_MAX : lg) / lg; h = ih * (lg > PIC_MAX ? PIC_MAX : lg) / lg; if (w < 1) w = 1; if (h < 1) h = 1;
                rgb = malloc((size_t)w * h * 3);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) memcpy(rgb + ((size_t)y * w + x) * 3, full + ((size_t)(y * ih / h) * iw + (x * iw / w)) * 3, 3);
                stbi_image_free(full);
            } else snprintf(err, sizeof err, "cannot decode the picture");
        }
        pthread_mutex_lock(&g_pm);
        Pic *p = pic_find(id);
        if (p) { if (rgb) { p->rgb = rgb; p->w = w; p->h = h; p->state = 2; rgb = NULL; } else { p->state = 3; snprintf(p->err, sizeof p->err, "%s", err[0] ? err : "failed"); } }
        pthread_mutex_unlock(&g_pm);
        free(rgb);
    }
    lh_close(c); free(c); return NULL;
}

static void blit_rgb(Surf *s, int x0, int y0, const unsigned char *rgb, int w, int h, int clip_y0, int clip_y1) {
    for (int y = 0; y < h; y++) {
        int sy = y0 + y; if (sy < clip_y0 || sy >= clip_y1 || sy < 0 || sy >= s->h) continue;
        uint32_t *row = s->px + (size_t)sy * s->stride;
        for (int x = 0; x < w; x++) {
            int sx = x0 + x; if (sx < 0 || sx >= s->w) continue;
            const unsigned char *q = rgb + ((size_t)y * w + x) * 3;
            row[sx] = ((uint32_t)q[0] << s->rs) | ((uint32_t)q[1] << s->gs) | ((uint32_t)q[2] << s->bs) | s->alpha;
        }
    }
}

// ---------------------------------------------------------------- text layout

static int wrap_lines(const char *text, int cols, int *starts, int *lens, int max) {   // word wrap; returns the number of lines
    int n = 0; const char *p = text;
    while (n < max) {
        const char *e = p; while (*e && *e != '\n') e++;
        int len = (int)(e - p);
        if (len <= cols) { starts[n] = (int)(p - text); lens[n] = len; n++; }
        else {
            int cut = cols;
            for (int i = cols; i > cols / 2; i--) if (p[i] == ' ') { cut = i; break; }
            starts[n] = (int)(p - text); lens[n] = cut; n++;
            p += cut; while (*p == ' ') p++;
            continue;
        }
        if (!*e) break;
        p = e + 1;
    }
    return n ? n : 1;
}

static int pic_extra(const Msg *m) {                              // extra height in a message bubble for its picture
    if (!m->has_thumb || m->deleted) return 0;
    pthread_mutex_lock(&g_pm); Pic *p = pic_find(m->id); int h = !p ? 0 : (p->state == 2 ? p->h + 8 : (p->state == 3 ? 22 : 0)); pthread_mutex_unlock(&g_pm);
    return h;
}
static void queue_pics(void) { int n = 0; for (int i = g_nmsg - 1; i >= 0 && n < 8; i--) if (g_msg[i].has_thumb && !g_msg[i].deleted) { pic_request(g_msg[i].id); n++; } }

enum { V_LIST, V_CHAT };

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    char dir[256] = DEFAULT_DIR; if (argc > 2) snprintf(dir, sizeof dir, "%s", argv[2]);
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    load_config(dir); g_exp = jwt_exp(g_cfg.token);
    signal(SIGPIPE, SIG_IGN);
    if (g_have_cfg && g_cfg.sig_host[0]) { pthread_t bt; if (pthread_create(&bt, NULL, boot_worker, NULL) == 0) pthread_detach(bt); }
    if (g_have_cfg && g_cfg.sig_host[0] && g_cfg.home_id[0]) { pthread_t th; if (pthread_create(&th, NULL, pic_worker, NULL) == 0) pthread_detach(th); }

    int key_h = W / 8, kbd_h = key_h * 5 + 6, kb_y = H - kbd_h;
    Kbd kb; kbd_init(&kb, 0, kb_y, W, kbd_h);
    Ui u; memset(&u, 0, sizeof u);
    UiEdit ed; ui_edit_init(&ed, 4000);
    UiScroll list_sc = {0}, chat_sc = {0};
    int view = V_LIST, sel = -1, dirty = 3, quit = 0, follow = 1, blink = 1;
    char status[220] = "";
    const int BAR = 64, IN_H = 96, ROW = 84, SC = 2, CW = 8 * SC, LH = 16 * SC + 4;
    double t0 = ui_now(), t_blink = t0, t_fetch = 0;

    if (g_have_cfg) { if (fetch_conversations() == 0) snprintf(status, sizeof status, "%d chats", g_nconv); else snprintf(status, sizeof status, "%s", g_err); }
    else snprintf(status, sizeof status, "%s", g_err[0] ? g_err : "no session: run tools/longhouse_login.py on the Mac");
    t_fetch = ui_now();

    while (!quit && ui_now() - t0 < seconds) {
        double t = ui_now();
        PlatTouch tc[10]; int n = plat_touch_poll(tc, 10);
        ui_poll(&u, tc, n);
        if (view == V_CHAT) {
            int keys[16], nk = ui_keyboard(&u, &kb, tc, n, keys, 16);
            for (int i = 0; i < nk; i++) { if (keys[i] == '\r' || keys[i] == '\n') ui_edit_key(&ed, ' '); else ui_edit_key(&ed, keys[i]); }
            if (nk) { dirty = 3; blink = 1; t_blink = t; }
            if (kb.dirty) { dirty = 3; kb.dirty = 0; }
        }
        // refresh in the background: the chat list every 12 s, the open chat every 4 s, but never while a finger is down
        if (g_have_cfg && !u.down && ((view == V_CHAT && t - t_fetch > 4) || (view == V_LIST && t - t_fetch > 12))) {
            int before = g_nmsg;
            int rc = view == V_CHAT ? fetch_messages(g_conv[sel].id) : fetch_conversations();
            snprintf(status, sizeof status, "%s", rc == 0 ? (view == V_CHAT ? "" : "") : g_err);
            if (view == V_CHAT && g_nmsg != before && follow) chat_sc.pos = 1 << 28;
            t_fetch = ui_now(); dirty = 3;
        }
        if (t - t_blink >= 0.5) { blink ^= 1; t_blink = t; if (view == V_CHAT) dirty = 2; }
        { static int last_n = -1; static int last_view = -1;
          if (view == V_CHAT && (g_nmsg != last_n || last_view != view)) { queue_pics(); dirty = 3; }
          last_n = g_nmsg; last_view = view; }
        if (u.changed || u.tapped) dirty = 3;
        if (dirty <= 0) { usleep(10000); continue; }
        dirty--;

        int stride; uint32_t *px = plat_begin(&stride);
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
        u.s = &s; u.W = W; u.H = H;
        draw_rect(&s, 0, 0, W, H, 0x10161E);
        draw_rect(&s, 0, 0, W, BAR, 0x26303E);

        if (view == V_LIST) {
            ui_label(&u, 14, 16, 2, "LONGHOUSE", 0xFFFFFF);
            if (ui_button(&u, W - 250, 8, 112, BAR - 16, "SYNC", 0x34608F)) { load_config(dir); if (g_have_cfg && fetch_conversations() == 0) snprintf(status, sizeof status, "%d chats", g_nconv); else snprintf(status, sizeof status, "%s", g_err); t_fetch = ui_now(); }
            if (ui_button(&u, W - 130, 8, 122, BAR - 16, "EXIT", 0x883333)) quit = 1;
            int ly = BAR, lh = H - BAR - 40;
            ui_scroll(&u, &list_sc, 0, ly, W, lh, g_nconv * ROW);
            for (int i = 0; i < g_nconv; i++) {
                int y = ly + i * ROW - list_sc.pos; if (y + ROW <= ly || y >= ly + lh) continue;
                draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                char buf[128]; ui_fit(buf, sizeof buf, g_conv[i].title, (W - 32) / CW);
                ui_label(&u, 14, y + 12, 2, buf, 0xF0F0F0);
                snprintf(buf, sizeof buf, "%s, %d people", strcmp(g_conv[i].kind, "dm") == 0 ? "direct message" : "group", g_conv[i].nmem);
                ui_label(&u, 14, y + 52, 1, buf, 0x8899AA);
                if (u.tapped && u.tap_y >= ly && u.tap_y < ly + lh && ui_inside(u.tap_x, u.tap_y, 0, y, W, ROW)) {
                    sel = i; view = V_CHAT; follow = 1; chat_sc.pos = 1 << 28; ui_edit_set(&ed, ""); free_messages();
                    snprintf(status, sizeof status, "loading..."); fetch_messages(g_conv[sel].id); snprintf(status, sizeof status, "%s", g_err);
                    t_fetch = ui_now(); u.tapped = 0; dirty = 3; break;
                }
            }
            if (!g_nconv && g_have_cfg) ui_label(&u, 14, ly + 24, 2, "No chats yet.", 0x8899AA);
        } else {
            char title[128]; ui_fit(title, sizeof title, g_conv[sel].title, (W - 260) / CW);
            if (ui_button(&u, 8, 8, 112, BAR - 16, "BACK", 0x34608F)) { view = V_LIST; t_fetch = 0; free_messages(); dirty = 3; }
            ui_label(&u, 134, 16, 2, title, 0xFFFFFF);
            if (ui_button(&u, W - 120, 8, 112, BAR - 16, "SYNC", 0x34608F)) { fetch_messages(g_conv[sel].id); snprintf(status, sizeof status, "%s", g_err); t_fetch = ui_now(); follow = 1; chat_sc.pos = 1 << 28; }

            // the message area, between the header and the input box
            int my = BAR, mh = kb_y - IN_H - BAR, cols = (W - 24) / CW;
            int total = 12;
            static int st[64], ln[64];
            for (int i = 0; i < g_nmsg; i++) total += LH + wrap_lines(g_msg[i].body, cols, st, ln, 64) * LH + 12 + pic_extra(&g_msg[i]);
            int maxs = total - mh; if (maxs < 0) maxs = 0;
            if (follow && chat_sc.pos > maxs) chat_sc.pos = maxs;
            int before = chat_sc.pos;
            ui_scroll(&u, &chat_sc, 0, my, W, mh, total);
            if (chat_sc.pos < before - 4) follow = 0; else if (chat_sc.pos >= maxs) follow = 1;
            int y = my + 8 - chat_sc.pos;
            for (int i = 0; i < g_nmsg; i++) {
                int nl = wrap_lines(g_msg[i].body, cols, st, ln, 64), ex = pic_extra(&g_msg[i]), h = LH + nl * LH + 12 + ex;
                if (y + h > my && y < my + mh) {
                    int mine = !strcmp(g_msg[i].sender, g_cfg.person);
                    int r0 = y < my ? my : y, r1 = y + h - 6 > my + mh ? my + mh : y + h - 6;           // clip the bubble to the message area, so it never covers the header
                    if (r1 > r0) draw_rect(&s, 4, r0, W - 8, r1 - r0, mine ? 0x1D3A2C : 0x1A2230);
                    char head[160]; snprintf(head, sizeof head, "%s  %s%s", name_of(g_msg[i].sender), g_msg[i].when, g_msg[i].edited ? "  (edited)" : "");
                    if (y + 4 >= my && y + 4 + 16 <= my + mh) ui_label(&u, 12, y + 4, 1, head, mine ? 0x77DD99 : 0x77BBFF);
                    for (int l = 0; l < nl; l++) {
                        char line[300]; int ll = ln[l] < 299 ? ln[l] : 299; memcpy(line, g_msg[i].body + st[l], (size_t)ll); line[ll] = 0;
                        int ly2 = y + LH - 4 + l * LH;
                        if (ly2 >= my && ly2 + LH <= my + mh) ui_label(&u, 12, ly2, SC, line, g_msg[i].deleted ? 0x778899 : 0xE8E8E8);       // whole lines only
                    }
                    if (ex) {
                        int iy2 = y + LH - 4 + nl * LH + 4;
                        pthread_mutex_lock(&g_pm); Pic *pp = pic_find(g_msg[i].id);
                        if (pp && pp->state == 2) blit_rgb(&s, 12, iy2, pp->rgb, pp->w, pp->h, my, my + mh);
                        else if (pp && pp->state == 3 && iy2 >= my && iy2 + 16 <= my + mh) { char eb[100]; snprintf(eb, sizeof eb, "(picture: %.70s)", pp->err); ui_label(&u, 12, iy2, 1, eb, 0xFFAA66); }
                        pthread_mutex_unlock(&g_pm);
                    }
                }
                y += h;
            }
            if (!g_nmsg) ui_label(&u, 14, my + 20, 2, g_have_cfg ? "No messages yet." : "No session.", 0x8899AA);
            draw_rect(&s, 0, my - 1, W, 1, 0x26303E);

            // the input box and SEND
            int iy = kb_y - IN_H;
            draw_rect(&s, 0, iy, W, IN_H, 0x1B2330);
            ui_edit_draw(&u, &ed, 6, iy + 6, W - 130, IN_H - 12, SC, blink);
            if (ui_button(&u, W - 118, iy + 10, 110, IN_H - 20, "SEND", 0x2E7D4F) && ed.len > 0 && g_have_cfg) {
                char *text = strdup(ed.buf); size_t tl = strlen(text); while (tl && text[tl - 1] == ' ') text[--tl] = 0;
                if (tl) {
                    if (send_message(g_conv[sel].id, text) == 0) { ui_edit_set(&ed, ""); fetch_messages(g_conv[sel].id); follow = 1; chat_sc.pos = 1 << 28; snprintf(status, sizeof status, "sent"); }
                    else snprintf(status, sizeof status, "not sent: %s", g_err);
                    t_fetch = ui_now();
                }
                free(text);
            }
            kbd_render(&kb, &s);
        }
        if (status[0] && view == V_LIST) { draw_rect(&s, 0, H - 38, W, 38, 0x1B2330); char sb[120]; ui_fit(sb, sizeof sb, status, (W - 16) / 8); ui_label(&u, 8, H - 28, 1, sb, strstr(status, "chats") ? 0x8899AA : 0xFFAA66); }
        else if (status[0] && view == V_CHAT && strcmp(status, "sent") && strcmp(status, "loading...")) { draw_rect(&s, 0, BAR, W, 26, 0x4A2A2A); char sb[120]; ui_fit(sb, sizeof sb, status, (W - 16) / 8); ui_label(&u, 8, BAR + 5, 1, sb, 0xFFCC99); }
        plat_flip();
    }
    g_pic_quit = 1; pthread_mutex_lock(&g_pm); pthread_cond_broadcast(&g_pc); pthread_mutex_unlock(&g_pm);
    free_messages();
    plat_close();
    return 0;
}
