# CI helper scripts

Scripts used by the GitHub Actions workflows to aid PR review.
Everything here is **informational**: findings are reported on the PR but
never fail the build.

## Warning collection → sticky PR comment

Every build workflow pipes its `make` output through `tee build.log` and
then runs the local composite action `.github/actions/collect-warnings`,
which calls `extract-warnings.sh` to grep, normalize, and deduplicate
compiler warnings and uploads them as a `warnings-<job>` artifact.

`ci-warnings-report.yml` triggers on `workflow_run` completion of each
build workflow.  Because it runs in the base repository with a
write-capable token (this is what makes fork PRs work), it must **never
check out or execute PR code**; it only downloads `warnings-*` and
`report-*` artifacts as text.  `post-warning-report.py` rebuilds the
entire report from all runs recorded for the PR head SHA on every
invocation, then creates or updates a single sticky comment (identified
by an HTML marker) on the PR.

To add a new build workflow to the report: pipe the build through
`tee build.log`, add the collect-warnings step with a unique `name`,
then add the workflow's display name to the `workflow_run` list in
`ci-warnings-report.yml` (`post-warning-report.py` parses that list, so
it is the only place to maintain).

All jobs added for review assistance run with `continue-on-error: true`
so they can never block a PR, even when a tool itself fails.

## Mechanical checks (`pr-review-checks.yml`)

- `check-caps-version.py` — flags modified backend files defining caps
  structs whose `.version` (or backend `*_VER` macro) was not bumped.
- `check-model-completeness.py` — verifies new model IDs added to
  riglist.h/rotlist.h/amplist.h are referenced, set in a caps struct,
  registered, declared extern, and listed in Makefile.am.
- `astyle-report.sh` — reports formatting deviations from
  `scripts/astylerc` in files changed by the PR.
- `dumpcaps-all.sh` + `baseline-report.sh` — the `baseline-compare` job
  builds both the PR and its merge base (the merge-base build is cached
  by commit SHA), diffs `--dump-caps` output for every model, and runs
  `abidiff` on libhamlib to flag ABI changes.

All of these print Markdown, which lands in the job's step summary and
in the sticky PR comment via `report-*` artifacts.

## Static analysis

- `cppcheck-analysis.yml` scans PR-changed files (full tree weekly),
  converting results with `cppcheck-xml2sarif.py` and uploading to
  GitHub code scanning, where findings annotate the PR diff.  Known
  noisy findings can be muted project-wide by listing suppressions in
  `.github/cppcheck-suppressions.txt` (one `id[:file[:line]]` per
  line).
- `gcc-analysis.yml` runs a GCC `-fanalyzer` build (findings feed the
  warnings comment) and an ASan+UBSan `make check` job.

Scripts are POSIX sh / Python 3 stdlib only and can be run locally, e.g.:

```sh
python3 .github/scripts/check-caps-version.py origin/master HEAD
sh .github/scripts/astyle-report.sh origin/master HEAD
python3 .github/scripts/post-warning-report.py --render-only <fixture-dir>
```
