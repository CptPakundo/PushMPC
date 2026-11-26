#!/bin/bash
# build-hook-branch.sh <branch, e.g. n6.1> <src dir> <out dir>: the hook's private libavfilter for EVERY tag of one FFmpeg
# branch pinned in mpvnative/pins.json (hookTags), one after another in one tree. Each tag gives
# libpushmpc-avfilter-<its libavfilter version>.so; tags of the same version give the same file (the later one stays).
# FFB_CACHE (optional): a folder that keeps each tag's built tree between runs (ffbuild-cache.sh), so a filter change
# recompiles vf_pushmpc.o and relinks instead of configuring and building the whole tag again.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../ci/fetch-commit.sh"
. "$here/../ci/ffbuild-cache.sh"
branch=$1; src=$2; out=$3
mkdir -p "$out"
cfgv=$(cksum < "$here/build-hook.sh" | cut -d' ' -f1)   # the configure options live in build-hook.sh

one() {   # one <label> <commit>: the tree is checked out
    ffb_restore "$src" "hook-$(uname -m)-$1" "${FFB_CACHE:-}" "$2 $cfgv" || rm -f "$src/config_components.h" "$src/config.h"   # reconfigure
    echo "== $1"
    bash "$here/build-hook.sh" "$src" "$out" > "$out/.build-$1.log" 2>&1 || { tail -30 "$out/.build-$1.log"; exit 1; }
    grep -E "^libpushmpc" "$out/.build-$1.log" || true
    ffb_store "$src" "hook-$(uname -m)-$1" "${FFB_CACHE:-}" "$2 $cfgv"
}

if [ "$branch" = snapshots ]; then   # pinned FFmpeg master snapshots (pins.json snapshotCommits), from the GitHub mirror
    for commit in $(python3 -c "import json,sys; print(' '.join(c['commit'] for c in json.load(open(sys.argv[1])).get('snapshotCommits', [])))" "$here/../pins.json"); do
        fetch_commit "$src" "$commit" https://github.com/FFmpeg/FFmpeg.git || exit 1
        one "master@${commit:0:12}" "$commit"
    done
    ls -la "$out"; exit 0
fi
tags=$(python3 -c "import json,sys,re; print(' '.join(t['tag'] for t in json.load(open(sys.argv[1]))['hookTags'] if re.fullmatch(re.escape(sys.argv[2]) + r'(\.\d+)?', t['tag'])))" "$here/../pins.json" "$branch")
[ -n "$tags" ] || { echo "::error::no pinned tags for $branch"; exit 1; }
for tag in $tags; do
    commit=$(python3 -c "import json,sys; print([t['commit'] for t in json.load(open(sys.argv[1]))['hookTags'] if t['tag']==sys.argv[2]][0])" "$here/../pins.json" "$tag")
    fetch_commit "$src" "$commit" https://git.ffmpeg.org/ffmpeg.git "$tag" || exit 1   # by commit (the pin), never by tag name alone
    one "$tag" "$commit"
done
ls -la "$out"
