// SPDX-License-Identifier: MPL-2.0
#include "terrain.h"

#ifdef _WIN32
// curl.h pulls in windows.h, whose min/max macros break std::min/std::max.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <curl/curl.h>
#include <png.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>
#include <thread>
#include <tuple>

namespace airbound::terrain
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
// ArduPilot's LATLON_TO_M (0.011131884502145034 m per 1e-7 degree) per degree: the scale the EKF
// uses for its local positions, so the terrain lands where XKF1 puts the vehicle.
constexpr double kMetresPerDegree = 111318.84502145034;
constexpr double kEquatorM = 40075016.686;
constexpr int kTileSize = 256;
constexpr int kMinZoom = 10;
constexpr int kMaxImageryZoom = 19;
constexpr int kMaxElevationZoom = 14;  // AWS Terrain Tiles are unreliable beyond this
constexpr int kTargetTilesAcross = 8;
constexpr size_t kMaxImageryTiles = 144;
constexpr int kPatchVertices = 17;  // per side of each imagery tile's height grid
constexpr size_t kDownloadThreads = 8;

struct TileSource
{
  const char* url;  // {z}/{x}/{y} placeholders
  const char* cache_subdir;
  const char* extension;
};
constexpr TileSource kImagery{
    "https://services.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}", "esri_imagery",
    ".jpg"};
constexpr TileSource kElevation{"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
                                "aws_terrarium", ".png"};

struct TileId
{
  int z = 0;
  int x = 0;
  int y = 0;
  bool operator<(const TileId& other) const
  {
    return std::tie(z, x, y) < std::tie(other.z, other.x, other.y);
  }
};

// Web Mercator, fractional tile coordinates (tile origin at the north-west corner of the world).
double lon_to_tile_x(double lon_deg, int z)
{
  return (lon_deg + 180.0) / 360.0 * std::ldexp(1.0, z);
}
double lat_to_tile_y(double lat_deg, int z)
{
  const double r = lat_deg * kDegToRad;
  return (1.0 - std::log(std::tan(r) + 1.0 / std::cos(r)) / kPi) / 2.0 * std::ldexp(1.0, z);
}
double tile_x_to_lon(double x, int z)
{
  return x / std::ldexp(1.0, z) * 360.0 - 180.0;
}
double tile_y_to_lat(double y, int z)
{
  return std::atan(std::sinh(kPi * (1.0 - 2.0 * y / std::ldexp(1.0, z)))) / kDegToRad;
}

struct TileRange
{
  int z = 0;
  int x_min = 0;
  int x_max = 0;
  int y_min = 0;
  int y_max = 0;
  size_t count() const
  {
    return static_cast<size_t>(x_max - x_min + 1) * static_cast<size_t>(y_max - y_min + 1);
  }
};

TileRange range_for(double lat_min, double lat_max, double lon_min, double lon_max, int z)
{
  return {.z = z,
          .x_min = static_cast<int>(std::floor(lon_to_tile_x(lon_min, z))),
          .x_max = static_cast<int>(std::floor(lon_to_tile_x(lon_max, z))),
          .y_min = static_cast<int>(std::floor(lat_to_tile_y(lat_max, z))),
          .y_max = static_cast<int>(std::floor(lat_to_tile_y(lat_min, z)))};
}

std::vector<TileId> tiles_of(const TileRange& range)
{
  std::vector<TileId> tiles;
  for (int y = range.y_min; y <= range.y_max; ++y)
  {
    for (int x = range.x_min; x <= range.x_max; ++x)
    {
      tiles.push_back({range.z, x, y});
    }
  }
  return tiles;
}

std::string tile_url(const TileSource& source, const TileId& tile)
{
  std::string url = source.url;
  const auto put = [&url](const char* key, int value) {
    const size_t at = url.find(key);
    if (at != std::string::npos)
    {
      url.replace(at, std::strlen(key), std::to_string(value));
    }
  };
  put("{z}", tile.z);
  put("{x}", tile.x);
  put("{y}", tile.y);
  return url;
}

std::optional<std::vector<uint8_t>> read_file(const std::filesystem::path& path)
{
  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    return std::nullopt;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.empty())
  {
    return std::nullopt;
  }
  return bytes;
}

