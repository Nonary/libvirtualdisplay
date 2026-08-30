#!/usr/bin/env bash

# ShellCheck cannot see variables and command mocks consumed by the sourced helper.
# shellcheck disable=SC2034,SC2329

set -euo pipefail

INSTALLER_UNDER_TEST=${1:?usage: test-vibeshine-drm-install.sh /path/to/vibeshine-drm-install}
TEST_SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
TEST_ROOT=$(mktemp -d)

cleanup_test_root() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup_test_root EXIT

# shellcheck source=/dev/null
source "$INSTALLER_UNDER_TEST"

# The source template is tested before CMake substitutes its deterministic hash.
MODULE_VERSION=1.2.3
MODULE_SOURCE_ID=$(printf 'a%.0s' {1..64})

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local path=$1
  local expected=$2

  [[ -f "$path" ]] || fail "missing file: ${path}"
  if ! grep -F -- "$expected" "$path" >/dev/null; then
    fail "${path} does not contain '${expected}'"
  fi
}

TEST_SOURCE="$TEST_ROOT/vibeshine-drm-$MODULE_VERSION"
TEST_STATE="$TEST_ROOT/state"
TEST_MODULES="$TEST_ROOT/modules"
TEST_SYS_MODULE="$TEST_ROOT/sys-module"
TEST_BUILD_TMP="$TEST_ROOT/build-tmp"
TEST_MOK_KEY="$TEST_ROOT/dkms/mok.key"
TEST_MOK_CERTIFICATE="$TEST_ROOT/dkms/mok.pub"
TEST_EFI_FIRMWARE="$TEST_ROOT/efi"
TEST_EFI_VARS="$TEST_ROOT/efi-vars"
TEST_MOK_VARIABLES="$TEST_ROOT/mok-variables"
TEST_LOCKDOWN_FILE="$TEST_ROOT/lockdown"
TEST_SIG_ENFORCE_FILE="$TEST_ROOT/sig-enforce"
TEST_DKMS_CONFIG="$TEST_ROOT/dkms-config"
TEST_KERNEL="6.16.0-vibeshine"
mkdir -p -- "$TEST_SOURCE" "$TEST_MODULES/$TEST_KERNEL/build/scripts" "$TEST_SYS_MODULE" \
  "$TEST_BUILD_TMP" "$TEST_EFI_FIRMWARE" "$TEST_EFI_VARS" "$TEST_DKMS_CONFIG/framework.conf.d"
printf 'obj-m += vibeshine_drm.o\n' >"$TEST_SOURCE/Makefile"
printf 'PACKAGE_NAME="vibeshine-drm"\n' >"$TEST_SOURCE/dkms.conf"
printf '/* test source */\n' >"$TEST_SOURCE/vkms_drv.c"
printf 'all:\n' >"$TEST_MODULES/$TEST_KERNEL/build/Makefile"
cp -- "$TEST_SCRIPT_DIR/../../linux/vibeshine-drm/build-module" "$TEST_SOURCE/build-module"
chmod 0755 "$TEST_SOURCE/build-module"
printf '#!/usr/bin/env bash\nprintf "signed:%%s\\n" "$1" >>"$4"\n' >"$TEST_MODULES/$TEST_KERNEL/build/scripts/sign-file"
chmod 0755 "$TEST_MODULES/$TEST_KERNEL/build/scripts/sign-file"

configure_install_paths \
  "$TEST_SOURCE" "$TEST_STATE" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP" \
  "$TEST_MOK_KEY" "$TEST_MOK_CERTIFICATE"
KERNEL_RELEASE_OVERRIDE=$TEST_KERNEL
EFI_FIRMWARE_ROOT=$TEST_EFI_FIRMWARE
EFI_VARS_ROOT=$TEST_EFI_VARS
EFI_MOK_VARIABLES_ROOT=$TEST_MOK_VARIABLES
LOCKDOWN_FILE=$TEST_LOCKDOWN_FILE
MODULE_SIG_ENFORCE_FILE=$TEST_SIG_ENFORCE_FILE
DKMS_CONFIG_ROOT=$TEST_DKMS_CONFIG
printf 'none [none] integrity confidentiality\n' >"$TEST_LOCKDOWN_FILE"
printf 'N\n' >"$TEST_SIG_ENFORCE_FILE"
printf 'mok_signing_key="%s"\nmok_certificate="%s"\ntry_sign_modules=true\n' \
  "$TEST_MOK_KEY" "$TEST_MOK_CERTIFICATE" >"$TEST_DKMS_CONFIG/framework.conf"

