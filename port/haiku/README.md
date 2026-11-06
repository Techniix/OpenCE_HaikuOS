# Haiku

`ninja haiku` builds the game for 64-bit [Haiku](https://www.haiku-os.org/)
(x86_64). On Haiku, `ninja` alone builds it too. The result is two files:

- `build/haiku/halo`, the program that you start.
- `build/haiku/halo_guest.elf`, the game itself, which `halo` loads.

Keep the two files together. Copy `build/haiku/brokers.txt` and
`build/haiku/masters.txt` beside them for Internet play.

The game shows its graphics with OpenGL (Mesa) and plays sound through
SDL3. Haiku has no 3D drivers, so OpenGL is drawn on the processor. Refer to
"Performance".

## How it works

The game's code assumes 32-bit pointers. 64-bit Haiku cannot run 32-bit
programs, so the Haiku build uses the arrangement of the Android build:

- The game is compiled for the x32 ABI: x86-64 instructions with 32-bit
  pointers. Its C library is musl (`port/haiku/guest`). The game never calls
  the kernel itself.
- `halo` is a native Haiku program (`port/haiku/host`). It reserves the low
  4 GB of its address space, loads the game there, and performs the game's
  system calls, OpenGL calls and SDL calls with Haiku's own libraries. It
  converts what differs between Linux and Haiku: errno values, file flags,
  socket addresses, futexes.

The game uses the platform layer of the Linux build (`port/linux/src`).
Refer to [port/linux/README.md](../linux/README.md).

## Requirements

- Haiku R1/beta6 or later, x86_64. R1/beta5 can no longer install packages
  from HaikuPorts. To update R1/beta5, enter these commands and restart:

  ```sh
  pkgman add-repo https://eu.hpkg.haiku-os.org/haiku/r1beta6/$(getarch)/current
  pkgman add-repo https://eu.hpkg.haiku-os.org/haikuports/r1beta6/$(getarch)/current
  pkgman full-sync
  ```

- The build tools and SDL3. Enter `pkgman search clang` and use the newest
  LLVM version that it shows (23 in this example):

  ```sh
  pkgman install cmd:git cmd:python3 cmd:ninja cmd:cmake cmd:pkg_config
  pkgman install llvm23_clang llvm23_lld llvm23 libsdl3 libsdl3_devel
  ```

  The game needs SDL 3.4.0 or later. If the system's SDL3 is older, the
  build compiles SDL 3.4.16 itself.

- A network connection for the first build. `configure.py` downloads musl
  1.2.5 and SDL 3.4.16 to `build/haiku/third_party`.

- Disk space: about 2 GB for the `maps` folder that the game extracts from
  the disc image, and about 800 MB for the game's cache files. Haiku's file
  system does not have sparse files, so the cache files use their full size.

To make sure that clang can compile for the game's ABI, enter:

```sh
echo 'int f(void){return sizeof(void*);}' > /tmp/t.c
clang --target=x86_64-linux-gnux32 -c /tmp/t.c -o /tmp/t.o && file /tmp/t.o
```

The result must show `ELF 32-bit LSB relocatable, x86-64`.

## Build

```sh
python3 configure.py --release
ninja
```

Without `--release`, the build is a debug build: a failed assertion stops
the game and is written to the log. Use it to find and report problems.

The option `--haiku-guest-cc` of `configure.py` selects a different clang for
the game. `ld.lld` and `llvm-ar` are taken from the same folder. The option
`--haiku-host-cc` selects the compiler for `halo`.

If `tar` or `ninja` says that file times are in the future, the clock of
the computer is wrong. This occurs in virtual machines. Set the time zone and
the clock in Preferences → Time, then enter `find . -exec touch {} +` and
build again.

On Linux, `ninja haiku` builds the same game and a Linux version of `halo`.
The project uses that build to test the Haiku code paths without Haiku.

## Play

Put your Xbox disc image (`.iso` or `.xiso`) beside `halo`, then start
`halo`. Haiku has no file picker that the game can use, so the disc image
must be in that folder. The first start extracts the `maps` folder.

```sh
mkdir -p ~/halo
cp build/haiku/halo build/haiku/halo_guest.elf build/haiku/*.txt ~/halo/
cp /path/to/halo.iso ~/halo/
cd ~/halo && ./halo
```

Saved games, profiles and settings are in `~/config/settings/halo-haikuos`.
Saves from earlier builds, in `~/config/non-packaged/data/halo-linux`, move
there at the next start.

To stop a game that does not respond, press Ctrl+Alt+Del. Team Monitor opens.
Select `halo` and click Kill Application.

## Performance

OpenGL is drawn on the processor (Mesa's llvmpipe). These settings make it
faster:

- Display mode Borderless, at the display's size. This is the default. F11
  switches between it and a window.
- Anti-aliasing off. This is the default. Supersampling (`ssaa2x`) draws four
  times as many pixels.
- Resolution scaling 75% or 50%.
- In a virtual machine, more processor cores. llvmpipe uses all of them.
  VirtualBox's 3D acceleration does not operate with Haiku.

## Releases and updates

The game updates itself from this repository's releases, as the other
builds do from OpenCE's (`port/linux/src/updater.c`). At start-up, a
numbered build asks GitHub for the repository's latest release. If its tag,
`build-<n>`, is newer than the build's own number, the game offers to
download its `halo-haiku-release.zip`, put its files in place and start
again. A build has the number and the repository it was configured with:
`HALO_BUILD_NUMBER` and `GITHUB_REPOSITORY` in the environment of
`configure.py`, which GitHub Actions gives. A build without a number (any
build of your own) never looks for updates. GitHub does not count drafts and
pre-releases as the latest release, so players never get those.

`.github/workflows/haiku-release.yml` makes the releases. Each night, and
when you run it by hand, it:

1. merges OpenCE's `main` into this fork's `main`. This fork's `README.md`
   is kept (`.gitattributes`). A merge that conflicts opens an issue and
   stops.
2. builds the result in a Haiku R1/beta6 virtual machine as
   `build-<n>` (the highest `build-` release, drafts too, plus 1), and checks
   that `halo --version` runs the game and names that build;
3. moves `main` to the merged code and makes a **draft** release with the
   zip.

Nothing reaches players until you publish the draft. Download its zip, play
it for a while, then click Publish release, with "Set as the latest
release" ticked. A run that fails opens an issue: most often OpenCE has
started to use an SDL or system function that the Haiku port does not
bridge yet (`port/haiku/host_imports.list`, `guest/runtime/guest_sdl.c`).

To set it up, once:

1. Make a fine-grained personal access token (GitHub: Settings → Developer
   settings → Personal access tokens → Fine-grained tokens) for this
   repository only, with the **Contents** and **Workflows** permissions set
   to Read and write. Add it to the repository as the secret `SYNC_TOKEN`
   (Settings → Secrets and variables → Actions). GitHub's own token cannot
   push a merge that changes OpenCE's workflows; this one is used for the
   pushes only.
2. In the Actions tab, enable workflows. Then disable OpenCE's own workflows,
   **Build** and **Server image**: in this fork they would publish Linux and
   Windows builds as `build-<n>` releases and delete older ones. (Each run
   of the Haiku workflow disables them too.)
3. Run "Haiku release" by hand (Actions → Haiku release → Run workflow),
   and tick "force" for the first run.

GitHub stops scheduled workflows in a repository that has had no activity
for 60 days; the Actions tab then offers to enable it again.

A release by hand: build with a number, pack it, and attach the zip to a
release whose tag has that number:

```sh
HALO_BUILD_NUMBER=5 GITHUB_REPOSITORY=Techniix/OpenCE_HaikuOS python3 configure.py --release
ninja
python3 tools/haiku_release.py
```

## GPU acceleration (experimental)

Haiku's own OpenGL is drawn by the processor. On a computer with an NVIDIA
GPU of the Turing generation or later (GeForce GTX 16 and RTX 20 series and
later), an NVIDIA driver set for Haiku can draw it with the GPU: NVIDIA's
open kernel driver, Mesa's NVK Vulkan driver and Mesa's Zink, which is
OpenGL over Vulkan. Two exist, both alpha software that takes the place of
Haiku's Mesa package, so try them on a separate Haiku installation on real
hardware (they do not work in a virtual machine):

- X547's [Nebula](https://github.com/X547/nvidia-haiku). Download the
  `libglvnd` and `nebula` `.hpkg` files from its releases and install them
  with `pkgman install ./<file>.hpkg`, accepting the removal of `mesa`.
  Nebula 0.0.2 does not start GeForce RTX 50 cards.
- [Techniix/nvidia-haiku](https://github.com/Techniix/nvidia-haiku), a fork
  with GeForce RTX 50 support. Follow its README.

Restart, then start the game from Terminal. Its first lines say which driver
draws:

```
host info: OpenGL lookups: SDL, Mesa's glapi not found, EGL found
host info: OpenGL: <n> functions from SDL, 0 from Mesa's glapi, <n> from EGL, 0 missing
host info: OpenGL driver: <vendor>, <renderer>, version <version> (hardware)
```

`(hardware)` means the GPU draws. `software: drawn by the processor`
means Haiku's own OpenGL is in use.

Tested on a GeForce RTX 5070 with Techniix/nvidia-haiku: menus, levels and
online play work; frame rates are uneven and the picture can tear.

- If a level freezes while it loads, the driver's occlusion queries may be
  faulting the GPU (look for `Warp Exception` in `/var/log/syslog`).
  `debug.gpu_visibility_queries = false` in `config.toml` (or
  `HALO_GPU_VISIBILITY_QUERIES=false`) skips them; lens flares and lights
  then draw even behind walls.
- The drivers' libGL is glvnd's, which exports fewer functions than Mesa's,
  and there is no `libglapi`. `host_gl.c` asks `eglGetProcAddress` for the
  functions that SDL does not find. `HALO_GL_LOOKUP` (`sdl`, `glapi` or
  `egl`) makes it use one lookup only, to compare them.

## Differences from the Linux build

- The window has no icon of its own.
- A click on an invite link in a browser does not start the game. The
  Linux build registers its link scheme with a freedesktop desktop entry,
  which Haiku does not read.
- Discord's game activity is not shown, because Discord has no Haiku client.
- The self-updater takes this repository's releases (the repository a build
  was made from), and `halo-haiku-<flavor>.zip` in them. Refer to
  "Releases and updates".

## Haiku-specific details for developers

These are the differences that the port had to handle:

- A thread with a stack from `pthread_attr_setstack` gets its TLS block at
  the top of that stack, at the stack's base plus the guard size, which is
  not ignored. `host_thread.c` sets the guard size to 0. Otherwise the TLS
  block is outside the stack and the thread stops before its first
  instruction.
- SDL's Haiku backend finds OpenGL functions only among the symbols that
  `libGL.so` exports. Mesa does not export the newer ones (`glClipControl`,
  `glCreateQueries`). `host_gl.c` asks `_glapi_get_proc_address` in
  `libglapi.so` for them.
- `sockaddr_in` has a length byte, an 8-bit family (`AF_INET` is 1) and 32
  bytes. `posix_net.c` converts the game's Winsock addresses.
- The process ends with `_exit()`. `exit()` from the game's thread waits for
  the application kit that SDL started, and does not return.
- `getentropy` instead of `getrandom`, `IFF_LINK` instead of `IFF_RUNNING`,
  `ucontext_t` in `<signal.h>`, and `ioctl` takes the size of its argument.
- The certificate authorities are in
  `/boot/system/data/ssl/CARootCertificates.pem`.
