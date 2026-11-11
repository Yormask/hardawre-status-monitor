/* ui.h - terminal interface and plain-text report */
#ifndef LHS_UI_H
#define LHS_UI_H

#include "collect.h"

enum { VIEW_DASH, VIEW_PROCS, VIEW_HW, VIEW_COUNT };
enum { SORT_CPU, SORT_MEM, SORT_IO, SORT_PID, SORT_NAME, SORT_TIME };

typedef struct {
    int view, sort, reverse, show_kthreads, paused, help, follow;
    double interval;
    char filter[64];
    int filter_edit;
    pid_t sel_pid;
    int sel_idx, sel_delta, scroll, page_rows;
    int hw_scroll, hw_max;
} ui_state_t;

void ui_init(int utf8);
void ui_end(void);
void ui_draw(sys_t *s, ui_state_t *u);
int ui_key(ui_state_t *u, int ch); /* returns 0 when the user wants to quit */

void report_print(sys_t *s, int nproc);

#endif
