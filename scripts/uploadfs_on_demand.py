import subprocess
import hashlib
from pathlib import Path

Import("env")


def _enabled(value):
    return str(value).strip().lower() in ("1", "true", "yes", "y", "on")


def _data_dir():
    return Path(env.subst("$PROJECT_DIR")) / env.GetProjectOption("data_dir", "data")


def _hash_state_file():
    return Path(env.subst("$BUILD_DIR")) / ".uploadfs_data_hash"


def _compute_tree_hash(root):
    digest = hashlib.sha256()
    if not root.exists():
        return ""

    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rel = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(rel)
        with path.open("rb") as handle:
            while True:
                chunk = handle.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
    return digest.hexdigest()


def _is_data_changed():
    current = _compute_tree_hash(_data_dir())
    state_file = _hash_state_file()
    previous = state_file.read_text(encoding="utf-8").strip() if state_file.exists() else ""
    return current != previous, current


def _save_data_hash(data_hash):
    state_file = _hash_state_file()
    state_file.parent.mkdir(parents=True, exist_ok=True)
    state_file.write_text(data_hash, encoding="utf-8")


def _uploadfs_after_upload(source, target, env):
    if not _enabled(env.GetProjectOption("custom_uploadfs", "no")):
        return

    from SCons.Script import COMMAND_LINE_TARGETS

    if "uploadfs" in COMMAND_LINE_TARGETS or "buildfs" in COMMAND_LINE_TARGETS:
        return

    changed, data_hash = _is_data_changed()
    if not changed:
        print("custom_uploadfs=yes -> skipping uploadfs (data/ unchanged)")
        return

    cmd = [
        env.subst("$PYTHONEXE"),
        "-m",
        "platformio",
        "run",
        "-d",
        env.subst("$PROJECT_DIR"),
        "-e",
        env.subst("$PIOENV"),
        "-t",
        "uploadfs",
    ]
    print("custom_uploadfs=yes -> uploading LittleFS image from data/")
    subprocess.run(cmd, check=True)
    _save_data_hash(data_hash)


def _mark_hash_after_manual_uploadfs(source, target, env):
    changed, data_hash = _is_data_changed()
    if changed:
        _save_data_hash(data_hash)
        print("uploadfs completed -> data hash state updated")


env.AddPostAction("upload", _uploadfs_after_upload)
env.AddPostAction("uploadfs", _mark_hash_after_manual_uploadfs)
