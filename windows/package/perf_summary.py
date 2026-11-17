# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 G30vamv and the Caravan4URecomp contributors. Part of a MODIFIED version of 4u-native by khorne-hub.
#
# Summarises the performance log the game writes (the CSV of "options = --perf-log ..." in Caravan4URecomp.ini).
#   python perf_summary.py Caravan4URecomp-perf.csv
import statistics
import sys

rows = []
with open(sys.argv[1], encoding="utf-8") as f:
    for line in f:
        if line.startswith("#") or line.startswith("frame"):
            continue
        p = line.strip().split(",")
        if len(p) >= 8:
            rows.append([float(x) for x in p[:8]])
rows = rows[1:]  # the first row holds the boot
n = len(rows)
if n == 0:
    sys.exit("no frames in the log")
present = [r[1] for r in rows]
guest = [r[3] for r in rows]
idle = [r[6] for r in rows]
svc = [r[4] for r in rows]
service = [r[5] for r in rows]
total = sum(present)
s = sorted(present)
print(f"frames logged: {n}, covers {total / 1e6:.1f} s")
print(f"mean frame interval: {statistics.mean(present) / 1000:.2f} ms  (= {1e6 / statistics.mean(present):.1f} fps)")
print(f"interval p50 {s[n // 2] / 1000:.2f} ms, p95 {s[int(n * 0.95)] / 1000:.2f} ms, p99 {s[int(n * 0.99)] / 1000:.2f} ms, max {s[-1] / 1000:.2f} ms")
print(f"CPU share: game code {100 * sum(guest) / total:.1f} %, system calls {100 * sum(svc) / total:.1f} %, "
      f"services {100 * sum(service) / total:.1f} %, idle {100 * sum(idle) / total:.1f} %")
late = sum(1 for x in present if x > 20000)
print(f"frames longer than 20 ms: {late} ({100 * late / n:.1f} %)")
