import csv
import json
import subprocess
from datetime import datetime
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


def _load_app_partition_size(partitions_csv):
    with partitions_csv.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            row = next(csv.reader([raw_line]))
            if len(row) >= 5 and row[0].strip() == "app0":
                return _parse_size(row[4])
    return 0


def _parse_size_sections(raw_output):
    sections = {}
    for line in raw_output.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[0]
        if name in ("section", "Total"):
            continue
        try:
            size = int(parts[1])
        except ValueError:
            continue
        sections[name] = size
    return sections


def _classify_sections(sections):
    flash = 0
    ram = 0
    iram = 0
    other = 0

    for name, size in sections.items():
        if name.startswith(".iram"):
            iram += size
        elif name.startswith(".dram") or name in (".data", ".bss", ".noinit"):
            ram += size
        elif name.startswith(".flash") or name.startswith(".rodata") or name.startswith(".text") or name.startswith(".app"):
            flash += size
        else:
            other += size

    return {
        "flash_bytes": flash,
        "ram_bytes": ram,
        "iram_bytes": iram,
        "other_bytes": other,
    }


def _write_reports(report_dir, payload):
    report_dir.mkdir(parents=True, exist_ok=True)
    stamp = payload["timestamp"]
    pioenv = payload["pioenv"]

    json_path = report_dir / f"size_report_{pioenv}_{stamp}.json"
    md_path = report_dir / f"size_report_{pioenv}_{stamp}.md"

    json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")

    lines = [
        f"# Size Report ({pioenv})",
        "",
        f"- Timestamp: {payload['timestamp']}",
        f"- Firmware bin: {payload['firmware_bin_bytes']} bytes",
        f"- Firmware elf: {payload['firmware_elf_bytes']} bytes",
        "",
        "| Metric | Bytes |",
        "|---|---:|",
        f"| Flash (estimated) | {payload['classification']['flash_bytes']} |",
        f"| RAM (estimated) | {payload['classification']['ram_bytes']} |",
        f"| IRAM (estimated) | {payload['classification']['iram_bytes']} |",
        f"| Other sections | {payload['classification']['other_bytes']} |",
    ]

    app_size = payload.get("app_partition_bytes", 0)
    if app_size > 0:
        lines.extend(
            [
                "",
                f"| app0 partition | {app_size} |",
                f"| Bin / app0 usage | {payload['app_usage_percent']:.2f}% |",
            ]
        )

    md_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"[size-report] wrote {json_path}")
    print(f"[size-report] wrote {md_path}")


def _generate_size_report(source, target, env):
    elf_path = Path(env.subst("$BUILD_DIR")) / "firmware.elf"
    bin_path = Path(str(target[0]))
    if not elf_path.exists() or not bin_path.exists():
        return

    size_tool = env.subst("$SIZE")
    if not size_tool or "$SIZE" in size_tool:
        print("[size-report] size tool not available, skipping")
        return

    result = subprocess.run(
        [size_tool, "-A", str(elf_path)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        print("[size-report] failed to execute size tool, skipping")
        return

    sections = _parse_size_sections(result.stdout)
    classified = _classify_sections(sections)

    project_dir = Path(env.subst("$PROJECT_DIR"))
    partitions_name = env.GetProjectOption("board_build.partitions", "partitions-N32R16.csv")
    partitions_csv = project_dir / partitions_name
    app_partition_size = _load_app_partition_size(partitions_csv) if partitions_csv.exists() else 0

    payload = {
        "timestamp": datetime.now().strftime("%Y%m%d_%H%M%S"),
        "pioenv": env.subst("$PIOENV"),
        "firmware_bin_bytes": bin_path.stat().st_size,
        "firmware_elf_bytes": elf_path.stat().st_size,
        "sections": sections,
        "classification": classified,
        "app_partition_bytes": app_partition_size,
        "app_usage_percent": (bin_path.stat().st_size * 100.0 / app_partition_size) if app_partition_size else 0.0,
    }

    report_dir = project_dir / "output" / "reports"
    _write_reports(report_dir, payload)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _generate_size_report)
