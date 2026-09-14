#!/usr/bin/env python3
"""Aggregate per-job warning artifacts and mechanical-check reports for a
PR head SHA into a single sticky PR comment.

Runs from the workflow_run-triggered "CI warnings report" workflow with a
base-repository token.  It must never execute anything coming from the PR:
artifacts are downloaded and treated strictly as text.  On every invocation
the comment is rebuilt from ALL workflow runs recorded for the head SHA, so
concurrent or re-run workflows converge to the same result.

Environment (CI mode): GH_TOKEN, GITHUB_REPOSITORY, HEAD_SHA.

Local test mode:
    post-warning-report.py --render-only <dir>
where <dir> contains artifact-shaped subdirectories, e.g.
    warnings-ubuntu-features/warnings.txt
    report-mechanical-checks/10-caps-version.md
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

MARKER = "<!-- hamlib-ci-report -->"
MAX_COMMENT = 60000
MAX_LINES_PER_JOB = 50
MAX_LINE_LEN = 400
MAX_REPORT_SECTION = 20000

# The workflow whose workflow_run trigger list names every build
# workflow feeding this report; parsed so the list only lives there.
TRIGGER_WORKFLOW_FILE = ".github/workflows/ci-warnings-report.yml"


def expected_workflows():
    """Workflow display names to wait for, from the workflow_run list."""
    names = []
    try:
        with open(TRIGGER_WORKFLOW_FILE, encoding="utf-8") as fh:
            in_list = False
            for line in fh:
                stripped = line.strip()
                if stripped.startswith("#"):
                    continue
                if stripped.startswith("workflows:"):
                    in_list = True
                    continue
                if in_list:
                    m = re.match(r'-\s+"([^"]+)"$', stripped)
                    if not m:
                        break
                    names.append(m.group(1))
    except OSError:
        pass
    return names

REPORT_TITLES = {
    "mechanical-checks": "Mechanical checks",
    "baseline-compare": "Baseline comparison (dump-caps and ABI)",
}


def gh(*args, binary=False):
    res = subprocess.run(["gh"] + list(args), capture_output=True)
    if res.returncode != 0:
        raise RuntimeError("gh %s failed: %s"
                           % (args[:2], res.stderr.decode(errors="replace")))
    return res.stdout if binary else res.stdout.decode()


def gh_json(path):
    return json.loads(gh("api", path))


def gh_json_paged(path, key=None):
    """Fetch all pages of a list endpoint."""
    sep = "&" if "?" in path else "?"
    items = []
    page = 1
    while True:
        data = gh_json(f"{path}{sep}per_page=100&page={page}")
        chunk = data[key] if key else data
        items.extend(chunk)
        if len(chunk) < 100:
            return items
        page += 1


def sanitize_line(line):
    line = "".join(c for c in line if c == "\t" or ord(c) >= 32)
    if len(line) > MAX_LINE_LEN:
        line = line[:MAX_LINE_LEN] + " …"
    if line.lstrip().startswith("~~~"):
        line = " " + line
    return line


def read_inputs(dirpath):
    """Read artifact-shaped subdirectories into (warnings, reports)."""
    warnings = {}
    reports = {}
    for entry in sorted(os.listdir(dirpath)):
        full = os.path.join(dirpath, entry)
        if not os.path.isdir(full):
            continue
        if entry.startswith("warnings-"):
            slug = entry[len("warnings-"):]
            path = os.path.join(full, "warnings.txt")
            lines = []
            if os.path.isfile(path):
                with open(path, encoding="utf-8", errors="replace") as fh:
                    lines = [sanitize_line(l) for l in fh.read().splitlines()
                             if l.strip()]
            warnings[slug] = lines
        elif entry.startswith("report-"):
            slug = entry[len("report-"):]
            parts = []
            for name in sorted(os.listdir(full)):
                if not name.endswith(".md"):
                    continue
                with open(os.path.join(full, name), encoding="utf-8",
                          errors="replace") as fh:
                    parts.append(fh.read())
            text = "\n".join(parts)
            if len(text) > MAX_REPORT_SECTION:
                text = text[:MAX_REPORT_SECTION] \
                    + "\n\n_…truncated (size limit)._"
            reports[slug] = text
    return warnings, reports


def render(warnings, reports, pending, head_sha):
    out = [MARKER,
           "## 🤖 CI report",
           "",
           f"_Automatically updated for {head_sha[:10]} as CI workflows "
           "complete. Manual edits will be overwritten._",
           "",
           "### Build warnings",
           ""]

    if warnings:
        out.append("| Job | Warnings |")
        out.append("|---|---|")
        for slug in sorted(warnings):
            n = len(warnings[slug])
            mark = "✅" if n == 0 else "⚠️"
            out.append(f"| `{slug}` | {mark} {n} |")
        out.append("")
        for slug in sorted(warnings):
            lines = warnings[slug]
            if not lines:
                continue
            shown = lines[:MAX_LINES_PER_JOB]
            out.append(f"<details><summary><code>{slug}</code> — "
                       f"{len(lines)} unique warning(s)</summary>")
            out.append("")
            out.append("~~~")
            out.extend(shown)
            out.append("~~~")
            if len(lines) > len(shown):
                out.append(f"_…and {len(lines) - len(shown)} more; see the "
                           f"`warnings-{slug}` artifact of the workflow "
                           "run._")
            out.append("</details>")
            out.append("")
    else:
        out.append("_No build workflows have reported warning data yet._")
        out.append("")

    if pending:
        names = ", ".join(sorted(pending))
        out.append(f"⏳ Not finished yet: {names}")
        out.append("")

    for slug in sorted(reports):
        title = REPORT_TITLES.get(slug, slug.replace("-", " ").capitalize())
        out.append(f"### {title}")
        out.append("")
        out.append(reports[slug])
        out.append("")

    body = "\n".join(out)
    if len(body) > MAX_COMMENT:
        body = body[:MAX_COMMENT] + "\n~~~\n\n_…truncated (comment size " \
            "limit); see the workflow run artifacts for full output._"
    return body


def find_pr(repo, sha):
    prs = gh_json(f"repos/{repo}/commits/{sha}/pulls?per_page=100")
    for pr in prs:
        if pr.get("state") == "open" and pr["head"]["sha"] == sha \
                and pr["base"]["repo"]["full_name"] == repo:
            return pr["number"]
    return None


def collect(repo, sha, workdir):
    runs = gh_json_paged(
        f"repos/{repo}/actions/runs?head_sha={sha}&event=pull_request",
        key="workflow_runs")

    latest = {}
    for run in runs:
        wid = run["workflow_id"]
        if wid not in latest or run["id"] > latest[wid]["id"]:
            latest[wid] = run

    completed = [r for r in latest.values() if r["status"] == "completed"]
    finished_names = {r["name"] for r in completed}
    pending = [name for name in expected_workflows()
               if name not in finished_names]

    for run in completed:
        artifacts = gh_json_paged(
            f"repos/{repo}/actions/runs/{run['id']}/artifacts",
            key="artifacts")
        for art in artifacts:
            name = art["name"]
            if art.get("expired"):
                continue
            if not (name.startswith("warnings-")
                    or name.startswith("report-")):
                continue
            if not re.fullmatch(r'[A-Za-z0-9._-]+', name):
                continue
            data = gh("api", f"repos/{repo}/actions/artifacts/"
                      f"{art['id']}/zip", binary=True)
            zpath = os.path.join(workdir, f"{art['id']}.zip")
            with open(zpath, "wb") as fh:
                fh.write(data)
            dest = os.path.join(workdir, name)
            os.makedirs(dest, exist_ok=True)
            try:
                with zipfile.ZipFile(zpath) as zf:
                    for info in zf.infolist():
                        base = os.path.basename(info.filename)
                        if not base or info.is_dir():
                            continue
                        # Flatten: ignore any path component from the zip.
                        with zf.open(info) as src, \
                                open(os.path.join(dest, base), "wb") as dst:
                            dst.write(src.read(10 * 1024 * 1024))
            except zipfile.BadZipFile:
                continue
            finally:
                os.unlink(zpath)

    return pending


def upsert_comment(repo, pr, body):
    comments = gh_json_paged(f"repos/{repo}/issues/{pr}/comments")
    existing = None
    for c in comments:
        if MARKER in c.get("body", "") \
                and c.get("user", {}).get("login") == "github-actions[bot]":
            existing = c["id"]
            break

    with tempfile.NamedTemporaryFile("w", suffix=".json",
                                     delete=False) as fh:
        json.dump({"body": body}, fh)
        payload = fh.name
    try:
        if existing:
            gh("api", "-X", "PATCH",
               f"repos/{repo}/issues/comments/{existing}",
               "--input", payload)
            print(f"Updated comment {existing} on PR #{pr}")
        else:
            gh("api", "-X", "POST", f"repos/{repo}/issues/{pr}/comments",
               "--input", payload)
            print(f"Created comment on PR #{pr}")
    finally:
        os.unlink(payload)


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--render-only":
        warnings, reports = read_inputs(sys.argv[2])
        print(render(warnings, reports, ["Example pending workflow"],
                     "0123456789abcdef"))
        return

    repo = os.environ["GITHUB_REPOSITORY"]
    sha = os.environ["HEAD_SHA"]

    pr = find_pr(repo, sha)
    if pr is None:
        print(f"No open PR found with head {sha}; nothing to do")
        return

    with tempfile.TemporaryDirectory() as workdir:
        pending = collect(repo, sha, workdir)
        warnings, reports = read_inputs(workdir)
    body = render(warnings, reports, pending, sha)
    upsert_comment(repo, pr, body)


if __name__ == "__main__":
    main()
