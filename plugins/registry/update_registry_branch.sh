#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Regenerate the Airbound plugin registry on the `plugin-registry` branch and
# push it (see README.md in this folder). Used by the plugin release and the
# registry sync workflows; needs git push access to `origin`.
#
#   update_registry_branch.sh                      re-merge upstream's registry
#   update_registry_branch.sh set-entry <args...>  also add/update one Airbound
#                                                  entry (registry_tool.py set-entry
#                                                  arguments, without --airbound)
#
# Creates the branch (orphan) on first use. Commits only when registry.json or
# airbound-extensions.json actually changed.
set -euo pipefail

BRANCH="${REGISTRY_BRANCH:-plugin-registry}"
UPSTREAM="${UPSTREAM_REGISTRY_URL:-https://raw.githubusercontent.com/PlotJuggler/pj-plugin-registry/refs/heads/development/registry.json}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python3}"
command -v "$PYTHON" >/dev/null 2>&1 || PYTHON=python

work="$(mktemp -d)"
trap 'git worktree remove --force "$work/registry" >/dev/null 2>&1 || true; rm -rf "$work"' EXIT

if git fetch --quiet origin "refs/heads/$BRANCH:refs/remotes/origin/$BRANCH" 2>/dev/null; then
  git worktree add --quiet --detach "$work/registry" "origin/$BRANCH"
else
  echo "Branch $BRANCH does not exist yet: creating it."
  git worktree add --quiet --detach "$work/registry"
  git -C "$work/registry" checkout --quiet --orphan "$BRANCH"
  git -C "$work/registry" rm -r --quiet --force .
fi
dir="$work/registry"

if [[ $# -gt 0 ]]; then
  [[ "$1" == "set-entry" ]] || { echo "unknown command: $1" >&2; exit 2; }
  shift
  "$PYTHON" "$HERE/registry_tool.py" set-entry --airbound "$dir/airbound-extensions.json" "$@"
fi
"$PYTHON" "$HERE/registry_tool.py" merge --upstream "$UPSTREAM" \
  --airbound "$dir/airbound-extensions.json" --out "$dir/registry.json"

if [[ ! -f "$dir/README.md" ]]; then
  cat > "$dir/README.md" <<'EOF'
# Airbound PlotJuggler plugin registry

Generated: do not edit by hand. `registry.json` is what PlotJuggler downloads
(upstream PlotJuggler's registry plus the Airbound plugins listed in
`airbound-extensions.json`). Maintained by the workflows on `airbound-main-4.x`,
see `plugins/registry/README.md` there.
EOF
fi

git -C "$dir" add -A
if git -C "$dir" diff --cached --quiet; then
  echo "Registry unchanged."
  exit 0
fi
git -C "$dir" -c user.name="github-actions[bot]" \
  -c user.email="41898282+github-actions[bot]@users.noreply.github.com" \
  commit --quiet -m "${REGISTRY_COMMIT_MESSAGE:-Update the plugin registry}"
git -C "$dir" push --quiet origin "HEAD:refs/heads/$BRANCH"
echo "Pushed $BRANCH."
