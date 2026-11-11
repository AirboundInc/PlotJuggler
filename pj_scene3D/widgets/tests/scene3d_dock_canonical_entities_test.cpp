// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A data source or toolbox may push SceneEntities as serialized canonical bytes
// with no parser bound (builtin_object_type metadata only). Dropping such a topic
// on a fresh 3D view must create a SceneEntities layer, exactly as a parser-backed
// marker topic does.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <string>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
#include "pj_scene3d_widgets/scene_view_widget.h"
#include "pj_scene3d_widgets/transform_service.h"
using namespace Qt::StringLiterals;

namespace {

PJ::ObjectTopicId registerCanonicalEntitiesTopic(PJ::ObjectStore& store, PJ::Timestamp ts, PJ::DatasetId dataset = 1) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset;
  desc.topic_name = "_MODEL";
  desc.metadata_json = R"({"builtin_object_type":"kSceneEntities"})";
  const auto id = store.registerTopic(desc);
  EXPECT_TRUE(id.has_value());

  PJ::sdk::SceneEntity entity;
  entity.timestamp = ts;
  entity.frame_id = "vehicle";
  entity.id = "aircraft";
  entity.frame_locked = true;
  PJ::sdk::CubePrimitive cube;
  cube.size = {1.0, 1.0, 1.0};
  entity.cubes.push_back(cube);
  PJ::sdk::SceneEntities batch;
  batch.entities.push_back(entity);
  EXPECT_TRUE(store.pushOwned(id.value(), ts, PJ::serializeSceneEntities(batch)).has_value());
  return id.has_value() ? id.value() : PJ::ObjectTopicId{};
}

TEST(Scene3DDockCanonicalEntities, AcceptsParserlessSceneEntitiesTopic) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  const PJ::ObjectTopicId topic = registerCanonicalEntitiesTopic(session.objectStore(), 100);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);

  ASSERT_TRUE(dock.addTopic(topic, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));
  EXPECT_EQ(dock.layers().size(), 1U) << "a canonical SceneEntities topic must create a layer";
}

PJ::ObjectTopicId registerCanonicalTfTopic(PJ::ObjectStore& store, PJ::DatasetId dataset, PJ::Timestamp ts) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = dataset;
  desc.topic_name = "_TF";
  desc.metadata_json = R"({"builtin_object_type":"kFrameTransforms"})";
  const auto id = store.registerTopic(desc);
  EXPECT_TRUE(id.has_value());
  PJ::sdk::FrameTransforms transforms;
  PJ::sdk::FrameTransform edge;
  edge.timestamp = ts;
  edge.parent_frame_id = "world";
  edge.child_frame_id = "vehicle";
  transforms.transforms.push_back(edge);
  EXPECT_TRUE(store.pushOwned(id.value(), ts, PJ::serializeFrameTransforms(transforms)).has_value());
  return id.has_value() ? id.value() : PJ::ObjectTopicId{};
}

// A view whose first log is held only by a dropped transform (config) topic — which
// has no layer row the user could remove — must switch to a second log when one of
// its topics is added, instead of refusing it for the rest of the session.
TEST(Scene3DDockCanonicalEntities, SwitchesLogWhenOnlyTransformTopicsHoldTheOldOne) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId tf_a = registerCanonicalTfTopic(store, /*dataset=*/1, 100);
  const PJ::ObjectTopicId model_a = registerCanonicalEntitiesTopic(store, 100, /*dataset=*/1);
  const PJ::ObjectTopicId model_b = registerCanonicalEntitiesTopic(store, 200, /*dataset=*/2);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf_a, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"_TF"_s));
  ASSERT_TRUE(dock.addTopic(model_a, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));
  ASSERT_EQ(dock.boundDatasetIdForTest(), 1U);

  dock.removeTopic(model_a);
  ASSERT_TRUE(dock.addTopic(model_b, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s))
      << "only the old log's transform topic remained, so the view must switch logs";
  EXPECT_EQ(dock.boundDatasetIdForTest(), 2U);
  EXPECT_EQ(dock.layers().size(), 1U);
}

