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
/* ui_sensors.c - Setup > Sensor names and limits: your own name for a
 * sensor ("Front fan" instead of "nct6798 fan2"), a limit of your own that
 * the colours, events and alerts follow, or hiding one that reads nonsense
 * (an unconnected input showing 127 °C). Saved in the config file as
 * "sensor = ..." lines, kept while the sensor is away. */
#define _GNU_SOURCE
#include "ui_int.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { R_CHIP, R_SENS, R_HEAD, R_RULE };
typedef struct {
    int kind, idx; /* idx: sensor (R_CHIP, R_SENS) or rule (R_RULE) */
} srow_t;

#define MAXR (MAX_SENSORS * 2 + MAX_SRULES + 2)
static srow_t R[MAXR];
static int NR;
static sys_t *g_s; /* for the keys, which come without it */

static int present(const sensors_t *S, const char *id)
{
    for (int i = 0; i < S->n; i++)
        if (!strcmp(S->s[i].id, id))
            return 1;
    return 0;
}

static void build(const sys_t *s, const ui_state_t *u)
{
    const sensors_t *S = &s->sens;
    int n = 0, last = -1, head = 0;
    for (int i = 0; i < S->n && n < MAXR - 2; i++) {
        if (S->s[i].chip_idx != last) {
            R[n++] = (srow_t){ R_CHIP, i };
            last = S->s[i].chip_idx;
        }
        R[n++] = (srow_t){ R_SENS, i };
    }
    for (int k = 0; k < u->nsrule && n < MAXR - 1; k++) {
        if (present(S, u->srule[k].id))
            continue;
        if (!head) {
            R[n++] = (srow_t){ R_HEAD, 0 };
            head = 1;
        }
        R[n++] = (srow_t){ R_RULE, k };
    }
    NR = n;
}

static int selectable(int i)
{
    return i >= 0 && i < NR && (R[i].kind == R_SENS || R[i].kind == R_RULE);
}

static void fix_sel(ui_state_t *u, int dir)
{
    if (!NR) {
        u->sens_sel = 0;
        return;
    }
    u->sens_sel = CLAMP(u->sens_sel, 0, NR - 1);
    for (int k = 0; k < NR && !selectable(u->sens_sel); k++)
        u->sens_sel = CLAMP(u->sens_sel + (dir ? dir : 1), 0, NR - 1);
    if (!selectable(u->sens_sel)) /* ran into the end: look the other way */
        for (int k = 0; k < NR && !selectable(u->sens_sel); k++)
            u->sens_sel = CLAMP(u->sens_sel - (dir ? dir : 1), 0, NR - 1);
}

/* the rule of the selected row (a copy, empty when there is none yet) */
static int cur_rule(const ui_state_t *u, srule_t *r, const sensor_t **t)
{
    memset(r, 0, sizeof *r);
    *t = NULL;
    if (!selectable(u->sens_sel) || !g_s)
        return 0;
    const srow_t *row = &R[u->sens_sel];
    if (row->kind == R_SENS) {
        *t = &g_s->sens.s[row->idx];
        str_copy(r->id, sizeof r->id, (*t)->id);
        const srule_t *x = config_srule(u, r->id);
        if (x)
            *r = *x;
    } else {
        *r = u->srule[row->idx];
    }
    return 1;
}

static void store(ui_state_t *u, const srule_t *r, const char *what)
{
    config_set_srule(u, r);
    if (g_s)
        sensors_apply(&g_s->sens, u->srule, u->nsrule);
    if (config_save(u) != 0)
        ui_message(u, 1, "Could not save %s: %s", config_path(), strerror(errno));
    else if (what)
        ui_message(u, 0, "%s", what);
}

static const char *const UNIT[] = { "°C", "RPM", "W", "V" };

