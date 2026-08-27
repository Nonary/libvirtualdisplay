// SPDX-License-Identifier: GPL-2.0+

#ifdef __linux__

#include <gtest/gtest.h>

#include <vibeshine_drm_change.h>

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

#endif
