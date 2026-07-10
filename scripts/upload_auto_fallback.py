import subprocess

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")


def _parse_speed_list(text):
    items = []
    for raw in str(text).replace(";", ",").split(","):
        token = raw.strip()
        if not token:
            continue
        try:
            val = int(token, 10)
        except ValueError:
            continue
        if val > 0 and val not in items:
            items.append(val)
    return items


def _upload_auto_action(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    pioenv = env.subst("$PIOENV")
    preferred = int(env.GetProjectOption("upload_speed", "921600"))
    fallback = _parse_speed_list(
        env.GetProjectOption(
            "custom_upload_retry_speeds",
            "3000000,2000000,1500000,921600,460800",
        )
    )

    speeds = [preferred] + [v for v in fallback if v != preferred]

    for speed in speeds:
        cmd = [
            env.subst("$PYTHONEXE"),
            "-m",
            "platformio",
            "run",
            "-d",
            project_dir,
            "-e",
            pioenv,
            "-t",
            "upload",
            "-O",
            f"upload_speed={speed}",
        ]
        print(f"[upload-auto] trying upload_speed={speed}")
        result = subprocess.run(cmd, check=False)
        if result.returncode == 0:
            print(f"[upload-auto] upload succeeded at {speed}")
            return

    raise RuntimeError(
        "[upload-auto] upload failed at all fallback speeds: " + ", ".join(str(v) for v in speeds)
    )


env.AddCustomTarget(
    "upload_auto",
    "$BUILD_DIR/${PROGNAME}.bin",
    _upload_auto_action,
    title="Upload (auto baud fallback)",
    description="Upload firmware with automatic baud fallback retries",
)

if "upload_auto" in set(COMMAND_LINE_TARGETS):
    print("[upload-auto] target active")
