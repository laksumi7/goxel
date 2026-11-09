#!/bin/bash
# AMFM #534 P0 — a group transform must take generated surfaces with it.
#
#   bash amfm/scripts/move_proof.sh
#
# Fixtures are safe copies; the originals and any open editor window are never
# touched.  Override with FIXTURES=<dir> EVIDENCE=<dir> if they live elsewhere.
export PATH=/ucrt64/bin:/usr/bin:$PATH
LAB=${LAB:-/c/projects_tools/goxel-lab}
F=${FIXTURES:-$LAB/fixtures}
OUT=${EVIDENCE:-$LAB/goxel/amfm/evidence/move}
CMP=$(dirname "${BASH_SOURCE[0]}")/compare_dumps.py
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1

run_case() {
    local name=$1 file=$2 region=$3 dx=$4 dy=$5 dz=$6
    local d=$OUT/$name
    rm -rf "$d"; mkdir -p "$d"
    echo "=== $name : region $region offset $dx,$dy,$dz ==="
    # pass 1: move, save, and let the shot capture the moved state
    GOXEL_HEADLESS=1 GOXEL_MOVE_TEST="$region,$dx,$dy,$dz" GOXEL_MOVE_TEST_DUMP="$d" \
      GOXEL_MOVE_TEST_SAVE="$d/moved.gox" GOXEL_SHOTS_ONCE="$d/moved.png" \
      GOXEL_SEL_DUMP="$d/sel.txt" GOXEL_CELL_DUMP="$d/open.txt" \
      ./goxel.exe "$file" > "$d/log.txt" 2>&1
    # pass 2: the same move, then undo, to prove one coherent undo step
    GOXEL_HEADLESS=1 GOXEL_MOVE_TEST="$region,$dx,$dy,$dz" GOXEL_MOVE_TEST_DUMP="$d" \
      GOXEL_MOVE_TEST_UNDO=1 GOXEL_SHOTS_ONCE="$d/undo.png" \
      ./goxel.exe "$file" >> "$d/log.txt" 2>&1
    grep -E 'move test' "$d/log.txt"
    echo "--- cells the grab takes (inside the region) ---"
    awk -v R="$region" 'BEGIN{split(R,a,",")} $1=="cell" && $3>=a[1] && $3<=a[4] && $4>=a[2] && $4<=a[5] && $5>=a[3] && $5<=a[6] {print "  ", $0}' "$d/before.txt" | head -16
    python -u "$CMP" "$d/before.txt" "$d/after.txt" --region "$region" \
        --dx $dx --dy $dy --dz $dz --undo "$d/undo.txt"
    echo "--- reopen the saved result (persistence) ---"
    GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE="$d/reopen.png" GOXEL_SEL_DUMP="$d/reopen_sel.txt" \
      GOXEL_CELL_DUMP="$d/reopen.txt" ./goxel.exe "$d/moved.gox" > /dev/null 2>&1
    python -u "$CMP" "$d/before.txt" "$d/reopen.txt" --region "$region" \
        --dx $dx --dy $dy --dz $dz --reopen
    grep 'aabb=' "$d/before.txt" | sed "s/^/$name before /"
    grep 'aabb=' "$d/after.txt"  | sed "s/^/$name after  /"
}

rotate_case() {
    local name=$1 file=$2 region=$3 axis=$4
    local d=$OUT/$name
    rm -rf "$d"; mkdir -p "$d"
    echo "=== $name : region $region rotate 90 about $axis ==="
    GOXEL_HEADLESS=1 GOXEL_MOVE_TEST="$region,0,0,0" GOXEL_MOVE_TEST_ROT=$axis \
      GOXEL_MOVE_TEST_DUMP="$d" GOXEL_MOVE_TEST_SAVE="$d/rotated.gox" \
      GOXEL_SHOTS_ONCE="$d/rotated.png" GOXEL_SEL_DUMP="$d/sel.txt" \
      ./goxel.exe "$file" > "$d/log.txt" 2>&1
    grep -E 'move test' "$d/log.txt"
    python -u "$CMP" "$d/before.txt" "$d/after.txt" --region "$region" --rot $axis
}

legacy_case() {
    local name=$1 file=$2 region=$3 dx=$4 dy=$5 dz=$6
    local d=$OUT/$name
    rm -rf "$d"; mkdir -p "$d"
    echo "=== $name : the defect, old Move path — region $region offset $dx,$dy,$dz ==="
    GOXEL_HEADLESS=1 GOXEL_MOVE_TEST="$region,$dx,$dy,$dz" GOXEL_MOVE_TEST_LEGACY=1 \
      GOXEL_MOVE_TEST_DUMP="$d" GOXEL_SHOTS_ONCE="$d/broken.png" \
      ./goxel.exe "$file" > "$d/log.txt" 2>&1
    grep -E 'move test' "$d/log.txt"
    echo "    (a FAIL here is the defect: the body moved and the surfaces did not)"
    python -u "$CMP" "$d/before.txt" "$d/after.txt" --region "$region" \
        --dx $dx --dy $dy --dz $dz
}

pcase() {
    local d=$OUT/partial
    rm -rf "$d"; mkdir -p "$d"
    echo "=== partial : a shape owns 2 cells, only 1 is selected, offset 0,0,2 ==="
    GOXEL_HEADLESS=1 GOXEL_MOVE_TEST="3,-8,9,3,-8,9,0,0,2" GOXEL_MOVE_TEST_GROW=1,0,0 \
      GOXEL_MOVE_TEST_DUMP="$d" GOXEL_MOVE_TEST_SAVE="$d/moved.gox" \
      GOXEL_SHOTS_ONCE="$d/moved.png" GOXEL_SEL_DUMP="$d/sel.txt" \
      ./goxel.exe "$F/fixture_pd.gox" > "$d/log.txt" 2>&1
    GOXEL_HEADLESS=1 GOXEL_MOVE_TEST="3,-8,9,3,-8,9,0,0,2" GOXEL_MOVE_TEST_GROW=1,0,0 \
      GOXEL_MOVE_TEST_DUMP="$d" GOXEL_MOVE_TEST_UNDO=1 GOXEL_SHOTS_ONCE="$d/undo.png" \
      ./goxel.exe "$F/fixture_pd.gox" >> "$d/log.txt" 2>&1
    grep -E 'move test' "$d/log.txt"
    python -u "$CMP" "$d/before.txt" "$d/after.txt" \
        --region "3,-8,9,3,-8,9" --dx 0 --dy 0 --dz 2 --undo "$d/undo.txt"
}

# 0. reproduce the defect first: the same fixtures through the old Move path
legacy_case broken_ship "$F/fixture_ship.gox" "3,-8,8,5,-4,10" 0 0 4
legacy_case broken_pd   "$F/fixture_pd.gox"   "3,-8,8,5,-4,10" 6 0 3
# 1. the standalone PD: body + two round barrels, each on its own layer
run_case pd   "$F/fixture_pd.gox"   "3,-8,8,5,-4,10" 6 0 3
# 2. the same turret mounted on the ship (its body cells live on the ship layer)
run_case ship "$F/fixture_ship.gox" "3,-8,8,5,-4,10" 0 0 4
# 3. a shape only partly inside the selection still moves as one object
pcase
# 4. rotate the assembly 90 degrees about Z
rotate_case rot "$F/fixture_pd.gox" "3,-8,8,5,-4,10" z
