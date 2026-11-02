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
python3 configure.py
ninja
```

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

## Differences from the Linux build

- The window has no icon of its own.
- A click on an invite link in a browser does not start the game. The
  Linux build registers its link scheme with a freedesktop desktop entry,
  which Haiku does not read.
- Discord's game activity is not shown, because Discord has no Haiku client.
- The self-updater looks for `halo-haiku-<flavor>.zip` releases.
  The project does not make those releases at this time.

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
