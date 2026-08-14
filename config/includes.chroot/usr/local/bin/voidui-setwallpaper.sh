#!/bin/sh
# set the VoidUI wallpaper if present
BG="/usr/share/backgrounds/voidui-wallpaper.jpg"
if [ -f "$BG" ]; then
  # attempt to set for primary monitor using xfconf-query (XFCE)
  xfconf-query -c xfce4-desktop -p /backdrop/screen0/monitor0/image-path -s "$BG" || true
  # As fallback try feh (if present)
  if command -v feh >/dev/null 2>&1; then
    feh --bg-scale "$BG" || true
  fi
fi
