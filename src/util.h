/* util.h - small helpers shared by every module */
#ifndef LHS_UTIL_H
#define LHS_UTIL_H

#include <stddef.h>
#include <sys/types.h>

#define LHS_VERSION "1.0.0"
#define PATHBUF 512

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Roots of the kernel pseudo-filesystems. Overridable with the LHS_SYSFS /
 * LHS_PROCFS environment variables, which is handy for testing against a
 * captured or fake tree. */
extern const char *g_sysfs;
extern const char *g_procfs;

/* File reading. All return -1 on failure (errno is preserved). */
ssize_t read_file(const char *path, char *buf, size_t size);       /* NUL-terminated */
ssize_t read_file_grow(const char *path, char **buf, size_t *cap); /* grows *buf as needed */
int read_line(const char *path, char *buf, size_t size);           /* first line, trimmed */
int read_ll(const char *path, long long *out);
int read_ull(const char *path, unsigned long long *out);
int read_hex(const char *path, unsigned *out);
int path_exists(const char *path);

/* Split a buffer in place: returns the next line or NULL at the end. */
char *next_line(char **cursor);

/* snprintf without -Wformat-truncation noise; returns snprintf's value. */
int sfmt(char *dst, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void str_copy(char *dst, size_t n, const char *src);

double now_sec(void); /* CLOCK_MONOTONIC in seconds */

/* Human-readable formatting. */
void fmt_bytes(double v, char *out, size_t n);    /* "12.3G"   (max 5 cols) */
void fmt_rate(double v, char *out, size_t n);     /* "12.3M/s" (max 7 cols) */
void fmt_si(double v, char *out, size_t n);       /* "12.3k"                */
void fmt_duration(double s, char *out, size_t n); /* "3d 04h", "12m 05s"    */

/* UTF-8 aware column counting (every code point counted as one column). */
int utf8_cols(const char *s);
size_t utf8_bytes_for_cols(const char *s, int cols);

/* Fixed-size history ring for graphs. */
#define HIST_LEN 600
typedef struct {
    float v[HIST_LEN];
    int head, count;
} hist_t;
void hist_push(hist_t *h, double v);
double hist_get(const hist_t *h, int age); /* age 0 = newest; -1 if none */
double hist_max(const hist_t *h, int n);

#endif
