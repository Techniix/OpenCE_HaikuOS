#!/usr/bin/env python3
"""Generate the x32 guest's OpenGL entry points.

The guest (x32 code, see port/haiku/README.md) cannot link against the
host's OpenGL, but it can call it: 32-bit integers and floats go in the
same registers and stack slots under both ABIs. What differs is

- pointers, which the wrappers widen to 64 bits themselves (the x32 ABI
  zero-extends them too, but a wrapper that does it needs no trust in that);
- GLsizeiptr and GLintptr, 32-bit in the guest and 64-bit in the host;
- returned pointers and handles, which point at host memory: glGetString's
  text is copied (host_gl_get_string), a GLsync is a small handle the host
  maps back (host_gl_fence_sync), and glMapBufferRange gives NULL (the
  renderer then reads its visibility queries on the CPU, as it does on
  AMD's driver: d3d8_gl.c);
- glDebugMessageCallback's callback, which the host's driver would call
  from its own threads: the host logs the messages itself
  (host_gl_debug_output).

A function the driver lacks is NULL to SDL_GL_GetProcAddress, as on the
other builds (host_gl_available).

For each function in the desktop list in port/linux/src/gl.h this writes a
guest wrapper that calls an import named hostgl_<function>, which the host
resolves to the real entry point (SDL_GL_GetProcAddress). The Android
guest's are made the same way for OpenGL ES (tools/android_gl_stubs.py).

Usage: haiku_gl_stubs.py gl.h SDL_opengl.h SDL_opengl_glext.h output.c imports.list
"""

import re
import sys

WIDE_TYPES = {"GLsizeiptr", "GLintptr", "GLsizeiptrARB", "GLintptrARB"}

SPECIAL = {
    "glGetString": [
        "static const GLubyte *APIENTRY guest_glGetString(GLenum name)",
        "{",
        "\tstatic __thread char buffer[4][4096];",
        "\tstatic __thread int next;",
        "\tchar *result = buffer[next++ & 3];",
        "",
        "\thost_gl_get_string(name, -1, result, sizeof(buffer[0]));",
        "\treturn (const GLubyte *)result;",
        "}",
    ],
    "glShaderSource": [
        "void hostgl_glShaderSource(GLuint shader, GLsizei count, unsigned long long strings, "
        "unsigned long long lengths);",
        "static void APIENTRY guest_glShaderSource(GLuint shader, GLsizei count, "
        "const GLchar *const *strings, const GLint *lengths)",
        "{",
        "\tunsigned long long wide[16];",
        "\tGLsizei index;",
        "",
        "\tif (count > 16)",
        "\t\tcount = 16;",
        "\tfor (index = 0; index < count; index++)",
        "\t\twide[index] = (unsigned long long)(unsigned int)strings[index];",
        "\thostgl_glShaderSource(shader, count, (unsigned long long)(unsigned int)wide,",
        "\t\t(unsigned long long)(unsigned int)lengths);",
        "}",
    ],
    "glMapBufferRange": [
        "static void *APIENTRY guest_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, "
        "GLbitfield access)",
        "{",
        "\t(void)target;",
        "\t(void)offset;",
        "\t(void)length;",
        "\t(void)access;",
        "\treturn NULL;",
        "}",
    ],
    "glFenceSync": [
        "static GLsync APIENTRY guest_glFenceSync(GLenum condition, GLbitfield flags)",
        "{",
        "\treturn (GLsync)(unsigned int)host_gl_fence_sync(condition, flags);",
        "}",
    ],
    "glClientWaitSync": [
        "static GLenum APIENTRY guest_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)",
        "{",
        "\treturn (GLenum)host_gl_client_wait_sync((unsigned int)sync, flags, timeout);",
        "}",
    ],
    "glDeleteSync": [
        "static void APIENTRY guest_glDeleteSync(GLsync sync)",
        "{",
        "\thost_gl_delete_sync((unsigned int)sync);",
        "}",
    ],
    "glDebugMessageCallback": [
        "static void APIENTRY guest_glDebugMessageCallback(GLDEBUGPROC callback, const void *user)",
        "{",
        "\t(void)user;",
        "\thost_gl_debug_output(callback != NULL);",
        "}",
    ],
}


