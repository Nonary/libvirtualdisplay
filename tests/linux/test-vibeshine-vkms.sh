#!/usr/bin/env bash

set -euo pipefail

SCRIPT_UNDER_TEST=${1:?usage: test-vibeshine-vkms.sh /path/to/vibeshine-vkms}
PACKAGING_DIR=$(dirname -- "$SCRIPT_UNDER_TEST")
SOCKET_UNIT="$PACKAGING_DIR/vibeshine-vkms-control.socket.in"
CONNECTION_SERVICE="$PACKAGING_DIR/vibeshine-vkms-control@.service.in"
SYSUSERS_FILE="$PACKAGING_DIR/vibeshine-vkms.sysusers"
QUIESCE_HELPER="$PACKAGING_DIR/vibeshine-vkms-quiesce"
SERVICE_UNIT="$PACKAGING_DIR/vibeshine-vkms.service.in"
SETUP_SERVICE="$PACKAGING_DIR/vibeshine-drm-setup.service.in"
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
[[ -f "$SYSUSERS_FILE" ]] || fail "missing sysusers file: ${SYSUSERS_FILE}"
[[ -x "$QUIESCE_HELPER" ]] || fail "missing quiesce helper: ${QUIESCE_HELPER}"
grep -Fq '/vibeshine-vkms-quiesce' "$SERVICE_UNIT" || fail 'pool service must quiesce the driver after its shutdown-aware stop'
grep -Fq 'Before=systemd-user-sessions.service display-manager.service' "$SERVICE_UNIT" || \
  fail 'pool service must remain active until all graphical user sessions have stopped'
grep -Fxq 'RuntimeDirectory=vibeshine/vkms-leases' "$SERVICE_UNIT" || \
  fail 'pool service must provision the broker lease directory before accepting connections'
grep -Fxq 'RuntimeDirectoryMode=0700' "$SERVICE_UNIT" || \
  fail 'broker lease directory must remain root-private'
grep -Fxq 'g vibeshine-vkms - -' "$SYSUSERS_FILE" || fail 'sysusers file must provision the dedicated socket group'
grep -Fxq 'Accept=yes' "$SOCKET_UNIT" || fail 'control socket must use one service instance per connection'
grep -Fxq 'SocketGroup=vibeshine-vkms' "$SOCKET_UNIT" || fail 'control socket must use its dedicated access group'
grep -Fxq 'MaxConnections=16' "$SOCKET_UNIT" || fail 'control socket must cap concurrent root helpers'
grep -Fxq 'MaxConnectionsPerSource=4' "$SOCKET_UNIT" || fail 'control socket must cap each peer source'
grep -Fxq 'RuntimeMaxSec=5s' "$CONNECTION_SERVICE" || fail 'control helper must have a runtime deadline'
grep -Fxq 'MemoryMax=32M' "$CONNECTION_SERVICE" || fail 'control helper must have a memory ceiling'
grep -Fxq 'SuccessExitStatus=4 5' "$SETUP_SERVICE" || fail 'expected reboot and enrollment outcomes must not latch setup failed'
if grep -Fq 'TriggerLimit' "$SOCKET_UNIT"; then
  fail 'control socket must not enter a persistent failed state after a request burst'
fi
if grep -Fxq 'SocketGroup=video' "$SOCKET_UNIT"; then
  fail 'control socket must not grant every video-group member mutation access'
fi
if grep -Eq '^Service=' "$SOCKET_UNIT"; then
  fail 'Accept=yes control socket must derive its matching @.service template'
fi
grep -Fq '/vibeshine-vkms-peercred ' "$CONNECTION_SERVICE" || fail 'control service must obtain peer credentials before invoking the broker'
if grep -Fxq 'ExecStart=@VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR@/vibeshine-vkms control' "$CONNECTION_SERVICE"; then
  fail 'control service must not invoke the root broker without peer credentials'
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
configure_lease_paths "$TEST_ROOT/vkms-leases"
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
OWNER_UID=11001
OTHER_UID=11002
LEASE_PATH="$LEASE_ROOT/Virtual-2.owner"
response=$(printf 'connect Virtual-2\n' | control_connection "$OWNER_UID") || fail "connect request failed"
[[ "$response" == "OK connected Virtual-2" ]] || fail "unexpected connect response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_CONNECTED"
assert_file_value "$LEASE_PATH" "$OWNER_UID"
response=$(printf 'status Virtual-2\n' | control_connection "$OTHER_UID") || fail "status request failed"
[[ "$response" == "STATUS connected Virtual-2" ]] || fail "unexpected status response: ${response}"
assert_file_value "$LEASE_PATH" "$OWNER_UID"

if response=$(printf 'connect Virtual-2\n' | control_connection "$OTHER_UID"); then
  fail "cross-uid connect request succeeded"
fi
[[ "$response" == "ERROR connector owned by another uid" ]] || fail "unexpected cross-uid connect response: ${response}"
if response=$(printf 'disconnect Virtual-2\n' | control_connection "$OTHER_UID"); then
  fail "cross-uid disconnect request succeeded"
fi
[[ "$response" == "ERROR connector owned by another uid" ]] || fail "unexpected cross-uid disconnect response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_CONNECTED"
assert_file_value "$LEASE_PATH" "$OWNER_UID"
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