/* your limits as you would type them: "95", "400", "1.10-1.40" */
static void limit_input(const sensor_t *t, const srule_t *r, char *o, size_t n)
{
    o[0] = '\0';
    int type = t ? t->type : SENS_VOLT;
    if (type == SENS_FAN) {
        if (r->lo > 0)
            sfmt(o, n, "%g", r->lo);
    } else if (type == SENS_VOLT) {
        if (r->lo > 0 || r->hi > 0) {
            char a[16] = "", b[16] = "";
            if (r->lo > 0)
                sfmt(a, sizeof a, "%g", r->lo);
            if (r->hi > 0)
                sfmt(b, sizeof b, "%g", r->hi);
            sfmt(o, n, "%s-%s", a, b);
        }
    } else if (r->hi > 0) {
        sfmt(o, n, "%g", r->hi);
    }
}

static void limit_text(const sensor_t *t, const srule_t *r, char *o, size_t n, int *mine)
{
    o[0] = '\0';
    *mine = r->lo > 0 || r->hi > 0;
    const char *u = t ? (t->type == SENS_TEMP ? DEG : UNIT[t->type]) : "";
    if (!t) { /* away: its type isn't known, the numbers are */
        if (r->lo > 0 && r->hi > 0)
            sfmt(o, n, "%g - %g", r->lo, r->hi);
        else if (r->lo > 0 || r->hi > 0)
            sfmt(o, n, "%s %g", r->lo > 0 ? "min" : "max", r->lo > 0 ? r->lo : r->hi);
        return;
    }
    switch (t->type) {
    case SENS_TEMP:
        if (r->hi > 0)
            sfmt(o, n, "%.0f%s", r->hi, u);
        else if (t->crit > 0)
            sfmt(o, n, "(chip: %.0f%s)", t->crit, u);
        break;
    case SENS_FAN:
        if (r->lo > 0)
            sfmt(o, n, "at least %.0f RPM", r->lo);
        break;
    case SENS_VOLT:
        if (r->lo > 0 && r->hi > 0)
            sfmt(o, n, "%.2f - %.2f V", r->lo, r->hi);
        else if (r->lo > 0 || r->hi > 0)
            sfmt(o, n, "%s %.2f V", r->lo > 0 ? "at least" : "at most", r->lo > 0 ? r->lo : r->hi);
        else if (t->lo > 0 || t->hi > 0)
            sfmt(o, n, "(chip: %.2f - %.2f V)", t->lo, t->hi);
        break;
    default:
        if (r->hi > 0)
            sfmt(o, n, "%.1f W", r->hi);
        break;
    }
}

/* "95", "1.1-1.4", "-1.4", "1.1-" -> lo / hi; 0 = fine (empty clears) */
static int parse_limit(const char *in, int type, double *lo, double *hi)
{
    char b[64];
    str_copy(b, sizeof b, in);
    for (char *p = b; *p; p++)
        if (*p == ',')
            *p = '.'; /* a decimal comma */
    char *p = b;
    while (*p == ' ')
        p++;
    *lo = *hi = 0;
    if (!*p)
        return 0;
    char *dash = strchr(p + 1, '-'); /* not a leading one: "-1.4" means "up to 1.4" */
    if (*p == '-')
        dash = p;
    char *end;
    if (type == SENS_VOLT && dash) {
        *dash = '\0';
        if (*p) {
            *lo = strtod(p, &end);
            if (end == p || *lo <= 0)
                return -1;
        }
        char *q = dash + 1;
        while (*q == ' ')
            q++;
        if (*q) {
            *hi = strtod(q, &end);
            while (*end == ' ')
                end++;
            if (end == q || *end || *hi <= 0)
                return -1;
        }
        return (*lo > 0 && *hi > 0 && *lo >= *hi) || (*lo <= 0 && *hi <= 0) ? -1 : 0;
    }
    double v = strtod(p, &end);
    while (*end == ' ')
        end++;
    if (end == p || *end || v <= 0 || v > 1e6)
        return -1;
    if (type == SENS_FAN)
        *lo = v;
    else
        *hi = v;
    return 0;
}

