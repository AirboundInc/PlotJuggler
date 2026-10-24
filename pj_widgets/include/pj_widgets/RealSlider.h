#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSlider>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

namespace PJ {

// Double-precision slider: wraps QSlider's integer API with a floating-point
// range.
class RealSlider : public QSlider {
  Q_OBJECT
 public:
  explicit RealSlider(QWidget* parent = nullptr) : QSlider(parent) {
    setLimits(0.0, 1.0, 1);
    connect(this, &QSlider::valueChanged, this, &RealSlider::onValueChanged);
  }

  void setLimits(double min, double max, int steps) {
    min_value_ = min;
    max_value_ = max;
    QSlider::setRange(0, steps);
  }

  double getValue() const {
    const int span = maximum() - minimum();
    if (span <= 0) {
      return min_value_;
    }
    const double ratio = static_cast<double>(value() - minimum()) / static_cast<double>(span);
    return (max_value_ - min_value_) * ratio + min_value_;
  }

  void setRealValue(double val) {
    val = std::clamp(val, min_value_, max_value_);
    if (max_value_ == min_value_) {
      QSlider::setValue(minimum());
      return;
    }
    const double ratio = (val - min_value_) / (max_value_ - min_value_);
    const long pos = std::lround(static_cast<double>(maximum() - minimum()) * ratio + minimum());
    QSlider::setValue(static_cast<int>(pos));
  }

  double getMaximum() const {
    return max_value_;
  }
  double getMinimum() const {
    return min_value_;
  }

  void setRealStepValue(double step) {
    QSlider::setSingleStep(realToTicks(step));
  }

  // Amount of value one mouse-wheel notch moves the slider, asked on every
  // wheel event so it can follow e.g. the plots' zoom level. Unset, or a
  // result <= 0, keeps QSlider's own wheel handling (the single step).
  void setWheelStepProvider(std::function<double()> provider) {
    wheel_step_provider_ = std::move(provider);
  }

 protected:
  void wheelEvent(QWheelEvent* event) override {
    const double step = wheel_step_provider_ ? wheel_step_provider_() : 0.0;
    if (!(step > 0.0) || !std::isfinite(step)) {
      QSlider::wheelEvent(event);
      return;
    }
    // Accumulate partial notches so high-resolution wheels and touchpads still move it.
    const QPoint delta = event->angleDelta();
    wheel_remainder_ += delta.y() != 0 ? delta.y() : delta.x();
    const int notches = wheel_remainder_ / QWheelEvent::DefaultDeltasPerStep;
    if (notches != 0) {
      wheel_remainder_ -= notches * QWheelEvent::DefaultDeltasPerStep;
      // Scrolling up / away from the user moves forward, like QSlider.
      setValue(value() + notches * realToTicks(step));
    }
    event->accept();
  }

 signals:
  void realValueChanged(double value);

 private slots:
  void onValueChanged(int value) {
    const int min = minimum();
    const int max = maximum();
    const double ratio = static_cast<double>(value) / static_cast<double>(max - min);
    const double pos_x = (max_value_ - min_value_) * ratio + min_value_;
    emit realValueChanged(pos_x);
  }

 private:
  // A value span converted to slider ticks, at least one.
  [[nodiscard]] int realToTicks(double step) const {
    const int ticks = maximum() - minimum();
    if (ticks <= 0 || max_value_ <= min_value_) {
      return 1;
    }
    const double ratio = (max_value_ - min_value_) / static_cast<double>(ticks);
    return std::max(1, static_cast<int>(std::round(step / ratio)));
  }

  double min_value_ = 0.0;
  double max_value_ = 1.0;
  std::function<double()> wheel_step_provider_;
  int wheel_remainder_ = 0;
};

}  // namespace PJ
