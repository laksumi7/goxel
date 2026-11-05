#!/bin/bash
# AMFM #534: full invariant battery on the real ship, all four shape kinds.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab
mkdir -p evidence/shapes
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 \
  GOXEL_SELFTEST_SAVE=/c/projects_tools/goxel-lab/evidence/shapes/selftest.gox \
  ./goxel/goxel.exe assets/vox1.vox > evidence/shapes/selftest.log 2>&1
echo "exit=$?"
grep -E '^(PASS|FAIL|== |suggested|model bbox)' evidence/shapes/selftest.log
