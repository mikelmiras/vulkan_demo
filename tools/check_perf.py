#!/usr/bin/env python3
"""Performance regression check: runs engine_bench a few times and compares its times with a baseline.

    check_perf.py --bench build/engine_bench --platform Linux [--runs 3] [--baseline tools/perf_baseline.json]
                  [--factor 2.0] [--write]

Each time is the best of the runs (the least disturbed by other work on the machine). A time fails when it is
more than `factor` times its baseline plus 1 ms (shared CI machines vary a lot: this catches big regressions, an
accidental O(n^2) or a lost cache, not 10 %). Times without a baseline only print. --write stores the results as
the platform's new baseline (after an intended change: run it on that platform, or copy the numbers CI prints).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile


def run_bench(bench, runs):
    best = {}
    for i in range(runs):
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "bench.json")
            subprocess.run([bench, "--json", out], check=True, stdout=subprocess.DEVNULL)
            with open(out) as f:
                times = json.load(f)["ms"]
        for name, ms in times.items():
            best[name] = min(best.get(name, ms), ms)
        print(f"run {i + 1}/{runs} done", flush=True)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--baseline", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "perf_baseline.json"))
    ap.add_argument("--factor", type=float, default=2.0)
    ap.add_argument("--slack", type=float, default=1.0, help="ms allowed on top of factor x baseline")
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    results = run_bench(args.bench, args.runs)
    baselines = {}
    if os.path.exists(args.baseline):
        with open(args.baseline) as f:
            baselines = json.load(f)
    base = baselines.get(args.platform, {})

    failed = []
    print(f"\n{'':24} {'now ms':>10} {'baseline':>10} {'limit':>10}")
    for name, ms in results.items():
        if name in base:
            limit = base[name] * args.factor + args.slack
            verdict = "SLOWER" if ms > limit else ""
            if verdict:
                failed.append(name)
            print(f"{name:24} {ms:10.3f} {base[name]:10.3f} {limit:10.3f} {verdict}")
        else:
            print(f"{name:24} {ms:10.3f} {'-':>10} {'-':>10} (no baseline)")
    print("\nThis run as a baseline:")
    print(json.dumps({args.platform: {k: round(v, 3) for k, v in results.items()}}, indent=2))

    if args.write:
        baselines[args.platform] = {k: round(v, 3) for k, v in results.items()}
        with open(args.baseline, "w") as f:
            json.dump(baselines, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"wrote {args.platform} to {args.baseline}")
        return 0
    if failed:
        print(f"\nPerformance regression: {', '.join(failed)} more than {args.factor}x the baseline (+{args.slack} ms)")
        return 1
    if not base:
        print(f"\n(no baseline for {args.platform} yet: nothing checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