test_dkms_install() (
  DKMS_REGISTERED=0
  DKMS_INSTALLED=0
  DKMS_OLD_REGISTERED=1
  SIGNATURE_MATCH=1
  DKMS_CALLS="$TEST_ROOT/dkms-calls"
  : >"$DKMS_CALLS"

  dkms() {
    local action=${1:-}
    printf '%s\n' "$*" >>"$DKMS_CALLS"
    case "$action" in
      status)
        if ((DKMS_OLD_REGISTERED)); then
          printf '%s/%s, %s, x86_64: installed\n' "$DKMS_NAME" 0.9.0 "$TEST_KERNEL"
        fi
        if ((DKMS_REGISTERED)); then
          if ((DKMS_INSTALLED)); then
            printf '%s/%s, %s, x86_64: installed\n' "$DKMS_NAME" "$MODULE_VERSION" "$TEST_KERNEL"
          else
            printf '%s/%s: added\n' "$DKMS_NAME" "$MODULE_VERSION"
          fi
        fi
        ;;
      add)
        DKMS_REGISTERED=1
        ;;
      build)
        ((DKMS_REGISTERED == 1))
        ;;
      install)
        DKMS_INSTALLED=1
        SIGNATURE_MATCH=1
        ;;
      remove)
        if [[ $* == *'-v 0.9.0'* ]]; then
          DKMS_OLD_REGISTERED=0
        else
          DKMS_REGISTERED=0
          DKMS_INSTALLED=0
        fi
        ;;
      *)
        return 1
        ;;
    esac
  }
  installed_module_matches_signing_certificate() {
    ((SIGNATURE_MATCH == 1))
  }

  mkdir -- "$TEST_ROOT/vibeshine-drm-0.9.0"
  install_module || fail "DKMS installation failed"
  [[ ! -e "$TEST_ROOT/vibeshine-drm-0.9.0" ]] || fail "obsolete source tree remains"
  assert_contains "$DKMS_CALLS" "remove -m $DKMS_NAME -v 0.9.0 --all"
  install_module || fail "idempotent DKMS installation failed"
  [[ $(grep -c '^install ' "$DKMS_CALLS") -eq 1 ]] || fail "DKMS installed more than once"

  SIGNATURE_MATCH=0
  install_module || fail "unexpected DKMS signature was not repaired"
  [[ $(grep -c '^install ' "$DKMS_CALLS") -eq 2 ]] || fail "unexpected DKMS signature was not reinstalled"

  MODULE_SOURCE_ID=$(printf 'b%.0s' {1..64})
  install_module || fail "same-version DKMS refresh failed"
  [[ $(grep -c '^install ' "$DKMS_CALLS") -eq 3 ]] || fail "changed DKMS source was not reinstalled"
  assert_contains "$DKMS_CALLS" "build -m $DKMS_NAME -v $MODULE_VERSION -k $TEST_KERNEL --force"
  assert_contains "$DKMS_CALLS" "install -m $DKMS_NAME -v $MODULE_VERSION -k $TEST_KERNEL --force"
  remove_module || fail "DKMS removal failed"
  assert_contains "$DKMS_CALLS" "remove -m $DKMS_NAME -v $MODULE_VERSION --all"
)

