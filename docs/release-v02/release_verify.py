"""Read-only local validation of the built application bundle and preserved inputs."""
from pathlib import Path
import hashlib
import json
import os
import struct
import subprocess
import sys
import zipfile
ROOT = Path(__file__).resolve().parent
UTILS = ROOT / "submodule/esp-mosaico-utils"
PROJECT = ROOT / "projects/bambu_monitor"
BUILD = PROJECT / "build"
BUNDLE = BUILD / "bambu_monitor-v0.2-system-update.irisfw"
sys.path.insert(0, str(UTILS / "mosaico-tools/tools"))
sys.path.insert(0, str(UTILS / "ESP-Iris/components/esp_iris/tools"))
from mosaico_cli.ideas_firmware import validate_firmware
from iris_gateway.firmware import inspect_firmware_image
raw = BUNDLE.read_bytes()
manifest, members = validate_firmware(raw)
assert manifest["schema"] == "esp-iris-system-update/0.2"
assert manifest["minimum_recovery_version"] == "0.2.0"
assert [c["kind"] for c in manifest["components"]] == ["partition_table", "application", "data"]
assert "manifest.sig" not in members
app_component = next(c for c in manifest["components"] if c["kind"] == "application")
app = inspect_firmware_image(members[app_component["file"]]).as_dict()
assert app["project_name"] == "bambu_monitor" and app["version"] == "1.0.0" and app["chip_id"] == 32
assert members[app_component["file"]] == (BUILD / "bambu_monitor.bin").read_bytes()
config = (BUILD / "sdkconfig").read_text()
required = ['CONFIG_ESP_IRIS_PRODUCT_CONTRACT="esp-mosaico/v2"', 'CONFIG_ESP_IRIS_LAYOUT_ID="mosaico-retained-test-2m-v2"', 'CONFIG_ESP_IRIS_RECOVERY_ABI=2', 'CONFIG_ESP_IRIS_FIRMWARE_ROLE=1', 'CONFIG_ESP_IRIS_OTA_DEFAULT_VIA_RECOVERY=y', '# CONFIG_ESP_IRIS_OTA is not set', 'CONFIG_TINYUSB_CDC_COUNT=2', 'CONFIG_ESPTOOLPY_FLASHMODE_QIO=y']
assert all(line in config.splitlines() for line in required)
sha = lambda b: hashlib.sha256(b).hexdigest()
source = json.loads((ROOT / "evidence/source-snapshot.json").read_text())
protected = {name: digest for name, digest in source.items() if name.startswith(("main/", "ui/", "pc/"))}
assert all(sha((PROJECT / name).read_bytes()) == digest for name, digest in protected.items())
contract = json.loads((UTILS / "esp-mosaico-vibe-mode/product_contract.json").read_text())
table = members[manifest["components"][0]["file"]]
partitions = []
for offset in range(0, 3072, 32):
    magic, kind, subtype, start, size, label, flags = struct.unpack("<HBBII16sI", table[offset:offset+32])
    if magic != 0x50aa:
        break
    partitions.append(dict(name=label.split(b"\0", 1)[0].decode("ascii"), type=kind, subtype=subtype, offset=start, size=size, flags=flags))
assert next(p for p in partitions if p["name"] == "nvs") == dict(name="nvs", type=1, subtype=2, offset=0x200000, size=0x10000, flags=0)
assert not any(c["target_offset"] == 0x200000 for c in manifest["components"])
ui = next(c for c in manifest["components"] if c["kind"] == "data")
assert members[ui["file"]] == (BUILD / "ui_apps.bin").read_bytes()
env = dict(os.environ)
for key in ("MSYSTEM", "MINGW_PREFIX", "PYTHONPATH", "PYTHONHOME"):
    env.pop(key, None)
env.update(PYTHONUTF8="1", PYTHONIOENCODING="utf-8")
with (ROOT / "evidence/new-bundle-inspect.stderr.log").open("w", encoding="utf-8") as errors:
    inspect_result = subprocess.run(
        [str(ROOT / ".venv-iris/Scripts/python.exe"), str(UTILS / "ESP-Iris/components/esp_iris/tools/esp_iris.py"), "bundle", "--json", "inspect", str(BUNDLE)],
        cwd=ROOT, env=env, capture_output=True, text=True)
(ROOT / "evidence/new-bundle-inspect.stderr.log").write_text(inspect_result.stderr, encoding="utf-8")
assert inspect_result.returncode == 0, inspect_result.stderr
official_inspect = json.loads(inspect_result.stdout)
(ROOT / "evidence/new-bundle-inspect.json").write_text(json.dumps(official_inspect, indent=2), encoding="utf-8")
assert official_inspect["schema"] == "esp-iris-system-update/0.2"
assert official_inspect["minimum_recovery_version"] == "0.2.0"
assert official_inspect["flash_size"] == 16777216
result = dict(status="PASS", bundle=str(BUNDLE), size=len(raw), sha256=sha(raw), official_ideas_product_validation="PASS", official_bundle_inspect=official_inspect, manifest=manifest, app_descriptor=app, effective_contract=required, binary_partitions=partitions, official_product_contract=contract, unchanged_main_ui_pc_file_count=len(protected), unchanged_main_ui_pc_hashes=protected, nvs_data="Source serialization/namespaces and partition offset/size unchanged; no NVS component in bundle; no device read/write was performed", limitations=["No Vibe Mode 0.2 hardware validation", "No community upload or production server acceptance test", "Portable native regressions and the GSP build cover compiled UI behavior but not on-device touch/audio interaction or recovery transitions"])
(ROOT / "evidence/validation.json").write_text(json.dumps(result, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
print(json.dumps({k: result[k] for k in ["status", "size", "sha256", "app_descriptor", "unchanged_main_ui_pc_file_count"]}, ensure_ascii=False, indent=2))
