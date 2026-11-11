// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#include "pj_scene3d_widgets/passes/trail_render_pass.h"

#include <fmt/core.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "pj_scene3d_widgets/gl/gl_functions.h"

namespace pj::scene3d {
namespace {

// Screen-space ribbon: each polyline point is expanded (CPU-side, at upload)
// into two vertices carrying the point's tangent and a +/-1 side flag; the
// vertex shader offsets them by half the line width in PIXELS, perpendicular
// to the tangent's screen projection. Thickness must come from geometry: core
// GL guarantees only 1 px lines, and glLineWidth(2) raises GL_INVALID_VALUE on
// strict core contexts.
constexpr std::string_view kVertSrc = R"(#version 450 core
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_dir;   // polyline tangent at this point (render space)
layout(location = 2) in float in_side; // ribbon side: +1 / -1
uniform mat4 u_mvp;
uniform vec2 u_viewport_px;
uniform float u_half_width_px;
void main() {
  vec4 clip = u_mvp * vec4(in_pos, 1.0);
  vec4 clip_ahead = u_mvp * vec4(in_pos + in_dir, 1.0);
  vec2 ndc0 = clip.xy / max(abs(clip.w), 1e-6);
  vec2 ndc1 = clip_ahead.xy / max(abs(clip_ahead.w), 1e-6);
  vec2 screen_dir = (ndc1 - ndc0) * u_viewport_px;
  float len = length(screen_dir);
  vec2 normal = len > 1e-6 ? vec2(-screen_dir.y, screen_dir.x) / len : vec2(0.0);
  clip.xy += normal * in_side * u_half_width_px * (2.0 * clip.w / u_viewport_px);
  gl_Position = clip;
}
)";

// u_color is a display (sRGB) colour; the scene buffer is linear light and the present
// re-encodes it, so linearise here like the other passes (else trails show much lighter).
constexpr std::string_view kFragSrc = R"(#version 450 core
uniform vec3 u_color;
out vec4 frag_color;
void main() {
  frag_color = vec4(pow(max(u_color, vec3(0.0)), vec3(2.2)), 1.0);
}
)";

// 3 pos + 3 dir + 1 side floats per expanded vertex, 2 vertices per point.
constexpr std::size_t kFloatsPerVertex = 7;

glm::dvec3 safeNormalize(const glm::dvec3& vector) {
  const double len = glm::length(vector);
  return len > 1e-12 ? vector / len : glm::dvec3(0.0);
}

}  // namespace

void TrailRenderPass::initializeGL() {
  // The view calls this on every layer every paintGL, so the guard every sibling
  // pass carries is what keeps the shader from being recompiled per frame;
  // releaseGL() clears the flag so a recreated context still rebuilds.
  if (initialized_) {
    return;
  }
  auto result = gl::Program::fromSources(kVertSrc, kFragSrc);
  if (auto* program = std::get_if<gl::Program>(&result); program != nullptr) {
    program_ = std::make_unique<gl::Program>(std::move(*program));
  } else {
    fmt::print(stderr, "TrailRenderPass shader error: {}\n", std::get<std::string>(result));
    program_.reset();
    return;
  }
  uploaded_valid_ = false;  // fresh context -> the VBO must be refilled
  initialized_ = true;
}

void TrailRenderPass::setPoints(std::vector<glm::dvec3> points) {
  points_ = std::move(points);
  ++revision_;
}

