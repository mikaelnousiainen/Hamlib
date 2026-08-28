#!/bin/sh
# Compare dump-caps output and the libhamlib ABI between a baseline
# (merge-base) build and the PR build.  Prints a Markdown report and
# always exits 0 (informational only).
#
# Usage: baseline-report.sh <base-caps-dir> <pr-caps-dir> \
#                           <base-lib> <pr-lib> <base-hdrs> <pr-hdrs>

set -u

bcaps=${1:?base caps dir}
pcaps=${2:?PR caps dir}
blib=${3:-}
plib=${4:-}
bhdr=${5:-}
phdr=${6:-}

echo "#### dump-caps comparison against merge base"
echo

detail=$(mktemp)
: > "$detail"
changed=0

files=$({ ls "$bcaps" 2>/dev/null; ls "$pcaps" 2>/dev/null; } | sort -u)

for f in $files; do
    a="$bcaps/$f"
    b="$pcaps/$f"
    if [ ! -f "$a" ]; then
        echo "- ➕ \`$f\`: new model (no baseline dump)" >> "$detail"
        changed=$((changed + 1))
    elif [ ! -f "$b" ]; then
        echo "- ➖ \`$f\`: model removed" >> "$detail"
        changed=$((changed + 1))
    elif ! cmp -s "$a" "$b"; then
        changed=$((changed + 1))
        {
            echo "<details><summary>🔀 <code>$f</code> capabilities" \
                 "changed</summary>"
            echo
            echo '~~~diff'
            diff -u --label "$f (merge base)" --label "$f (PR)" "$a" "$b" \
                | sed 's/^~~~/ ~~~/' | head -n 100
            echo '~~~'
            echo '</details>'
            echo
        } >> "$detail"
    fi
done

if [ "$changed" -eq 0 ]; then
    echo "✅ No rig/rotator/amplifier capability changes detected" \
         '(`--dump-caps` output identical for all models).'
else
    echo "🔀 $changed model(s) with capability changes:"
    echo
    head -c 25000 "$detail"
fi
rm -f "$detail"

echo
echo "#### ABI comparison against merge base"
echo

if ! command -v abidiff >/dev/null 2>&1; then
    echo "_abidiff (abigail-tools) not available; skipped._"
elif [ ! -f "$blib" ] || [ ! -f "$plib" ]; then
    echo "_Shared library missing on one side; skipped._"
else
    out=$(mktemp)
    if abidiff --headers-dir1 "$bhdr" --headers-dir2 "$phdr" \
            "$blib" "$plib" > "$out" 2>&1; then
        echo '✅ No ABI changes in `libhamlib`.'
    else
        rc=$?
        echo "⚠️ \`abidiff\` reports ABI differences (exit code $rc —" \
             "see the abidiff manual for the meaning of the bits):"
        echo
        echo '~~~'
        sed 's/^~~~/ ~~~/' "$out" | head -n 200
        echo '~~~'
    fi
    rm -f "$out"
fi
echo
