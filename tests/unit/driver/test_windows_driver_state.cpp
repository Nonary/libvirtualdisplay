#include "virtual_display/driver/windows_driver_state.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  vdd::TemporaryDisplayProfile make_profile(
    const std::uint64_t display_id,
    const std::uint32_t connector_index
  ) {
    return {
      display_id,
      connector_index,
      vdd::Guid {
        0x11223344u,
        0x5566u,
        0x7788u,
        {0x90u, 0xabu, 0xcdu, 0xefu, 0x01u, 0x23u, 0x45u, static_cast<std::uint8_t>(display_id)}
      },
      static_cast<std::uint32_t>(0x5000u | (display_id & 0xffu)),
      static_cast<std::uint32_t>(0x7000u | (display_id & 0xffu))
    };
  }
}  // namespace

TEST(VirtualDisplayWindowsDriverState, SerializesTemporaryProfilesRoundTrip) {
  const std::vector<vdd::TemporaryDisplayProfile> profiles {
    make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays),
    make_profile(0x6000000000000002ull, vdd::kWindowsDriverMaxPermanentDisplays + 1u)
  };

  const auto blob = vdd::serialize_temporary_display_profiles(profiles);

  const auto parsed = vdd::parse_temporary_display_profiles_blob(blob);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, profiles);
}

TEST(VirtualDisplayWindowsDriverState, RejectsInvalidTemporaryProfileBlobShapeAndRange) {
  const std::vector<vdd::TemporaryDisplayProfile> profiles {
    make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays)
  };
  auto blob = vdd::serialize_temporary_display_profiles(profiles);

  auto wrong_schema = blob;
  wrong_schema[0] = 2u;
  EXPECT_FALSE(vdd::parse_temporary_display_profiles_blob(wrong_schema).has_value());

  auto truncated = blob;
  truncated.pop_back();
  EXPECT_FALSE(vdd::parse_temporary_display_profiles_blob(truncated).has_value());

  auto invalid_connector = profiles;
  invalid_connector[0].connector_index = vdd::kWindowsDriverMaxPermanentDisplays - 1u;
  EXPECT_FALSE(vdd::parse_temporary_display_profiles_blob(
    vdd::serialize_temporary_display_profiles(invalid_connector)
  ).has_value());

  auto too_high_connector = profiles;
  too_high_connector[0].connector_index =
    vdd::kWindowsDriverMaxPermanentDisplays + vdd::kWindowsDriverMaxTemporaryDisplays;
  EXPECT_FALSE(vdd::parse_temporary_display_profiles_blob(
    vdd::serialize_temporary_display_profiles(too_high_connector)
  ).has_value());

  auto invalid_display_id = profiles;
  invalid_display_id[0].display_id = 0;
  EXPECT_FALSE(vdd::parse_temporary_display_profiles_blob(
    vdd::serialize_temporary_display_profiles(invalid_display_id)
  ).has_value());
}

TEST(VirtualDisplayWindowsDriverState, UsesVersionedFixedRecordFormatForPersistentProfiles) {
  const std::vector<vdd::TemporaryDisplayProfile> profiles {
    make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays)
  };

  const auto blob = vdd::serialize_temporary_display_profiles(profiles);
  const std::vector<std::uint8_t> expected {
    0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x60,
    0x04, 0x00, 0x00, 0x00,
    0x44, 0x33, 0x22, 0x11,
    0x66, 0x55,
    0x88, 0x77,
    0x90, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x01,
    0x01, 0x50, 0x00, 0x00,
    0x01, 0x70, 0x00, 0x00
  };

  EXPECT_EQ(blob, expected);
}

TEST(VirtualDisplayWindowsDriverState, UpsertReplacesExistingDisplayAndConflictingConnectorReservation) {
  std::vector<vdd::TemporaryDisplayProfile> profiles {
    make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays),
    make_profile(0x6000000000000002ull, vdd::kWindowsDriverMaxPermanentDisplays + 1u)
  };
  const auto replacement = make_profile(0x6000000000000003ull, vdd::kWindowsDriverMaxPermanentDisplays + 1u);

  const auto updated = vdd::upsert_temporary_display_profile(std::move(profiles), replacement);
  ASSERT_TRUE(updated.has_value());
  ASSERT_EQ(updated->size(), 2u);
  EXPECT_EQ((*updated)[0].display_id, 0x6000000000000001ull);
  EXPECT_EQ((*updated)[1], replacement);

  auto same_display = vdd::upsert_temporary_display_profile(*updated, make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays + 2u));
  ASSERT_TRUE(same_display.has_value());
  ASSERT_EQ(same_display->size(), 2u);
  EXPECT_EQ((*same_display)[0].connector_index, vdd::kWindowsDriverMaxPermanentDisplays + 2u);
}

