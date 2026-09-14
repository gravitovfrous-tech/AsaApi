#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# SpawnGuard build helper: macro collision lint.
#
# The AsaApi headers pull in UE headers that define a lot of function-like
# macros (check, verify, ensure, TEXT, LOCTEXT, checkf, ...). If a plugin uses
# one of those names for its own variable or function - e.g.
#
#     UE::Math::TVector<double> check(loc.X, loc.Y, loc.Z);   // <-- BOOM
#
# the preprocessor rewrites the code before the compiler sees it and MSVC
# reports a cascade of confusing errors:
#
#     C4002 too many arguments for function-like macro invocation 'check'
#     C2065 'check': undeclared identifier
#     C2143/C2447 syntax error: missing ';' / missing function header
#
# Run this script after editing Main.cpp:
#
#     python tools/check_macro_collisions.py         (from the plugin folder)
#     python Plugins/SpawnGuard/tools/check_macro_collisions.py   (from repo root)
#
# Exit code 0 = clean, 1 = collisions found.
# ---------------------------------------------------------------------------

import glob
import os
import re
import sys

# Names that are intentionally used even though they are macros.
WHITELIST = {"DECLARE_HOOK"}


def find_repo_root(start):
    path = os.path.abspath(start)
    while True:
        if os.path.isdir(os.path.join(path, "AsaApi", "Core", "Public")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            return None
        path = parent


def collect_macros(root):
    macros = {}
    pattern = os.path.join(root, "AsaApi", "Core", "Public", "**", "*.h")
    for path in glob.glob(pattern, recursive=True):
        try:
            text = open(path, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\()?", text, re.M):
            macros.setdefault(m.group(1), (os.path.relpath(path, root), bool(m.group(2))))
    return macros


def strip_comments_and_strings(source):
    """Roughly remove comments/strings so only real code is scanned."""
    out = []
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        if c == "/" and i + 1 < n and source[i + 1] == "/":
            j = source.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and i + 1 < n and source[i + 1] == "*":
            j = source.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if c == '"':
            j = i + 1
            while j < n:
                if source[j] == "\\":
                    j += 2
                    continue
                if source[j] == '"':
                    break
                j += 1
            i = j + 1
            out.append('""')
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if source[j] == "\\":
                    j += 2
                    continue
                if source[j] == "'":
                    break
                j += 1
            i = j + 1
            continue
        out.append(c)
        i += 1
    return "".join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = find_repo_root(here)
    if root is None:
        root = find_repo_root(os.getcwd())
    if root is None:
        print("error: AsaApi repository root not found (AsaApi/Core/Public missing)")
        return 2

    source_path = os.path.join(root, "Plugins", "SpawnGuard", "Main.cpp")
    if not os.path.isfile(source_path):
        print("error: %s not found" % source_path)
        return 2

    macros = collect_macros(root)
    code = strip_comments_and_strings(open(source_path, encoding="utf-8", errors="ignore").read())

    used = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", code))
    collisions = sorted(n for n in used if n in macros and n not in WHITELIST)

    if not collisions:
        print("OK: no collisions with the %d macros defined by the AsaApi headers" % len(macros))
        return 0

    print("FAIL: these identifiers are used as a call but are function-like macros in the SDK headers:")
    for name in collisions:
        path, is_func = macros[name]
        print("  %-24s %s  (%s)" % (name, "function-like" if is_func else "object-like", path))
    print()
    print("Rename the identifier in Main.cpp (or add it to WHITELIST if it is intentional).")
    return 1


if __name__ == "__main__":
    sys.exit(main())
