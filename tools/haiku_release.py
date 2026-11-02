#!/usr/bin/env python3
"""Packs the Haiku build into dist/halo-haiku-<flavor>.zip for a GitHub
release, as tools/ci_build.py packs the other builds: the two programs, the
network lists, and the licences and notices that copies must carry.

Run it on Haiku after `ninja` (the host program, build/haiku/halo, is
built there):

    python3 tools/haiku_release.py            # release (configure.py --release)
    python3 tools/haiku_release.py debug      # a debug build
"""

import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "haiku"

# (source, name in the zip): OpenCE's licence, the AGPL, and those of what the
# build contains (the list tools/ci_build.py gives the other builds), with
# musl's, the game's C library on Haiku. SDL3 is the system's, not shipped.
NOTICES = [
    (ROOT / "LICENSE", "LICENSE.txt"),
    (ROOT / "port/third_party/mbedtls/LICENSE", "mbedtls-LICENSE.txt"),
    (ROOT / "port/third_party/miniupnpc/LICENSE", "miniupnpc-LICENSE.txt"),
    (ROOT / "port/assets/fonts/Overpass-OFL.txt", "Overpass-OFL.txt"),
    (ROOT / "port/third_party/expat/COPYING", "expat-COPYING.txt"),
    (ROOT / "port/third_party/opus/COPYING", "opus-COPYING.txt"),
    (ROOT / "port/third_party/jsmn/LICENSE", "jsmn-LICENSE.txt"),
    (ROOT / "port/assets/icons/lucide/LICENSE", "lucide-LICENSE.txt"),
    (ROOT / "port/third_party/musl-math/COPYRIGHT", "musl-COPYRIGHT.txt"),
]

README = """OpenCE for HaikuOS - Halo: Combat Evolved on Haiku ({flavor} build)

Needs Haiku R1/beta6 or later (x86_64) and SDL3:

    pkgman install libsdl3

1. Put your own Xbox disc image of Halo: Combat Evolved (.iso or .xiso)
   in this folder. No game data is included.
2. Start ./halo from a Terminal in this folder. The first start extracts
   the maps folder (about 2 GB; the game's cache files take about 800 MB
   more).

Saves and settings: ~/config/settings/halo-haikuos
If the menus are cut off, use Borderless (Settings > Video Setup, or F11).
More: README-Haiku.md, and https://github.com/Techniix/OpenCE_HaikuOS

This is free software under the GNU AGPL 3.0 or later (LICENSE.txt). Its
source is at https://github.com/Techniix/OpenCE_HaikuOS. The other files
here are the licences of the parts it contains.
"""


def main() -> int:
    flavor = sys.argv[1] if len(sys.argv) > 1 else "release"
    if flavor not in ("release", "debug"):
        print(f"usage: {sys.argv[0]} [release|debug]", file=sys.stderr)
        return 2
    programs = [BUILD / "halo", BUILD / "halo_guest.elf"]
    for program in programs:
        if not program.is_file():
            print(f"{program.relative_to(ROOT)} is missing: build first (python3 configure.py"
                  f"{' --release' if flavor == 'release' else ''}, then ninja)", file=sys.stderr)
            return 1
    name = f"halo-haiku-{flavor}"
    dist = ROOT / "dist" / name
    if dist.exists():
        shutil.rmtree(dist)
    dist.mkdir(parents=True)
    for program in programs:
        shutil.copy2(program, dist / program.name)
    for network in ("brokers.txt", "masters.txt"):
        source = ROOT / "port/assets/network" / network
        if source.is_file():
            shutil.copy2(source, dist / network)
    for source, target in NOTICES:
        shutil.copy2(source, dist / target)
    shutil.copy2(ROOT / "port/haiku/README.md", dist / "README-Haiku.md")
    (dist / "README.txt").write_text(README.format(flavor=flavor))

    archive = ROOT / "dist" / f"{name}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zip_file:
        for path in sorted(dist.iterdir()):
            info = zipfile.ZipInfo.from_file(path, f"{name}/{path.name}")
            # (the programs keep their execute permission: unzip restores it)
            info.external_attr = (0o755 if path.name in ("halo", "halo_guest.elf") else 0o644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            zip_file.writestr(info, path.read_bytes())
    print(f"{archive.relative_to(ROOT)}: {archive.stat().st_size / 1e6:.1f} MB, {len(list(dist.iterdir()))} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