// Written beside the target and renamed into place, so a concurrent reader (another PlotJuggler
// loading a log) never sees a half-written tile. Failures only cost the cache entry.
void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
{
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::filesystem::path partial = path;
  partial += ".part" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
  {
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    if (!file)
    {
      return;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
    {
      file.close();
      std::filesystem::remove(partial, ec);
      return;
    }
  }
  std::filesystem::rename(partial, path, ec);
  if (ec)
  {
    std::filesystem::remove(partial, ec);
  }
}

size_t append_body(char* data, size_t size, size_t count, void* user)
{
  auto* body = static_cast<std::vector<uint8_t>*>(user);
  body->insert(body->end(), data, data + size * count);
  return size * count;
}

struct HttpResult
{
  std::optional<std::vector<uint8_t>> body;  // set on HTTP 200 with content
  bool network_error = false;                // no answer at all (offline, DNS, timeout, TLS)
};

std::once_flag g_curl_init;

#ifndef _WIN32
// The CA bundle path compiled into libcurl is the build machine's, which may not exist on this
// distribution. SSL_CERT_FILE wins, then the first bundle found among the usual locations; empty
// leaves libcurl's built-in default.
const std::string& ca_bundle()
{
  static const std::string path = [] {
    if (const char* env = std::getenv("SSL_CERT_FILE"); env != nullptr && *env != '\0')
    {
      return std::string(env);
    }
    for (const char* candidate : {
             "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch
             "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL
             "/etc/ssl/ca-bundle.pem",              // openSUSE
             "/etc/ssl/cert.pem",                   // Alpine, others
         })
    {
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec))
      {
        return std::string(candidate);
      }
    }
    return std::string();
  }();
  return path;
}
#endif

HttpResult http_get(const std::string& url)
{
  std::call_once(g_curl_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  CURL* curl = curl_easy_init();
  if (curl == nullptr)
  {
    return {.network_error = true};
  }
  std::vector<uint8_t> body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);  // timeouts without SIGALRM: safe off the main thread
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "Airbound-PlotJuggler-APBin");
#ifdef _WIN32
  // OpenSSL has no CA bundle on Windows; trust the system certificate store instead.
  curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#else
  if (!ca_bundle().empty())
  {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle().c_str());
  }
#endif
  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (code != CURLE_OK)
  {
    return {.network_error = true};
  }
  if (status != 200 || body.empty())
  {
    return {};
  }
  return {.body = std::move(body)};
}

// Cache-first tile access. After a network error (or when downloads are off) only the cache is
// used, so an offline load costs one connect timeout, not one per tile.
class Fetcher
{
public:
  explicit Fetcher(const Options& options) : options_(options), offline_(!options.allow_download)
  {
  }

  std::optional<std::vector<uint8_t>> get(const TileSource& source, const TileId& tile)
  {
    const std::filesystem::path path = options_.cache_dir / source.cache_subdir / std::to_string(tile.z) /
                                       std::to_string(tile.x) / (std::to_string(tile.y) + source.extension);
    if (auto cached = read_file(path))
    {
      return cached;
    }
    if (offline_)
    {
      return std::nullopt;
    }
    HttpResult result = http_get(tile_url(source, tile));
    if (result.network_error)
    {
      network_failed_ = true;
      offline_ = true;
      return std::nullopt;
    }
    if (result.body)
    {
      write_file(path, *result.body);
      ++downloaded_;
    }
    return std::move(result.body);
  }

  // All of `tiles`, in order (nullopt where unavailable). The first is fetched alone so an
  // offline machine is detected before the parallel fetch starts.
  std::vector<std::optional<std::vector<uint8_t>>> get_all(const TileSource& source, const std::vector<TileId>& tiles)
  {
    std::vector<std::optional<std::vector<uint8_t>>> results(tiles.size());
    if (tiles.empty())
    {
      return results;
    }
    results[0] = get(source, tiles[0]);
    std::atomic<size_t> next{1};
    const auto worker = [&] {
      for (size_t k = next++; k < tiles.size(); k = next++)
      {
        results[k] = get(source, tiles[k]);
      }
    };
    std::vector<std::thread> threads;
    for (size_t t = 0; t < std::min(kDownloadThreads, tiles.size() - 1); ++t)
    {
      threads.emplace_back(worker);
    }
    for (std::thread& thread : threads)
    {
      thread.join();
    }
    return results;
  }

  int downloaded() const
  {
    return downloaded_;
  }
  bool network_failed() const
  {
    return network_failed_;
  }

private:
  Options options_;
  std::atomic<bool> offline_;
  std::atomic<bool> network_failed_{false};
  std::atomic<int> downloaded_{0};
};

