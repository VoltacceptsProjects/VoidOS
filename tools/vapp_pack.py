#!/usr/bin/env python3
"""Pack a manifest + payload into a binary .vapp package.

This is a *host-side* tool: VoidOS has no compiler or general-purpose
executable loader on-target, so .vapp packages are always built on a
regular machine and shipped in as GRUB Multiboot modules (see
vapps/README.md and tools/sync-vapps.sh). This script produces exactly
the byte layout kernel/fs.h expects (struct vapp_header), so anything
it writes will install cleanly via voidfs_install_vapp().

Layout of a .vapp file:

    [ vapp_header  ] fixed 132 bytes, matches kernel/fs.h exactly
    [ manifest     ] plain "key=value" text (see apps.c for keys read)
    [ payload      ] app-specific data, zero-padded to --payload-capacity

Usage:
    python3 tools/vapp_pack.py \\
        --name notepad.vapp \\
        --version 1.0 \\
        --manifest apps-src/notepad/manifest.txt \\
        --payload apps-src/notepad/payload.txt \\
        --payload-capacity 480 \\
        --out vapps/notepad.vapp
"""
import argparse
import struct
import sys

# Must match "struct vapp_header" in kernel/fs.h exactly: packed,
# little-endian (the kernel is built for x86-32).
#   char     magic[4]
#   uint16_t format_version
#   uint16_t header_size
#   uint32_t package_size
#   uint32_t manifest_offset
#   uint32_t manifest_size
#   uint32_t payload_offset
#   uint32_t payload_size
#   char     mime[40]
#   char     name[48]
#   char     app_version[16]
HEADER_FORMAT = "<4sHHIIIII40s48s16s"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
assert HEADER_SIZE == 132, HEADER_SIZE

VAPP_MIME = b"application/x-voidos-app"  # VOIDFS_VAPP_MIME in kernel/fs.h
NAME_MAX = 48   # VOIDFS_NAME_MAX
MIME_MAX = 40   # VOIDFS_MIME_MAX


def fixed(data: bytes, size: int, field: str) -> bytes:
    if len(data) >= size:
        raise ValueError(f"{field} ({len(data)} bytes) must be < {size} bytes")
    return data + b"\x00" * (size - len(data))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True, help='package file name, must end in ".vapp" (e.g. notepad.vapp)')
    parser.add_argument("--version", default="1.0", help="app_version string stored in the header")
    parser.add_argument("--manifest", required=True, help="path to the manifest text file")
    parser.add_argument("--payload", required=True, help="path to the payload file")
    parser.add_argument("--payload-capacity", type=int, default=480,
                         help="bytes reserved for the payload (payload is zero-padded up to this; "
                              "must be >= the payload file's size). This is how much room an app "
                              "like Notepad has to grow saved content by, not just the seed content.")
    parser.add_argument("--out", required=True, help="output .vapp path")
    args = parser.parse_args()

    if not args.name.endswith(".vapp"):
        parser.error('--name must end in ".vapp"')

    with open(args.manifest, "rb") as fh:
        manifest = fh.read()
    with open(args.payload, "rb") as fh:
        payload = fh.read()

    if len(payload) > args.payload_capacity:
        parser.error(f"payload is {len(payload)} bytes, exceeds --payload-capacity {args.payload_capacity}")

    manifest_offset = HEADER_SIZE
    manifest_size = len(manifest)
    payload_offset = manifest_offset + manifest_size
    payload_size = args.payload_capacity
    package_size = payload_offset + payload_size

    header = struct.pack(
        HEADER_FORMAT,
        b"VAPP",
        1,                      # format_version
        HEADER_SIZE,            # header_size
        package_size,
        manifest_offset,
        manifest_size,
        payload_offset,
        payload_size,
        fixed(VAPP_MIME, MIME_MAX, "mime"),
        fixed(args.name.encode("ascii"), NAME_MAX, "name"),
        fixed(args.version.encode("ascii"), 16, "app_version"),
    )

    padded_payload = payload + b"\x00" * (payload_size - len(payload))

    with open(args.out, "wb") as fh:
        fh.write(header)
        fh.write(manifest)
        fh.write(padded_payload)

    print(f"wrote {args.out} ({package_size} bytes: {HEADER_SIZE}b header + "
          f"{manifest_size}b manifest + {payload_size}b payload capacity)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
