/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "timeseries_qwt.h"
#include "qwt_scale_map.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QString>

RangeOpt QwtSeriesWrapper::getVisualizationRangeY(Range range_x)
{
  if (range_x.min <= std::numeric_limits<double>::lowest() &&
      range_x.min <= std::numeric_limits<double>::max())
  {
    return _data->rangeY();
  }

  double min_y = (std::numeric_limits<double>::max());
  double max_y = (std::numeric_limits<double>::lowest());

  for (size_t i = 0; i < size(); i++)
  {
    const double Y = sample(i).y();
    min_y = std::min(min_y, Y);
    max_y = std::max(max_y, Y);
  }
  return Range{ min_y, max_y };
}

RangeOpt QwtTimeseries::getVisualizationRangeY(Range range_X)
{
  if (_ts_data->size() == 0)
  {
    return {};
  }

  // Data X bounds in display coordinates (after time offset)
  const double data_x_min = _ts_data->front().x - _time_offset;
  const double data_x_max = _ts_data->back().x - _time_offset;

  // All data is to the left of the visible range: hold the last value forward
  // so the curve still contributes to Y auto-scaling.
  if (data_x_max < range_X.min)
  {
    const double y = _ts_data->back().y;
    return Range{ y, y };
  }
  // All data is to the right of the visible range: nothing to hold yet.
  if (data_x_min > range_X.max)
  {
    return {};
  }

  int first_index = _ts_data->getIndexFromX(range_X.min + _time_offset);
  int last_index = _ts_data->getIndexFromX(range_X.max + _time_offset);

  if (first_index > last_index || first_index < 0 || last_index < 0)
  {
    return {};
  }

  if (first_index == 0 && last_index == plotData()->size() - 1)
  {
    return _ts_data->rangeY();
  }

  double min_y = (std::numeric_limits<double>::max());
  double max_y = (std::numeric_limits<double>::lowest());

  for (size_t i = first_index; i < last_index; i++)
  {
    const double Y = sample(i).y();
    min_y = std::min(min_y, Y);
    max_y = std::max(max_y, Y);
  }
  return Range{ min_y, max_y };
}

std::optional<QPointF> QwtTimeseries::sampleFromTime(double t)
{
  int index = _ts_data->getIndexFromX(t);
  if (index < 0)
  {
    return {};
  }
  const auto& p = plotData()->at(size_t(index));
  return QPointF(p.x, p.y);
}

TransformedTimeseries::TransformedTimeseries(const PlotData* source_data)
  : QwtTimeseries(&_dst_data), _dst_data(source_data->plotName(), {}), _src_data(source_data)
{
}

TransformFunction::Ptr TransformedTimeseries::transform()
{
  return _transform;
}

bool TransformedTimeseries::setTransform(QString transform_ID)
{
  if (transformName() == transform_ID)
  {
    return true;
  }
  if (transform_ID.isEmpty())
  {
    _transform.reset();
    return false;
  }

  _transform = TransformFactory::create(transform_ID.toStdString());
  if (!_transform)
  {
    return false;
  }
  std::vector<PlotData*> dest = { &_dst_data };
  _dst_data.clear();
  _transform->setData(nullptr, { _src_data }, dest);
  return true;
}

void TransformedTimeseries::updateCache(bool reset_old_data)
{
  if (_transform)
  {
    if (reset_old_data)
    {
      _dst_data.clear();
      _transform->reset();
    }
    std::vector<PlotData*> dest = { &_dst_data };
    _transform->calculate();
  }
  else
  {
    // TODO: optimize ??
    _dst_data.clear();
    for (size_t i = 0; i < _src_data->size(); i++)
    {
      _dst_data.pushBack(_src_data->at(i));
    }
  }
}

QString TransformedTimeseries::transformName()
{
  return (!_transform) ? QString() : _transform->name();
}

QString TransformedTimeseries::alias() const
{
  return _alias;
}

void TransformedTimeseries::setAlias(QString alias)
{
  _alias = alias;
}

QRectF QwtSeriesWrapper::boundingRect() const
{
  if (size() == 0)
  {
    return {};
  }
  auto range_x = plotData()->rangeX().value();
  auto range_y = plotData()->rangeY().value();

  QRectF box;
  box.setLeft(range_x.min);
  box.setRight(range_x.max);
  box.setTop(range_y.max);
  box.setBottom(range_y.min);
  return box;
}

