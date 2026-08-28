#!/bin/sh
# Dump the capabilities of every rig/rotator/amplifier model in a build
# tree, one file per model, for later comparison.
#
# Usage: dumpcaps-all.sh <build-tree> <out-dir>

set -eu

tree=${1:?usage: dumpcaps-all.sh <build-tree> <out-dir>}
out=${2:?usage: dumpcaps-all.sh <build-tree> <out-dir>}

mkdir -p "$out"

for tool in rigctl rotctl ampctl; do
    bin="$tree/tests/$tool"
    if [ ! -x "$bin" ]; then
        echo "dumpcaps-all: $bin not found; skipping" >&2
        continue
    fi
    "$bin" -l 2>/dev/null | awk '$1 ~ /^[0-9]+$/ { print $1 }' \
            | sort -n -u | while read -r model; do
        # The "Hamlib version:" line carries the build timestamp and
        # commit SHA, which would make every model differ between the
        # baseline and PR builds.
        if ! "$bin" -m "$model" -u 2>/dev/null \
                | grep -v '^Hamlib version:' \
                > "$out/$tool-model-$model.txt"; then
            echo "(dump-caps failed for $tool model $model)" \
                >> "$out/$tool-model-$model.txt"
        fi
    done
done
