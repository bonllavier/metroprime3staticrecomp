#!/usr/bin/env bash
# Usage: watchdog.sh PID LOG — samples memory every second; kills PID if MemAvailable < 1500 MB.
pid=$1; log=$2
while kill -0 "$pid" 2>/dev/null; do
  avail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)
  swap=$(awk '/SwapTotal/{t=$2}/SwapFree/{f=$2}END{print int((t-f)/1024)}' /proc/meminfo)
  rss=$(awk '/VmRSS/{print int($2/1024)}' /proc/$pid/status 2>/dev/null)
  thr=$(ls /proc/$pid/task 2>/dev/null | wc -l)
  echo "$(date +%T) avail=${avail}M swap=${swap}M rss=${rss}M threads=$thr load=$(cut -d' ' -f1 /proc/loadavg)" >> "$log"
  if (( avail < 1500 )); then echo "$(date +%T) KILL: low memory" >> "$log"; kill -9 "$pid"; fi
  sleep 1
done
echo "$(date +%T) game exited" >> "$log"
