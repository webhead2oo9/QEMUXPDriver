#!/usr/bin/env python3
"""Measure isolated and visible-bounded black runs in a frontend BMP ring."""

import argparse
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RING_NAME = re.compile(r"frame-ring-([0-9]+)\.bmp$")


def bmp_dark_percent(path, sample_step=16):
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    declared_size = struct.unpack_from("<I", data, 2)[0]
    offset = struct.unpack_from("<I", data, 10)[0]
    dib_size = struct.unpack_from("<I", data, 14)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    planes = struct.unpack_from("<H", data, 26)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    image_size = struct.unpack_from("<I", data, 34)[0]
    if dib_size < 40 or 14 + dib_size > len(data):
        raise ValueError(f"{path}: unsupported DIB header")
    if offset < 14 + dib_size or offset > len(data):
        raise ValueError(f"{path}: invalid pixel offset")
    if planes != 1 or bpp != 32 or compression != 0:
        raise ValueError(f"{path}: expected uncompressed 32-bit BMP")
    if width <= 0 or height == 0 or width > 16384 or abs(height) > 16384:
        raise ValueError(f"{path}: invalid dimensions")
    rows = abs(height)
    row_bytes = width * 4
    pixel_bytes = row_bytes * rows
    if (image_size not in (0, pixel_bytes) or
            offset + pixel_bytes != len(data) or declared_size != len(data)):
        raise ValueError(f"{path}: inconsistent or truncated pixels")
    dark = 0
    count = 0
    for y in range(0, rows, sample_step):
        row = offset + y * row_bytes
        for x in range(0, width, sample_step):
            pixel = data[row + x * 4:row + x * 4 + 3]
            dark += max(pixel) < 8
            count += 1
    return 100.0 * dark / count


def classify(dark_percent):
    if dark_percent >= 99.0:
        return "B"
    if dark_percent >= 90.0:
        return "D"
    return "V"


def class_metrics(classes):
    isolated = sum(classes[index] == "B" and
                   classes[index - 1] == "V" and
                   classes[index + 1] == "V"
                   for index in range(1, len(classes) - 1))
    runs = []
    start = 0
    for index in range(1, len(classes) + 1):
        if index == len(classes) or classes[index] != classes[start]:
            runs.append({"start": start, "end": index - 1,
                         "class": classes[start], "length": index - start})
            start = index
    interior_black_runs = sum(
        run["class"] == "B" and run["start"] > 0 and
        run["end"] + 1 < len(classes) and
        classes[run["start"] - 1] == "V" and
        classes[run["end"] + 1] == "V"
        for run in runs)
    return {
        "visibleFrames": classes.count("V"),
        "blackFrames": classes.count("B"),
        "darkFrames": sum(value != "V" for value in classes),
        "isolatedBlackFrames": isolated,
        "interiorBlackRuns": interior_black_runs,
        "vvbWindows": sum(classes[index:index + 3] == "VVB"
                          for index in range(len(classes) - 2)),
        "bvvWindows": sum(classes[index:index + 3] == "BVV"
                          for index in range(len(classes) - 2)),
        "runs": runs,
    }


def analyze(directory):
    frames = []
    for path in Path(directory).glob("frame-ring-*.bmp"):
        match = RING_NAME.fullmatch(path.name)
        if not match:
            continue
        frames.append((path.stat().st_mtime_ns, int(match.group(1)),
                       path.name, bmp_dark_percent(path)))
    frames.sort(key=lambda item: (item[0], item[1]))
    if not frames:
        raise ValueError(f"{directory}: no numeric frame-ring BMPs")
    if any(frames[index][0] == frames[index - 1][0]
           for index in range(1, len(frames))):
        raise ValueError(f"{directory}: ambiguous tied frame mtimes")
    classes = "".join(classify(item[3]) for item in frames)
    metrics = class_metrics(classes)
    result = {
        "schema": 1,
        "kind": "xpdriver-frame-ring-black-cadence",
        "directory": str(Path(directory).resolve()),
        "frames": len(frames),
        "durationNanoseconds": frames[-1][0] - frames[0][0],
        "durationSeconds": round((frames[-1][0] - frames[0][0]) / 1e9, 6),
        "classes": classes,
        "frameDetails": [
            {"index": index, "slot": item[1], "file": item[2],
             "darkPercent": round(item[3], 3),
             "class": classes[index]}
            for index, item in enumerate(frames)
        ],
    }
    result.update(metrics)
    return result


