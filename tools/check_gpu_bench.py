#!/usr/bin/env python3
"""GPU benchmark: runs vkgame --benchmark on scenes and compares the GPU's frame with this GPU's baseline.

    check_gpu_bench.py --game build/vkgame [--scene examples/night_drive] [--scene --stress] [--frames 200]
                       [--quality high] [--baseline tools/gpu_baselines.json] [--factor 1.5] [--write]

Each scene runs once with the camera turning a whole circle (vkgame --benchmark: the passes' medians). A GPU frame
fails when it's more than `factor` times the baseline stored for the same GPU (by the name the driver gives) and
scene; GPUs and scenes without a baseline only print. --write stores the results as this GPU's baseline (run it on
the machine whose numbers they are: a GTX 1050 Ti, an M4, ...).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile


def run(game, scene, frames, quality):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "bench.json")
        env = dict(os.environ, VKWORLD_QUALITY=quality, VKWORLD_SAVE_DIR=tmp)
        args = [game] + ([scene] if scene else []) + ["--benchmark", str(frames), "--benchmark-out", out]
        subprocess.run(args, env=env, check=True)
        with open(out) as f:
            return json.load(f)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--game", required=True)
    p.add_argument("--scene", action="append")
    p.add_argument("--frames", type=int, default=200)
    p.add_argument("--quality", default="high")
    p.add_argument("--baseline", default=os.path.join(os.path.dirname(__file__), "gpu_baselines.json"))
    p.add_argument("--factor", type=float, default=1.5)
    p.add_argument("--write", action="store_true")
    a = p.parse_args()
    scenes = a.scene or ["examples/night_drive", "--stress"]
    baselines = {}
    if os.path.exists(a.baseline):
        with open(a.baseline) as f:
            baselines = json.load(f)
    failed = False
    for scene in scenes:
        r = run(a.game, scene, a.frames, a.quality)
        gpu, key = r["gpu"], scene + " @" + a.quality
        base = baselines.get(gpu, {}).get(key)
        line = "%-40s %-28s %9.2f ms" % (gpu[:40], key, r["gpu_ms"])
        if base:
            limit = base["gpu_ms"] * a.factor
            slow = r["gpu_ms"] > limit
            failed = failed or slow
            line += "   baseline %9.2f   limit %9.2f%s" % (base["gpu_ms"], limit, "   SLOWER" if slow else "")
        print(line)
        for name, ms in r["passes"].items():
            print("    %-36s %9.2f" % (name, ms))
        if a.write:
            baselines.setdefault(gpu, {})[key] = {"gpu_ms": round(r["gpu_ms"], 3), "passes": r["passes"]}
    if a.write:
        with open(a.baseline, "w") as f:
            json.dump(baselines, f, indent=1, sort_keys=True)
            f.write("\n")
        print("wrote", a.baseline)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
