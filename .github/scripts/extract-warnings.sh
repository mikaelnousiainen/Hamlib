#!/bin/sh
# Extract compiler warnings from a build log into a normalized,
# deduplicated list (one warning per line).
#
# Paths are canonicalized so the same warning seen through different
# build directories (VPATH/distcheck builds, "../.." prefixes, the
# distdir) collapses into a single repository-relative entry.
#
# Usage: extract-warnings.sh <log-file> <out-file> [only-pattern]
#
#   only-pattern: optional awk ERE; when given, only warning lines
#   matching it are kept (e.g. '\[-Wanalyzer' for -fanalyzer runs).
#
# Works with POSIX sh/awk (Linux, macOS, MSYS2).

set -eu

log=${1:?usage: extract-warnings.sh <log-file> <out-file> [only-pattern]}
out=${2:?usage: extract-warnings.sh <log-file> <out-file> [only-pattern]}
only=${3:-}

outdir=$(dirname "$out")
mkdir -p "$outdir"

if [ ! -f "$log" ]; then
    : > "$out"
    echo "extract-warnings: log file '$log' not found; wrote empty list" >&2
    exit 0
fi

# The pattern and workspace path are passed through the environment:
# awk -v would reprocess backslash escapes (turning '\[' into '[').
EW_ONLY=$only
EW_WS=$(pwd)
export EW_ONLY EW_WS

awk '
function canon(p,    n, parts, keep, i, j, res, abs) {
    abs = (substr(p, 1, 1) == "/")
    n = split(p, parts, "/")
    j = 0
    for (i = 1; i <= n; i++) {
        if (parts[i] == "." || parts[i] == "") { continue }
        if (parts[i] == "..") { if (j > 0) { j-- }; continue }
        j++
        keep[j] = parts[i]
    }
    # Drop distcheck/VPATH build prefixes so paths are repo-relative.
    i = 1
    while (i <= j && (keep[i] ~ /^hamlib-/ || keep[i] == "_build" || \
                      keep[i] == "sub")) {
        i++
    }
    res = ""
    for (; i <= j; i++) {
        res = (res == "" ? keep[i] : res "/" keep[i])
    }
    if (abs) { res = "/" res }
    return res
}
BEGIN {
    esc = sprintf("%c", 27)
    only = ENVIRON["EW_ONLY"]
    wsl = ENVIRON["EW_WS"] "/"
}
{
    sub(/\r$/, "")
    gsub(esc "\\[[0-9;]*[A-Za-z]", "")
}
$0 !~ /warning:/       { next }
/configure: WARNING/   { next }
/^ *make(\[[0-9]+\])?:/ { next }
/libtool: warning/     { next }
{
    if (only != "" && $0 !~ only) { next }
    line = $0
    # Remove absolute workspace prefixes (literal match, may repeat).
    while ((k = index(line, wsl)) > 0) {
        line = substr(line, 1, k - 1) substr(line, k + length(wsl))
    }
    # Canonicalize the file part of "file:line[:col]: warning: ...".
    if (line !~ /^[A-Za-z]:[\/\\]/ && match(line, /^[^:]+:[0-9]+/)) {
        colon = index(line, ":")
        line = canon(substr(line, 1, colon - 1)) substr(line, colon)
    }
    print line
}
' "$log" | sort -u > "$out"
