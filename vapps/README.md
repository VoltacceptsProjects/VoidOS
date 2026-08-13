# vapps/

Every `.vapp` package here gets baked into `voidos.iso` and shows up as
a card in VoidOS's Applications launcher. This directory is a **local
mirror** of the online application directory:

https://github.com/VoltacceptsProjects/VoidOS-Applications

Run `tools/sync-vapps.sh` to refresh it from there, then `make` to
rebuild the ISO.

## Why a mirror, and not a live download

VoidOS's kernel has no network driver or TCP/IP stack, so it can't
reach GitHub itself while it's running. Instead:

1. `.vapp` packages are published to the GitHub directory above.
2. `tools/sync-vapps.sh` copies them into this folder on a regular
   machine.
3. The `Makefile` stages everything here into `iso/boot/vapps/` and
   `iso/boot/grub/grub.cfg` lists each one as a GRUB Multiboot module.
4. At boot, `voidfs_install_multiboot_modules()` (see
   `kernel/fs.c`) installs every module ending in `.vapp` into VoidFS.
5. `kernel/apps.c` discovers installed packages by mime type
   (`application/x-voidos-app`) and lists them in the launcher
   automatically - no kernel changes needed to add a new app.

So the GitHub repo is the canonical, always-up-to-date source of
"every .vapp that can be installed"; this folder plus the boot process
is how that reaches an actual VoidOS machine.

## Package format

A `.vapp` is a small binary container: a fixed 132-byte header
(`struct vapp_header` in `kernel/fs.h`), followed by a plain-text
manifest, followed by a payload. `kernel/apps.c` reads the manifest for:

- `name=`        display name shown on the launcher card
- `description=` one-line blurb shown on the card
- `kind=`        which built-in VoidOS runtime executes the app
  (currently only `text_editor` is implemented, used by the bundled
  Notepad example)

Build one with `tools/vapp_pack.py`:

```sh
python3 tools/vapp_pack.py \
    --name myapp.vapp \
    --version 1.0 \
    --manifest apps-src/myapp/manifest.txt \
    --payload apps-src/myapp/payload.txt \
    --payload-capacity 480 \
    --out vapps/myapp.vapp
```

Then check it against the real kernel struct before booting it:

```sh
gcc -Ikernel -o /tmp/test_vapp tools/test_vapp.c
/tmp/test_vapp vapps/myapp.vapp
```

## Adding a package to the boot ISO

1. Put it in `vapps/` (synced or hand-built).
2. Add a line to `iso/boot/grub/grub.cfg`:
   `module /boot/vapps/myapp.vapp`
3. `make run`

## notepad.vapp

The bundled example: a `text_editor`-kind package modelled on Windows
Notepad. Source is in `apps-src/notepad/`. Esc saves whatever you've
typed back into VoidFS and closes the app.
