# Airbound plugin registry

The app's marketplace and its startup "updates available" check read one plugin
registry (a `registry.json`). This fork points them at the **Airbound registry**:
upstream PlotJuggler's registry plus the Airbound plugins from `plugins/`. So
official plugins keep updating as before, and the Airbound plugins update the same
way, including copies bundled by the installer, once a newer version is published.

- URL: `https://raw.githubusercontent.com/AirboundInc/PlotJuggler/refs/heads/plugin-registry/registry.json`
  (the app default, `kDefaultRegistryUrl` in `pj_app/src/MainWindow.cpp`; can be
  overridden in Preferences).
- Storage: the `plugin-registry` branch. `airbound-extensions.json` holds the
  Airbound entries, `registry.json` is the merged file the app downloads. Both are
  generated: never edit that branch by hand.

## Releasing a plugin

1. Bump `version` in `plugins/<folder>/manifest.json` (semantic version) and commit
   it on `airbound-main-4.x`.
2. Tag that commit `<folder>/v<version>` and push the tag, e.g.
   ```
   git tag data_load_apbin/v0.1.1
   git push origin data_load_apbin/v0.1.1
   ```
3. `.github/workflows/airbound-plugin-release.yml` builds the plugin on Windows,
   attaches `<id>-<version>-windows-x64.zip` to a GitHub Release for the tag (never
   marked "latest", which stays reserved for app releases), and adds the release
   to the registry. Apps offer the update on their next start.

The workflow refuses a tag whose version differs from the manifest. A manual run
of the workflow (Actions tab, with the plugin folder as input) only builds the
ZIP as a workflow artifact: a dry run.

Only Windows x64 archives are published for now. A Linux build would add its
platform (`linux-x86_64`) to the same entry with another `set-entry` call.

## Keeping up with upstream

`.github/workflows/airbound-registry-sync.yml` re-merges upstream's registry daily
(and on demand from the Actions tab). An Airbound entry replaces an upstream
entry with the same id.

## Files

- `build_plugin.ps1` builds one plugin on its own (only `plotjuggler_sdk` and
  `nlohmann_json`, at the app's pins) and writes the marketplace ZIP in the
  official layout (`<id>/<dll>`, `<id>/manifest.json`) plus its SHA-256. Runs
  locally too, with the same prerequisites as `build.bat`:
  `.\plugins\registry\build_plugin.ps1 -Plugin data_load_apbin`
- `registry_tool.py` edits `airbound-extensions.json` (`set-entry`) and writes the
  merged `registry.json` (`merge`).
- `update_registry_branch.sh` runs the tool on a worktree of `plugin-registry`,
  creating the branch on first use, and pushes when something changed.