void TrailRenderPass::uploadIfNeeded(const glm::dvec3& render_origin) {
  if (uploaded_valid_ && uploaded_revision_ == revision_ && uploaded_origin_ == render_origin) {
    return;
  }
  const std::size_t total = points_.size();
  // Joint tangent: mean of the adjacent segment directions (endpoints take the
  // lone segment) — keeps the ribbon continuous through smooth turns. A trail
  // is a trajectory, so sample-resolution hairpins are rare and only pinch the
  // ribbon locally when they happen.
  const auto tangent_at = [this, total](std::size_t index) {
    glm::dvec3 direction(0.0);
    if (index + 1 < total) {
      direction += safeNormalize(points_[index + 1] - points_[index]);
    }
    if (index > 0) {
      direction += safeNormalize(points_[index] - points_[index - 1]);
    }
    const glm::dvec3 unit = safeNormalize(direction);
    return unit == glm::dvec3(0.0) ? glm::dvec3(1.0, 0.0, 0.0) : unit;
  };
  upload_scratch_.clear();
  upload_scratch_.reserve(total * 2 * kFloatsPerVertex);
  for (std::size_t index = 0; index < total; ++index) {
    const glm::vec3 position(points_[index] - render_origin);  // double subtract, THEN downcast
    const glm::vec3 direction(tangent_at(index));
    for (const float side : {1.0F, -1.0F}) {
      upload_scratch_.push_back(position.x);
      upload_scratch_.push_back(position.y);
      upload_scratch_.push_back(position.z);
      upload_scratch_.push_back(direction.x);
      upload_scratch_.push_back(direction.y);
      upload_scratch_.push_back(direction.z);
      upload_scratch_.push_back(side);
    }
  }
  vao_.bind();
  vbo_.uploadStatic(
      GL_ARRAY_BUFFER, upload_scratch_.data(), static_cast<GLsizeiptr>(sizeof(float) * upload_scratch_.size()));
  withGlFunctions([](auto& functions) {
    const auto stride = static_cast<GLsizei>(sizeof(float) * kFloatsPerVertex);
    functions.glEnableVertexAttribArray(0U);
    functions.glVertexAttribPointer(0U, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    functions.glEnableVertexAttribArray(1U);
    functions.glVertexAttribPointer(1U, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 3));
    functions.glEnableVertexAttribArray(2U);
    functions.glVertexAttribPointer(2U, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(sizeof(float) * 6));
  });
  vao_.unbind();
  uploaded_revision_ = revision_;
  uploaded_origin_ = render_origin;
  uploaded_valid_ = true;
}

void TrailRenderPass::render(
    const ViewParams& view_params, const glm::dvec3& render_origin, std::size_t past_count, const glm::vec3& past_color,
    const glm::vec3& future_color, float width_px, bool draw_past, bool draw_future) {
  if (!initialized_ || program_ == nullptr || points_.size() < 2 || (!draw_past && !draw_future)) {
    return;
  }
  // The scene renders into the off-screen chain at render_scale x device px, so
  // pixel-space params scale with it (same rule as EDL's neighbour radius).
  const float device_width = view_params.device_width_px > 0 ? static_cast<float>(view_params.device_width_px)
                                                             : static_cast<float>(view_params.viewport_width_px);
  const float device_height = view_params.device_height_px > 0 ? static_cast<float>(view_params.device_height_px)
                                                               : static_cast<float>(view_params.viewport_height_px);
  if (device_width <= 0.0F || device_height <= 0.0F) {
    return;  // no viewport yet (first frames while the dock lays out)
  }
  uploadIfNeeded(render_origin);

  const glm::mat4 mvp = view_params.proj * view_params.view;  // vertices already origin-relative
  program_->use();
  program_->setMat4("u_mvp", mvp);
  program_->setVec2("u_viewport_px", glm::vec2(device_width, device_height) * view_params.render_scale);
  program_->setFloat("u_half_width_px", 0.5F * std::max(width_px, 1.0F) * view_params.render_scale);

  const std::size_t total = points_.size();
  const std::size_t past = std::min(past_count, total);
  vao_.bind();
  if (draw_past && past >= 2) {
    program_->setVec3("u_color", past_color);
    withGlFunctions(
        [past](auto& functions) { functions.glDrawArrays(GL_TRIANGLE_STRIP, 0, static_cast<GLsizei>(2 * past)); });
  }
  // The future ribbon starts at the last past point (shared pair -> continuous).
  const std::size_t future_first = past > 0 ? past - 1 : 0;
  const std::size_t future_points = total - future_first;
  if (draw_future && future_points >= 2) {
    program_->setVec3("u_color", future_color);
    withGlFunctions([future_first, future_points](auto& functions) {
      functions.glDrawArrays(
          GL_TRIANGLE_STRIP, static_cast<GLint>(2 * future_first), static_cast<GLsizei>(2 * future_points));
    });
  }
  vao_.unbind();
  unuseProgram();
}

void TrailRenderPass::releaseGL() {
  program_.reset();
  vao_ = gl::VertexArray{};
  vbo_ = gl::Buffer{};
  initialized_ = false;
  uploaded_valid_ = false;
}

}  // namespace pj::scene3d
