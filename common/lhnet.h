// lhnet.h - HTTPS (TLS 1.2 through BearSSL) and WebSocket for the Longhouse chat app, and the thumbnail call.
//
// Longhouse sends the thumbnail of a picture through the signaling server: the app opens wss://<host><path>/ws?token=<token>, sends
//   {"type":"home-rpc","id":"rpc_1","homeId":"home_...","method":"getMessageThumbnail","payload":{"messageId":"msg_..."}}
// and reads {"type":"home-rpc-result","id":"rpc_1","result":{"messageId":"...","thumbnail":"<base64 JPEG>"}}.
// The server certificate is checked against the two Let's Encrypt roots in lh_trust.h. TLS needs the right date to check the
// certificate dates, and a phone clock is often wrong, so the caller passes the date of the home-node (its HTTP "Date" header).
// Needs: bearssl.h (common/third_party/bearssl/inc), libbearssl.a for the platform (common/build_bearssl.sh), lh_trust.h.
#ifndef LHNET_H
#define LHNET_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "bearssl.h"
#include "lh_trust.h"

typedef struct {
    int fd;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context ioc;
    int open;
} LhConn;

static char lh_err[200];
static char lh_ip_override[48];                    // an IPv4 address to connect to instead of looking the name up (the certificate is still checked for the name)

static void lh_random(void *buf, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t r = fread(buf, 1, n, f); fclose(f); if (r == n) return; }
    for (size_t i = 0; i < n; i++) ((unsigned char *)buf)[i] = (unsigned char)(rand() >> 4);
}

static int lh_tcp(const char *host, int port) {
    struct sockaddr_in sa4; struct sockaddr *addr; socklen_t alen; struct addrinfo hints, *res = NULL;
    memset(&sa4, 0, sizeof sa4); sa4.sin_family = AF_INET; sa4.sin_port = htons((uint16_t)port);
    if (lh_ip_override[0] && inet_pton(AF_INET, lh_ip_override, &sa4.sin_addr) == 1) { addr = (struct sockaddr *)&sa4; alen = sizeof sa4; }
    else {                                                        // a static program on old Android often cannot look names up: see lh_ip_override
        memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
        char ps[16]; snprintf(ps, sizeof ps, "%d", port);
        if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) { snprintf(lh_err, sizeof lh_err, "cannot find %s (no DNS; sign in again from the Mac)", host); return -1; }
        addr = res->ai_addr; alen = res->ai_addrlen;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { if (res) freeaddrinfo(res); return -1; }
    int fl = fcntl(fd, F_GETFL); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, addr, alen);
    if (res) freeaddrinfo(res);
    if (r < 0 && errno == EINPROGRESS) {
        fd_set w; FD_ZERO(&w); FD_SET(fd, &w); struct timeval tv = { 8, 0 };
        if (select(fd + 1, NULL, &w, NULL, &tv) <= 0) { close(fd); snprintf(lh_err, sizeof lh_err, "cannot reach %s", host); return -1; }
        int e = 0; socklen_t el = sizeof e; getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
        if (e) { close(fd); snprintf(lh_err, sizeof lh_err, "cannot reach %s (%s)", host, strerror(e)); return -1; }
    } else if (r < 0) { close(fd); snprintf(lh_err, sizeof lh_err, "connect: %s", strerror(errno)); return -1; }
    fcntl(fd, F_SETFL, fl);
    struct timeval tv = { 12, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static int lh_sock_read(void *ctx, unsigned char *buf, size_t len) {
    for (;;) { ssize_t r = recv(*(int *)ctx, buf, len, 0); if (r < 0 && errno == EINTR) continue; return r <= 0 ? -1 : (int)r; }
}
static int lh_sock_write(void *ctx, const unsigned char *buf, size_t len) {
    for (;;) { ssize_t r = send(*(int *)ctx, buf, len, 0); if (r < 0 && errno == EINTR) continue; return r <= 0 ? -1 : (int)r; }
}

static void lh_close(LhConn *c) { if (c) { if (c->open && c->fd >= 0) close(c->fd); c->open = 0; } }

// Open a TLS connection. epoch = the current time (seconds since 1970), for the certificate dates. Returns 0 on success.
static int lh_tls_open(LhConn *c, const char *host, int port, unsigned long epoch) {
    memset(c, 0, sizeof *c); c->fd = -1;
    c->fd = lh_tcp(host, port);
    if (c->fd < 0) return -1;
    c->open = 1;
    br_ssl_client_init_full(&c->sc, &c->xc, TAs, TAs_NUM);
    br_x509_minimal_set_time(&c->xc, (uint32_t)(epoch / 86400 + 719528), (uint32_t)(epoch % 86400));
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
    br_ssl_client_reset(&c->sc, host, 0);
    br_sslio_init(&c->ioc, &c->sc.eng, lh_sock_read, &c->fd, lh_sock_write, &c->fd);
    return 0;
}

static int lh_tls_fail(LhConn *c, const char *what) {
    int e = br_ssl_engine_last_error(&c->sc.eng);
    if (e == BR_ERR_X509_NOT_TRUSTED) snprintf(lh_err, sizeof lh_err, "%s: the server certificate is not trusted", what);
    else if (e >= BR_ERR_X509_OK && e < 64) snprintf(lh_err, sizeof lh_err, "%s: certificate problem (%d); is the phone date right?", what, e);
    else snprintf(lh_err, sizeof lh_err, "%s: TLS error %d", what, e);
    return -1;
}
static int lh_tls_write(LhConn *c, const void *p, size_t n) {
    if (br_sslio_write_all(&c->ioc, p, n) < 0) return lh_tls_fail(c, "send");
    return br_sslio_flush(&c->ioc) < 0 ? lh_tls_fail(c, "send") : 0;
}
static int lh_tls_read_exact(LhConn *c, void *p, size_t n) {
    unsigned char *b = p;
    while (n) { int r = br_sslio_read(&c->ioc, b, n); if (r < 0) return lh_tls_fail(c, "receive"); b += r; n -= (size_t)r; }
    return 0;
}

// ---------------------------------------------------------------- WebSocket (RFC 6455), client side

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void lh_b64_encode(const unsigned char *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64chars[v >> 18 & 63]; out[o++] = b64chars[v >> 12 & 63];
        out[o++] = i + 1 < n ? b64chars[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? b64chars[v & 63] : '=';
    }
    out[o] = 0;
}
// Decode base64 (ignores whitespace and stops at "="). Returns the byte count; out must hold at least len * 3 / 4 bytes.
static size_t lh_b64_decode(const char *in, size_t len, unsigned char *out) {
    static signed char map[256]; static int init;
    if (!init) { memset(map, -1, sizeof map); for (int i = 0; i < 64; i++) map[(unsigned char)b64chars[i]] = (signed char)i; init = 1; }
    size_t o = 0; unsigned acc = 0; int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = map[(unsigned char)in[i]];
        if (v < 0) { if (in[i] == '=') break; continue; }
        acc = acc << 6 | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(acc >> bits & 255); }
    }
    return o;
}