test_direct_install() (
  DIRECT_CALLS="$TEST_ROOT/direct-calls"
  : >"$DIRECT_CALLS"

  dkms_available() {
    return 1
  }
  run_build_helper() {
    local build_workdir=$1
    local kernel_release=$2
    printf 'build-helper %s %s\n' "$kernel_release" "$build_workdir" >>"$DIRECT_CALLS"
    printf 'fake module\n' >"$build_workdir/$MODULE_NAME.ko"
  }
  depmod() {
    printf 'depmod %s\n' "$*" >>"$DIRECT_CALLS"
  }
  modprobe() {
    printf 'modprobe %s\n' "$*" >>"$DIRECT_CALLS"
  }
  verify_module_signature_matches_certificate() {
    grep -F -- "signed:sha256" "$1" >/dev/null
  }

  install_module || fail "direct installation failed"
  destination=$(direct_destination "$TEST_KERNEL")
  marker=$(direct_marker "$TEST_KERNEL")
  [[ -f "$destination" ]] || fail "direct module was not installed"
  assert_contains "$destination" "signed:sha256"
  [[ -f "$marker" ]] || fail "direct-install marker was not created"
  [[ ! -e "$SOURCE_DIR/$MODULE_NAME.ko" ]] || fail "direct build polluted the packaged source tree"
  install_module || fail "idempotent direct installation failed"
  [[ $(grep -c '^build-helper ' "$DIRECT_CALLS") -eq 1 ]] || fail "direct module built more than once"

  printf 'unexpected signature\n' >"$destination"
  install_module || fail "unexpected direct signature was not repaired"
  [[ $(grep -c '^build-helper ' "$DIRECT_CALLS") -eq 2 ]] || fail "unexpected direct signature was not rebuilt"

  rm -f -- "$TEST_MOK_KEY" "$TEST_MOK_CERTIFICATE"
  install_module || fail "signing-key rotation direct refresh failed"
  [[ $(grep -c '^build-helper ' "$DIRECT_CALLS") -eq 3 ]] || fail "module was not rebuilt after signing-key rotation"

  MODULE_SOURCE_ID=$(printf 'c%.0s' {1..64})
  install_module || fail "same-version direct refresh failed"
  [[ $(grep -c '^build-helper ' "$DIRECT_CALLS") -eq 4 ]] || fail "changed direct source was not rebuilt"
  if compgen -G "$BUILD_TMP_ROOT/vibeshine-drm-build.*" >/dev/null; then
    fail "temporary direct-build directory was not cleaned"
  fi

  mkdir -- "$SYS_MODULE_ROOT/$MODULE_NAME"
  remove_module || fail "direct module removal failed"
  [[ ! -e "$destination" ]] || fail "direct module remains after removal"
  [[ ! -e "$marker" ]] || fail "direct-install marker remains after removal"
  assert_contains "$DIRECT_CALLS" "modprobe -r $MODULE_NAME"
  assert_contains "$DIRECT_CALLS" "depmod -a $TEST_KERNEL"
)

test_unsupported_kernel_stops_before_side_effects() (
  UNSUPPORTED_KERNEL="6.15.99-vibeshine"
  UNSUPPORTED_LOG="$TEST_ROOT/unsupported-kernel.log"
  UNSUPPORTED_MOK_KEY="$TEST_ROOT/unsupported-dkms/mok.key"
  UNSUPPORTED_MOK_CERTIFICATE="$TEST_ROOT/unsupported-dkms/mok.pub"

  configure_install_paths \
    "$TEST_SOURCE" "$TEST_ROOT/unsupported-state" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP" \
    "$UNSUPPORTED_MOK_KEY" "$UNSUPPORTED_MOK_CERTIFICATE"
  KERNEL_RELEASE_OVERRIDE=$UNSUPPORTED_KERNEL
  prepare_signing_key() {
    fail "unsupported kernel generated a signing key"
  }
  cleanup_obsolete_installations() {
    fail "unsupported kernel cleaned module installations"
  }
  dkms_available() {
    fail "unsupported kernel attempted DKMS detection"
  }

  if install_module >"$UNSUPPORTED_LOG" 2>&1; then
    fail "unsupported kernel was accepted"
  fi
  assert_contains "$UNSUPPORTED_LOG" "managed virtual displays require Linux 6.16 or newer"
  assert_contains "$UNSUPPORTED_LOG" "the running kernel is ${UNSUPPORTED_KERNEL} and is unsupported"
  assert_contains "$UNSUPPORTED_LOG" "install and boot a supported Linux kernel"
  assert_contains "$UNSUPPORTED_LOG" "Vibeshine was installed, but its virtual-display driver was not"
  [[ ! -e "$UNSUPPORTED_MOK_KEY" && ! -e "$UNSUPPORTED_MOK_CERTIFICATE" ]] ||
    fail "unsupported kernel created signing files"
)

