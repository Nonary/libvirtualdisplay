# Vibeshine HDR virtual DRM driver

This directory contains Vibeshine's out-of-tree virtual KMS driver. It is
derived from the Linux 7.2 VKMS driver and retains the upstream SPDX license
identifiers on every source file. The imported upstream revision is Linux tag `v7.2`
(`237a1c39e8df`).

Vibeshine's changes give each configfs-created virtual connector a stable HDR10
monitor contract:

- a CTA-861 EDID advertising BT.2020, PQ, HLG, and static HDR metadata;
- atomic `HDR_OUTPUT_METADATA`, `Colorspace`, and 8-16 `max bpc` properties;
- 10-bit RGB plane formats in addition to upstream VKMS formats;
- a versioned, read-only presentation-wait ioctl so direct KMS capture can
  follow completed scanout changes instead of polling at a fixed rate; and
- an independent `/sys/kernel/config/vibeshine-drm` configfs namespace, so the
  driver can coexist with a distribution's normal `vkms` module.

## Presentation notification ABI

`vibeshine_drm_uapi.h` defines `DRM_VIBESHINE_WAIT_PRESENT`. Each CRTC owns a
monotonic presentation sequence. The sequence advances after an atomic commit
which can change that CRTC's planes, mode, color state, or connector state has
completed. A caller supplies its last sequence and may block for up to one
second; the ioctl returns the newest sequence and its `CLOCK_MONOTONIC`
timestamp. Consumers deliberately coalesce sequence gaps and import only the
latest scanout buffer.

The response also reports when a newer atomic state has been submitted but is
not presented yet. Capture waits until that pending count reaches zero before
exporting the current plane framebuffer, so a later software state swap cannot
be mistaken for the presentation which generated an earlier notification.

The ioctl is observational: it cannot modify display state and does not require
DRM master ownership. ABI additions must preserve the fixed-width version 1
structure and use its reserved fields for compatible extension. To keep an
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
