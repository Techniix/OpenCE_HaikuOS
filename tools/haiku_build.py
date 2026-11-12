"""Ninja rules for the Haiku build (``ninja haiku``).

The Haiku port (port/haiku/README.md) runs the game as x32 code - x86-64
instructions with 32-bit pointers, as the game's data formats require -
inside an ordinary 64-bit program, as the Android port does with ILP32
AArch64 code (port/android/README.md). Haiku's 64-bit kernel runs no 32-bit
programs, and its 32-bit one keeps the upper 2 GB the game's Xbox memory
window needs. This graph builds

- the guest image, build/haiku/halo_guest.elf: the game sources, the
  platform layer shared with the Linux port (port/linux/src) and the guest
  runtime (port/haiku/guest) with a subset of musl as its C library, all
  compiled by clang for x86_64-linux-gnux32 and linked by ld.lld at a fixed
  address below 4 GB;
- the host program, build/haiku/halo: the loader and the services the guest
  calls, over SDL3, OpenGL and the system's C library (port/haiku/host);

The host is compiled for the machine that runs configure.py: Haiku's, or
Linux's (x86-64), where the same program tests the guest and the host's
portable half without a Haiku machine.
"""

import hashlib
import os
import platform
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

from .linux_build import (MINIUPNPC_DEFINES, MINIUPNPC_DIR, MUSL_MATH_DIR, STB_VORBIS_SOURCE, XDK_INCLUDE,
                          compile_launcher, configuration_defines, game_defines_and_includes, game_sources,
                          miniupnpc_sources, musl_math_sources, opus_cflags, opus_sources, updater_defines,
                          xdk_headers)
from .embed_assets import hud_assets_build, hud_configure_inputs, server_ui_build
from .ninja_syntax import Writer

PORT_DIR = Path("port/haiku")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/haiku")
THIRD_PARTY = BUILD / "third_party"
TOML_DIR = Path("port/third_party/tomlc17")
EXPAT_DIR = Path("port/third_party/expat")
EXPAT_SOURCES = ("xmlparse.c", "xmlrole.c", "xmltok.c")
KCP_DIR = Path("port/third_party/kcp")
MBEDTLS_DIR = Path("port/third_party/mbedtls")
MONOCYPHER_DIR = Path("port/third_party/monocypher")
ZLIB_DIR = Path("port/third_party/zlib")
ZLIB_SOURCES = ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c")
ZLIB_DEFINES = ("-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg")
# (as tools/android_build.py's: the same musl, and the same SDL3's headers for
# the guest)
MUSL_VERSION = "1.2.5"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"
MUSL_SHA256 = "a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4"
SDL_TAG = "release-3.4.16"
SDL_COMMIT = "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_URL = "https://github.com/libsdl-org/SDL.git"

# The guest ABI: x86-64 code with 32-bit pointers. The C library is musl,
# whose system calls go to the host (port/haiku/guest/libc/arch/x32).
GUEST_ABI_FLAGS = [
    "--target=x86_64-linux-gnux32",
    # the ILP32 guest's code paths, and the desktop's (the x32 guest is a
    # desktop build: the Linux build's settings, menus and memory window)
    "-DHALO_X32_GUEST=1",
    # (the Haiku port's own names: its log prefix, save folder; platform.h)
    "-DHALO_HAIKU_PORT=1",
    # any x86-64 processor (SSE2), as the Linux build's --portable
    "-march=x86-64",
    "-nostdinc",
    "-fshort-wchar",
    "-fno-pic",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    # the thread pointer register (the fs base) is the host's
    "-femulated-tls",
    # MSVC's long double is double (the x32 ABI's is x87's 80 bits)
    "-mlong-double-64",
    # no fused multiply-add: the same results on every port
    "-ffp-contract=off",
    "-O2",
]

# as the Linux build (tools/linux_build.py), minus what only i386 needs: the
# x32 ABI aligns 64-bit members to 8 bytes as MSVC does (-malign-double) and
# returns small structures in registers (-freg-struct-return)
GUEST_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

