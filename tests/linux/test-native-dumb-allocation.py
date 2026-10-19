#!/usr/bin/env python3
"""Compile and execute the production C component with mocked kernel APIs."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver_root", type=Path)
    parser.add_argument("--baseline-object", type=Path)
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    driver = args.driver_root.resolve()
    with tempfile.TemporaryDirectory(prefix="vibeshine-dumb-component-") as temporary:
        output = args.output_dir.resolve() if args.output_dir else Path(temporary)
        output.mkdir(parents=True, exist_ok=True)
        versions = ((7, 2),) if args.baseline_object else ((6, 16), (6, 17), (6, 18), (7, 2))
        pages = (4096,) if args.baseline_object else (4096, 65536)
        for major, minor in versions:
            for page_size in pages:
                name = f"native-dumb-{major}.{minor}-{page_size}"
                binary = output / name
                command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-UNDEBUG", "-ffunction-sections", "-fdata-sections", "-no-pie",
                           f"-DPAGE_SIZE={page_size}U",
                           f"-DLINUX_VERSION_CODE={(major << 16) + (minor << 8)}",
                           "-I", str(tests / "kernel-shim"), "-I", str(driver),
                           str(driver / "vibeshine_drm_dumb.c"),
                           str(tests / "test-native-dumb-allocation.c")]
                if args.baseline_object:
                    command += ["-DVIBESHINE_TEST_BASELINE", str(args.baseline_object.resolve())]
                command += ["-Wl,--gc-sections", "-o", str(binary)]
                subprocess.run(command, check=True)
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                (output / f"{name}.log").write_text(result.stdout + result.stderr, encoding="utf-8")
                if result.returncode:
                    print(result.stdout + result.stderr, end="")
                    return result.returncode
                print(f"PASS: production .c, Linux {major}.{minor}, PAGE_SIZE={page_size}")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
