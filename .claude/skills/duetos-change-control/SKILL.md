---
name: duetos-change-control
description: How DuetOS changes are classified, gated, and landed — session-start git sync, parallel-session claims, branch discipline, Definition of Done, and the incidents behind each rule. TRIGGER when the user says "start a session", "sync with main", "claim a subsystem", "commit this", "land this slice", "open a PR", "merge to main", "am I done", or asks why main looks broken. DO NOT TRIGGER for build/toolchain mechanics (duetos-build-and-env), CI polling details (duetos-ci-and-release), or wiki-page editing mechanics (duetos-docs-and-wiki).
---

# DuetOS Change Control

Runbook for getting a change from "idea" to "landed on `main`" without colliding
with the other sessions, worktrees, and fleets that work this repo concurrently.
Shell context: **Git Bash on Windows** unless stated. All repo-root-relative
paths assume `C:\Users\natew\DuetOS-fable-drivers-20260801` (or whichever
DuetOS checkout you are in — most checkouts here are **worktrees**: `.git` is a
*file* pointing at the main repo's `.git/worktrees/<name>`, not a directory).

**Jargon:** a *slice* is one coherent unit of work (feature, fix, audit) landed
as one or a few commits. A *claim* is a registered ownership record in
`PARALLEL_WORK.md`. A *session branch* is the `claude/<slug>` branch the
harness checked out for you.

## When NOT to use this skill

| You need | Go to |
|---|---|
| CMake presets, compilers, build flags, host doctor | `duetos-build-and-env` |
| Running/QEMU-booting the kernel, reading boot logs | `duetos-boot-smoke-and-qemu` |
| Polling CI checks after a push, release process | `duetos-ci-and-release` |
| Which wiki page to update and how | `duetos-docs-and-wiki` |
| Writing tests / what counts as validation | `duetos-testing-and-validation` |
| Past-incident forensics beyond what's cited here | `duetos-failure-archaeology` |

This skill owns the *sequence and gates*: what you must do before editing,
before committing, and before calling a slice done.

## Ground rules (non-negotiable)

1. **All work happens on a `claude/<slug>` session branch. Merge target is
   `main`.** Never push to other branches without explicit permission.
   `tools/parallel/release.sh <sub> --merge` is the only sanctioned
   DIRECT-merge path to `main`, and only with CI green (see
   `CLAUDE_PARALLEL.md`); the PR route with green CI is the other.
2. **Never commit or push while behind `origin/main`.** Rebase first, always.
3. **Never `git add -A`.** Stage explicit paths. The fleet protocol forbids
   `-A` (see the rationale block at the top of
   `tools/test/include-tracked-audit.py`); the hosted `include_tracked` test
   (`tests/host/CMakeLists.txt`, test name `include_tracked`) exists precisely
   because explicit staging forgets brand-new headers — check `git status`
   for untracked files your TUs `#include` before committing.
4. **Never hand-edit `PARALLEL_WORK.md`.** The `tools/parallel/*.sh` scripts
   own it.
5. **Compiling is not done.** The Definition of Done checklist below is the
   exit gate for every slice.

## Session-start ritual (run this before reading code)

```bash
# 1. Where am I, and is anything already dirty?
git branch --show-current        # expect claude/<slug>
git status --short               # expect clean (or explainably not)

# 2. Sync with upstream — FIRST, before diagnosing anything
git fetch origin main
git log --oneline HEAD..origin/main | wc -l   # behind-count
git rebase origin/main                         # if behind
# On conflicts: resolve, git add <files>, git rebase --continue

# 3. Parallel-session check — who else is working this tree?
tools/parallel/status.sh

# 4. Claim your subsystem BEFORE editing anything
tools/parallel/claim.sh <subsystem> "<files/dirs>" "<one-line description>"

# 5. Read the docs entry points
#    wiki/Home.md, wiki/reference/Roadmap.md, CLAUDE.md, CLAUDE_PARALLEL.md
```

Notes on each step:

- **Step 2 is not optional and not deferrable.** Incident (verified in this
  tree): commit `7d2b4271` (2026-08-01) recorded in its body that the full
  kernel link was "blocked upstream: origin/main's service_manifest.h includes
  loader/load_plan.h which is not yet in tree". True *that day* — and
  **inverted by 2026-08-13**: `origin/main` now HAS `kernel/loader/load_plan.h`
  while this stale branch still includes it (`kernel/core/service_manifest.h:32`)
  without the file. A session that trusts an old "main is broken" note — or its
  own stale checkout — diagnoses a phantom. **Rebase before diagnosing "main
  is broken".** If main really looks broken, confirm on a fresh
  `git log origin/main` and a rebased tree first.
- **Step 3/4 mechanics:** `claim.sh <sub> "<files>" "<desc>"` fetches +
  rebases on `origin/main`, warns if the exact Files string is already claimed
  by an IN PROGRESS block (interactive y/N prompt), appends a claim block, and
  commits it. Session identity defaults to `hostname-$$`; override with
  `CLAUDE_SESSION_ID=<id>`. It keeps you on your existing `claude/*` branch,
  or creates `claude/<subsystem>` if you're not on one.
- **`status.sh` output trap — do NOT read `[ACTIVE]` as "someone is editing
  right now".** Measured 2026-08-13 on this branch: `PARALLEL_WORK.md` has
  394 claim blocks, of which 73 are still headed `### [ACTIVE]` (70 with
  `Status: IN PROGRESS`) — including claims whose work has already landed on
  `origin/main` (e.g. `immutable-load-plan`; and `net-driver-ids` /
  `net-driver-ids-smoke` claimed on *this very branch*, whose headline file
  `kernel/drivers/net/nic_ids.h` now exists on `origin/main`). Sessions that
  crash or hand off never run `release.sh`. The real liveness signal is
  `git log` recency on the claiming branch, plus the conflict check at the
  bottom of `status.sh` (two IN PROGRESS claims on the same Files string).
  If the liveness check says the claim IS live, CLAUDE_PARALLEL's rule
  stands unchanged: **stop and coordinate** — the stale-claim statistics are
  never a licence to bulldoze an active claim.
- **Cross-cutting roots are high-conflict:** `kernel/core/`, `kernel/arch/`,
  `CMakeLists.txt`/`cmake/`/`CMakePresets.json`, tree-wide shared headers.
  Coordinate explicitly or stage the change subsystem-locally
  (`CLAUDE_PARALLEL.md` cheatsheet).

## Classifying the change (what gates apply)

| Change class | Extra gates on top of the baseline |
|---|---|
| Docs-only (`.md`, `docs/`, `wiki/`) | Proofread; `docs/sync-wiki.sh sync`, `tools/check-wiki-nav.sh`, `tools/check-wiki-quality.sh`. No build needed. |
| Code (`.h/.hpp/.c/.cpp/.rs/.asm`, CMake) | Full pre-commit battery (next section). |
| Runtime-behaviour code | Also a live QEMU boot smoke — a compile can't prove a boot claim (see `duetos-boot-smoke-and-qemu`). |
| New syscall / ABI surface | Syscall numbers are forever once published. Re-read the CLAUDE.md "Before Writing Code" checklist; be sure. |
| Subsystem (Win32/Linux) code | Must pass the isolation rules — cap-gated syscalls only, no kernel-internal mutation (see `duetos-subsystem-isolation`). |

Anti-bloat sanity gates before you write anything (thresholds are pause-points,
not hard limits): ~500-line `.cpp`, ~300-line `.h`, ~60-line functions, no
helper for a single caller, no system without a wired-in caller. If you can't
name the caller, don't write it.

## Landing a slice (numbered, in order)

1. **Re-sync.** `git fetch origin main && git log --oneline HEAD..origin/main | wc -l`.
   If behind, rebase before anything else. Never commit while behind.
2. **Run the pre-commit battery for your change class.** One-call preflight:

   ```bash
   # check-local.sh assumes a Linux host — run it INSIDE WSL against the
   # scratch tree (see duetos-build-and-env), not from Git Bash on /mnt/c.
   tools/dev/check-local.sh                 # doctor + wiki checks + clang-format
                                            #   dry-run + Rust fmt/clippy + configure
   tools/dev/check-local.sh --build         # ...plus build the preset
   tools/dev/check-local.sh --ctest         # ...plus hosted CTest (implies --build)
   tools/dev/check-local.sh --smoke         # ...plus QEMU boot smoke (implies --build)
   tools/dev/check-local.sh --all           # build + ctest + smoke + static analyze
   # Other flags (verified): --preset <name> (default x86_64-debug), --analyze,
   # --live, --no-doctor, --no-wiki, --no-format, --no-rust, --no-configure
   ```

   Hand-run equivalents live in CLAUDE.md "Pre-commit checks" and
   `wiki/tooling/Git-Workflow.md`. **Never pass `.S` files to `clang-format`**
   — it parses them as C++ and mangles them; the check-local format step only
   sweeps `.h/.hpp/.c/.cpp` for this reason.
3. **Walk the Definition of Done checklist** (from CLAUDE.md — all in the SAME
   commit/slice, not follow-ups):
   - [ ] Re-scan every signal, not just the one you started on: build warnings,
         `ctest --output-on-failure`, clang-format, boot log
         (`tools/test/boot-log-analyze.sh <log>`), CI.
   - [ ] Landed a Roadmap item? **Delete its section** from
         `wiki/reference/Roadmap.md` in the same commit.
   - [ ] Update the owning wiki page (`wiki/<area>/…`), incl.
         `wiki/reference/Win32-Surface-Status.md` if a REAL/STUB/MISSING row
         flipped. (Mechanics → `duetos-docs-and-wiki`.)
   - [ ] Append `wiki/reference/Design-Decisions.md` if the change rules out
         an alternative a future slice could otherwise pick.
   - [ ] Update `wiki/getting-started/History.md` for project-level milestones.
   - [ ] `// STUB:` / `// GAP:` markers present on deliberate omissions,
         absent on code that does its job. Inventory: `git grep -nE "// (STUB|GAP):"`.
4. **Stage explicit paths and commit.**

   ```bash
   git status --short                # check for untracked headers you #include!
   git add kernel/foo/bar.cpp kernel/foo/bar.h wiki/kernel/Foo.md
   git commit -s -m "feat(foo): one-line summary"
   ```

   Commit body: what changed, the evidence (which tests/smokes ran and their
   results), and any tooling committed with it. Commit reusable
   harnesses/scripts into `tools/` in the same slice — don't leave them in
   `/tmp` (CLAUDE.md "Reusable Tooling").
5. **Release the claim / push.**

   ```bash
   tools/parallel/release.sh <subsystem>            # flips claim to [DONE],
                                                    # commits ONLY PARALLEL_WORK.md,
                                                    # pushes branch --force-with-lease
   tools/parallel/release.sh <subsystem> --merge    # ...AND merges to main.
                                                    # ONLY with CI green + no
                                                    # in-flight cross-session deps.
   ```

   `release.sh` deliberately stages only the coordinator file — your
   implementation commits must already exist (step 4). If the push is rejected
   by `--force-with-lease`, someone else moved the branch: fetch, rebase,
   retry. Never blind force-push.
6. **Post-PR: poll CI and fix failures before moving on.** Use the GitHub MCP
   tools, not `gh` (per `wiki/tooling/Git-Workflow.md`). Every red check is a
   fix target. Full polling workflow → `duetos-ci-and-release`.

## Doctrine: Fix Anything You Surface — No Deferring

- **Scope is whatever the signal exposes**, not what fits the commit message.
  A warning, failing test, stale comment, or boot-log `[E]` line that your
  work surfaced is yours to fix now, even if it predates your slice.
- **One investigation per symptom-cluster.** N similar failures → trace ONE to
  root cause before touching the others; the root usually retires all N.
- **No "follow-up slice" notes, no stashing issues in a to-do file.** Fix it now or argue
  concretely why it can't land this session (cyclic dep needing a real
  refactor, change bigger than the context window, missing runtime artefact).
