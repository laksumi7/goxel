#!/bin/bash
# AMFM #534: capture the four shape kinds, then reopen the saved document.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel
scons -j 20 mode=release werror=false 2>&1 | grep -E 'error:|scons: \*\*\*' | head -20
cd /c/projects_tools/goxel-lab
rm -f evidence/shapes/shape*.png evidence/shapes/shapes.gox

GOXEL_HEADLESS=1 GOXEL_SHAPE_DEMO=1 GOXEL_SHOTS=/c/projects_tools/goxel-lab/evidence/shapes \
  ./goxel/goxel.exe assets/vox1.vox > evidence/shapes/shapes_demo.log 2>&1
echo "shapes demo exit=$?"
grep -E 'shape [0-9]|layer [0-9]|shapes: save|wrote' evidence/shapes/shapes_demo.log

GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=/c/projects_tools/goxel-lab/evidence/shapes/reopened_shapes.png \
  ./goxel/goxel.exe evidence/shapes/shapes.gox > evidence/shapes/reopen_shapes.log 2>&1
echo "reopen exit=$?"
ls -la evidence/shapes/*.png evidence/shapes/*.gox