// Do the HTTP upgrade. path includes the query ("/longhouse/ws?token=..."). Returns 0 on success.
static int lh_ws_open(LhConn *c, const char *host, const char *path) {
    unsigned char kraw[16]; char key[40]; lh_random(kraw, sizeof kraw); lh_b64_encode(kraw, sizeof kraw, key);
    size_t cap = strlen(path) + 400; char *req = malloc(cap);
    int n = snprintf(req, cap, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                     "Sec-WebSocket-Version: 13\r\nUser-Agent: fbtools-longhouse\r\n\r\n", path, host, key);
    int rc = lh_tls_write(c, req, (size_t)n); free(req);
    if (rc) return -1;
    char head[4096]; size_t hl = 0;
    while (hl < sizeof head - 1) {
        if (lh_tls_read_exact(c, head + hl, 1)) return -1;
        hl++; head[hl] = 0;
        if (hl >= 4 && !memcmp(head + hl - 4, "\r\n\r\n", 4)) break;
    }
    if (strncmp(head, "HTTP/1.1 101", 12)) { snprintf(lh_err, sizeof lh_err, "websocket refused: %.40s", head); return -1; }
    return 0;
}

static int lh_ws_send_text(LhConn *c, const char *s, size_t n) {
    unsigned char hdr[14]; size_t h = 0;
    hdr[h++] = 0x81;
    if (n < 126) hdr[h++] = (unsigned char)(0x80 | n);
    else if (n < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (unsigned char)(n >> 8); hdr[h++] = (unsigned char)n; }
    else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((uint64_t)n >> (8 * i)); }
    unsigned char mask[4]; lh_random(mask, 4); memcpy(hdr + h, mask, 4); h += 4;
    unsigned char *buf = malloc(h + n); memcpy(buf, hdr, h);
    for (size_t i = 0; i < n; i++) buf[h + i] = (unsigned char)s[i] ^ mask[i & 3];
    int rc = lh_tls_write(c, buf, h + n); free(buf); return rc;
}

