// SPDX-License-Identifier: MPL-2.0
//
// Splitting a GLB into a body and moving parts, and the web viewer's PWM -> angle mapping.
// The models are synthetic: a root with a moving part A (with its own non-uniform scale) that
// carries a nested moving part B (with its own non-uniform scale), plus a static part sharing
// A's textured mesh.

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

#include "airframe_model.h"

namespace
{

using airbound::airframe::Config;
using airbound::airframe::Part;
using airbound::airframe::PwmRange;
using airbound::airframe::Surface;
using airbound::airframe::Vec3;
using nlohmann::json;

using Mat4 = std::array<double, 16>;  // row-major

constexpr double kPi = std::numbers::pi;

Mat4 multiply(const Mat4& a, const Mat4& b)
{
  Mat4 out{};
  for (int r = 0; r < 4; ++r)
  {
    for (int c = 0; c < 4; ++c)
    {
      for (int k = 0; k < 4; ++k)
      {
        out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
      }
    }
  }
  return out;
}

Vec3 apply(const Mat4& m, const Vec3& p)
{
  Vec3 out{};
  for (int r = 0; r < 3; ++r)
  {
    out[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
  }
  return out;
}

std::array<double, 4> axis_quaternion(int axis, double angle)  // glTF order x, y, z, w
{
  std::array<double, 4> q{0.0, 0.0, 0.0, std::cos(angle / 2)};
  q[static_cast<size_t>(axis)] = std::sin(angle / 2);
  return q;
}

Mat4 trs(const Vec3& t, const std::array<double, 4>& q, const Vec3& s)
{
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double r[3][3] = {
      {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
      {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
      {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
  };
  Mat4 m{};
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      m[i * 4 + j] = r[i][j] * s[j];
    }
    m[i * 4 + 3] = t[i];
  }
  m[15] = 1.0;
  return m;
}

Mat4 rotation(int axis, double angle)
{
  return trs({0, 0, 0}, axis_quaternion(axis, angle), {1, 1, 1});
}

// Rodrigues: turns p by `angle` about the line through `pivot` along unit `axis`.
Vec3 turn(const Vec3& p, const Vec3& pivot, const Vec3& axis, double angle)
{
  const Vec3 v = {p[0] - pivot[0], p[1] - pivot[1], p[2] - pivot[2]};
  const Vec3 k_x_v = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
  const double k_dot_v = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
  Vec3 out{};
  for (int i = 0; i < 3; ++i)
  {
    out[i] = pivot[i] + v[i] * std::cos(angle) + k_x_v[i] * std::sin(angle) + axis[i] * k_dot_v * (1 - std::cos(angle));
  }
  return out;
}

struct NodeSpec
{
  Vec3 t;
  std::array<double, 4> q;
  Vec3 s;
  Mat4 local() const { return trs(t, q, s); }
};

// The synthetic model. root -> {A -> B, Static}; A and Static share mesh 0 (textured).
struct Model
{
  NodeSpec root{{1.0, 2.0, 3.0}, axis_quaternion(1, 0.5), {1.0, 1.0, 1.0}};
  NodeSpec a{{0.5, 0.0, 0.0}, axis_quaternion(2, 0.35), {1.5, 1.5, 1.5}};
  NodeSpec b{{0.0, 1.0, 0.0}, axis_quaternion(0, 0.2), {1.0, 2.0, 1.0}};
  NodeSpec fixed{{-1.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}, {1.0, 1.0, 1.0}};

  static json node(const char* name, const NodeSpec& spec, const json& extra)
  {
    json out = {{"name", name},
                {"translation", {spec.t[0], spec.t[1], spec.t[2]}},
                {"rotation", {spec.q[0], spec.q[1], spec.q[2], spec.q[3]}},
                {"scale", {spec.s[0], spec.s[1], spec.s[2]}}};
    out.update(extra);
    return out;
  }

  std::vector<uint8_t> glb() const
  {
    std::vector<uint8_t> bin;
    const auto add = [&bin](const void* data, size_t bytes) {
      while (bin.size() % 4 != 0)
      {
        bin.push_back(0);
      }
      const size_t offset = bin.size();
      const auto* p = static_cast<const uint8_t*>(data);
      bin.insert(bin.end(), p, p + bytes);
      return json{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", bytes}};
    };
    const float tri0[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const uint16_t idx0[3] = {0, 1, 2};
    const uint8_t png[16] = {0x89, 'P', 'N', 'G', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // bytes only
    const float tri1[9] = {0, 0, 0, 0.5f, 0, 0, 0, 0, 0.5f};
    json views = json::array({add(tri0, sizeof(tri0)), add(idx0, sizeof(idx0)), add(png, sizeof(png)),
                              add(tri1, sizeof(tri1))});
    json doc = {
        {"asset", {{"version", "2.0"}}},
        {"scene", 0},
        {"scenes", json::array({{{"nodes", json::array({0})}}})},
        {"nodes", json::array({node("root", root, {{"children", {1, 3}}}),
                               node("~CS_A", a, {{"mesh", 0}, {"children", json::array({2})}}),
                               node("~CS_B", b, {{"mesh", 1}}), node("static", fixed, {{"mesh", 0}})})},
        {"meshes",
         json::array({{{"primitives", json::array({{{"attributes", {{"POSITION", 0}}}, {"indices", 1}, {"material", 0}}})}},
                      {{"primitives", json::array({{{"attributes", {{"POSITION", 2}}}}})}}})},
        {"accessors",
         {{{"bufferView", 0}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {1, 1, 0}}},
          {{"bufferView", 1}, {"componentType", 5123}, {"count", 3}, {"type", "SCALAR"}},
          {{"bufferView", 3}, {"componentType", 5126}, {"count", 3}, {"type", "VEC3"}, {"min", {0, 0, 0}}, {"max", {0.5, 0, 0.5}}}}},
        {"materials", json::array({{{"pbrMetallicRoughness", {{"baseColorTexture", {{"index", 0}}}}}}})},
        {"textures", json::array({{{"source", 0}, {"sampler", 0}}})},
        {"images", json::array({{{"bufferView", 2}, {"mimeType", "image/png"}}})},
        {"samplers", json::array({json::object()})},
        {"bufferViews", views},
    };
    while (bin.size() % 4 != 0)
    {
      bin.push_back(0);
    }
    doc["buffers"] = json::array({{{"byteLength", bin.size()}}});
    std::string text = doc.dump();
    while (text.size() % 4 != 0)
    {
      text.push_back(' ');
    }
    std::vector<uint8_t> out;
    const auto put32 = [&out](size_t v) {
      for (int k = 0; k < 4; ++k)
      {
        out.push_back(static_cast<uint8_t>(v >> (8 * k)));
      }
    };
    put32(0x46546C67u);
    put32(2);
    put32(12 + 8 + text.size() + 8 + bin.size());
    put32(text.size());
    put32(0x4E4F534Au);
    out.insert(out.end(), text.begin(), text.end());
    put32(bin.size());
    put32(0x004E4942u);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
  }
};

Config two_surfaces()
{
  Config config;
  config.surfaces.push_back({.channel = 5, .node_name = "~CS_A", .hinge_axis = 0});
  config.surfaces.push_back({.channel = 6, .node_name = "~CS_B", .hinge_axis = 1});
  return config;
}

json glb_json(const std::vector<uint8_t>& glb)
{
  uint32_t length = 0;
  std::memcpy(&length, glb.data() + 12, sizeof(length));
  return json::parse(glb.begin() + 20, glb.begin() + 20 + length);
}

std::vector<std::string> node_names(const json& doc)
{
  std::vector<std::string> names;
  for (const auto& node : doc["nodes"])
  {
    names.push_back(node.value("name", std::string{}));
  }
  return names;
}

TEST(AirframeSplit, CutsEachSurfaceIntoItsOwnPartAndLeavesTheRestAsBody)
{
  Config config = two_surfaces();
  config.surfaces.push_back({.channel = 7, .node_name = "~CS_MISSING"});
  std::string error;
  const auto split = airbound::airframe::split(Model{}.glb(), config, &error);
  ASSERT_TRUE(split.has_value()) << error;

  ASSERT_EQ(split->parts.size(), 2u);
  EXPECT_EQ(split->parts[0].surface, 0u);
  EXPECT_FALSE(split->parts[0].parent_part.has_value());
  EXPECT_EQ(split->parts[1].surface, 1u);
  ASSERT_TRUE(split->parts[1].parent_part.has_value());
  EXPECT_EQ(*split->parts[1].parent_part, 0u);
  ASSERT_EQ(split->warnings.size(), 1u);
  EXPECT_NE(split->warnings[0].find("~CS_MISSING"), std::string::npos);

  const json body = glb_json(split->body);
  EXPECT_EQ(node_names(body), (std::vector<std::string>{"static", "root"}));
  EXPECT_EQ(body["meshes"].size(), 1u);
  EXPECT_EQ(body["images"].size(), 1u) << "the static part keeps the shared textured mesh";

  const json part_a = glb_json(split->parts[0].glb);
  EXPECT_EQ(node_names(part_a), (std::vector<std::string>{"~CS_A", "rest"})) << "B is cut out of A";
  const json part_b = glb_json(split->parts[1].glb);
  EXPECT_EQ(node_names(part_b), (std::vector<std::string>{"~CS_B", "rest"}));
  EXPECT_FALSE(part_b.contains("images")) << "B's mesh has no material";
  EXPECT_LT(split->parts[1].glb.size(), split->body.size());
}

// Each piece draws where it was in the whole model.
TEST(AirframeSplit, PiecesKeepTheirRestPlace)
{
  const Model model;
  const auto split = airbound::airframe::split(model.glb(), two_surfaces(), nullptr);
  ASSERT_TRUE(split.has_value());
  const Mat4 world_b = multiply(multiply(model.root.local(), model.a.local()), model.b.local());
  const auto bounds_b = airbound::airframe::glb_bounds(split->parts[1].glb);
  ASSERT_TRUE(bounds_b.has_value());
  const Vec3 expected = apply(world_b, {0.25, 0.0, 0.25});  // centre of B's triangle bounds
  for (int k = 0; k < 3; ++k)
  {
    EXPECT_NEAR(bounds_b->centre[k], expected[k], 1e-9);
  }
}

// Turning the parts about their pivots/axes matches turning the nodes about their local axes, as
// the web viewer does (node rotation = rest * axis_angle), for nested parts and a mirrored root.
void expect_matches_node_rotation(const Model& model)
{
  const auto split = airbound::airframe::split(model.glb(), two_surfaces(), nullptr);
  ASSERT_TRUE(split.has_value());
  ASSERT_EQ(split->parts.size(), 2u);
  const Part& pa = split->parts[0];
  const Part& pb = split->parts[1];
  const double angle_a = 0.4;
  const double angle_b = -0.7;
  const Vec3 local = {0.3, -0.2, 0.6};

  const Mat4 local_a = trs(model.a.t, model.a.q, {1, 1, 1});
  const Mat4 local_b = trs(model.b.t, model.b.q, {1, 1, 1});
  const Mat4 scale_a = trs({0, 0, 0}, {0, 0, 0, 1}, model.a.s);
  const Mat4 scale_b = trs({0, 0, 0}, {0, 0, 0, 1}, model.b.s);
  const Mat4 node_turned = multiply(
      multiply(multiply(multiply(model.root.local(), local_a), rotation(0, angle_a)), scale_a),
      multiply(multiply(local_b, rotation(1, angle_b)), scale_b));
  const Vec3 expected = apply(node_turned, local);

  const Mat4 rest = multiply(multiply(model.root.local(), model.a.local()), model.b.local());
  Vec3 p = apply(rest, local);
  p = turn(p, pb.pivot, pb.axis, pb.angle_sign * angle_b);  // child first, at its rest hinge
  p = turn(p, pa.pivot, pa.axis, pa.angle_sign * angle_a);  // then carried by the parent
  for (int k = 0; k < 3; ++k)
  {
    EXPECT_NEAR(p[k], expected[k], 1e-9);
  }
}

TEST(AirframeSplit, PartMotionMatchesTurningTheNodes)
{
  expect_matches_node_rotation(Model{});
}

TEST(AirframeSplit, PartMotionMatchesTurningTheNodesUnderAMirroredRoot)
{
  Model model;
  model.root.s = {-1.0, 1.0, 1.0};
  const auto split = airbound::airframe::split(model.glb(), two_surfaces(), nullptr);
  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(split->parts[0].angle_sign, -1.0);
  expect_matches_node_rotation(model);
}

TEST(AirframeSplit, NonUniformScaleAboveASurfaceLeavesItOnTheBody)
{
  Model model;
  model.root.s = {2.0, 1.0, 1.0};
  const auto split = airbound::airframe::split(model.glb(), two_surfaces(), nullptr);
  ASSERT_TRUE(split.has_value());
  // A and B both sit under the stretched root, so neither can turn rigidly.
  EXPECT_TRUE(split->parts.empty());
  EXPECT_EQ(split->warnings.size(), 2u);
}

TEST(AirframeSplit, RejectsNonGlbInput)
{
  std::string error;
  EXPECT_FALSE(airbound::airframe::split({1, 2, 3}, two_surfaces(), &error).has_value());
  EXPECT_FALSE(error.empty());
}

// The real Airbound models are not in the repository: with AIRBOUND_MODELS_DIR set, every
// <model>.glb with a <model>.json there must split into one part per surface, without warnings,
// and the pieces must cover exactly the whole model.
TEST(AirframeSplit, RealModelsSplitCleanly)
{
  const char* dir = std::getenv("AIRBOUND_MODELS_DIR");
  if (dir == nullptr || *dir == '\0')
  {
    GTEST_SKIP() << "AIRBOUND_MODELS_DIR not set";
  }
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  };
  int checked = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir))
  {
    std::filesystem::path config_path = entry.path();
    if (entry.path().extension() != ".glb" || !std::filesystem::exists(config_path.replace_extension(".json")))
    {
      continue;
    }
    SCOPED_TRACE(entry.path().filename().string());
    const std::vector<uint8_t> glb = read(entry.path());
    const std::vector<uint8_t> config_bytes = read(config_path);
    std::string error;
    const auto config =
        airbound::airframe::parse_config(std::string_view(reinterpret_cast<const char*>(config_bytes.data()), config_bytes.size()), &error);
    ASSERT_TRUE(config.has_value()) << error;
    const auto split = airbound::airframe::split(glb, *config, &error);
    ASSERT_TRUE(split.has_value()) << error;
    EXPECT_TRUE(split->warnings.empty()) << split->warnings.front();
    EXPECT_EQ(split->parts.size(), config->surfaces.size());

    const auto full = airbound::airframe::glb_bounds(glb);
    ASSERT_TRUE(full.has_value());
    Vec3 lo = {1e30, 1e30, 1e30};
    Vec3 hi = {-1e30, -1e30, -1e30};
    std::vector<const std::vector<uint8_t>*> pieces = {&split->body};
    for (const Part& part : split->parts)
    {
      pieces.push_back(&part.glb);
    }
    for (const auto* piece : pieces)
    {
      const auto bounds = airbound::airframe::glb_bounds(*piece);
      ASSERT_TRUE(bounds.has_value());
      for (int k = 0; k < 3; ++k)
      {
        lo[k] = std::min(lo[k], bounds->min[k]);
        hi[k] = std::max(hi[k], bounds->max[k]);
      }
    }
    for (int k = 0; k < 3; ++k)
    {
      EXPECT_NEAR(lo[k], full->min[k], 1e-6);
      EXPECT_NEAR(hi[k], full->max[k], 1e-6);
    }
    ++checked;
  }
  EXPECT_GT(checked, 0) << "no <model>.glb with a <model>.json in " << dir;
}

TEST(AirframePwm, MapsAroundTrimLikeTheWebViewer)
{
  Surface surface;
  surface.angle_at_min = 0.5;
  surface.angle_at_max = -0.5;
  const PwmRange range{1000.0, 1500.0, 2000.0};
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(1500.0, range, surface), 0.0);
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(2000.0, range, surface), -0.5);
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(1000.0, range, surface), 0.5);
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(1750.0, range, surface), -0.25);
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(2500.0, range, surface), -0.5) << "clamped";
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(std::nan(""), range, surface), 0.0);
  surface.reverse = true;
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(2000.0, range, surface), 0.5);
  // Trim at max: nothing above trim, the angle there is 0.
  EXPECT_DOUBLE_EQ(airbound::airframe::pwm_to_angle(2000.0, {1000.0, 2000.0, 2000.0}, surface), 0.0);
  EXPECT_FALSE(airbound::airframe::is_valid({1500.0, 1000.0, 2000.0}));
  EXPECT_TRUE(airbound::airframe::is_valid(airbound::airframe::kDefaultPwmRange));
}

