# DataLoadAPBin: PJ3 → PJ4 porting map

Source of truth: `plotjuggler_plugins/DataLoadAPBin/` on `airbound-main` @ `45fc7211`
(line refs below are `dataload_apbin.cpp` unless noted). Target: PJ4 `FileSourceBase`
data source, built in-tree against the app's pinned `plotjuggler_sdk` (0.34.1).

Rules (from `pj-official-plugins/porting_guide.md`): mechanical translation, every code
path ported, no silent improvements. Suspected bugs in the original are **flagged**
here, not fixed, unless a decision says otherwise.

## Concept mapping

| PJ3 | PJ4 |
|---|---|
| `DataLoader::readDataFromFile(info, PlotDataMapRef&)` | `FileSourceBase::importData()`; file path arrives via `loadConfig({"filepath": ...})` |
| `plot_data.addNumeric("/GPS/#0/Lat")` → one flat series per field | `writeHost().ensureTopic("GPS/#0")` + one field per column (`Lat`, `Lng`, ...) |
| Values stored as `double` | Typed fields (`ensureField(topic, name, PrimitiveType)`), rows via `appendBoundRecord` |
| Time in seconds (`double`) | `PJ::Timestamp` in absolute **nanoseconds** (`int64`) |
| `QProgressDialog` + `wasCanceled()` | `runtimeHost().progressStart/progressUpdate`, `isStopRequested()` |
| `printf` / `qDebug` diagnostics | `runtimeHost().reportMessage(kInfo/kWarning, ...)` |
| Qt `.ui` dialog + `QSettings` | `PJ::DialogPluginTyped` + `.ui` embedded via `pj_embed_file`; **pre-import only** (see Decision 1) |
| Qt plugin metadata (`Q_PLUGIN_METADATA`) | `manifest.json` + `PJ_DATA_SOURCE_PLUGIN` / `PJ_DIALOG_PLUGIN` |
| Free-threaded plugin | Host calls must stay on the `importData()` thread; internal worker threads may decode into our own buffers only |

## Feature audit (2026-10-23: all rows DONE, verified against PJ3, see Verification)

