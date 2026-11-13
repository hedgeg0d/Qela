#!/usr/bin/env python3
"""Compare compiler CPU/wall time on identical input in alternating order."""
import argparse
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--entry", default="srcql/main.qela")
    parser.add_argument("--target", default="x86_64")
    parser.add_argument("--runs", type=int, default=10)
    args = parser.parse_args()
    if args.runs < 2:
        parser.error("--runs must be at least 2")
    compilers = [args.before.resolve(), args.after.resolve()]
    samples = [[], []]
    sizes = [0, 0]
    with tempfile.TemporaryDirectory(prefix="qela-bench-") as tmp:
        output = Path(tmp) / "output"
        for iteration in range(args.runs + 1):
            order = [0, 1] if iteration % 2 == 0 else [1, 0]
            for index in order:
                start_cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
                start_wall = time.perf_counter()
                subprocess.run(
                    [str(compilers[index]), args.entry, "--target", args.target,
                     "-o", str(output)], check=True,
                    stdout=subprocess.DEVNULL)
                wall = time.perf_counter() - start_wall
                end_cpu = resource.getrusage(resource.RUSAGE_CHILDREN)
                cpu = (end_cpu.ru_utime + end_cpu.ru_stime
                       - start_cpu.ru_utime - start_cpu.ru_stime)
                sizes[index] = output.stat().st_size
                if iteration:
                    sample = {"wall": wall, "cpu": cpu}
                    samples[index].append(sample)
                    print(json.dumps({"run": iteration, "compiler": index,
                                      **sample}), flush=True)
    medians = [{key: statistics.median(row[key] for row in group)
                for key in ("wall", "cpu")} for group in samples]
    print(json.dumps({
        "compilers": [str(path) for path in compilers],
        "compiler_bytes": [os.stat(path).st_size for path in compilers],
        "output_bytes": sizes,
        "medians": medians,
        "after_over_before": {key: medians[1][key] / medians[0][key]
                              for key in ("wall", "cpu")},
    }, indent=2))


if __name__ == "__main__":
    main()
