#!/bin/sh
# Runs every probe under the bootstrap and under antc (itself under the
# bootstrap) and reports SAME or DIFF per probe (stdout, exit status, first
# stderr line). Run from the repository root: sh work/2.2/probes/run.sh [NN ...]
cd "$(dirname "$0")/../../.." || exit 1
D=work/2.2/probes
sel="$*"
for f in $D/[0-9][0-9]-*.ant; do
  n=$(basename "$f" .ant)
  case "$sel" in "") ;; *) echo " $sel " | grep -q " ${n%%-*} " || continue;; esac
  python3 -I bootstrap/antpy.py run "$f" > "$D/$n.py.out" 2> "$D/$n.py.err"
  { echo "exit=$?"; head -1 "$D/$n.py.err"; } >> "$D/$n.py.out"
  python3 -I bootstrap/antpy.py run src/antc/main.ant run "$f" > "$D/$n.c.out" 2> "$D/$n.c.err"
  { echo "exit=$?"; head -1 "$D/$n.c.err"; } >> "$D/$n.c.out"
  if diff -q "$D/$n.py.out" "$D/$n.c.out" > /dev/null; then echo "SAME $n"
  else echo "DIFF $n"; diff "$D/$n.py.out" "$D/$n.c.out" | sed 's/^/    /'; fi
done
