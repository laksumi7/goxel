#!/bin/bash
# AMFM #534: capture the placement/orientation freedom on the real ship, then
# reopen the saved document from a separate process.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel
scons -j 20 mode=release werror=false > /tmp/place_build.log 2>&1
if [ $? -ne 0 ]; then grep -E 'error:|scons: \*\*\*' /tmp/place_build.log | head -20; echo BUILD_FAILED; exit 1; fi
cd /c/projects_tools/goxel-lab
mkdir -p evidence/placement
rm -f evidence/placement/place*.png evidence/placement/placement.gox

GOXEL_HEADLESS=1 GOXEL_PLACE_DEMO=1 GOXEL_SHOTS=/c/projects_tools/goxel-lab/evidence/placement \
  ./goxel/goxel.exe assets/vox1.vox > evidence/placement/placement_demo.log 2>&1
echo "placement demo exit=$?"
grep -E 'place [0-9]|region|layer [0-9]|placement: save|wrote' evidence/placement/placement_demo.log

GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=/c/projects_tools/goxel-lab/evidence/placement/reopened_placement.png \
  ./goxel/goxel.exe evidence/placement/placement.gox > evidence/placement/reopen_placement.log 2>&1
echo "reopen exit=$?"
ls -la evidence/placement/