// Terrarium: height = R * 256 + G + B / 256 - 32768 metres above sea level.
std::optional<std::vector<float>> decode_terrarium(const std::vector<uint8_t>& png)
{
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (png_image_begin_read_from_memory(&image, png.data(), png.size()) == 0)
  {
    return std::nullopt;
  }
  if (image.width != kTileSize || image.height != kTileSize)
  {
    png_image_free(&image);
    return std::nullopt;
  }
  image.format = PNG_FORMAT_RGB;
  std::vector<uint8_t> rgb(PNG_IMAGE_SIZE(image));
  if (png_image_finish_read(&image, nullptr, rgb.data(), 0, nullptr) == 0)
  {
    png_image_free(&image);
    return std::nullopt;
  }
  std::vector<float> heights(static_cast<size_t>(kTileSize) * kTileSize);
  for (size_t i = 0; i < heights.size(); ++i)
  {
    heights[i] =
        static_cast<float>(rgb[3 * i] * 256.0 + rgb[3 * i + 1] + rgb[3 * i + 2] / 256.0 - 32768.0);
  }
  return heights;
}

// Heights over a set of elevation tiles, sampled bilinearly between pixel centres.
class ElevationGrid
{
public:
  explicit ElevationGrid(int zoom) : zoom_(zoom)
  {
  }
  void add(const TileId& tile, std::vector<float> heights)
  {
    tiles_[{tile.x, tile.y}] = std::move(heights);
  }
  bool empty() const
  {
    return tiles_.empty();
  }

  std::optional<double> at(double lat_deg, double lon_deg) const
  {
    const double px = lon_to_tile_x(lon_deg, zoom_) * kTileSize - 0.5;
    const double py = lat_to_tile_y(lat_deg, zoom_) * kTileSize - 0.5;
    const auto x0 = static_cast<int64_t>(std::floor(px));
    const auto y0 = static_cast<int64_t>(std::floor(py));
    const double fx = px - static_cast<double>(x0);
    const double fy = py - static_cast<double>(y0);
    double sum = 0.0;
    double weight = 0.0;
    for (int dy = 0; dy <= 1; ++dy)
    {
      for (int dx = 0; dx <= 1; ++dx)
      {
        const std::optional<float> h = pixel(x0 + dx, y0 + dy);
        if (!h)
        {
          continue;
        }
        const double w = (dx == 1 ? fx : 1.0 - fx) * (dy == 1 ? fy : 1.0 - fy);
        sum += w * *h;
        weight += w;
      }
    }
    if (weight <= 1e-9)
    {
      return std::nullopt;
    }
    return sum / weight;  // renormalised where a neighbouring tile is missing
  }

private:
  std::optional<float> pixel(int64_t gx, int64_t gy) const
  {
    const auto it = tiles_.find({static_cast<int>(gx / kTileSize), static_cast<int>(gy / kTileSize)});
    if (it == tiles_.end() || gx < 0 || gy < 0)
    {
      return std::nullopt;
    }
    return it->second[static_cast<size_t>(gy % kTileSize) * kTileSize + static_cast<size_t>(gx % kTileSize)];
  }

  int zoom_;
  std::map<std::pair<int, int>, std::vector<float>> tiles_;
};

// Minimal glTF 2.0 binary writer: one buffer, accessors over 4-byte-aligned views.
class GlbWriter
{
public:
  int add_view(const void* data, size_t bytes, std::optional<int> target)
  {
    while (bin_.size() % 4 != 0)
    {
      bin_.push_back(0);
    }
    nlohmann::json view = {{"buffer", 0}, {"byteOffset", bin_.size()}, {"byteLength", bytes}};
    if (target)
    {
      view["target"] = *target;
    }
    const auto* begin = static_cast<const uint8_t*>(data);
    bin_.insert(bin_.end(), begin, begin + bytes);
    views_.push_back(std::move(view));
    return static_cast<int>(views_.size()) - 1;
  }

  int add_accessor(nlohmann::json accessor)
  {
    accessors_.push_back(std::move(accessor));
    return static_cast<int>(accessors_.size()) - 1;
  }

