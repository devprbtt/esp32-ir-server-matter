import os
import subprocess
import sys

Import("env")


if sys.platform.startswith("win"):
    os.environ.setdefault("IDF_COMPONENT_CACHE_PATH", r"C:\icm")
else:
    os.environ.setdefault("IDF_COMPONENT_CACHE_PATH", os.path.expanduser("~/.cache/idf_component_manager"))


def _generate_insights_cert_stub():
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    cert_path = os.path.join(
        project_dir, "managed_components", "espressif__esp_insights", "server_certs", "https_server.crt"
    )
    if not os.path.exists(cert_path):
        return

    source_path = os.path.join(build_dir, "https_server.crt.S")
    if os.path.exists(source_path):
        return

    os.makedirs(build_dir, exist_ok=True)
    cmake_name = "cmake.exe" if sys.platform.startswith("win") else "cmake"
    cmake = os.path.join(env.PioPlatform().get_package_dir("tool-cmake"), "bin", cmake_name)
    script = os.path.join(
        env.PioPlatform().get_package_dir("framework-espidf"), "tools", "cmake", "scripts", "data_file_embed_asm.cmake"
    )
    subprocess.run(
        [
            cmake,
            f"-DDATA_FILE={cert_path}",
            f"-DSOURCE_FILE={source_path}",
            "-DFILE_TYPE=TEXT",
            "-P",
            script,
        ],
        check=True,
    )


_generate_insights_cert_stub()