static void edit_start(ui_state_t *u, int what)
{
    srule_t r;
    const sensor_t *t;
    if (!cur_rule(u, &r, &t))
        return;
    if (what == 2 && !t) {
        ui_message(u, 1, "This sensor isn't there right now: its limit can be set once it is back");
        return;
    }
    u->sens_edit = what;
    if (what == 1) {
        str_copy(u->sens_buf, sizeof u->sens_buf, r.name);
        sfmt(u->sens_prompt, sizeof u->sens_prompt, "Name for %s", t ? t->label : r.id);
    } else {
        limit_input(t, &r, u->sens_buf, sizeof u->sens_buf);
        static const char *const how[] = {
            "Limit in °C: red and an alert from here on, yellow 10° before",
            "Slowest this fan should turn, in RPM: slower raises an alert",
            "Most this may draw, in W",
            "Range in V, like 1.10-1.40 (or 1.10- / -1.40 for one side)",
        };
        str_copy(u->sens_prompt, sizeof u->sens_prompt, how[t->type]);
    }
}

static void edit_key(ui_state_t *u, int ch)
{
    size_t len = strlen(u->sens_buf);
    u->msg_until = 0; /* typing again: the last complaint is done */
    if (ch == 27) {
        u->sens_edit = 0;
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        while (len && ((unsigned char)u->sens_buf[len - 1] & 0xc0) == 0x80) /* a whole UTF-8 character */
            len--;
        if (len)
            len--;
        u->sens_buf[len] = '\0';
        return;
    }
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
        srule_t r;
        const sensor_t *t;
        if (!cur_rule(u, &r, &t)) {
            u->sens_edit = 0;
            return;
        }
        char msg[160];
        const char *who = t ? t->label : r.id;
        if (u->sens_edit == 1) {
            char *b = u->sens_buf;
            while (*b == ' ')
                b++;
            size_t l = strlen(b);
            while (l && b[l - 1] == ' ')
                b[--l] = '\0';
            str_copy(r.name, sizeof r.name, b);
            if (r.name[0])
                sfmt(msg, sizeof msg, "%s is now called \"%s\"", who, r.name);
            else
                sfmt(msg, sizeof msg, "%s keeps its own name", who);
        } else {
            double lo, hi;
            if (parse_limit(u->sens_buf, t->type, &lo, &hi) != 0) {
                ui_message(u, 1, "%s", t->type == SENS_VOLT ? "Type a range like 1.10-1.40, or one side: 1.10- or -1.40"
                                                            : "Type a number (or nothing for no limit)");
                return;
            }
            r.lo = lo;
            r.hi = hi;
            if (lo <= 0 && hi <= 0)
                sfmt(msg, sizeof msg, "%s: no limit of yours", who);
            else
                sfmt(msg, sizeof msg, "%s: your limit is saved; past it the value turns red and raises an event", who);
        }
        u->sens_edit = 0;
        store(u, &r, msg);
        return;
    }
    int ok = u->sens_edit == 1 ? (ch >= 32 && ch < 256 && ch != 127 && ch != '|')
                               : ((ch >= '0' && ch <= '9') || ch == '.' || ch == ',' || ch == '-' || ch == ' ');
    size_t cap = u->sens_edit == 1 ? sizeof ((srule_t *)0)->name - 1 : 15;
    if (ok && len + 1 <= cap) {
        u->sens_buf[len] = (char)ch;
        u->sens_buf[len + 1] = '\0';
    }
}

