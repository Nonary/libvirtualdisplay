# Vibeshine HDR virtual DRM driver

This directory contains Vibeshine's out-of-tree virtual KMS driver. It is
derived from the Linux 7.2 VKMS driver and retains the upstream SPDX license
identifiers on every source file. The imported upstream revision is Linux tag `v7.2`
(`237a1c39e8df`).

Vibeshine's changes give each configfs-created virtual connector a stable HDR10
monitor contract:

- a CTA-861 EDID advertising BT.2020, PQ, HLG, and static HDR metadata;
- atomic `HDR_OUTPUT_METADATA`, `Colorspace`, and 8-16 `max bpc` properties;
- adaptive-sync capability with exactly one synthetic vblank per submitted
  frame, independent of the disabled fixed-rate timer and nominal mode period;
- 10-bit RGB plane formats in addition to upstream VKMS formats;
- versioned, read-only presentation and frame-export ioctls so direct KMS
  capture can follow completed scanout changes and import the exact presented
  DMA-BUF instead of polling KMS state at a fixed rate;
- late reboot quiescing which drains virtual scanout and releases imported
  renderer-GPU framebuffers before physical GPU device shutdown; and
- an independent `/sys/kernel/config/vibeshine-drm` configfs namespace, so the
  driver can coexist with a distribution's normal `vkms` module.

## Presentation and frame-export ABI

`vibeshine_drm_uapi.h` defines `DRM_VIBESHINE_WAIT_PRESENT`. Each CRTC owns a
monotonic presentation sequence. The sequence advances after an atomic commit
which can change that CRTC's planes, mode, color state, or connector state has
completed. A caller supplies its last sequence and may block for up to one
second; the ioctl returns the newest sequence and its `CLOCK_MONOTONIC`
timestamp. Consumers deliberately coalesce sequence gaps and import only the
latest scanout buffer.

The response also reports when a newer atomic state has been submitted but is
not presented yet. Capture waits until that pending count reaches zero before
dequeueing a frame.

`DRM_VIBESHINE_GET_FRAME` pins and exports the newest completed primary-plane
framebuffer together with its presentation sequence, monotonic timestamp,
format, modifier, pitches, offsets, original PRIME DMA-BUF descriptors, and
optional per-plane sync-file snapshots. The framebuffer reference is exchanged under the
same lock as the sequence and timestamp, so a later KMS software-state swap
cannot be mistaken for the presentation which generated an earlier
notification. Sequence gaps are deliberately coalesced to the newest completed
frame rather than growing an unbounded kernel queue.

An explicit configfs teardown disables scanout before unplugging the DRM
device. During a global shutdown the service instead preserves the device while
KWin closes its descriptors; a kernel reboot notifier then performs the same
atomic quiesce after userspace has exited but before physical GPU devices shut
down. This two-phase ordering prevents both a live-compositor unplug and stale
imported DMA-BUF references crossing into NVIDIA teardown.

The managed configfs pool provisions exactly one primary plane per CRTC. It
does not create cursor or overlay planes, forcing KWin to composite the cursor,
desktop overlays, and application content into the exported framebuffer.
Non-linear imported PRIME objects are passed through to userspace as the
renderer GPU's original DMA-BUF; the export ioctl fails rather than silently
copying a virtual-device shmem buffer.

Both ioctls are observational and cannot modify display state. Presentation
waiting does not require DRM master ownership; framebuffer export requires
`CAP_SYS_ADMIN`, matching direct KMS capture. ABI additions must preserve the fixed-width
version 1 structures and use their reserved fields for compatible extension. To keep an
untrusted card-node client from amplifying every presentation into unbounded
wakeup work, each CRTC accepts at most 64 concurrent blocking waits; additional
blocking requests fail transiently with `EBUSY`, while zero-timeout queries are
never subject to that limit.

The EDID is generated deterministically by `generate_hdr_edid.py`. Update the
generator, not `vibeshine_hdr_edid.h`, and regenerate the header with:

```bash
./generate_hdr_edid.py --header vibeshine_hdr_edid.h
```

The current source is derived from Linux 7.2 and includes the compatibility
shim needed for the Linux 7.1 DRM atomic API. The installer builds it for the
running kernel (or registers it with DKMS when available). If it cannot be
built or loaded, managed virtual displays remain unavailable; the helper does
not substitute CPU-backed upstream VKMS scanout.

The installer generates a persistent local DKMS key at
`/var/lib/dkms/mok.key`, signs every DKMS or direct build, and verifies the
embedded module signature before accepting the installation. Stock Arch and
CachyOS kernels need no separate signing command: accept the package manager's
normal installation confirmation and the module is built, signed, and verified
automatically, including with Secure Boot through Limine or systemd-boot.

Only a custom kernel that enforces trusted module signatures needs shim and a
one-time MOK authorization. The Arch package installation detects this and
launches the authorization prompt automatically. Confirm it, reboot, and
approve the pending firmware confirmations once. A noninteractive package
frontend that cannot display the prompt should be retried from a terminal.

DKMS signs rebuilt modules with the same key after future kernel and Vibeshine
updates. The helper requests both certificate enrollment and Linux MOK-list
trust, because enrollment alone does not put MOK keys in the kernel's trusted
module keyring.

Stock Arch Linux and CachyOS kernels currently permit a correctly signed but
untrusted external module and record the normal external-module taint, so a
direct Limine or systemd-boot installation remains loadable. Kernels that
enforce trusted module signatures must boot through shim and complete the MOK
requests, or use a custom kernel that embeds the local certificate. An `sbctl`
owner key authorizes EFI programs; it is not imported into Linux's trusted
module keyring and cannot by itself authorize an out-of-tree module.
