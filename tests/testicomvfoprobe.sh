#!/bin/sh
# Checks the Icom vfo_probe option: an IC-7300 cannot report whether VFO A or B is selected, so Hamlib
# assumes A and reads VFO A through the selected VFO. With vfo_probe=1 Hamlib finds the selected VFO
# when it opens the rig, addresses A and B correctly, and leaves the rig on the VFO it was on; while
# the rig transmits it is left alone.

set -u

simulator=../simulators/simic7300
if [ ! -x "$simulator" ]; then
    echo "SKIP: $simulator is not built"
    exit 0
fi

simulator_log=$(mktemp)
line_buffered=""
if command -v stdbuf >/dev/null 2>&1; then
    line_buffered="stdbuf -oL"
fi
$line_buffered "$simulator" unused >"$simulator_log" 2>&1 &
simulator_pid=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q "^name=" "$simulator_log" && break
    sleep 0.3
done
pty=$(sed -n 's/^name=//p' "$simulator_log" | head -1)

# The simulated rig has VFO A on 14074000 Hz and VFO B on 14074500 Hz
rigctl() {
    ./rigctl -m 3073 -r "$pty" -s 115200 "$@" 2>/dev/null | tr '\n' ' ' | sed 's/ $//'
}

failures=0
check() {
    if [ "$2" = "$3" ]; then
        echo "PASS: $1"
    else
        echo "FAIL: $1: got '$2', want '$3'"
        failures=$((failures + 1))
    fi
}

rigctl V VFOB >/dev/null
check "VFO B selected: VFO A and B read" "$(rigctl --set-conf=vfo_probe=1 -o f VFOA f VFOB)" \
    "14074000 14074500"
check "VFO B selected: the rig is still on VFO B after the probe" "$(rigctl f)" "14074500"

rigctl V VFOA >/dev/null
check "VFO A selected: VFO A and B read" "$(rigctl --set-conf=vfo_probe=1 -o f VFOA f VFOB)" \
    "14074000 14074500"
check "VFO A selected: the rig is still on VFO A after the probe" "$(rigctl f)" "14074000"

# While transmitting the rig is left alone: Hamlib still assumes VFO A
rigctl V VFOB T 1 >/dev/null
check "transmitting on VFO B: the probe is skipped" \
    "$(rigctl --set-conf=vfo_probe=1 -o f VFOA f VFOB)" "14074500 14074000"
rigctl T 0 >/dev/null

kill "$simulator_pid" 2>/dev/null
rm -f "$simulator_log"

if [ "$failures" -ne 0 ]; then
    exit 1
fi
exit 0
