"""Install the pinned standalone herdsman runtime; never open a serial port.

No Zigbee2MQTT service or MQTT broker is installed. npm ci checks the locked
package integrity values. All dependencies/cache stay inside this workspace.
"""
from pathlib import Path
import json
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    node = shutil.which("node")
    if not node: raise RuntimeError("Node.js 22.2+ is required")
    npm = Path(node).parent / "node_modules/npm/bin/npm-cli.js"
    if not npm.is_file():
        npm = Path(node).resolve().parent.parent / "lib/node_modules/npm/bin/npm-cli.js"
    if not npm.is_file(): raise RuntimeError("Cannot locate npm next to the active Node.js installation")
    lock = ROOT / "tools/herdsman-lab-lock.json"
    locked = json.loads(lock.read_text(encoding="utf-8"))
    package = locked["packages"]["node_modules/zigbee-herdsman"]
    if package["version"] != "11.2.1" or not package["resolved"].startswith("https://registry.npmjs.org/"):
        raise ValueError("Unexpected herdsman dependency lock")
    destination = ROOT / ".tooling/herdsman-lab"
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(lock, destination / "package-lock.json")
    (destination / "package.json").write_text(json.dumps({"private": True,
        "dependencies": locked["packages"][""]["dependencies"]}, indent=2) + "\n", encoding="utf-8")
    subprocess.run([node, str(npm), "--prefix", str(destination), "--cache", str(ROOT / ".cache/npm"),
                    "ci", "--no-audit", "--no-fund"], cwd=ROOT, check=True)
    subprocess.run([node, str(ROOT / "tools/herdsman_lab.cjs"), "--check-runtime"], cwd=ROOT, check=True)

if __name__ == "__main__": main()
