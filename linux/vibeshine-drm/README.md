# Vibeshine HDR virtual DRM driver

This directory contains Vibeshine's out-of-tree virtual KMS driver. It is
derived from the Linux 7.2 VKMS driver and retains the upstream SPDX license
identifiers on every source file. The imported upstream revision is Linux tag `v7.2`
(`237a1c39e8df`).

Vibeshine's changes give each configfs-created virtual connector a stable HDR10
monitor contract:

- a CTA-861 EDID advertising BT.2020, PQ, HLG, and static HDR metadata;
- atomic `HDR_OUTPUT_METADATA`, `Colorspace`, and 8-16 `max bpc` properties;
- 10-bit RGB plane formats in addition to upstream VKMS formats; and
- an independent `/sys/kernel/config/vibeshine-drm` configfs namespace, so the
  driver can coexist with a distribution's normal `vkms` module.

The EDID is generated deterministically by `generate_hdr_edid.py`. Update the
generator, not `vibeshine_hdr_edid.h`, and regenerate the header with:

```bash
./generate_hdr_edid.py --header vibeshine_hdr_edid.h
```

The current source targets the DRM APIs in Linux 7.2. The installer builds it
for the running kernel (or registers it with DKMS when available). If it cannot
be built or loaded, managed virtual displays remain unavailable; the helper
does not substitute CPU-backed upstream VKMS scanout.

The installer signs both DKMS and direct builds with the standard local DKMS
key at `/var/lib/dkms/mok.key`. On Secure Boot systems that boot through shim,
enroll its public certificate once:

```bash
sudo /usr/libexec/vibeshine/vibeshine-drm-install enroll-key
# Reboot, choose "Enroll MOK", and confirm with the temporary password.
/usr/libexec/vibeshine/vibeshine-drm-install signing-status
```

DKMS signs rebuilt modules with the same key after future kernel and Vibeshine
updates. Direct bootloaders that do not pass through shim cannot expose MOK
keys to the kernel; those systems must add shim to their boot chain or use
their distribution's equivalent trusted-module signing mechanism.
