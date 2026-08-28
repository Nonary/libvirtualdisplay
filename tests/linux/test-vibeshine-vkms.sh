#!/usr/bin/env bash

set -euo pipefail

SCRIPT_UNDER_TEST=${1:?usage: test-vibeshine-vkms.sh /path/to/vibeshine-vkms}
PACKAGING_DIR=$(dirname -- "$SCRIPT_UNDER_TEST")
SOCKET_UNIT="$PACKAGING_DIR/vibeshine-vkms-control.socket.in"
CONNECTION_SERVICE="$PACKAGING_DIR/vibeshine-vkms-control@.service.in"
TEST_ROOT=$(mktemp -d)

cleanup_test_root() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup_test_root EXIT

# shellcheck source=/dev/null
source "$SCRIPT_UNDER_TEST"

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

[[ -f "$SOCKET_UNIT" ]] || fail "missing socket unit: ${SOCKET_UNIT}"
[[ -f "$CONNECTION_SERVICE" ]] || fail "missing connection service: ${CONNECTION_SERVICE}"
grep -Fxq 'Accept=yes' "$SOCKET_UNIT" || fail 'control socket must use one service instance per connection'
if grep -Eq '^Service=' "$SOCKET_UNIT"; then
  fail 'Accept=yes control socket must derive its matching @.service template'
fi

assert_file_value() {
  local path=$1
  local expected=$2
  local actual

  [[ -f "$path" ]] || fail "missing file: ${path}"
  actual=$(<"$path")
  [[ "$actual" == "$expected" ]] || fail "${path}: expected '${expected}', found '${actual}'"
}

assert_link_target() {
  local path=$1
  local expected=$2
  local actual

  [[ -L "$path" ]] || fail "missing symbolic link: ${path}"
  actual=$(readlink -f -- "$path") || fail "could not resolve ${path}"
  [[ "$actual" == "$expected" ]] || fail "${path}: expected target ${expected}, found ${actual}"
}

CONFIGFS_TEST_ROOT="$TEST_ROOT/configfs"
mkdir -p -- "$CONFIGFS_TEST_ROOT/vibeshine-drm"
configure_paths "$CONFIGFS_TEST_ROOT"
# The sourced helper functions consume this test-only switch.
# shellcheck disable=SC2034
FAKE_CONFIGFS=1

ensure_display_backend || fail "preferred backend selection failed"
create_pool || fail "initial pool creation failed"
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1

for ((pipeline = 0; pipeline < VKMS_OUTPUT_COUNT; ++pipeline)); do
  plane="$VKMS_DEVICE_DIR/planes/plane${pipeline}"
  crtc="$VKMS_DEVICE_DIR/crtcs/crtc${pipeline}"
  encoder="$VKMS_DEVICE_DIR/encoders/encoder${pipeline}"
  connector="$VKMS_DEVICE_DIR/connectors/Virtual-$((pipeline + 1))"

  assert_file_value "$plane/type" 1
  assert_file_value "$crtc/writeback" 0
  assert_file_value "$connector/status" "$CONNECTOR_STATUS_DISCONNECTED"
  assert_link_target "$plane/possible_crtcs/crtc${pipeline}" "$crtc"
  assert_link_target "$encoder/possible_crtcs/crtc${pipeline}" "$crtc"
  assert_link_target "$connector/possible_encoders/encoder${pipeline}" "$encoder"
done

# The socket broker may connect and disconnect one provisioned output without
# rebuilding the DRM device or disturbing the other dormant connectors.
configfs_is_mounted() {
  return 0
}
response=$(printf 'connect Virtual-2\n' | control_connection) || fail "connect request failed"
[[ "$response" == "OK connected Virtual-2" ]] || fail "unexpected connect response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_CONNECTED"
response=$(printf 'status Virtual-2\n' | control_connection) || fail "status request failed"
[[ "$response" == "STATUS connected Virtual-2" ]] || fail "unexpected status response: ${response}"
validate_topology || fail "live connected topology was rejected"

