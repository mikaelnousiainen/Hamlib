#!/bin/sh
# Checks that messages which only trace what the programs do are not reported as errors: at the error
# level (-vv) none of them appears.

set -eu

failed=0

check() {
    description=$1
    output=$2
    pattern=$3

    if printf '%s\n' "$output" | grep -q -- "$pattern"; then
        echo "FAIL: $description"
        failed=1
    else
        echo "PASS: $description"
    fi
}

check "rigctl does not trace set_ptt as an error" "$(./rigctl -m 1 -vv T 0 2>&1)" "ptt="
# An integer, a float and a string parameter: get_parm traces the branch it takes for each type
for parm in BEEP BACKLIGHT BANDSELECT; do
    check "rigctl does not trace get_parm $parm as an error" "$(./rigctl -m 1 -vv p "$parm" 2>&1)" "rigctl_get_parm:"
done

# rigctld stopped by a signal: the signal interrupts select(), which is not an error
log=$(mktemp)
./rigctld -m 1 -t 0 -vv >"$log" 2>&1 &
rigctld=$!
sleep 1
kill -TERM "$rigctld"
wait "$rigctld" || true
check "rigctld does not report its stop as an error" "$(cat "$log")" "select() failed"
rm -f "$log"

# icom_get_ant through the simulated IC-7610, when the simulators are built
simulator=../simulators/simic7610
if [ -x "$simulator" ]; then
    log=$(mktemp)
    # The simulator names its terminal on standard output, which is buffered when redirected
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
        failed=1
    elif ! output=$(./rigctl -m 3078 -r "$pty" -s 115200 -vv y 1 2>&1); then
        echo "FAIL: get_ant on the simulated IC-7610 did not answer"
        printf '%s\n' "$output"
        failed=1
    else
        check "the Icom backend does not trace get_ant as an error" "$output" "ackbuf="
    fi
    kill "$simulator_pid" 2>/dev/null || true
    rm -f "$log"
else
    echo "SKIP: get_ant check, $simulator is not built"
fi

exit "$failed"
