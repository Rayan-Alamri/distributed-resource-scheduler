#ifndef DASHBOARD_H
#define DASHBOARD_H

/* Minimal stubs — full ncurses implementation is Mohammed Yar's scope.
 * These symbols let the master binary compile without the UI being wired up. */

void dashboard_init(void);
void dashboard_run(void);
void dashboard_stop(void);

#endif /* DASHBOARD_H */
