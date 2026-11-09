#!/bin/bash
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel || exit 1
export GOXEL_HEADLESS=1
export GOXEL_SELFTEST=1
export GOXEL_SELFTEST_SAVE='C:/projects_tools/goxel-lab/evidence/selftest.gox'
./goxel.exe /c/projects_tools/goxel-lab/assets/vox1.vox 2>&1 | tail -40
echo "=== exit $? ==="
