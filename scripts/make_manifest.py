#!/usr/bin/env python3
"""Write manifest.json for a PlaneSpotter release.

Usage: make_manifest.py <version> <tag> <dist_dir> [notes]

Expects <dist_dir>/planespotter-<variant>.bin for each variant. The manifest
points at the stable github.com release URLs (never the signed redirect
targets), which is what the firmware's URL check requires.
"""
import hashlib
import json
import sys
from pathlib import Path

REPO = "bigtreacle/planespotter"
VARIANTS = ["cyd", "cyd2usb"]


def main() -> None:
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    version, tag, dist = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    notes = sys.argv[4] if len(sys.argv) > 4 else ""
    builds = {}
    for variant in VARIANTS:
        path = dist / f"planespotter-{variant}.bin"
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        # TAMPER TEST BUILD: publish a wrong hash so devices must reject the download.
        digest = ("0" if digest[0] != "0" else "1") + digest[1:]
        builds[variant] = {
            "url": f"https://github.com/{REPO}/releases/download/{tag}/{path.name}",
            "sha256": digest,
        }
        print(f"{variant}: {path.stat().st_size} bytes, sha256 {digest}")
    manifest = {"version": version, "notes": notes, "builds": builds}
    (dist / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print((dist / "manifest.json").read_text())


if __name__ == "__main__":
    main()