test_missing_headers_stops_before_dkms() (
  MISSING_HEADERS_KERNEL="6.16.0-missing-headers"
  MISSING_HEADERS_LOG="$TEST_ROOT/missing-headers.log"
  MISSING_HEADERS_MOK_KEY="$TEST_ROOT/missing-headers-dkms/mok.key"
  MISSING_HEADERS_MOK_CERTIFICATE="$TEST_ROOT/missing-headers-dkms/mok.pub"

  mkdir -p -- "$TEST_MODULES/$MISSING_HEADERS_KERNEL"
  : >"$TEST_MODULES/$MISSING_HEADERS_KERNEL/vmlinuz"
  configure_install_paths \
    "$TEST_SOURCE" "$TEST_ROOT/missing-headers-state" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP" \
    "$MISSING_HEADERS_MOK_KEY" "$MISSING_HEADERS_MOK_CERTIFICATE"
  KERNEL_RELEASE_OVERRIDE=$MISSING_HEADERS_KERNEL
  pacman() {
    [[ ${1:-} == -Qqo && ${2:-} == "$TEST_MODULES/$MISSING_HEADERS_KERNEL/vmlinuz" ]] || return 1
    printf 'linux-cachyos\n'
  }
  prepare_signing_key() {
    fail "missing headers generated a signing key"
  }
  cleanup_obsolete_installations() {
    fail "missing headers cleaned module installations"
  }
  dkms_available() {
    fail "missing headers attempted DKMS detection"
  }

  if install_module >"$MISSING_HEADERS_LOG" 2>&1; then
    fail "missing headers were accepted"
  fi
  assert_contains "$MISSING_HEADERS_LOG" "kernel headers for ${MISSING_HEADERS_KERNEL} are missing"
  assert_contains "$MISSING_HEADERS_LOG" "sudo pacman -S --needed linux-cachyos-headers"
  [[ ! -e "$MISSING_HEADERS_MOK_KEY" && ! -e "$MISSING_HEADERS_MOK_CERTIFICATE" ]] ||
    fail "missing headers created signing files"
)

test_status_reports_kernel_requirements() (
  STATUS_UNSUPPORTED_KERNEL="6.15.99-status"
  STATUS_UNSUPPORTED_LOG="$TEST_ROOT/status-unsupported.log"
  STATUS_HEADERS_KERNEL="6.16.0-status-noheaders"
  STATUS_HEADERS_LOG="$TEST_ROOT/status-headers.log"

  configure_install_paths \
    "$TEST_SOURCE" "$TEST_ROOT/status-state" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP" \
    "$TEST_MOK_KEY" "$TEST_MOK_CERTIFICATE"

  KERNEL_RELEASE_OVERRIDE=$STATUS_UNSUPPORTED_KERNEL
  if module_status >"$STATUS_UNSUPPORTED_LOG" 2>&1; then
    fail "status accepted an unsupported kernel"
  fi
  assert_contains "$STATUS_UNSUPPORTED_LOG" "managed virtual displays require Linux 6.16 or newer"
  assert_contains "$STATUS_UNSUPPORTED_LOG" "the running kernel is ${STATUS_UNSUPPORTED_KERNEL} and is unsupported"

  mkdir -p -- "$TEST_MODULES/$STATUS_HEADERS_KERNEL"
  : >"$TEST_MODULES/$STATUS_HEADERS_KERNEL/vmlinuz"
  KERNEL_RELEASE_OVERRIDE=$STATUS_HEADERS_KERNEL
  dkms_available() {
    return 1
  }
  pacman() {
    [[ ${1:-} == -Qqo && ${2:-} == "$TEST_MODULES/$STATUS_HEADERS_KERNEL/vmlinuz" ]] || return 1
    printf 'linux-cachyos-lts\n'
  }
  if module_status >"$STATUS_HEADERS_LOG" 2>&1; then
    fail "status accepted a missing driver installation"
  fi
  assert_contains "$STATUS_HEADERS_LOG" "kernel headers for ${STATUS_HEADERS_KERNEL} are missing"
  assert_contains "$STATUS_HEADERS_LOG" "sudo pacman -S --needed linux-cachyos-lts-headers"
)

