#!/bin/bash
# AMFM #534: full regression pass — battery x3, the original one-slope demo,
# and the shapes demo reopen.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab

for run in 1 2 3; do
  GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 \
    GOXEL_SELFTEST_SAVE=/c/projects_tools/goxel-lab/evidence/shapes/selftest.gox \
    ./goxel/goxel.exe assets/vox1.vox > "evidence/shapes/selftest_run${run}.log" 2>&1
  echo "selftest run $run exit=$? :: $(tail -1 evidence/shapes/selftest_run${run}.log)"
done

rm -f evidence/frame*.png evidence/reopen.gox
GOXEL_HEADLESS=1 GOXEL_SLOPE_DEMO=1 GOXEL_SHOTS=/c/projects_tools/goxel-lab/evidence \
  ./goxel/goxel.exe assets/vox1.vox > evidence/demo.log 2>&1
echo "slope demo exit=$?"
grep -E 'demo region|demo: saved|layer [0-9]' evidence/demo.log

GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=/c/projects_tools/goxel-lab/evidence/reopened.png \
  ./goxel/goxel.exe evidence/reopen.gox > evidence/reopen.log 2>&1
echo "slope reopen exit=$?"
ls evidence/frame*.png evidence/reopened.png
