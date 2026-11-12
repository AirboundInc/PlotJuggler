// SPDX-License-Identifier: MPL-2.0

#include "airframe_model.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>

namespace airbound::airframe
{
namespace
{

using nlohmann::json;

// 4x4 affine transform, row-major.
using Mat4 = std::array<double, 16>;

constexpr Mat4 kIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
constexpr int kMaxDepth = 64;  // deeper than any real model; guards malformed (cyclic) files

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

Vec3 transform_point(const Mat4& m, const Vec3& p)
{
  Vec3 out{};
  for (int r = 0; r < 3; ++r)
  {
    out[r] = m[r * 4] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3];
  }
  return out;
}

Vec3 transform_direction(const Mat4& m, const Vec3& v)
{
  Vec3 out{};
  for (int r = 0; r < 3; ++r)
  {
    out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2];
  }
  return out;
}

double dot(const Vec3& a, const Vec3& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// A node's local transform, with and without its own scale. For a node given as a matrix both
// are that matrix (its scale cannot be separated).
struct NodeTransform
{
  Mat4 local = kIdentity;        // T * R * S
  Mat4 without_scale = kIdentity;  // T * R
};

NodeTransform node_transform(const json& node)
{
  NodeTransform out;
  if (node.contains("matrix") && node["matrix"].is_array() && node["matrix"].size() == 16)
  {
    for (int r = 0; r < 4; ++r)
    {
      for (int c = 0; c < 4; ++c)
      {
        out.local[r * 4 + c] = node["matrix"][c * 4 + r].get<double>();  // glTF stores column-major
      }
    }
    out.without_scale = out.local;
    return out;
  }
  const auto t = node.value("translation", std::vector<double>{0.0, 0.0, 0.0});
  const auto q = node.value("rotation", std::vector<double>{0.0, 0.0, 0.0, 1.0});
  const auto s = node.value("scale", std::vector<double>{1.0, 1.0, 1.0});
  if (t.size() != 3 || q.size() != 4 || s.size() != 3)
  {
    return out;
  }
  const double x = q[0], y = q[1], z = q[2], w = q[3];
  const double rot[3][3] = {
      {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
      {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
      {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
  };
  for (int i = 0; i < 3; ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      out.local[i * 4 + j] = rot[i][j] * s[j];
      out.without_scale[i * 4 + j] = rot[i][j];
    }
    out.local[i * 4 + 3] = t[i];
    out.without_scale[i * 4 + 3] = t[i];
  }
  return out;
}

json column_major(const Mat4& m)
{
  json out = json::array();
  for (int c = 0; c < 4; ++c)
  {
    for (int r = 0; r < 4; ++r)
    {
      out.push_back(m[r * 4 + c]);
    }
  }
  return out;
}

// A parsed GLB: the JSON document and the BIN chunk (empty when absent).
struct Glb
{
  json doc;
  std::vector<uint8_t> bin;
};

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t offset)
{
  uint32_t v = 0;
  std::memcpy(&v, bytes.data() + offset, sizeof(v));
  return v;
}

std::optional<Glb> parse_glb(const std::vector<uint8_t>& bytes)
{
  constexpr uint32_t kMagicGltf = 0x46546C67;  // "glTF"
  constexpr uint32_t kChunkJson = 0x4E4F534A;  // "JSON"
  constexpr uint32_t kChunkBin = 0x004E4942;   // "BIN\0"
  if (bytes.size() < 12 || read_u32(bytes, 0) != kMagicGltf)
  {
    return std::nullopt;
  }
  const size_t total = std::min<size_t>(read_u32(bytes, 8), bytes.size());
  Glb glb;
  bool has_json = false;
  for (size_t offset = 12; offset + 8 <= total;)
  {
    const size_t length = read_u32(bytes, offset);
    const uint32_t type = read_u32(bytes, offset + 4);
    const size_t begin = offset + 8;
    if (length > total - begin)
    {
      return std::nullopt;
    }
    if (type == kChunkJson && !has_json)
    {
      glb.doc = json::parse(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                            bytes.begin() + static_cast<std::ptrdiff_t>(begin + length), nullptr, false);
      if (glb.doc.is_discarded() || !glb.doc.is_object())
      {
        return std::nullopt;
      }
      has_json = true;
    }
    else if (type == kChunkBin && glb.bin.empty())
    {
      glb.bin.assign(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                     bytes.begin() + static_cast<std::ptrdiff_t>(begin + length));
    }
    offset = begin + length;
  }
  if (!has_json)
  {
    return std::nullopt;
  }
  return glb;
}

const json& array_or_empty(const json& doc, const char* key)
{
  static const json kEmpty = json::array();
  const auto it = doc.find(key);
  return it != doc.end() && it->is_array() ? *it : kEmpty;
}

std::vector<size_t> scene_roots(const json& doc)
{
  std::vector<size_t> roots;
  const json& scenes = array_or_empty(doc, "scenes");
  const size_t scene = doc.value("scene", 0);
  if (scene < scenes.size())
  {
    for (const auto& root : array_or_empty(scenes[scene], "nodes"))
    {
      if (root.is_number_unsigned())
      {
        roots.push_back(root.get<size_t>());
      }
    }
  }
  return roots;
}

// Visits every node reachable from the scene roots with its parent index (nullopt for a root)
// and its model-space matrix. Stops at a malformed index or past kMaxDepth.
void walk_nodes(const json& doc,
                const std::function<void(size_t node, std::optional<size_t> parent, const Mat4& world)>& visit)
{
  const json& nodes = array_or_empty(doc, "nodes");
  std::function<void(size_t, std::optional<size_t>, const Mat4&, int)> walk =
      [&](size_t index, std::optional<size_t> parent, const Mat4& parent_world, int depth) {
        if (index >= nodes.size() || depth > kMaxDepth)
        {
          return;
        }
        const Mat4 world = multiply(parent_world, node_transform(nodes[index]).local);
        visit(index, parent, world);
        for (const auto& child : array_or_empty(nodes[index], "children"))
        {
          if (child.is_number_unsigned())
          {
            walk(child.get<size_t>(), index, world, depth + 1);
          }
        }
      };
  for (size_t root : scene_roots(doc))
  {
    walk(root, std::nullopt, kIdentity, 0);
  }
}

// Builds a GLB holding only the given roots' subtrees, minus the `cut` nodes' subtrees, with the
// meshes, materials, textures, images, samplers, accessors and buffer bytes they use. A root
// with a `wrapper` matrix hangs under a new node carrying it.
class SubsetWriter
{
public:
  explicit SubsetWriter(const Glb& source) : src_(source.doc), bin_src_(source.bin) {}

  struct Root
  {
    size_t node;
    std::optional<Mat4> wrapper;
  };

  std::optional<std::vector<uint8_t>> write(const std::vector<Root>& roots, const std::set<size_t>& cut,
                                            std::string* error)
  {
    json scene_nodes = json::array();
    for (const Root& root : roots)
    {
      const auto node = add_node(root.node, cut, 0);
      if (!node)
      {
        continue;
      }
      if (root.wrapper)
      {
        nodes_.push_back({{"name", "rest"}, {"matrix", column_major(*root.wrapper)}, {"children", {*node}}});
        scene_nodes.push_back(nodes_.size() - 1);
      }
      else
      {
        scene_nodes.push_back(*node);
      }
    }
    if (!error_.empty())
    {
      if (error != nullptr)
      {
        *error = error_;
      }
      return std::nullopt;
    }

    json out = {{"asset", src_.value("asset", json{{"version", "2.0"}})},
                {"scene", 0},
                {"scenes", json::array({{{"nodes", scene_nodes}}})},
                {"nodes", nodes_}};
    const auto put = [&out](const char* key, const json& items) {
      if (!items.empty())
      {
        out[key] = items;
      }
    };
    put("meshes", meshes_);
    put("accessors", accessors_);
    put("materials", materials_);
    put("textures", textures_);
    put("images", images_);
    put("samplers", samplers_);
    for (const char* key : {"extensionsUsed", "extensionsRequired"})
    {
      if (src_.contains(key))
      {
        out[key] = src_[key];
      }
    }
    while (bin_.size() % 4 != 0)
    {
      bin_.push_back(0);
    }
    if (!views_.empty())
    {
      out["bufferViews"] = views_;
      out["buffers"] = json::array({{{"byteLength", bin_.size()}}});
    }
    return encode(out.dump(), bin_);
  }

private:
  // Remaps one index of the source's `key` array, copying the item on first use.
  template <typename Copy>
  std::optional<size_t> remap(std::map<size_t, size_t>& table, const char* key, const json& index, Copy copy)
  {
    if (!index.is_number_unsigned())
    {
      fail(std::string("bad ") + key + " index");
      return std::nullopt;
    }
    const size_t from = index.get<size_t>();
    if (const auto it = table.find(from); it != table.end())
    {
      return it->second;
    }
    const json& items = array_or_empty(src_, key);
    if (from >= items.size())
    {
      fail(std::string(key) + " index out of range");
      return std::nullopt;
    }
    const size_t to = copy(items[from]);
    table[from] = to;
    return to;
  }

  std::optional<size_t> add_node(size_t index, const std::set<size_t>& cut, int depth)
  {
    const json& nodes = array_or_empty(src_, "nodes");
    if (index >= nodes.size() || depth > kMaxDepth)
    {
      fail("bad node hierarchy");
      return std::nullopt;
    }
    json node = nodes[index];
    node.erase("skin");
    node.erase("camera");
    json children = json::array();
    for (const auto& child : array_or_empty(nodes[index], "children"))
    {
      if (child.is_number_unsigned() && cut.count(child.get<size_t>()) == 0)
      {
        if (const auto added = add_node(child.get<size_t>(), cut, depth + 1))
        {
          children.push_back(*added);
        }
      }
    }
    node.erase("children");
    if (!children.empty())
    {
      node["children"] = children;
    }
    if (node.contains("mesh"))
    {
      const auto mesh = remap(mesh_map_, "meshes", node["mesh"], [this](const json& m) { return copy_mesh(m); });
      if (mesh)
      {
        node["mesh"] = *mesh;
      }
      else
      {
        node.erase("mesh");
      }
    }
    nodes_.push_back(std::move(node));
    return nodes_.size() - 1;
  }

  size_t copy_mesh(json mesh)
  {
    for (auto& primitive : mesh["primitives"])
    {
      for (auto& [name, accessor] : primitive["attributes"].items())
      {
        accessor = remap_accessor(accessor).value_or(0);
      }
      if (primitive.contains("indices"))
      {
        primitive["indices"] = remap_accessor(primitive["indices"]).value_or(0);
      }
      if (primitive.contains("targets"))
      {
        for (auto& target : primitive["targets"])
        {
          for (auto& [name, accessor] : target.items())
          {
            accessor = remap_accessor(accessor).value_or(0);
          }
        }
      }
      if (primitive.contains("material"))
      {
        primitive["material"] =
            remap(material_map_, "materials", primitive["material"], [this](const json& m) {
              return copy_material(m);
            }).value_or(0);
      }
      if (primitive.contains("extensions"))
      {
        fail("mesh compression extensions are not supported");  // e.g. Draco: views inside extensions
      }
    }
    meshes_.push_back(std::move(mesh));
    return meshes_.size() - 1;
  }

  std::optional<size_t> remap_accessor(const json& index)
  {
    return remap(accessor_map_, "accessors", index, [this](json accessor) {
      if (accessor.contains("bufferView"))
      {
        accessor["bufferView"] = remap_view(accessor["bufferView"]).value_or(0);
      }
      if (accessor.contains("sparse"))
      {
        for (const char* part : {"indices", "values"})
        {
          auto& sparse = accessor["sparse"][part];
          sparse["bufferView"] = remap_view(sparse["bufferView"]).value_or(0);
        }
      }
      accessors_.push_back(std::move(accessor));
      return accessors_.size() - 1;
    });
  }

  std::optional<size_t> remap_view(const json& index)
  {
    return remap(view_map_, "bufferViews", index, [this](json view) {
      const size_t offset = view.value("byteOffset", size_t{0});
      const size_t length = view.value("byteLength", size_t{0});
      const bool in_bin =
          view.value("buffer", size_t{0}) == 0 && offset <= bin_src_.size() && length <= bin_src_.size() - offset;
      if (!in_bin)
      {
        fail("buffer views outside the GLB's binary chunk are not supported");
      }
      while (bin_.size() % 4 != 0)
      {
        bin_.push_back(0);
      }
      view["buffer"] = 0;
      view["byteOffset"] = bin_.size();
      if (in_bin)
      {
        bin_.insert(bin_.end(), bin_src_.begin() + static_cast<std::ptrdiff_t>(offset),
                    bin_src_.begin() + static_cast<std::ptrdiff_t>(offset + length));
      }
      views_.push_back(std::move(view));
      return views_.size() - 1;
    });
  }

  // Remaps every texture reference in a material: the "index" of any object under a key ending
  // in "Texture" (baseColorTexture, normalTexture, ... and the KHR_materials_* extensions').
  void remap_texture_refs(json& value)
  {
    if (!value.is_object())
    {
      return;
    }
    for (auto& [key, child] : value.items())
    {
      const bool texture_info = key.size() >= 7 && key.compare(key.size() - 7, 7, "Texture") == 0 &&
                                child.is_object() && child.contains("index");
      if (texture_info)
      {
        child["index"] = remap(texture_map_, "textures", child["index"], [this](const json& t) {
                           return copy_texture(t);
                         }).value_or(0);
      }
      remap_texture_refs(child);
    }
  }

  size_t copy_material(json material)
  {
    remap_texture_refs(material);
    materials_.push_back(std::move(material));
    return materials_.size() - 1;
  }

  size_t copy_texture(json texture)
  {
    const auto copy_image = [this](json image) {
      if (image.contains("bufferView"))
      {
        image["bufferView"] = remap_view(image["bufferView"]).value_or(0);
      }
      images_.push_back(std::move(image));
      return images_.size() - 1;
    };
    if (texture.contains("source"))
    {
      texture["source"] = remap(image_map_, "images", texture["source"], copy_image).value_or(0);
    }
    if (texture.contains("extensions"))
    {
      for (auto& [name, extension] : texture["extensions"].items())  // KHR_texture_basisu, EXT_texture_webp
      {
        if (extension.is_object() && extension.contains("source"))
        {
          extension["source"] = remap(image_map_, "images", extension["source"], copy_image).value_or(0);
        }
      }
    }
    if (texture.contains("sampler"))
    {
      texture["sampler"] = remap(sampler_map_, "samplers", texture["sampler"], [this](json sampler) {
                             samplers_.push_back(std::move(sampler));
                             return samplers_.size() - 1;
                           }).value_or(0);
    }
    textures_.push_back(std::move(texture));
    return textures_.size() - 1;
  }

  void fail(const std::string& message)
  {
    if (error_.empty())
    {
      error_ = message;
    }
  }

  static std::vector<uint8_t> encode(std::string json_text, const std::vector<uint8_t>& bin)
  {
    while (json_text.size() % 4 != 0)
    {
      json_text.push_back(' ');
    }
    std::vector<uint8_t> glb;
    const auto put32 = [&glb](size_t v) {
      for (int b = 0; b < 4; ++b)
      {
        glb.push_back(static_cast<uint8_t>(v >> (8 * b)));
      }
    };
    const size_t total = 12 + 8 + json_text.size() + (bin.empty() ? 0 : 8 + bin.size());
    put32(0x46546C67u);  // "glTF"
    put32(2);
    put32(total);
    put32(json_text.size());
    put32(0x4E4F534Au);  // "JSON"
    glb.insert(glb.end(), json_text.begin(), json_text.end());
    if (!bin.empty())
    {
      put32(bin.size());
      put32(0x004E4942u);  // "BIN\0"
      glb.insert(glb.end(), bin.begin(), bin.end());
    }
    return glb;
  }

  const json& src_;
  const std::vector<uint8_t>& bin_src_;
  json nodes_ = json::array();
  json meshes_ = json::array();
  json accessors_ = json::array();
  json views_ = json::array();
  json materials_ = json::array();
  json textures_ = json::array();
  json images_ = json::array();
  json samplers_ = json::array();
  std::vector<uint8_t> bin_;
  std::map<size_t, size_t> mesh_map_, accessor_map_, view_map_, material_map_, texture_map_, image_map_, sampler_map_;
  std::string error_;
};

// True when the 3x3 linear part is a rotation (possibly mirrored) times one uniform scale, the
// only case where turning about a local axis is a rigid turn in model space.
bool is_uniform_similarity(const Mat4& m)
{
  const Vec3 c[3] = {{m[0], m[4], m[8]}, {m[1], m[5], m[9]}, {m[2], m[6], m[10]}};
  const double n0 = std::sqrt(dot(c[0], c[0]));
  if (!(n0 > 0.0))
  {
    return false;
  }
  constexpr double kTolerance = 1e-4;
  for (int i = 0; i < 3; ++i)
  {
    const double ni = std::sqrt(dot(c[i], c[i]));
    if (std::abs(ni / n0 - 1.0) > kTolerance)
    {
      return false;
    }
    for (int j = i + 1; j < 3; ++j)
    {
      if (std::abs(dot(c[i], c[j])) > kTolerance * n0 * n0)
      {
        return false;
      }
    }
  }
  return true;
}

double determinant3(const Mat4& m)
{
  return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

}  // namespace

std::optional<Bounds> glb_bounds(const std::vector<uint8_t>& glb_bytes)
{
  const std::optional<Glb> glb = parse_glb(glb_bytes);
  if (!glb)
  {
    return std::nullopt;
  }
  const json& meshes = array_or_empty(glb->doc, "meshes");
  const json& accessors = array_or_empty(glb->doc, "accessors");
  const json& nodes = array_or_empty(glb->doc, "nodes");
  Vec3 lo = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::infinity()};
  Vec3 hi = {-lo[0], -lo[1], -lo[2]};
  walk_nodes(glb->doc, [&](size_t index, std::optional<size_t> /*parent*/, const Mat4& world) {
    const json& node = nodes[index];
    if (!node.contains("mesh") || !node["mesh"].is_number_unsigned() || node["mesh"].get<size_t>() >= meshes.size())
    {
      return;
    }
    for (const auto& primitive : array_or_empty(meshes[node["mesh"].get<size_t>()], "primitives"))
    {
      if (!primitive.contains("attributes") || !primitive["attributes"].contains("POSITION"))
      {
        continue;
      }
      const size_t accessor_index = primitive["attributes"]["POSITION"].get<size_t>();
      if (accessor_index >= accessors.size())
      {
        continue;
      }
      const json& accessor = accessors[accessor_index];
      if (!accessor.contains("min") || !accessor.contains("max") || accessor["min"].size() != 3 ||
          accessor["max"].size() != 3)
      {
        continue;
      }
      for (int corner = 0; corner < 8; ++corner)
      {
        Vec3 p{};
        for (int k = 0; k < 3; ++k)
        {
          p[k] = ((corner >> k) & 1) != 0 ? accessor["max"][k].get<double>() : accessor["min"][k].get<double>();
        }
        const Vec3 v = transform_point(world, p);
        for (int r = 0; r < 3; ++r)
        {
          lo[r] = std::min(lo[r], v[r]);
          hi[r] = std::max(hi[r], v[r]);
        }
      }
    }
  });
  if (!(lo[0] <= hi[0]))
  {
    return std::nullopt;  // no mesh with bounds
  }
  return Bounds{lo, hi, {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2},
                std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]})};
}