test_obsolete_direct_cleanup() (
  OLD_KERNEL="6.98.0-old"
  OLD_SOURCE="$TEST_ROOT/vibeshine-drm-0.8.0"
  UNMANAGED_SOURCE="$TEST_ROOT/vibeshine-drm-backup"
  OLD_DESTINATION="$TEST_MODULES/$OLD_KERNEL/updates/vibeshine/$MODULE_NAME.ko"
  OLD_MARKER="$TEST_STATE/direct-0.8.0-$OLD_KERNEL"
  mkdir -p -- "$OLD_SOURCE" "$UNMANAGED_SOURCE" "${OLD_DESTINATION%/*}" "$TEST_STATE"
  printf 'old module\n' >"$OLD_DESTINATION"
  # Legacy markers contained only the installed destination.
  printf '%s\n' "$OLD_DESTINATION" >"$OLD_MARKER"

  dkms_available() {
    return 1
  }
  depmod() {
    :
  }

  cleanup_obsolete_installations || fail "obsolete direct cleanup failed"
  [[ ! -e "$OLD_DESTINATION" ]] || fail "obsolete direct module remains"
  [[ ! -e "$OLD_MARKER" ]] || fail "obsolete direct marker remains"
  [[ ! -e "$OLD_SOURCE" ]] || fail "obsolete direct source tree remains"
  [[ -d "$UNMANAGED_SOURCE" ]] || fail "non-versioned source directory was removed"
)

test_dkms_failure_falls_back() (
  FALLBACK_CALLS="$TEST_ROOT/fallback-calls"
  : >"$FALLBACK_CALLS"

  dkms_available() {
    return 0
  }
  dkms() {
    case ${1:-} in
      status)
        return 0
        ;;
      add)
        return 1
        ;;
    esac
    return 1
  }
  run_build_helper() {
    local build_workdir=$1
    local kernel_release=$2
    printf 'build-helper %s %s\n' "$kernel_release" "$build_workdir" >>"$FALLBACK_CALLS"
    printf 'fake module\n' >"$build_workdir/$MODULE_NAME.ko"
  }
  depmod() {
    :
  }
  verify_module_signature_matches_certificate() {
    [[ -f "$1" ]] || return 1
  }

  install_module || fail "direct fallback after DKMS failure failed"
  [[ -f "$(direct_destination "$TEST_KERNEL")" ]] || fail "DKMS failure did not install direct module"
)

test_dkms_signing_mismatch_falls_back() (
  MISMATCH_STATE="$TEST_ROOT/mismatch-state"
  MISMATCH_SYS_MODULE="$TEST_ROOT/mismatch-sys-module"
  MISMATCH_DKMS_CONFIG="$TEST_ROOT/mismatch-dkms-config"
  MISMATCH_CALLS="$TEST_ROOT/mismatch-calls"
  MISMATCH_INSTALLED=1
  FAIL_DIRECT=1
  mkdir -p -- "$MISMATCH_SYS_MODULE" "$MISMATCH_DKMS_CONFIG/framework.conf.d"
  : >"$MISMATCH_CALLS"
  printf '%s\n' \
    'mok_signing_key="/var/lib/dkms/site.key"' \
    'mok_certificate="/var/lib/dkms/site.pub"' \
    'try_sign_modules=true' >"$MISMATCH_DKMS_CONFIG/framework.conf"
  configure_install_paths \
    "$TEST_SOURCE" "$MISMATCH_STATE" "$TEST_MODULES" "$MISMATCH_SYS_MODULE" "$TEST_BUILD_TMP" \
    "$TEST_MOK_KEY" "$TEST_MOK_CERTIFICATE"
  DKMS_CONFIG_ROOT=$MISMATCH_DKMS_CONFIG

  dkms_available() {
    return 0
  }
  dkms() {
    printf '%s\n' "$*" >>"$MISMATCH_CALLS"
    case ${1:-} in
      status)
        if ((MISMATCH_INSTALLED)); then
          printf '%s/%s, %s, x86_64: installed\n' "$DKMS_NAME" "$MODULE_VERSION" "$TEST_KERNEL"
        else
          printf '%s/%s: added\n' "$DKMS_NAME" "$MODULE_VERSION"
        fi
        ;;
      remove) MISMATCH_INSTALLED=0 ;;
      *) return 1 ;;
    esac
  }
  run_build_helper() {
    local build_workdir=$1
    printf 'direct-build\n' >>"$MISMATCH_CALLS"
    ((FAIL_DIRECT == 0)) || return 1
    printf 'fake module\n' >"$build_workdir/$MODULE_NAME.ko"
  }
  depmod() {
    return 0
  }
  verify_module_signature_matches_certificate() {
    [[ -f "$1" ]]
  }
  installed_module_matches_signing_certificate() {
    return 0
  }

  if install_module; then
    fail "a custom-signed current DKMS installation was replaced"
  fi
  ((MISMATCH_INSTALLED == 1)) || fail "custom-signed DKMS installation was removed"
  if grep -q '^remove ' "$MISMATCH_CALLS"; then
    fail "custom signing mismatch called DKMS remove"
  fi
  if grep -q '^direct-build$' "$MISMATCH_CALLS"; then
    fail "custom-signed current DKMS installation attempted a direct replacement"
  fi

  MISMATCH_INSTALLED=0
  if install_module; then
    fail "a failed direct fallback was accepted"
  fi
  if grep -q '^remove ' "$MISMATCH_CALLS"; then
    fail "failed direct fallback called DKMS remove"
  fi
  FAIL_DIRECT=0
  install_module || fail "DKMS signing mismatch did not use the verified direct fallback"
  [[ $(grep -c '^direct-build$' "$MISMATCH_CALLS") -eq 2 ]] || fail "direct fallback attempts were not recorded"
  [[ -f "$(direct_destination "$TEST_KERNEL")" ]] || fail "signing mismatch did not install the direct module"
)

