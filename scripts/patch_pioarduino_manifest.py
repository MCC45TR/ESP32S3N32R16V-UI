from pathlib import Path

Import("env")

UNUSED_HYBRID_DEPENDENCIES = (
    "espressif/esp_rainmaker",
    "espressif/esp_insights",
    "espressif/esp_diagnostics",
    "espressif/esp_diag_data_store",
    "espressif/rmaker_common",
    "wolfssl/wolfssl",
)

def _as_text_list(value):
    if isinstance(value, (list, tuple)):
        return [str(item) for item in value]
    return str(value).replace(",", " ").split()


def _remove_dependency_block(text, dependency_name):
    lines = text.splitlines()
    output = []
    skip = False
    removed = False
    needle = f"  {dependency_name}:"

    for line in lines:
        if line == needle:
            skip = True
            removed = True
            continue

        if skip:
            if line.startswith("  ") and not line.startswith("    ") and line.endswith(":"):
                skip = False
                output.append(line)
            continue

        output.append(line)

    return "\n".join(output) + ("\n" if text.endswith("\n") else ""), removed


frameworks = _as_text_list(env.GetProjectOption("framework", ""))
if "espidf" in frameworks:
    manifest = (
        Path(env.subst("$PROJECT_PACKAGES_DIR"))
        / "framework-arduinoespressif32"
        / "idf_component.yml"
    )

    if manifest.exists():
        original = manifest.read_text(encoding="utf-8")
        patched = original
        removed_names = []
        for dependency_name in UNUSED_HYBRID_DEPENDENCIES:
            patched, removed = _remove_dependency_block(patched, dependency_name)
            if removed:
                removed_names.append(dependency_name)

        if removed_names:
            backup = manifest.with_name("idf_component.yml.orig")
            if not backup.exists():
                backup.write_text(original, encoding="utf-8")
            manifest.write_text(patched, encoding="utf-8")
            print(
                "Patched Arduino component manifest: removed unused hybrid dependencies "
                + ", ".join(removed_names)
            )
