#!/usr/bin/env python3

import os
import socket
import subprocess
import sys


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


if len(sys.argv) != 2:
    fail("usage: test-vkms-peercred.py /path/to/vibeshine-vkms-peercred")

helper = os.path.abspath(sys.argv[1])
parent_socket, child_socket = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    result = subprocess.run(
        [helper, "/bin/sh", "-c", 'printf "%s\\n" "$1"', "peer-uid"],
        stdin=child_socket,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
finally:
    parent_socket.close()
    child_socket.close()

if result.returncode != 0:
    fail(f"peer credential handoff failed: {result.stderr.strip()}")
if result.stdout.strip() != str(os.getuid()):
    fail(f"expected peer uid {os.getuid()}, found {result.stdout.strip()!r}")

result = subprocess.run(
    [helper, "/bin/true"],
    stdin=subprocess.DEVNULL,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
    check=False,
)
if result.returncode == 0:
    fail("non-socket standard input was accepted")

print("PASS: vibeshine-vkms SO_PEERCRED tests")