TEST(VirtualDisplayWindowsDriverState, UpsertRejectsInvalidOrFullProfileSet) {
  EXPECT_FALSE(vdd::upsert_temporary_display_profile(
    {},
    make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays - 1u)
  ).has_value());

  std::vector<vdd::TemporaryDisplayProfile> profiles;
  for (std::uint32_t index = 0; index < vdd::kWindowsDriverMaxTemporaryDisplays; ++index) {
    profiles.push_back(make_profile(
      0x6000000000000100ull + index,
      vdd::kWindowsDriverMaxPermanentDisplays
    ));
  }

  EXPECT_FALSE(vdd::upsert_temporary_display_profile(
    profiles,
    make_profile(0x6000000000000200ull, vdd::kWindowsDriverMaxPermanentDisplays + vdd::kWindowsDriverMaxTemporaryDisplays - 1u)
  ).has_value());

  EXPECT_TRUE(vdd::upsert_temporary_display_profile(
    profiles,
    make_profile(0x6000000000000100ull, vdd::kWindowsDriverMaxPermanentDisplays)
  ).has_value());
}

TEST(VirtualDisplayWindowsDriverState, RemoveAndMapTemporaryConnectorReservations) {
  std::vector<vdd::TemporaryDisplayProfile> profiles {
    make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays),
    make_profile(0x6000000000000002ull, vdd::kWindowsDriverMaxPermanentDisplays + 1u)
  };

  profiles = vdd::remove_temporary_display_profile(std::move(profiles), 0x6000000000000001ull);
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0].display_id, 0x6000000000000002ull);

  const auto reservations = vdd::temporary_connector_reservations(profiles);
  ASSERT_EQ(reservations.size(), 1u);
  EXPECT_EQ(reservations.at(0x6000000000000002ull), vdd::kWindowsDriverMaxPermanentDisplays + 1u);
}

TEST(VirtualDisplayWindowsDriverState, StaleProfilePinsConnectorAndIdentityAcrossDriverRestarts) {
  // A temporary display whose departure never completed (host crash, wedged
  // teardown, power loss) keeps its persisted profile. Every subsequent driver
  // start restores the same display_id -> connector reservation and the same
  // EDID identity, so a Windows-side wedge keyed to that (connector, identity)
  // pair recurs deterministically across reboots; only an explicit
  // remove_temporary_display_profile (a completed departure) clears it. This
  // pins the mechanism that makes the field enumeration wedge reboot-proof.
  const auto stale = make_profile(0x6000000000000001ull, vdd::kWindowsDriverMaxPermanentDisplays);

  auto blob = vdd::serialize_temporary_display_profiles(std::vector {stale});
  for (int boot = 0; boot < 3; ++boot) {
    const auto restored = vdd::parse_temporary_display_profiles_blob(blob);
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->size(), 1u);
    EXPECT_EQ((*restored)[0], stale);

    const auto reservations = vdd::temporary_connector_reservations(*restored);
    ASSERT_EQ(reservations.size(), 1u);
    EXPECT_EQ(reservations.at(stale.display_id), stale.connector_index);

    // The next session recreates the same display_id; upsert keeps the same
    // slot and identity rather than rotating to a fresh one.
    const auto upserted = vdd::upsert_temporary_display_profile(*restored, stale);
    ASSERT_TRUE(upserted.has_value());
    blob = vdd::serialize_temporary_display_profiles(*upserted);
  }

  const auto cleared = vdd::remove_temporary_display_profile(
    *vdd::parse_temporary_display_profiles_blob(blob),
    stale.display_id
  );
  EXPECT_TRUE(cleared.empty());
}
