#!/usr/bin/env python3
"""Fail the build if an image does not fit in a named ESP-IDF partition."""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys


SIZE_RE = re.compile(r"^\s*(0x[0-9a-fA-F]+|\d+)\s*([kKmM]?)\s*$")


def parse_size(value: str) -> int:
    match = SIZE_RE.match(value)
    if not match:
        raise ValueError(f"invalid size value: {value!r}")
    number_text, suffix = match.groups()
    number = int(number_text, 0)
    if suffix.lower() == "k":
        number *= 1024
    elif suffix.lower() == "m":
        number *= 1024 * 1024
    return number


def find_partition_size(partition_csv: str, partition: str) -> int:
    with open(partition_csv, newline="", encoding="utf-8") as handle:
        rows = (line for line in handle if line.strip() and not line.lstrip().startswith("#"))
        for row in csv.reader(rows):
            if len(row) < 5:
                continue
            name = row[0].strip()
            if name == partition:
                return parse_size(row[4].strip())
    raise ValueError(f"partition {partition!r} not found in {partition_csv}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--partition-csv", required=True)
    parser.add_argument("--partition", required=True)
    parser.add_argument("--image", required=True)
    args = parser.parse_args()

    partition_size = find_partition_size(args.partition_csv, args.partition)
    image_size = os.path.getsize(args.image)
    free_size = partition_size - image_size
    if image_size > partition_size:
        print(
            f"ERROR: {args.image} is {image_size} bytes, partition "
            f"{args.partition} is {partition_size} bytes, overflow {abs(free_size)} bytes.",
            file=sys.stderr,
        )
        return 1

    print(
        f"{args.partition}: {os.path.basename(args.image)} fits "
        f"({image_size}/{partition_size} bytes, {free_size} bytes free)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
