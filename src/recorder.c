// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * hsm (hardware status monitor) - live monitor for CPU, GPU, memory,
 * power, disks, network and processes on Linux
 * Copyright (C) 2026  Yormask
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/* recorder.c - the flight recorder.
 *
 * Every value hsm can log is kept for the last few minutes (10 by default),
 * one row per second, in memory. When something goes wrong - the CPU
 * throttles, memory runs out, the kernel reports an error, a temperature
 * reaches its limit, an alert fires - those minutes are written to an
 * incident log, which then keeps going until a minute after the last
 * problem. You get the lead-up to a rare problem without logging around
 * the clock.
 *
 * A crash or power cut would take the memory with it, so the same rows also
 * go to two small rolling files in the session folder (each covering the
 * recorder's minutes, overwritten in turn). If the session doesn't end
 * normally, the next start turns them into an incident log. */
#define _GNU_SOURCE
#include "log.h"
#include "watch.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AFTER 60.0          /* keep writing this long after the last problem */
#define LONGEST (30 * 60.0) /* ... but close an incident log after this long */
#define GAP 120.0           /* quiet time before a new incident log may start */
#define PER_DAY 30          /* incident logs per day at most */
#define SYNC_EVERY 15.0

typedef struct {
    double wall;
    unsigned long seq; /* events up to this one had happened */
} rrow_t;

static struct {
    /* the ring: cap rows of nchan values */
    int nchan, cap, head, n, minutes;
    unsigned hash;
    float *v;
    rrow_t *row;
    double last, t0;
    unsigned long seen; /* events checked for triggers */
    int primed;

    /* incident log being written */
    FILE *f;
    char path[PATHBUF];
    double trig_wall, until, started, ended;
    unsigned long wseq; /* events already written */
    int day, today;

    /* rolling files */
    FILE *rf[2];
    int cur;
    double cur_t0, next_sync;
    unsigned long rseq;
    char rpath[2][PATHBUF];

    /* for the screen */
    int saved;
    char last_path[PATHBUF], reason[160];
} R;

static double wall(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static unsigned chan_hash(void)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < g_log.nchan; i++)
        for (const char *p = g_log.ch[i].id; ; p++) {
            h = (h ^ (unsigned char)*p) * 16777619u;
            if (!*p)
                break;
        }
    return h;
}

/* ------------------------------------------------------------- writing */

/* one buffered row; prev = events already shown in the row before it */
static void put_row(FILE *f, int i, double ref, const health_t *h, unsigned long prev)
{
    const rrow_t *rw = &R.row[i];
    const float *v = &R.v[(size_t)i * (size_t)R.nchan];
    log_put_time(f, rw->wall);
    fprintf(f, ",%.1f", rw->wall - ref);
    for (int c = 0; c < R.nchan; c++) {
        if (log_chan_is_events(&g_log.ch[c]))
            log_put_events(f, h, prev, rw->seq);
        else
            log_put_value(f, &g_log.ch[c], v[c]);
    }
    fputc('\n', f);
}

static int ring_index(int k) /* k = 0 oldest .. n-1 newest */
{
    return (R.head - R.n + k + R.cap) % R.cap;
}

/* ------------------------------------------------------- rolling files */

static void rolling_close(int remove)
{
    int who = remove ? config_as_user() : 0;
    for (int k = 0; k < 2; k++) {
        if (R.rf[k])
            fclose(R.rf[k]);
        R.rf[k] = NULL;
        if (remove && who >= 0 && R.rpath[k][0])
            unlink(R.rpath[k]);
    }
    if (remove && who >= 0)
        config_as_root(who);
}