// Read the next complete text message. Answers pings, stops on close. *out is malloc'ed and NUL-terminated.
static int lh_ws_recv(LhConn *c, char **out, size_t *outn, size_t max) {
    char *msg = NULL; size_t mn = 0;
    for (;;) {
        unsigned char h[2]; if (lh_tls_read_exact(c, h, 2)) { free(msg); return -1; }
        int fin = h[0] >> 7, op = h[0] & 15, masked = h[1] >> 7; uint64_t len = h[1] & 127;
        if (len == 126) { unsigned char e[2]; if (lh_tls_read_exact(c, e, 2)) { free(msg); return -1; } len = (uint64_t)e[0] << 8 | e[1]; }
        else if (len == 127) { unsigned char e[8]; if (lh_tls_read_exact(c, e, 8)) { free(msg); return -1; } len = 0; for (int i = 0; i < 8; i++) len = len << 8 | e[i]; }
        unsigned char mk[4] = {0}; if (masked && lh_tls_read_exact(c, mk, 4)) { free(msg); return -1; }
        if (len > max || mn + len > max) { snprintf(lh_err, sizeof lh_err, "websocket message too big"); free(msg); return -1; }
        unsigned char *pl = malloc((size_t)len + 1);
        if (len && lh_tls_read_exact(c, pl, (size_t)len)) { free(pl); free(msg); return -1; }
        if (masked) for (size_t i = 0; i < len; i++) pl[i] ^= mk[i & 3];
        if (op == 8) { free(pl); free(msg); snprintf(lh_err, sizeof lh_err, "websocket closed by the server"); return -1; }
        if (op == 9) {                                                          // ping: answer with the same bytes
            unsigned char hdr[6 + 125]; size_t pn = (size_t)len > 125 ? 125 : (size_t)len; unsigned char m2[4]; lh_random(m2, 4);
            hdr[0] = 0x8A; hdr[1] = (unsigned char)(0x80 | pn); memcpy(hdr + 2, m2, 4); for (size_t i = 0; i < pn; i++) hdr[6 + i] = pl[i] ^ m2[i & 3];
            lh_tls_write(c, hdr, 6 + pn); free(pl); continue;
        }
        if (op == 10) { free(pl); continue; }                                   // pong
        msg = realloc(msg, mn + (size_t)len + 1); memcpy(msg + mn, pl, (size_t)len); mn += (size_t)len; msg[mn] = 0; free(pl);
        if (fin) { *out = msg; *outn = mn; return 0; }
    }
}

// Find a JSON string value for a key in a flat search, without a full parse: returns a pointer into text and the length.
// Good enough for {"result":{"thumbnail":"...."}} where the value has no escapes (base64).
static const char *lh_json_string_value(const char *text, const char *key, size_t *len) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(text, pat); if (!p) return NULL;
    p += strlen(pat); const char *e = p; while (*e && *e != '"') e++;
    *len = (size_t)(e - p); return p;
}

// Ask the home for the thumbnail of a message. conn must be an open WebSocket. Returns the JPEG bytes (malloc'ed) or NULL.
static unsigned char *lh_get_thumbnail(LhConn *c, const char *home_id, const char *message_id, int rpc_no, size_t *jpeg_len) {
    char req[600]; char id[24]; snprintf(id, sizeof id, "rpc_%d", rpc_no);
    int n = snprintf(req, sizeof req, "{\"type\":\"home-rpc\",\"id\":\"%s\",\"homeId\":\"%s\",\"method\":\"getMessageThumbnail\",\"payload\":{\"messageId\":\"%s\"}}", id, home_id, message_id);
    if (lh_ws_send_text(c, req, (size_t)n)) return NULL;
    for (int tries = 0; tries < 20; tries++) {                                  // the server also sends "hello", "presence", live events
        char *msg; size_t mn; if (lh_ws_recv(c, &msg, &mn, 600000)) return NULL;
        char want[48]; snprintf(want, sizeof want, "\"id\":\"%s\"", id);
        if (!strstr(msg, "\"home-rpc-result\"") || !strstr(msg, want)) { free(msg); continue; }
        const char *err = strstr(msg, "\"error\":\"");
        size_t bl; const char *b64 = lh_json_string_value(msg, "thumbnail", &bl);
        if (!b64) { snprintf(lh_err, sizeof lh_err, "no thumbnail: %.60s", err ? err + 9 : "unknown"); free(msg); return NULL; }
        unsigned char *jpg = malloc(bl * 3 / 4 + 4); *jpeg_len = lh_b64_decode(b64, bl, jpg); free(msg);
        return jpg;
    }
    snprintf(lh_err, sizeof lh_err, "no answer to the thumbnail request"); return NULL;
}

#endif
