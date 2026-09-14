#!/usr/bin/env python3
"""Verify that model IDs newly added to riglist.h/rotlist.h/amplist.h are
completely wired up: referenced by a backend, set in a caps struct,
registered via rig/rot/amp_register(), declared extern in a backend header,
and the defining file listed in the backend's Makefile.am.

Informational only: prints a Markdown report and always exits 0.
Runs against the checked-out head tree (grep) plus the git diff.

Usage: check-model-completeness.py <base-ref> <head-ref>
"""

import os
import re
import subprocess
import sys

KINDS = {
    "RIG": ("include/hamlib/riglist.h", "rigs", "rig_caps",
            "RIG_MODEL", "rig_register"),
    "ROT": ("include/hamlib/rotlist.h", "rotators", "rot_caps",
            "ROT_MODEL", "rot_register"),
    "AMP": ("include/hamlib/amplist.h", "amplifiers", "amp_caps",
            "AMP_MODEL", "amp_register"),
}
CAPS_RE = re.compile(r'\bstruct\s+(?:rig|rot|amp)_caps\s+(\w+)\s*=')


def git(*args, check=True):
    res = subprocess.run(["git"] + list(args), capture_output=True, text=True)
    if check and res.returncode not in (0, 1):  # git grep: 1 = no matches
        raise RuntimeError(res.stderr)
    return res.stdout


def grep_files(pattern, path, extended=False, word=False):
    # NOTE: git grep uses POSIX regexes; \s is not supported, so
    # patterns must use [[:space:]] and word matching must use -w.
    args = ["grep", "-l"]
    if extended:
        args.append("-E")
    if word:
        args.extend(["-F", "-w"])
    out = git(*args, "-e", pattern, "--", path)
    return [l for l in out.splitlines() if l]


def find_caps_for_model(macro, srcdir, wrapper):
    """Return (file, caps_name) defining the caps that sets this model ID."""
    use_re = re.compile(
        r'(?:%s\s*\(\s*%s\s*\)|\.\s*(?:rig|rot|amp)_model\s*=\s*%s\b)'
        % (re.escape(wrapper), re.escape(macro), re.escape(macro)))
    for f in grep_files(macro, srcdir, word=True):
        if not f.endswith(".c"):
            continue
        try:
            with open(f, encoding="utf-8", errors="replace") as fh:
                content = fh.read()
        except OSError:
            continue
        m = use_re.search(content)
        if not m:
            continue
        caps = None
        for cm in CAPS_RE.finditer(content, 0, m.start() + 1):
            caps = cm.group(1)
        if caps:
            return f, caps
    return None, None


def main():
    base, head = sys.argv[1], sys.argv[2]
    print("#### New model completeness check")
    print()

    added = []  # (macro, kind)
    for kind, (header, *_rest) in KINDS.items():
        diff = git("diff", base, head, "--", header)
        for line in diff.splitlines():
            m = re.match(r'^\+\s*#define\s+(%s_MODEL_\w+)\s+%s_MAKE_MODEL'
                         % (kind, kind), line)
            if m:
                added.append((m.group(1), kind))

    if not added:
        print("✅ No new model IDs added to riglist.h/rotlist.h/amplist.h.")
        print()
        return

    rows = []
    problems = 0
    for macro, kind in added:
        header, srcdir, caps_struct, wrapper, register = KINDS[kind]
        refs = grep_files(macro, srcdir, word=True)
        if not refs:
            rows.append((macro, "❌ not referenced", "—", "—", "—", "—"))
            problems += 1
            continue
        caps_file, caps = find_caps_for_model(macro, srcdir, wrapper)
        if not caps:
            rows.append((macro, "✅", "❌ no caps struct sets it",
                         "—", "—", "—"))
            problems += 1
            continue
        backend = os.path.dirname(caps_file)
        sp = "[[:space:]]"
        registered = bool(grep_files(
            '%s%s*\\(%s*&%s*%s%s*\\)'
            % (register, sp, sp, sp, re.escape(caps), sp),
            backend, extended=True))
        extern = bool(grep_files(
            'extern%s+(const%s+)?struct%s+%s%s+%s%s*;'
            % (sp, sp, sp, caps_struct, sp, re.escape(caps), sp),
            backend, extended=True))
        try:
            with open(os.path.join(backend, "Makefile.am"),
                      encoding="utf-8", errors="replace") as fh:
                in_makefile = os.path.basename(caps_file) in fh.read()
        except OSError:
            in_makefile = False
        ok = "✅"
        row = (macro, ok, f"✅ `{caps}` in `{caps_file}`",
               "✅" if registered else f"❌ no `{register}(&{caps})`",
               "✅" if extern else "⚠️ no extern declaration",
               "✅" if in_makefile else "❌ not in Makefile.am")
        if not registered or not in_makefile or not extern:
            problems += 1
        rows.append(row)

    print("| New model | Referenced | Caps struct | Registered "
          "| Extern decl | Makefile.am |")
    print("|---|---|---|---|---|---|")
    for r in rows:
        print("| `%s` | %s | %s | %s | %s | %s |" % r)
    print()
    if problems:
        print(f"⚠️ {problems} of {len(added)} new model(s) look incompletely "
              "wired up — see README.developer, \"Adding a Model\".")
    else:
        print(f"✅ All {len(added)} new model(s) are fully wired up.")
    print()


if __name__ == "__main__":
    main()
