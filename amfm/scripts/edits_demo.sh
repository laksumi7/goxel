#!/bin/bash
# AMFM #534: one inside-region deletion and one recolour on the real ship,
# before/after visible, then the resolve path.
export PATH=/ucrt64/bin:/usr/bin:$PATH
cd /c/projects_tools/goxel-lab/goxel
scons -j 20 mode=release werror=false > /tmp/edit_build.log 2>&1
if [ $? -ne 0 ]; then grep -E 'error:|scons: \*\*\*' /tmp/edit_build.log | head -20; echo BUILD_FAILED; exit 1; fi
cd /c/projects_tools/goxel-lab
mkdir -p evidence/edits
rm -f evidence/edits/edit*.png evidence/edits/edits.gox

GOXEL_HEADLESS=1 GOXEL_EDIT_DEMO=1 GOXEL_SHOTS=/c/projects_tools/goxel-lab/evidence/edits \
  ./goxel/goxel.exe assets/vox1.vox > evidence/edits/edits_demo.log 2>&1
echo "edits demo exit=$?"
grep -E 'edit demo|layer [0-9]|edits: save|wrote' evidence/edits/edits_demo.log

GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=/c/projects_tools/goxel-lab/evidence/edits/reopened_edits.png \
  ./goxel/goxel.exe evidence/edits/edits.gox > evidence/edits/reopen_edits.log 2>&1
echo "reopen exit=$?"
ls -la evidence/edits/