int sensors_page_key(ui_state_t *u, int ch)
{
    if (u->sens_edit) {
        edit_key(u, ch);
        return 1;
    }
    srule_t r;
    const sensor_t *t;
    switch (ch) {
    case KEY_UP:
        for (int i = u->sens_sel - 1; i >= 0; i--)
            if (selectable(i)) {
                u->sens_sel = i;
                break;
            }
        return 1;
    case KEY_DOWN:
        for (int i = u->sens_sel + 1; i < NR; i++)
            if (selectable(i)) {
                u->sens_sel = i;
                break;
            }
        return 1;
    case KEY_PPAGE: u->sens_sel -= 10; fix_sel(u, -1); return 1;
    case KEY_NPAGE: u->sens_sel += 10; fix_sel(u, 1); return 1;
    case KEY_HOME: u->sens_sel = 0; fix_sel(u, 1); return 1;
    case KEY_END: u->sens_sel = NR - 1; fix_sel(u, -1); return 1;
    case 27:
    case KEY_LEFT:
        u->sens_open = 0;
        return 1;
    case '\n':
    case '\r':
    case KEY_ENTER:
    case 'n':
        edit_start(u, 1);
        return 1;
    case 'l':
        edit_start(u, 2);
        return 1;
    case 'h':
    case ' ':
        if (cur_rule(u, &r, &t)) {
            r.hide ^= 1;
            char msg[160];
            sfmt(msg, sizeof msg, r.hide ? "%s is hidden: left out of the panels, events and logs (h shows it again)"
                                         : "%s is shown again",
                 t ? sensor_label(t) : r.id);
            store(u, &r, msg);
        }
        return 1;
    case KEY_DC:
    case 'd':
        if (cur_rule(u, &r, &t)) {
            if (!r.name[0] && r.lo <= 0 && r.hi <= 0 && !r.hide) {
                ui_message(u, 0, "Nothing of yours to remove here");
                return 1;
            }
            char id[96];
            str_copy(id, sizeof id, r.id);
            memset(&r, 0, sizeof r);
            str_copy(r.id, sizeof r.id, id);
            store(u, &r, "Back to the chip's own name and limits");
            if (g_s)
                build(g_s, u);
            fix_sel(u, 1);
        }
        return 1;
    }
    return 0;
}

void sensors_page_click(ui_state_t *u, int row, int dbl)
{
    if (u->sens_edit) /* a click elsewhere keeps what was typed */
        sensors_page_key(u, '\n');
    if (!selectable(row))
        return;
    u->sens_sel = row;
    if (dbl)
        edit_start(u, 1);
}

