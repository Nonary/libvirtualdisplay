// SPDX-License-Identifier: GPL-2.0+
#ifdef __linux__
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <vibeshine_drm_capture.h>

TEST(LinuxCaptureLease, RetirementRejectsActiveReadersAndBlocksNewReaders) {
  vibeshine_capture_state state {};
  ASSERT_TRUE(vibeshine_capture_acquire(&state, false));
  EXPECT_FALSE(vibeshine_capture_retire(&state, false));
  EXPECT_EQ(state.retiring, 0u);
  EXPECT_TRUE(vibeshine_capture_release(&state));
  ASSERT_TRUE(vibeshine_capture_retire(&state, false));
  ASSERT_TRUE(vibeshine_capture_retire(&state, false));
  EXPECT_FALSE(vibeshine_capture_acquire(&state, false));
  EXPECT_TRUE(vibeshine_capture_publish(&state));
  EXPECT_FALSE(vibeshine_capture_acquire(&state, false));
  EXPECT_TRUE(vibeshine_capture_publish(&state));
  EXPECT_TRUE(vibeshine_capture_acquire(&state, false));
}
TEST(LinuxCaptureLease, AcquisitionAndRetirementShareOneLinearizationPoint) {
  for (int iteration = 0; iteration < 1000; ++iteration) {
    vibeshine_capture_state state {};
    std::mutex lock;
    bool acquired = false;
    bool retired = false;
    std::thread reader([&] { std::lock_guard guard(lock); acquired = vibeshine_capture_acquire(&state, false); });
    std::thread committer([&] { std::lock_guard guard(lock); retired = vibeshine_capture_retire(&state, false); });
    reader.join(); committer.join();
    EXPECT_NE(acquired, retired);
    EXPECT_EQ(state.retiring, retired ? 1u : 0u);
    EXPECT_EQ(state.readers, acquired ? 1u : 0u);
    if (acquired) {
      EXPECT_TRUE(vibeshine_capture_release(&state));
      EXPECT_TRUE(vibeshine_capture_retire(&state, false));
    }
    EXPECT_FALSE(vibeshine_capture_acquire(&state, false));
    EXPECT_TRUE(vibeshine_capture_publish(&state));
  }
}
TEST(LinuxCaptureLease, FailedAdmissionRollsBackAndAllowsRetry) {
  vibeshine_capture_state first {};
  vibeshine_capture_state second {};
  ASSERT_TRUE(vibeshine_capture_acquire(&second, false));
  ASSERT_TRUE(vibeshine_capture_retire(&first, false));
  EXPECT_FALSE(vibeshine_capture_retire(&second, false));
  ASSERT_TRUE(vibeshine_capture_publish(&first));
  EXPECT_TRUE(vibeshine_capture_acquire(&first, false));
  EXPECT_TRUE(vibeshine_capture_release(&first));
  EXPECT_TRUE(vibeshine_capture_release(&second));
  EXPECT_TRUE(vibeshine_capture_retire(&first, false));
  EXPECT_TRUE(vibeshine_capture_retire(&second, false));
  EXPECT_TRUE(vibeshine_capture_publish(&first));
  EXPECT_TRUE(vibeshine_capture_publish(&second));
}
TEST(LinuxCaptureLease, AsyncRetirementAllowsExistingReadersButBlocksNewOnes) {
  vibeshine_capture_state state {};
  ASSERT_TRUE(vibeshine_capture_acquire(&state, false));
  ASSERT_TRUE(vibeshine_capture_retire(&state, true));
  EXPECT_FALSE(vibeshine_capture_acquire(&state, false));
  EXPECT_FALSE(vibeshine_capture_retire(&state, false));
  EXPECT_TRUE(vibeshine_capture_release(&state));
  EXPECT_TRUE(vibeshine_capture_publish(&state));
  EXPECT_TRUE(vibeshine_capture_acquire(&state, false));
}
TEST(LinuxCaptureLease, ReadersAreBoundedAndStoppingRejectsNewWork) {
  vibeshine_capture_state state {};
  EXPECT_FALSE(vibeshine_capture_acquire(&state, true));
  for (unsigned i = 0; i < VIBESHINE_CAPTURE_MAX_READERS; ++i)
    EXPECT_TRUE(vibeshine_capture_acquire(&state, false));
  EXPECT_FALSE(vibeshine_capture_acquire(&state, false));
  for (unsigned i = 0; i < VIBESHINE_CAPTURE_MAX_READERS; ++i)
    EXPECT_TRUE(vibeshine_capture_release(&state));
  EXPECT_FALSE(vibeshine_capture_release(&state));
  EXPECT_FALSE(vibeshine_capture_publish(&state));
}
#endif