def desktop_functions(gl_header: str):
    text = open(gl_header, encoding="utf-8").read()
    start = text.index("#else\n#define GL_FUNCTIONS(X)")
    end = text.index("#endif", start)
    return re.findall(r"X\((\w+)\)", text[start:end])


def prototypes(*headers):
    result = {}
    pattern = re.compile(r"GLAPI\s+([^;]+?)\s*\b(?:GL)?APIENTRY\s+(\w+)\s*\((.*?)\)\s*;", re.S)
    for header in headers:
        text = open(header, encoding="utf-8").read()
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for ret, name, params in pattern.findall(text):
            params = " ".join(params.split())
            result.setdefault(name, (" ".join(ret.split()), params))
    return result


def split_parameter(param: str):
    param = param.strip()
    m = re.match(r"(.*?)(\w+)\s*$", param)
    ptype, pname = m.group(1).strip(), m.group(2)
    if not ptype or ptype in ("const",):  # unnamed
        ptype, pname = param, None
    return ptype, pname


def main():
    gl_header, sdl_gl, sdl_glext, output, imports = sys.argv[1:6]
    functions = desktop_functions(gl_header)
    protos = prototypes(sdl_gl, sdl_glext)
    out = [
        "/* generated by tools/haiku_gl_stubs.py from port/linux/src/gl.h; do not edit */",
        "#define GL_GLEXT_PROTOTYPES 1",
        "#include <SDL3/SDL_opengl.h>",
        "#include <string.h>",
        "",
        "#include \"guest_host.h\"",
        "",
    ]
    table = []
    import_names = []
    for name in functions:
        if name in SPECIAL:
            out += SPECIAL[name] + [""]
            if name == "glShaderSource":
                import_names.append("hostgl_glShaderSource")
            table.append(name)
            continue
        if name not in protos:
            raise SystemExit(f"{name}: no prototype in SDL's OpenGL headers")
        ret, params = protos[name]
        plist = [] if params in ("void", "") else [split_parameter(p) for p in params.split(",")]
        if "*" in ret:
            raise SystemExit(f"{name}: returns a pointer; add a special case")
        if any(p.count("*") > 1 for p, _ in plist):
            raise SystemExit(f"{name}: takes an array of pointers; add a special case")
        decl_params, host_params, call_args = [], [], []
        for index, (ptype, pname) in enumerate(plist):
            arg = f"a{index}"
            decl_params.append(f"{ptype} {arg}")
            base = ptype.replace("const", "").strip()
            if "*" in ptype:
                host_params.append("unsigned long long")
                call_args.append(f"(unsigned long long)(unsigned int){arg}")
            elif base in WIDE_TYPES:
                host_params.append("long long")
                call_args.append(f"(long long){arg}")
            else:
                host_params.append(ptype)
                call_args.append(arg)
        host_name = "hostgl_" + name
        import_names.append(host_name)
        out.append(f"{ret} {host_name}({', '.join(host_params) or 'void'});")
        out.append(f"static {ret} APIENTRY guest_{name}({', '.join(decl_params) or 'void'})")
        out.append("{")
        call = f"{host_name}({', '.join(call_args)})"
        out.append(f"\t{'return ' if ret != 'void' else ''}{call};")
        out.append("}")
        out.append("")
        table.append(name)
    out.append("static const struct { const char *name; void *function; } guest_gl_functions[] =")
    out.append("{")
    for name in table:
        out.append(f'\t{{ "{name}", (void *)guest_{name} }},')
    out.append("};")
    out.append("")
    out.append("typedef void (*guest_gl_function)(void);")
    out.append("guest_gl_function guest_gl_get_proc_address(const char *name)")
    out.append("{")
    out.append("\tunsigned long index;")
    out.append("")
    out.append("\tfor (index = 0; index < sizeof(guest_gl_functions) / sizeof(guest_gl_functions[0]); index++)")
    out.append("\t{")
    out.append("\t\tif (!strcmp(guest_gl_functions[index].name, name))")
    out.append("\t\t\treturn host_gl_available(name) ? (guest_gl_function)guest_gl_functions[index].function : 0;")
    out.append("\t}")
    out.append("\treturn 0;")
    out.append("}")
    with open(output, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    with open(imports, "w", encoding="utf-8") as f:
        f.write("\n".join(import_names) + "\n")


if __name__ == "__main__":
    main()
