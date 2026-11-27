#!/usr/bin/env bash
# Per-thread CPU time of a running game, for A/B of the work done by each thread over the same QA replay.
#   tools/linux_diag/thread_cpu.sh [PID] > out.txt   (default: the running "Corruption_3 PC")
# Every 5 s prints, per thread, its name and its cumulative CPU seconds (utime+stime). The last block is the
# total of the run; the busiest thread with the game's name is the game thread.
set -euo pipefail
pid=${1:-}
while [[ -z $pid ]]; do pid=$(pgrep -f '(^|/)Corruption_3 PC --' | head -1 || true); [[ -n $pid ]] || sleep 0.5; done
tick=$(getconf CLK_TCK)
echo "# pid $pid clk_tck $tick"
while kill -0 "$pid" 2>/dev/null; do
    echo "t $(date +%s)"
    for stat in /proc/$pid/task/*/stat; do
        # comm may contain spaces: take the text between the parentheses, fields after it.
        line=$(cat "$stat" 2>/dev/null) || continue
        comm=${line#*(}; comm=${comm%)*}
        rest=${line##*) }
        read -r -a f <<< "$rest"
        tid=${stat#/proc/$pid/task/}; tid=${tid%/stat}
        echo "$tid $(( (${f[11]} + ${f[12]}) )) $comm"
    done
    sleep 5
done