# A second start must retain the exact four-pipeline object graph and must not
# reset a live connector back to dormant.
create_pool || fail "idempotent pool creation failed"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_CONNECTED"
for group in planes crtcs encoders connectors; do
  count=0
  for entry in "$VKMS_DEVICE_DIR/$group"/*; do
    [[ -e "$entry" || -L "$entry" ]] || continue
    ((count += 1))
  done
  [[ $count -eq $VKMS_OUTPUT_COUNT ]] || fail "${group}: expected 4 items after restart, found ${count}"
done

response=$(printf 'disconnect Virtual-2\n' | control_connection) || fail "disconnect request failed"
[[ "$response" == "OK disconnected Virtual-2" ]] || fail "unexpected disconnect response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_DISCONNECTED"
response=$(printf 'status Virtual-2\n' | control_connection) || fail "disconnected status request failed"
[[ "$response" == "STATUS disconnected Virtual-2" ]] || fail "unexpected disconnected status: ${response}"

for invalid_request in 'connect Virtual-0' 'connect Virtual-5' 'connect Virtual-1 extra' 'CONNECT Virtual-1'; do
  if response=$(printf '%s\n' "$invalid_request" | control_connection); then
    fail "invalid control request succeeded: ${invalid_request}"
  fi
  [[ "$response" == "ERROR malformed request" ]] || fail "unexpected protocol error: ${response}"
done

# Control must never follow a connector attribute symlink in the test model.
mv -- "$VKMS_DEVICE_DIR/connectors/Virtual-3/status" "$TEST_ROOT/Virtual-3.status"
printf 'outside' >"$TEST_ROOT/outside-status"
ln -s -- "$TEST_ROOT/outside-status" "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"
if response=$(printf 'connect Virtual-3\n' | control_connection); then
  fail "control followed a symbolic-link status attribute"
fi
[[ "$response" == "ERROR unsafe connector path" ]] || fail "unexpected unsafe-path response: ${response}"
assert_file_value "$TEST_ROOT/outside-status" outside
unlink -- "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"
mv -- "$TEST_ROOT/Virtual-3.status" "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"

# Cleanup must refuse to touch an instance containing paths it does not own.
mkdir -- "$VKMS_DEVICE_DIR/connectors/unmanaged"
if remove_pool; then
  fail "cleanup accepted an unmanaged connector"
fi
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1
[[ -d "$VKMS_DEVICE_DIR/connectors/Virtual-1" ]] || fail "safe cleanup changed a managed connector"
rmdir -- "$VKMS_DEVICE_DIR/connectors/unmanaged"

# A managed-looking name must not be allowed to redirect cleanup elsewhere.
mv -- "$VKMS_DEVICE_DIR/connectors/Virtual-1" "$TEST_ROOT/Virtual-1.saved"
mkdir -- "$TEST_ROOT/outside"
printf 'keep' >"$TEST_ROOT/outside/sentinel"
ln -s -- "$TEST_ROOT/outside" "$VKMS_DEVICE_DIR/connectors/Virtual-1"
if remove_pool; then
  fail "cleanup followed a managed-name symbolic link"
fi
assert_file_value "$TEST_ROOT/outside/sentinel" keep
unlink -- "$VKMS_DEVICE_DIR/connectors/Virtual-1"
mv -- "$TEST_ROOT/Virtual-1.saved" "$VKMS_DEVICE_DIR/connectors/Virtual-1"

# A global shutdown must leave the DRM object graph alive until KWin and other
# clients have closed their file descriptors. Manual service stops retain the
# normal full cleanup behavior used by hot reload and package removal.
system_is_stopping() {
  return 0
}
stop_pool || fail "shutdown-aware stop failed"
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1
[[ -d "$VKMS_DEVICE_DIR/connectors/Virtual-1" ]] || fail "global shutdown removed the live DRM pool"

system_is_stopping() {
  return 1
}
stop_pool || fail "manual stop pool removal failed"
[[ ! -e "$VKMS_DEVICE_DIR" ]] || fail "manual stop left the managed instance behind"

remove_pool || fail "idempotent pool removal failed"

# Unsupported systems fail without silently creating a CPU-backed stock-VKMS pool.
rmdir -- "$VKMS_CONFIGFS_ROOT"
# shellcheck disable=SC2329
modprobe() {
  return 1
}
if ensure_display_backend >/dev/null 2>&1; then
  fail "backend detection succeeded without vibeshine_drm"
fi
[[ ! -e "$VKMS_DEVICE_DIR" ]] || fail "managed pool appeared on module failure"

printf 'PASS: vibeshine-vkms shell tests\n'
