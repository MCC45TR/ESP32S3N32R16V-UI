#!/usr/bin/env python3
"""Reject first-party publication residue that must not enter a public release."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


SELF_FILES = {
    "tools/ci/check_public_release_hygiene.py",
    "tools/ci/test_public_release_hygiene.py",
}

SKIP_PREFIXES = (
    "components/",
    "managed_components/",
    "recovery_app/managed_components/",
)

RULES = (
    (
        "ai_tool_residue",
        re.compile(
            r"(?i)(?:\bcodex\b|\bchatgpt\b|\bopenai\b|\bclaude\b|\banthropic\b|"
            r"\bgemini\b|\bcopilot\b|generated\s+by\s+ai|ai[- ]generated|"
            r"\bsystem\s+prompt\b|\.codex(?:[/\\]|\b))"
        ),
        "assistant marker or local assistant path",
    ),
    (
        "personal_absolute_path",
        re.compile(
            r"(?i)(?:[A-Z]:[/\\]Users[/\\][^/\\\s`]+[/\\]|"
            r"/Users/[^/\s`]+/|/home/[^/\s`]+/)"
        ),
        "personal absolute filesystem path",
    ),
    (
        "fixed_windows_serial_port",
        re.compile(r"(?i)\bCOM\d+\b"),
        "machine-specific Windows serial port; use <SERIAL_PORT>",
    ),
    (
        "weak_default",
        re.compile(
            r"(?i)(?:default\s*=|password\s*[:=])\s*['\"](?:1234|12345|123456|admin|"
            r"password|changeme|change-me|change_me_local_only|mcc45tr|root)['\"]"
        ),
        "weak or maintainer-specific default",
    ),
)


def candidates(root: Path) -> list[Path]:
    proc = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        check=True,
        capture_output=True,
    )
    rels = [part for part in proc.stdout.decode("utf-8", errors="replace").split("\0") if part]
    return [root / rel for rel in rels]


def read_text(path: Path) -> str | None:
    try:
        raw = path.read_bytes()
    except OSError:
        return None
    if b"\0" in raw:
        return None
    for encoding in ("utf-8", "latin-1"):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            continue
    return None


def scan_text(rel: str, text: str) -> list[dict[str, object]]:
    rel = rel.replace("\\", "/")
    if rel in SELF_FILES or rel.startswith(SKIP_PREFIXES):
        return []
    issues: list[dict[str, object]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        for rule, pattern, detail in RULES:
            if pattern.search(line):
                issues.append({"path": rel, "line": line_number, "rule": rule, "detail": detail})
    return issues


def scan_repository(root: Path) -> tuple[int, list[dict[str, object]]]:
    issues: list[dict[str, object]] = []
    scanned = 0
    for path in candidates(root):
        if not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if rel.startswith(SKIP_PREFIXES):
            continue
        text = read_text(path)
        if text is None:
            continue
        scanned += 1
        issues.extend(scan_text(rel, text))
    return scanned, issues


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--json-out", default="")
    args = parser.parse_args()

    root = Path(args.repo_root).resolve()
    scanned, issues = scan_repository(root)
    summary = {
        "schema": "mros.esp32.public-release-hygiene.v1",
        "scanned_files": scanned,
        "issue_count": len(issues),
        "issues": issues,
    }
    if args.json_out:
        output = Path(args.json_out)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if issues:
        for issue in issues:
            print(f"{issue['path']}:{issue['line']}: {issue['rule']}: {issue['detail']}")
        return 1
    print(json.dumps({"scanned_files": scanned, "issue_count": 0}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
