"""Fetch pinned public sources into ignored upstream/. Does not execute downloaded code."""
import json
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def main():
    lock = json.loads((ROOT / "sources.lock.json").read_text())
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("names", nargs="*", help="Optional source names; default: all pinned sources")
    args = ap.parse_args()
    if set(args.names) - set(lock): ap.error("Unknown source name")
    for name, source in lock.items():
        if args.names and name not in args.names: continue
        dest = ROOT / "upstream" / name
        if not dest.exists():
            dest.mkdir(parents=True)
            subprocess.run(["git", "init", str(dest)], check=True)
            subprocess.run(["git", "-C", str(dest), "remote", "add", "origin", source["url"]], check=True)
        def git(*args):
            return subprocess.check_output(["git", "-C", str(dest), *args], text=True).strip()
        if git("remote", "get-url", "origin").lower() != source["url"].lower():
            raise SystemExit(f"Unexpected remote: {dest}")
        if git("status", "--porcelain"):
            raise SystemExit(f"Refusing to change dirty checkout: {dest}")
        if name == "ti-sdk":
            git("config", "remote.origin.promisor", "true")
            git("config", "remote.origin.partialclonefilter", "blob:none")
            git("sparse-checkout", "init", "--cone")
            git("sparse-checkout", "set", "source/ti/devices/cc13x2_cc26x2", "source/ti/zstack")
        options = ["--filter=blob:none"] if name == "ti-sdk" else []
        git("fetch", *options, "--depth", "1", "origin", source["commit"])
        git("checkout", "--detach", source["commit"])
        print(name, git("rev-parse", "HEAD"))

if __name__ == "__main__":
    main()
