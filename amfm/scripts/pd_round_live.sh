#!/bin/bash
# AMFM #534: round the four unconverted dorsal PD barrels live, through the
# bridge, in a window the Owner can watch.  Works on a copy; nothing reaches
# disk until the Owner saves.  See notes/P3-live-shape-and-belly.md.
export PATH=/ucrt64/bin:/usr/bin:$PATH
OUT=${OUT:-/tmp}
D="/z/data_art/Ref_SF/voxels/Star Marine Trooper"
SRC="$D/StarMarineTrooper_andrew.gox"
DST="$D/StarMarineTrooper_andrew_cyl.gox"
CTL=/c/projects_tools/goxel-lab/labctl.py
PORT=${1:-47534}
cd "$(dirname "$0")/../.." || exit 1

cp -p "$SRC" "$DST" || exit 1
GOXEL_LAB_PORT=$PORT GOXEL_FULLSCREEN_AT=3840,956 \
  nohup ./goxel.exe "$DST" > "$OUT/live_editor.log" 2>&1 &
disown

for i in $(seq 1 80); do
    python $CTL -p $PORT ping > "$OUT/ping.txt" 2>&1 && grep -q pong "$OUT/ping.txt" && break
    sleep 0.5
done
python $CTL -p $PORT state > "$OUT/live_state_before.txt" 2>&1

while read -r x y z name; do
    [ -z "$x" ] && continue
    printf '%-20s ' "$name"
    python $CTL -p $PORT shape $x $y $z 0.45 $name | tr '\n' ' '
    echo
    sleep 0.9
done <<'EOF'
-23 7 8 PD_m1_barrel_L
-21 7 8 PD_m1_barrel_R
-6 17 10 PD_m2_barrel_L
-4 17 10 PD_m2_barrel_R
3 17 10 PD_m4_barrel_L
5 17 10 PD_m4_barrel_R
20 6 8 PD_m5_barrel_L
22 6 8 PD_m5_barrel_R
EOF

python $CTL -p $PORT state > "$OUT/live_state_after.txt" 2>&1
grep shape=1 "$OUT/live_state_after.txt"
