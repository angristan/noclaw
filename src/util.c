/*
 * Utility functions: string ops, path joining, file I/O, random.
 */

#include "nc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/* ── Logging ──────────────────────────────────────────────────── */

nc_log_level nc_log_min_level = NC_LOG_INFO;

static const char *log_prefix[] = {
    [NC_LOG_DEBUG] = "DBG",
    [NC_LOG_INFO]  = "INF",
    [NC_LOG_WARN]  = "WRN",
    [NC_LOG_ERROR] = "ERR",
};

void nc_log(nc_log_level level, const char *fmt, ...) {
    if (level < nc_log_min_level) return;
    fprintf(stderr, "[%s] ", log_prefix[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ── String view ──────────────────────────────────────────────── */

bool nc_str_eq(nc_str a, nc_str b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

bool nc_str_eql(nc_str a, const char *b) {
    size_t blen = strlen(b);
    return a.len == blen && memcmp(a.ptr, b, blen) == 0;
}

nc_str nc_str_from(const char *s) {
    if (!s) return NC_STR_NULL;
    return (nc_str){ .ptr = s, .len = strlen(s) };
}

/* ── Safe strlcpy ─────────────────────────────────────────────── */

size_t nc_strlcpy(char *dst, const char *src, size_t dstsize) {
    size_t srclen = strlen(src);
    if (dstsize > 0) {
        size_t cplen = srclen < dstsize - 1 ? srclen : dstsize - 1;
        memcpy(dst, src, cplen);
        dst[cplen] = '\0';
    }
    return srclen;
}

/* Return the length of one valid UTF-8 sequence, or zero if malformed. */
size_t nc_utf8_sequence_len(const char *s, size_t len) {
    if (!s || len == 0) return 0;

    const unsigned char *p = (const unsigned char *)s;
    unsigned char b0 = p[0];
    if (b0 <= 0x7f) return 1;

    if (b0 >= 0xc2 && b0 <= 0xdf) {
        return len >= 2 && p[1] >= 0x80 && p[1] <= 0xbf ? 2 : 0;
    }

    if (b0 >= 0xe0 && b0 <= 0xef) {
        if (len < 3 || p[2] < 0x80 || p[2] > 0xbf) return 0;
        if (b0 == 0xe0) return p[1] >= 0xa0 && p[1] <= 0xbf ? 3 : 0;
        if (b0 == 0xed) return p[1] >= 0x80 && p[1] <= 0x9f ? 3 : 0;
        return p[1] >= 0x80 && p[1] <= 0xbf ? 3 : 0;
    }

    if (b0 >= 0xf0 && b0 <= 0xf4) {
        if (len < 4 || p[2] < 0x80 || p[2] > 0xbf ||
            p[3] < 0x80 || p[3] > 0xbf)
            return 0;
        if (b0 == 0xf0) return p[1] >= 0x90 && p[1] <= 0xbf ? 4 : 0;
        if (b0 == 0xf4) return p[1] >= 0x80 && p[1] <= 0x8f ? 4 : 0;
        return p[1] >= 0x80 && p[1] <= 0xbf ? 4 : 0;
    }

    return 0;
}

size_t nc_utf8_safe_prefix(const char *s, size_t len, size_t max_bytes) {
    size_t pos = 0;
    while (pos < len && pos < max_bytes) {
        size_t n = nc_utf8_sequence_len(s + pos, len - pos);
        if (n == 0 || n > max_bytes - pos) break;
        pos += n;
    }
    return pos;
}

/* ── Home directory ───────────────────────────────────────────── */

const char *nc_home_dir(void) {
    const char *h = getenv("HOME");
    if (!h) h = "/tmp";
    return h;
}

/* ── Path joining ─────────────────────────────────────────────── */

char *nc_path_join(char *buf, size_t bufsz, const char *a, const char *b) {
    snprintf(buf, bufsz, "%s/%s", a, b);
    return buf;
}

char *nc_path_join3(char *buf, size_t bufsz, const char *a, const char *b, const char *c) {
    snprintf(buf, bufsz, "%s/%s/%s", a, b, c);
    return buf;
}

/* ── File I/O ─────────────────────────────────────────────────── */

char *nc_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz < 0 || sz > 10 * 1024 * 1024) { /* 10MB limit */
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

bool nc_write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t wr = fwrite(data, 1, len, f);
    fclose(f);
    return wr == len;
}

bool nc_mkdir_p(const char *path) {
    char tmp[1024];
    nc_strlcpy(tmp, path, sizeof(tmp));

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
}

bool nc_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* ── Random ───────────────────────────────────────────────────── */

void nc_random_hex(char *out, size_t len) {
    static const char hex[] = "0123456789abcdef";

    /* Read from /dev/urandom for cryptographic randomness */
    unsigned char raw[128];
    if (len > sizeof(raw) * 2) len = sizeof(raw) * 2;  /* cap to prevent OOB */
    size_t half = (len + 1) / 2;  /* bytes needed (2 hex chars per byte) */

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < half) {
            ssize_t n = read(fd, raw + got, half - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(fd);

        for (size_t i = 0; i < len; i++)
            out[i] = hex[(raw[i / 2] >> ((i & 1) ? 0 : 4)) & 0x0f];
        out[len] = '\0';
        return;
    }

    /* Fallback (should never happen on any sane POSIX system) */
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        seeded = true;
    }
    for (size_t i = 0; i < len; i++)
        out[i] = hex[rand() % 16];
    out[len] = '\0';
}

/* ── Tests ──────────────────────────────────────────────────────── */

#ifdef NC_TEST
void nc_test_str(void) {
    nc_str a = NC_STR("hello");
    nc_str b = NC_STR("hello");
    nc_str c = NC_STR("world");

    NC_ASSERT(nc_str_eq(a, b), "str_eq same");
    NC_ASSERT(!nc_str_eq(a, c), "str_eq diff");
    NC_ASSERT(nc_str_eql(a, "hello"), "str_eql match");
    NC_ASSERT(!nc_str_eql(a, "hell"), "str_eql no match");

    nc_str d = nc_str_from("test");
    NC_ASSERT(d.len == 4, "str_from len");
    NC_ASSERT(nc_str_eql(d, "test"), "str_from content");

    nc_str e = nc_str_from(NULL);
    NC_ASSERT(e.ptr == NULL && e.len == 0, "str_from NULL");

    char buf[64];
    nc_path_join(buf, sizeof(buf), "/home", ".noclaw");
    NC_ASSERT(strcmp(buf, "/home/.noclaw") == 0, "path_join 2");

    nc_path_join3(buf, sizeof(buf), "/home", ".noclaw", "config.json");
    NC_ASSERT(strcmp(buf, "/home/.noclaw/config.json") == 0, "path_join 3");

    const char *utf8 = "A\xc3\xa9\xe4\xbd\xa0\xf0\x9f\x91\x8b";
    NC_ASSERT(nc_utf8_safe_prefix(utf8, strlen(utf8), 10) == 10,
              "utf8 prefix keeps complete sequences");
    NC_ASSERT(nc_utf8_safe_prefix(utf8, strlen(utf8), 9) == 6,
              "utf8 prefix drops partial four-byte sequence");
    NC_ASSERT(nc_utf8_safe_prefix(utf8, strlen(utf8), 2) == 1,
              "utf8 prefix drops partial two-byte sequence");
    NC_ASSERT(nc_utf8_sequence_len("\xed\xa0\x80", 3) == 0,
              "utf8 rejects surrogate code points");
}
#endif