test_signing_status() (
  MOK_ENROLLED=0
  EFI_MOK_VARIABLES_ROOT="$TEST_ROOT/signing-status-mok-variables"
  SIGNING_STATUS_LOG="$TEST_ROOT/signing-status.log"
  mkdir -p -- "$EFI_MOK_VARIABLES_ROOT"

  mokutil() {
    case ${1:-} in
      --list-enrolled)
        if ((MOK_ENROLLED == 1)); then
          printf 'SHA1 Fingerprint: %s\n' \
            "$(openssl x509 -inform DER -in "$MOK_CERTIFICATE" -noout -sha1 -fingerprint | cut -d= -f2)"
        fi
        ;;
      --sb-state)
        printf 'SecureBoot enabled\n'
        ;;
      *)
        return 1
        ;;
    esac
  }

  signing_status >"$SIGNING_STATUS_LOG" || fail "unenforced Arch kernel incorrectly required MOK enrollment"
  assert_contains "$SIGNING_STATUS_LOG" "does not require MOK enrollment"
  if grep -q 'enroll-key' "$SIGNING_STATUS_LOG"; then
    fail "unenforced Arch kernel printed manual enrollment instructions"
  fi
  report_enrollment_needed >"$SIGNING_STATUS_LOG" || fail "unenforced signing guidance failed"
  if grep -q 'enroll-key' "$SIGNING_STATUS_LOG"; then
    fail "package install printed manual enrollment instructions on an unenforced Arch kernel"
  fi

  printf 'Y\n' >"$MODULE_SIG_ENFORCE_FILE"
  if signing_status >"$SIGNING_STATUS_LOG"; then
    fail "unenrolled signing key was reported as enrolled"
  fi
  MOK_ENROLLED=1
  printf '\x01' >"$EFI_MOK_VARIABLES_ROOT/MokListTrustedRT"
  signing_status || fail "enrolled signing key was not reported as enrolled"
  report_enrollment_needed || fail "enrollment guidance failed"
  printf 'N\n' >"$MODULE_SIG_ENFORCE_FILE"
)