void sensors_page(sys_t *s, ui_state_t *u)
{
    g_s = s;
    build(s, u);
    fix_sel(u, 0);
    const sensors_t *S = &s->sens;
    int top = 1, H = LINES - 2, W = COLS;
    char info[80];
    int nh = 0;
    for (int i = 0; i < S->n; i++)
        nh += S->s[i].hidden;
    sfmt(info, sizeof info, "%d sensor%s", S->n, S->n == 1 ? "" : "s");
    if (nh)
        app(info, sizeof info, ", %d hidden", nh);
    box_(top, 0, H, W, "Sensor names and limits", info);
    int ix = 2, iw = W - 4, iy = top + 1, ih = H - 2;
    if (ih < 4)
        return;
    if (!S->n && !u->nsrule) {
        put(iy, ix, iw, A_GRAY, "No hwmon sensors found in /sys/class/hwmon: nothing to name.");
        return;
    }
    int c1 = MIN(26, iw / 4), c2 = 11, c3 = MIN(28, iw / 4), x2 = ix + c1, x3 = x2 + c2, x4 = x3 + c3;
    int c4 = ix + iw - x4;
    put(iy, ix, c1, CP(C_THEAD), "%-*s", c1, "Sensor");
    put(iy, x2, c2, CP(C_THEAD), "%-*s", c2, "Now");
    put(iy, x3, c3, CP(C_THEAD), "%-*s", c3, "Your name");
    put(iy, x4, c4, CP(C_THEAD), "%-*s", c4, "Your limit");
    int rows = ih - 3; /* heading, then the list, then two lines of help */
    if (u->sens_sel < u->sens_scroll)
        u->sens_scroll = u->sens_sel;
    if (u->sens_sel >= u->sens_scroll + rows)
        u->sens_scroll = u->sens_sel - rows + 1;
    if (u->sens_scroll > 0 && R[u->sens_scroll - 1].kind == R_CHIP && u->sens_sel - u->sens_scroll + 1 < rows)
        u->sens_scroll--; /* show the chip above its first sensor */
    u->sens_scroll = CLAMP(u->sens_scroll, 0, MAX(0, NR - rows));
    hit_add(iy + 1, ix, rows, iw, HIT_SCROLL, 0, 0);
    for (int k = 0; k < rows && u->sens_scroll + k < NR; k++) {
        int i = u->sens_scroll + k, y = iy + 1 + k, sel = i == u->sens_sel;
        const srow_t *row = &R[i];
        if (row->kind == R_CHIP) {
            const sensor_t *t = &S->s[row->idx];
            char nm[24];
            sensors_chip_name(t->chip, nm, sizeof nm);
            if (strcmp(nm, t->chip))
                put(y, ix, iw, CP(C_INFO) | A_BOLD, "%s  (%s)", t->chip, nm);
            else
                put(y, ix, iw, CP(C_INFO) | A_BOLD, "%s", t->chip);
            continue;
        }
        if (row->kind == R_HEAD) {
            put(y, ix, iw, A_GRAY | A_BOLD, "%s", "Not found right now (kept for when they're back; d removes)");
            continue;
        }
        attr_t base = sel ? CP(C_SEL) : A_NORMAL;
        if (sel)
            fill(y, ix - 1, iw + 2, base);
        hit_add(y, ix, 1, iw, HIT_SENSOR, i, 0);
        const sensor_t *t = row->kind == R_SENS ? &S->s[row->idx] : NULL;
        srule_t r;
        memset(&r, 0, sizeof r);
        if (t) {
            const srule_t *x = config_srule(u, t->id);
            if (x)
                r = *x;
        } else {
            r = u->srule[row->idx];
        }
        attr_t dim = sel ? base : A_GRAY;
        put(y, ix + 2, c1 - 3, r.hide ? dim : base, "%s", t ? t->label : r.id);
        char v[24] = "";
        if (t && t->ok && t->type == SENS_TEMP)
            sfmt(v, sizeof v, "%.1f%s", t->value, DEG);
        else if (t && t->ok)
            sfmt(v, sizeof v, t->type == SENS_VOLT ? "%.3f %s" : t->type == SENS_POWER ? "%.1f %s" : "%.0f %s",
                 t->value, UNIT[t->type]);
        else
            str_copy(v, sizeof v, t ? "n/a" : "away");
        attr_t va = sel ? base | A_BOLD
                  : !t || !t->ok || r.hide ? A_GRAY
                  : sensor_bad(t) ? CP(C_CRIT) | A_BOLD
                  : t->type == SENS_TEMP ? temp_attr_lim(t->value, t->uhi) | A_BOLD : A_BOLD;
        put(y, x2, c2 - 1, va, "%s", v);
        int editing = sel && u->sens_edit;
        if (editing && u->sens_edit == 1) {
            int xx = x3 + put(y, x3, c3 - 2, CP(C_SEL) | A_BOLD | A_UNDERLINE, "%s", u->sens_buf);
            put(y, xx, 1, A_REVERSE, " ");
        } else if (r.name[0]) {
            put(y, x3, c3 - 1, sel ? base | A_BOLD : A_BOLD, "%s", r.name);
        } else {
            put(y, x3, c3 - 1, dim, "%s", U ? "·" : "-");
        }
        char lt[48];
        int mine;
        limit_text(t, &r, lt, sizeof lt, &mine);
        int lx = x4;
        if (editing && u->sens_edit == 2) {
            lx += put(y, x4, c4 - 2, CP(C_SEL) | A_BOLD | A_UNDERLINE, "%s", u->sens_buf);
            lx += put(y, lx, 1, A_REVERSE, " ");
        } else {
            lx += put(y, x4, c4, mine ? (sel ? base | A_BOLD : A_BOLD) : dim, "%s", lt);
        }
        if (r.hide && x4 + c4 - lx > 8)
            put(y, ix + iw - 6, 6, sel ? base : CP(C_WARN), "hidden");
    }
    if (NR > rows) {
        char m[32];
        sfmt(m, sizeof m, " %d%s ", NR - rows, U ? " more ↑↓" : " more");
        put(top + H - 1, W - 2 - utf8_cols(m), utf8_cols(m), A_GRAY, "%s", m);
    }
    int hy = iy + ih - 2;
    wrap_text(hy, ix, iw, 2, A_GRAY,
              "Your names show in every panel, the Hardware view, events and new logs. Past a limit of yours the value "
              "turns red and raises an event; hidden sensors are left out everywhere.");
}
