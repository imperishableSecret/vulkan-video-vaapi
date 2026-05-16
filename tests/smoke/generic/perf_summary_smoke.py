#!/usr/bin/env python3

import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: perf_summary_smoke.py <lifetime-smoke-binary>", file=sys.stderr)
        return 2

    env = os.environ.copy()
    env["VKVV_PERF"] = "1"
    result = subprocess.run([sys.argv[1]], env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    output = result.stdout + result.stderr
    if result.returncode != 0:
        print(output, file=sys.stderr)
        return result.returncode
    if "nvidia-vulkan-vaapi: perf " not in output:
        print("missing VKVV_PERF summary output", file=sys.stderr)
        print(output, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
