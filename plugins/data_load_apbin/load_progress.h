// SPDX-License-Identifier: MPL-2.0
//
// One progress bar over every stage of a load, each stage sized by its estimated cost, so the
// bar keeps moving until the load is done instead of reaching 100% when parsing ends.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <utility>
#include <vector>

namespace airbound
{

class LoadProgress
{
public:
  static constexpr uint64_t kTotal = 1000;  // the progress total given to the host

  // `costs`: estimated cost of each stage, in any unit (seconds here). `report` forwards a value
  // in [0, kTotal] to the host and returns false when the user cancelled.
  LoadProgress(std::vector<double> costs, std::function<bool(uint64_t)> report)
    : costs_(std::move(costs)), report_(std::move(report))
  {
  }

  // Re-estimates a stage that has not started yet. The bar not yet used is split again over the
  // stages left when the next one begins, so the bar never moves backwards.
  void set_cost(size_t stage, double cost)
  {
    if (stage > stage_ || !started_)
    {
      costs_[stage] = std::max(cost, 0.0);
    }
  }

  // Starts `stage`; stages run in order, and skipped stages count as done. False when cancelled.
  bool begin(size_t stage)
  {
    if (started_)
    {
      value_ = std::max(value_, static_cast<uint64_t>(base_ + span_));  // the previous stage is done
    }
    started_ = true;
    stage_ = stage;
    base_ = static_cast<double>(value_);
    const double remaining = std::accumulate(costs_.begin() + static_cast<std::ptrdiff_t>(stage), costs_.end(), 0.0);
    span_ = remaining > 0.0 ? (static_cast<double>(kTotal) - base_) * costs_[stage] / remaining : 0.0;
    return report_(value_);
  }

  // Moves to `fraction` (0..1) of the current stage. Only reports when the bar moves by 0.2% or
  // more. False when cancelled.
  bool advance(double fraction)
  {
    const auto value = static_cast<uint64_t>(base_ + span_ * std::clamp(fraction, 0.0, 1.0));
    if (value < value_ + 2)
    {
      return true;
    }
    value_ = std::min(value, kTotal);
    return report_(value_);
  }

private:
  std::vector<double> costs_;
  std::function<bool(uint64_t)> report_;
  bool started_ = false;
  size_t stage_ = 0;
  double base_ = 0.0;
  double span_ = 0.0;
  uint64_t value_ = 0;
};

}  // namespace airbound
