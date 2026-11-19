"""Set up the complete TI SDK and standalone Windows tools inside this workspace."""
import os
import argparse
from pathlib import Path
import subprocess
import sys

from download_ti_toolchain import PACKAGES

ROOT = Path(__file__).resolve().parents[1]

def mapped_root():
    # A reversible alias for legacy tools that cannot handle spaces in paths.
    alias = Path("Z:/")
    if alias.exists():
        if not alias.samefile(ROOT):
            raise SystemExit("Z: already belongs to another directory; no change made")
    else:
        subprocess.run(["subst", "Z:", str(ROOT)], check=True)
        if not alias.samefile(ROOT):
            raise SystemExit("Workspace drive alias verification failed")
    return alias

def main():
    if os.name != "nt": raise SystemExit("This setup script targets Windows")
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sdk-version',choices=['8.30.01.01','8.33.00.16'],default='8.30.01.01')
    args=ap.parse_args()
    sdk = ROOT / ('vendor-sdk/simplelink_cc13xx_cc26xx_sdk_'+args.sdk_version.replace('.','_'))
    if not (sdk / ".extraction-verified.json").is_file():
        subprocess.run([sys.executable, str(ROOT / "tools/download_ti_toolchain.py"), "sdk833" if args.sdk_version=='8.33.00.16' else "sdk", "--extract"], check=True)
    alias = mapped_root()
    logs = ROOT / "build/ti-toolchain-discovery"
    logs.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["__COMPAT_LAYER"] = "RunAsInvoker"
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    for name, executable in [("compiler", "compiler/ti-cgt-armllvm_4.0.2.LTS/bin/tiarmclang.exe"),
                             ("sysconfig", "sysconfig/sysconfig_cli.bat")]:
        subprocess.run([sys.executable, str(ROOT / "tools/download_ti_toolchain.py"), name], check=True)
        if (ROOT / ".tooling" / executable).is_file(): continue
        installer = ROOT / ".tooling/downloads" / PACKAGES[name]["url"].rsplit("/", 1)[1]
        print("Installing local TI", name, flush=True)
        with (logs / (name + "-install-output.txt")).open("wb") as output:
            result = subprocess.run([str(installer), "--mode", "unattended", "--prefix",
                str(alias / ".tooling" / name), "--debugtrace", str(logs / (name + "-install.trace"))],
                env=env, startupinfo=startup, stdout=output, stderr=subprocess.STDOUT)
        if result.returncode or not (ROOT / ".tooling" / executable).is_file():
            raise SystemExit(f"TI {name} installation failed; see {logs}")
    subprocess.run([str(alias / ".tooling/compiler/ti-cgt-armllvm_4.0.2.LTS/bin/tiarmclang.exe"), "--version"], check=True)
    subprocess.run([str(alias / ".tooling/sysconfig/nodejs/node.exe"),
                    str(alias / ".tooling/sysconfig/dist/cli.js"), "--version"], check=True)
    print("TI toolchain ready. No CCS IDE or device driver installation is required.")

if __name__ == "__main__":
    main()
