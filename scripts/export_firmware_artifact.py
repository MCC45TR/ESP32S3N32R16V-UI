import re
import shutil
from datetime import datetime
from pathlib import Path

Import("env")


def _board_slug():
    explicit = str(env.GetProjectOption("custom_artifact_board_slug", "")).strip().lower()
    if explicit:
        return re.sub(r"[^a-z0-9]+", "", explicit)

    board = str(env.GetProjectOption("board", "esp32s3")).strip().lower()
    slug = re.sub(r"[^a-z0-9]+", "", board)
    for token in ("devkitc1", "devkitc", "local"):
        slug = slug.replace(token, "")
    return slug or "esp32s3"


def _export_artifact(source, target, env):
    bin_path = Path(str(target[0]))
    if not bin_path.exists():
        return

    stamp = datetime.now().strftime("%Y%m%d_%H%M")
    board = _board_slug()
    out_name = f"mros_{board}_{stamp}.bin"

    out_dir = Path(env.subst("$PROJECT_DIR")) / "output" / "artifacts"
    out_dir.mkdir(parents=True, exist_ok=True)

    out_file = out_dir / out_name
    shutil.copy2(bin_path, out_file)
    print(f"[artifact] exported {out_file}")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", _export_artifact)
