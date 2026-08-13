#!/usr/bin/env bash
# Mirrors every .vapp package from the online VoidOS application
# directory into vapps/ at the repo root, where the Makefile picks
# them up and bakes them into the ISO as Multiboot modules.
#
# VoidOS has no network stack, so this script - run on a regular
# machine, not on VoidOS itself - is the actual "install" step for
# apps published to the directory. Add or update a package by pushing
# a .vapp file to that repo, then re-run this script and rebuild the
# ISO.
#
# Usage:
#   tools/sync-vapps.sh
#
# Set VAPPS_REPO to point somewhere else (e.g. a fork) if needed.
set -euo pipefail

VAPPS_REPO="${VAPPS_REPO:-https://github.com/VoltacceptsProjects/VoidOS-Applications}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="$REPO_ROOT/vapps"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "Syncing .vapp packages from $VAPPS_REPO"
git clone --depth 1 "$VAPPS_REPO" "$WORK_DIR/repo" --quiet

mkdir -p "$DEST_DIR"
found=0
while IFS= read -r -d '' pkg; do
    found=$((found + 1))
    name="$(basename "$pkg")"
    cp "$pkg" "$DEST_DIR/$name"
    echo "  synced $name"
done < <(find "$WORK_DIR/repo" -name '*.vapp' -print0)

if [ "$found" -eq 0 ]; then
    echo "No .vapp files found in $VAPPS_REPO."
    echo "vapps/ was left as-is - nothing was removed."
else
    echo "Synced $found package(s) into $DEST_DIR/"
fi
echo "Run 'make' to rebuild the ISO with these packages included."
