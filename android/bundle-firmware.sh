#!/bin/sh
# Builds the remote firmware and packs it into the app, so the app can update the CYD over
# Bluetooth. Run it before ./gradlew (the release workflow does); without it the app still
# works but offers no firmware update. PIO overrides the PlatformIO command.
set -e
cd "$(dirname "$0")/.."
${PIO:-pio} run -d firmware -e remote -e remote2usb
out=android/app/src/main/assets/firmware
rm -rf "$out"
mkdir -p "$out"
python3 - "$out" remote remote2usb <<'PY'
import hashlib, json, os, shutil, sys

out, envs = sys.argv[1], sys.argv[2:]
manifest = {"boards": {}}
for e in envs:
    build = os.path.join("firmware/.pio/build", e)
    with open(os.path.join(build, "fw_info.json")) as f:
        info = json.load(f)
    with open(os.path.join(build, "firmware.bin"), "rb") as f:
        data = f.read()
    name = info["board"] + ".bin"
    shutil.copy(os.path.join(build, "firmware.bin"), os.path.join(out, name))
    manifest["id"], manifest["flavor"] = info["id"], info["flavor"]
    manifest["boards"][info["board"]] = dict(file=name, size=len(data), sha256=hashlib.sha256(data).hexdigest())
with open(os.path.join(out, "manifest.json"), "w") as f:
    json.dump(manifest, f, indent=1)
print("Packed firmware", manifest["id"], manifest["flavor"], "for", ", ".join(manifest["boards"]))
PY
