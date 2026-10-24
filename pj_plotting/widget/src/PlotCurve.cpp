// SPDX-License-Identifier: MPL-2.0
#include "pj_plotting/PlotCurve.h"

#include <qwt_painter.h>
#include <qwt_point_mapper.h>
#include <qwt_scale_map.h>

#include <QPainter>
#include <algorithm>
#include <cmath>

#include "pj_plotting/DatastoreCurveAdapter.h"

namespace PJ {

PlotCurve::PlotCurve(const QString& title) : QwtPlotCurve(title) {}

void PlotCurve::drawSeries(
    QPainter* painter, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& canvas_rect, int from,
    int to) const {
  QwtPlotCurve::drawSeries(painter, x_map, y_map, canvas_rect, from, to);

  // XY curves and other series types keep their plain rendering.
  if (dynamic_cast<const DatastoreCurveAdapter*>(data()) == nullptr) {
    return;
  }
  if (style() != Lines && style() != Steps) {
    return;  // Dots and Sticks mark individual samples; a held line would be invented data.
  }
  const int count = static_cast<int>(dataSize());
  // Only the pass that draws the final sample may extend it (to < 0 means "to the end").
  if (count == 0 || (to >= 0 && to < count - 1)) {
    return;
  }

  // The adapter serves the visible samples plus one guard sample beyond each
  // edge, so a last sample left of the right edge is the end of the series.
  const QPointF last = sample(static_cast<size_t>(count - 1));
  if (!std::isfinite(last.x()) || !std::isfinite(last.y())) {
    return;
  }
  const double x_start = std::max(x_map.transform(last.x()), canvas_rect.left());
  const double x_end = canvas_rect.right();
  if (x_start >= x_end) {
    return;
  }
  const double y = y_map.transform(last.y());

  painter->save();
  painter->setPen(pen());
  painter->setRenderHint(QPainter::Antialiasing, testRenderHint(QwtPlotItem::RenderAntialiased));
  QwtPainter::drawLine(painter, QPointF(x_start, y), QPointF(x_end, y));
  painter->restore();
}

void PlotCurve::setDotWidth(double width) {
  if (dot_width_ != width) {
    dot_width_ = width;
    itemChanged();
  }
}

double PlotCurve::dotWidth() const {
  return dot_width_;
}

void PlotCurve::drawCurve(
    QPainter* painter, int style, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& canvas_rect,
    int from, int to) const {
  QwtPlotCurve::drawCurve(painter, style, x_map, y_map, canvas_rect, from, to);
  if (style != QwtPlotCurve::Lines || dot_width_ <= 0.0) {
    return;
  }
  // SquareCap keeps the batched points on the cheap quad-stroking path; RoundCap
  // would turn every zero-length segment back into per-point arc geometry, and
  // FlatCap draws nothing at all for zero-length segments.
  const QPen line_pen = painter->pen();
  QPen dot_pen = line_pen;
  dot_pen.setWidthF(dot_width_);
  dot_pen.setCapStyle(Qt::SquareCap);
  painter->setPen(dot_pen);
  // Map and weed the points here instead of delegating to drawDots():
  // QwtPlotCurve::drawDots skips weeding whenever antialiasing is on, and the
  // OpenGL paint engine reports no rounding alignment — together that emits one
  // quad per SAMPLE. RoundPoints + WeedOutPoints with a valid bounding rect
  // selects the pixel-matrix path: one dot per covered pixel.
  QwtPointMapper mapper;
  mapper.setBoundingRect(canvas_rect);
  mapper.setFlag(QwtPointMapper::RoundPoints, true);
  mapper.setFlag(QwtPointMapper::WeedOutPoints, true);
  const QPolygonF points = mapper.toPointsF(x_map, y_map, data(), from, to);
  QwtPainter::drawPoints(painter, points);
  painter->setPen(line_pen);
}

}  // namespace PJ
