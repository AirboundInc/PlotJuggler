// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// A data source or toolbox may push SceneEntities as serialized canonical bytes
// with no parser bound (builtin_object_type metadata only). Dropping such a topic
// on a fresh 3D view must create a SceneEntities layer, exactly as a parser-backed
// marker topic does.

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>
#include <vector>

#include "pj_base/builtin/builtin_object.hpp"
#include "pj_base/builtin/frame_transforms_codec.hpp"
#include "pj_base/builtin/scene_entities_codec.hpp"
#include "pj_datastore/object_store.hpp"
#include "pj_runtime/SessionManager.h"
#include "pj_scene3d_widgets/Scene3DDockWidget.h"
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

}  // namespace