  std::vector<uint8_t> finish(nlohmann::json gltf)
  {
    while (bin_.size() % 4 != 0)
    {
      bin_.push_back(0);
    }
    gltf["asset"] = {{"version", "2.0"}, {"generator", "Airbound data_load_apbin terrain"}};
    gltf["buffers"] = nlohmann::json::array({{{"byteLength", bin_.size()}}});
    gltf["bufferViews"] = views_;
    gltf["accessors"] = accessors_;
    std::string json = gltf.dump();
    while (json.size() % 4 != 0)
    {
      json.push_back(' ');
    }
    std::vector<uint8_t> glb;
    const auto put32 = [&glb](uint32_t v) {
      for (int b = 0; b < 4; ++b)
      {
        glb.push_back(static_cast<uint8_t>(v >> (8 * b)));
      }
    };
    put32(0x46546C67u);  // "glTF"
    put32(2);
    put32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin_.size()));
    put32(static_cast<uint32_t>(json.size()));
    put32(0x4E4F534Au);  // "JSON"
    glb.insert(glb.end(), json.begin(), json.end());
    put32(static_cast<uint32_t>(bin_.size()));
    put32(0x004E4942u);  // "BIN\0"
    glb.insert(glb.end(), bin_.begin(), bin_.end());
    return glb;
  }

private:
  std::vector<uint8_t> bin_;
  nlohmann::json views_ = nlohmann::json::array();
  nlohmann::json accessors_ = nlohmann::json::array();
};

const char* image_mime_type(const std::vector<uint8_t>& bytes)
{
  if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF)
  {
    return "image/jpeg";
  }
  if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
  {
    return "image/png";
  }
  return nullptr;  // not an image (e.g. an error page): leave the patch untextured
}

}  // namespace

std::filesystem::path default_cache_dir()
{
  if (const char* env = std::getenv("AIRBOUND_TILE_CACHE_DIR"); env != nullptr && *env != '\0')
  {
    return env;
  }
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0')
  {
    return std::filesystem::path(local) / "Airbound" / "PlotJuggler" / "tiles";
  }
#else
  if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0')
  {
    return std::filesystem::path(xdg) / "airbound" / "plotjuggler" / "tiles";
  }
  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
  {
    return std::filesystem::path(home) / ".cache" / "airbound" / "plotjuggler" / "tiles";
  }
#endif
  std::error_code ec;
  return std::filesystem::temp_directory_path(ec) / "airbound_plotjuggler_tiles";
}

