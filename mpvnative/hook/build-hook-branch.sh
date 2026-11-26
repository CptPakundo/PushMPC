#!/bin/bash
# build-hook-branch.sh <branch, e.g. n6.1> <src dir> <out dir>: the hook's private libavfilter for EVERY tag of one FFmpeg
# branch pinned in mpvnative/pins.json (hookTags), one after another in one tree (ccache keeps it quick). Each tag gives
# libpushmpc-avfilter-<its libavfilter version>.so; tags of the same version give the same file (the later one stays).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
branch=$1; src=$2; out=$3
mkdir -p "$out"
if [ "$branch" = snapshots ]; then   # pinned FFmpeg master snapshots (pins.json snapshotCommits), from the GitHub mirror
    for commit in $(python3 -c "import json,sys; print(' '.join(c['commit'] for c in json.load(open(sys.argv[1])).get('snapshotCommits', [])))" "$here/../pins.json"); do
        [ -d "$src/.git" ] || git init -q "$src"
        git -C "$src" fetch -q --depth 1 https://github.com/FFmpeg/FFmpeg.git "$commit" && git -C "$src" checkout -q -f "$commit"
        [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "::error::snapshot $commit: checkout failed"; exit 1; }
        rm -f "$src/config_components.h" "$src/config.h"
        echo "== master@${commit:0:12}"
        bash "$here/build-hook.sh" "$src" "$out" > "$out/.build-$commit.log" 2>&1 || { tail -30 "$out/.build-$commit.log"; exit 1; }
        grep -E "^libpushmpc" "$out/.build-$commit.log" || true
    done
    ls -la "$out"; exit 0
fi
tags=$(python3 -c "import json,sys,re; print(' '.join(t['tag'] for t in json.load(open(sys.argv[1]))['hookTags'] if re.fullmatch(re.escape(sys.argv[2]) + r'(\.\d+)?', t['tag'])))" "$here/../pins.json" "$branch")
[ -n "$tags" ] || { echo "::error::no pinned tags for $branch"; exit 1; }
for tag in $tags; do
    commit=$(python3 -c "import json,sys; print([t['commit'] for t in json.load(open(sys.argv[1]))['hookTags'] if t['tag']==sys.argv[2]][0])" "$here/../pins.json" "$tag")
    if [ ! -d "$src/.git" ]; then git clone -q --depth 1 -b "$tag" https://git.ffmpeg.org/ffmpeg.git "$src"
    else git -C "$src" fetch -q --depth 1 origin tag "$tag" && git -C "$src" checkout -q -f "$tag"; fi
    [ "$(git -C "$src" rev-parse HEAD)" = "$commit" ] || { echo "::error::$tag: commit mismatch"; exit 1; }
    rm -f "$src/config_components.h" "$src/config.h"   # reconfigure per tag
    echo "== $tag"
    bash "$here/build-hook.sh" "$src" "$out" > "$out/.build-$tag.log" 2>&1 || { tail -30 "$out/.build-$tag.log"; exit 1; }
    grep -E "^libpushmpc" "$out/.build-$tag.log" || true
done
ls -la "$out"
