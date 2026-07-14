#include "virtual_display/driver/display_identity.h"

namespace virtual_display::driver {
  std::uint64_t permanent_display_id(const std::uint32_t index) {
    return kPermanentDisplayIdBase | (static_cast<std::uint64_t>(index) + 1ull);
  }

  std::uint16_t permanent_product_code(const std::uint32_t index) {
    return static_cast<std::uint16_t>(kPermanentDisplayProductCodeBase | (index & 0x0fffu));
  }

  std::uint32_t serial_number_from_display_id(const std::uint64_t display_id) {
    const auto folded = static_cast<std::uint32_t>(display_id) ^
                        static_cast<std::uint32_t>(display_id >> 32);
    return folded == 0 ? 1 : folded;
  }

  std::uint16_t product_code_from_display_id(const std::uint64_t display_id) {
    return static_cast<std::uint16_t>(kTemporaryDisplayProductCodeBase | (display_id & 0x0fffu));
  }

  Guid container_guid_from_display_id(const std::uint64_t display_id) {
    return Guid {
      0x9161d0d5,
      static_cast<std::uint16_t>((display_id >> 48) & 0xffffu),
      static_cast<std::uint16_t>((display_id >> 32) & 0xffffu),
      {
        0x93,
        0x7d,
        static_cast<std::uint8_t>((display_id >> 24) & 0xffu),
        static_cast<std::uint8_t>((display_id >> 16) & 0xffu),
        static_cast<std::uint8_t>((display_id >> 8) & 0xffu),
        static_cast<std::uint8_t>(display_id & 0xffu),
        0x53,
        0x44
      }
    };
  }

  EdidOptions edid_options_for_temporary_display(const TemporaryDisplayRecord &record) {
    const auto identity_display_id = record.identity_display_id == 0 ?
      record.display_id :
      record.identity_display_id;

    EdidOptions options {};
    options.manufacturer_id = kSunshineDriverManufacturerId;
    options.product_code = product_code_from_display_id(identity_display_id);
    options.serial_number = serial_number_from_display_id(identity_display_id);
    options.width = record.width;
    options.height = record.height;
    options.physical_width_mm = record.physical_width_mm;
    options.physical_height_mm = record.physical_height_mm;
    options.hdr_max_luminance_nits = record.hdr_max_luminance_nits;
    options.refresh_rate_millihz = record.refresh_rate_millihz;
    options.monitor_name = record.display_name;
    options.hdr_supported = true;
    return options;
  }
}  // namespace virtual_display::driver
