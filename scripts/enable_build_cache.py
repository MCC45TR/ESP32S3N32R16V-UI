import glob
import os
import shutil

Import("env")


def _ensure_env_dict(scons_env):
    if "ENV" not in scons_env:
        scons_env["ENV"] = {}
    return scons_env["ENV"]


def _set_launcher_vars(process_env, tool_path):
    process_env["CMAKE_C_COMPILER_LAUNCHER"] = tool_path
    process_env["CMAKE_CXX_COMPILER_LAUNCHER"] = tool_path
    process_env["CMAKE_ASM_COMPILER_LAUNCHER"] = tool_path


def _prepend_path_entries(process_env, entries):
    current_parts = [part for part in str(process_env.get("PATH") or os.environ.get("PATH", "")).split(os.pathsep) if part]
    normalized_current = {os.path.normcase(os.path.normpath(part)) for part in current_parts}
    new_parts = []
    for entry in entries:
        if not entry or not os.path.isdir(entry):
            continue
        normalized = os.path.normcase(os.path.normpath(entry))
        if normalized in normalized_current:
            continue
        new_parts.append(entry)
        normalized_current.add(normalized)
    if not new_parts:
        return
    updated_path = os.pathsep.join(new_parts + current_parts)
    process_env["PATH"] = updated_path
    os.environ["PATH"] = updated_path


def _enable_cache():
    process_env = _ensure_env_dict(env)

    sccache = shutil.which("sccache")
    if sccache:
        _set_launcher_vars(process_env, sccache)
        process_env.setdefault("SCCACHE_DIR", os.path.join(env.subst("$PROJECT_DIR"), ".pio", "sccache"))
        print(f"[build-cache] enabled sccache: {sccache}")
        return

    ccache = shutil.which("ccache")
    if ccache:
        _set_launcher_vars(process_env, ccache)
        process_env["IDF_CCACHE_ENABLE"] = "1"
        process_env.setdefault("CCACHE_DIR", os.path.join(env.subst("$PROJECT_DIR"), ".pio", "ccache"))
        print(f"[build-cache] enabled ccache: {ccache}")
        return

    print("[build-cache] no sccache/ccache found, continuing without compiler cache")


def _configure_idf_python_env():
    process_env = _ensure_env_dict(env)
    core_dir = env.subst("$PROJECT_CORE_DIR")
    packages_dir = env.subst("$PROJECT_PACKAGES_DIR")
    if not core_dir or not packages_dir:
        return

    idf_path = os.path.join(packages_dir, "framework-espidf")
    _prepend_path_entries(
        process_env,
        (
            os.path.join(packages_dir, "tool-cmake", "bin"),
            os.path.join(packages_dir, "tool-ninja"),
            os.path.join(packages_dir, "toolchain-xtensa-esp-elf", "bin"),
            os.path.join(packages_dir, "toolchain-riscv32-esp", "bin"),
        ),
    )
    penv_candidates = sorted(glob.glob(os.path.join(core_dir, "penv", ".espidf-*")))
    if not penv_candidates:
        return

    idf_python_env = penv_candidates[-1]
    process_env["IDF_TOOLS_PATH"] = core_dir
    process_env["IDF_PYTHON_ENV_PATH"] = idf_python_env
    process_env["IDF_PYTHON_CHECK_CONSTRAINTS"] = "0"
    process_env["IDF_COMPONENT_MANAGER"] = "0"
    idf_target = env.BoardConfig().get("build.mcu")
    if idf_target:
        process_env["IDF_TARGET"] = str(idf_target)
    if os.path.isdir(idf_path):
        process_env["IDF_PATH"] = idf_path
    os.environ["IDF_TOOLS_PATH"] = process_env["IDF_TOOLS_PATH"]
    os.environ["IDF_PYTHON_ENV_PATH"] = process_env["IDF_PYTHON_ENV_PATH"]
    os.environ["IDF_PYTHON_CHECK_CONSTRAINTS"] = process_env["IDF_PYTHON_CHECK_CONSTRAINTS"]
    os.environ["IDF_COMPONENT_MANAGER"] = process_env["IDF_COMPONENT_MANAGER"]
    if "IDF_TARGET" in process_env:
        os.environ["IDF_TARGET"] = process_env["IDF_TARGET"]
    if "IDF_PATH" in process_env:
        os.environ["IDF_PATH"] = process_env["IDF_PATH"]
    print(f"[build-env] IDF_TOOLS_PATH={process_env['IDF_TOOLS_PATH']}")
    print(f"[build-env] IDF_PYTHON_ENV_PATH={process_env['IDF_PYTHON_ENV_PATH']}")
    if "IDF_TARGET" in process_env:
        print(f"[build-env] IDF_TARGET={process_env['IDF_TARGET']}")
    print(f"[build-env] IDF_COMPONENT_MANAGER={process_env['IDF_COMPONENT_MANAGER']}")


_configure_idf_python_env()
_enable_cache()