// After switching logs, the view must re-convert the current playhead against the new
// log's layers. The cached render time was clamped to the OLD log's layer range;
// replaying it would look the new log up at the old instant, where its frames do not
// exist (only the fixed root draws) until the slider moves.
TEST(Scene3DDockCanonicalEntities, SwitchingLogsReconvertsThePlayhead) {
  PJ::SessionManager session;  // "Use time offset" off: display seconds are absolute
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();
  constexpr PJ::Timestamp kLogBStart = 5'000'000'000;
  const PJ::ObjectTopicId tf_a = registerCanonicalTfTopic(store, /*dataset=*/1, 100);
  const PJ::ObjectTopicId model_a = registerCanonicalEntitiesTopic(store, 100, /*dataset=*/1);
  PJ::sdk::SceneEntities later;
  later.entities.resize(1);
  later.entities.front().id = "aircraft";
  later.entities.front().frame_id = "vehicle";
  ASSERT_TRUE(store.pushOwned(model_a, 200, PJ::serializeSceneEntities(later)).has_value());  // A spans [100, 200]
  registerCanonicalTfTopic(store, /*dataset=*/2, kLogBStart);
  const PJ::ObjectTopicId model_b = registerCanonicalEntitiesTopic(store, kLogBStart, /*dataset=*/2);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf_a, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"_TF"_s));
  ASSERT_TRUE(dock.addTopic(model_a, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));

  dock.onTrackerTime(5.0);  // the slider is in log B's span; log A's layer clamps it to 200
  ASSERT_EQ(dock.lastTrackerNsForTest(), std::optional<int64_t>(200));

  dock.removeTopic(model_a);
  ASSERT_TRUE(dock.addTopic(model_b, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));
  EXPECT_EQ(dock.lastTrackerNsForTest(), std::optional<int64_t>(kLogBStart))
      << "the playhead must be re-converted against log B, not replayed at log A's clamped instant";
}

// While the first log still has a render layer in the view, a second log's topic is
// still refused: its frames would resolve against the wrong transform tree.
TEST(Scene3DDockCanonicalEntities, RefusesAnotherLogWhileTheOldOneHasALayer) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId tf_a = registerCanonicalTfTopic(store, /*dataset=*/1, 100);
  const PJ::ObjectTopicId model_a = registerCanonicalEntitiesTopic(store, 100, /*dataset=*/1);
  const PJ::ObjectTopicId model_b = registerCanonicalEntitiesTopic(store, 200, /*dataset=*/2);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf_a, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"_TF"_s));
  ASSERT_TRUE(dock.addTopic(model_a, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));

  EXPECT_FALSE(dock.addTopic(model_b, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));
  EXPECT_EQ(dock.boundDatasetIdForTest(), 1U);
  EXPECT_EQ(dock.layers().size(), 1U);
}

// A TF topic moving `child` under world: at x = 10 from t = 100, x = 12 from t = 200.
PJ::ObjectTopicId registerMovingTfTopic(PJ::ObjectStore& store, const std::string& child) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "_TF";
  desc.metadata_json = R"({"builtin_object_type":"kFrameTransforms"})";
  const auto id = store.registerTopic(desc);
  EXPECT_TRUE(id.has_value());
  for (const auto& [ts, x] : {std::pair<PJ::Timestamp, double>{100, 10.0}, {200, 12.0}}) {
    PJ::sdk::FrameTransform edge;
    edge.timestamp = ts;
    edge.parent_frame_id = "world";
    edge.child_frame_id = child;
    edge.translation = {.x = x, .y = 0.0, .z = 0.0};
    edge.rotation = {.x = 0.0, .y = 0.0, .z = 0.0, .w = 1.0};
    PJ::sdk::FrameTransforms transforms;
    transforms.transforms.push_back(edge);
    EXPECT_TRUE(store.pushOwned(id.value(), ts, PJ::serializeFrameTransforms(transforms)).has_value());
  }
  return id.has_value() ? id.value() : PJ::ObjectTopicId{};
}