std::optional<Config> parse_config(std::string_view text, std::string* error)
{
  const auto fail = [error](const std::string& message) -> std::optional<Config> {
    if (error != nullptr)
    {
      *error = message;
    }
    return std::nullopt;
  };
  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object())
  {
    return fail("not a JSON object");
  }
  Config config;
  config.pwm_from_params = doc.value("pwmSource", std::string{}) == "auto";
  const auto number = [](const json& item, const char* key, double fallback) {
    const auto it = item.find(key);
    return it != item.end() && it->is_number() ? it->get<double>() : fallback;
  };
  for (const auto& item : array_or_empty(doc, "surfaces"))
  {
    if (!item.is_object() || !item.contains("channel") || !item["channel"].is_number_integer() ||
        !item.contains("nodeName") || !item["nodeName"].is_string())
    {
      return fail("a surface without a channel or nodeName");
    }
    Surface surface;
    surface.channel = item["channel"].get<int>();
    surface.node_name = item["nodeName"].get<std::string>();
    const std::string axis = item.value("hingeLocalAxis", std::string("x"));
    if (axis != "x" && axis != "y" && axis != "z")
    {
      return fail("surface " + surface.node_name + ": hingeLocalAxis must be x, y or z");
    }
    surface.hinge_axis = axis[0] - 'x';
    surface.pwm_min = number(item, "pwmMin", surface.pwm_min);
    surface.pwm_neutral = number(item, "pwmNeutral", surface.pwm_neutral);
    surface.pwm_max = number(item, "pwmMax", surface.pwm_max);
    surface.angle_at_min = number(item, "angleAtMin", 0.0);
    surface.angle_at_max = number(item, "angleAtMax", 0.0);
    surface.reverse = item.value("reverse", false);
    config.surfaces.push_back(std::move(surface));
  }
  return config;
}

