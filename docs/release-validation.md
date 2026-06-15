# Release Validation

libvirtualdisplay release packages are CI-built driver/tool bundles for the
Sunshine virtual display stack.

## Signing Channel

- Release packages use the project's self-signed driver package.
- Consumers must install the driver in environments that accept this package
  signing model.

## Release Evidence

Keep these artifacts with each release record:

- Driver package with INF, CAT, DLL, and PDB.
- Tool package with `virtualdisplay.exe`, `virtualdisplay_broker.exe`, and
  `virtualdisplay_probe.exe`, plus the Vulkan layer (`VkLayer_sunshine_hdr.dll`
  and `VkLayer_sunshine_hdr.json`).
- Exact commit SHA, tag, Windows SDK version, WDK version, and ZIP SHA256.
- Generated evidence JSON attached to the GitHub release.

## Validation

Run functional validation for each supported release candidate:

- Install the package.
- Create and remove a permanent virtual display.
- Create, feed, and release a temporary virtual display.
- Run the probe tools for the relevant display mode and HDR scenarios.

## Functional Matrix

At minimum, release validation must cover:

- Windows 10 22H2 SDR behavior.
- Windows 11 23H2 or newer HDR-capable behavior.
- Fresh install.
- Upgrade install from the previous released package.
- Driver disable/enable or PnP stop/start.
- Permanent display identity retention across restart.
- Temporary display lease expiry and explicit release.

## Release Blockers

A production release is blocked until resolved when any of these are true:

- The shipped package contains sample identifiers, a permissive control
  interface ACL, or undocumented protocol/ABI changes.
- CI packaging fails or the release ZIP/evidence JSON is missing.
- Functional install, upgrade, identity-retention, or cleanup validation fails.