TEST(AirframeConfig, ParsesTheWebViewerFormat)
{
  const char* text = R"({
    "pwmSource": "auto",
    "motors": [{"axis": "z", "channel": 11, "nodeName": "~CS_ENGINE_R", "direction": -1}],
    "surfaces": [{"pwmMax": 2000, "pwmMin": 1000, "channel": 5, "reverse": true, "nodeName": "~CS_ELEVON_R",
                  "angleAtMax": -0.5235987755982988, "angleAtMin": 0.5235987755982988, "pwmNeutral": 1500,
                  "hingeLocalAxis": "y"}]
  })";
  std::string error;
  const auto config = airbound::airframe::parse_config(text, &error);
  ASSERT_TRUE(config.has_value()) << error;
  EXPECT_TRUE(config->pwm_from_params);
  ASSERT_EQ(config->surfaces.size(), 1u);
  const Surface& s = config->surfaces[0];
  EXPECT_EQ(s.channel, 5);
  EXPECT_EQ(s.node_name, "~CS_ELEVON_R");
  EXPECT_EQ(s.hinge_axis, 1);
  EXPECT_TRUE(s.reverse);
  EXPECT_NEAR(s.angle_at_min, kPi / 6, 1e-12);
  EXPECT_DOUBLE_EQ(s.pwm_neutral, 1500.0);

  const auto manual = airbound::airframe::parse_config(R"({"surfaces": []})", &error);
  ASSERT_TRUE(manual.has_value());
  EXPECT_FALSE(manual->pwm_from_params) << "configs without pwmSource are manual";

  EXPECT_FALSE(airbound::airframe::parse_config(R"({"surfaces": [{"channel": 5}]})", &error).has_value());
  EXPECT_FALSE(airbound::airframe::parse_config(R"({"surfaces": [{"channel": 5, "nodeName": "n", "hingeLocalAxis": "w"}]})",
                                                &error)
                   .has_value());
  EXPECT_FALSE(airbound::airframe::parse_config("not json", &error).has_value());
}

}  // namespace
