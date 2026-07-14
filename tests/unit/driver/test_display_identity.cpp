#include <gtest/gtest.h>
#include "virtual_display/driver/display_identity.h"

namespace vdd = virtual_display::driver;

namespace {

  vdd::TemporaryDisplayRecord record_for_display_id(const std::uint64_t display_id) {
    return vdd::TemporaryDisplayRecord {
      100,
      display_id,
      3840,
      2160,
      700,
      390,
      120'000,
      30'000,
      8,
      "Sunshine HDR",
      std::chrono::steady_clock::now()
    };
  }
}  // namespace

TEST(VirtualDisplayDriverDisplayIdentity, DerivesStableNonzeroSerialFromDisplayId) {
  EXPECT_EQ(vdd::serial_number_from_display_id(0), 1u);
  EXPECT_EQ(
    vdd::serial_number_from_display_id(0x1234567800000000ull),
    vdd::serial_number_from_display_id(0x0000000012345678ull)
  );
  EXPECT_NE(
    vdd::serial_number_from_display_id(0x0000000012345678ull),
    vdd::serial_number_from_display_id(0x0000000087654321ull)
  );
}

TEST(VirtualDisplayDriverDisplayIdentity, DerivesPermanentDisplayIdentityFromSlot) {
  EXPECT_EQ(vdd::permanent_display_id(0), 0x7000000000000001ull);
  EXPECT_EQ(vdd::permanent_display_id(3), 0x7000000000000004ull);
  EXPECT_EQ(vdd::permanent_display_id(0xffffffffu), 0x7000000100000000ull);
  EXPECT_EQ(vdd::permanent_product_code(0), 0x4000u);
  EXPECT_EQ(vdd::permanent_product_code(3), 0x4003u);
}

TEST(VirtualDisplayDriverDisplayIdentity, DerivesProductCodeFromTemporaryRange) {
  EXPECT_EQ(vdd::product_code_from_display_id(0), vdd::kTemporaryDisplayProductCodeBase);
  EXPECT_EQ(vdd::product_code_from_display_id(0x1234), 0x5234);
}

TEST(VirtualDisplayDriverDisplayIdentity, DerivesStableContainerGuidFromDisplayId) {
  const auto first = vdd::container_guid_from_display_id(0x1122334455667788ull);
  const auto second = vdd::container_guid_from_display_id(0x1122334455667788ull);
  const auto different = vdd::container_guid_from_display_id(0x1122334455667789ull);

  EXPECT_EQ(first, second);
  EXPECT_NE(first, different);
  EXPECT_EQ(first.data1, 0x9161d0d5u);
  EXPECT_EQ(first.data4[6], 0x53u);
  EXPECT_EQ(first.data4[7], 0x44u);
}

TEST(VirtualDisplayDriverDisplayIdentity, BuildsHdrEdidOptionsFromDisplayRecord) {
  auto record = record_for_display_id(0x12345678);
  record.hdr_max_luminance_nits = 2000;
  const auto options = vdd::edid_options_for_temporary_display(record);

  EXPECT_EQ(options.manufacturer_id, vdd::kSunshineDriverManufacturerId);
  EXPECT_EQ(options.product_code, vdd::product_code_from_display_id(record.display_id));
  EXPECT_EQ(options.serial_number, vdd::serial_number_from_display_id(record.display_id));
  EXPECT_EQ(options.width, record.width);
  EXPECT_EQ(options.height, record.height);
  EXPECT_EQ(options.physical_width_mm, record.physical_width_mm);
  EXPECT_EQ(options.physical_height_mm, record.physical_height_mm);
  EXPECT_EQ(options.hdr_max_luminance_nits, record.hdr_max_luminance_nits);
  EXPECT_EQ(options.refresh_rate_millihz, record.refresh_rate_millihz);
  EXPECT_EQ(options.monitor_name, record.display_name);
  EXPECT_TRUE(options.hdr_supported);

  const auto edid = vdd::create_edid(options);
  EXPECT_EQ(vdd::read_manufacturer_id(edid), (std::array<char, 3> {'S', 'D', 'D'}));
  EXPECT_EQ(vdd::read_product_code(edid), 0x5678u);
  EXPECT_EQ(vdd::read_monitor_name(edid), record.display_name);
  EXPECT_TRUE(vdd::has_hdr_static_metadata(edid));
}
