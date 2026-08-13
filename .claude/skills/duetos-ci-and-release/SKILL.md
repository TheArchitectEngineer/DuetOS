---
name: duetos-ci-and-release
description: >-
  DuetOS GitHub Actions CI and release-channel machinery — every workflow, every job, what gates vs
  what is advisory, how to poll runs and map a red job to its local repro, and how builds get
  published. TRIGGER when: "CI is red", "fix the pipeline", "qemu smoke failed in CI", "poll checks
  on my PR", "workflow failed", "the clang-format job failed", "release channel / publish build",
  "download the CI artifact". DO NOT TRIGGER for: running tests locally with no CI involvement (use
  duetos-testing-and-validation), reproducing a boot-smoke failure in a local QEMU (use
  duetos-boot-smoke-and-qemu), or branch/merge discipline (use duetos-change-control).
---

# DuetOS CI and release channels

Runbook for the three GitHub Actions workflows in `.github/workflows/`, the polling ritual after
every push, and the rolling release channels. Facts verified against the workflow YAML at commit
`8a55872c` (2026-08-13).

**Definitions.** "Gates" = the job fails the workflow run when it fails (no `continue-on-error`).
"Advisory" = the job has `continue-on-error: true`; it can be red without blocking, but per
CLAUDE.md "Fix Anything You Surface" a red advisory signal is still a fix target.
"Rolling channel" = a GitHub Release under a fixed tag (`latest-debug`, `latest-release`,
`latest-flavors`) whose assets are overwritten on every green publish.

## The three workflows

| File | `name:` | Triggers |
|---|---|---|
| `build.yml` | `build` | push to `main` + `claude/**` (with `paths-ignore`), PRs to `main` |
| `release.yml` | `release-channels` | push to `main`, tags `v*`, `workflow_dispatch` |
| `lifetime-downloads.yml` | `lifetime-downloads` | cron `*/30 * * * *`, `workflow_dispatch`, `workflow_call` |

Three trip-wires baked into the triggers:

1. **Docs-only pushes run NO CI.** `build.yml` has `paths-ignore: ['**/*.md', 'LICENSE',
   'docs/**', 'wiki/**']`. If you push only wiki/docs changes and see zero checks, that is by
   design — do not wait for a run that will never start.
2. **Concurrency cancels superseded runs.** `build.yml` groups by workflow+ref with
   `cancel-in-progress: true`. Push twice quickly and the first run shows "cancelled" — that is
   not a failure.
3. **Every push to `main` triggers BOTH `build` and `release-channels`.** They are independent;
   check both when auditing what a `main` push did.

## build.yml — jobs table

All jobs run on `ubuntu-24.04`. Artifacts retained 7 days. Every "Local
repro" command is a Linux command: on this Windows dev host, run
kernel-build/smoke/fuzz repros **inside WSL in a WSL-native scratch tree**
(never on `/mnt/c`) — setup and rationale in `duetos-build-and-env`.