# musl's parts the guest uses (as the Android guest's, tools/android_build.py)
MUSL_DIRECTORIES = [
    "conf", "ctype", "dirent", "env", "errno", "exit", "fcntl", "internal",
    "locale", "malloc", "malloc/mallocng", "math", "mman", "multibyte",
    "prng", "sched", "select", "signal", "stat", "stdio", "stdlib", "string",
    "time", "unistd",
]
MUSL_FILES = [
    "thread/__lock.c", "thread/__wait.c", "thread/__timedwait.c",
    "thread/__syscall_cp.c", "thread/vmlock.c", "thread/pthread_self.c",
    "thread/pthread_equal.c", "thread/pthread_once.c",
    "thread/pthread_setcancelstate.c", "thread/pthread_testcancel.c",
    "thread/default_attr.c", "thread/lock_ptc.c", "misc/getauxval.c",
    "linux/sysinfo.c", "misc/basename.c", "misc/dirname.c",
    "misc/realpath.c", "misc/uname.c", "misc/ioctl.c", "misc/getrlimit.c",
    "misc/syscall.c", "network/htonl.c", "network/htons.c", "network/ntohl.c",
    "network/ntohs.c", "network/inet_addr.c", "network/inet_aton.c",
    "network/inet_ntoa.c", "network/inet_pton.c", "network/inet_ntop.c",
]
MUSL_THREAD_PREFIXES = (
    "pthread_attr_", "pthread_cond", "pthread_mutex", "pthread_rwlock",
    "pthread_spin", "pthread_key", "sem_",
)
# replaced by the guest runtime (port/haiku/guest/runtime)
MUSL_EXCLUDE = {
    "env/__stack_chk.c", "env/__init_tls.c", "env/__libc_start_main.c",
    "env/__reset_tls.c", "malloc/oldmalloc", "thread/pthread_create.c",
    "string/explicit_bzero.c",
    "string/memcmp.c",
}
# game files that call variadic functions without a prototype in scope (as
# the Android guest's: tools/android_build.py, tools/android_abi_check.py)
VARIADIC_PROTOTYPE_FILES = {
    "source/ai/action_uncover.c", "source/ai/ai.c", "source/ai/ai_debug.c",
    "source/bungie_net/common/public_key_crypt.c", "source/camera/editor_flying_camera.c",
    "source/game/cheats.c", "source/game/game_engine.c", "source/game/players.c",
    "source/hs/hs.c", "source/interface/hud_nav_points.c",
    "source/networking/telnet_console.c", "source/rasterizer/xbox/rasterizer_xbox_errors.c",
    "source/render/render.c",
}
VARIADIC_PROTOTYPES = Path("port/android/include/halo_android_variadic_prototypes.h")

# the platform layer's units that run in the host (their structures are the
# host C library's): every posix_*.c, built with the host's ABI
HOST_POSIX_SOURCES = ("posix_files.c", "posix_net.c", "posix_server.c",
                      "posix_mcc_preview.c", "posix_mcc_share.c")


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def host_system() -> str:
    """"haiku" or "linux": the system configure.py runs on, for which the
    host is compiled"""
    return "haiku" if platform.system().lower().startswith("haiku") else "linux"


def check_sdl_commit(directory: Path) -> None:
    commit = subprocess.run(["git", "-C", str(directory), "rev-parse", "HEAD"], capture_output=True, text=True,
                            check=True).stdout.strip()
    if commit != SDL_COMMIT:
        shutil.rmtree(directory)
        raise SystemExit(f"SDL3 {SDL_TAG} is {commit}, not the pinned {SDL_COMMIT}")


def fetch_third_party() -> None:
    """Download musl and SDL3's source (configure time, once): musl is the
    guest's C library, SDL3's headers are the guest's view of SDL, and its
    source the host's SDL where the system has none"""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not MUSL_DIR.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        if digest != MUSL_SHA256:
            archive.unlink()
            raise SystemExit(f"{MUSL_URL}: SHA-256 {digest}, not the pinned {MUSL_SHA256}")
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)
        check_sdl_commit(SDL_DIR)


