#!/usr/bin/env python3
"""Report changed backend files whose caps .version field was not bumped.

Hamlib convention is to bump the ".version" string of a rig/rot/amp caps
struct (or the backend-wide *_VER macro it is built from) whenever backend
behavior changes.  This check is informational only: it prints a Markdown
report and always exits 0.

Usage: check-caps-version.py <base-ref> <head-ref>
"""

import re
import subprocess
import sys

BACKEND_DIRS = ("rigs/", "rotators/", "amplifiers/")
CAPS_RE = re.compile(r'\bstruct\s+(?:rig|rot|amp)_caps\s+(\w+)\s*=')
VERSION_LINE_RE = re.compile(r'^[+-].*\.\s*version\s*=')
VER_DEFINE_RE = re.compile(r'^[+-]\s*#\s*define\s+\w*VER\w*\b')
MAX_FILES = 300


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True, check=True).stdout


def backend_dir(path):
    return "/".join(path.split("/")[:2])


def main():
    base, head = sys.argv[1], sys.argv[2]
    print("#### Caps `.version` bump check")
    print()

    changed = []
    for line in git("diff", "--name-status", base, head).splitlines():
        fields = line.split("\t")
        if len(fields) < 2:
            continue
        changed.append((fields[0][:1], fields[-1]))

    if len(changed) > MAX_FILES:
        print(f"_Skipped: more than {MAX_FILES} changed files._")
        return

    # Backend directories where some *_VER macro definition changed.
    ver_bumped_dirs = set()
    for status, path in changed:
        if status not in ("M", "A", "R") or not path.startswith(BACKEND_DIRS):
            continue
        diff = git("diff", base, head, "--", path)
        if any(VER_DEFINE_RE.match(l) for l in diff.splitlines()):
            ver_bumped_dirs.add(backend_dir(path))

    findings = []
    checked = 0
    for status, path in changed:
        if status != "M" or not path.startswith(BACKEND_DIRS) \
                or not path.endswith(".c"):
            continue
        content = git("show", f"{head}:{path}")
        caps = CAPS_RE.findall(content)
        if not caps:
            continue
        checked += 1
        diff = git("diff", base, head, "--", path)
        if any(VERSION_LINE_RE.match(l) for l in diff.splitlines()):
            continue
        if backend_dir(path) in ver_bumped_dirs:
            continue
        findings.append((path, caps))

    if not checked:
        print("✅ No modified backend files with caps definitions.")
    elif not findings:
        print(f"✅ All {checked} modified backend file(s) with caps "
              "definitions also update `.version` (or a backend `*_VER` "
              "macro).")
    else:
        print("⚠️ These modified files define caps structs, but neither "
              "their `.version` fields nor a backend `*_VER` macro were "
              "changed. Hamlib convention is to bump the caps version "
              "whenever backend behavior changes:")
        print()
        print("| File | Caps structs |")
        print("|---|---|")
        for path, caps in findings:
            print(f"| `{path}` | {', '.join('`%s`' % c for c in caps)} |")
        print()
        print("_Informational only — pure refactoring may not need a bump._")
    print()


if __name__ == "__main__":
    main()
