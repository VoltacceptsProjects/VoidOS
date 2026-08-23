# VoidOS Debian 13 ISO builder

Builds a custom Debian 13 (trixie) live/installable ISO with GNOME and
only the requested applications, via `live-build`, run entirely inside
GitHub Actions.

## Layout

```
.github/workflows/build-iso.yml   <- the workflow (workflow_dispatch or push)
live-build/
  auto/config                     <- `lb config` call (distro, archive areas, ISO metadata)
  auto/clean
  config/package-lists/voidos.list.chroot       <- exact package list
  config/hooks/normal/
    0010-flatpak-apps.hook.chroot     <- installs Steam + Modrinth via Flathub
    0020-permissions.hook.chroot      <- fixes exec bits on included files
    0030-compile-schemas.hook.chroot  <- rebuilds glib schema cache for the wallpaper override
  config/includes.chroot/
    usr/local/bin/lotus                          <- your Lotus script
    usr/share/backgrounds/voltaccept/wallpaper.png
    usr/share/glib-2.0/schemas/99_voltaccept.gschema.override
    etc/profile.d/lotus.sh
```

## Running it

Push this to a repo and either push to `main` (path-filtered on
`live-build/**`) or run it manually from the Actions tab
(`workflow_dispatch`). The finished ISO is uploaded as a build
artifact (`voidos-debian13-iso`), not committed to the repo.

Build time is typically 30-90 minutes depending on GitHub's mirror
speed; the job timeout is set to 4 hours as headroom. GNOME + Steam's
flatpak alone will pull several GB, so the "free up disk space" step
in the workflow matters — GitHub's standard Linux runners only have
~14GB free by default.

## Design decisions worth knowing about

- **`gnome-core`, not `gnome`.** The `gnome` meta-package pulls in
  ~2GB of extra apps (Maps, Weather, games, Evolution, etc.) that
  weren't on your list. `gnome-core` + `gdm3` + `gnome-session` gives
  you GNOME Shell, Settings, Files, and a login manager without the
  extras. Swap the first three lines of the package list for `gnome`
  if you'd rather have the full suite.

- **Steam and Modrinth are installed as Flatpaks, not apt packages.**
  Native Steam on Debian needs i386 multiarch enabled and its
  `steam-installer` package just downloads the real client on first
  run anyway, and the Modrinth App isn't packaged for Debian/apt at
  all. Since Flatpak was already on your list, both are pulled from
  Flathub and installed system-wide *during the build* (in
  `0010-flatpak-apps.hook.chroot`), so they're baked into the ISO and
  don't need network access on first boot.

- **Drivers are a general-hardware bundle, not chipset-specific.**
  Since the ISO isn't built for one specific machine, the package
  list includes the open-source display stack (`xserver-xorg-video-all`,
  Mesa), common wifi/ethernet firmware packages, and PipeWire for
  audio. If you're targeting known hardware:
  - NVIDIA: add `nvidia-driver firmware-nvidia-gsp` (pulls in DKMS
    and non-free; increases build time and ISO size noticeably).
  - A specific wifi chipset only: trim the `firmware-*` list down to
    just that one.

- **Lock screen vs. GDM login screen wallpaper.** The gschema
  override sets the wallpaper as both the desktop background and the
  screensaver/lock screen background (what you see when you lock an
  already-logged-in session). The GDM *login* screen itself uses
  GNOME Shell's compiled theme resource rather than these keys —
  doable, but it's a separate step (rebuilding
  `gnome-shell-theme.gresource`) and wasn't included here to keep
  things from getting fragile. Ask if you want that added.

- **Lotus auto-runs on new terminals** via `/etc/profile.d/lotus.sh`
  (neofetch-style). Delete that file from `includes.chroot` if you'd
  rather it only run when typed manually.

- **Calamares installer included.** `--debian-installer` is set to
  `live`, and `calamares` / `calamares-settings-debian` are in the
  package list, so the ISO boots straight into the live GNOME desktop
  *and* has an "Install VoidOS" icon on the desktop (via
  `/etc/skel/Desktop/calamares.desktop`) for installing to disk.
  Revert `--debian-installer` to `none` and drop those two packages
  from the list if you want a live-only image again.