bool is_valid(const PwmRange& range)
{
  return std::isfinite(range.min) && std::isfinite(range.trim) && std::isfinite(range.max) && range.min < range.max &&
         range.trim >= range.min && range.trim <= range.max;
}

double pwm_to_angle(double pwm, const PwmRange& range, const Surface& surface)
{
  if (!std::isfinite(pwm))
  {
    return 0.0;
  }
  const double clamped = std::clamp(pwm, range.min, range.max);
  double angle = 0.0;
  if (clamped >= range.trim)
  {
    const double span = range.max - range.trim;
    angle = span > 0.0 ? (clamped - range.trim) / span * surface.angle_at_max : 0.0;
  }
  else
  {
    const double span = range.trim - range.min;
    angle = span > 0.0 ? (range.trim - clamped) / span * surface.angle_at_min : 0.0;
  }
  if (!std::isfinite(angle))
  {
    return 0.0;
  }
  return surface.reverse ? -angle : angle;
}

std::optional<Split> split(const std::vector<uint8_t>& glb_bytes, const Config& config, std::string* error)
{
  const std::optional<Glb> glb = parse_glb(glb_bytes);
  if (!glb)
  {
    if (error != nullptr)
    {
      *error = "not a GLB file";
    }
    return std::nullopt;
  }
  const json& nodes = array_or_empty(glb->doc, "nodes");

  // Parent and model-space matrices of every node in the scene.
  std::map<size_t, std::optional<size_t>> parent_of;
  std::map<size_t, Mat4> world_of;
  std::map<size_t, int> depth_of;
  walk_nodes(glb->doc, [&](size_t index, std::optional<size_t> parent, const Mat4& world) {
    parent_of[index] = parent;
    world_of[index] = world;
    depth_of[index] = parent ? depth_of[*parent] + 1 : 0;
  });

  Split result;
  struct Candidate
  {
    size_t surface;
    size_t node;
  };
  std::vector<Candidate> moving;
  std::set<size_t> moving_nodes;
  for (size_t s = 0; s < config.surfaces.size(); ++s)
  {
    const Surface& surface = config.surfaces[s];
    std::optional<size_t> found;
    for (size_t n = 0; n < nodes.size(); ++n)
    {
      if (world_of.count(n) != 0 && nodes[n].value("name", std::string{}) == surface.node_name)
      {
        found = n;
        break;
      }
    }
    if (!found)
    {
      result.warnings.push_back(surface.node_name + ": no such node in the model");
      continue;
    }
    if (moving_nodes.count(*found) != 0)
    {
      result.warnings.push_back(surface.node_name + ": configured twice, the first entry is used");
      continue;
    }
    if (nodes[*found].contains("matrix"))
    {
      result.warnings.push_back(surface.node_name + ": node has a matrix instead of translation/rotation/scale");
      continue;
    }
    const std::optional<size_t> parent = parent_of[*found];
    const Mat4 hinge_frame = multiply(parent ? world_of[*parent] : kIdentity, node_transform(nodes[*found]).without_scale);
    if (!is_uniform_similarity(hinge_frame))
    {
      result.warnings.push_back(surface.node_name + ": non-uniform scale above the node, left in place");
      continue;
    }
    moving.push_back({s, *found});
    moving_nodes.insert(*found);
  }

  // Parents before children, so a part's parent index is always already assigned.
  std::stable_sort(moving.begin(), moving.end(),
                   [&](const Candidate& a, const Candidate& b) { return depth_of[a.node] < depth_of[b.node]; });

  std::map<size_t, size_t> part_of_node;
  for (const Candidate& candidate : moving)
  {
    const Surface& surface = config.surfaces[candidate.surface];
    const std::optional<size_t> parent = parent_of[candidate.node];
    const Mat4 parent_world = parent ? world_of[*parent] : kIdentity;
    const Mat4 hinge_frame = multiply(parent_world, node_transform(nodes[candidate.node]).without_scale);

    Part part;
    part.surface = candidate.surface;
    for (std::optional<size_t> up = parent; up; up = parent_of[*up])
    {
      if (const auto it = part_of_node.find(*up); it != part_of_node.end())
      {
        part.parent_part = it->second;
        break;
      }
    }
    part.pivot = transform_point(hinge_frame, {0.0, 0.0, 0.0});
    Vec3 local_axis{0.0, 0.0, 0.0};
    local_axis[static_cast<size_t>(surface.hinge_axis)] = 1.0;
    part.axis = transform_direction(hinge_frame, local_axis);
    const double length = std::sqrt(dot(part.axis, part.axis));
    for (double& v : part.axis)
    {
      v /= length;
    }
    part.angle_sign = determinant3(hinge_frame) < 0.0 ? -1.0 : 1.0;

    std::set<size_t> cut = moving_nodes;
    cut.erase(candidate.node);
    std::string subset_error;
    SubsetWriter writer(*glb);
    auto bytes = writer.write({{candidate.node, parent ? std::optional<Mat4>(parent_world) : std::nullopt}}, cut,
                              &subset_error);
    if (!bytes)
    {
      if (error != nullptr)
      {
        *error = surface.node_name + ": " + subset_error;
      }
      return std::nullopt;
    }
    part.glb = std::move(*bytes);
    part_of_node[candidate.node] = result.parts.size();
    result.parts.push_back(std::move(part));
  }

  std::vector<SubsetWriter::Root> body_roots;
  for (size_t root : scene_roots(glb->doc))
  {
    if (moving_nodes.count(root) == 0)
    {
      body_roots.push_back({root, std::nullopt});
    }
  }
  std::string subset_error;
  SubsetWriter writer(*glb);
  auto body = writer.write(body_roots, moving_nodes, &subset_error);
  if (!body)
  {
    if (error != nullptr)
    {
      *error = "body: " + subset_error;
    }
    return std::nullopt;
  }
  result.body = std::move(*body);
  return result;
}

}  // namespace airbound::airframe