test_secure_boot_and_enforcement_detection() (
  SECURE_BOOT_VAR="$EFI_VARS_ROOT/SecureBoot-test"
  printf '\x07\x00\x00\x00\x01' >"$SECURE_BOOT_VAR"
  [[ $(secure_boot_state) == enabled ]] || fail "EFI Secure Boot enabled state was not detected"
  printf '\x07\x00\x00\x00\x00' >"$SECURE_BOOT_VAR"
  [[ $(secure_boot_state) == disabled ]] || fail "EFI Secure Boot disabled state was not detected"

  printf 'Y\n' >"$MODULE_SIG_ENFORCE_FILE"
  module_signature_is_enforced || fail "module.sig_enforce was not detected"
  printf 'N\n' >"$MODULE_SIG_ENFORCE_FILE"
  printf 'none [integrity] confidentiality\n' >"$LOCKDOWN_FILE"
  module_signature_is_enforced || fail "integrity lockdown was not detected"
  printf 'none [none] integrity confidentiality\n' >"$LOCKDOWN_FILE"
  if module_signature_is_enforced; then
    fail "unenforced module signatures were reported as enforced"
  fi

  EFI_MOK_VARIABLES_ROOT="$TEST_ROOT/trust-mok-variables"
  mkdir -p -- "$EFI_MOK_VARIABLES_ROOT"
  printf '\x00' >"$EFI_MOK_VARIABLES_ROOT/MokListTrustedRT"
  if mok_list_is_trusted; then
    fail "a disabled MokListTrustedRT flag was accepted"
  fi
  printf '\x01' >"$EFI_MOK_VARIABLES_ROOT/MokListTrustedRT"
  mok_list_is_trusted || fail "the shim MOK trust flag was not detected"

  rm -f -- "$EFI_MOK_VARIABLES_ROOT/MokListTrustedRT"
  printf '\x07\x00\x00\x00\x01' >"$EFI_VARS_ROOT/MokListTrustedRT-test"
  mok_list_is_trusted || fail "the EFI-variable MOK trust flag was not detected"
)

test_partial_signing_key_is_preserved() (
  PARTIAL_KEY="$TEST_ROOT/partial/mok.key"
  PARTIAL_CERTIFICATE="$TEST_ROOT/partial/mok.pub"
  mkdir -p -- "${PARTIAL_KEY%/*}"
  printf 'do-not-replace\n' >"$PARTIAL_KEY"
  configure_install_paths \
    "$TEST_SOURCE" "$TEST_ROOT/partial-state" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP" \
    "$PARTIAL_KEY" "$PARTIAL_CERTIFICATE"

  if prepare_signing_key; then
    fail "an incomplete signing-key pair was overwritten"
  fi
  assert_contains "$PARTIAL_KEY" "do-not-replace"
  [[ ! -e "$PARTIAL_CERTIFICATE" ]] || fail "missing certificate was unexpectedly generated"
)

test_existing_signing_pair_rejects_symlinked_directory() (
  LINKED_SIGNING_DIR="$TEST_ROOT/linked-signing-dir"
  ln -s -- "${TEST_MOK_KEY%/*}" "$LINKED_SIGNING_DIR"
  configure_install_paths \
    "$TEST_SOURCE" "$TEST_ROOT/linked-state" "$TEST_MODULES" "$TEST_SYS_MODULE" "$TEST_BUILD_TMP" \
    "$LINKED_SIGNING_DIR/mok.key" "$LINKED_SIGNING_DIR/mok.pub"

  if prepare_signing_key; then
    fail "an existing signing pair beneath a symlinked directory was accepted"
  fi
)

test_enrollment_requires_current_shim_boot() (
  ENROLL_CALLS="$TEST_ROOT/enroll-calls"
  EFI_VARS_ROOT="$TEST_ROOT/enroll-efi-vars"
  mkdir -p -- "$EFI_VARS_ROOT"
  : >"$ENROLL_CALLS"
  printf 'stale\n' >"$EFI_VARS_ROOT/MokListRT-stale"

  require_root() {
    return 0
  }
  sbctl_is_configured() {
    return 0
  }
  mokutil() {
    case ${1:-} in
      --list-enrolled | --list-new) return 0 ;;
      --import) printf 'import %s\n' "$2" >>"$ENROLL_CALLS" ;;
      --trust-mok) printf 'trust-mok\n' >>"$ENROLL_CALLS" ;;
      --help) printf '%s\n' '  --trust-mok' ;;
      *) return 1 ;;
    esac
  }

  if enroll_signing_key; then
    fail "MOK enrollment was scheduled without a current shim MOK table"
  fi
  [[ ! -s "$ENROLL_CALLS" ]] || fail "mokutil import ran without a current shim boot"

  mkdir -p -- "$EFI_MOK_VARIABLES_ROOT"
  enroll_signing_key || fail "MOK enrollment was not scheduled after a shim boot"
  assert_contains "$ENROLL_CALLS" "import $MOK_CERTIFICATE"
  assert_contains "$ENROLL_CALLS" "trust-mok"
)

