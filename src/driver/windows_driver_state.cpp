#include "virtual_display/driver/windows_driver_state.h"

#include <algorithm>

namespace virtual_display::driver {
  namespace {
    void append_u32(std::vector<std::uint8_t> &blob, const std::uint32_t value) {
      blob.push_back(static_cast<std::uint8_t>(value & 0xffu));
      blob.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
      blob.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xffu));
      blob.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xffu));
    }

    void append_u64(std::vector<std::uint8_t> &blob, const std::uint64_t value) {
      append_u32(blob, static_cast<std::uint32_t>(value & 0xffffffffull));
      append_u32(blob, static_cast<std::uint32_t>((value >> 32ull) & 0xffffffffull));
    }

    void append_guid(std::vector<std::uint8_t> &blob, const Guid &value) {
      append_u32(blob, value.data1);
      blob.push_back(static_cast<std::uint8_t>(value.data2 & 0xffu));
      blob.push_back(static_cast<std::uint8_t>((value.data2 >> 8u) & 0xffu));
      blob.push_back(static_cast<std::uint8_t>(value.data3 & 0xffu));
      blob.push_back(static_cast<std::uint8_t>((value.data3 >> 8u) & 0xffu));
      blob.insert(blob.end(), value.data4.begin(), value.data4.end());
    }

    bool has_bytes(const std::span<const std::uint8_t> blob, const std::size_t offset, const std::size_t count) {
      return offset <= blob.size() && count <= blob.size() - offset;
    }

    bool read_u32(const std::span<const std::uint8_t> blob, std::size_t &offset, std::uint32_t &value) {
      if (!has_bytes(blob, offset, sizeof(std::uint32_t))) {
        return false;
      }

      value =
        static_cast<std::uint32_t>(blob[offset]) |
        (static_cast<std::uint32_t>(blob[offset + 1]) << 8u) |
        (static_cast<std::uint32_t>(blob[offset + 2]) << 16u) |
        (static_cast<std::uint32_t>(blob[offset + 3]) << 24u);
      offset += sizeof(std::uint32_t);
      return true;
    }

    bool read_u64(const std::span<const std::uint8_t> blob, std::size_t &offset, std::uint64_t &value) {
      std::uint32_t low {};
      std::uint32_t high {};
      if (!read_u32(blob, offset, low) || !read_u32(blob, offset, high)) {
        return false;
      }

      value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32ull);
      return true;
    }

    bool read_guid(const std::span<const std::uint8_t> blob, std::size_t &offset, Guid &value) {
      if (!has_bytes(blob, offset, sizeof(Guid))) {
        return false;
      }

      std::uint32_t data1 {};
      std::uint32_t data2 {};
      std::uint32_t data3 {};
      if (!read_u32(blob, offset, data1) || !has_bytes(blob, offset, 12)) {
        return false;
      }

      data2 = static_cast<std::uint32_t>(blob[offset]) | (static_cast<std::uint32_t>(blob[offset + 1]) << 8u);
      offset += sizeof(std::uint16_t);
      data3 = static_cast<std::uint32_t>(blob[offset]) | (static_cast<std::uint32_t>(blob[offset + 1]) << 8u);
      offset += sizeof(std::uint16_t);

      value.data1 = data1;
      value.data2 = static_cast<std::uint16_t>(data2);
      value.data3 = static_cast<std::uint16_t>(data3);
      std::copy_n(blob.begin() + static_cast<std::ptrdiff_t>(offset), value.data4.size(), value.data4.begin());
      offset += value.data4.size();
      return true;
    }
  }  // namespace

  bool valid_temporary_display_profile(const std::uint64_t display_id, const std::uint32_t connector_index) {
    return display_id != 0 &&
           connector_index >= kWindowsDriverMaxPermanentDisplays &&
           connector_index < kWindowsDriverMaxPermanentDisplays + kWindowsDriverMaxTemporaryDisplays;
  }

  std::optional<std::vector<TemporaryDisplayProfile>> parse_temporary_display_profiles_blob(
    const std::span<const std::uint8_t> blob
  ) {
    std::size_t offset {};
    std::uint32_t schema_version {};
    std::uint32_t profile_count {};
    if (!read_u32(blob, offset, schema_version) ||
        !read_u32(blob, offset, profile_count) ||
        schema_version != kWindowsDriverPersistentStateSchemaVersion ||
        profile_count > kWindowsDriverMaxTemporaryDisplays ||
        blob.size() != kTemporaryDisplayProfilesHeaderBytes + profile_count * kTemporaryDisplayProfileBytes) {
      return std::nullopt;
    }

    std::vector<TemporaryDisplayProfile> profiles;
    profiles.reserve(profile_count);
    for (std::uint32_t index = 0; index < profile_count; ++index) {
      TemporaryDisplayProfile profile {};
      if (!read_u64(blob, offset, profile.display_id) ||
          !read_u32(blob, offset, profile.connector_index) ||
          !read_guid(blob, offset, profile.container_id) ||
          !read_u32(blob, offset, profile.edid_product_code) ||
          !read_u32(blob, offset, profile.edid_serial_number) ||
          !valid_temporary_display_profile(profile.display_id, profile.connector_index)) {
        return std::nullopt;
      }

      profiles.push_back(profile);
    }

    return profiles;
  }

  std::vector<std::uint8_t> serialize_temporary_display_profiles(
    const std::span<const TemporaryDisplayProfile> profiles
  ) {
    std::vector<std::uint8_t> blob;
    blob.reserve(kTemporaryDisplayProfilesHeaderBytes + profiles.size() * kTemporaryDisplayProfileBytes);
    append_u32(blob, kWindowsDriverPersistentStateSchemaVersion);
    append_u32(blob, static_cast<std::uint32_t>(profiles.size()));
    for (const auto &entry: profiles) {
      append_u64(blob, entry.display_id);
      append_u32(blob, entry.connector_index);
      append_guid(blob, entry.container_id);
      append_u32(blob, entry.edid_product_code);
      append_u32(blob, entry.edid_serial_number);
    }
    return blob;
  }

  std::optional<std::vector<TemporaryDisplayProfile>> upsert_temporary_display_profile(
    std::vector<TemporaryDisplayProfile> profiles,
    const TemporaryDisplayProfile &profile
  ) {
    if (!valid_temporary_display_profile(profile.display_id, profile.connector_index)) {
      return std::nullopt;
    }

    profiles.erase(
      std::remove_if(
        profiles.begin(),
        profiles.end(),
        [&](const TemporaryDisplayProfile &entry) {
          return entry.connector_index == profile.connector_index &&
                 entry.display_id != profile.display_id;
        }
      ),
      profiles.end()
    );

    const auto existing = std::find_if(
      profiles.begin(),
      profiles.end(),
      [&](const TemporaryDisplayProfile &entry) {
        return entry.display_id == profile.display_id;
      }
    );
    if (existing != profiles.end()) {
      *existing = profile;
    } else {
      if (profiles.size() >= kWindowsDriverMaxTemporaryDisplays) {
        return std::nullopt;
      }
      profiles.push_back(profile);
    }

    return profiles;
  }

  std::vector<TemporaryDisplayProfile> remove_temporary_display_profile(
    std::vector<TemporaryDisplayProfile> profiles,
    const std::uint64_t display_id
  ) {
    profiles.erase(
      std::remove_if(
        profiles.begin(),
        profiles.end(),
        [display_id](const TemporaryDisplayProfile &entry) {
          return entry.display_id == display_id;
        }
      ),
      profiles.end()
    );
    return profiles;
  }

  std::map<std::uint64_t, std::uint32_t> temporary_connector_reservations(
    const std::span<const TemporaryDisplayProfile> profiles
  ) {
    std::map<std::uint64_t, std::uint32_t> reservations;
    for (const auto &profile: profiles) {
      reservations.emplace(profile.display_id, profile.connector_index);
    }
    return reservations;
  }
}  // namespace virtual_display::driver
