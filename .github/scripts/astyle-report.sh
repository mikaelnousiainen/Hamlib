#!/bin/sh
# Report astyle formatting deviations in files changed between two refs.
# Informational only: prints a Markdown report and always exits 0.
#
# Usage: astyle-report.sh <base-ref> <head-ref>

set -eu

base=${1:?usage: astyle-report.sh <base-ref> <head-ref>}
head=${2:?usage: astyle-report.sh <base-ref> <head-ref>}

echo "#### astyle formatting check"
echo

if ! command -v astyle >/dev/null 2>&1; then
    echo "_astyle is not installed; check skipped._"
    echo
    exit 0
fi

files=$(git diff --name-only --diff-filter=ACMR "$base" "$head" -- \
        '*.c' '*.h' '*.cpp' '*.cc')

if [ -z "$files" ]; then
    echo "✅ No C/C++ files changed."
    echo
    exit 0
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

count=0
total=0
detail="$tmpdir/details.md"
: > "$detail"

for f in $files; do
    [ -f "$f" ] || continue
    total=$((total + 1))
    astyle --options=scripts/astylerc < "$f" > "$tmpdir/fmt" 2>/dev/null \
        || continue
    if ! cmp -s "$f" "$tmpdir/fmt"; then
        count=$((count + 1))
        lines=$(diff -u "$f" "$tmpdir/fmt" | grep -c '^[+-][^+-]') || true
        {
            echo "<details><summary><code>$f</code> — $lines line(s)" \
                 "differ from astyle output</summary>"
            echo
            echo '~~~diff'
            diff -u --label "$f (PR)" --label "$f (astyle)" \
                "$f" "$tmpdir/fmt" | sed 's/^~~~/ ~~~/' | head -n 120
            echo '~~~'
            echo '</details>'
            echo
        } >> "$detail"
    fi
done

if [ "$count" -eq 0 ]; then
    echo "✅ All $total changed C/C++ file(s) match" \
         '`astyle --options=scripts/astylerc` formatting.'
else
    echo "⚠️ $count of $total changed C/C++ file(s) differ from the" \
         "project astyle style (informational — see"
    echo '`README.coding_style`):'
    echo
    head -c 20000 "$detail"
fi
echo
