#!/bin/bash
# AMFM #534: rebuild and run the full battery, showing build errors first.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel
scons -j 20 mode=release werror=false 2>&1 | grep -E 'error:|scons: \*\*\*' | head -20
echo "BUILD_DONE rc=${PIPESTATUS[0]}"
bash /c/projects_tools/goxel-lab/selftest_shapes.sh
