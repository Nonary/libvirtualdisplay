#pragma once

#include "virtual_display/driver/control_protocol.h"

#include <string>
#include <string_view>

// Single source of truth for the security-critical values that the Windows driver
// package (SunshineVirtualDisplayDriver.inf) and its tests must agree on. These were
// previously authored only as literals inside the INF and re-pinned by unit tests that
// scraped the INF/README text at runtime. Centralizing them here lets the INF be
// generated from these functions and lets the tests assert behavior instead of strings.
namespace virtual_display::driver {
  // Formats a GUID as the brace string Windows uses in INF/registry entries, e.g.
  // "{5f894d6c-3a69-48a2-86ef-e4c671932d63}" (lower-case, hyphenated). Derived from a
  // Guid value (kDeviceInterfaceGuid) so the INF and the runtime device interface can
  // never disagree.
  std::string format_inf_guid(const Guid &guid);

  // Canonical name of the elevated broker Windows service. The control device's access
  // grant is derived from this name, so renaming the service is the ONLY edit needed to
  // keep the INF security descriptor correct -- the SID below recomputes automatically.
  inline constexpr std::string_view kBrokerServiceName = "SunshineVirtualDisplayBroker";

  // Wide spelling of the same name for the Win32 service / event-log APIs the tools call.
  // The static_assert keeps the two forms from drifting: the narrow form drives the derived
  // SID, the wide form names the actual running service they must agree on.
  inline constexpr std::wstring_view kBrokerServiceNameW = L"SunshineVirtualDisplayBroker";

  namespace detail {
    constexpr bool same_ascii(const std::string_view narrow, const std::wstring_view wide) {
      if (narrow.size() != wide.size()) {
        return false;
      }
      for (std::size_t i = 0; i < narrow.size(); ++i) {
        if (static_cast<unsigned char>(narrow[i]) != static_cast<unsigned long>(wide[i])) {
          return false;
        }
      }
      return true;
    }
  }  // namespace detail

  static_assert(
    detail::same_ascii(kBrokerServiceName, kBrokerServiceNameW),
    "broker service name must be identical in narrow and wide forms"
  );

  // The per-service SID that Windows assigns to a service of the given name. Service SIDs
  // are "S-1-5-80-" followed by the five little-endian 32-bit words of the SHA-1 digest of
  // the upper-cased, UTF-16LE service name. This reproduces Windows' own derivation, so the
  // returned SID is exactly the principal a running SunshineVirtualDisplayBroker presents.
  std::string derive_service_sid(std::string_view service_name);

  // SDDL security descriptor applied to BOTH the control device interface and the device
  // hardware key. Grants GENERIC_ALL to LocalSystem ("SY") and the broker service only, and
  // is protected (no inherited ACEs), e.g.
  // "D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)".
  std::string control_interface_security_descriptor();

  // The control protocol version as documented prose, e.g. "3.5.0", from the
  // kProtocolVersion* constants.
  std::string protocol_version_string();
}  // namespace virtual_display::driver