response=$(printf 'disconnect Virtual-2\n' | control_connection 0) || fail "root handoff disconnect failed"
[[ "$response" == "OK disconnected Virtual-2" ]] || fail "unexpected root handoff response: ${response}"
assert_file_value "$VKMS_DEVICE_DIR/connectors/Virtual-2/status" "$CONNECTOR_STATUS_DISCONNECTED"
[[ ! -e "$LEASE_PATH" ]] || fail "root handoff left the prior ownership lease behind"

response=$(printf 'connect Virtual-2\n' | control_connection "$OTHER_UID") || fail "new owner connect failed"
assert_file_value "$LEASE_PATH" "$OTHER_UID"
response=$(printf 'disconnect Virtual-2\n' | control_connection "$OTHER_UID") || fail "owner release failed"
[[ "$response" == "OK disconnected Virtual-2" ]] || fail "unexpected owner release response: ${response}"
[[ ! -e "$LEASE_PATH" ]] || fail "owner release left its lease behind"

# Model an interrupted disconnect: configfs reached disconnected, but the
# owner file was not removed. The next connect may safely reclaim it because a
# disconnected output cannot still be controlled by the old session.
response=$(printf 'connect Virtual-2\n' | control_connection "$OWNER_UID") || fail "stale lease setup failed"
printf '%s\n' "$CONNECTOR_STATUS_DISCONNECTED" >"$VKMS_DEVICE_DIR/connectors/Virtual-2/status"
assert_file_value "$LEASE_PATH" "$OWNER_UID"
response=$(printf 'connect Virtual-2\n' | control_connection "$OTHER_UID") || fail "stale lease reclamation failed"
[[ "$response" == "OK connected Virtual-2" ]] || fail "unexpected stale lease response: ${response}"
assert_file_value "$LEASE_PATH" "$OTHER_UID"
response=$(printf 'disconnect Virtual-2\n' | control_connection "$OTHER_UID") || fail "stale lease test cleanup failed"
[[ ! -e "$LEASE_PATH" ]] || fail "stale lease test cleanup left an owner file"

response=$(printf 'status Virtual-2\n' | control_connection "$OWNER_UID") || fail "disconnected status request failed"
[[ "$response" == "STATUS disconnected Virtual-2" ]] || fail "unexpected disconnected status: ${response}"

if response=$(printf 'status Virtual-2\n' | control_connection); then
  fail "request without trusted peer credentials succeeded"
fi
[[ "$response" == "ERROR peer credentials unavailable" ]] || fail "unexpected missing-peer response: ${response}"

for invalid_request in 'connect Virtual-0' 'connect Virtual-5' 'connect Virtual-1 extra' 'CONNECT Virtual-1'; do
  if response=$(printf '%s\n' "$invalid_request" | control_connection "$OWNER_UID"); then
    fail "invalid control request succeeded: ${invalid_request}"
  fi
  [[ "$response" == "ERROR malformed request" ]] || fail "unexpected protocol error: ${response}"
done

oversized_request=$(printf '%0130d' 0)
if response=$(printf '%s\n' "$oversized_request" | control_connection "$OWNER_UID"); then
  fail 'oversized control request succeeded'
fi
[[ "$response" == "ERROR request too large" ]] || fail "unexpected oversized-request response: ${response}"

# Control must never follow a connector attribute symlink in the test model.
mv -- "$VKMS_DEVICE_DIR/connectors/Virtual-3/status" "$TEST_ROOT/Virtual-3.status"
printf 'outside' >"$TEST_ROOT/outside-status"
ln -s -- "$TEST_ROOT/outside-status" "$VKMS_DEVICE_DIR/connectors/Virtual-3/status"
if response=$(printf 'connect Virtual-3\n' | control_connection "$OWNER_UID"); then
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
response=$(printf 'connect Virtual-4\n' | control_connection "$OWNER_UID") || fail "pool cleanup lease setup failed"
assert_file_value "$LEASE_ROOT/Virtual-4.owner" "$OWNER_UID"
system_is_stopping() {
  return 0
}
stop_pool || fail "shutdown-aware stop failed"
assert_file_value "$VKMS_DEVICE_DIR/enabled" 1
[[ -d "$VKMS_DEVICE_DIR/connectors/Virtual-1" ]] || fail "global shutdown removed the live DRM pool"
assert_file_value "$LEASE_ROOT/Virtual-4.owner" "$OWNER_UID"

QUIESCE_TEST_PATH="$TEST_ROOT/sysfs/vibeshine/quiesce"
mkdir -p -- "$(dirname -- "$QUIESCE_TEST_PATH")"
printf '0\n' >"$QUIESCE_TEST_PATH"
VIBESHINE_SYSTEM_STATE=running VIBESHINE_VKMS_QUIESCE_PATH="$QUIESCE_TEST_PATH" \
  "$QUIESCE_HELPER" || fail "non-shutdown quiesce helper failed"
assert_file_value "$QUIESCE_TEST_PATH" 0
VIBESHINE_SYSTEM_STATE=stopping VIBESHINE_VKMS_QUIESCE_PATH="$QUIESCE_TEST_PATH" \
  "$QUIESCE_HELPER" || fail "shutdown quiesce helper failed"
assert_file_value "$QUIESCE_TEST_PATH" 1

system_is_stopping() {
  return 1
}
stop_pool || fail "manual stop pool removal failed"
[[ ! -e "$VKMS_DEVICE_DIR" ]] || fail "manual stop left the managed instance behind"
[[ ! -e "$LEASE_ROOT/Virtual-4.owner" ]] || fail "manual stop left a connector ownership lease behind"

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
