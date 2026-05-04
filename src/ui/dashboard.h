#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "../master/server.h"

/* Initialize ncurses against /dev/tty and attach the UI to the master state. */
int  dashboard_init(MasterState *master);

/* Main UI loop: poll keyboard, render snapshots, and handle dashboard forms. */
void dashboard_run(void);

/* Tear down ncurses and close terminal handles. Safe to call after init failure. */
void dashboard_stop(void);

#endif /* DASHBOARD_H */
