/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef REALSLIDER_H
#define REALSLIDER_H

#include <QSlider>
#include <QWheelEvent>
#include <functional>

class RealSlider : public QSlider
{
  Q_OBJECT
public:
  RealSlider(QWidget* parent = nullptr);

  void setLimits(double min, double max, int steps);

  double getValue() const;

  void setRealValue(double val);

  double getMaximum() const
  {
    return _max_value;
  }

  double getMinimum() const
  {
    return _min_value;
  }

  void setRealStepValue(double step);

  /// Provide the amount of time that a single mouse wheel notch should move the slider.
  /// It is invoked on every wheel event, so that the step can follow the zoom level of
  /// the plots. Returning a value <= 0 (or not setting a provider at all) restores the
  /// default behaviour, i.e. the step set by setRealStepValue().
  void setWheelStepProvider(std::function<double()> provider);

protected:
  void wheelEvent(QWheelEvent* event) override;

private slots:
  void onValueChanged(int value);

signals:
  void realValueChanged(double);

private:
  /// Convert an amount of time into a number of slider ticks (at least one).
  int realToTicks(double step) const;

  double _min_value;
  double _max_value;
  std::function<double()> _wheel_step_provider;
  int _wheel_remainder = 0;
};
//-------------------------------------------------------------

inline RealSlider::RealSlider(QWidget* parent) : QSlider(parent)
{
  setLimits(0.0, 1.0, 1);
  connect(this, &QSlider::valueChanged, this, &RealSlider::onValueChanged);
}

inline void RealSlider::setLimits(double min, double max, int steps)
{
  _min_value = min;
  _max_value = max;
  QSlider::setRange(0, steps);
}

inline void RealSlider::setRealValue(double val)
{
  val = std::max(val, _min_value);
  val = std::min(val, _max_value);
  const double ratio = (val - _min_value) / (_max_value - _min_value);
  long pos = std::round((double)(maximum() - minimum()) * ratio + minimum());
  QSlider::setValue(pos);
}

inline int RealSlider::realToTicks(double step) const
{
  const double ratio = (_max_value - _min_value) / (double)(maximum() - minimum());
  if (ratio <= 0.0)
  {
    return 1;
  }
  return std::max(1, static_cast<int>(std::round(step / ratio)));
}

inline void RealSlider::setRealStepValue(double step)
{
  QSlider::setSingleStep(realToTicks(step));
}

inline void RealSlider::setWheelStepProvider(std::function<double()> provider)
{
  _wheel_step_provider = std::move(provider);
}

inline void RealSlider::wheelEvent(QWheelEvent* event)
{
  const double step = _wheel_step_provider ? _wheel_step_provider() : 0.0;

  if (step <= 0.0)
  {
    QSlider::wheelEvent(event);
    return;
  }

  // Accumulate the deltas, so that high resolution wheels (which report fractions
  // of a notch) are not silently discarded.
  _wheel_remainder += event->angleDelta().y();
  const int notches = _wheel_remainder / 120;

  if (notches != 0)
  {
    _wheel_remainder -= notches * 120;
    // scrolling up/away from the user moves forward in time
    setValue(value() + notches * realToTicks(step));
  }
  event->accept();
}

inline void RealSlider::onValueChanged(int value)
{
  int min = minimum();
  int max = maximum();
  const double ratio = (double)value / (double)(max - min);
  double posX = (_max_value - _min_value) * ratio + _min_value;
  emit realValueChanged(posX);
}

#endif  // REALSLIDER_H
