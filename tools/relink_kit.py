"""Relink kit of the program (LGPL-2.1 §6: OCCT is linked statically — decision Р-084, the third-party list, component
«opencascade»): the object files and static libraries of the build folder that the final link command of `construe`
uses, and relink.sh with that command where libraries of the vcpkg tree and of Qt become ${VCPKG_PREFIX} and
${QT_PREFIX}. Used by the macOS package script (S8-PKG-003); the Linux package script does the same transformation.

Usage: python3 -E tools/relink_kit.py --build-dir <dir> --vcpkg <triplet dir> --qt <Qt dir> --kit <out dir>
       --command "<link command>" [--exe bin/Release/construe] [--qt-hint "Qt 6 macos"] [--vcpkg-hint "…"]
Exit code: 0 — done, 1 — error.
"""

import argparse
import os
import pathlib
import shlex
import shutil
import sys


def link_words(cmd):
    """Words of the final link command without the Ninja wrapping («: && … && :», «cd <dir> && …»)."""
    words = shlex.split(cmd)
    if words[:2] == [":", "&&"] or (words and words[0] == "cd"):
        words = words[words.index("&&") + 1:]
    if "&&" in words:
        words = words[:words.index("&&")]
    return words


def transform(words, build, vcpkg, qt, exe_rel="bin/Release/construe"):
    """([words of relink.sh], [build-relative files to copy into obj/]). Paths of the vcpkg tree and of Qt become
    ${VCPKG_PREFIX}/… and ${QT_PREFIX}/…, files of the build folder — obj/<relative path>, the output — construe;
    rpath options of the build machine are dropped."""
    build, vcpkg, qt = (os.path.realpath(p) for p in (build, vcpkg, qt))
    out, copy = [], []
    skip_next = False
    for w in words:
        if skip_next:
            skip_next = False
            continue
        if w.startswith("-Wl,-rpath"):
            continue
        if w == "-rpath":  # clang-style «-Xlinker -rpath -Xlinker <dir>» is rare; plain «-rpath <dir>» — drop both
            skip_next = True
            continue
        if w[:2] in ("-F", "-L") and len(w) > 2:  # framework and library search folders (Qt frameworks on macOS)
            d = os.path.realpath(os.path.join(build, w[2:]))
            for prefix, var in ((vcpkg, "VCPKG_PREFIX"), (qt, "QT_PREFIX")):
                if d == prefix or d.startswith(prefix + os.sep):
                    rel = os.path.relpath(d, prefix).replace(os.sep, "/")
                    w = w[:2] + "${%s}" % var + ("" if rel == "." else "/" + rel)
                    break
            out.append(w)
            continue
        looks_like_path = not w.startswith("-") and ("/" in w or w.endswith((".o", ".a")))
        path = os.path.realpath(os.path.join(build, w)) if looks_like_path else None
        if path and path.startswith(vcpkg + os.sep):
            out.append("${VCPKG_PREFIX}/" + os.path.relpath(path, vcpkg).replace(os.sep, "/"))
        elif path and path.startswith(qt + os.sep):
            out.append("${QT_PREFIX}/" + os.path.relpath(path, qt).replace(os.sep, "/"))
        elif path and path.startswith(build + os.sep) and os.path.relpath(path, build).replace(os.sep, "/") == exe_rel:
            out.append("construe")
        elif path and path.startswith(build + os.sep) and os.path.isfile(path):
            rel = os.path.relpath(path, build).replace(os.sep, "/")
            copy.append(rel)
            out.append("obj/" + rel)
        else:
            out.append(w)
    return out, copy


def script_text(words, vcpkg_hint, qt_hint):
    quoted = " ".join('"%s"' % w if "${" in w else shlex.quote(w) for w in words)
    return ("#!/bin/sh\n# Links construe again from the object files of this kit (README.txt).\nset -e\n"
            ': "${VCPKG_PREFIX:?VCPKG_PREFIX — %s}"\n: "${QT_PREFIX:?QT_PREFIX — %s}"\ncd "$(dirname "$0")"\n%s\n'
            % (vcpkg_hint, qt_hint, quoted))


def main(argv=None):
    ap = argparse.ArgumentParser(description="construe relink kit (LGPL-2.1 §6)")
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--vcpkg", required=True)
    ap.add_argument("--qt", required=True)
    ap.add_argument("--kit", required=True)
    ap.add_argument("--command", required=True)
    ap.add_argument("--exe", default="bin/Release/construe")
    ap.add_argument("--vcpkg-hint", default="vcpkg tree with OCCT (lib/libTK*.a)")
    ap.add_argument("--qt-hint", default="Qt 6")
    args = ap.parse_args(argv)
    words, copy = transform(link_words(args.command), args.build_dir, args.vcpkg, args.qt, args.exe)
    if not copy:
        print("ERROR: the link command has no files of the build folder", file=sys.stderr)
        return 1
    kit = pathlib.Path(args.kit)
    for rel in copy:
        dst = kit / "obj" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(pathlib.Path(args.build_dir) / rel, dst)
    script = kit / "relink.sh"
    script.write_text(script_text(words, args.vcpkg_hint, args.qt_hint), encoding="utf-8")
    script.chmod(0o755)
    return 0


if __name__ == "__main__":
    sys.exit(main())
