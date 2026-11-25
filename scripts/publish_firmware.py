Import("env")
import os
import shutil
import json
from datetime import datetime

REPO_RAW_BASE = "https://githubusercontent.com"

def after_build(source, target, env):
    bin_path = str(next(iter(target)))
    if not os.path.exists(bin_path):
        return

    project_dir = env.subst("$PROJECT_DIR")
    fw_dir = os.path.join(project_dir, "firmware")
    os.makedirs(fw_dir, exist_ok=True)

    ver_header = os.path.join(project_dir, "include", "fw_version.h")
    version_str = datetime.now().strftime("%Y.%m.%d.%H%M")
    if os.path.exists(ver_header):
        with open(ver_header, "r", encoding="utf-8") as f:
            for line in f:
                if "#define FW_VERSION" in line:
                    parts = line.split('"')
                    if len(parts) >= 2:
                        it = iter(parts)
                        next(it)
                        version_str = next(it)
                    break

    # 1. Copy compiled firmware.bin into /firmware/firmware.bin
    dest_bin = os.path.join(fw_dir, "firmware.bin")
    shutil.copy2(bin_path, dest_bin)

    # 2. Write /firmware/version.json
    version_data = {
        "version": version_str,
        "url": f"{REPO_RAW_BASE}/firmware.bin"
    }
    dest_json = os.path.join(fw_dir, "version.json")
    with open(dest_json, "w", encoding="utf-8") as f:
        json.dump(version_data, f, indent=2)

    print(f"[OTA] Published v{version_str} -> {dest_bin}")

def stamp_version_before_build():
    project_dir = env.subst("$PROJECT_DIR")
    inc_dir = os.path.join(project_dir, "include")
    os.makedirs(inc_dir, exist_ok=True)
    ver_header = os.path.join(inc_dir, "fw_version.h")

    version_str = datetime.now().strftime("%Y.%m.%d.%H%M")
    with open(ver_header, "w", encoding="utf-8") as f:
        f.write("#pragma once\n")
        f.write(f'#define FW_VERSION "{version_str}"\n')

    print(f"[OTA] Stamped FW_VERSION = {version_str}")

stamp_version_before_build()
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)
