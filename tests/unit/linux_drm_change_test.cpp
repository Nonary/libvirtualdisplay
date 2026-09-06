// SPDX-License-Identifier: GPL-2.0+

#ifdef __linux__

#include <gtest/gtest.h>

#include <vibeshine_drm_change.h>
#include <vibeshine_drm_present.h>
#include <vibeshine_drm_vrr.h>

TEST(LinuxDrmPresentPolicy, ReturnsACompletedSequenceWithoutWaitingForQueueDrain) {
  vibeshine_drm_wait_present request {};
  request.abi_version = VIBESHINE_DRM_PRESENT_ABI_VERSION;
  request.sequence = 7;
  request.timeout_ms = 16;

  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 8, VIBESHINE_DRM_PRESENT_MAX_WAITERS),
    VIBESHINE_DRM_PRESENT_RETURN_CURRENT
  );
  EXPECT_EQ(
    vibeshine_drm_present_decide_wait(&request, 7, VIBESHINE_DRM_PRESENT_MAX_WAITERS - 1),
    VIBESHINE_DRM_PRESENT_REGISTER_WAITER
  );
}

TEST(LinuxDrmChangePolicy, DistinguishesNoOpFromContentAndStateChanges) {
  vibeshine_drm_plane_snapshot old_state {};
  old_state.present = true;
  old_state.visible = true;
  old_state.crtc = 1;
  old_state.framebuffer = 2;
  auto new_state = old_state;

  EXPECT_FALSE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));

  new_state.framebuffer = 3;
  EXPECT_TRUE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));
  new_state = old_state;

  new_state.content_update = true;
  EXPECT_TRUE(vibeshine_drm_plane_changes_scanout(&old_state, &new_state));
}

TEST(LinuxDrmVrrPolicy, EnforcesTheModePeriodAsMaximumRefreshRate) {
  constexpr unsigned long long period_ns = 8'333'333;

  EXPECT_EQ(vibeshine_drm_vrr_presentation_deadline_ns(0, 100, period_ns), 100);
  EXPECT_EQ(
    vibeshine_drm_vrr_presentation_deadline_ns(1'000'000'000, 1'007'000'000, period_ns),
    1'008'333'333
  );
  EXPECT_EQ(
    vibeshine_drm_vrr_presentation_deadline_ns(1'000'000'000, 1'010'000'000, period_ns),
    1'010'000'000
  );
}

TEST(LinuxDrmVrrPolicy, DoesNotWrapADeadlineIntoThePast) {
  EXPECT_EQ(
    vibeshine_drm_vrr_presentation_deadline_ns(~0ULL - 2, 100, 10),
    ~0ULL
  );
}

#endif
