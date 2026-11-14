#!/usr/bin/env python3
"""Makes the Haiku package of the game, dist/opence-<n>-1-x86_64.hpkg, which
pkgman installs and updates (the release workflow attaches it to each
release, with the zip).

Run it on Haiku after building, with the build's number:

    python3 tools/haiku_package.py 8

It needs Haiku's own tools (package, addattr), so it runs on Haiku only;
the zip (tools/haiku_release.py) can be packed anywhere.

The package installs:
- apps/OpenCE/: the two programs (halo, with the icon in its BEOS:ICON
  attribute, and halo_guest.elf), the network lists, README-Haiku.md and the
  licences, as the zip has them;
- data/deskbar/menu/Applications/OpenCE: a link to apps/OpenCE/halo, the
  Deskbar's entry;
- data/licenses/GNU AGPL v3: the licence, which Haiku does not ship.

It holds no game data. Installed this way, the game's folder is read-only
(packagefs), and the game knows it (platform_installed_package,
xbox_files.c): its own updater stays off, and config.toml and the maps
folder it extracts from the player's disc image go in
~/config/settings/halo-haikuos, with the saves.

It needs SDL 3.4.0 or later (libsdl3 >= 3.4.0: the package's own version,
as lib:libSDL3's is the library's, 0.4.x) and an OpenGL and EGL, Mesa's or
any other's (lib:libGL, lib:libEGL), but no particular driver: with a GPU
driver installed, the game draws with it.
"""

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build" / "haiku"
sys.path.insert(0, str(ROOT / "tools"))
from haiku_release import NOTICES  # noqa: E402  (the licences the zip carries)

ICON = ROOT / "port/haiku/package/OpenCE.hvif"

PACKAGE_INFO = """name			opence
version			{number}-1
architecture	x86_64
summary			"Halo: Combat Evolved, from the OpenCE decompilation"
description		"OpenCE is a port of the Halo: Combat Evolved decompilation, \\
with internet play, Custom Edition maps and the PC version's menus. It \\
needs the player's own Xbox disc image of the game: the first start asks \\
for it and extracts its maps to ~/config/settings/halo-haikuos. No game \\
data is included."
packager		"Techniix"
vendor			"Techniix"
copyrights		{{
	"OpenCE contributors"
}}
licenses		{{
	"GNU AGPL v3"
}}
urls			{{
	"https://github.com/Techniix/OpenCE_HaikuOS"
}}
provides		{{
	opence = {number}-1
	app:OpenCE = {number}
}}
requires		{{
	haiku >= r1~beta6
	libsdl3 >= 3.4.0
	lib:libGL
	lib:libEGL
}}
"""


def run(*command: str) -> None:
    subprocess.run(command, check=True)


def main() -> int:
    if len(sys.argv) != 2 or not sys.argv[1].isdigit():
        print(f"usage: {sys.argv[0]} <build number>", file=sys.stderr)
        return 2
    number = sys.argv[1]
    if not shutil.which("package") or not shutil.which("addattr"):
        print("the package and addattr tools are Haiku's: run this on Haiku", file=sys.stderr)
        return 1
    programs = [BUILD / "halo", BUILD / "halo_guest.elf"]
    for program in programs:
        if not program.is_file():
            print(f"{program.relative_to(ROOT)} is missing: build first", file=sys.stderr)
            return 1

    staging = ROOT / "dist" / "opence-package"
    if staging.exists():
        shutil.rmtree(staging)
    app = staging / "apps" / "OpenCE"
    app.mkdir(parents=True)
    for program in programs:
        shutil.copy2(program, app / program.name)
        os.chmod(app / program.name, 0o755)
    for network in ("brokers.txt", "masters.txt"):
        source = ROOT / "port/assets/network" / network
        if source.is_file():
            shutil.copy2(source, app / network)
    for source, target in NOTICES:
        shutil.copy2(source, app / target)
    shutil.copy2(ROOT / "port/haiku/README.md", app / "README-Haiku.md")
    # (Tracker and the Deskbar draw the program with the vector icon in its
    # BEOS:ICON attribute; the package keeps files' attributes)
    run("addattr", "-t", "icon", "-f", str(ICON), "BEOS:ICON", str(app / "halo"))

    # (a licence Haiku does not ship in /system/data/licenses must be in the
    # package, under the name its .PackageInfo gives: package create checks)
    licenses = staging / "data" / "licenses"
    licenses.mkdir(parents=True)
    shutil.copy2(ROOT / "LICENSE", licenses / "GNU AGPL v3")

    menu = staging / "data" / "deskbar" / "menu" / "Applications"
    menu.mkdir(parents=True)
    # (relative: the package may be installed in the system or in ~/config)
    (menu / "OpenCE").symlink_to("../../../../apps/OpenCE/halo")

    (staging / ".PackageInfo").write_text(PACKAGE_INFO.format(number=number))
    output = ROOT / "dist" / f"opence-{number}-1-x86_64.hpkg"
    output.unlink(missing_ok=True)
    run("package", "create", "-C", str(staging), str(output))
    run("package", "list", str(output))
    print(f"{output.relative_to(ROOT)}: {output.stat().st_size / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