// A model topic whose one entity carries an inline STL triangle spanning 2 x 4 m in `frame`.
PJ::ObjectTopicId registerInlineModelTopic(PJ::ObjectStore& store, const std::string& frame, PJ::Timestamp ts) {
  PJ::ObjectTopicDescriptor desc;
  desc.dataset_id = 1;
  desc.topic_name = "_MODEL";
  desc.metadata_json = R"({"builtin_object_type":"kSceneEntities"})";
  const auto id = store.registerTopic(desc);
  EXPECT_TRUE(id.has_value());
  const std::string stl =
      "solid t\n"
      "facet normal 0 0 1\n"
      "outer loop\n"
      "vertex 0 0 0\n"
      "vertex 2 0 0\n"
      "vertex 0 4 0\n"
      "endloop\n"
      "endfacet\n"
      "endsolid t\n";
  PJ::sdk::ModelPrimitive model;
  model.scale = {.x = 1.0, .y = 1.0, .z = 1.0};
  model.pose.orientation = {.x = 0.0, .y = 0.0, .z = 0.0, .w = 1.0};
  model.media_type = "model/stl";
  model.data.assign(stl.begin(), stl.end());
  PJ::sdk::SceneEntity entity;
  entity.timestamp = ts;
  entity.frame_id = frame;
  entity.id = "aircraft";
  entity.models.push_back(model);
  PJ::sdk::SceneEntities batch;
  batch.entities.push_back(entity);
  EXPECT_TRUE(store.pushOwned(id.value(), ts, PJ::serializeSceneEntities(batch)).has_value());
  return id.has_value() ? id.value() : PJ::ObjectTopicId{};
}

template <typename Predicate>
bool pumpUntil(Predicate done, int timeout_ms = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (!done() && timer.elapsed() < timeout_ms) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return done();
}

// Dropping a model that rides a moving frame points the camera at it and follows that
// frame, even with the playhead before the frame's first pose.
TEST(Scene3DDockCanonicalEntities, DroppedModelFocusesAndFollowsItsFrame) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId tf = registerMovingTfTopic(store, "vehicle");
  const PJ::ObjectTopicId model = registerInlineModelTopic(store, "vehicle", 100);
  transform_service.ingestFrameTransformsForDataset(1);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"_TF"_s));
  ASSERT_EQ(dock.currentFixedFrame(), u"world"_s);
  ASSERT_TRUE(dock.addTopic(model, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));

  ASSERT_TRUE(pumpUntil([&] { return dock.currentFollowFrame() == u"vehicle"_s; }))
      << "the dropped model's frame was never followed";
  const pj::scene3d::CameraState state = dock.sceneView()->camera().state();
  // Centred on the triangle's box centre (1, 2, 0) in the vehicle frame, the vehicle at its
  // first pose (x = 10) since the playhead has not reached it.
  EXPECT_NEAR(state.focal.x, 11.0f, 1e-3f);
  EXPECT_NEAR(state.focal.y, 2.0f, 1e-3f);
  EXPECT_NEAR(state.focal.z, 0.0f, 1e-3f);
  EXPECT_NEAR(state.radius, 2.5f * std::sqrt(20.0f), 1e-3f);
}

// A model on the fixed frame (terrain, a static map) has nothing to follow: the camera
// and follow stay as they were.
TEST(Scene3DDockCanonicalEntities, DroppedStaticModelDoesNotFollow) {
  PJ::SessionManager session;
  pj::scene3d::TransformService transform_service(session);
  PJ::ObjectStore& store = session.objectStore();
  const PJ::ObjectTopicId tf = registerMovingTfTopic(store, "vehicle");
  const PJ::ObjectTopicId model = registerInlineModelTopic(store, "world", 100);
  transform_service.ingestFrameTransformsForDataset(1);

  PJ::Scene3DDockWidget dock;
  dock.setSessionManager(&session);
  dock.setTransformService(&transform_service);
  ASSERT_TRUE(dock.addTopic(tf, PJ::sdk::BuiltinObjectType::kFrameTransforms, u"_TF"_s));
  const pj::scene3d::CameraState before = dock.sceneView()->camera().state();
  ASSERT_TRUE(dock.addTopic(model, PJ::sdk::BuiltinObjectType::kSceneEntities, u"_MODEL"_s));
  pumpUntil([] { return false; }, 1000);  // let the mesh load land and any focus run
  EXPECT_TRUE(dock.currentFollowFrame().isEmpty());
  EXPECT_EQ(dock.sceneView()->camera().state().focal, before.focal);
}

}  // namespace