| Job id | Display name | Gates? | Artifact | Local repro |
|---|---|---|---|---|
| `check-format` | `clang-format` | yes | — | `find kernel userland \( -name '*.h' -o -name '*.hpp' -o -name '*.c' -o -name '*.cpp' \) \| xargs clang-format-18 --dry-run --Werror` |
| `check-rust` | `cargo fmt + clippy` | yes | — | `cargo fmt --check --all` then `cargo clippy --workspace --release --locked -- -D warnings` (toolchain auto-pinned by `/rust-toolchain.toml`) |
| `build-debug` | `build debug` | yes | `duetos-kernel-debug` (ISO + ELF) | `cmake --preset x86_64-debug && cmake --build build/x86_64-debug --parallel $(nproc)` |
| `build-release` | `build release` | yes | `duetos-kernel-release` | same, preset `x86_64-release` |
| `fuzz` | `fuzz-all` | yes | `fuzz-crashes` (on failure) | `FUZZ_SECONDS=90 tools/test/fuzz-all.sh` (45-min job budget) |
| `build-flavor-matrix` | `build+smoke flavor (<preset>)` | yes | `duetos-kernel-<preset>`; `flavor-smoke-log-<preset>` on failure | `cmake --preset <preset>` + build, then `DUETOS_PRESET=<preset> DUETOS_TIMEOUT=480 tools/test/profile-boot-smoke.sh bringup build/<preset>` |
| `qemu-smoke` | `qemu smoke (<profile>)` | yes | `qemu-serial-log-<profile>` on failure | `DUETOS_TIMEOUT=480 tools/test/profile-boot-smoke.sh <profile> build/x86_64-debug` |
| `diff-boot-smoke` | `diff-boot-smoke (bringup)` | ADVISORY | `diff-boot-smoke-logs` (always) | `DUETOS_TIMEOUT=480 tools/test/diff-boot-smoke.sh bringup build/x86_64-debug` |
| `clang-tidy` | `clang-tidy (advisory)` | ADVISORY | — | `clang-tidy -p build/x86_64-debug <file.cpp>` (needs `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` at configure) |
| `host-tests` | `host tests` | yes | — | `cmake -S tests/host -B build/host-tests -G Ninja -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18` then build + `ctest --test-dir build/host-tests --output-on-failure --parallel $(nproc)` |
| `pre-publish-lifetime-snapshot` | `snapshot lifetime downloads` | main-push only | — | (calls `lifetime-downloads.yml`) |
| `publish-rolling` | `publish rolling channels` | main-push only | GitHub Releases `latest-debug` / `latest-release` | — |

Flavor-matrix presets: `x86_64-release-asserts`, `x86_64-release-audit`, `x86_64-release-lto`,
`x86_64-debug-fast` (`fail-fast: false` — one red flavor does not cancel the others).

qemu-smoke profiles (`fail-fast: false`, `needs: build-debug`): `bringup`, `ring3`, `pe-hello`,
`pe-winapi`, `pe-threads`, `pe-winkill`, `linux`. The smoke script's exit contract is
`0 = pass, 1 = regression (incl. crash), 2 = env skip` — CI converts skip to a warning-pass.
Smokes are **single-attempt by design**: the retry-on-flake tier was deliberately removed so a
"passes on retry" run cannot hide a real boot-path crash. On PRs, each profile maintains a
sticky PR comment with the last 40 serial-log lines on failure.

Local repro mechanics for the smoke profiles (QEMU setup, WSL invocation, serial-log reading)
live in the sibling skill `duetos-boot-smoke-and-qemu`; hosted-test and fuzz mechanics live in
`duetos-testing-and-validation`. This skill only maps CI job → repro entrypoint.

### Load-bearing compiler pin (host-tests)

`host-tests` pins `-DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18` explicitly —
**keep the pin when copying the configure command** (an unpinned configure resolves to GCC,
whose green run is non-authoritative; full rationale in `duetos-build-and-env`). The job
prints the resolved compiler from `CMakeCache.txt` — check that line first when host-tests
behaves differently from your local run.

### What actually gates the rolling publish

`publish-rolling` runs only on push to `main` and `needs: [check-format, check-rust,
build-debug, build-release, qemu-smoke, host-tests, pre-publish-lifetime-snapshot]`.
Note what is NOT in that list: `fuzz` and `build-flavor-matrix` fail the workflow run but do
not block the rolling publish chain, and the advisory jobs never block anything.

## The polling ritual (after every PR / push)

Per CLAUDE.md and `wiki/tooling/Git-Workflow.md` ("Post-PR Checks"): after creating or pushing
to a PR, **always poll CI and fix failures before moving on**. Use the GitHub MCP tools — do
NOT shell out to `gh`.

1. List recent runs for the commit (`mcp__github__list_*` tools for the `build` workflow).
2. Wait for completion — no tight sleep loops.
3. On failure: read the failed step's log via MCP, then reproduce locally with the same
   command the workflow runs (table above).
4. For smoke failures, download the `qemu-serial-log-<profile>` artifact and run
   `tools/test/boot-log-analyze.sh <log>` on it before touching code.

