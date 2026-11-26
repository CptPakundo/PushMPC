#!/bin/sh
# key.sh <repository checkout> <nvof: true|false>: the cache key of the core build's output (core-out): a hash of the
# git objects of everything the core build compiles or reads. Tests, tools and other workflows are left out on purpose
# (they do not change core-out). One formula for both places that look the key up (core.yml's core-cache and its
# single-job mode). A new input of the core build goes into this list.
set -eu
r=$1
h=$(git -C "$r" ls-tree -r HEAD -- \
      filter pushmpc updater patches cmake \
      vsplugin/CMakeLists.txt vsplugin/src vsplugin/engines vsplugin/stub vsplugin/third_party \
      pmml/CMakeLists.txt pmml/include pmml/src pmml/tools pmml/scripts/fetch_runtime.py \
      scripts/pushfps-pin.json scripts/ml-runtime-pin.json \
      .github/workflows/core.yml .github/actions/pushmpc-core \
    | sha256sum | cut -c1-40)
echo "pushmpc-core-out-v2-nvof$2-$h"
