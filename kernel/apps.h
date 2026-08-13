#ifndef APPS_H
#define APPS_H

#include <stdint.h>

/* The application registry is deliberately static for this first runtime:
 * applications are linked into the kernel and launched through a common
 * event loop. This keeps the feature usable before VoidOS has a filesystem
 * or an executable loader. */

void apps_print_launcher(void);
void apps_run_launcher(void);

#endif