- This supersedes anti-bloat ONLY for things concretely broken right now. It
  does not license speculative refactoring. Debugging technique for the chase
  itself → `duetos-debugging-playbook`.

## Conflict resolution rules

- Rebase conflicts: read each `<<<<<<< HEAD` block individually.
  **Auto-generated sections (`<!-- AUTO:* -->`) → always take upstream**; they
  regenerate via `docs/sync-wiki.sh sync`. Wiki cross-refs are usually
  append-only → take both. Code → behavioural correctness wins; re-run the
  covering self-test.
- **Never blanket `git checkout --theirs` / `--ours` across multiple files.**
- Two sessions edited the same file: don't force-push over the other's work.
  Scope with `git diff origin/main...HEAD`, rebase, resolve, `git commit -s`,
  then re-run `release.sh` (it only commits its own coordinator record).

## Fenced-off wrong paths (each backed by a real incident)

| NEVER do this | Why (incident) | Do instead |
|---|---|---|
| `git stash` (any form) | This repo has **40+ live worktrees** (`git worktree list`, counted 2026-08-13). `refs/stash` is ONE ref shared by ALL worktrees — your `stash drop`/`pop` silently destroys another session's stash. Bit three times in one session on a sibling project (2026-08-10). | `git checkout <sha> -- <paths>` to a clean state, or copy files aside. Valuable stash found? Convert: `git branch wip/<name> <stash-sha>`. |
| `git commit --amend` right after `git checkout <sha> -- <path>` | `checkout <sha> -- <path>` **stages what it restores** — the amend silently absorbs the restored old content and guts the commit (observed 2026-08-11, sibling project). | Check `git diff --cached` before any commit after such a checkout. |
| `git add -A` / `git add .` | Sweeps in other lanes' files, junk trees, and stray artifacts; the fleet protocol forbids it (`tools/test/include-tracked-audit.py` header). | Stage explicit paths; then check `git status` for untracked headers your code includes. |
| Hand-edit `PARALLEL_WORK.md` | The awk parsers in `claim.sh`/`release.sh`/`status.sh` key on exact line shapes (`### [ACTIVE] <sub>`, `- **Status**: IN PROGRESS`); a hand edit breaks release matching. | Use the scripts. |
| Trust `[ACTIVE]` claims or `HANDOFF.md` as current state | 73 of 394 claim blocks are stale-ACTIVE (2026-08-13 count). `HANDOFF.md` at repo root is dated **2026-07-28** for a *different* branch (`claude/host-app-compat`) — its gotchas are good background, its "not pushed"/state claims are stale. | `git log --format="%h %cd %s" --date=short -6 <branch>` for actual liveness; `origin/main` for actual state. |
| Diagnose "main is broken" from a stale tree | The `load_plan.h` inversion above: a true 2026-08-01 claim was false by 2026-08-13. | Rebase (or inspect `origin/main` directly with `git show origin/main:<path>`) before diagnosing. |
| `clang-format -i` on a `.S` file | Parses assembly as C++ and mangles it (CLAUDE.md, `wiki/tooling/Git-Workflow.md`). | Assembly stays hand-formatted. |
| Windows drive path (`C:/...`) into a POSIX tool | In Git Bash/WSL, `C:` is a relative directory name — creates a junk `./C:/...` tree *inside the repo* (U+F03A colon) that `git add -A` then sweeps up (bit 2026-07-26, in DuetOS). | Write `/c/Users/...` or `cygpath -u`. Detect damage: `git status --short | grep -F 'C:'`. |
| Gate repo scripts on `[ -d .git ]` | Most DuetOS checkouts are worktrees where `.git` is a **file** — the gate silently fails and the script publishes wrong values. | Use `git rev-parse --git-dir` / `--is-inside-work-tree`. |
| Push to `main` directly | "Never push to main without explicit permission" — `release.sh --merge` *is* the only opt-in, gated on green CI. | `release.sh <sub>` (branch only), then PR/merge via the sanctioned path. |