std::optional<Terrain> build_terrain(const GeoOrigin& origin, const LocalBounds& area, const Options& options,
                                     std::string& error)
{
  const double cos_lat0 = std::cos(origin.lat_deg * kDegToRad);
  if (!(area.max_east > area.min_east) || !(area.max_north > area.min_north) || !(cos_lat0 > 1e-3))
  {
    error = "the flight area is empty or too close to a pole";
    return std::nullopt;
  }
  const double lat_min = origin.lat_deg + area.min_north / kMetresPerDegree;
  const double lat_max = origin.lat_deg + area.max_north / kMetresPerDegree;
  const double lon_min = origin.lon_deg + area.min_east / (kMetresPerDegree * cos_lat0);
  const double lon_max = origin.lon_deg + area.max_east / (kMetresPerDegree * cos_lat0);

  // Imagery about kTargetTilesAcross tiles across the larger dimension, capped in total.
  const double extent = std::max(area.max_east - area.min_east, area.max_north - area.min_north);
  int imagery_zoom = static_cast<int>(std::lround(std::log2(kTargetTilesAcross * kEquatorM * cos_lat0 / extent)));
  imagery_zoom = std::clamp(imagery_zoom, kMinZoom, kMaxImageryZoom);
  TileRange imagery_range = range_for(lat_min, lat_max, lon_min, lon_max, imagery_zoom);
  while (imagery_zoom > kMinZoom && imagery_range.count() > kMaxImageryTiles)
  {
    --imagery_zoom;
    imagery_range = range_for(lat_min, lat_max, lon_min, lon_max, imagery_zoom);
  }
  if (imagery_range.count() > kMaxImageryTiles)
  {
    error = "the flight area is too large for terrain";
    return std::nullopt;
  }
  const int elevation_zoom = std::min(imagery_zoom, kMaxElevationZoom);
  const TileRange elevation_range = range_for(lat_min, lat_max, lon_min, lon_max, elevation_zoom);

  Fetcher fetcher(options);
  const std::vector<TileId> elevation_tiles = tiles_of(elevation_range);
  const auto elevation_bytes = fetcher.get_all(kElevation, elevation_tiles);
  ElevationGrid elevation(elevation_zoom);
  for (size_t k = 0; k < elevation_tiles.size(); ++k)
  {
    if (elevation_bytes[k])
    {
      if (auto heights = decode_terrarium(*elevation_bytes[k]))
      {
        elevation.add(elevation_tiles[k], std::move(*heights));
      }
    }
  }
  if (elevation.empty())
  {
    error = fetcher.network_failed() ? "no internet connection and no cached elevation tiles for this area"
                                     : "no elevation tiles available for this area";
    return std::nullopt;
  }
  const std::vector<TileId> imagery_tiles = tiles_of(imagery_range);
  const auto imagery_bytes = fetcher.get_all(kImagery, imagery_tiles);

  Terrain terrain;
  terrain.imagery_zoom = imagery_zoom;
  terrain.elevation_zoom = elevation_zoom;
  terrain.imagery_tiles = static_cast<int>(imagery_tiles.size());
  terrain.elevation_tiles = static_cast<int>(elevation_tiles.size());

  // One patch per imagery tile: a kPatchVertices^2 grid draped over the elevation, textured with
  // the tile as downloaded (no image decoding). glTF axes (east, up, -north): the 3D view's Y-up
  // to Z-up import, (x, y, z) -> (x, -z, y), turns them into ENU.
  constexpr int kN = kPatchVertices;
  std::vector<uint16_t> indices;
  for (int j = 0; j + 1 < kN; ++j)
  {
    for (int i = 0; i + 1 < kN; ++i)
    {
      const auto v = [](int ii, int jj) { return static_cast<uint16_t>(jj * kN + ii); };
      // Counter-clockwise seen from above (i runs east, j south).
      indices.insert(indices.end(), {v(i, j), v(i, j + 1), v(i + 1, j), v(i + 1, j), v(i, j + 1), v(i + 1, j + 1)});
    }
  }
  GlbWriter writer;
  const int index_view = writer.add_view(indices.data(), indices.size() * sizeof(uint16_t), 34963);
  const int index_accessor = writer.add_accessor(
      {{"bufferView", index_view}, {"componentType", 5123}, {"count", indices.size()}, {"type", "SCALAR"}});

  std::vector<std::array<float, 2>> uvs;
  for (int j = 0; j < kN; ++j)
  {
    for (int i = 0; i < kN; ++i)
    {
      uvs.push_back({static_cast<float>(i) / (kN - 1), static_cast<float>(j) / (kN - 1)});
    }
  }
  const int uv_view = writer.add_view(uvs.data(), uvs.size() * sizeof(uvs[0]), 34962);
  const int uv_accessor = writer.add_accessor(
      {{"bufferView", uv_view}, {"componentType", 5126}, {"count", uvs.size()}, {"type", "VEC2"}});

  nlohmann::json primitives = nlohmann::json::array();
  nlohmann::json materials = nlohmann::json::array();
  nlohmann::json images = nlohmann::json::array();
  nlohmann::json textures = nlohmann::json::array();
  // Untextured fallback for patches whose imagery is unavailable.
  materials.push_back({{"pbrMetallicRoughness",
                        {{"baseColorFactor", {0.45, 0.45, 0.42, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
                       {"doubleSided", true}});
  for (size_t k = 0; k < imagery_tiles.size(); ++k)
  {
    const TileId& tile = imagery_tiles[k];
    std::array<std::array<double, 3>, kN * kN> enu{};
    for (int j = 0; j < kN; ++j)
    {
      for (int i = 0; i < kN; ++i)
      {
        const double lon = tile_x_to_lon(tile.x + static_cast<double>(i) / (kN - 1), tile.z);
        const double lat = tile_y_to_lat(tile.y + static_cast<double>(j) / (kN - 1), tile.z);
        const double mean_cos = std::cos((lat + origin.lat_deg) / 2.0 * kDegToRad);
        const double height = elevation.at(lat, lon).value_or(origin.alt_m);
        enu[static_cast<size_t>(j * kN + i)] = {(lon - origin.lon_deg) * kMetresPerDegree * mean_cos,
                                                (lat - origin.lat_deg) * kMetresPerDegree, height - origin.alt_m};
      }
    }
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::array<float, 3> lo{1e30f, 1e30f, 1e30f};
    std::array<float, 3> hi{-1e30f, -1e30f, -1e30f};
    for (int j = 0; j < kN; ++j)
    {
      for (int i = 0; i < kN; ++i)
      {
        const auto& p = enu[static_cast<size_t>(j * kN + i)];
        const std::array<float, 3> gltf_pos{static_cast<float>(p[0]), static_cast<float>(p[2]),
                                            static_cast<float>(-p[1])};
        positions.push_back(gltf_pos);
        for (int c = 0; c < 3; ++c)
        {
          lo[c] = std::min(lo[c], gltf_pos[c]);
          hi[c] = std::max(hi[c], gltf_pos[c]);
        }
        // Normal from the grid's tangents (one-sided at the patch edges), in ENU.
        const auto& east_a = enu[static_cast<size_t>(j * kN + std::max(i - 1, 0))];
        const auto& east_b = enu[static_cast<size_t>(j * kN + std::min(i + 1, kN - 1))];
        const auto& south_a = enu[static_cast<size_t>(std::max(j - 1, 0) * kN + i)];
        const auto& south_b = enu[static_cast<size_t>(std::min(j + 1, kN - 1) * kN + i)];
        const std::array<double, 3> du{east_b[0] - east_a[0], east_b[1] - east_a[1], east_b[2] - east_a[2]};
        const std::array<double, 3> dv{south_a[0] - south_b[0], south_a[1] - south_b[1], south_a[2] - south_b[2]};
        std::array<double, 3> n{du[1] * dv[2] - du[2] * dv[1], du[2] * dv[0] - du[0] * dv[2],
                                du[0] * dv[1] - du[1] * dv[0]};  // east x north = up
        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12)
        {
          n = {n[0] / len, n[1] / len, n[2] / len};
        }
        else
        {
          n = {0.0, 0.0, 1.0};
        }
        normals.push_back({static_cast<float>(n[0]), static_cast<float>(n[2]), static_cast<float>(-n[1])});
      }
    }
    const int pos_view = writer.add_view(positions.data(), positions.size() * sizeof(positions[0]), 34962);
    const int pos_accessor = writer.add_accessor({{"bufferView", pos_view},
                                                  {"componentType", 5126},
                                                  {"count", positions.size()},
                                                  {"type", "VEC3"},
                                                  {"min", lo},
                                                  {"max", hi}});
    const int normal_view = writer.add_view(normals.data(), normals.size() * sizeof(normals[0]), 34962);
    const int normal_accessor = writer.add_accessor(
        {{"bufferView", normal_view}, {"componentType", 5126}, {"count", normals.size()}, {"type", "VEC3"}});

    int material = 0;
    const char* mime = imagery_bytes[k] ? image_mime_type(*imagery_bytes[k]) : nullptr;
    if (mime != nullptr)
    {
      const int image_view = writer.add_view(imagery_bytes[k]->data(), imagery_bytes[k]->size(), std::nullopt);
      images.push_back({{"bufferView", image_view}, {"mimeType", mime}});
      textures.push_back({{"source", images.size() - 1}, {"sampler", 0}});
      materials.push_back({{"pbrMetallicRoughness",
                            {{"baseColorTexture", {{"index", textures.size() - 1}}},
                             {"metallicFactor", 0.0},
                             {"roughnessFactor", 1.0}}},
                           {"doubleSided", true}});
      material = static_cast<int>(materials.size()) - 1;
    }
    else
    {
      ++terrain.missing_imagery;
    }
    primitives.push_back({{"attributes", {{"POSITION", pos_accessor}, {"NORMAL", normal_accessor}, {"TEXCOORD_0", uv_accessor}}},
                          {"indices", index_accessor},
                          {"material", material}});
  }

  nlohmann::json gltf;
  gltf["scene"] = 0;
  gltf["scenes"] = nlohmann::json::array({{{"nodes", {0}}}});
  gltf["nodes"] = nlohmann::json::array({{{"mesh", 0}, {"name", "terrain"}}});
  gltf["meshes"] = nlohmann::json::array({{{"primitives", primitives}, {"name", "terrain"}}});
  gltf["materials"] = materials;
  if (!images.empty())
  {
    gltf["images"] = images;
    gltf["textures"] = textures;
    // Linear filtering, clamped so neighbouring patches don't bleed into each other's edges.
    gltf["samplers"] = nlohmann::json::array({{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 33071}, {"wrapT", 33071}}});
  }
  terrain.glb = writer.finish(std::move(gltf));
  terrain.downloaded = fetcher.downloaded();
  terrain.offline = fetcher.network_failed();
  return terrain;
}

}  // namespace airbound::terrain
