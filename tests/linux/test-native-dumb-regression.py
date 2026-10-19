#!/usr/bin/env python3
"""Run the actual historical driver callback red, then the production component green.

Requires prepared kernel headers and an x86-64 toolchain, but never loads a module.
The historical vkms_drv.c is compiled whole, not parsed or copied into a harness.
Only kernel instrumentation metadata and unrelated symbol visibility are adjusted
for the userspace link; executable instructions remain unchanged.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-build", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--baseline-ref", default="0ecc3739a5fc473cd3c71341c2ac48e2b39b4f23")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    repo = tests.parents[1]
    output = args.output_dir.resolve()
    source = output / "source"
    source.mkdir(parents=True, exist_ok=False)
    archive = subprocess.check_output(
        ["git", "-C", str(repo), "archive", args.baseline_ref, "linux/vibeshine-drm"])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(source, filter="data")
    driver = source / "linux/vibeshine-drm"
    original = driver / "vkms_drv.c"
    expected_blob = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", f"{args.baseline_ref}:linux/vibeshine-drm/vkms_drv.c"],
        text=True).strip()
    actual_blob = subprocess.check_output(["git", "hash-object", str(original)], text=True).strip()
    if actual_blob != expected_blob:
        raise RuntimeError("historical driver differs from its Git blob")
    shutil.copyfile(tests / "native-dumb-baseline-probe.c", driver / "baseline-probe.c")
    (driver / "vibeshine_drm_version.h").write_text(
        '#define VIBESHINE_DRM_VERSION "baseline-component-test"\n', encoding="utf-8")
    (driver / "Makefile").write_text(
        "obj-m += baseline-probe.o\n"
        "CFLAGS_REMOVE_baseline-probe.o += -pg -mfentry -mrecord-mcount "
        "-mfunction-return=thunk-extern -mindirect-branch=thunk-extern\n", encoding="utf-8")
    build = subprocess.run(
        ["make", "-C", str(args.kernel_build.resolve()), f"M={driver}", "LLVM=1", "-j2",
         "KCFLAGS=-ffunction-sections -fdata-sections -fno-stack-protector -fno-lto",
         "baseline-probe.o"], capture_output=True, text=True)
    (output / "compile.log").write_text(build.stdout + build.stderr, encoding="utf-8")
    build.check_returncode()
    compiled = driver / "baseline-probe.o"
    userspace = output / "baseline-userspace.o"
    # Localizing init_module avoids interposing libc's syscall wrapper. Dropping
    # ftrace patch-location metadata lets normal linker section GC remove code
    # unreachable from the callback. No instruction section is modified.
    subprocess.run(
        ["objcopy", "--strip-debug", "--keep-global-symbol=vibeshine_baseline_dumb_create",
         "--remove-section=__patchable_function_entries", str(compiled), str(userspace)], check=True)
    section = ".text.vibeshine_baseline_dumb_create"
    instruction_files = []
    for name, obj in (("before", compiled), ("after", userspace)):
        instructions = output / f"callback-{name}.bin"
        subprocess.run(["objcopy", f"--dump-section={section}={instructions}", str(obj)], check=True)
        instruction_files.append(instructions.read_bytes())
    if not instruction_files[0] or instruction_files[0] != instruction_files[1]:
        raise RuntimeError("baseline callback instructions changed during userspace preparation")
    disassembly = subprocess.check_output(["objdump", "-dr", f"--section={section}", str(userspace)], text=True)
    (output / "callback-disassembly.txt").write_text(disassembly, encoding="utf-8")
    runner = [sys.executable, str(tests / "test-native-dumb-allocation.py"),
              str(repo / "linux/vibeshine-drm")]
    red = subprocess.run(runner + ["--baseline-object", str(userspace), "--output-dir", str(output / "red")],
                         capture_output=True, text=True)
    (output / "red.log").write_text(red.stdout + red.stderr, encoding="utf-8")
    if red.returncode != 1 or "3440x1440 returned pitch=13760" not in red.stdout or "expected pitch=13824" not in red.stdout:
        raise RuntimeError("baseline did not fail on the expected observable pitch mismatch: " + red.stdout + red.stderr)
    green = subprocess.run(runner + ["--output-dir", str(output / "green")], capture_output=True, text=True)
    (output / "green.log").write_text(green.stdout + green.stderr, encoding="utf-8")
    green.check_returncode()
    evidence = {
        "baseline_ref": args.baseline_ref,
        "baseline_driver_blob": actual_blob,
        "baseline_driver_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
        "baseline_callback_instructions_sha256": hashlib.sha256(instruction_files[0]).hexdigest(),
        "baseline_compiled_whole_with_real_kernel_headers": True,
        "baseline_exit": red.returncode,
        "baseline_observed_pitch": 13760,
        "corrected_observed_pitch": 13824,
        "logical_resolution": [3440, 1440],
        "corrected_production_source": "linux/vibeshine-drm/vibeshine_drm_dumb.c",
        "corrected_component_exit": green.returncode,
        "amd_runtime_tested": False,
    }
    (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(red.stdout, end="")
    print(green.stdout, end="")
    print("PASS: observed historical callback failure and corrected production-component behavior")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
