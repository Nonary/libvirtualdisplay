#pragma once

#include "virtual_display/driver/control_protocol.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace virtual_display::driver {
  inline constexpr std::uint32_t kWindowsDriverMaxPermanentDisplays = 4;
  inline constexpr std::uint32_t kWindowsDriverMaxTemporaryDisplays = 8;
  inline constexpr std::uint32_t kWindowsDriverPersistentStateSchemaVersion = 1;
  inline constexpr std::size_t kTemporaryDisplayProfileBytes = 36;
  inline constexpr std::size_t kTemporaryDisplayProfilesHeaderBytes = 8;
  inline constexpr std::size_t kTemporaryDisplayProfilesMaxBytes =
    kTemporaryDisplayProfilesHeaderBytes + kTemporaryDisplayProfileBytes * kWindowsDriverMaxTemporaryDisplays;

  struct TemporaryDisplayProfile {
    std::uint64_t display_id {};
    std::uint32_t connector_index {};
    Guid container_id {};
    std::uint32_t edid_product_code {};
    std::uint32_t edid_serial_number {};

    friend constexpr bool operator==(const TemporaryDisplayProfile &lhs, const TemporaryDisplayProfile &rhs) = default;
  };

  bool valid_temporary_display_profile(std::uint64_t display_id, std::uint32_t connector_index);
  std::optional<std::vector<TemporaryDisplayProfile>> parse_temporary_display_profiles_blob(
    std::span<const std::uint8_t> blob
  );
  std::vector<std::uint8_t> serialize_temporary_display_profiles(
    std::span<const TemporaryDisplayProfile> profiles
  );
  std::optional<std::vector<TemporaryDisplayProfile>> upsert_temporary_display_profile(
    std::vector<TemporaryDisplayProfile> profiles,
    const TemporaryDisplayProfile &profile
  );
  std::vector<TemporaryDisplayProfile> remove_temporary_display_profile(
    std::vector<TemporaryDisplayProfile> profiles,
    std::uint64_t display_id
  );
  std::map<std::uint64_t, std::uint32_t> temporary_connector_reservations(
    std::span<const TemporaryDisplayProfile> profiles
  );
}  // namespace virtual_display::driver