static FILE *rolling_open(int k)
{
    sfmt(R.rpath[k], sizeof R.rpath[k], "%s/%d.rec%d.csv", session_dir(), (int)getpid(), k);
    int who = config_as_user();
    if (who < 0)
        return NULL;
    FILE *f = NULL;
    if (config_mkdir_p(session_dir()) == 0) {
        int fd = open(R.rpath[k], O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
        f = fd >= 0 ? fdopen(fd, "w") : NULL;
        if (!f && fd >= 0)
            close(fd);
    }
    config_as_root(who);
    return f;
}

/* start file k afresh: header, then rows from now on */
static int rolling_restart(int k)
{
    if (!R.rf[k] && !(R.rf[k] = rolling_open(k)))
        return -1;
    fflush(R.rf[k]);
    if (ftruncate(fileno(R.rf[k]), 0) != 0)
        return -1;
    rewind(R.rf[k]);
    log_put_header(R.rf[k], "time,utc_offset,elapsed_s", 1);
    fflush(R.rf[k]);
    return 0;
}

static void rolling_row(const health_t *h, double mono)
{
    if (!R.rf[R.cur] || mono - R.cur_t0 >= R.minutes * 60.0) { /* switch to the other file */
        int k = R.rf[R.cur] ? 1 - R.cur : R.cur;
        if (rolling_restart(k) != 0)
            return;
        R.cur = k;
        R.cur_t0 = mono;
    }
    FILE *f = R.rf[R.cur];
    put_row(f, ring_index(R.n - 1), R.t0, h, R.rseq);
    R.rseq = R.row[ring_index(R.n - 1)].seq;
    fflush(f);
    if (mono >= R.next_sync) { /* on the disk itself, in case the power goes */
        fdatasync(fileno(f));
        R.next_sync = mono + SYNC_EVERY;
    }
}

/* --------------------------------------------------------- incident logs */

static void incident_close(const char *why)
{
    if (!R.f)
        return;
    fflush(R.f);
    fsync(fileno(R.f));
    fclose(R.f);
    R.f = NULL;
    R.ended = now_sec();
    R.saved++;
    str_copy(R.last_path, sizeof R.last_path, R.path);
    char p[PATHBUF];
    config_pretty_path(R.path, p, sizeof p);
    log_note(0, "Flight recorder: incident log saved%s: %s", why, p);
}

static int is_trigger(const event_t *e)
{
    if (e->level == EV_ERR)
        return 1;
    /* warnings too (throttling, low memory, slow disk, freezes...), but not
     * the corrected PCIe errors some machines report all day long, a Wi-Fi
     * that comes and goes, or a slowly growing process */
    return e->level == EV_WARN && strcmp(e->cat, "pcie") && strcmp(e->cat, "shutdown") && strcmp(e->cat, "net") &&
           strcmp(e->cat, "leak");
}

static void incident_start(const health_t *h, const event_t *e, double mono)
{
    int today = log_day_key(wall());
    if (today != R.day) {
        R.day = today;
        R.today = 0;
    }
    if (R.today >= PER_DAY || (R.ended > 0 && mono - R.ended < GAP))
        return;
    char ts[32], base[96], err[PATHBUF + 64];
    time_t t = (time_t)e->t;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", &tm);
    sfmt(base, sizeof base, "hsm-incident-%s-%s", ts, e->cat);
    R.f = log_create(e->t, base, R.path, sizeof R.path, err, sizeof err);
    if (!R.f) {
        log_note(1, "Flight recorder: %s", err);
        R.ended = mono; /* don't retry on every event */
        return;
    }
    R.today++;
    R.trig_wall = e->t;
    R.started = mono;
    R.until = mono + AFTER;
    sfmt(R.reason, sizeof R.reason, "%s: %s", e->cat, e->text);
    log_put_header(R.f, "time,utc_offset,from_problem_s", 1);
    unsigned long prev = R.n ? R.row[ring_index(0)].seq : h->ev_seq;
    for (int k = 0; k < R.n; k++) { /* the lead-up */
        int i = ring_index(k);
        put_row(R.f, i, R.trig_wall, h, prev);
        prev = R.row[i].seq;
    }
    R.wseq = prev;
    fflush(R.f);
    char p[PATHBUF];
    config_pretty_path(R.path, p, sizeof p);
    log_note(1, "Problem (%s): saving the last %d minutes to %s", e->cat, R.minutes, p);
}

/* ------------------------------------------------------------------ ring */

static void ring_free(void)
{
    free(R.v);
    free(R.row);
    R.v = NULL;
    R.row = NULL;
    R.n = R.head = R.cap = R.nchan = 0;
}

static int ring_setup(int minutes)
{
    int cap = minutes * 60 + 5;
    unsigned hash = chan_hash();
    if (R.v && R.cap == cap && R.nchan == g_log.nchan && R.hash == hash)
        return 0;
    incident_close("");
    ring_free();
    R.v = calloc((size_t)cap * (size_t)MAX(g_log.nchan, 1), sizeof *R.v);
    R.row = calloc((size_t)cap, sizeof *R.row);
    if (!R.v || !R.row) {
        ring_free();
        return -1;
    }
    R.cap = cap;
    R.nchan = g_log.nchan;
    R.hash = hash;
    R.minutes = minutes;
    /* the columns changed: the rolling files start over too */
    for (int k = 0; k < 2; k++)
        if (R.rf[k]) {
            fclose(R.rf[k]);
            R.rf[k] = NULL;
            int who = config_as_user();
            if (who >= 0) {
                unlink(R.rpath[k]);
                config_as_root(who);
            }
        }
    R.cur = 0;
    return 0;
}

void rec_tick(sys_t *s, const ui_state_t *u)
{
    double mono = now_sec();
    const health_t *h = &s->health;
    if (!u->rec_on) {
        if (R.v || R.f) {
            incident_close("");
            ring_free();
            rolling_close(1);
        }
        R.seen = h->ev_seq;
        return;
    }
    if (!R.primed) { /* problems from before the recorder started have no lead-up */
        R.seen = R.rseq = h->ev_seq;
        R.t0 = wall();
        R.primed = 1;
    }
    if (!g_log.nchan || ring_setup(CLAMP(u->rec_minutes, 1, 60)) != 0)
        return;

    if (!R.n || mono - R.last >= 0.95) { /* a row a second at most */
        int i = R.head;
        float *v = &R.v[(size_t)i * (size_t)R.nchan];
        for (int c = 0; c < R.nchan; c++)
            v[c] = log_chan_is_events(&g_log.ch[c]) ? NAN : (float)log_chan_value(s, &g_log.ch[c]);
        R.row[i].wall = wall();
        R.row[i].seq = h->ev_seq;
        R.head = (R.head + 1) % R.cap;
        if (R.n < R.cap)
            R.n++;
        R.last = mono;
        if (R.f) {
            put_row(R.f, i, R.trig_wall, h, R.wseq);
            R.wseq = R.row[i].seq;
            fflush(R.f);
        }
        rolling_row(h, mono);
    }

    /* anything new that calls for an incident log? */
    for (int k = h->nev - 1; k >= 0; k--) { /* oldest first */
        const event_t *e = health_event_at(h, k);
        if (e->seq <= R.seen || !is_trigger(e))
            continue;
        if (R.f) /* already recording: keep going a while longer */
            R.until = mono + AFTER;
        else
            incident_start(h, e, mono);
    }
    R.seen = h->ev_seq;
    if (R.f && (mono >= R.until || mono - R.started >= LONGEST))
        incident_close("");
}

void rec_end(int clean)
{
    incident_close(" on exit");
    rolling_close(clean);
    ring_free();
}

void rec_status(rec_status_t *st)
{
    memset(st, 0, sizeof *st);
    st->on = R.v != NULL;
    st->minutes = R.minutes;
    st->rows = R.n;
    st->nchan = R.nchan;
    st->busy = R.f != NULL;
    st->saved = R.saved;
    str_copy(st->last, sizeof st->last, R.f ? R.path : R.last_path);
    str_copy(st->reason, sizeof st->reason, R.reason);
}

/* ------------------------------------------------------------- recovery */

static double file_mtime(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (double)st.st_mtime + (double)st.st_mtim.tv_nsec / 1e9 : -1;
}

/* copy a rolling file's rows (not its header) */
static long copy_rows(FILE *in, FILE *out)
{
    char *line = NULL;
    size_t cap = 0;
    long n = 0;
    ssize_t len;
    if (getline(&line, &cap, in) < 0) { /* header */
        free(line);
        return 0;
    }
    while ((len = getline(&line, &cap, in)) > 0) {
        if (line[len - 1] != '\n') /* cut off when the power went */
            break;
        fputs(line, out);
        n++;
    }
    free(line);
    return n;
}

int rec_recover(long pid, double last_seen, const char *why, char *out, size_t n)
{
    char p[2][PATHBUF], head[2][8192];
    double mt[2];
    FILE *in[2] = { NULL, NULL };
    out[0] = '\0';
    int who = config_as_user();
    if (who < 0)
        return -1;
    for (int k = 0; k < 2; k++) {
        sfmt(p[k], sizeof p[k], "%s/%ld.rec%d.csv", session_dir(), pid, k);
        mt[k] = file_mtime(p[k]);
        int fd = mt[k] > 0 ? open(p[k], O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
        in[k] = fd >= 0 ? fdopen(fd, "r") : NULL;
        if (!in[k] && fd >= 0)
            close(fd);
        head[k][0] = '\0';
        if (in[k] && (!fgets(head[k], sizeof head[k], in[k]) || !strchr(head[k], '\n'))) {
            fclose(in[k]); /* empty, or no complete header */
            in[k] = NULL;
        }
    }
    int newer = in[1] && (!in[0] || mt[1] > mt[0]) ? 1 : 0, older = 1 - newer;
    if (in[older] && in[newer] && strcmp(head[older], head[newer])) { /* columns changed in between */
        fclose(in[older]);
        in[older] = NULL;
    }
    int rc = -1;
    if (in[newer]) {
        char ts[32], base[96], err[PATHBUF + 64];
        time_t t = (time_t)last_seen;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(ts, sizeof ts, "%Y-%m-%d_%H-%M-%S", &tm);
        sfmt(base, sizeof base, "hsm-incident-%s-%s", ts, why);
        config_as_root(who); /* log_create switches by itself */
        FILE *f = log_create(last_seen, base, out, n, err, sizeof err);
        who = config_as_user();
        if (f) {
            fputs(head[newer], f);
            long rows = 0;
            for (int k = 0; k < 2; k++) {
                FILE *src = k == 0 ? in[older] : in[newer];
                if (!src)
                    continue;
                rewind(src);
                rows += copy_rows(src, f);
            }
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            rc = rows > 0 ? 0 : -1;
            if (rc) { /* nothing worth keeping */
                unlink(out);
                out[0] = '\0';
            }
        } else {
            out[0] = '\0';
        }
    }
    for (int k = 0; k < 2; k++) {
        if (in[k])
            fclose(in[k]);
        if (mt[k] > 0 && who >= 0)
            unlink(p[k]);
    }
    if (who >= 0)
        config_as_root(who);
    return rc;
}