def write_solid_bmp(path, value, width=32, height=32):
    row = bytes((value, value, value, 0)) * width
    write_bmp(path, row * height, width, height)


def write_bmp(path, pixels, width, height):
    header = bytearray(54)
    header[:2] = b"BM"
    struct.pack_into("<I", header, 2, 54 + len(pixels))
    struct.pack_into("<I", header, 10, 54)
    struct.pack_into("<I", header, 14, 40)
    struct.pack_into("<i", header, 18, width)
    struct.pack_into("<i", header, 22, height)
    struct.pack_into("<H", header, 26, 1)
    struct.pack_into("<H", header, 28, 32)
    struct.pack_into("<I", header, 34, len(pixels))
    path.write_bytes(header + pixels)


def write_sampled_bmp(path, dark_samples):
    width = 160
    height = 160
    pixels = bytearray(bytes((40, 40, 40, 0)) * width * height)
    for sample in range(dark_samples):
        x = (sample % 10) * 16
        y = (sample // 10) * 16
        offset = (y * width + x) * 4
        pixels[offset:offset + 3] = b"\0\0\0"
    write_bmp(path, pixels, width, height)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def expect_value_error(path):
    try:
        bmp_dark_percent(path)
    except ValueError:
        return
    raise RuntimeError(f"malformed BMP accepted: {path}")


def self_test():
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        slots = (14, 15, 0, 1, 2, 3, 4)
        values = (40, 40, 0, 40, 40, 0, 40)
        for index, (slot, value) in enumerate(zip(slots, values)):
            path = root / f"frame-ring-{slot}.bmp"
            write_solid_bmp(path, value)
            stamp = 1_700_000_000_000_000_000 + index * 1_000_000
            os.utime(path, ns=(stamp, stamp))
        write_solid_bmp(root / "frame-ring-tmp.bmp", 0)
        result = analyze(root)
        require(result["classes"] == "VVBVVBV", "class sequence mismatch")
        require([item["slot"] for item in result["frameDetails"]] ==
                list(slots), "mtime wrap chronology mismatch")
        require(result["isolatedBlackFrames"] == 2,
                "isolated black count mismatch")
        require(result["interiorBlackRuns"] == 2,
                "interior black-run count mismatch")
        require(result["vvbWindows"] == 2 and result["bvvWindows"] == 1,
                "window count mismatch")
        require(result["runs"][1] ==
                {"start": 2, "end": 2, "class": "B", "length": 1},
                "run record mismatch")

        tied = root / "tied"
        tied.mkdir()
        for slot in (10, 2):
            path = tied / f"frame-ring-{slot}.bmp"
            write_solid_bmp(path, 40)
            os.utime(path, ns=(1_700_000_000_000_000_000,) * 2)
        try:
            analyze(tied)
        except ValueError:
            pass
        else:
            raise RuntimeError("ambiguous tied mtimes were accepted")

        thresholds = root / "thresholds"
        thresholds.mkdir()
        expected = ((89, "V"), (90, "D"), (98, "D"), (99, "B"))
        for slot, (dark_samples, expected_class) in enumerate(expected):
            path = thresholds / f"frame-ring-{slot}.bmp"
            write_sampled_bmp(path, dark_samples)
            actual = classify(bmp_dark_percent(path))
            require(actual == expected_class,
                    f"threshold {dark_samples}% classified {actual}")

        multi_black = root / "multi-black-run"
        multi_black.mkdir()
        for index, value in enumerate((40, 40, 0, 0, 40, 40)):
            path = multi_black / f"frame-ring-{index}.bmp"
            write_solid_bmp(path, value)
            stamp = 1_700_000_100_000_000_000 + index * 1_000_000
            os.utime(path, ns=(stamp, stamp))
        multi_result = analyze(multi_black)
        require(multi_result["classes"] == "VVBBVV" and
                multi_result["isolatedBlackFrames"] == 0 and
                multi_result["interiorBlackRuns"] == 1,
                "multi-frame interior black-run calibration mismatch")
        command = [sys.executable, str(Path(__file__).resolve())]
        multi_isolated_only = subprocess.run(
            command + [str(multi_black), "--max-isolated-black", "0",
                       "--min-frames", "6", "--min-duration-seconds",
                       "0.004"], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, check=False)
        multi_interior_gate = subprocess.run(
            command + [str(multi_black), "--max-isolated-black", "0",
                       "--max-interior-black-runs", "0", "--min-frames",
                       "6", "--min-duration-seconds", "0.004"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False)
        require(multi_isolated_only.returncode == 0 and
                multi_interior_gate.returncode == 1,
                "multi-frame black-run CLI sensitivity mismatch")

        visibility_dirs = []
        for label, classes in (("all-black", "BBBBBB"),
                               ("all-dark", "DDDDDD"),
                               ("edge-black", "BVVVVV")):
            capture = root / label
            capture.mkdir()
            for index, value in enumerate(classes):
                path = capture / f"frame-ring-{index}.bmp"
                if value == "D":
                    write_sampled_bmp(path, 95)
                else:
                    write_solid_bmp(path, 0 if value == "B" else 40)
                stamp = 1_700_000_200_000_000_000 + index * 1_000_000
                os.utime(path, ns=(stamp, stamp))
            visibility_dirs.append(capture)
        visibility_args = ["--max-isolated-black", "0",
                           "--max-interior-black-runs", "0",
                           "--min-visible-frames", "4", "--min-frames", "6",
                           "--min-duration-seconds", "0.004"]
        all_black_exit = subprocess.run(
            command + [str(visibility_dirs[0])] + visibility_args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False)
        all_dark_exit = subprocess.run(
            command + [str(visibility_dirs[1])] + visibility_args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False)
        edge_black_exit = subprocess.run(
            command + [str(visibility_dirs[2])] + visibility_args,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False)
        require(all_black_exit.returncode == 1 and
                all_dark_exit.returncode == 1 and
                edge_black_exit.returncode == 0,
                "visibility gate did not reject sustained black/dark evidence")

        valid = root / "valid.bmp"
        write_solid_bmp(valid, 40)
        malformed = []
        mutations = ((10, "<I", 0), (14, "<I", 12),
                     (26, "<H", 2), (30, "<I", 1))
        for index, (offset, fmt, value) in enumerate(mutations):
            path = root / f"malformed-{index}.bmp"
            data = bytearray(valid.read_bytes())
            struct.pack_into(fmt, data, offset, value)
            path.write_bytes(data)
            malformed.append(path)
        truncated = root / "malformed-truncated.bmp"
        truncated.write_bytes(valid.read_bytes()[:-1])
        malformed.append(truncated)
        for path in malformed:
            expect_value_error(path)

        baseline = "VVB" * 50 + "VV" + "BBBB" + "DD" + "V" * 98
        candidate = "V" * 163 + "BBB" + "D" + "V" * 89
        capture_dirs = []
        for label, classes in (("baseline-292", baseline),
                               ("candidate-295", candidate)):
            capture = root / label
            capture.mkdir()
            for index, value in enumerate(classes):
                path = capture / f"frame-ring-{index}.bmp"
                if value == "D":
                    write_sampled_bmp(path, 95)
                else:
                    write_solid_bmp(path, 0 if value == "B" else 40)
                stamp = (1_700_000_000_000_000_000 +
                         index * 10_000_000)
                os.utime(path, ns=(stamp, stamp))
            capture_dirs.append(capture)
        before = analyze(capture_dirs[0])
        after = analyze(capture_dirs[1])
        require((before["frames"], before["blackFrames"],
                 before["isolatedBlackFrames"],
                 before["interiorBlackRuns"], before["vvbWindows"],
                 before["bvvWindows"]) == (256, 54, 50, 50, 51, 50),
                ".292 capture calibration mismatch")
        require((after["frames"], after["blackFrames"],
                 after["isolatedBlackFrames"],
                 after["interiorBlackRuns"], after["vvbWindows"],
                 after["bvvWindows"]) == (256, 3, 0, 0, 1, 0),
                ".295 capture calibration mismatch")

        failed = subprocess.run(
            command + [str(capture_dirs[0]), "--max-isolated-black", "0",
                       "--max-interior-black-runs", "0"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        passed = subprocess.run(
            command + [str(capture_dirs[1]), "--max-isolated-black", "0",
                       "--max-interior-black-runs", "0"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        insufficient = subprocess.run(
            command + [str(root), "--max-isolated-black", "2"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        boundary_fail = subprocess.run(
            command + [str(capture_dirs[1]), "--max-isolated-black", "0",
                       "--min-duration-seconds", "2.5500004"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        boundary_pass = subprocess.run(
            command + [str(capture_dirs[1]), "--max-isolated-black", "0",
                       "--min-duration-seconds", "2.55"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        malformed_dir = root / "malformed-directory"
        (malformed_dir / "frame-ring-0.bmp").mkdir(parents=True)
        malformed_exit = subprocess.run(
            command + [str(malformed_dir)], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, check=False)
        nan_exit = subprocess.run(
            command + [str(capture_dirs[1]), "--min-duration-seconds", "nan"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        inf_exit = subprocess.run(
            command + [str(capture_dirs[1]), "--min-duration-seconds", "inf"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        negative_exit = subprocess.run(
            command + [str(capture_dirs[1]),
                       "--max-interior-black-runs", "-1"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        require(failed.returncode == 1 and passed.returncode == 0 and
                insufficient.returncode == 2 and
                boundary_fail.returncode == 2 and
                boundary_pass.returncode == 0 and
                malformed_exit.returncode == 2 and nan_exit.returncode == 2 and
                inf_exit.returncode == 2 and negative_exit.returncode == 2,
                "CLI metric/evidence/error exit gate mismatch")
    print("frame-ring black-cadence self-test: PASS")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("directory", nargs="?")
    parser.add_argument("--output")
    parser.add_argument("--max-isolated-black", type=int)
    parser.add_argument("--max-interior-black-runs", type=int)
    parser.add_argument("--min-visible-frames", type=int, default=1)
    parser.add_argument("--min-frames", type=int, default=256)
    parser.add_argument("--min-duration-seconds", type=float, default=1.0)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if (args.min_frames < 1 or args.min_visible_frames < 1 or
            not math.isfinite(args.min_duration_seconds) or
            args.min_duration_seconds <= 0 or
            (args.max_isolated_black is not None and
             args.max_isolated_black < 0) or
            (args.max_interior_black_runs is not None and
             args.max_interior_black_runs < 0)):
        parser.error("evidence bounds must be finite and nonnegative")
    if args.self_test:
        self_test()
        return 0
    if not args.directory:
        parser.error("directory is required unless --self-test is used")
    try:
        result = analyze(args.directory)
    except (OSError, ValueError) as error:
        print(f"frame-ring oracle: {error}", file=sys.stderr)
        return 2
    result["minimumFrames"] = args.min_frames
    result["minimumVisibleFrames"] = args.min_visible_frames
    result["minimumDurationSeconds"] = args.min_duration_seconds
    result["evidenceSufficient"] = (
        result["frames"] >= args.min_frames and
        result["durationNanoseconds"] / 1e9 >=
            args.min_duration_seconds)
    result["visibilitySufficient"] = (
        result["visibleFrames"] >= args.min_visible_frames)
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    if not result["evidenceSufficient"]:
        return 2
    if not result["visibilitySufficient"]:
        return 1
    if (args.max_isolated_black is not None and
            result["isolatedBlackFrames"] > args.max_isolated_black):
        return 1
    if (args.max_interior_black_runs is not None and
            result["interiorBlackRuns"] > args.max_interior_black_runs):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
