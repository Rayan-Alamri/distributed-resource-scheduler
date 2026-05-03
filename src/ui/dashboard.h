#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "../master/server.h"

int  dashboard_init(MasterState *master);
void dashboard_run(void);
void dashboard_stop(void);

#endif /* DASHBOARD_H */
