# VoidOS

A modern, glassmorphism-styled Linux distribution built on **Debian 12 (bookworm)** using `live-build`.
Desktop: XFCE4 + `picom` (real background blur/transparency) + a custom "VoidGlass" GTK/xfwm theme + a Plank dock.

VoidOS is not a from-scratch kernel — it uses the stock Debian/Linux kernel and packaging tools, and layers
branding, theming, and a curated package set on top via `live-build`. This is the standard, realistic way to
build a custom Linux OS; writing a Linux kernel itself is a multi-decade undertaking done by thousands of
contributors, so this project reuses it (as every distro does) and focuses on the part that's actually yours:
the look, feel, and package selection.

## What's in here

```
voidos/
├── .github/workflows/build.yml     # GitHub Actions: builds a bootable VoidOS ISO
├── config/
│   ├── package-lists/voidos.list.chroot   # packages installed into the ISO
│   ├── hooks/                              # chroot scripts: rsvg shim + branding + theme install
│   └── includes.chroot/                   # files copied verbatim into the ISO's filesystem
│       ├── etc/skel/.config/picom.conf            # blur/glass compositor config
│       ├── usr/share/themes/VoidGlass-gtk/        # GTK3 glass theme (CSS)
│       ├── usr/share/themes/VoidGlass-xfwm/       # window border theme
│       ├── usr/share/backgrounds/voidos/          # generated wallpaper
│       ├── usr/share/plymouth/themes/voidos/      # boot splash
│       └── etc/lightdm/...                        # login screen theme
└── auto/config                     # live-build configuration script
```

## Build it yourself (locally, on a Debian/Ubuntu machine)

```bash
sudo apt update
sudo apt install -y live-build
cd voidos
sudo lb clean --purge
chmod +x auto/config
sudo ./auto/config    # runs auto/config directly: sets distribution=bookworm, arch, etc.
sudo lb build
```

> **Note:** Always run `chmod +x auto/config && sudo ./auto/config` (not bare `lb config`)
> before `lb build`, and re-run it after any `lb clean` or edit to `auto/config`. Bare
> `lb config` only auto-executes `auto/config` if it's marked executable in your working
> copy - if it isn't (e.g. because the repo was pushed via GitHub's web upload, which
> strips the executable bit, rather than `git push`), `lb config` silently falls back to
> bare defaults based on your host OS instead of our Debian bookworm settings. Also, if
> you're building on an **Ubuntu** host/runner, live-build defaults to Ubuntu "mode"
> (mirror/keyring) even when you set `--distribution bookworm`; `auto/config` now passes
> `--mode debian` explicitly to force Debian's mirrors and keyring regardless of host OS.
> It also passes `--security false`, since the live-build version on current GitHub
> runners generates an outdated `security.debian.org` suite line (`bookworm/updates`)
> that 404s - Debian renamed that suite to `bookworm-security` a while back. Installed
> systems can still be pointed at the current security repo manually after install.
> `linux-image-amd64` is listed explicitly in `config/package-lists/voidos.list.chroot`
> as a safety net so the kernel is always pulled in regardless of flavour-detection
> behavior. `auto/config` also no longer passes `--linux-flavours amd64`: doing so forces
> live-build to verify the kernel package by downloading a merged `Contents-amd64.gz`
> index directly under `dists/bookworm/`, which Debian no longer publishes at that path
> (it's now only published per-component, e.g. `dists/bookworm/main/Contents-amd64.gz`),
> so that lookup 404s. `amd64` is already live-build's own default flavour for this
> architecture, so passing it explicitly was redundant and only triggered the broken check.

This produces `live-image-amd64.hybrid.iso` — flash it with `dd` or `Rufus`/`balenaEtcher`, or boot it directly in a VM (VirtualBox/QEMU/UTM).

> **Note on hook scripts:** local chroot hooks live directly under `config/hooks/*.chroot`
> (flat, no `live/`/`normal/` subfolder, no `.hook.` infix — e.g. `config/hooks/0100-branding.chroot`).
> This is *not* the layout documented in the current upstream Debian Live Manual
> (`config/hooks/live/NAME.hook.chroot`) — that convention belongs to the actively
> maintained Debian `live-build` package. The `live-build` shipped by Ubuntu (and thus by
> `ubuntu-latest` GitHub runners) is a much older `3.0~a57` snapshot from 2012 that never
> got that rewrite, and it silently finds zero hooks — no error, no log output — if they're
> placed in `config/hooks/live/`. If you add a new hook and it doesn't seem to run, check
> the build log for its `echo` output right after the `P: Begin executing hooks...` line;
> if nothing printed, it's in the wrong place.

## Build it with GitHub Actions (recommended — this is the workflow you asked for)

Just push this repo to GitHub. The workflow at `.github/workflows/build.yml`:

1. Spins up an `ubuntu-latest` runner
2. Installs `live-build` and dependencies
3. Runs `lb build` inside this repo
4. Uploads the resulting `.iso` as a downloadable build artifact
5. Optionally attaches the ISO to a GitHub Release when you push a tag like `v0.1.0`

You can trigger it manually from the **Actions** tab ("Run workflow"), on every push to `main`, or on a version tag.
Build takes roughly 20–40 minutes on the free GitHub-hosted runners.

## Customizing

- **Add/remove packages**: edit `config/package-lists/voidos.list.chroot`
- **Change wallpaper**: replace `config/includes.chroot/usr/share/backgrounds/voidos/wallpaper.svg`
- **Tweak glass effect** (blur strength, opacity, rounding): edit `config/includes.chroot/etc/skel/.config/picom.conf`
- **Change accent color**: edit the `@define-color accent` line in
  `config/includes.chroot/usr/share/themes/VoidGlass-gtk/gtk-3.0/gtk.css`
- **Distro name/branding**: edit `config/hooks/0100-branding.chroot`

## Why XFCE + picom instead of GNOME/KDE?

XFCE is lightweight and very configurable, which makes it easy to reskin convincingly and keeps boot/login
fast — good for "easy to use." `picom` gives real, hardware-accelerated background blur and transparency
(the actual mechanic behind a glassmorphism look), which is layered on top of a custom GTK3 stylesheet and
window-border theme so panels, the dock, menus, and windows all pick up frosted-glass panes, soft shadows,
and rounded corners consistently.