test_package_install_auto_enrollment() (
  AUTO_ENROLL=1
  ENROLLMENT_REQUESTED=0

  module_signing_trust_is_available() {
    return 1
  }
  request_signing_key_enrollment() {
    ENROLLMENT_REQUESTED=1
  }
  report_loaded_module_freshness() {
    fail "an untrusted strict-kernel module was treated as immediately usable"
  }

  if finish_module_installation; then
    fail "package enrollment request was reported as immediately complete"
  else
    finish_rc=$?
  fi
  [[ $finish_rc -eq $ENROLLMENT_PENDING_EXIT ]] || fail "package enrollment returned the wrong status"
  ((ENROLLMENT_REQUESTED == 1)) || fail "package install did not launch enrollment automatically"
)

test_package_enrollment_requires_terminal() (
  NON_TTY_DEVICE="$TEST_ROOT/not-a-terminal"
  NON_TTY_LOG="$TEST_ROOT/non-tty-enrollment.log"
  : >"$NON_TTY_DEVICE"
  TTY_DEVICE=$NON_TTY_DEVICE

  enroll_signing_key() {
    fail "enrollment ran without an interactive terminal"
  }

  if request_signing_key_enrollment </dev/null 2>"$NON_TTY_LOG"; then
    fail "noninteractive package enrollment was accepted"
  fi
  assert_contains "$NON_TTY_LOG" "retry the package installation from a terminal"
)

test_loaded_module_freshness() (
  mkdir -p -- "$SYS_MODULE_ROOT/$MODULE_NAME"
  printf '1.2.0\n' >"$SYS_MODULE_ROOT/$MODULE_NAME/version"
  printf 'CURRENT-SOURCE\n' >"$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"

  dkms_available() {
    return 1
  }
  direct_is_installed_for_kernel() {
    return 0
  }
  verify_module_signature_matches_certificate() {
    return 0
  }
  installed_module_matches_signing_certificate() {
    return 0
  }
  modinfo() {
    [[ ${1:-} == -F && ${3:-} == "$MODULE_NAME" ]] || return 1
    case ${2:-} in
      version) printf '1.2.0\n' ;;
      srcversion) printf 'CURRENT-SOURCE\n' ;;
      *) return 1 ;;
    esac
  }

  module_status || fail "matching loaded module was rejected"

  printf 'STALE-SOURCE\n' >"$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
  if module_status; then
    fail "same-version stale loaded module was accepted"
  else
    [[ $? -eq $RELOAD_REQUIRED_EXIT ]] || fail "stale source returned the wrong status"
  fi

  printf 'CURRENT-SOURCE\n' >"$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
  printf '1.0.0\n' >"$SYS_MODULE_ROOT/$MODULE_NAME/version"
  if module_status; then
    fail "stale loaded module version was accepted"
  else
    [[ $? -eq $RELOAD_REQUIRED_EXIT ]] || fail "stale version returned the wrong status"
  fi

  rm -f -- "$SYS_MODULE_ROOT/$MODULE_NAME/srcversion"
  if module_status; then
    fail "unverifiable loaded module was accepted"
  else
    [[ $? -eq $RELOAD_REQUIRED_EXIT ]] || fail "unverifiable module returned the wrong status"
  fi
)

test_dkms_install
test_direct_install
test_unsupported_kernel_stops_before_side_effects
test_missing_headers_stops_before_dkms
test_status_reports_kernel_requirements
test_obsolete_direct_cleanup

# The direct fallback test uses its own state tree so an earlier direct marker
# cannot turn the fallback path into an idempotent no-op.
configure_install_paths \
  "$TEST_SOURCE" "$TEST_ROOT/fallback-state" "$TEST_MODULES" "$TEST_ROOT/fallback-sys-module" "$TEST_BUILD_TMP" \
  "$TEST_MOK_KEY" "$TEST_MOK_CERTIFICATE"
mkdir -p -- "$SYS_MODULE_ROOT"
test_dkms_failure_falls_back
test_dkms_signing_mismatch_falls_back
test_signing_status
test_secure_boot_and_enforcement_detection
test_partial_signing_key_is_preserved
test_existing_signing_pair_rejects_symlinked_directory
test_enrollment_requires_current_shim_boot
test_package_install_auto_enrollment
test_package_enrollment_requires_terminal
test_loaded_module_freshness

printf 'PASS: vibeshine-drm installer shell tests\n'
