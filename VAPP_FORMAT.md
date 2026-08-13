# VoidOS application packages

VoidOS recognizes packages with the filename suffix `.vapp` and the MIME
type `application/x-voidos-app`.

## Package header

The package starts with the packed `struct vapp_header` declared in
`kernel/fs.h`:

| Field | Meaning |
| --- | --- |
| `magic` | Four bytes: `VAPP` |
| `format_version` | `1` |
| `header_size` | At least `sizeof(struct vapp_header)` |
| `package_size` | Exact byte length of the package |
| `manifest_offset` / `manifest_size` | Bounds of the package manifest |
| `payload_offset` / `payload_size` | Bounds of the application payload |
| `mime` | Exactly `application/x-voidos-app` |
| `name` | Optional package name ending in `.vapp` |
| `app_version` | Human-readable package version |

The manifest and payload bytes are copied as opaque data. VoidFS validates
their bounds but does not try to build or execute an application.

## Installation path

VoidOS uses a persistent ATA PIO-backed volume when it finds a formatted
VoidFS volume. If no formatted volume is found, it falls back to a RAM volume
for the current boot.

To make a package available at boot, add it as a GRUB Multiboot module in the
ISO's `grub.cfg`:

```text
module /boot/apps/example.vapp
```

`voidfs_install_multiboot_modules()` imports every module whose name ends in
`.vapp`, validates its header and MIME type, and stores it in the VoidFS
directory. The Files page displays each installed package, its MIME type, and
size, along with detected ATA drives.

## Formatting a drive

Formatting is never automatic. From the built-in Terminal, run:

```text
format voidfs
format confirm
```

The confirmation formats Drive 0 and uses a reserved VoidFS volume beginning
at LBA 2048. This is destructive to data in that reserved region, so use a
dedicated drive or disk image. VoidOS currently supports legacy IDE/ATA PIO
devices; AHCI and NVMe devices are reported but not mounted yet.