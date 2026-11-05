#!/bin/bash
# AMFM #532 A3-P0 bounded proof against real Windows editor instances.
#
#   bash amfm/scripts/p2_proof.sh <docA> [docB]
#
# Prints PASS/FAIL per check and exits non-zero if anything fails. It opens and
# closes its own editor windows; it never touches an editor it did not start.
export PATH=/ucrt64/bin:/usr/bin:$PATH
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
cd "$REPO" || exit 1
OUT=${P2_OUT:-/tmp/p2}
PORT_A=${P2_PORT_A:-47534}
PORT_B=${P2_PORT_B:-47535}
DOC_A=${1:?usage: p2_proof.sh <docA> [docB]}
DOC_B=$2
mkdir -p "$OUT"

CTL="python $HERE/labctl.py"
[ -n "${P2_EXE:-}" ] && EXE="$P2_EXE" || EXE=./goxel.exe
fails=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
ctl() { local p="$1"; shift; $CTL -p "$p" "$@" 2>&1; }
cap() { powershell -NoProfile -ExecutionPolicy Bypass -File "$HERE/cap_pid.ps1" \
        -ProcId "$1" -Out "$2" 2>&1 | tail -1; }

if [ -n "$(ss -ltn 2>/dev/null | grep -E ":$PORT_A|:$PORT_B")" ]; then
    echo "FAIL ports $PORT_A/$PORT_B already in use"; exit 1
fi

echo "=== launch A ($DOC_A) on $PORT_A ==="
GOXEL_LAB_PORT=$PORT_A GOXEL_FULLSCREEN_AT=${P2_WINPOS:-3840,956} \
  ./goxel.exe "$DOC_A" > "$OUT/bridgeA.log" 2>&1 &
A=$!
for i in $(seq 1 60); do grep -q listening "$OUT/bridgeA.log" 2>/dev/null && break; sleep 0.5; done
grep 'lab bridge' "$OUT/bridgeA.log"
AW=$(ctl $PORT_A ping | sed -n 's/^pong pid=\([0-9]*\).*/\1/p')
[ -n "$AW" ] && pass "connect to the live editor (windows pid $AW)" || fail "no bridge answer"

echo "--- state ---"
ctl $PORT_A state > "$OUT/state_before.txt" 2>&1
grep -q '^ok$' "$OUT/state_before.txt" && pass "read live state" || fail "state"
grep -q '^doc ' "$OUT/state_before.txt" || fail "state has no doc"
cap $AW "$OUT/p2_1_before.png" >/dev/null

echo "--- one bounded edit ---"
BEFORE=$(grep -o "cells=[0-9]*" "$OUT/state_before.txt" | tail -1 | cut -d= -f2)
R=$(ctl $PORT_A set ${P2_POS:-4 -4 12}); echo "$R"
echo "$R" | grep -q 'history pushed' && pass "edit applied through the undo history" || fail "edit"
sleep 1.5; cap $AW "$OUT/p2_2_edited.png" >/dev/null

echo "--- undo ---"
ctl $PORT_A undo
sleep 1.5; cap $AW "$OUT/p2_3_undone.png" >/dev/null
ctl $PORT_A state > "$OUT/state_after.txt" 2>&1
AFTER=$(grep -o "cells=[0-9]*" "$OUT/state_after.txt" | tail -1 | cut -d= -f2)
[ "$BEFORE" = "$AFTER" ] && pass "undo restored the cell count ($BEFORE)" \
    || fail "undo left $BEFORE -> $AFTER"

echo "--- guards ---"
ctl $PORT_A del ${P2_POS:-4 -4 12} | grep -q noop && pass "delete on an empty cell is a noop" || fail "noop guard"
ctl $PORT_A bogus | grep -q '^err' && pass "unknown verb rejected" || fail "verb guard"

if [ -n "$DOC_B" ]; then
    echo "=== second instance B ($DOC_B) on $PORT_B ==="
    GOXEL_LAB_PORT=$PORT_B GOXEL_FULLSCREEN_AT=${P2_WINPOS:-3840,956} \
      ./goxel.exe "$DOC_B" > "$OUT/bridgeB.log" 2>&1 &
    B=$!
    for i in $(seq 1 60); do grep -q listening "$OUT/bridgeB.log" 2>/dev/null && break; sleep 0.5; done
    PA=$(ctl $PORT_A ping | sed -n 's/^pong pid=\([0-9]*\).*/\1/p')
    PB=$(ctl $PORT_B ping | sed -n 's/^pong pid=\([0-9]*\).*/\1/p')
    [ -n "$PA" ] && [ -n "$PB" ] && [ "$PA" != "$PB" ] \
        && pass "two editors answer separately (A=$PA B=$PB)" || fail "isolation"
    DA=$(ctl $PORT_A state | sed -n 's/^doc //p' | head -1)
    DB=$(ctl $PORT_B state | sed -n 's/^doc //p' | head -1)
    [ "$DA" != "$DB" ] && pass "each reports its own document" || fail "same doc reported"
    ctl $PORT_B set 0 0 0 >/dev/null
    DA2=$(ctl $PORT_A state | sed -n 's/^doc //p' | head -1)
    [ "$DA" = "$DA2" ] && pass "editing B does not disturb A" || fail "cross-talk"
fi

echo "=== rude clients must not stall the editor ==="
python - <<PY 2>&1 | tail -1
import socket
s = socket.create_connection(("127.0.0.1", $PORT_A), timeout=5)
s.sendall(b"\x00\xff junk\n"); s.close()
s2 = socket.create_connection(("127.0.0.1", $PORT_A), timeout=5)
s2.shutdown(socket.SHUT_WR); s2.close()
print("rude clients done")
PY
sleep 1
ctl $PORT_A ping | grep -q pong && pass "editor still responsive after bad clients" || fail "stalled"

echo "=== cleanup (only the instances this script started) ==="
[ -n "${B:-}" ] && kill $B 2>/dev/null
kill $A 2>/dev/null; sleep 2
kill -0 $A 2>/dev/null && kill -9 $A 2>/dev/null
[ -n "${B:-}" ] && { kill -0 $B 2>/dev/null && kill -9 $B 2>/dev/null; }
echo "evidence in $OUT"
echo "== $fails failed =="
[ $fails -eq 0 ]
