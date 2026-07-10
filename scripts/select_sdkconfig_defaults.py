from pathlib import Path

Import("env")


def _as_text_list(value):
    if isinstance(value, (list, tuple)):
        return [str(item) for item in value]
    return str(value).replace(",", " ").split()


frameworks = _as_text_list(env.GetProjectOption("framework", ""))
source_name = env.GetProjectOption("custom_sdkconfig_source", "")

if "espidf" in frameworks and source_name:
    project_dir = Path(env.subst("$PROJECT_DIR"))
    source = project_dir / source_name
    destination = project_dir / "sdkconfig.defaults"

    if not source.exists():
        raise FileNotFoundError(f"custom_sdkconfig_source not found: {source}")

    content = source.read_text(encoding="utf-8")
    if not destination.exists() or destination.read_text(encoding="utf-8") != content:
        destination.write_text(content, encoding="utf-8")
        print(f"Selected {source_name} -> sdkconfig.defaults")
