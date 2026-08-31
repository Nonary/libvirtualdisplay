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
    launcher_path = linux_root / "kwin_preload_launcher.cpp"
    dropin_path = linux_root / "packaging" / "vibeshine-kwin-gpu.conf.in"
    login_dropin_path = linux_root / "packaging" / "vibeshine-login-kwin-gpu.conf.in"
    cmake_path = linux_root.parent / "src" / "driver" / "CMakeLists.txt"

    interposer = interposer_path.read_text(encoding="utf-8")
    launcher = launcher_path.read_text(encoding="utf-8")
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
        "stop_preload_inheritance",
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

    require_all(launcher, (
        "dlopen(kInterposerPath",
        "VIBESHINE_KWIN_GPU_PRELOAD_ACTIVE",
        'setenv("LD_PRELOAD"',
        '"LD_AUDIT"',
        '"LD_LIBRARY_PATH"',
        "unsetenv(name)",
        'open(descriptor_path.c_str(), O_RDONLY | O_CLOEXEC)',
        "launch_metadata.st_ino != shadow_metadata.st_ino",
        'return fail("verify read-only KWin shadow")',
        "close(executable);\n\n  std::vector<char *> arguments",
        "fexecve(launch_image",
        "--verify-plasma-login-unit",
        "kInterposerAbi = 3",
    ), launcher_path.name)
    require("VIBESHINE_KWIN_PARENT_LD_PRELOAD" not in launcher,
            "launcher preserves an inherited preload across the greeter boundary")
    require("rename(temporary_name.data(), shadow_path" not in launcher,
            "launcher reopens a same-UID mutable KWin pathname")
    require(launcher.index("close(executable);\n\n  std::vector<char *> arguments") <
            launcher.index("fexecve(launch_image"),
            "launcher executes while its KWin shadow is still open for writing")
    require("Environment=LD_PRELOAD" not in dropin, "systemd drop-in leaks preload to KWin children")
    require("VIBESHINE_KWIN_GPU_LAUNCHER_INSTALL_DIR" in dropin,
            "systemd drop-in does not select the capability-free launcher")
    require_all(login_dropin, (
        "UnsetEnvironment=LD_AUDIT",
        "ExecCondition=",
        "--verify-plasma-login-unit",
        "ExecStart=\n",
        "VIBESHINE_KWIN_GPU_LAUNCHER_INSTALL_DIR",
        "/kwin_wayland --no-lockscreen",
        "--no-global-shortcuts",
        "--no-kactivities",
        "--inputmethod plasma-keyboard",
        "--locale1",
    ), login_dropin_path.name)
    require("Environment=LD_PRELOAD" not in login_dropin,
            "login-manager drop-in leaks preload to KWin children")
    require("BUILD_VIBESHINE_KWIN_GPU_BRIDGE" in cmake,
            "KWin bridge is not guarded by an explicit build option")
    require("plasma-login-kwin_wayland.service.d" in cmake,
            "KWin bridge is not installed for the Plasma login compositor")

    for obsolete in (
        linux_root / "kwin_hdr_interposer.cpp",
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
