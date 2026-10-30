#ifndef OCS_SERVER_H
#define OCS_SERVER_H

#include "app.h"

/* Blocks serving HTTP on a->cfg.listen. SIGTERM/SIGINT -> returns 0;
 * listen failure -> -1. */
int server_run(app_t *a);

#endif
