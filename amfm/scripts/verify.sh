#!/bin/bash
# AMFM #534: rebuild and write the full evidence trail.
export PATH=/ucrt64/bin:/usr/bin:$PATH
LAB=/c/projects_tools/goxel-lab
cd $LAB/goxel || exit 1

{
  echo "AMFM #534 verification run: $(date)"
  echo "goxel commit: 33be5c8ed826105ab35746f893065e9ef00d3134 (v0.15.1-61-g33be5c8e)"
  echo "gcc: $(gcc --version | head -1)"
  echo "vox1.vox sha256: $(sha256sum $LAB/assets/vox1.vox | cut -c1-64)"
} > $LAB/evidence/run.txt

bash $LAB/build.sh > /tmp/build.log 2>&1
echo "build exit=$?" >> $LAB/evidence/run.txt
grep -Ei ' error|warning:' /tmp/build.log | head -10 >> $LAB/evidence/run.txt

rm -f $LAB/evidence/frame*.png $LAB/evidence/reopened.png $LAB/evidence/*.gox

# 1) Invariants on the real ship, including save/reopen.
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 \
GOXEL_SELFTEST_SAVE="$LAB/evidence/selftest.gox" \
  ./goxel.exe $LAB/assets/vox1.vox > $LAB/evidence/selftest.log 2>&1
echo "selftest exit=$? pass=$(grep -c '\[PASS\]' $LAB/evidence/selftest.log) fail=$(grep -c '\[FAIL\]' $LAB/evidence/selftest.log)" >> $LAB/evidence/run.txt

# 2) The workflow, rendered headlessly, and the document saved at the end.
GOXEL_HEADLESS=1 GOXEL_SLOPE_DEMO=1 GOXEL_SHOTS="$LAB/evidence" \
  timeout 120 ./goxel.exe $LAB/assets/vox1.vox > $LAB/evidence/demo.log 2>&1
echo "demo exit=$?" >> $LAB/evidence/run.txt

# 3) A separate process opens the saved document back: the modifier survives.
GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE="$LAB/evidence/reopened.png" \
  timeout 120 ./goxel.exe $LAB/evidence/reopen.gox > $LAB/evidence/reopen.log 2>&1
echo "reopen exit=$?" >> $LAB/evidence/run.txt

grep -E '^layer |demo region|demo:' $LAB/evidence/demo.log >> $LAB/evidence/run.txt
grep -E 'slope=' $LAB/evidence/reopen.log >> $LAB/evidence/run.txt 2>/dev/null
ls -la $LAB/evidence >> $LAB/evidence/run.txt
echo "DONE"
