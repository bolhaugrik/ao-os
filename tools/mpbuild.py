#!/usr/bin/env python3
"""MicroPython AO-OS port epitese GNU make nelkul: a generalt fejlecek (qstr, modul- es
root-pointer-lista, verzio) a MicroPython sajat py/*.py szkriptjeivel keszulnek, a forditas clanggal,
a linkeles az AOX crt0-val (1 MiB veremmel). Kimenet: share/python.aox (a netbookra:
fetch python.aox /state/bin). Az ao.py build hivja; onalloan: python tools/mpbuild.py"""
import hashlib
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MP = os.path.join(ROOT, "user", "micropython")
MP_VERSION = "v1.26.0"

# py/py.mk PY_CORE_O_BASENAME listaja
PY_CORE = """mpstate nlr nlrx86 nlrx64 nlrthumb nlraarch64 nlrmips nlrpowerpc nlrxtensa nlrrv32 nlrrv64 nlrsetjmp
malloc gc pystack qstr vstr mpprint unicode mpz reader lexer parse scope compile emitcommon emitbc asmbase asmx64
emitnx64 asmx86 emitnx86 asmthumb emitnthumb emitinlinethumb asmarm emitnarm asmxtensa emitnxtensa emitinlinextensa
emitnxtensawin asmrv32 emitnrv32 emitinlinerv32 emitndebug formatfloat parsenumbase parsenum emitglue persistentcode
runtime runtime_utils scheduler nativeglue pairheap ringbuf cstack stackctrl argcheck warning profile map obj objarray
objattrtuple objbool objboundmeth objcell objclosure objcode objcomplex objdeque objdict objenumerate objexcept
objfilter objfloat objfun objgenerator objgetitemiter objint objint_longlong objint_mpz objlist objmap objmodule
objobject objpolyiter objproperty objnone objnamedtuple objrange objreversed objringio objset objsingleton objslice
objstr objstrunicode objstringio objtuple objtype objzip opmethods sequence stream binary builtinimport builtinevex
builtinhelp modarray modbuiltins modcollections modgc modio modmath modcmath modmicropython modstruct modsys moderrno
modthread vm bc showbc repl smallint frozenmod""".split()
EXTMOD = ["vfs", "vfs_reader", "modos", "modtime", "modrandom", "modre", "modjson", "modheapq", "modbinascii"]
SHARED = ["shared/runtime/pyexec.c", "shared/runtime/gchelper_generic.c", "shared/runtime/interrupt_char.c",
          "shared/runtime/stdout_helpers.c", "shared/readline/readline.c", "shared/libc/printf.c"]
PORT = ["main.c", "mphalport.c", "vfs_ao.c", "modao.c", "libc_ao.c"]


def sources():
    srcs = [f"py/{n}.c" for n in PY_CORE] + [f"extmod/{n}.c" for n in EXTMOD] + SHARED + [f"port/{n}" for n in PORT]
    srcs += sorted("lib/libm_dbl/" + f for f in os.listdir(os.path.join(MP, "lib", "libm_dbl")) if f.endswith(".c"))
    return srcs


def qstr_sources(srcs):
    return [s for s in srcs if not s.startswith("py/nlr") and not s.startswith("lib/") and "gchelper" not in s]


def cflags(build):
    return [
        "--target=x86_64-elf", "-ffreestanding", "-fno-builtin", "-nostdlib", "-nostdlibinc",
        "-fno-stack-protector", "-fno-pic", "-fno-pie", "-fvisibility=hidden",
        "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-mno-red-zone", "-fomit-frame-pointer",
        "-std=gnu11", "-g", "-w", "-DNDEBUG", "-DMICROPY_ROM_TEXT_COMPRESSION=0",
        "-I", MP, "-I", os.path.join(MP, "port"), "-I", os.path.join(MP, "port", "include"),
        "-I", build, "-I", os.path.join(ROOT, "user"), "-I", os.path.join(ROOT, "kernel", "include"),
    ]


def run(cmd, **kw):
    r = subprocess.run(cmd, cwd=MP, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"mpbuild: hiba: {' '.join(cmd[:3])} ...")
    return r.stdout


def signature(files):
    h = hashlib.sha1()
    for f in files:
        st = os.stat(f)
        h.update(f"{f}:{st.st_mtime_ns}:{st.st_size};".encode())
    return h.hexdigest()


