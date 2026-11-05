#!/bin/bash
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel || exit 1
scons -j 20 mode=release werror=false 2>&1 | grep -E 'error:|Error [0-9]|scons: \*\*\*|warning: unused' | head -40
echo "=== build exit ${PIPESTATUS[0]} ==="
ls -la goxel.exe 2>&1 | head -2
