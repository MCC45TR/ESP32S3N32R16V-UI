import os

Import("env")


def _ensure_env_dict(scons_env):
    if "ENV" not in scons_env:
        scons_env["ENV"] = {}
    return scons_env["ENV"]


def _is_git_shell_path(path_entry):
    normalized = os.path.normpath(path_entry).lower()
    git_markers = (
        os.path.normpath(r"Git\usr\bin").lower(),
        os.path.normpath(r"GitHubDesktop\bin").lower(),
    )
    return any(normalized.endswith(marker) for marker in git_markers)


def _sanitize_path(raw_path):
    parts = [part for part in str(raw_path).split(os.pathsep) if part]
    kept = [part for part in parts if not _is_git_shell_path(part)]
    return os.pathsep.join(kept), len(parts) - len(kept)


def _drop_shell_vars(process_env):
    removed = []
    for key in (
        "SHELL",
        "MSYSTEM",
        "CHERE_INVOKING",
        "MINGW_CHOST",
        "BASH_ENV",
        "ENV",
        "TERM",
        "SHLVL",
    ):
        if key in process_env:
            process_env.pop(key, None)
            removed.append(key)
        if key in os.environ:
            os.environ.pop(key, None)
    return removed


if os.name == "nt":
    process_env = _ensure_env_dict(env)
    source_path = process_env.get("PATH") or os.environ.get("PATH", "")
    sanitized_path, removed_entries = _sanitize_path(source_path)
    if sanitized_path:
        process_env["PATH"] = sanitized_path
        os.environ["PATH"] = sanitized_path

    removed_vars = _drop_shell_vars(process_env)

    if removed_entries or removed_vars:
        print(
            "[build-env] sanitized Windows shell environment: "
            f"removed_git_usr_bin={removed_entries}, removed_vars={','.join(removed_vars) or 'none'}"
        )