QRectF QwtTimeseries::boundingRect() const
{
  if (size() == 0)
  {
    return {};
  }
  auto range_x = plotData()->rangeX().value();
  auto range_y = plotData()->rangeY().value();

  QRectF box;
  box.setLeft(range_x.min - _time_offset);
  box.setRight(range_x.max - _time_offset);
  box.setTop(range_y.max);
  box.setBottom(range_y.min);
  return box;
}

QPointF QwtSeriesWrapper::sample(size_t i) const
{
  const auto& p = _data->at(i);
  return QPointF(p.x, p.y);
}

QPointF QwtTimeseries::sample(size_t i) const
{
  const auto& p = _ts_data->at(i);
  return QPointF(p.x - _time_offset, p.y);
}

size_t QwtSeriesWrapper::size() const
{
  return _data->size();
}

void QwtTimeseries::setTimeOffset(double offset)
{
  _time_offset = offset;
}

RangeOpt QwtSeriesWrapper::getVisualizationRangeX()
{
  if (this->size() < 2)
  {
    return {};
  }
  else
  {
    return _data->rangeX();
  }
}

RangeOpt QwtTimeseries::getVisualizationRangeX()
{
  if (this->size() < 2)
  {
    return {};
  }
  else
  {
    auto range = _ts_data->rangeX().value();
    return RangeOpt({ range.min - _time_offset, range.max - _time_offset });
  }
}

const PlotDataBase<double, double>* QwtSeriesWrapper::plotData() const
{
  return _data;
}

//---------------------------------------------------------

void PJPlotCurve::drawSeries(QPainter* painter, const QwtScaleMap& xMap,
                             const QwtScaleMap& yMap, const QRectF& canvasRect,
                             int from, int to) const
{
  // Default rendering of the actual samples (does nothing if all samples
  // happen to fall outside the visible canvas).
  QwtPlotCurve::drawSeries(painter, xMap, yMap, canvasRect, from, to);

  const auto* series = data();
  const auto* ts = dynamic_cast<const QwtTimeseries*>(series);
  if (!ts)
  {
    return;  // Not a time series (e.g. XY plot): no sample-and-hold extension.
  }
  const int n = static_cast<int>(ts->size());
  if (n == 0)
  {
    return;
  }

  // Only line-based styles get extended. Dots and Sticks render per-sample
  // markers, where extending makes no visual sense.
  const auto cs = style();
  if (cs != Lines && cs != LinesAndDots && cs != Steps)
  {
    return;
  }

  // Visible X range expressed in data coordinates.
  double x_left = xMap.invTransform(canvasRect.left());
  double x_right = xMap.invTransform(canvasRect.right());
  if (x_left > x_right)
  {
    std::swap(x_left, x_right);
  }

  const QPointF first = ts->sample(0);
  const QPointF last = ts->sample(n - 1);

  painter->save();
  painter->setPen(pen());
  painter->setRenderHint(QPainter::Antialiasing,
                         testRenderHint(QwtPlotItem::RenderAntialiased));

  if (last.x() <= x_left)
  {
    // All samples are before the visible range — hold the last value across
    // the entire canvas so the user still sees the held line.
    const double y_pix = yMap.transform(last.y());
    painter->drawLine(QPointF(canvasRect.left(), y_pix),
                      QPointF(canvasRect.right(), y_pix));
  }
  else if (first.x() < x_right)
  {
    // Right extension: hold the last sample's value forward to the canvas
    // right edge whenever the data ends before the visible window does.
    if (last.x() < x_right)
    {
      const double x0_pix = xMap.transform(last.x());
      const double y_pix = yMap.transform(last.y());
      painter->drawLine(QPointF(x0_pix, y_pix),
                        QPointF(canvasRect.right(), y_pix));
    }
    // Left extension: if there's a sample at or before the canvas left edge,
    // hold its value into the visible window up to the next sample.
    if (first.x() < x_left)
    {
      int lo = 0;
      int hi = n - 1;
      while (lo < hi)
      {
        const int mid = (lo + hi + 1) / 2;
        if (ts->sample(mid).x() <= x_left)
        {
          lo = mid;
        }
        else
        {
          hi = mid - 1;
        }
      }
      const QPointF held = ts->sample(lo);
      double x_end_pix = canvasRect.right();
      if (lo + 1 < n)
      {
        x_end_pix = std::min<double>(canvasRect.right(),
                                     xMap.transform(ts->sample(lo + 1).x()));
      }
      const double y_pix = yMap.transform(held.y());
      painter->drawLine(QPointF(canvasRect.left(), y_pix),
                        QPointF(x_end_pix, y_pix));
    }
  }
  // else: data is entirely after the visible range — no held value yet.

  painter->restore();
}
