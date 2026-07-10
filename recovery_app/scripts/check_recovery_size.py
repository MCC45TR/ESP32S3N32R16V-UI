import csv
from pathlib import Path

Import("env")


def _parse_size(value):
    text = str(value).strip()
    upper = text.upper()
    if upper.endswith("K"):
        return int(upper[:-1], 0) * 1024
    if upper.endswith("M"):
        return int(upper[:-1], 0) * 1024 * 1024
    return int(text, 0)


def _load_recovery_partition_size(partitions_csv: Path):
    with partitions_csv.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            row = next(csv.reader([raw_line]))
            if len(row) >= 5 and row[0].strip() == "recovery":
                return _parse_size(row[4])
    raise ValueError(f"recovery partition not found in {partitions_csv}")


def _check_size(source, target, env):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    partitions_csv = project_dir.parent / "partitions-N32R16.csv"
    recovery_limit = _load_recovery_partition_size(partitions_csv)
    firmware_bin = Path(str(target[0]))
    image_size = firmware_bin.stat().st_size
    if image_size > recovery_limit:
        raise ValueError(
            f"Recovery image ({image_size} bytes) exceeds recovery partition "
            f"({recovery_limit} bytes)"
        )
    print(
        f"Recovery size check passed: {image_size} / {recovery_limit} bytes"
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _check_size)