def _musl_sources() -> List[Path]:
    src = MUSL_DIR / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


# the oldest SDL3 the host can use as the system's (SDL_LoadPNG_IO and
# SDL_SavePNG are 3.4's)
SDL_MINIMUM_VERSION = "3.4.0"


def _system_sdl(host_cc: str) -> Optional[List[str]]:
    """the system's SDL3 (pkg-config), if it is new enough: [cflags, libs],
    or None"""
    pkg_config = shutil.which("pkg-config") or shutil.which("pkgconf")
    if not pkg_config:
        return None
    try:
        subprocess.run([pkg_config, f"--atleast-version={SDL_MINIMUM_VERSION}", "sdl3"], check=True,
                       capture_output=True)
        cflags = subprocess.run([pkg_config, "--cflags", "sdl3"], capture_output=True, text=True, check=True)
        libs = subprocess.run([pkg_config, "--libs", "sdl3"], capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return [cflags.stdout.strip(), libs.stdout.strip()]


def haiku_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "guest" / "runtime", PORT_DIR / "host", LINUX_DIR / "src",
            *hud_configure_inputs()]


def generate_haiku_build(n: Writer, sln: Any) -> None:
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file() or not (PORT_DIR / "host").is_dir():
        return
    if platform.machine().lower() not in ("x86_64", "amd64", "x86_64h"):
        n.comment("Haiku build: needs an x86-64 machine (the host is compiled for it)")
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        print(f"Haiku build disabled: cannot fetch musl/SDL3 ({error})", file=sys.stderr)
        return
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    system = host_system()
    guest_cc = getattr(sln, "haiku_guest_cc", None) or "clang"
    host_cc = getattr(sln, "haiku_host_cc", None) or ("clang" if shutil.which("clang") else "cc")
    # (lld and llvm-ar beside the guest's clang, or on the PATH)
    guest_bin = Path(shutil.which(guest_cc) or guest_cc).resolve().parent
    lld = next((str(guest_bin / name) for name in ("ld.lld",) if (guest_bin / name).is_file()), "ld.lld")
    llvm_ar = next((str(guest_bin / name) for name in ("llvm-ar",) if (guest_bin / name).is_file()), "llvm-ar")

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    arch = PORT_DIR / "guest" / "libc" / "arch" / "x32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    host_program = BUILD / "halo"
    python = "$python"

    n.comment(f"Haiku build (ninja haiku), its host compiled for {system}; see port/haiku/README.md")
    n.variable("haiku_guest_cc", guest_cc)
    n.variable("haiku_host_cc", host_cc)
    n.variable("haiku_lld", lld)
    n.variable("haiku_ar", llvm_ar)

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="haiku_alltypes",
        command=f"sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="HAIKU MUSL $out",
    )
    n.build(outputs=alltypes, rule="haiku_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="haiku_syscall_h",
        command="cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="HAIKU MUSL $out",
    )
    n.build(outputs=syscall_h, rule="haiku_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name="haiku_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="HAIKU MUSL $out",
    )
    n.build(outputs=version_h, rule="haiku_version_h")

    # the OpenGL entry points (desktop OpenGL, gl.h's list), from SDL3's
    # OpenGL headers
    guest_gl_c = gen_dir / "guest_gl.c"
    gl_imports = gen_dir / "gl_imports.list"
    sdl_gl_headers = [SDL_DIR / "include" / "SDL3" / "SDL_opengl.h",
                      SDL_DIR / "include" / "SDL3" / "SDL_opengl_glext.h"]
    n.rule(
        name="haiku_gl_stubs",
        command=(f"{python} tools/haiku_gl_stubs.py {LINUX_DIR}/src/gl.h "
                 + " ".join(str(h) for h in sdl_gl_headers) + f" {guest_gl_c} {gl_imports}"),
        description="HAIKU GL STUBS",
    )
    n.build(outputs=[guest_gl_c, gl_imports], rule="haiku_gl_stubs",
            implicit=[Path("tools/haiku_gl_stubs.py"), LINUX_DIR / "src" / "gl.h", *sdl_gl_headers])

    # the posix_*.c functions the host runs for the guest
    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    posix_headers = [LINUX_DIR / "src" / "posix.h", LINUX_DIR / "src" / "posix_server.h"]
    n.rule(
        name="haiku_posix_stubs",
        command=f"{python} tools/haiku_posix_stubs.py {guest_posix_c} {posix_imports} $in",
        description="HAIKU POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="haiku_posix_stubs", inputs=posix_headers,
            implicit=[Path("tools/haiku_posix_stubs.py")])

    imports_s = gen_dir / "imports.s"
    host_table_c = BUILD / "host" / "host_import_table.c"
    host_imports_list = PORT_DIR / "host_imports.list"
    n.rule(
        name="haiku_imports",
        command=f"{python} tools/haiku_imports.py --host-table {host_table_c} {imports_s} $in",
        description="HAIKU IMPORTS",
    )
    n.build(outputs=[imports_s, host_table_c], rule="haiku_imports",
            inputs=[host_imports_list, posix_imports, gl_imports],
            implicit=[Path("tools/haiku_imports.py")])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h, semantics_header,
                         platform_semantics_header]

    # ---------- guest compilation

    n.rule(
        name="haiku_guest_cc",
        command=f"{compile_launcher(sln)}$haiku_guest_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="HAIKU CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="haiku_guest_as",
        command="$haiku_guest_cc --target=x86_64-linux-gnux32 -c $in -o $out",
        description="HAIKU AS $out",
    )

    # (and, after musl's, the compiler's own headers: its intrinsics, which
    # Opus uses on x86)
    try:
        resource_dir = subprocess.run([guest_cc, "-print-resource-dir"], capture_output=True, text=True,
                                      check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        n.comment(f"Haiku build: {guest_cc} cannot be run")
        return
    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include", f"-idirafter {_quote(Path(resource_dir) / 'include')}",
    ]
    guest_abi = " ".join(GUEST_ABI_FLAGS + configuration_defines(sln))
    guest_code = " ".join(GUEST_CODE_FLAGS)

    def guest_object(source: Path, cflags: str, prefix: str = "") -> Path:
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        n.build(outputs=obj, rule="haiku_guest_cc", inputs=source, implicit=generated_headers,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{MUSL_DIR}/src/include", f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="haiku_ar",
        command="rm -f $out && $haiku_ar rcs $out @$out.rsp",
        description="HAIKU AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="haiku_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags),
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() == "source/main/main.c":
            cflags += " " + updater_defines(getattr(sln, "port_release", False))
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {VARIADIC_PROTOTYPES}"
        objects.append(guest_object(source, cflags))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags))
    objects.append(guest_object(STB_VORBIS_SOURCE, game_cflags))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w",
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{PORT_DIR}/guest/runtime",
        f"-I{PORT_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
        f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    # replaced by the guest runtime's (guest_memory_watch.c), or the host's
    # (posix_trace_marker.c: the GPU driver is the host's)
    guest_excluded = {"memory_watch.c", "posix_trace_marker.c"}
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_excluded:
            continue
        cflags = platform_cflags
        if source.name in ("updater.c", "server_mode.c"):
            cflags += " " + updater_defines(getattr(sln, "port_release", False))
        objects.append(guest_object(source, cflags))
    for source in hud_assets_build(n, "haiku", gen_dir / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    for source in server_ui_build(n, "haiku", gen_dir / "server_ui_assets.c"):
        objects.append(guest_object(source, platform_cflags))
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags))
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags))
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags))
    for source in opus_sources():
        objects.append(guest_object(source, " ".join([opus_cflags(guest_abi), *libc_includes])))
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags))
    for name in ZLIB_SOURCES:
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES])))
    musl_math_cflags = " ".join([
        guest_abi, "-std=gnu11", "-w", *libc_includes, f"-I{MUSL_MATH_DIR}/include",
        f"-include {MUSL_MATH_DIR}/include/libm.h",
    ])
    for source in musl_math_sources():
        objects.append(guest_object(source, musl_math_cflags))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        f"-I{PORT_DIR}/guest/runtime", f"-I{PORT_DIR}/include",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{MUSL_DIR}/src/include", f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{PORT_DIR}/guest/runtime", f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", *libc_includes,
    ])
    runtime_dir = PORT_DIR / "guest" / "runtime"
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name == "guest_memory_watch.c":
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_gl_c, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="haiku_guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- the guest image

    linker_script = PORT_DIR / "guest" / "guest.ld"
    n.rule(
        name="haiku_guest_link",
        command=(f"$haiku_lld -m elf32_x86_64 --error-limit=0 -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="HAIKU LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="haiku_guest_link", inputs=objects, implicit=[libguestc, linker_script])

    # ---------- SDL3 for the host: the system's, else built from the source

    host_outputs: List[Path] = []
    system_sdl = _system_sdl(host_cc)
    if system_sdl:
        sdl_cflags, sdl_libs = system_sdl
        sdl_implicit: List[Path] = []
    else:
        sdl_build = BUILD / "sdl3-build"
        libsdl = sdl_build / "libSDL3.so"
        n.rule(
            name="haiku_sdl3",
            command=(f"cmake -S {SDL_DIR} -B {sdl_build} -G Ninja -DCMAKE_BUILD_TYPE=Release "
                     f"-DCMAKE_C_COMPILER={host_cc} "
                     f"-DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF "
                     # (HALO_SDL_CMAKE_FLAGS: more, such as the features a
                     # machine has no development files for)
                     f"{os.environ.get('HALO_SDL_CMAKE_FLAGS', '')} "
                     f"> {BUILD}/sdl3-configure.log && ninja -C {sdl_build} > {BUILD}/sdl3-build.log"),
            description="HAIKU SDL3",
            pool="console",
        )
        n.build(outputs=libsdl, rule="haiku_sdl3", implicit=[SDL_DIR / "CMakeLists.txt"])
        sdl_cflags = f"-I{SDL_DIR}/include"
        # (beside the program, which looks for it there: $ORIGIN, and
        # Haiku's own %A/lib)
        sdl_libs = f"-L{sdl_build} -lSDL3 -Wl,-rpath,'$$ORIGIN' -Wl,-rpath,'$$ORIGIN/lib'"
        sdl_implicit = [libsdl]
        staged = BUILD / "lib" / "libSDL3.so.0"
        n.rule(name="haiku_copy_sdl", command="cp -L $in $out", description="HAIKU STAGE $out")
        n.build(outputs=staged, rule="haiku_copy_sdl", inputs=libsdl)
        host_outputs.append(staged)

    # ---------- the host program

    host_objects: List[Path] = []
    host_obj_dir = BUILD / "host" / "obj"
    # the guest's system call numbers, for the host's dispatcher (host_syscall.c)
    guest_syscalls_h = BUILD / "host" / "guest_syscalls.h"
    n.rule(
        name="haiku_guest_syscalls_h",
        command="sed -n -e 's/^#define __NR_/#define GUEST_SYS_/p' $in > $out",
        description="HAIKU $out",
    )
    n.build(outputs=guest_syscalls_h, rule="haiku_guest_syscalls_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name="haiku_host_cc",
        command="$haiku_host_cc -MMD -MF $out.d $cflags -c $in -o $out",
        description="HAIKU HOST CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    host_defines = ["-DHALO_HAIKU_HOST=1"] if system == "haiku" else ["-DHALO_X32_LINUX_HOST=1"]
    host_defines.append("-DHALO_HAIKU_PORT=1")
    host_cflags = " ".join([
        "-O2", "-g", "-Wall", "-Wno-unused-function", "-D_GNU_SOURCE", "-D_DEFAULT_SOURCE",
        "-D_FILE_OFFSET_BITS=64", *host_defines,
        f"-I{PORT_DIR}/include", f"-I{PORT_DIR}/host", f"-I{BUILD / 'host'}", sdl_cflags, f"-I{LINUX_DIR}/src",
    ])
    host_sources = sorted((PORT_DIR / "host").glob("*.c")) + [LINUX_DIR / "src" / name for name in HOST_POSIX_SOURCES]
    for source in host_sources:
        obj = host_obj_dir / (source.name + ".o")
        n.build(outputs=obj, rule="haiku_host_cc", inputs=source, implicit=[*sdl_implicit, guest_syscalls_h],
                variables={"cflags": host_cflags})
        host_objects.append(obj)
    miniupnpc_cflags = " ".join([host_cflags, f"-I{MINIUPNPC_DIR / 'include'}", f"-I{MINIUPNPC_DIR / 'src'}",
                                 *MINIUPNPC_DEFINES])
    for source in [LINUX_DIR / "src" / "posix_upnp.c", *miniupnpc_sources()]:
        obj = host_obj_dir / ("miniupnpc_" + source.name + ".o" if source.parent.parent == MINIUPNPC_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="haiku_host_cc", inputs=source,
                variables={"cflags": miniupnpc_cflags + (" -w" if source.name != "posix_upnp.c" else "")})
        host_objects.append(obj)
    mbedtls_cflags = " ".join([host_cflags, f"-I{MBEDTLS_DIR / 'include'}", f"-I{MBEDTLS_DIR / 'library'}",
                               "-DMBEDTLS_THREADING_C", "-DMBEDTLS_THREADING_PTHREAD"])
    for source in [LINUX_DIR / "src" / "posix_dtls.c", LINUX_DIR / "src" / "posix_update.c",
                   *sorted((MBEDTLS_DIR / "library").glob("*.c"))]:
        obj = host_obj_dir / ("mbedtls_" + source.name + ".o" if source.parent.parent == MBEDTLS_DIR
                              else source.name + ".o")
        n.build(outputs=obj, rule="haiku_host_cc", inputs=source,
                variables={"cflags": mbedtls_cflags + (" -w" if source.parent.parent == MBEDTLS_DIR else "")})
        host_objects.append(obj)
    table_obj = host_obj_dir / "host_import_table.c.o"
    n.build(outputs=table_obj, rule="haiku_host_cc", inputs=host_table_c, variables={"cflags": host_cflags})
    host_objects.append(table_obj)
    if system == "haiku":
        # (sockets and getifaddrs are libnetwork's; the rest is libroot's,
        # which every program links)
        system_libs = "-lnetwork"
    else:
        system_libs = "-lpthread -ldl -lm"
    n.rule(
        name="haiku_host_link",
        command=f"$haiku_host_cc -o $out $in {sdl_libs} {system_libs}",
        description="HAIKU HOST LINK $out",
    )
    n.build(outputs=host_program, rule="haiku_host_link", inputs=host_objects, implicit=sdl_implicit)

    # ---------- beside the program

    n.rule(name="haiku_copy", command="cp $in $out", description="HAIKU STAGE $out")
    brokers = BUILD / "brokers.txt"
    n.build(outputs=brokers, rule="haiku_copy", inputs=Path("port/assets/network/brokers.txt"))
    masters = BUILD / "masters.txt"
    n.build(outputs=masters, rule="haiku_copy", inputs=Path("port/assets/network/masters.txt"))
    n.build(outputs="haiku", rule="phony", inputs=[host_program, image, brokers, masters, *host_outputs])
    n.build(outputs="haiku_guest", rule="phony", inputs=[image])
    n.newline()