## State of this specific worktree (volatile — verify before relying)

As of 2026-08-13 on branch `claude/fable-driver-wave-20260801`
(HEAD `8a55872c`, authored 2026-08-01):

- **207 commits behind `origin/main`, 667 ahead** (`git rev-list --count` both
  directions). This branch is a long-diverged campaign branch; expect real
  rebase work, and expect facts recorded in its commit bodies to be stale.
- The branch's headline work (`kernel/drivers/net/nic_ids.h`) already exists
  on `origin/main` — check what actually still needs landing before assuming
  the whole 667-commit delta is unmerged value.
- `kernel/core/service_manifest.h:32` includes `loader/load_plan.h`, which is
  missing in THIS tree but present on `origin/main` — a full kernel link here
  fails until rebased. That is a staleness artifact, not a bug to fix in place.

## Provenance and maintenance

Authored 2026-08-13 on branch `claude/fable-driver-wave-20260801`
(HEAD `8a55872c`). All commands and line numbers verified against this tree on
that date. Re-verification one-liners (Git Bash, repo root):

- Ahead/behind: `git fetch origin main && git rev-list --count HEAD..origin/main && git rev-list --count origin/main..HEAD`
- Parallel scripts still take these args: `head -12 tools/parallel/claim.sh tools/parallel/release.sh tools/parallel/status.sh`
- Stale-ACTIVE claim count: `grep -c '^### \[ACTIVE\]' PARALLEL_WORK.md` (was 73) and `grep -c '^### ' PARALLEL_WORK.md` (was 394)
- check-local flags: `tools/dev/check-local.sh --help`
- load_plan.h inversion still true?: `ls kernel/loader/load_plan.h; git cat-file -e origin/main:kernel/loader/load_plan.h && echo upstream-has-it`
- Worktree count (stash hazard): `git worktree list | wc -l` (~45 incl. main checkout, 2026-08-13)
- include_tracked gate: `grep -n include_tracked tests/host/CMakeLists.txt`
- HANDOFF.md still stale?: `head -3 HANDOFF.md` (was dated 2026-07-28, branch claude/host-app-compat)
- Doctrine sources: CLAUDE.md ("Git Sync Workflow", "Definition of Done", "Fix Anything You Surface", "Pre-commit checks"), `CLAUDE_PARALLEL.md`, `wiki/tooling/Git-Workflow.md` (168 lines incl. MCP-not-gh rule at the Post-PR section)
