#!/usr/bin/env python3
"""Validate that the KWin bridge is GPU-only and does not leak its preload."""

from __future__ import annotations

import re
import sys
from pathlib import Path


class ContractError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractError(message)


def require_all(source: str, needles: tuple[str, ...], filename: str) -> None:
    for needle in needles:
        require(needle in source, f"{filename} is missing {needle!r}")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} LINUX_ROOT", file=sys.stderr)
        return 2

    linux_root = Path(sys.argv[1]).resolve()
    interposer_path = linux_root / "kwin_gpu_interposer.cpp"
    capability_path = linux_root / "kwin_capability.c"
    dropin_path = linux_root / "packaging" / "vibeshine-kwin-gpu.conf.in"
    login_dropin_path = linux_root / "packaging" / "vibeshine-login-kwin-gpu.conf.in"
    cmake_path = linux_root.parent / "src" / "driver" / "CMakeLists.txt"

    interposer = interposer_path.read_text(encoding="utf-8")
    capability = capability_path.read_text(encoding="utf-8")
    dropin = dropin_path.read_text(encoding="utf-8")
    login_dropin = login_dropin_path.read_text(encoding="utf-8")
    cmake = cmake_path.read_text(encoding="utf-8")

    require_all(interposer, (
        'extern "C" int drmGetDevices2',
        'extern "C" int drmGetDevice2',
        'extern "C" int drmGetDeviceFromDevId',
        "DRM_BUS_FAUX",
        'std::strcmp(device->businfo.faux->name, "vibeshine")',
        "deviceinfo.pci->vendor_id == 0x10de",
        "matches > 1",
        "VIBESHINE_KWIN_RENDER_PCI",
        "vibeshine_kwin_gpu_interposer_abi",
        "return 3",
        "stop_kwin_preload_inheritance",
        "running_inside_kwin",
        'readlink("/proc/self/exe"',
        '"/usr/bin/kwin_wayland"',
        'unsetenv("LD_PRELOAD")',
    ), interposer_path.name)
    for function_name in ("drmGetDevice2", "drmGetDeviceFromDevId"):
        require(
            re.search(
                rf'extern "C" int {function_name}\(.*?'
                r"attach_vibeshine_display_to_nvidia\(\*device\);",
                interposer,
                flags=re.DOTALL,
            )
            is not None,
            f"{function_name} does not normalize the per-device Vibeshine identity",
        )
    require("drmIoctl" not in interposer, "physical-connector DRM ioctl suppression returned")
    require("HEADLESS_HDR" not in interposer, "physical headless-HDR policy returned")

    require_all(capability, (
        '"/usr/bin/kwin_wayland"',
        '"user.vibeshine.cap_sys_nice_removed"',
        "O_NOFOLLOW",
        "metadata->st_uid != 0",
        "S_IWGRP | S_IWOTH",
        'cap_from_text("cap_sys_nice=ep")',
        "cap_compare(actual, empty)",
        "cap_compare(actual, sys_nice)",
        "cap_set_fd(fd, empty)",
        "cap_set_fd(fd, sys_nice)",
        "fsetxattr(fd, marker_name",
        "fgetxattr(fd, marker_name",
        "fremovexattr(fd, marker_name)",
        'strcmp(argv[1], "prepare")',
        'strcmp(argv[1], "restore")',
    ), capability_path.name)
    require_all(dropin, (
        "Environment=LD_PRELOAD=@VIBESHINE_KWIN_GPU_LIBRARY_PATH@",
        "Environment=VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE=1",
    ), dropin_path.name)
    require("ExecStart=" not in dropin, "desktop drop-in replaces the vendor KWin wrapper")
    require("PATH=" not in dropin, "desktop drop-in intercepts KWin through PATH")
    require_all(login_dropin, (
        "UnsetEnvironment=LD_AUDIT",
        "Environment=LD_PRELOAD=@VIBESHINE_KWIN_GPU_LIBRARY_PATH@",
        "Environment=VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE=1",
    ), login_dropin_path.name)
    require("ExecStart=" not in login_dropin, "login drop-in replaces the vendor KWin command")
    require("BUILD_VIBESHINE_KWIN_GPU_BRIDGE" in cmake,
            "KWin bridge is not guarded by an explicit build option")
    require("plasma-login-kwin_wayland.service.d" in cmake,
            "KWin bridge is not installed for the Plasma login compositor")
    require("vibeshine_kwin_capability" in cmake,
            "KWin capability helper is not built with the bridge")
    require("kwin_preload_launcher" not in cmake,
            "obsolete dynamic-loader launcher is still built")

    for obsolete in (
        linux_root / "kwin_hdr_interposer.cpp",
        linux_root / "kwin_preload_launcher.cpp",
        linux_root / "packaging" / "vibeshine-kwin-hdr.conf.in",
        linux_root / "packaging" / "vibeshine-nvidia-display",
        linux_root / "packaging" / "vibeshine-nvidia-display.service.in",
    ):
        require(not obsolete.exists(), f"obsolete physical-display artifact remains: {obsolete.name}")

    print("KWin GPU bridge contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ContractError as error:
        print(f"KWin GPU bridge contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
