#ifndef APPS_H
#define APPS_H

#include <stdint.h>

/* The launcher itself still ships two built-in system utilities
 * (Calculator, Terminal) that need direct access to VoidOS internals -
 * "format voidfs" and friends aren't things a distributable package
 * should be able to do.
 *
 * Every other entry in the launcher is discovered at boot from VoidFS:
 * any installed file with mime VOIDFS_VAPP_MIME is a .vapp package (see
 * fs.h) and gets its own card, driven by the small manifest carried
 * inside the package. VoidOS itself has no network stack, so it never
 * reaches out to the internet directly - .vapp packages arrive as
 * Multiboot modules that GRUB loads alongside the kernel (see
 * voidfs_install_multiboot_modules() in fs.c) and are installed into
 * VoidFS at boot. The canonical, always-up-to-date source for which
 * .vapp packages exist is the online application directory:
 *
 *   https://github.com/VoltacceptsProjects/VoidOS-Applications
 *
 * The vapps/ directory at the repo root mirrors packages from there and
 * is what actually gets baked into the ISO - see vapps/README.md and
 * tools/sync-vapps.sh. */

void apps_print_launcher(void);
void apps_run_launcher(void);

#endif