Fallback when MCP is unavailable: `tools/test/duetos-ciwatch.sh <commit-sha|run-id>
[job-name-substr ...]` polls the Actions REST API, prints per-job status, and saves failed-job
logs to `/tmp/ciwatch_job_<id>.log`. It exits 3 if any named job is not `success`. Env:
`DUETOS_GH_REPO` (default `Krilliac/DuetOS`), `DUETOS_CIWATCH_POLL` (seconds between polls,
0 = once), `DUETOS_GH_PAT_FILE`. It discovers a token from `$GH_TOKEN`/`$GITHUB_TOKEN`, git
credentials, or a PAT file — the job-log endpoint needs auth even on public repos.

## CI red — first moves

1. **Identify the job, not just the workflow.** Job display names above map 1:1 to check names
   in the PR UI. `qemu smoke (pe-threads)` red and `qemu smoke (bringup)` green localises the
   failure to the pe-threads scenario, not the boot path.
2. **A symptom cluster gets ONE investigation.** Several smoke profiles red at once usually
   share a root (a boot-path change, a hung subsystem upstream of all of them). Trace one to
   root cause before touching the others.
3. **Timeout is not a verdict — it is a mask.** A `qemu-smoke` job that hit its 480 s budget
   means "something upstream hung", not "the runner was slow". Two real bugs shipped behind
   `qemu_timeout` before being caught (both on `origin/main`): `83717fd0` — the filesystem
   could block the watchdog; `05121fe8` — a fix-journal ABBA deadlock hung the whole smoke
   matrix. Pull the `qemu-serial-log-<profile>` artifact and run
   `tools/test/boot-log-analyze.sh` on it; the last serial line before silence names the
   hung subsystem.
4. **SKIP is not PASS.** The hosted ctest suite uses `SKIP_RETURN_CODE` (see the root
   `CMakeLists.txt` and `tools/pkg/CMakeLists.txt`), so environment-limited tests report SKIP
   visibly. A green job whose log shows skips did not prove what a full pass proves — read
   the ctest summary line, not just the job color.
5. **Cancelled ≠ failed.** `cancel-in-progress: true` kills superseded runs; only investigate
   runs for the head commit.
6. **Advisory jobs red?** `diff-boot-smoke` and `clang-tidy` cannot block, but their output is
   still a fix target. `diff-boot-smoke` exit meanings: 0 converged, 1 a row failed,
   2 rows passed but sentinel streams diverged (config-dependent behaviour — also a
   regression), 3 env skip. Its per-row logs are always uploaded as `diff-boot-smoke-logs`.
7. **Reproduce locally before pushing a guess.** Every gate has a one-command repro (table
   above). Pushing "maybe this fixes it" burns a full CI cycle (~30 min for the smoke matrix)
   per guess.
8. **Format failures:** run the `clang-format-18` dry-run locally, then `clang-format -i` the
   listed files. Never pass `.S` files to clang-format — it mangles them as C++.
   `tools/build/wsl-clang-format-check.sh` does the same sweep but hardcodes
   `cd ~/source/DuetOS`, so it only works from the WSL clone at that path.

## release.yml — release-channels

Triggers: push to `main`, tags `v*`, and `workflow_dispatch` with inputs `source_ref`
(default `main`) and `smoke_gate` (boolean, default `false`). `permissions: contents: write`.
Concurrency group `release-channels-${{ github.ref }}` with `cancel-in-progress: false` —
publishes queue rather than cancel.

| Job id | Display name | Role |
|---|---|---|
| `build-release-assets` | `build release assets` | Builds debug + release ISOs/ELFs from the resolved `source_ref`, packages `dist/**` with SHA256SUMS |
| `build-flavor-assets` | `build flavor (<preset>)` | Builds the 4 non-default flavor presets in parallel |
| `release-smoke-gate` | `qemu smoke gate (optional)` | OPT-IN: runs only on dispatch with `smoke_gate=true`, or when repo variable `RELEASE_SMOKE_GATE == 'true'`; runs `tools/test/ctest-boot-smoke.sh` on the debug ISO |
| `flavor-smoke-gate` | `flavor smoke gate (<preset>)` | Same opt-in condition; bringup profile per flavor |
| `pre-publish-lifetime-snapshot` | `snapshot lifetime downloads` | Folds current asset download counts into the tally BEFORE assets are overwritten |
| `publish-debug-release` | `publish debug channel` | Overwrites release at tag `latest-debug` (prerelease) |
| `publish-release-release` | `publish release channel` | Overwrites release at tag `latest-release` (`make_latest: true`) |
| `publish-flavor-channels` | `publish flavor channel` | Bundles all 4 flavor ISOs into tag `latest-flavors` |

