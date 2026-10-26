# SPDX-License-Identifier: MPL-2.0
"""Maintain the Airbound plugin registry (see README.md in this folder).

The registry the app reads is upstream PlotJuggler's registry with the Airbound
plugins added. Two files live on the `plugin-registry` branch:

  airbound-extensions.json  our entries only, edited by `set-entry`
  registry.json             what the app downloads, written by `merge`

Subcommands:

  set-entry  add or update one plugin's entry from its manifest.json and one
             platform's release archive (url + sha256). A new version replaces
             the entry's platforms; the same version adds or replaces just that
             platform, so per-platform builds of one release accumulate.

  merge      write registry.json = upstream registry + our entries. An Airbound
             entry replaces an upstream one with the same id. Output is
             deterministic, so an unchanged input never produces a diff.

Python 3.8+, standard library only.
"""

import argparse
import json
import re
import sys
import urllib.request

# Manifest fields copied into a registry entry, in the order upstream's entries use.
ENTRY_FIELDS = (
    "id", "name", "version", "description", "author", "publisher", "website", "repository",
    "license", "icon_url", "category", "tags", "min_plotjuggler_version", "min_sdk_required",
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
CHECKSUM_RE = re.compile(r"^sha256:[0-9a-f]{64}$")


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")


def load_airbound(path):
    """Our entries; a missing file is an empty registry (first publish)."""
    try:
        data = load_json(path)
    except FileNotFoundError:
        return {"extensions": []}
    if not isinstance(data.get("extensions"), list):
        raise SystemExit(f"{path}: no 'extensions' array")
    return data


def validate_entry(entry, where):
    for field in ("id", "name", "version"):
        if not isinstance(entry.get(field), str) or not entry[field]:
            raise SystemExit(f"{where}: entry without a '{field}'")
    platforms = entry.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        raise SystemExit(f"{where}: '{entry['id']}' has no platforms")
    for platform, artifact in platforms.items():
        if not isinstance(artifact, dict) or not str(artifact.get("url", "")).startswith("https://"):
            raise SystemExit(f"{where}: '{entry['id']}' {platform} has no https url")
        if not CHECKSUM_RE.match(str(artifact.get("checksum", ""))):
            raise SystemExit(f"{where}: '{entry['id']}' {platform} has no sha256 checksum")


def cmd_set_entry(args):
    manifest = load_json(args.manifest)
    if not SHA256_RE.match(args.sha256):
        raise SystemExit(f"--sha256 must be 64 lowercase hex digits, got '{args.sha256}'")
    entry = {field: manifest[field] for field in ENTRY_FIELDS if field in manifest}
    artifact = {"url": args.url, "checksum": "sha256:" + args.sha256}

    registry = load_airbound(args.airbound)
    extensions = registry["extensions"]
    existing = next((e for e in extensions if e.get("id") == entry["id"]), None)
    if existing is not None and existing.get("version") == entry["version"]:
        platforms = dict(existing.get("platforms", {}))
    else:
        platforms = {}
    platforms[args.platform] = artifact
    entry["platforms"] = dict(sorted(platforms.items()))
    validate_entry(entry, args.manifest)

    if existing is None:
        extensions.append(entry)
    else:
        extensions[extensions.index(existing)] = entry
    extensions.sort(key=lambda e: e["id"])
    write_json(args.airbound, registry)
    print(f"{entry['id']} {entry['version']}: {args.platform} -> {args.url}")


def read_upstream(source):
    if re.match(r"^https?://", source):
        request = urllib.request.Request(source, headers={"User-Agent": "airbound-registry-tool"})
        with urllib.request.urlopen(request, timeout=60) as response:
            return json.loads(response.read().decode("utf-8"))
    return load_json(source)


def cmd_merge(args):
    upstream = read_upstream(args.upstream)
    if not isinstance(upstream.get("extensions"), list):
        raise SystemExit(f"{args.upstream}: no 'extensions' array")
    ours = load_airbound(args.airbound)["extensions"]
    for entry in ours:
        validate_entry(entry, args.airbound)
    our_ids = {e["id"] for e in ours}
    if len(our_ids) != len(ours):
        raise SystemExit(f"{args.airbound}: duplicate ids")

    # Upstream order first (as upstream lists them), then ours sorted by id.
    merged = {key: value for key, value in upstream.items() if key != "extensions"}
    merged.setdefault("registry_version", "1.0")
    merged["extensions"] = [e for e in upstream["extensions"] if e.get("id") not in our_ids] + ours
    write_json(args.out, merged)
    replaced = sorted(our_ids & {e.get("id") for e in upstream["extensions"]})
    print(f"registry: {len(merged['extensions'])} extensions "
          f"({len(merged['extensions']) - len(ours)} upstream, {len(ours)} Airbound"
          + (f", replacing upstream {', '.join(replaced)}" if replaced else "") + ")")


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("set-entry", help="add or update one plugin entry")
    p.add_argument("--airbound", required=True, help="airbound-extensions.json (created if missing)")
    p.add_argument("--manifest", required=True, help="the plugin's manifest.json")
    p.add_argument("--platform", required=True, help="registry platform key, e.g. windows-x86_64")
    p.add_argument("--url", required=True, help="download URL of the release archive")
    p.add_argument("--sha256", required=True, help="SHA-256 of the archive, lowercase hex")
    p.set_defaults(func=cmd_set_entry)

    p = sub.add_parser("merge", help="write the merged registry.json")
    p.add_argument("--upstream", required=True, help="upstream registry.json URL or path")
    p.add_argument("--airbound", required=True, help="airbound-extensions.json")
    p.add_argument("--out", required=True, help="registry.json to write")
    p.set_defaults(func=cmd_merge)

    args = parser.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main(sys.argv[1:])
