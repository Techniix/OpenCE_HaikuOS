# OpenCE for HaikuOS - Halo: Combat Evolved on Haiku

<p align="center">
  <img src="port/assets/icon/opence-icon.png" alt="OpenCE" width="128">
</p>

> [!WARNING]
> **This is the experimental `gpu-acceleration` branch, and it is untested.**
> It adds support for GPU-accelerated OpenGL with X547's
> [Nebula](https://github.com/X547/nvidia-haiku) driver set on NVIDIA GPUs
> (Turing and later), but nobody has run it on Haiku with Nebula yet. It
> may not work, or may not start. For a build that has been tested, use the
> [`main`](https://github.com/Techniix/OpenCE_HaikuOS) branch. Refer to
> "GPU acceleration" below.

This is a fork of [OpenCE](https://github.com/OpenCommunityEdition/OpenCE)
(Open Community Edition) that adds a native port to
[Haiku](https://www.haiku-os.org/), the open source successor of BeOS.

**OpenCE** is an open source port of the Halo: Combat Evolved decompilation,
based on an early Xbox development build (2342). It adds 128-player
multiplayer, online campaign co-op, a server browser, peer-to-peer hosting,
mods, Custom Edition maps and much more. This fork keeps all of that and
makes it run on 64-bit Haiku. The other platforms (Windows, Linux, Android
and the web) are upstream's: refer to
[the OpenCE repository](https://github.com/OpenCommunityEdition/OpenCE).

## Status

On Haiku, these work: the menus, the campaign, the server browser, joining
internet games, sound, and quitting.

> **Tested in virtual machines only.** The port was made and tested in
> VirtualBox (Haiku R1/beta6, x86_64), where OpenGL is drawn by the
> processor. It has not been tried on real hardware yet. If you have Haiku
> on a real computer, please try it, and
> [open an issue](https://github.com/Techniix/OpenCE_HaikuOS/issues) with
> your hardware, your Haiku version (`uname -a`) and what happened. Reports
> that it works are welcome too.

### If something looks wrong

- **The menus are cut off at the edges, or the window does not fit the
  screen:** use **Borderless** fullscreen. It is the default. Set it in
  Settings → Video Setup → Display Mode, press **F11**, or put
  `display.mode = "borderless"` in `config.toml`.
- **The game is slow:** Haiku has no 3D drivers, so OpenGL is drawn by the
  processor. Keep Borderless at the display's size and anti-aliasing off,
  and try resolution scaling at 75% or 50%. In a virtual machine, give it
  more processor cores.
- **The server browser shows no games:** check the computer's clock and
  time zone (Preferences → Time). The game refuses a server list that seems
  to be from the future.
- **The game does not respond:** press **Ctrl+Alt+Del** to open Team
  Monitor, select `halo` and click Kill Application.

## GPU acceleration (experimental branch, untested)

The [`gpu-acceleration`](https://github.com/Techniix/OpenCE_HaikuOS/tree/gpu-acceleration)
branch is for computers with an NVIDIA GPU of the Turing generation or
later (GeForce GTX 16 and RTX 20 series and later). With X547's
[Nebula](https://github.com/X547/nvidia-haiku) driver set, the GPU could
draw the game instead of the processor.

> **Nobody has tested this branch yet.** It was made without the hardware
> and has not been run on Haiku with Nebula. Nebula itself is alpha
> software, and it replaces Haiku's Mesa package, so use a separate Haiku
> installation on real hardware. For playing, use `main`.

This is that branch. If you are a brave volunteer with such a GPU, get it
with:

```sh
git clone -b gpu-acceleration https://github.com/Techniix/OpenCE_HaikuOS.git
```

Then build the game as in "Build and play on Haiku", and follow
"GPU acceleration with Nebula" in
[port/haiku/README.md](port/haiku/README.md#gpu-acceleration-with-nebula-experimental).
[Open an issue](https://github.com/Techniix/OpenCE_HaikuOS/issues) with
your GPU, your Haiku and Nebula versions, the first lines that the game
prints (the `host info: OpenGL` lines say which driver draws), and how it
played. Reports that it does not work help as much as reports that it does.

## A copy of Halo: Combat Evolved is required

This project does not include the game data. You **must provide your own
Xbox disc image** (`.iso` or `.xiso`) of Halo: Combat Evolved. Every version
of the game works.

## Build and play on Haiku

The full guide is [port/haiku/README.md](port/haiku/README.md). In short:

### 1. Requirements

- Haiku **R1/beta6** or later, 64-bit (x86_64). R1/beta5 can no longer
  install packages; [port/haiku/README.md](port/haiku/README.md) says how to
  update it.
- About **3 GB of free disk space**: about 2 GB for the maps and about
  800 MB for the game's cache files.
- The tools and SDL3. Enter `pkgman search clang` and use the newest LLVM
  version that it shows (23 in this example):

  ```sh
  pkgman install cmd:git cmd:python3 cmd:ninja cmd:cmake cmd:pkg_config
  pkgman install llvm23_clang llvm23_lld llvm23 libsdl3 libsdl3_devel
  ```

### 2. Build

```sh
git clone https://github.com/Techniix/OpenCE_HaikuOS.git
cd OpenCE_HaikuOS
python3 configure.py --release
ninja
```

`--release` makes the build for playing. Without it, the build is a debug
build, which stops at the first failed check; refer to "Build options".
The first build downloads musl and SDL3's source, so it needs a network
connection. It takes 10 to 30 minutes.

### 3. Play

Put the game's files and your disc image in a folder, then start `halo`:

```sh
mkdir -p ~/halo
cp build/haiku/halo build/haiku/halo_guest.elf build/haiku/*.txt ~/halo/
cp /path/to/your/halo.iso ~/halo/
cd ~/halo
./halo
```

Haiku has no file picker that the game can use, so the disc image must be
next to `halo`. At the first start, the game extracts the `maps` folder
from it.

Your saved games, profiles and settings are in
`~/config/settings/halo-haikuos`.

### Updating

```sh
cd OpenCE_HaikuOS
git pull
python3 configure.py --release
ninja
cp build/haiku/halo build/haiku/halo_guest.elf build/haiku/*.txt ~/halo/
```

## Download

Ready-made Haiku builds are on this fork's
[Releases](https://github.com/Techniix/OpenCE_HaikuOS/releases) page, as
`halo-haiku-release.zip`. Unpack it, install SDL3 (`pkgman install libsdl3`),
put your disc image in the folder and start `halo`. Its `README.txt` says
the rest. These builds update themselves: when a newer release is out, the
game offers to install it when it starts. The downloads for Windows, Linux,
Android and the web are
[upstream's releases](https://github.com/OpenCommunityEdition/OpenCE/releases/latest).

The releases are built automatically from OpenCE's latest code, in a Haiku
virtual machine on GitHub, and published after a test. To make such a zip
from your own build, enter `python3 tools/haiku_release.py` after building.
It writes `dist/halo-haiku-release.zip`, with the licences that copies must
carry. Refer to "Releases and updates" in
[port/haiku/README.md](port/haiku/README.md#releases-and-updates).

## Build options

Give these options to `configure.py`:

| Option | Result |
| --- | --- |
| (none) | A debug build. A failed assertion stops the game and is written to the log: use it to find and report problems. |
| `--release` | A release build. The game does not examine assertions, as in the retail game. Use it to play. |
| `--profile` | A profiling build. It records CPU times. |
| `--haiku-guest-cc PATH` | The clang that compiles the game (`ld.lld` and `llvm-ar` are taken from its folder). |
| `--haiku-host-cc PATH` | The compiler for `halo`. |

The Haiku build always operates on every x86-64 processor, and it has no
link-time or profile-guided optimization. The options of the other builds
(`--portable`, `--lto`, `--pgo`, `--web-emcc`) are in
[upstream's README](https://github.com/OpenCommunityEdition/OpenCE#build-options).

## How the port works

The game's code assumes 32-bit pointers, and 64-bit Haiku cannot run 32-bit
programs. So the game is compiled for the x32 ABI (64-bit instructions with
32-bit pointers), and a small native Haiku program, `halo`, loads it and
carries out its system calls, OpenGL and SDL calls with Haiku's own
libraries. OpenCE's Android build works the same way. The details, and the
Haiku-specific problems that the port solves, are in
[port/haiku/README.md](port/haiku/README.md).

## What OpenCE adds

Everything that OpenCE has works the same on Haiku. Refer to
[the OpenCE README](https://github.com/OpenCommunityEdition/OpenCE#readme)
and [port/linux/README.md](port/linux/README.md) for the controls, every
setting, multiplayer, Custom Edition maps (in a `custom_maps` folder beside
`maps`) and mods (in a `mods` folder beside `maps`). Haiku players play with
the players of every other platform.

## Credits

The Haiku port is by Techniix. OpenCE is the work of its contributors; the
main contributors are Jonas Volman (@bnunu) and MrBruh (@cybersecurity).
OpenCE starts from the decompilation of
[bnunu/halo-1](https://github.com/bnunu/halo-1), which is a fork of
[punpckhdq/halo](https://github.com/punpckhdq/halo). The decompilation is of
the Xbox build 2342 (`cachebeta.exe`, SHA-256
`4cc87b45f721270392a96f1674ed2b5cd4a7bb4355faeab4531d1cf1884d9520`). The
artists of the high-res redraws are in [AUTHORS.md](AUTHORS.md).

## Licence

OpenCE is free software: you can give it to others and change it under the
terms of the GNU Affero General Public License as published by the Free
Software Foundation, version 3 of the License or (at your option) any later
version (`AGPL-3.0-or-later`). It comes with no warranty. The full text is in
[LICENSE](LICENSE).

- **From the change.** The AGPL applies from the commit that made the change,
  and to each release built from that commit or a later one. Earlier
  releases, and the source at every earlier commit, were released under
  [CC0 1.0](LICENSES/CC0-1.0.txt) and stay that way.
- **What OpenCE builds on.** The decompilation of
  [punpckhdq/halo](https://github.com/punpckhdq/halo) is under the AGPL 3.0
  from 17 October 2026, and under CC0 1.0 before that date.
  [bnunu/halo-1](https://github.com/bnunu/halo-1) is under CC0 1.0.
- **Contributions** are under `AGPL-3.0-or-later`. A change taken from a
  project under GPL 3.0 keeps that licence and its notices; the GPL 3.0 and
  the AGPL 3.0 let the two be combined (section 13 of each).
- **Hosts and servers.** If you change the game and let other persons play
  with your version over a network (for example, as the host of an internet
  game, or on a web site), the AGPL asks you to offer them the source of
  your version (section 13). The source of this fork, the Haiku port, is
  this repository.
- **Third-party code, fonts and art** keep their own licences, which are
  next to them (`port/third_party`, `port/assets/fonts`,
  `port/assets/icons`, `port/assets/text_filter`).
- **Halo is not covered.** The licence covers the work of OpenCE's authors.
  It gives no rights in Halo, its maps or anything else that Microsoft owns.

## Disclaimer

OpenCE (Open Community Edition) is an unofficial fan project. It is not endorsed by or affiliated with Microsoft, Xbox Game Studios, Halo Studios, Activision or Bungie. Halo and Halo: Combat Evolved are trademarks of Microsoft Corporation; Halo © Microsoft Corporation.
No game data is included. You must provide your own copy of the game.