def gen_headers(clang, build, srcs):
    """genhdr/: mpversion.h, qstrdefs.generated.h, moduledefs.h, root_pointers.h (mkrules.mk lepesei)"""
    genhdr = os.path.join(build, "genhdr")
    os.makedirs(genhdr, exist_ok=True)
    py = sys.executable
    qsrc = qstr_sources(srcs)
    deps = [os.path.join(MP, "py", "mpconfig.h"), os.path.join(MP, "port", "mpconfigport.h"),
            os.path.join(MP, "port", "qstrdefsport.h"), os.path.join(MP, "py", "qstrdefs.h")]
    stamp = os.path.join(genhdr, "stamp")
    sig = signature([os.path.join(MP, s) for s in qsrc] + deps)
    if os.path.exists(stamp) and open(stamp).read() == sig and os.path.exists(os.path.join(genhdr, "qstrdefs.generated.h")):
        return False
    print("  [micropython] fejlecek generalasa (qstr, modulok, root pointerek)")
    env = dict(os.environ, MICROPY_GIT_TAG=MP_VERSION, MICROPY_GIT_HASH="ao-os")
    run([py, "py/makeversionhdr.py", os.path.join(genhdr, "mpversion.h")], env=env)
    qi = os.path.join(genhdr, "qstr.i.last")
    run([py, "py/makeqstrdefs.py", "pp", clang, "-E", "output", qi, "cflags"] + cflags(build) + ["-DNO_QSTR"] +
        ["cxxflags", "sources"] + qsrc + ["dependencies"] + deps + ["changed_sources"] + qsrc)
    for mode, out in (("qstr", "qstrdefs.collected.h"), ("module", "moduledefs.collected"), ("root_pointer", "root_pointers.collected")):
        d = os.path.join(genhdr, mode)
        run([py, "py/makeqstrdefs.py", "split", mode, qi, d, "_"])
        run([py, "py/makeqstrdefs.py", "cat", mode, "_", d, os.path.join(genhdr, out)])
    # qstrdefs.preprocessed.h: a Q(...) sorok idezojelben mennek at az elofeldolgozon
    lines = []
    for f in ("py/qstrdefs.h", "port/qstrdefsport.h", os.path.join(genhdr, "qstrdefs.collected.h")):
        with open(os.path.join(MP, f), encoding="utf-8") as fh:
            for line in fh:
                line = line.rstrip("\n")
                lines.append(re.sub(r"^(Q\(.*\))$", r'"\1"', line))
    pre = subprocess.run([clang, "-E", "-x", "c", "-"] + cflags(build), cwd=MP, input="\n".join(lines) + "\n",
                         capture_output=True, text=True)
    if pre.returncode != 0:
        sys.stderr.write(pre.stderr)
        raise SystemExit("mpbuild: qstrdefs elofeldolgozas sikertelen")
    with open(os.path.join(genhdr, "qstrdefs.preprocessed.h"), "w", encoding="utf-8") as fh:
        for line in pre.stdout.splitlines():
            fh.write(re.sub(r'^"(Q\(.*\))"$', r"\1", line) + "\n")
    with open(os.path.join(genhdr, "qstrdefs.generated.h"), "w", encoding="utf-8") as fh:
        fh.write(run([py, "py/makeqstrdata.py", os.path.join(genhdr, "qstrdefs.preprocessed.h")]))
    with open(os.path.join(genhdr, "moduledefs.h"), "w", encoding="utf-8") as fh:
        fh.write(run([py, "py/makemoduledefs.py", os.path.join(genhdr, "moduledefs.collected")]))
    with open(os.path.join(genhdr, "root_pointers.h"), "w", encoding="utf-8") as fh:
        fh.write(run([py, "py/make_root_pointers.py", os.path.join(genhdr, "root_pointers.collected")]))
    with open(stamp, "w") as fh:
        fh.write(sig)
    return True


def build(tools, build_root, ulib_objs, out_path):
    clang, nasm, ld, objcopy = tools["clang"], tools["nasm"], tools["ld.lld"], tools["llvm-objcopy"]
    build = os.path.join(build_root, "mp")
    os.makedirs(os.path.join(build, "obj"), exist_ok=True)
    srcs = sources()
    regenerated = gen_headers(clang, build, srcs)
    stamp_time = os.path.getmtime(os.path.join(build, "genhdr", "stamp"))
    objs = []
    compiled = 0
    failed = []
    for s in srcs:
        src = os.path.join(MP, s)
        obj = os.path.join(build, "obj", s.replace("/", "_").replace(".c", ".o"))
        objs.append(obj)
        if not regenerated and os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(src), stamp_time):
            continue
        opt = "-Os" if os.path.basename(s).startswith("nlr") else ("-O3" if s.endswith(("gc.c", "vm.c")) else "-O2")
        r = subprocess.run([clang] + cflags(build) + [opt, "-c", src, "-o", obj], cwd=MP, capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr)
            failed.append(s)
        compiled += 1
    if failed:
        raise SystemExit(f"mpbuild: {len(failed)} fajl nem fordult le: {' '.join(failed)}")
    if compiled:
        print(f"  [micropython] {compiled} fajl leforditva")
    crt0 = os.path.join(build, "crt0_py.o")
    run([nasm, "-f", "elf64", "-DAOX_STACK=1048576", os.path.join(ROOT, "user", "crt0.asm"), "-o", crt0])
    elf = os.path.join(build, "python.aox.elf")
    run([ld, "-T", os.path.join(ROOT, "user", "aox.ld"), "-nostdlib", "-static", "--no-pie", "-z", "max-page-size=0x10",
         "-o", elf, crt0] + objs + ulib_objs)
    run([objcopy, "-O", "binary", elf, out_path])
    return os.path.getsize(out_path)


if __name__ == "__main__":
    sys.path.insert(0, ROOT)
    import ao
    t = ao.tools()
    ulib = [os.path.join(ao.BUILD, "ulib_" + s.replace("/", "_").replace(".c", ".o"))
            for s in ("user/aolib.c", "user/malloc.c", "kernel/lib/string.c", "kernel/lib/fmt.c")]
    out = os.path.join(ROOT, "share", "python.aox")
    print(f"python.aox: {build(t, ao.BUILD, ulib, out)} B")
