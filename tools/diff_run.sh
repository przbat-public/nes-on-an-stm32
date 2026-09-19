#!/bin/sh
# diff_run.sh — build and run the differential test (old core vs new core).
#
#   tools/diff_run.sh [instructions] [revision] [extra diff_test args...]
#
# The old core is taken from `revision` (default HEAD, the last commit
# before the macro change — the tree's own current commit) and staged into
# build/difftest/old_cpu_core/ with renamed globals, so both cores link
# into one binary. Pass `--current` as the revision to compare the working
# tree against itself (a smoke test of the harness).
#
#   tools/diff_run.sh 5000000            old = git HEAD, 5M instructions
#   tools/diff_run.sh 2000000 --current  self-check harness
#   tools/diff_run.sh 5000000 HEAD -v    progress lines
#   tools/diff_run.sh 5000000 HEAD seed=99   a different random stream
#
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
INS=${1:-5000000}
REV=${2:-HEAD}
BUILD="$ROOT/build/difftest"

cd "$ROOT"
python3 tools/diff_setup.py "$REV"

# The old core is compiled on its own with the renaming prefix forced in
# (-include), so the prefix never touches the harness's own declarations.
cc -O2 -Wall -Wextra -Isrc -I"$BUILD/old_cpu_core" -DNES_BUS_INLINE \
   -include "$BUILD/old_cpu_core/diff_prefix.h" \
   -c -o "$BUILD/old_cpu6502.o" "$BUILD/old_cpu_core/cpu6502.c"

cc -O2 -Wall -Wextra -Isrc -I"$BUILD" -I"$BUILD/old_cpu_core" -DNES_BUS_INLINE \
   -o "$BUILD/diff_test" \
   tools/diff_test.c "$BUILD/old_cpu6502.o" src/cpu6502.c

"$BUILD/diff_test" "$INS" "$@"
