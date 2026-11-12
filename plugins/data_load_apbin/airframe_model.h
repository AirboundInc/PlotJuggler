// SPDX-License-Identifier: MPL-2.0
//
// Airframe models for the 3D view: GLB bounds, the moving-part (control surface) configuration
// shared with the Airbound web log viewer, and splitting a GLB into a static body plus one GLB
// per moving part so each part can be posed by its own TF frame.
//
// The 3D view bakes a GLB's node hierarchy into one rigid mesh, so a part can only move on its
// own as a separate model. All geometry here is in the GLB's own model space (glTF, Y-up).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace airbound::airframe
{

using Vec3 = std::array<double, 3>;

// Axis-aligned bounds of a GLB's meshes in model space (node transforms applied).
struct Bounds
{
  Vec3 min;
  Vec3 max;
  Vec3 centre;
  double max_extent;  // the box's largest side
};

// Bounds from the POSITION accessors' min/max; nullopt for a file that is not a GLB with meshes.
std::optional<Bounds> glb_bounds(const std::vector<uint8_t>& glb);

// One servo-driven part, as in the web viewer's model config ("surfaces" entries).
struct Surface
{
  int channel = 0;        // servo output number (SERVOn, RCOU/RCO2 Cn)
  std::string node_name;  // glTF node the part hangs from; it turns about its own origin
  int hinge_axis = 0;     // the node's local axis (0 = x, 1 = y, 2 = z) it turns about
  double pwm_min = 1000.0;
  double pwm_neutral = 1500.0;
  double pwm_max = 2000.0;
  double angle_at_min = 0.0;  // radians at pwm_min (and at pwm_max below), 0 at neutral
  double angle_at_max = 0.0;
  bool reverse = false;  // negates the angle
};

struct Config
{
  // "auto": the PWM range comes from the log's SERVOn_MIN/TRIM/MAX; otherwise ("manual", or a
  // config without the field) from the surface's own pwm_* values.
  bool pwm_from_params = false;
  std::vector<Surface> surfaces;
};

// Parses the web viewer's config JSON. Its "motors" are ignored (propellers do not spin).
// nullopt with `error` set on malformed JSON or a surface without a channel or node name.
std::optional<Config> parse_config(std::string_view json, std::string* error);

struct PwmRange
{
  double min;
  double trim;
  double max;
};

// ArduPilot's SERVOn_MIN/TRIM/MAX defaults, used when the log's values are missing or invalid.
constexpr PwmRange kDefaultPwmRange{1100.0, 1500.0, 1900.0};

// True when min < max and trim lies within them.
bool is_valid(const PwmRange& range);

// The web viewer's mapping: linear from trim (angle 0) to max (angle_at_max) and from trim to
// min (angle_at_min), with the PWM clamped to the range. 0 for a non-finite PWM.
double pwm_to_angle(double pwm, const PwmRange& range, const Surface& surface);

// One moving part cut out of the model.
struct Part
{
  size_t surface = 0;                  // index into Config::surfaces
  std::optional<size_t> parent_part;   // the nearest moving ancestor, an index into the parts
  Vec3 pivot{};                        // the hinge point in model space, at rest
  Vec3 axis{};                         // the hinge direction in model space (unit), at rest
  // +1, or -1 when the node's frame is mirrored (negative determinant): turning by `angle` about
  // the node's local axis is turning by angle_sign * angle about `axis`.
  double angle_sign = 1.0;
  std::vector<uint8_t> glb;  // the part and its non-moving descendants, at their rest place
};

struct Split
{
  std::vector<uint8_t> body;  // the model without the moving parts
  std::vector<Part> parts;    // parents before their children
  // Surfaces left on the body, with the reason (node missing, non-uniform scale above it, ...).
  std::vector<std::string> warnings;
};

// Cuts every configured surface's node (with its descendants, minus nested moving parts) out of
// `glb`. Each part GLB and the body keep only the meshes, materials, textures and buffer bytes
// they use, and draw in the same model space as the whole file did, so the pieces assemble into
// the original model at rest. nullopt with `error` set when `glb` is not a usable GLB.
std::optional<Split> split(const std::vector<uint8_t>& glb, const Config& config, std::string* error);

}  // namespace airbound::airframe
