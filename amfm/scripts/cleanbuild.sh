#!/bin/bash
# AMFM #534: a fresh clean build from scratch after the out-of-bounds fixes, plus
# the targeted memory check. Recorded as evidence, not a new test campaign.
export PATH=/ucrt64/bin:/usr/bin:$PATH
LAB=/c/projects_tools/goxel-lab
cd $LAB/goxel || exit 1

echo "== clean =="
scons -c > /tmp/clean.log 2>&1
echo "clean exit=$?"
rm -f src/*.o src/**/*.o goxel.exe 2>/dev/null

echo "== build from scratch =="
scons -j 20 mode=release werror=false > $LAB/evidence/clean_build.log 2>&1
rc=$?
echo "build exit=$rc"
echo "compiler: $(gcc --version | head -1)"
echo "warnings: $(grep -c 'warning:' $LAB/evidence/clean_build.log)"
echo "errors:   $(grep -c 'error:' $LAB/evidence/clean_build.log)"
grep -E 'error:' $LAB/evidence/clean_build.log | head -5
# Anything from our own files is worth reading even if it is only a warning.
grep -E 'slope|gox\.c' $LAB/evidence/clean_build.log | grep -E 'warning:|error:' | head -10
ls -la goxel.exe

echo "== targeted memory check (heap validation + leak counter, 120 cycles) =="
cd $LAB || exit 1
mkdir -p evidence/stress
GOXEL_HEADLESS=1 GOXEL_SELFTEST=1 GOXEL_STRESS=120 \
  GOXEL_SELFTEST_SAVE=$LAB/evidence/stress/stress.gox \
  ./goxel/goxel.exe assets/vox1.vox > evidence/stress/cleanbuild_stress.log 2>&1
echo "stress exit=$?"
grep -E 'stress |self test' evidence/stress/cleanbuild_stress.log
grep -c '\[FAIL\]' evidence/stress/cleanbuild_stress.log