| # | Original feature | Original location | PJ4 target | Status | Notes |
|---|---|---|---|---|---|
| 1 | `.bin` extension registration | `h`:64 (`"BIN"`) | `manifest.json` `file_extensions: [".bin"]` | DONE | Host lowercases the suffix before matching (`pj_app/src/FileLoader.cpp:344`), so `.BIN` works |
| 2 | Reset all state per load | 74–95 | Fresh members per `importData()` | DONE | PJ4 creates a new instance per load; keep the explicit reset anyway |
| 3 | Read whole file into memory | 97–108 | `std::ifstream` → `std::vector<uint8_t>`; error `Status` if unopenable | DONE | PJ3 returned `false` silently |
| 4 | Progress dialog, 5% steps, cancel aborts with no data | 110–162 | `progressStart(..., cancellable=true)`, `progressUpdate`, `isStopRequested()` | DONE | PJ3 cancel = discard everything. PJ4 hosts offer "Stop and Keep"; ULog commits partial data. Port as discard (faithful) unless told otherwise |
| 5 | Header sync: skip bytes until `A3 95` | 164–179 | same | DONE | |
| 6 | FMT parsing: id→name, labels→index | 185–281 | same | DONE | **Flag:** the "non-printable name" check (204–215) `continue`s the inner `for`, so a bad FMT is still stored; translate as-is |
| 7 | Skip zero-length formats; stop at truncated tail | 288–300 | same | DONE | |
| 8 | FMTU: units/multipliers; `#` marks instance field | 303–338 | same | DONE | |
| 9 | Defer data records seen before their FMTU; replay on FMTU / at EOF | 141–147, 340–351, 498–503, 519–531 | same | DONE | Fix from `82671a65` |
| 10 | MULT table (`Id`, `Mult` hard-typed `B`/`d`) | 365–390 | same | DONE | |
| 11 | UNIT table (`Id`, `Label`) | 393–418 | same | DONE | Only used by #22 |
| 12 | Drop ISBD / ISBH | 426–437 | same | DONE | |
| 13 | MSG → log message list | 438–461 | log messages list (+ see Decision 1) | DONE | **Bug:** hardcodes old `QZ` layout. ArduPilot now logs `QBBZ` (TimeUS,ID,Seq,Message) → text gets 2 garbage bytes. Upstream fix `ArduPilot/plotjuggler-apbin-plugins@3e2f925` locates fields by name. Decision 3 **Fixed in the upstream review** (fields located by name). |
| 14 | PARM → latest value per name + full time series | 462–492 | same | DONE | Hardcoded `QNf` prefix still valid for newer `QNff` `Default` (`QNff`) now shown in Dataset Info (upstream review). |
| 15 | Decode all 20 format types to double | 786–923 | typed `ValueRef` per format char | DONE | Unknown type: rest of the record is skipped with a stderr line, as in PJ3. **Flag:** for `a`/`n`/`N`/`Z` PJ3 pushes the previous field's value (string fields become a numeric series repeating their neighbour); translated as-is, written as `kFloat64` |
| 16 | Instance id: numeric, or full string for `N`/`n`/`Z` | 998–1052 | same | DONE | Fix from `d3eb917d` |
| 17 | Multipliers from FMTU×MULT, skip 0/1, warn on missing | 1087–1138 | same; field becomes `kFloat64` when a multiplier applies, native type otherwise | DONE | Values decoded to double exactly as in PJ3, then written back as the native type, so values and precision are identical to PJ3 (integers above 2^53 still round, as before) |
| 18 | GPS time sync to Unix time (GWk/GMS, sample `[1]`, −18 leap s), applied to all TimeUS and PARM | 1142–1217 | offset added in **ns** to every timestamp | DONE | **Flags:** (a) indexes sample `[1]` → out of bounds if GPS has <2 samples; (b) missing `GWk`/`GMS` silently map to field 0 via `operator[]`; (c) uses first GPS instance. Translate as-is, but (a) must not crash the host: guard and skip sync **(a), (b) and the GMS double scaling fixed in the upstream review**; (c) first instance kept, as upstream. |
| 19 | Publish only messages with `TimeUS`; skip time + instance columns | 660–727 | topics per message (per instance) | DONE | Naming → Decision 2 |
| 20 | PARM series `/_PARM/<name>` | 731–736 | topic `_PARM/<name>`, field `value` | DONE | |
| 21 | Load stats (read/skipped msgs, skipped bytes, elapsed ms) | 763–767 | `reportMessage(kInfo, ...)` | DONE | Was stdout only |
| 22 | `LABEL_WITH_UNIT` (compile-time, **off**) + `/x`→`x⁻¹` unit rewrite | 37, 534–574, 710–716, 1056–1083 | keep behind the same define | DONE | SDK has no field-unit concept. **Flag:** `unit.erase(pos, pos + 1)` erases `pos+1` chars, not 2 |
| 23 | `DEBUG_*` compile-time dumps | 29–33, 129–139, 604–651, 743–759 | keep behind the same defines | DONE | |
| 24 | Fatal `exit(EXIT_FAILURE)` on missing FMT / unknown type in offset calc | 948–984 | return error `Status` | DONE | **Necessary deviation:** `exit()` would kill the whole app |
| 25 | Info dialog: Parameters tab (latest values, sorted) + Message Logs tab (boot-time s, 2 dp), shown only if non-empty, modeless, parented to main window, title `APBin file <path>` | 769–777; `apbin_messages_dialog.*` | Decision 1: `setDatasetMetadata` → Dataset Info (`Parameters`, `Message Logs`) + `_MSG` topic | DONE (deviation) | Same content and formatting (params `%g` with Qt's half-away tie rounding; messages boot-time `%.2f`). Deviations: shown on demand via the dataset's Info button, not auto-opened; generic tree, not two sortable tables; key order is the host's (`QJsonObject`), not PJ3's locale-aware sort |
| 26 | Dialog geometry + both header states persisted (QSettings) | `apbin_messages_dialog.cpp`:44–74 | none (host owns the Dataset Info window) | DONE (deviation) | Dropped by decision 1 |

## Decisions (2026-10-23)

1. **Info dialog (#25, #26) → Dataset Info + `_MSG` topic.** No pre-import dialog. Parameters
   (latest values) and log messages go into `setDatasetMetadata(json)`, shown in PJ4's built-in
   Dataset Info window. MSG is also written as a time-synced text topic `_MSG` (field `message`).
   Accepted deviations: shown on demand rather than auto-popped after load; generic key/value
   tree instead of two sortable tables; no geometry/header persistence (#26, the host owns that window).
2. **Naming → topic per message.** Topic `GPS` / `GPS/#0`, fields `Lat`, ...; `_PARM/<name>`
   with field `value`. Existing PJ3 layouts must be converted regardless (PJ4 layouts store
   `topic` + `field` attributes, not PJ3's `<curve name="/GPS/Lat">`); a converter splits
   PJ3 names at the last `/`.
3. **MSG layout bug (#13) → deferred to the upstream review below.** Port faithfully first.
4. **Parallel decoding → serial first.** Re-apply `87bf81ad` / `9f978e59` as a separate step
   once the serial port works, and re-measure (PJ4 forbids host writes off the import thread).

## ArduPilot upstream review (2026-10-23)

Our import (`e05c8218`) is byte-identical to upstream `399136d` (2025-06-16). Reviewed against
upstream `main` @ `3e2f925`:

| Upstream | Change | Finding | Recommendation |
|---|---|---|---|
| `3e2f925` | MSG `QZ` → `QBBZ`: locate `TimeUS`/`Message` by name | Our #13 bug. AA-TRT-0051 (ArduPlane 4.5.7) is still `QZ`; upstream's test log is `QBBZ` | **Done** (clean text on the `QBBZ` test log) |
| `39d5c7f`, `49208c1` | Time sync: anchor on the first GPS sample with NSats ≥ 4 and non-zero TimeUS/GWk/GMS; look up GWk/GMS/NSats by name, skip sync if missing | Fixes our #18 (a) and (b): sample `[1]` without a fix anchors the log to the 1980 GPS epoch | **Done** |
| (none) | **GMS double scaling** (found in this review, also present upstream) | FMTU gives GMS multiplier `C` (×0.001), so `apply_multipliers` already converts it to seconds and `apply_timesync` multiplies by 0.001 again. Absolute times are off by GMS × 0.999 (0–7 days): AA-TRT-0051 plots at 2025-10-19 00:07 UTC instead of 2025-10-24 11:19 UTC. Relative times are unaffected | **Done**: when a multiplier was applied, recover the exact integer ms by undoing it (the log stores `C` at float precision, 0.0010000000475, ~20 ms off otherwise). AA-TRT-0051 now starts 2025-10-24 11:19:49.8 UTC; every timestamp moved by the same 472 335.072 s, series and counts unchanged |
| `db0618d` (part) | PARM `QNff`: show each parameter's default value | Upstream appends it to the parameter name | **Done**: Dataset Info shows `value  (default d)`; read by field name, so `QNf` logs show the value only. ArduPlane 4.5.7 already logs `QNff` |
| `db0618d`, `d90b7c3` | Rename RCOU/RCO2 `Cn` fields after their `SERVOn_FUNCTION` | Off by default upstream; renaming fields breaks layouts | Skip |
| `76a5822` | Message/field descriptions (embedded Plane/Copter XML, ~1 MB) as tooltips | SDK 0.34.1 has no description/tooltip for scalar fields | Blocked by SDK |
| `632dbfd` | Message timeline + log as numeric series synced to plot time | PJ3 workaround for the lack of text series; superseded by our `_MSG` topic | Skip |
| `be8cf24` | 16-char parameter names | We already use `strnlen(name, 16)` | Nothing to do |
| `f2f5531` | Reset per-load state | Already done (our `0f5e5c4b`, row #2) | Nothing to do |
| `e68139e` | Initialize timestamp members | Only upstream's new members | Nothing to do |
| `d7a72b7` | Units no longer force-enabled | Ours were never on (#22) | Nothing to do |
| `43a8d81` | Test log `tests/logs/00000001.BIN` (1.9 MB, `QBBZ` MSG, `QNff` PARM) | Useful second fixture for the MSG fix | Use for verification |

## Verification (2026-10-23, faithful port before the upstream review)

Log `AA-TRT-0051 - 2025-10-24 11-19-54.bin` (53 MB, ArduPlane 4.5.7), loaded by PJ3's
`DataLoadAPBin.dll` (airbound-main build of 2026-08-13) and by this plugin, each through a headless
harness printing per-series count, time range and an FNV-1a hash of every (timestamp ns, value)
pair (PJ4 side: `tools/apbin_dump.cpp`; points stable-sorted by time, as PJ3's PlotData keeps them):

- 2320 / 2320 series bit-identical (names mapped `/X` → `X`, `/_PARM/N` → `_PARM/N/value`).
- 1305 / 1305 parameters with identical displayed values. Order differs only (see #25).
- 49 / 49 log messages identical.
- PJ4 additionally writes the `_MSG` text topic (49 rows), per decision 1.

Host-level difference outside the plugin: PJ3's PlotData silently drops NaN/±inf samples, the PJ4
datastore keeps them. `apbin_dump` counts those separately (`NONFINITE`); this log has none.

The PJ3 harness must run from an interactive desktop session (its progress dialog needs Qt's
Windows event loop); it cannot run from Claude Code's shell.

After the upstream review the plugin intentionally differs from PJ3: absolute timestamps (GMS fix,
first-valid-fix anchor), MSG text on `QBBZ` logs, and parameter entries with defaults. On
AA-TRT-0051 the series set and per-series counts are unchanged and every timestamp moved by one
constant (472 335.072 s, ±256 ns double rounding).

## PJ4-only additions

- **Vehicle pose for the 3D view (2026-10-24).** Object topic `_3D/frames` (`kFrameTransforms`; `_TF` until 2026-11-03), one
  `world -> vehicle` transform per EKF sample: XKF1 of the primary EKF3 core at each moment, as
  reported by XKF4's `PI` (core 0 if XKF4 is missing); no 3D pose without XKF1. Position is `PN/PE/PD` (metres from the EKF origin) converted to ENU
  (x = east, y = north, z = up); attitude is `Roll/Pitch/Yaw` converted from body FRD in NED to
  body FLU in ENU (REP-103), so the frame's x axis is the nose. Checked on AA-TRT-0051 with
  `apbin_dump` (`OBJECT`/`TF` lines): 5652 transforms, translation and heading match the raw XKF1
  values, roll unchanged, pitch sign flipped as FLU requires. In the app: drag `_3D/vehicle` (or `_3D/frames`) onto an empty
  plot area to open a 3D view, then right-click the `vehicle` frame for "Create trail" and "Follow
  this frame" (also in the 3D settings panel: Create Trail, Follow frame, Frames size).
- **Position setpoint for the 3D view (2026-10-26).** Same `_3D/frames` topic (all transforms merged in time
  order), frame `world -> setpoint`: the position controller's target `PSCN.TPN` / `PSCE.TPE` /
  `PSCD.TPD` (metres north/east/down from the EKF origin, the same frame as XKF1), converted to ENU
  like the vehicle. Rotation (2026-11-03): the attitude controller's target, `ATT.DesRoll` /
  `DesPitch` / `DesYaw` nearest within 0.1 s, identity when none. In VTOL these are angles of the
  quadplane's AHRS view, not the body (`Write_AttitudeView`): the view is the body rotated by
  `Ry(y_angle + Q_TRIM_PITCH)`, `y_angle` 90 for a tailsitter (`Q_TAILSIT_ENABLE` > 0), so the
  target body attitude is `R_des * Ry(...)` (AP_AHRS_View.cpp, quadplane.cpp `create_view`). ATT's
  actual Roll/Pitch/Yaw are view angles too (so unusable for the vehicle; XKF1 is). Driven by PSCN's
  timestamps; PSCE/PSCD are paired by nearest sample within 5 ms (the three are logged in the same
  loop, ~10 us apart). Only logged while the position controller runs (VTOL / multicopter), so
  the 3D view holds the last setpoint through fixed-wing flight. Checked on AA-TRT-0051:
  `TFEDGES _TF ordered=yes world->setpoint=2868 world->vehicle=5652`, values equal to the raw
  PSC fields.
- **Aircraft model for the 3D view (2026-11-02).** Object topic `_3D/vehicle` (`kSceneEntities`; `_MODEL` until 2026-11-03, `_3D/model` until 2026-11-04): one
  entity `aircraft`, `frame_locked` to `vehicle`, lifetime 0, published at the first vehicle pose,
  holding the airframe's GLB as an inline `ModelPrimitive` (`model/gltf-binary`).
  - Airframe from the board name in the boot messages (first word of a MSG):
    `Pixhawk6C-bdshot`/`AB-TRT` -> TRT (`TRT.glb`), `AB-MOD` -> MOD (`TRT_MOD.glb`),
    `AB-V2` -> V2 (`V2.glb`), table `kAirframeModels`. Unknown board: info message, no model.
  - Model files (and their moving-part configs) are not in the repository: `AIRBOUND_MODELS_DIR`,
    else `models/` next to the plugin library. Missing file: warning naming the path.
  - The models (Blender glTF exports) have the nose towards -Z, right wing +X, up +Y. The 3D
    view's mesh loader converts glTF's Y-up to Z-up on import ((x, y, z) -> (x, -z, y)), so the
    loaded mesh has the nose +Y, right wing +X, up +Z, and the pose rotation maps THAT to the
    vehicle frame (FLU): -90 degrees about z. (Mapping the raw glTF axes instead double-counts the
    loader's conversion and pitches the model 90 degrees: vertical in fixed-wing flight,
    horizontal in hover.) Drawn with the largest dimension at 5 m (as the
    Airbound web log viewer does, so it is visible at flight scale) and the bounding-box centre on
    the frame origin; bounds come from the GLB's POSITION accessor min/max with node transforms.
  - Checked on AA-TRT-0051 (board `Pixhawk6C-bdshot`): `MODEL ... id=aircraft frame=vehicle
    locked=1 lifetime=0 airframe=TRT ... scale=3.575` (1.398 m span).
  - Setpoint model (2026-11-03; own topic since 2026-11-04 so it can be hidden and given its own
    opacity): object topic `_3D/setpoint`, the same GLB, entity `setpoint` locked to the
    `setpoint` frame, `override_color` translucent red (230, 40, 40, alpha 45; the 3D view draws
    an overridden model with a plain material, so the colour covers textured parts too). One
    message per setpoint run (a gap over 1 s ends a run), lifetime = run length + 0.5 s, so it is
    hidden through fixed-wing flight. Each message carries its own GLB copy. With more than one
    run, a last message at the final vehicle pose deletes `setpoint`: the 3D view limits the
    playhead to a multi-message topic's first..last span, so without it the view froze at the last
    run's start (a lone message counts as valid from then on, which is why `_3D/vehicle` is one
    message). It is a deletion because an empty batch serializes to no bytes and the codec rejects
    that. AA-TRT-0051: `_3D/vehicle count=1`, `_3D/setpoint count=3` (2 runs), t1 = the last
    vehicle pose.
  - Control-surface animation (2026-11-12), `airframe_model.{h,cpp}`:
    - Config: `<model>.json` next to the GLB (`TRT.json`, ...), the Airbound web log viewer's
      format unchanged, so a config copied from its Models panel works as is. Per surface: servo
      `channel`, `nodeName`, `hingeLocalAxis`, `angleAtMin`/`angleAtMax` (radians, 0 at trim),
      `reverse`; `pwmSource: "auto"` takes the PWM range from `SERVOn_MIN/TRIM/MAX` (ArduPilot's
      1100/1500/1900 when invalid), otherwise the surface's `pwmMin/pwmNeutral/pwmMax`. `motors`
      are ignored (propellers do not spin). No config: rigid model, as before.
    - The 3D view bakes a GLB into one rigid mesh, so the model is split: each surface's node with
      its descendants (minus nested surfaces) becomes its own GLB, the rest is the body. Every piece
      keeps only the meshes, materials, textures and buffer bytes it uses, and draws at its rest
      place in model space, so body + parts assemble into the original model. AA-TRT-0051 (TRT):
      body 7.7 MB, four parts 0.4-0.9 MB; the setpoint copy stays the whole, rigid model.
    - Each part is an entity `aircraft_<name>` locked to the frame `vehicle/<name>` (`~CS_ELEVON_L`
      -> `vehicle/elevon_l`), placed like the body. The frame's parent is `vehicle`, or the
      enclosing surface's frame for nested ones (TRT_MOD/V2 wrists on elbows), and its transform
      is the turn about the hinge in the vehicle frame: angle from the web viewer's PWM mapping
      (`pwm_to_angle`, piecewise linear around trim, clamped) of the logged output (`RCOU` C1-C14,
      `RCO2` C15-C18), pivot and axis from the node's rest place mapped like the model (scale,
      Y-up to Z-up, placement pose). The turn matches the web viewer's `rest * axis_angle` on the
      node; a mirrored frame above the node flips its sign. A surface under a non-uniformly
      scaled node, a missing node or a node with a `matrix` stays on the body with a warning.
    - Only angle changes are published (steps under 0.05 degrees are dropped, keeping the sample
      before each change so interpolation holds the angle), plus a rest transform at the first
      vehicle pose. AA-TRT-0051: 5652 RCOU samples -> 3060-5088 transforms per surface.
    - Tests: `apbin_plugin_tests` (synthetic GLB: split contents, rest places, motion equal to the
      node rotation also under a mirrored root, the PWM mapping, the config format). With
      `AIRBOUND_MODELS_DIR` set it also splits the real models: every surface of TRT, TRT_MOD and V2
      becomes a part, without warnings, and the pieces' bounds union equals the whole model's.
- **Terrain for the 3D view (2026-11-04).** Object topic `_3D/terrain` (`kSceneEntities`): one
  entity `terrain` on `world`, published at the first vehicle pose, holding a GLB built in memory
  by `terrain.{h,cpp}`: satellite imagery draped over an elevation model around the flight.
  - Sources (as in the Airbound web log viewer): ESRI World Imagery tiles (JPEG) and AWS Terrain
    Tiles in Terrarium encoding (PNG, `R*256 + G + B/256 - 32768` metres above sea level). The
    entity's metadata carries the attributions.
  - Placement: the EKF origin from the first `ORGN` of type 0 (Lat/Lng in 1e-7 degrees unless FMTU
    scaled them, Alt in metres AMSL), the point XKF1 positions are relative to. Lat/lon map to
    east/north with ArduPilot's own scale (`LATLON_TO_M`, longitude scaled by the cosine of the
    mean latitude), heights are `elevation - ORGN.Alt`. No ORGN: info message, no terrain.
  - Area: the vehicle poses' bounding box plus max(150 m, 25 %). Imagery zoom for about 8 tiles
    across (at most 144 tiles, zoom 10-19), elevation zoom min(imagery, 14) (Terrarium is
    unreliable above 14). One patch per imagery tile: a 17x17 grid sampled bilinearly from the
    elevation, textured with the tile's JPEG as downloaded (no image decoding). glTF axes
    (east, up, -north), which the 3D view's Y-up to Z-up import turns into ENU.
  - Downloads: libcurl (static, TLS via OpenSSL), 8 in parallel, 5 s connect / 20 s total
    timeout. Certificates: the Windows certificate store; on Linux `SSL_CERT_FILE`, else the
    first CA bundle found among the Debian, Fedora, openSUSE and Alpine locations, since the
    path compiled into libcurl is the build machine's. Only tiles missing from the cache are
    downloaded; they are kept forever under `AIRBOUND_TILE_CACHE_DIR`, else
    `%LOCALAPPDATA%\Airbound\PlotJuggler\tiles` (`$XDG_CACHE_HOME/airbound/plotjuggler/tiles`,
    default `~/.cache/...`, on Linux). The first tile is fetched alone; if it gets no answer
    the rest use the cache only, so an offline load waits for one timeout.
    `AIRBOUND_TERRAIN=0` skips terrain.
  - Checked on AA-TRT-0051: 130 imagery tiles (zoom 19), 2 elevation tiles (zoom 14), 2.7 MB GLB;
    first load 8.5 s (132 downloads), cached 81 ms. Vertically the terrain at the origin is
    5.4 m below `ORGN.Alt`, and the vehicle at landing (XKF1 z = -8.1 m) is 2.8 m below the
    terrain: GPS altitude, the elevation data (surface model, ~10 m pixels) and the EKF's height
    drift disagree by a few metres.
  - `apbin_dump` with `APBIN_DUMP_MODELS_DIR=<dir>` writes every inline model to `<dir>/<id>.glb`.
- **Progress over the whole load (2026-11-05).** PJ3's bar tracked parsing only (row 4), so in PJ4
  it sat at 100% while the rows, transforms, models and terrain were written. `load_progress.h`
  now gives each stage a share of one bar (total 1000) by its estimated seconds: parsing and
  processing by file size, the rows by count, the transforms by count, the models a fixed 0.3 s,
  and the terrain 0.07 s per tile still to download plus 1 ms per cached one
  (`terrain::count_tiles`). The estimates that depend on the parsed data are set after parsing,
  and the bar left over is split again at each stage start, so it never moves backwards. While
  tiles are fetched (`Options::on_tiles`) the bar follows the fetched tiles or the elapsed time
  against the estimate, whichever is further, because single tiles can take seconds. Rows are
  written with progress ticks, so the host also flushes them during the load. On AA-TRT-0051 the
  bar moves steadily: about 3 s with the tile cache, about 10 s when downloading. The file is now
  read with one sized read: `istreambuf_iterator` took 1.3 s for 53 MB.
  - `apbin_dump` with `APBIN_DUMP_TIMINGS=1` prints every progress tick, host message and first
    object push to stderr with the seconds since the start.
