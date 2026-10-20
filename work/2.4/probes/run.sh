#!/bin/sh
# Runs every probe under the bootstrap and under antc (itself under the
# bootstrap) and reports SAME or DIFF per probe (stdout, exit status, first
# stderr line). A probe is a file NN-name.ant or a directory NN-name/ whose
# entry is main.ant. Every probe runs with the arguments alpha "b c" "" and
# with ANT_PROBE=1 in the environment, and both commands get the same
# absolute path, so load diagnostics compare. Run from the repository root:
# sh work/2.4/probes/run.sh [NN ...]
cd "$(dirname "$0")/../../.." || exit 1
D=work/2.4/probes
sel="$*"
for f in $D/[0-9][0-9]-*; do
  case "$f" in *.out|*.err) continue;; *.ant) src=$f;; *) src=$f/main.ant;; esac
  n=$(basename "$f" .ant)
  case "$sel" in "") ;; *) echo " $sel " | grep -q " ${n%%-*} " || continue;; esac
  abs="$PWD/$src"
  ANT_PROBE=1 python3 -I bootstrap/antpy.py run "$abs" alpha "b c" "" > "$D/$n.py.out" 2> "$D/$n.py.err"
  { echo "exit=$?"; head -1 "$D/$n.py.err"; } >> "$D/$n.py.out"
  ANT_PROBE=1 python3 -I bootstrap/antpy.py run src/antc/main.ant run "$abs" alpha "b c" "" > "$D/$n.c.out" 2> "$D/$n.c.err"
  { echo "exit=$?"; head -1 "$D/$n.c.err"; } >> "$D/$n.c.out"
  if diff -q "$D/$n.py.out" "$D/$n.c.out" > /dev/null; then echo "SAME $n"
  else echo "DIFF $n"; diff "$D/$n.py.out" "$D/$n.c.out" | sed 's/^/    /'; fi
done
