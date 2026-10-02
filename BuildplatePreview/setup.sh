#!/usr/bin/env bash
set -euo pipefail

JAR_URL="https://piston-data.mojang.com/v1/objects/fd19469fed4a4b4c15b2d5133985f0e3e7816a8a/client.jar"
DEST="/root/solace/solace-server/staticdata/resourcepacks/java/minecraft"
JAR="$(mktemp --suffix=.jar)"

# Always clean up the jar, even if something fails
trap 'rm -f "$JAR"' EXIT

# Make sure unzip is available
command -v unzip >/dev/null 2>&1 || {
  echo "unzip is required (apt install unzip)" >&2
  exit 1
}

echo "Downloading client.jar..."
curl -fSL --retry 3 -o "$JAR" "$JAR_URL"

echo "Extracting assets/minecraft/ to $DEST ..."
mkdir -p "$DEST"
TMP_DIR="$(mktemp -d)"
unzip -q -o "$JAR" 'assets/minecraft/*' -d "$TMP_DIR"
cp -a "$TMP_DIR/assets/minecraft/." "$DEST/"
rm -rf "$TMP_DIR"

echo "Done. Jar removed."
