"""Launcher of the MSVC compiler for Ninja (docs/dev/setup.md, «Header dependencies (Ninja + MSVC)»).

Ninja learns the header dependencies from the /showIncludes lines with the prefix that CMake wrote to rules.ninja
(`msvc_deps_prefix`). The encoding of this prefix depends on how the configuration is run: with a console — the OEM encoding of the console,
without a console (a background process, an automatic CMake restart from Ninja) — UTF-8. A localized cl under Ninja
also writes these lines either in OEM or in UTF-8 (it depends on the process environment). Ninja does not recognize lines in the wrong encoding,
and the dependencies are silently lost. The launcher runs the compiler and re-encodes the output lines with non-ASCII characters
into the encoding of the prefix from rules.ninja of the current build folder (no file, or a prefix of ASCII only — OEM).

Run (CMake adds it itself): python -I -E cl_launcher.py <compiler> <arguments...>
"""

import ctypes
import os
import subprocess
import sys

PREFIX_KEY = b"msvc_deps_prefix = "


def oem_codec():
    try:
        return "cp%d" % ctypes.windll.kernel32.GetOEMCP()
    except Exception:  # не Windows
        return None


def target_codec(oem):
    """Encoding of the prefix in CMakeFiles/rules.ninja (Ninja runs the commands from the build folder)."""
    try:
        with open(os.path.join("CMakeFiles", "rules.ninja"), "rb") as f:
            for line in f:
                s = line.lstrip()
                if s.startswith(PREFIX_KEY) and any(b > 127 for b in s):
                    try:
                        s.decode("utf-8")
                        return "utf-8"
                    except UnicodeDecodeError:
                        return oem
    except OSError:
        pass
    return oem


def convert(data, oem, target):
    if not oem:
        return data
    out = []
    for line in data.splitlines(keepends=True):
        if any(b > 127 for b in line):
            try:
                text = line.decode("utf-8")
            except UnicodeDecodeError:
                text = line.decode(oem, errors="replace")
            line = text.encode(target, errors="replace")
        out.append(line)
    return b"".join(out)


def main():
    proc = subprocess.run(sys.argv[1:], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    oem = oem_codec()
    target = target_codec(oem) if oem else None
    sys.stdout.buffer.write(convert(proc.stdout, oem, target))
    sys.stderr.buffer.write(convert(proc.stderr, oem, target))
    sys.stdout.flush()
    sys.stderr.flush()
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
