// SPDX-License-Identifier: MPL-2.0
//
// Terrain for the 3D view: satellite imagery draped over an elevation model around a flight,
// built as one in-memory GLB the 3D view loads like any other model.
//
// Sources (the same as the Airbound web log viewer): ESRI World Imagery tiles (JPEG) and AWS
// Terrain Tiles in Terrarium encoding (PNG, metres above sea level). Tiles are downloaded with
// libcurl and cached on disk forever (imagery and elevation rarely change).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace airbound::terrain
{

// The local frame's origin: the EKF origin (ORGN), the point XKF1 positions are relative to.
struct GeoOrigin
{
  double lat_deg = 0.0;
  double lon_deg = 0.0;
  double alt_m = 0.0;  // above mean sea level
};

// An area in the local frame, metres east/north of the origin.
struct LocalBounds
{
  double min_east = 0.0;
  double max_east = 0.0;
  double min_north = 0.0;
  double max_north = 0.0;
};

struct Options
{
  std::filesystem::path cache_dir;  // tiles are read from and written to here
  bool allow_download = true;       // false: cached tiles only
};

struct Terrain
{
  // A GLB in glTF's Y-up axes chosen so that the 3D view's Y-up -> Z-up import lands it in the
  // local ENU frame (x east, y north, z up relative to the origin altitude): place it with an
  // identity pose on the frame the vehicle positions are in.
  std::vector<uint8_t> glb;
  int imagery_zoom = 0;
  int elevation_zoom = 0;
  int imagery_tiles = 0;
  int elevation_tiles = 0;
  int downloaded = 0;        // tiles fetched over the network this time (the rest came from the cache)
  int missing_imagery = 0;   // patches drawn untextured because their imagery tile is unavailable
  bool offline = false;      // the first download failed, so only cached tiles were used
};

// Builds the terrain covering `area` (the caller adds any margin). Blocks while tiles download.
// Returns nullopt with `error` set when no elevation is available (offline with nothing cached,
// or the area is unusable); missing imagery alone only leaves those patches untextured.
std::optional<Terrain> build_terrain(const GeoOrigin& origin, const LocalBounds& area, const Options& options,
                                     std::string& error);

// The default tile cache: AIRBOUND_TILE_CACHE_DIR, else %LOCALAPPDATA%\Airbound\PlotJuggler\tiles on
// Windows, $XDG_CACHE_HOME (or ~/.cache)/airbound/plotjuggler/tiles elsewhere.
std::filesystem::path default_cache_dir();

}  // namespace airbound::terrain
