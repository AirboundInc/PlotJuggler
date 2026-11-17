// SPDX-License-Identifier: MPL-2.0
//
// The one progress bar over every stage of a load, and how it carries a cancel.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "load_progress.h"

namespace
{

using airbound::LoadProgress;

// A cancel that a stage stopped on stays a cancel, even when the next update is too small to be
// reported (the terrain stage stops on a refused update, then the load calls advance(1.0)).
TEST(LoadProgress, CancelStaysLatchedThroughThrottledUpdates)
{
  std::vector<uint64_t> reported;
  bool cancel = false;
  LoadProgress progress({99.0, 1.0}, [&](uint64_t value) {
    reported.push_back(value);
    return !cancel;
  });
  ASSERT_TRUE(progress.begin(0));
  ASSERT_TRUE(progress.begin(1));  // the last stage spans the last 1% of the bar
  cancel = true;
  EXPECT_FALSE(progress.advance(0.9));
  cancel = false;  // the host is not asked again: only its first answer counts
  EXPECT_FALSE(progress.advance(1.0)) << "a throttled update turned the cancelled load into a success";
  EXPECT_FALSE(progress.begin(1));
}

TEST(LoadProgress, ReportsEachStageInOrder)
{
  std::vector<uint64_t> reported;
  LoadProgress progress({1.0, 1.0}, [&](uint64_t value) {
    reported.push_back(value);
    return true;
  });
  ASSERT_TRUE(progress.begin(0));
  ASSERT_TRUE(progress.advance(0.5));
  ASSERT_TRUE(progress.advance(0.501));  // under 0.2% of the bar: not reported
  ASSERT_TRUE(progress.begin(1));
  ASSERT_TRUE(progress.advance(1.0));
  EXPECT_EQ(reported, (std::vector<uint64_t>{0, 250, 500, LoadProgress::kTotal}));
}

}  // namespace
