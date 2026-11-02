#!/bin/sh
# Checks that an Icom whose power status is inferred from a frequency read reports it from the rig,
# not from the cache: on while it answers, off once it is in standby and answers NG, as an IC-7610 does.

set -u

simulator=../simulators/simic7610
if [ ! -x "$simulator" ]; then
    echo "SKIP: $simulator is not built"
    exit 0
fi

log=$(mktemp)
line_buffered=""
if command -v stdbuf >/dev/null 2>&1; then
    line_buffered="stdbuf -oL"
fi
$line_buffered "$simulator" unused >"$log" 2>&1 &
simulator_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q "^name=" "$log" && break
    sleep 0.3
done
pty=$(sed -n 's/^name=//p' "$log" | head -1)
if [ -z "$pty" ]; then
    echo "FAIL: the simulated IC-7610 did not name its terminal"
    kill "$simulator_pid" 2>/dev/null
    exit 1
fi

# One session: the power status while on, then switched to standby and read again at once, while
# the frequency it read a moment ago would still be cached
output=$(./rigctl -m 3078 -r "$pty" -s 115200 get_powerstat set_powerstat 0 get_powerstat 2>/dev/null)
kill "$simulator_pid" 2>/dev/null
rm -f "$log"

powerstats=$(printf '%s\n' "$output" | tr '\n' ' ')
if [ "$powerstats" = "1 0 " ]; then
    echo "PASS: on while it answers, off in standby ($powerstats)"
    exit 0
fi
echo "FAIL: power status read '$powerstats', want '1 0 '"
printf '%s\n' "$output"
exit 1
