// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QColor>
#include <QString>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pj_scene3d_widgets/mesh_data.h"

namespace pj::scene3d {

// View-level visibility/opacity applies only to robot visual and collision
// groups. Independent model draws (SceneEntities ModelPrimitive) retain their
// layer's own presentation controls.
enum class WasmModelDrawGroup : std::uint8_t { kIndependent, kVisual, kCollision };

struct WasmModelDrawCall {
  std::string mesh_key;
  std::string frame_id;
  glm::dmat4 model{1.0};
  glm::vec4 override_color{1.0F};
  bool use_material = true;
  // Shade with a plain default material (no textures or glTF PBR/alpha), as for a
  // SceneEntities ModelPrimitive whose override_color replaces the model's
  // materials. Mirrors MeshRenderPass::DrawCall::replace_materials.
  bool replace_materials = false;
  WasmModelDrawGroup group = WasmModelDrawGroup::kIndependent;
};

// Non-owning QRhi input seam shared by browser layers that submit imported or
// procedural MeshData. Implementations retain CPU meshes until modelRevision()
// changes; the view owns and rebuilds the corresponding GPU resources.
class WasmModelRenderable {
 public:
  virtual ~WasmModelRenderable() = default;

  [[nodiscard]] virtual const std::vector<WasmModelDrawCall>& modelDrawCalls() const = 0;
  [[nodiscard]] virtual const std::unordered_map<std::string, std::shared_ptr<const MeshData>>& modelMeshes() const = 0;
  [[nodiscard]] virtual std::uint64_t modelRevision() const = 0;
  [[nodiscard]] virtual bool visible() const = 0;
  [[nodiscard]] virtual float opacity() const = 0;
  [[nodiscard]] virtual bool colorOverrideEnabled() const = 0;
  [[nodiscard]] virtual QColor overrideColor() const = 0;
  [[nodiscard]] virtual bool wireframe() const = 0;
  [[nodiscard]] virtual const std::string& sourceFrame() const = 0;
  [[nodiscard]] virtual bool contributesToSceneBounds() const = 0;
  virtual void noteRenderFailure(QString warning) = 0;
  virtual void noteRenderSuccess() = 0;
};

}  // namespace pj::scene3d
