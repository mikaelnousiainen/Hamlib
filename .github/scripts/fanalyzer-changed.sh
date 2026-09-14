#!/bin/sh
# Run GCC -fanalyzer on the C files changed between two refs, writing
# the combined diagnostics to build.log for the warning collector.
#
# A whole-tree -fanalyzer build is infeasible here: the analyzer is
# pathologically slow on some files (exponential state explosion, e.g.
# in the security/ crypto code), so only the changed files are
# analyzed, each compiled standalone with a hard per-file timeout.
# Files that need flags this script does not pass simply record a note
# instead of diagnostics.
#
# Requires a configured tree (include/hamlib/config.h must exist).
#
# Usage: fanalyzer-changed.sh <base-ref> <head-ref>

set -eu

base=${1:?usage: fanalyzer-changed.sh <base-ref> <head-ref>}
head=${2:?usage: fanalyzer-changed.sh <base-ref> <head-ref>}

CC=${CC:-gcc}
PER_FILE_TIMEOUT=${PER_FILE_TIMEOUT:-300}
MAX_FILES=${MAX_FILES:-100}
export CC PER_FILE_TIMEOUT

git diff --name-only --diff-filter=ACMR "$base" "$head" -- '*.c' \
    > fanalyzer-files.txt
total=$(wc -l < fanalyzer-files.txt | tr -d ' ')
: > build.log

if [ "$total" -eq 0 ]; then
    echo "No C files changed; nothing to analyze" | tee -a build.log
    exit 0
fi

if [ "$total" -gt "$MAX_FILES" ]; then
    echo "(note: analyzing only the first $MAX_FILES of $total changed" \
         "C files)" | tee -a build.log
    head -n "$MAX_FILES" fanalyzer-files.txt > fanalyzer-files.tmp
    mv fanalyzer-files.tmp fanalyzer-files.txt
fi

mkdir -p fanalyzer-logs
njobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)

xargs -P "$njobs" -n 1 sh -c '
    f=$1
    [ -e "$f" ] || exit 0
    log="fanalyzer-logs/$(printf "%s" "$f" | tr "/" "_").log"
    if ! timeout "$PER_FILE_TIMEOUT" "$CC" -fanalyzer \
            -DHAVE_CONFIG_H -DIN_HAMLIB \
            -g -O1 -I. -Iinclude -Iinclude/hamlib -Isrc -Ilib \
            -I"$(dirname "$f")" -I"$(dirname "$(dirname "$f")")" \
            -c "$f" -o /dev/null > "$log" 2>&1; then
        echo "(-fanalyzer could not fully analyze $f:" \
             "timeout or compile error)" >> "$log"
    fi
' analyze < fanalyzer-files.txt

cat fanalyzer-logs/*.log >> build.log 2>/dev/null || true
echo "Analyzed $(wc -l < fanalyzer-files.txt | tr -d ' ') file(s)"
