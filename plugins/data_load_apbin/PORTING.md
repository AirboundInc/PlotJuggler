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
| 13 | MSG → log message list | 438–461 | log messages list (+ see Decision 1) | DONE | **Bug:** hardcodes old `QZ` layout. ArduPilot now logs `QBBZ` (TimeUS,ID,Seq,Message) → text gets 2 garbage bytes. Upstream fix `ArduPilot/plotjuggler-apbin-plugins@3e2f925` locates fields by name. Decision 3 |
| 14 | PARM → latest value per name + full time series | 462–492 | same | DONE | Hardcoded `QNf` prefix still valid for newer `QNff` |
| 15 | Decode all 20 format types to double | 786–923 | typed `ValueRef` per format char | DONE | Unknown type: rest of the record is skipped with a stderr line, as in PJ3. **Flag:** for `a`/`n`/`N`/`Z` PJ3 pushes the previous field's value (string fields become a numeric series repeating their neighbour); translated as-is, written as `kFloat64` |
| 16 | Instance id: numeric, or full string for `N`/`n`/`Z` | 998–1052 | same | DONE | Fix from `d3eb917d` |
| 17 | Multipliers from FMTU×MULT, skip 0/1, warn on missing | 1087–1138 | same; field becomes `kFloat64` when a multiplier applies, native type otherwise | DONE | Values decoded to double exactly as in PJ3, then written back as the native type, so values and precision are identical to PJ3 (integers above 2^53 still round, as before) |
| 18 | GPS time sync to Unix time (GWk/GMS, sample `[1]`, −18 leap s), applied to all TimeUS and PARM | 1142–1217 | offset added in **ns** to every timestamp | DONE | **Flags:** (a) indexes sample `[1]` → out of bounds if GPS has <2 samples; (b) missing `GWk`/`GMS` silently map to field 0 via `operator[]`; (c) uses first GPS instance. Translate as-is, but (a) must not crash the host: guard and skip sync |
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

## Follow-up: ArduPilot upstream review

Upstream (`ArduPilot/plotjuggler-apbin-plugins`) changes since our import (`e05c8218`, 2026-01-15),
to evaluate one by one after the faithful port:

| Upstream | Change | Overlaps |
|---|---|---|
| `3e2f925` | MSG `QZ` → `QBBZ`: locate fields by name | #13 bug |
| `39d5c7f`, `49208c1` | GPS time unavailable at log start; time-sync refactor | #18 flags (a)–(c) |
| `632dbfd` | Message timeline + values synced to plot time | #25 (our dialog shows boot time) |
| `76a5822` | Field descriptions parser + tooltips | new |
| `db0618d`, `d90b7c3` | Default param values; labels for configured RCOU functions | #14, new |
| `be8cf24` | 16-char parameter name display | #14 (we use `strnlen(16)`; verify) |
| `e68139e` | Initialize timestamps | verify |
| `f2f5531` | Reset per-load state | #2 (we have our own `0f5e5c4b`) |
| `d7a72b7` | Remove force-enable of units | #22 |

## Verification (2026-10-23)

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
