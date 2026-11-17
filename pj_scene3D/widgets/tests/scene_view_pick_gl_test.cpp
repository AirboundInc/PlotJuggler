// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Real-GL test for the depth pick behind "orbit about the clicked surface" and the wheel
// zoom towards it: an opaque box hovering above the grid floor (an aircraft without
// terrain) must pick ON the box, at any camera distance, also after wheel zooms. While a
// frame is followed, wheel and drag stay locked on that frame instead.
//
// Skips without a usable GL >= 4.5 context, like shadow_persistence_gl_test.

#include <gtest/gtest.h>

#include <QApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gl_scene_test_support.h"  // haveGl, liveGlVersion
#include "pj_base/time.hpp"
#include "pj_gpu_test_gate.h"  // PJ_SKIP_WITHOUT_GPU
#include "pj_scene3d_core/camera/camera.h"
#include "pj_scene3d_core/tf/tf_buffer.h"
#include "pj_scene3d_core/tf/transform.h"
#include "pj_scene3d_widgets/passes/mesh_render_pass.h"
#include "pj_scene3d_widgets/scene3d_layer.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace {

using pj::scene3d::AABB;
using pj::scene3d::CameraState;
using pj::scene3d::FrameContext;
using pj::scene3d::MeshRenderPass;
using pj::scene3d::Scene3DLayer;
using pj::scene3d::SceneViewWidget;
using pj::scene3d::ViewParams;
using pj::scene3d::test::haveGl;
using pj::scene3d::test::liveGlVersion;

constexpr glm::vec3 kBoxCentre{0.0f, 0.0f, 30.0f};
constexpr float kBoxSize = 2.0f;

// An opaque box drawn at kBoxCentre (world), in the frame's camera-relative render space.
class HoveringBoxLayer final : public Scene3DLayer {
 public:
  void initializeGL() override {
    mesh_pass_.initializeGL();
  }
  void render(const ViewParams& view_params, const FrameContext& frame_ctx) override {
    MeshRenderPass::DrawCall box;
    box.kind = MeshRenderPass::GeometryKind::kBox;
    const glm::vec3 centre = kBoxCentre - glm::vec3(frame_ctx.render_origin);
    box.model = glm::scale(glm::translate(glm::mat4(1.0f), centre), glm::vec3(kBoxSize));
    mesh_pass_.renderVisuals(view_params, {box}, 1.0f);
  }
  void releaseGL() override {
    mesh_pass_.releaseGL();
  }
  [[nodiscard]] QStringList fallbackFrames() const override {
    return {};
  }
  [[nodiscard]] QString sourceFrame() const override {
    return {};
  }
  [[nodiscard]] PJ::SceneLayerInfo info() const override {
    PJ::SceneLayerInfo info;
    info.topic_id = PJ::ObjectTopicId{1};
    info.display_name = u"box"_s;
    info.visible = true;
    return info;
  }
  [[nodiscard]] PJ::Range<PJ::Timepoint> timeRange() const override {
    return PJ::liveTopicTimeRange(nullptr, PJ::ObjectTopicId{1});
  }
  bool attach(const PJ::SceneLayerContext& /*ctx*/) override {
    return true;
  }
  void detach() override {}
  void setTrackerTime(PJ::Timepoint /*time*/) override {}
  void setVisible(bool /*visible*/) override {}
  QWidget* createConfigWidget(QWidget* /*parent*/) override {
    return nullptr;
  }

 private:
  MeshRenderPass mesh_pass_;
};

class ScenePickGlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) {
      PJ_SKIP_WITHOUT_GPU("platform '" << platform.toStdString() << "' has no real-GL QOpenGLWidget support");
    }
    host_ = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(host_.get());
    layout->setContentsMargins(
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None),
        PJ::theme::space(PJ::theme::Space::None), PJ::theme::space(PJ::theme::Space::None));
    view_ = new SceneViewWidget();
    layout->addWidget(view_);
    host_->resize(480, 360);
    host_->show();
    QApplication::processEvents();
    if (!haveGl(*view_)) {
      PJ_SKIP_WITHOUT_GPU("No usable OpenGL context (headless without xvfb/GL).");
    }
    if (liveGlVersion(*view_) < std::pair<int, int>(4, 5)) {
      PJ_SKIP_WITHOUT_GPU("GL context below 4.5 cannot compile the scene's #version 450 shaders");
    }
    tf_ = std::make_shared<pj::scene3d::TransformBuffer>(pj::scene3d::TransformBuffer::kKeepAll);
    ASSERT_TRUE(tf_->setTransform(
                       pj::scene3d::StampedTransform{
                           .stamp = PJ::fromRaw(0),
                           .parent_frame = "map",
                           .child_frame = "body",
                           .transform = pj::scene3d::Transform(glm::dvec3(kBoxCentre), glm::dquat{1.0, 0.0, 0.0, 0.0}),
                       })
                    .has_value());
    view_->setTransformBuffer(tf_);
    view_->setFixedFrame("map");
    view_->setTrackerTime(PJ::fromRaw(1'000'000));
    box_ = std::make_unique<HoveringBoxLayer>();
    view_->setLayers({box_.get()});
  }

  void TearDown() override {
    if (view_ != nullptr) {
      view_->setLayers({});
    }
    host_.reset();
  }

  // Looks at the box from `distance` along a 30 deg elevation, focal on the box.
  void lookAtBox(float distance) {
    CameraState s = view_->camera().state();
    s.focal = kBoxCentre;
    s.radius = distance;
    s.azimuth = 0.0f;
    s.elevation = glm::radians(30.0f);
    view_->camera().adoptState(s);
    view_->grabFramebuffer();
  }

  [[nodiscard]] QPointF centre() const {
    return {view_->width() / 2.0, view_->height() / 2.0};
  }

  // A pixel near the top-left corner, over empty sky (the box is at the centre).
  [[nodiscard]] QPointF emptyCorner() const {
    return {view_->width() * 0.1, view_->height() * 0.1};
  }

  void wheel(int ticks, std::optional<QPointF> at = std::nullopt) {
    const QPointF pos = at.value_or(centre());
    QWheelEvent event(
        pos, view_->mapToGlobal(pos), QPoint(), QPoint(0, 120 * ticks), Qt::NoButton, Qt::NoModifier,
        Qt::NoScrollPhase, false);
    QApplication::sendEvent(view_, &event);
    view_->grabFramebuffer();
  }

  void leftDrag(const QPointF& from, const QPointF& delta) {
    QMouseEvent press(
        QEvent::MouseButtonPress, from, view_->mapToGlobal(from), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view_, &press);
    const QPointF to = from + delta;
    QMouseEvent move(QEvent::MouseMove, to, view_->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view_, &move);
    QMouseEvent release(
        QEvent::MouseButtonRelease, to, view_->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view_, &release);
    view_->grabFramebuffer();
  }

  std::unique_ptr<QWidget> host_;
  std::shared_ptr<pj::scene3d::TransformBuffer> tf_;
  std::unique_ptr<HoveringBoxLayer> box_;
  SceneViewWidget* view_ = nullptr;
};

TEST_F(ScenePickGlTest, PicksTheHoveringBoxAtAnyDistance) {
  for (const float distance : {10.0f, 40.0f, 150.0f}) {
    lookAtBox(distance);
    const std::optional<glm::vec3> hit = view_->pickSurfaceAtForTest(centre());
    ASSERT_TRUE(hit.has_value()) << "no pick at distance " << distance;
    EXPECT_LT(glm::length(*hit - kBoxCentre), kBoxSize) << "distance " << distance << ": picked (" << hit->x << ", "
                                                        << hit->y << ", " << hit->z << ")";
  }
}

// The user's sequence: zoom out, zoom back in over the box. The camera must close in on the
// box (not fly past it towards the floor) and the box must stay pickable on the way.
TEST_F(ScenePickGlTest, WheelZoomOverTheBoxClosesInOnIt) {
  lookAtBox(40.0f);
  for (int k = 0; k < 10; ++k) {
    wheel(-1);  // out
  }
  for (int k = 0; k < 40; ++k) {
    wheel(1);  // back in
    const std::optional<glm::vec3> hit = view_->pickSurfaceAtForTest(centre());
    ASSERT_TRUE(hit.has_value()) << "box not under the cursor at tick " << k;
    ASSERT_LT(glm::length(*hit - kBoxCentre), kBoxSize) << "picked off the box at tick " << k;
  }
  EXPECT_LT(glm::length(view_->camera().position() - kBoxCentre), 3.0f);
}

// Following the box's frame, wheel and drag over empty space stay on the box: the zoom
// closes in on it and the orbit keeps it as the centre.
TEST_F(ScenePickGlTest, FollowedFrameStaysTheZoomAndOrbitCentre) {
  view_->setFollowFrame("body");
  lookAtBox(40.0f);
  for (int k = 0; k < 10; ++k) {
    wheel(-1, emptyCorner());
  }
  for (int k = 0; k < 40; ++k) {
    wheel(1, emptyCorner());
  }
  EXPECT_LT(glm::length(view_->camera().state().focal - kBoxCentre), 1e-3f);
  EXPECT_LT(view_->camera().state().radius, 40.0f);

  leftDrag(emptyCorner(), QPointF(60.0, 30.0));
  EXPECT_LT(glm::length(view_->camera().state().focal - kBoxCentre), 1e-3f);
  const std::optional<glm::vec3> hit = view_->pickSurfaceAtForTest(centre());
  ASSERT_TRUE(hit.has_value()) << "box left the centre of the view";
  EXPECT_LT(glm::length(*hit - kBoxCentre), kBoxSize);
}

// Without a followed frame the wheel over empty space keeps zooming towards the cursor ray,
// which moves the orbit centre off the box.
TEST_F(ScenePickGlTest, FreeCameraZoomsTowardsTheCursor) {
  lookAtBox(40.0f);
  wheel(3, emptyCorner());
  EXPECT_GT(glm::length(view_->camera().state().focal - kBoxCentre), 0.5f);
}

}  // namespace

int main(int argc, char** argv) {
  QSurfaceFormat fmt;
  fmt.setVersion(4, 5);
  fmt.setProfile(QSurfaceFormat::CoreProfile);
  fmt.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(fmt);

  QApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
