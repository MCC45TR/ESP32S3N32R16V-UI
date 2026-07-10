#!/usr/bin/env python3
"""High-confidence scanner for first-party credentials and private key material."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


SKIP_DIRS = {".git", ".pio", "build", "dist", "output", "__pycache__"}
SKIP_PREFIXES = ("components/", "managed_components/", "recovery_app/managed_components/")
ALLOW_SECRET_LIKE_FILES = {
    ".env.example",
    "src/config/wifi_secrets.example.h",
    "tools/ci/scan_secrets.py",
    "tools/ci/test_secret_scan.py",
}
SELF_FILES = {"tools/ci/scan_secrets.py", "tools/ci/test_secret_scan.py"}
PLACEHOLDERS = {
    "change-me", "changeme", "example", "placeholder", "dummy", "sample",
    "not-a-secret", "<redacted>", "redacted", "set_", "<set_", "required", "build_", "change",
}
TEXT_SUFFIXES = {
    "", ".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".ini", ".json",
    ".md", ".ps1", ".py", ".sh", ".txt", ".yml", ".yaml", ".toml",
}

SECRET_FILE_RE = re.compile(
    r"(^\.env$|secret|private[_-]?key|(?:^|[-_])key(?:[-_.]|$).*\.pem$|\.p12$)", re.IGNORECASE
)
PRIVATE_KEY_RE = re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")
AWS_ACCESS_KEY_RE = re.compile(r"\bAKIA[0-9A-Z]{16}\b")
GITHUB_TOKEN_RE = re.compile(r"\b(?:ghp|gho|ghu|ghs|ghr)_[A-Za-z0-9_]{30,}\b")
GENERIC_ASSIGNMENT_RE = re.compile(
    r"(?i)\b(password|passwd|api[_-]?key|secret|token|private[_-]?key|psk|hmac[_-]?key)\b"
    r"\s*[:=]\s*(['\"])([^'\"]{8,})\2"
)
SECRET_DEFINE_RE = re.compile(
    r"(?i)#define\s+\w*(password|passwd|secret|token|psk|key)\w*\s+['\"]([^'\"]{8,})['\"]"
)


def should_skip(path: Path, root: Path) -> bool:
    rel = path.relative_to(root).as_posix()
    if any(part in SKIP_DIRS for part in path.relative_to(root).parts[:-1]):
        return True
    return rel.startswith(SKIP_PREFIXES)


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


def placeholder_value(value: str) -> bool:
    lowered = value.lower()
    return any(word in lowered for word in PLACEHOLDERS) or value in {
        "true", "false", "null", "none", "nullptr"
    }


def scan_file(path: Path, root: Path) -> list[dict[str, object]]:
    rel = path.relative_to(root).as_posix()
    if rel in SELF_FILES:
        return []
    issues: list[dict[str, object]] = []
    if SECRET_FILE_RE.search(path.name) and rel not in ALLOW_SECRET_LIKE_FILES:
        issues.append({"path": rel, "line": 1, "rule": "secret_like_filename", "detail": "secret-like filename"})
    if path.suffix.lower() not in TEXT_SUFFIXES:
        return issues
    text = read_text(path)
    if text is None:
        return issues
    for line_number, line in enumerate(text.splitlines(), start=1):
        if PRIVATE_KEY_RE.search(line):
            issues.append({"path": rel, "line": line_number, "rule": "private_key_block", "detail": "private key material"})
        if AWS_ACCESS_KEY_RE.search(line):
            issues.append({"path": rel, "line": line_number, "rule": "aws_access_key", "detail": "AWS access key literal"})
        if GITHUB_TOKEN_RE.search(line):
            issues.append({"path": rel, "line": line_number, "rule": "github_token", "detail": "GitHub token literal"})
        match = GENERIC_ASSIGNMENT_RE.search(line)
        if match and not placeholder_value(match.group(3)):
            issues.append({
                "path": rel,
                "line": line_number,
                "rule": "literal_secret_assignment",
                "detail": f"literal assignment to {match.group(1)}",
            })
        define_match = SECRET_DEFINE_RE.search(line)
        if define_match and not placeholder_value(define_match.group(2)):
            issues.append({
                "path": rel,
                "line": line_number,
                "rule": "literal_secret_define",
                "detail": f"literal #define for {define_match.group(1)}",
            })
    return issues


def candidates(root: Path) -> list[Path]:
    proc = subprocess.run(
        ["git", "-C", str(root), "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        check=True,
        capture_output=True,
    )
    rels = [item for item in proc.stdout.decode("utf-8", errors="replace").split("\0") if item]
    return [root / rel for rel in rels]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--json-out", default="")
    args = parser.parse_args()
    root = Path(args.repo_root).resolve()
    issues: list[dict[str, object]] = []
    scanned = 0
    for path in candidates(root):
        if not path.is_file() or should_skip(path, root):
            continue
        scanned += 1
        issues.extend(scan_file(path, root))
    summary = {
        "schema": "mros.esp32.secret-scan.v1",
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
            print(f"{issue['path']}:{issue['line']}: {issue['rule']}: {issue['detail']}", file=sys.stderr)
        return 1
    print(json.dumps({"scanned_files": scanned, "issue_count": 0}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
