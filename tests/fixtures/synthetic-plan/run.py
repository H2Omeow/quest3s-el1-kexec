#!/usr/bin/env python3
"""Exercise the host planner with synthetic, non-device memory layouts."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "build/artifacts/Image.el1")
    parser.add_argument("--planner", type=Path, default=ROOT / "user/quest-el1")
    parser.add_argument("--dtc", default=shutil.which("dtc"))
    args = parser.parse_args()
    args.image = args.image.resolve()
    args.planner = args.planner.resolve()
    if not args.dtc:
        parser.error("dtc is required (install it or pass --dtc)")
    if not args.image.is_file():
        parser.error(f"Image not found: {args.image}; build it or pass --image")
    if not args.planner.is_file():
        parser.error(f"planner not found: {args.planner}; build it or pass --planner")

    with tempfile.TemporaryDirectory(prefix="quest-synthetic-plan-") as directory:
        work = Path(directory)
        dtbs = {}
        for name in ("synthetic-ram", "synthetic-reserved-kernel"):
            source = FIXTURE / f"{name}.dts"
            output = work / f"{name}.dtb"
            subprocess.run([args.dtc, "-I", "dts", "-O", "dtb", "-o",
                            str(output), str(source)], check=True)
            dtbs[name] = output

        common = [
            str(args.planner), "plan", "--image", str(args.image),
            "--kernel-base", "0x80000000", "--dtb-addr", "0x84000000",
            "--ram", "0x80000000:0x10000000",
            "--bootargs", "quest_el1.synthetic_validation=1",
        ]
        cases = [
            ("baseline synthetic layout", dtbs["synthetic-ram"],
             "0x8ff00000:0x100000", 0, "segment[0]"),
            ("operator reservation overlaps kernel", dtbs["synthetic-ram"],
             "0x80000000:0x400000", 1,
             "kernel Image overlaps a supplied or DT reserved range"),
            ("DT reservation overlaps kernel", dtbs["synthetic-reserved-kernel"],
             "0x8ff00000:0x100000", 1,
             "kernel Image overlaps a supplied or DT reserved range"),
            ("reservation overlaps DTB mapping block", dtbs["synthetic-ram"],
             "0x84100000:0x1000", 1,
             "DTB's 2 MiB mapping block overlaps reserved memory"),
        ]

        failed = False
        for label, dtb, reserve, expected_exit, expected_output in cases:
            result = subprocess.run(
                common + ["--dtb", str(dtb), "--reserve", reserve],
                cwd=ROOT, capture_output=True, text=True,
            )
            output = result.stdout + result.stderr
            passed = (result.returncode == expected_exit and
                      expected_output in output)
            print(f"{label}: {'PASS' if passed else 'FAIL'}")
            if not passed:
                print(output, end="")
                print(f"exit status: {result.returncode} (expected {expected_exit})")
                failed = True
        return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
