import csv
import subprocess
from pathlib import Path

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")


def _parse_size(value):
    text = str(value).strip()
    if not text:
        raise ValueError("empty partition size")
    upper = text.upper()
    if upper.endswith("K"):
        return int(upper[:-1], 0) * 1024
    if upper.endswith("M"):
        return int(upper[:-1], 0) * 1024 * 1024
    return int(text, 0)


def _load_recovery_partition(partitions_csv: Path):
    with partitions_csv.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            row = next(csv.reader([raw_line]))
            if len(row) < 5:
                continue
            name = row[0].strip()
            if name != "recovery":
                continue
            offset = _parse_size(row[3])
            size = _parse_size(row[4])
            return offset, size
    raise ValueError(f"recovery partition not found in {partitions_csv}")


def _should_bundle():
    pioenv = env.subst("$PIOENV")
    if not (pioenv.startswith("s3_mros_hub_") or pioenv.startswith("s3_mros_hub_ddr_")):
        return False
    targets = set(COMMAND_LINE_TARGETS)
    return "upload" in targets


def _build_recovery():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    recovery_project = project_dir / env.GetProjectOption(
        "custom_recovery_project_dir", "recovery_app"
    )
    recovery_env = env.GetProjectOption("custom_recovery_env", "recovery_s3")
    cmd = [
        env.subst("$PYTHONEXE"),
        "-m",
        "platformio",
        "run",
        "-d",
        str(recovery_project),
        "-e",
        recovery_env,
    ]
    print(f"Building bundled recovery image ({recovery_env})...")
    subprocess.run(cmd, check=True)
    candidates = [
        recovery_project / ".b" / recovery_env / "firmware.bin",
        recovery_project / ".pio" / "build" / recovery_env / "firmware.bin",
    ]
    for firmware in candidates:
        if firmware.exists():
            return firmware
    raise FileNotFoundError(
        "Recovery firmware not found in any expected build path: "
        + ", ".join(str(path) for path in candidates)
    )


if _should_bundle():
    project_dir = Path(env.subst("$PROJECT_DIR"))
    partitions_csv = project_dir / env.GetProjectOption("board_build.partitions", "partitions-N32R16.csv")
    recovery_offset, recovery_size = _load_recovery_partition(partitions_csv)
    recovery_bin = _build_recovery()
    recovery_bin_size = recovery_bin.stat().st_size
    if recovery_bin_size > recovery_size:
        raise ValueError(
            f"Recovery image ({recovery_bin_size} bytes) exceeds partition size "
            f"({recovery_size} bytes) from {partitions_csv}"
        )

    env.Append(FLASH_EXTRA_IMAGES=[(hex(recovery_offset), str(recovery_bin))])
    print(
        "Bundled recovery image: "
        f"{recovery_bin.name} -> 0x{recovery_offset:X} ({recovery_bin_size} bytes)"
    )
