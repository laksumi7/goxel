#!/bin/bash
# AMFM #534: stress the shape builders and the undo stack on the real asset.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel
scons -j 20 mode=release werror=false > /tmp/stress_build.log 2>&1
if [ $? -ne 0 ]; then grep -E 'error:|scons: \*\*\*' /tmp/stress_build.log | head -20; echo BUILD_FAILED; exit 1; fi
cd /c/projects_tools/goxel-lab
mkdir -p evidence/stress
N=${1:-40}
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 GOXEL_STRESS=$N \
  GOXEL_SELFTEST_SAVE=/c/projects_tools/goxel-lab/evidence/stress/stress.gox \
  ./goxel/goxel.exe assets/vox1.vox > evidence/stress/stress.log 2>&1
echo "exit=$?"
grep -E 'stress|FAIL|self test' evidence/stress/stress.log