Gate semantics worth knowing before you rely on them:

- **By default (push to `main` without the repo variable set), the smoke gates are SKIPPED
  and the publish proceeds** — skipped smoke is explicitly allowed by every publish job's
  `if:`. A failed smoke gate (when it did run) blocks publish.
- A failed `pre-publish-lifetime-snapshot` does NOT block publish (`success || failure`
  accepted); it only risks losing a few download-count deltas.
- To cut a publish from an arbitrary ref with a real smoke gate:
  `workflow_dispatch` on `release-channels` with `source_ref=<ref>` and `smoke_gate=true`.

### Do not trust tags for "latest"

The rolling channels re-point `latest-debug` / `latest-release` / `latest-flavors` on every
publish, but a local clone's tags go stale (in this worktree they read 2026-04-25 /
2026-06-06). The GitHub Releases page is the source of truth for what is currently published;
`git describe`/local tag dates are not. `v*` tags are the separate, manual versioned-release
path — none currently drive the day-to-day flow.

## lifetime-downloads.yml — the stats branch

Job `update` ("update tally") runs every 30 min by cron, on dispatch, and via `workflow_call`
from both other workflows before their publish steps. It runs
`tools/release/update-lifetime-downloads.py` and pushes two files to the **`stats` branch**:
`lifetime-downloads.json` (shields.io badge envelope the README points at) and
`lifetime-downloads-state.json` (persistence). Why it exists: `overwrite_files: true` deletes
the live asset object on every publish, resetting GitHub's per-asset download counter to 0 —
so the "lifetime" badge would otherwise show a near-zero snapshot.

**`origin/stats` is a bot-owned orphan branch.** Never rebase onto it, merge it, or push to it
from a dev session; concurrent pushes are serialized by the workflow's own concurrency group.

## Provenance and maintenance

Authored 2026-08-13 against worktree branch `claude/fable-driver-wave-20260801`, HEAD
`8a55872c`. All job ids, display names, triggers, artifacts, and gate conditions were read
directly from the three workflow files. Re-verify with:

```bash
# Workflow inventory + names/triggers
ls .github/workflows/ && grep -n '^name:\|^on:' .github/workflows/*.yml
# Job ids, display names, advisory flags, gate chains
grep -nE '^  [a-z-]+:|name:|continue-on-error|needs:' .github/workflows/build.yml
grep -nE '^  [a-z-]+:|name:|if: \|' .github/workflows/release.yml
# Smoke profile list and flavor presets
grep -nA9 'matrix:' .github/workflows/build.yml
# Local watcher + repro entrypoints still exist
ls tools/test/duetos-ciwatch.sh tools/test/profile-boot-smoke.sh tools/test/fuzz-all.sh \
   tools/test/boot-log-analyze.sh tools/test/diff-boot-smoke.sh tools/test/ctest-boot-smoke.sh \
   tools/release/update-lifetime-downloads.py
# Archaeology SHAs (on origin/main)
git log --oneline -1 83717fd0; git log --oneline -1 05121fe8
# Polling doctrine
sed -n '86,98p' wiki/tooling/Git-Workflow.md
```

Volatile facts (re-check if stale): the advisory status of `diff-boot-smoke` and `clang-tidy`
(both marked "until stabilised" in workflow comments — may be promoted to gates), the
`RELEASE_SMOKE_GATE` repo variable's value (not readable from the tree), fuzz's 90 s budget,
and the 7-profile smoke matrix (grows as smoke profiles land in `kernel/test/smoke_profile.h`).

