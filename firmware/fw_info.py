# Which firmware this is, for updates over Bluetooth: the last commit that changed firmware/
# ("+" if built with uncommitted changes), the flavor (custom_fw_flavor) and the board
# (custom_fw_board). Compiled in as FW_ID, FW_FLAVOR and FW_BOARD, and written next to the
# build as fw_info.json, which android/bundle-firmware.sh packs into the app.
Import("env")

import json
import os
import subprocess


def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env["PROJECT_DIR"], text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


fw_id = git("log", "-1", "--format=%h", "--", ".") or "unknown"
if git("status", "--porcelain", "--untracked-files=no", "--", "."):
    fw_id += "+"
flavor = env.GetProjectOption("custom_fw_flavor", "public")
board = env.GetProjectOption("custom_fw_board", env["PIOENV"])

env.Append(CPPDEFINES=[
    ("FW_ID", env.StringifyMacro(fw_id)),
    ("FW_FLAVOR", env.StringifyMacro(flavor)),
    ("FW_BOARD", env.StringifyMacro(board)),
])

build_dir = env.subst("$BUILD_DIR")
os.makedirs(build_dir, exist_ok=True)
with open(os.path.join(build_dir, "fw_info.json"), "w") as f:
    json.dump(dict(id=fw_id, flavor=flavor, board=board), f)
