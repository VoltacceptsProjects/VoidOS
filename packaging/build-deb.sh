#!/bin/sh
# Builds voidwm_<version>_amd64.deb from this source tree.
#
# Usage: packaging/build-deb.sh <version> [outdir]
#   packaging/build-deb.sh 1.1.0 dist/
#
# Requires: build-essential, pkg-config, libx11-dev, libxext-dev,
# libcairo2-dev, libpango1.0-dev, dpkg-deb (dpkg-dev package).
set -eu

VERSION="${1:?usage: build-deb.sh <version> [outdir]}"
OUTDIR="${2:-.}"
ARCH=amd64
PKG=voidwm
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
PKGDIR="$WORK/${PKG}_${VERSION}_${ARCH}"

trap 'rm -rf "$WORK"' EXIT

cd "$ROOT"
make clean
make

mkdir -p "$PKGDIR/DEBIAN"
make install DESTDIR="$PKGDIR" PREFIX=/usr

mkdir -p "$PKGDIR/usr/share/doc/$PKG"
install -m644 packaging/copyright "$PKGDIR/usr/share/doc/$PKG/copyright"
if [ -f "packaging/changelog" ]; then
    gzip -9n packaging/changelog -c > "$PKGDIR/usr/share/doc/$PKG/changelog.Debian.gz"
fi

INSTALLED_SIZE=$(du -sk --exclude=DEBIAN "$PKGDIR" | cut -f1)

sed -e "s/@VERSION@/${VERSION}/" \
    -e "s/@INSTALLED_SIZE@/${INSTALLED_SIZE}/" \
    packaging/control.in > "$PKGDIR/DEBIAN/control"

chmod -R go=rX "$PKGDIR"
mkdir -p "$OUTDIR"
dpkg-deb --root-owner-group --build "$PKGDIR" "$OUTDIR/${PKG}_${VERSION}_${ARCH}.deb"
echo "built $OUTDIR/${PKG}_${VERSION}_${ARCH}.deb"
