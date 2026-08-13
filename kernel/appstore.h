#ifndef APPSTORE_H
#define APPSTORE_H

#include <stdint.h>

struct multiboot_info;

/* The "App Store" sidebar section: a card grid, one card per .vapp file
 * listed at the canonical online directory - VoidOS fetches and parses
 * that listing itself, at runtime, rather than a package having to be
 * baked onto the ISO at build time first (that older path still exists
 * for kernel/iwlwifi.c's own firmware blob and still works for any
 * .vapp already installed - see fs.c - this adds a second, live one).
 *
 * The directory endpoint returns JSON shaped like:
 *   {"directory": "htdocs", "files": [
 *     {"name": "notepad.vapp", "type": "file", "size": 715,
 *      "modified": "...", "url": "notepad.vapp"}, ...
 *   ]}
 * appstore.c's parser reads exactly that shape: it keeps every entry
 * whose "name" ends in ".vapp" (so "index.php", the script generating
 * the listing itself, is correctly skipped) and ignores unrecognised
 * keys, so the endpoint can grow fields without breaking this.
 *
 * "VoidOS fetches it itself" is true of this file's logic - the actual
 * bytes still have to come from somewhere, and there is no network
 * stack for them to come over the network with yet. See net.h for
 * exactly what net_http_get() can and can't do today; this file calls
 * it exactly the way it would once that's real, so nothing here needs
 * to change when it is. */

#define APPSTORE_HOST "voidos.infinityfree.io"

/* Fetches and (re)parses the directory listing via net_http_get(), then
 * cross-checks each entry against VoidFS (voidfs_file_at()) so already-
 * installed apps show as such. appstore_run() calls this on entry and
 * after every install attempt; call it directly only if you want to
 * force a refresh without opening the grid. */
void appstore_refresh(struct multiboot_info* mbi);

/* Full-screen card grid - same interaction model as apps_run_launcher()
 * in apps.c: Left/Right moves the selection, Enter or a click on a card
 * downloads that .vapp (net_http_get()) and installs it into VoidFS
 * (voidfs_install_vapp()), Esc returns to the shell. Every card shows
 * its current state (not installed / installed / an error from the
 * last attempt) instead of silently doing nothing on failure. */
void appstore_run(struct multiboot_info* mbi);

#endif
