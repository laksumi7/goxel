#!/bin/bash
# AMFM #534: the whole canonical regression pass in one command.
#   build -> self test x3 -> self test + stress -> slope demo -> shapes demo
#   -> placement demo, each with its reopen from a separate process.
export PATH=/ucrt64/bin:/usr/bin:$PATH
LAB=/c/projects_tools/goxel-lab
cd $LAB/goxel || exit 1
scons -j 20 mode=release werror=false > /tmp/regress_build.log 2>&1
if [ $? -ne 0 ]; then
  grep -E 'error:|scons: \*\*\*' /tmp/regress_build.log | head -20
  echo BUILD_FAILED; exit 1
fi
echo "BUILD_OK"
cd $LAB || exit 1
mkdir -p evidence/shapes evidence/placement evidence/stress

for run in 1 2 3; do
  GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 GOXEL_SELFTEST_SAVE=$LAB/evidence/shapes/selftest.gox \
    ./goxel/goxel.exe assets/vox1.vox > "evidence/shapes/selftest_run${run}.log" 2>&1
  echo "selftest run $run exit=$? :: $(tail -1 evidence/shapes/selftest_run${run}.log)"
done

GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 GOXEL_STRESS=120 \
  GOXEL_SELFTEST_SAVE=$LAB/evidence/stress/stress.gox \
  ./goxel/goxel.exe assets/vox1.vox > evidence/stress/stress.log 2>&1
echo "stress exit=$? :: $(grep 'self test' evidence/stress/stress.log)"
grep -E '\[FAIL\]' evidence/stress/stress.log | head -5

rm -f evidence/frame*.png evidence/reopen.gox
GOXEL_HEADLESS=1 GOXEL_SLOPE_DEMO=1 GOXEL_SHOTS=$LAB/evidence \
  ./goxel/goxel.exe assets/vox1.vox > evidence/demo.log 2>&1
echo "slope demo exit=$?"
GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=$LAB/evidence/reopened.png \
  ./goxel/goxel.exe evidence/reopen.gox > evidence/reopen.log 2>&1
echo "slope reopen exit=$?"

rm -f evidence/shapes/shape*.png evidence/shapes/shapes.gox
GOXEL_HEADLESS=1 GOXEL_SHAPE_DEMO=1 GOXEL_SHOTS=$LAB/evidence/shapes \
  ./goxel/goxel.exe assets/vox1.vox > evidence/shapes/shapes_demo.log 2>&1
echo "shapes demo exit=$? :: $(grep -c '\[PASS\]' evidence/shapes/shapes_demo.log) pass lines"
GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=$LAB/evidence/shapes/reopened_shapes.png \
  ./goxel/goxel.exe evidence/shapes/shapes.gox > evidence/shapes/reopen_shapes.log 2>&1
echo "shapes reopen exit=$?"

rm -f evidence/placement/place*.png evidence/placement/placement.gox
GOXEL_HEADLESS=1 GOXEL_PLACE_DEMO=1 GOXEL_SHOTS=$LAB/evidence/placement \
  ./goxel/goxel.exe assets/vox1.vox > evidence/placement/placement_demo.log 2>&1
echo "placement demo exit=$?"
grep -E 'mesh [0-9]+ verts' evidence/placement/placement_demo.log
GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=$LAB/evidence/placement/reopened_placement.png \
  ./goxel/goxel.exe evidence/placement/placement.gox > evidence/placement/reopen_placement.log 2>&1
echo "placement reopen exit=$?"

rm -f evidence/edits/edit*.png evidence/edits/edits.gox
mkdir -p evidence/edits
GOXEL_HEADLESS=1 GOXEL_EDIT_DEMO=1 GOXEL_SHOTS=$LAB/evidence/edits \
  ./goxel/goxel.exe assets/vox1.vox > evidence/edits/edits_demo.log 2>&1
echo "inside-region edits demo exit=$?"
grep -E 'edit demo' evidence/edits/edits_demo.log
GOXEL_HEADLESS=1 GOXEL_SHOTS_ONCE=$LAB/evidence/edits/reopened_edits.png \
  ./goxel/goxel.exe evidence/edits/edits.gox > evidence/edits/reopen_edits.log 2>&1
echo "edits reopen exit=$?"

echo "--- summary ---"
for f in evidence/shapes/selftest_run1.log evidence/shapes/selftest_run2.log \
         evidence/shapes/selftest_run3.log evidence/stress/stress.log; do
  echo "$f :: $(grep 'self test' $f)"
done
ls evidence/placement/place*.png | wc -l | xargs echo "placement shots:"
