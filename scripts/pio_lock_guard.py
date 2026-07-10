import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


def _extract_pid(lock_path):
    try:
        text = lock_path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None

    text = text.strip()
    if not text:
        return None

    try:
        data = json.loads(text)
        if isinstance(data, dict) and "pid" in data:
            pid_val = int(data["pid"])
            return pid_val if pid_val > 0 else None
    except Exception:
        pass

    patterns = [
        r'"pid"\s*:\s*(\d+)',
        r"\bpid\s*[:=]\s*(\d+)",
        r"^(\d+)$",
    ]
    for pattern in patterns:
        match = re.search(pattern, text, flags=re.IGNORECASE | re.MULTILINE)
        if not match:
            continue
        pid_val = int(match.group(1))
        return pid_val if pid_val > 0 else None

    return None


def _is_process_alive(pid):
    if pid is None or pid <= 0:
        return False

    if os.name == "nt":
        result = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            capture_output=True,
            text=True,
            check=False,
        )
        output = (result.stdout or "") + "\n" + (result.stderr or "")
        lowered = output.lower()
        if "no tasks are running" in lowered or "bilgi:" in lowered:
            return False
        return str(pid) in output

    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _lock_age_seconds(lock_path):
    try:
        return max(0.0, time.time() - lock_path.stat().st_mtime)
    except OSError:
        return 0.0


def _candidate_lock_paths(project_dir):
    unique = []

    for root in (
        project_dir / ".platformio",
        project_dir / ".pio",
        Path.home() / ".platformio",
    ):
        candidate = (root / "platforms.lock").resolve()
        if candidate not in unique:
            unique.append(candidate)

    core_dir = os.environ.get("PLATFORMIO_CORE_DIR")
    if core_dir:
        candidate = (Path(core_dir) / "platforms.lock").resolve()
        if candidate not in unique:
            unique.append(candidate)

    return unique


def _cleanup_lock(lock_path, stale_seconds, wait_seconds):
    if not lock_path.exists():
        return False, False

    waited = 0
    while lock_path.exists():
        pid = _extract_pid(lock_path)
        pid_alive = _is_process_alive(pid)
        age = _lock_age_seconds(lock_path)

        if pid_alive:
            if waited < wait_seconds:
                if waited == 0:
                    print(f"[pio-lock] active lock detected: {lock_path} (pid={pid}), waiting...")
                time.sleep(1)
                waited += 1
                continue
            print(f"[pio-lock] lock still active after wait: {lock_path} (pid={pid})")
            return False, True

        if age >= stale_seconds:
            try:
                lock_path.unlink()
                print(
                    f"[pio-lock] removed stale lock: {lock_path} "
                    f"(age={age:.1f}s, pid={pid if pid else 'unknown'})"
                )
                return True, False
            except OSError as exc:
                print(f"[pio-lock] failed to remove {lock_path}: {exc}")
                return False, True

        if waited < wait_seconds:
            if waited == 0:
                print(
                    f"[pio-lock] lock exists but looks fresh: {lock_path} "
                    f"(age={age:.1f}s), waiting up to {wait_seconds}s"
                )
            time.sleep(1)
            waited += 1
            continue

        print(
            f"[pio-lock] lock remained after wait and is younger than stale threshold: {lock_path} "
            f"(age={age:.1f}s, stale_after={stale_seconds}s)"
        )
        return False, True

    return False, False


def main():
    parser = argparse.ArgumentParser(
        description="Clean stale PlatformIO platforms.lock files and run a pio command"
    )
    parser.add_argument("--project-dir", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--stale-seconds", type=int, default=180)
    parser.add_argument("--wait-seconds", type=int, default=20)
    parser.add_argument("--clean-only", action="store_true")
    parser.add_argument("pio_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    project_dir = Path(args.project_dir).resolve()
    lock_files = _candidate_lock_paths(project_dir)

    print("[pio-lock] project_dir={}".format(project_dir))
    removed_any = False
    blocked = False

    for lock_file in lock_files:
        removed, is_blocked = _cleanup_lock(
            lock_file,
            stale_seconds=max(1, int(args.stale_seconds)),
            wait_seconds=max(0, int(args.wait_seconds)),
        )
        removed_any = removed_any or removed
        blocked = blocked or is_blocked

    if removed_any:
        print("[pio-lock] stale lock cleanup completed")

    if blocked:
        print("[pio-lock] lock is still active; aborting to avoid corruption")
        return 2

    if args.clean_only:
        print("[pio-lock] clean-only mode done")
        return 0

    pio_args = list(args.pio_args)
    if pio_args and pio_args[0] == "--":
        pio_args = pio_args[1:]
    if not pio_args:
        pio_args = ["run"]

    cmd = ["pio"] + pio_args
    print("[pio-lock] exec: {}".format(" ".join(cmd)))
    result = subprocess.run(cmd, cwd=str(project_dir), check=False)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
