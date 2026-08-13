---
name: duetos-debugging-playbook
description: Symptom-to-triage playbook for DuetOS kernel and subsystem failures. TRIGGER when the symptom report sounds like "kernel panic", "triple fault", "boot hangs", "the boot log just stops", "regression in the boot log", "this test is flaky / fails intermittently", "#GP/#PF in ring 3", "qemu timed out", or "debug this driver/self-test failure". DO NOT TRIGGER for the chronological incident record or past-session postmortems (use duetos-failure-archaeology), for QEMU launch and log-capture mechanics (duetos-boot-smoke-and-qemu), or for how to write new probes/klog lines (duetos-kernel-conventions).
---

# DuetOS Debugging Playbook

Symptom-indexed triage for DuetOS failures: which script to run first, which
recurring bug class the symptom usually maps to, and the discriminating
experiment that separates the candidates. This skill owns the **symptomatic
index**; the chronological record of past incidents lives in
`duetos-failure-archaeology`. Run/log-capture mechanics live in
`duetos-boot-smoke-and-qemu`; probe/klog authoring rules live in
`duetos-kernel-conventions`.

**Shell context:** every `tools/...` command below is a bash script — run it
inside WSL (builds/logs live in the WSL scratch tree; see
`duetos-build-and-env`).

**Scope note (2026-08-13):** this worktree's branch was cut 2026-08-01. Several
fixes and one tool cited below landed on `origin/main` on 2026-08-03
(commits `8cfcc470`, `dd45d709`, `3d6e870a`, `285278ca`, `05121fe8`,
`83717fd0`, `cb53f104`, and scanner `3e730aa1`). They are NOT ancestors of
this branch's HEAD — `git merge-base --is-ancestor <sha> HEAD` fails — so on
an un-rebased tree you may re-encounter the exact bugs they fixed, and
`tools/test/check-spinlock-log-order.py` will not exist on disk. Rebase on
`origin/main` first — sync ritual and the stale-branch inversion story in
`duetos-change-control`.

## First moves — any failure

All four analyzers consume a captured COM1 serial log; they are
launcher-agnostic (QEMU stdout, VMware/VBox serial-to-file, real-HW UART).

| Step | Command | What it answers | Exit codes |
|------|---------|-----------------|------------|
| 1 | `tools/test/boot-log-analyze.sh [logfile]` | Full regression scan: completion sentinels, self-test PASS/FAIL, lockdep pairs, phase timings, SMP banner, final `verdict: OK/ATTENTION` | 0 = completed + no non-deliberate failure; 1 = attention; 2 = no log found |
| 2 | `tools/test/boot-progress-localizer.sh [logfile]` | "Boot reached X, next expected Y" against the ordered sentinel list | 0 = reached final sentinel; 1 = stopped early; 2 = no log |
| 3 | `tools/qemu/triage-truncated-boot.sh <serial.log> [qemu-int.log]` | Log-just-stops disambiguation: guest fault vs QEMU abort vs hang | 0 = completed; 2 = hang/timeout; 3 = guest fault; 4 = host-emulator abort; 64 = usage |
| 4 | Raw fallback | `grep -nE "\[E\] \|PANIC\|TRIPLE\|FAIL\|out of range\|task-kill\|kernel oops" /tmp/duetos-*.log` | — |

With no argument, steps 1–2 auto-pick the newest of `/tmp/duetos-*.log`,
`build/*/stress-*.log`, `build/*/*.log`. Step 3 requires the serial log path
and guesses the QEMU `-d int` trace (`build/<preset>/qemu.log`) if omitted —
that trace recovers the last guest exception vector+RIP even when a QEMU
abort killed the VM mid-panic-dump, so a host crash can never fully mask a
guest bug. Symbolize raw panic addresses with
`tools/debug/symbolize.sh [kernel.elf] < panic_log.txt` (defaults to
`build/x86_64-debug/kernel/duetos-kernel.elf`; needs llvm-symbolizer or
addr2line).

## Symptom index

| Symptom | First check | Likely class | Discriminator |
|---------|-------------|--------------|---------------|
| Log just stops, no sentinel | `triage-truncated-boot.sh` | hang vs guest panic vs QEMU abort | exit code 2/3/4; `[panic-precis]` line present = guest fault |
| Boot "hangs" at a known line | `boot-progress-localizer.sh`, then re-run with a longer timeout | pathological slowness, not a hang | does the next sentinel appear at 3–5x the timeout? (see "Freeze vs slowness") |
| Ring-3 #GP/#UD/#PF at valid-looking RIP inside a DLL | loader base logs | lost-page / base collision | two mappings logged at the same VA |
| Test fails this run, passed last run | `boot-determinism-sweep.sh` | ASLR / scheduling / timing sensitivity | failure rate over >= 6 boots; intermittent = still a bug |
| Self-test FAILs only with >1 vCPU | check the oracle, not the code under test | global-counter oracle unsound under SMP | does the assert read a global a peer CPU can move? |
| Expected serial signature MISSING but run looks healthy | read the raw log around the label | serial interleaving, not a real failure | label and verdict split across lines; other verdicts confirm pass |
| Kernel halts / refuses on a newly-added enum member | grep the predicate | whitelist incompleteness | per-call-site allow-list missing the new member |
| WARN/ERROR floods on a normal workload | log call site | log-level abuse | the "failure" is a legitimate API return (timeout, non-owner release) |
| Task hung for minutes, zero hung-task warnings | heartbeat trace in log | watchdog blockable by watched subsystem | did the detector's own last beat stop at the hang time? |
| Smoke matrix wedges after tasks finish, boot keeps ticking | lockdep section of `boot-log-analyze.sh` | ABBA: lock held across KLOG | one CPU holds subsystem lock blocked on serial lock |
| Object never freed / use-after-free | walk every exit path | refcount asymmetry | an error/orphan path skips the matching release |
| Test justified by a comment fails | diff comment vs code | stale-comment drift | code moved on; fix both |
| Two paths disagree on a stand-in value | grep both spellings | sentinel divergence | `"X:\\"` vs `"C:\\"` style split; align both |

## Class-of-bug shapes

These recur across slices. When a symptom matches a shape, check for the
class before chasing the calling code (source: CLAUDE.md "Fix Anything You
Surface", plus dated incidents).

### Lost-page / lost-slot collision

Two structures share a randomized base, a fixed VA, or a slab class;
whichever landed LATER silently overwrites the EARLIER, and the earlier's
callers fault at a valid-looking RIP.

- **Symptom:** ring-3 #GP/#UD/#PF at an address inside a DLL or stub region;
  or a kernel value that read back changed with no observable writer.
- **Discriminator:** two base-address log lines at the same address, or two
  slab callers with the same cache pointer. The DLL loader logs
  `KLOG_DEBUG_V("loader/dll", "DLL load BEGIN base_va", base_va)`
  (`kernel/loader/dll_loader.cpp`) — boot with debug klog enabled and
  compare bases. `kernel/loader/pe_loader.cpp` also hard-fails
  `ImageBase overlaps loader-reserved VA band`.
- **Precedent:** the vcruntime140 memmove crash, 2026-05-11.

### Stale-comment drift

A comment claims a behaviour the code no longer implements ("v0 returns
empty cmdline" when the code now routes through a populated proc-env page).

- **Symptom:** a test that the comment justifies fails.
- **Discriminator:** read the code, not the comment. Fix both in one commit.

### Sentinel divergence

Two paths claim the same v0 stand-in value but spell it differently
(e.g. `"X:\\"` vs `"C:\\"`).

- **Symptom:** a smoke test checking one sentinel flags the other path.
- **Discriminator:** grep for both spellings; one was updated, the other
  wasn't. Pick one and align both.

### Whitelist incompleteness

A predicate enumerates the legal set explicitly (`if (x == A) ...`) but a
new member was added elsewhere and the predicate wasn't updated.

- **Symptom:** kernel halts / refuses / mis-routes on the new member only.
- **Discriminator:** per-call-site allow-lists. Fix: add the member AND
  consider converting to a property test so future additions don't need a
  new whitelist edit.
- **Precedent:** the F9 IrqInstall halt, 2026-05-11.

### Refcount asymmetry

Acquire path adds a reference the release path doesn't drop, or vice versa.

- **Symptom:** object pinned forever, or use-after-free.
- **Discriminator:** walk EVERY exit from the acquiring scope — each one must
  either have succeeded-and-handed-off OR failed-and-rolled-back. The
  asymmetric path is almost always an error/orphan/early-return leg.

### Log-level abuse

A WARN/ERROR fires on a legitimate API failure mode
(`WaitForSingleObject` timeout, `ReleaseMutex` from non-owner).

- **Symptom:** log floods on normal contended workloads.
- **Discriminator:** is the "failure" part of the API's contract? Demote to
  DEBUG; the caller's return-value handling is the real notification
  channel. Gating rules: `duetos-kernel-conventions`.

## Intermittency discipline

**One run is never enough.** If a test crashes on this run but not the last
one, the bug is ASLR / scheduling-order / hash-order / work-steal-timing
dependent. Intermittent bugs ARE bugs — they hit in production
proportionally to their sensitivity. Never conclude "the previous run was
fine, so this is flaky and not worth fixing."

```bash
tools/test/boot-determinism-sweep.sh [runs] [per_boot_timeout_s]   # defaults: 8 runs, 120 s
```

The sweep boots N times, runs `boot-log-analyze.sh` on each, and diffs
per-boot signal into `/tmp/sweep-summary.tsv` (verdict, completion,
`[smp] online=N/M`, self-test OK/FAIL/SKIP, panic count, distinct lockdep
pairs). A deterministic kernel must produce byte-stable counts; raw lockdep
inversion COUNT is legitimately timing-variable (global held-stack) and is
reported, not failed on — a NEW distinct pair across runs is a real finding.
Keep the per-boot timeout generous: AP bring-up lands ~t=20000 ms guest, and
a tight cap truncates slow TCG boots before the `[smp]` sentinel, faking an
"aps varies" result.

**The >= 6-boot rule and the red-herring lesson** (commit `a0d56701`,
2026-05-17): a fix that looked correct on a single boot crashed 3 of 6 boots
in the sweep — and the first attempt chased a noise line that also appears in
known-clean boots. Full incident narrative: `duetos-failure-archaeology`.
Two lessons:

1. Before treating any log line as your regression oracle, confirm it is
   **absent from a known-clean boot** (or the reverted tree). A line that
   also appears in clean boots discriminates nothing.
2. Run at least 6 boots before declaring an intermittent symptom fixed
   (`a0d56701` used "6/6 OK, 0 panic" as its acceptance bar).

## SMP-era oracle traps

All six fixes below landed on `origin/main` 2026-08-03, triggered by SMP
actually starting to work. The bug class: a test or diagnostic whose
**oracle** was only sound on a quiescent uniprocessor. If your tree predates
them (this branch does — see scope note), you can hit the originals. Full
incident narratives: `duetos-failure-archaeology`. The symptoms and distilled
rules:

| Trap (symptom) | Rule | Incident |
|------|------|----------|
| Global-counter oracle (self-test FAILs only with >1 vCPU) | A global counter is a sound oracle only quiescent-uniprocessor. Gate the hard failure on `SmpCpusOnline() > 1` (degrade to KLOG_WARN + KBP_PROBE) and assert the real invariant per-address-space instead. Do NOT widen tolerance — a slack bump blinds the check in both modes. | `8cfcc470` |
| Cross-CPU fault injection (unrelated subsystem panics during an injection test) | Scope test-only fault injection to the task that armed it. A harness that injects faults into unrelated subsystems manufactures failures indistinguishable from product bugs. | `dd45d709` |
| Serial interleaving (expected signature MISSING though the run passed) | The write syscall is line-atomic PER CALL, never across calls. Emit each grep-able verdict as one write. When a signature is MISSING, read the raw log around the label before believing the failure. | `3d6e870a` |
| "Must stay 0" invariant fires constantly | When a stay-at-zero counter goes non-zero, make the invariant TRUE again (close the window); do not weaken the claim to match the bug. A constantly-false regression signal is worse than none. | `285278ca` |
| Blockable watchdog (long hang, zero hung-task warnings) | A watchdog the watched subsystem can block is not a watchdog. Diagnostic writers get bounded lock acquisition; if the log shows a hang with no watchdog output, check when the detector's own last beat was. | `83717fd0` |
| Lock held across KLOG (tasks finish, summaries never emit, boot ticks to timeout) | Never hold a spinlock across a klog call. Tree-wide scanner: `tools/test/check-spinlock-log-order.py` (added `3e730aa1`, on origin/main only — rebase to get it). | `05121fe8` |

## False-signal traps — before you believe a pass OR a fail

- **`wsl.exe -- bash -lc '...'` fabricates exit codes** — use
  `wsl.exe -e bash -lc '...'` or a sentinel branch. Full write-up:
  `duetos-build-and-env` "Trap zero".
- **`ctest-boot-smoke.sh` without its binary-dir argument exits 2** with a
  one-line usage message that scrolls past like a pass — exit 2 is
  environment skip **or** usage error; always check which. Full exit
  contract: `duetos-boot-smoke-and-qemu`.
- **SKIP != PASS.** Exit 2 / "SKIP" lines mean the check did not run. The
  absence of a FAIL line is not proof of pass — it can be proof the
  self-test was never called; verify the hook exists.
- **`-fsyntax-only` passes code that fails to LINK** (commit `cb53f104`).
  Recurring class: a shared kernel TU gains a dependency and a non-kernel
  consumer (tests/fuzz, tests/host) breaks at link. Run
  `make -C tests/fuzz all` after touching a shared TU — the actionable
  host_shim link-drift rule lives in `duetos-testing-and-validation`.
- **Metric-as-floor.** A falling error count can mean "the run got less far",
  not "it got better" — truncation, early exit, a failed launch, a timeout
  all shrink a count. Compare counts only when both runs reached the same
  stage; a count of exactly 0 or 1 is the classic infrastructure-failure
  tell. Ask "could this number be a floor?" before quoting it as progress.

## Freeze vs slowness

Before declaring a hang, check for progress at a longer timeout. Precedent
(commit `071ac8d6`, 2026-06-02): the x86_64-release-audit boot
"deterministically froze" right after `[ec-selftest] PASS`. It was NOT a
hang — `crypto::BigIntMod`'s bit-by-bit long division ran over the full
128-limb width for every reduction of a ~12-limb P-384 modulus; under TCG an
ECDSA verify took minutes, starving the timer so the guest clock *appeared*
frozen. The discriminating experiment there was bounded serial heartbeats
inside the suspect loops: a hang emits nothing; pathological slowness keeps
beating. Cheap first pass: re-run with `DUETOS_TIMEOUT` at 3–5x and see
whether the next sentinel eventually appears
(`triage-truncated-boot.sh` classifying "hang" only tells you the harness
timed out, not that the guest stopped computing).

## Ring-3 fault triage

A #GP / #UD / #PF in ring 3 at a valid-looking RIP inside a DLL or stub
region is the signature of a **base collision** (two mappings at the same
VA — see "Lost-page / lost-slot collision" above), not usually of the
faulting code itself.

1. Symbolize the fault: `tools/debug/symbolize.sh` resolves kernel
   higher-half addresses; user-mode addresses need the loader's base logs.
2. Boot with debug klog visible and collect every
   `DLL load BEGIN base_va` line (`kernel/loader/dll_loader.cpp`); look for
   two loads at the same base, or a load at a base something else already
   claimed.
3. Only after ruling out a collision, disassemble at the RIP
   (`tools/debug/disasm-at.sh`) and treat it as a code bug.
4. Localization guides for this family live in
   `tools/debug/PE-WIN32-FAILURE-LOCALIZATION.md` and siblings
   (`HANG-`, `CORRUPTION-`, `LOCK-DEADLOCK-`, `SMP-BRINGUP-`,
   `PERF-REGRESSION-LOCALIZATION.md`) — read the matching one before
   re-deriving a method.

## Tooling shelf

| Tool | Use |
|------|-----|
| `KBP_PROBE(...)` / `KBP_PROBE_V(...)` (`kernel/debug/probes.h`) | Fire a breakpoint-able probe on a failure leg; `ProbeArm::ArmedLog` probes log only when a regression occurs. Extend `ProbeId` + `kProbeTable` (one row each in probes.h/.cpp) for a new failure category. Authoring rules: `duetos-kernel-conventions`. |
| `tools/debug/duetos-gdb-attach.sh` | Live GDB against the running kernel (TCP default, `--via-pty`, `--com /dev/ttyUSB0` for real HW; `--demo` halts at int3). Then `b duetos::debug::ProbeFire` halts at the exact frame a probe fires in. |
| `probe` shell command (kernel shell, admin) | `probe list` / `probe arm <name> [--suspend]` / `probe disarm <name>` / `probe arm-all` / `probe disarm-all` — runtime probe arming without a rebuild. |
| `kernel/debug/` | breakpoints, tripwire, watch (data watchpoints), extable, disasm, syscall_scan, hot_patch, inspect. |
| lockdep (`kernel/sync/lockdep.{h,cpp}`) | Lock-order inversion detection; `boot-log-analyze.sh` summarizes distinct `(held,id)` pairs — the selftest-A/B pair is the deliberate self-test; any other pair is a finding. Held-set is currently global (not per-task), so raw counts are timing-variable. |
| fix journal (`kernel/diag/fix_journal.{h,cpp}`, shell `dfix`) | Kernel-side ledger of applied diagnostics/fixes; read-only `dfix` queries need no admin. `tools/qemu/dfix-apply-interactive.sh` / `dfix-to-branch.sh` consume it host-side. |
| `tools/debug/symbolize.sh` | Attach `function+offset (file:line)` to raw panic hex addresses. |
| `tools/debug/decode-panic.sh`, `duetos-cpu-state.sh`, `soft-lockup-resolve.sh` | Panic decode, CPU state dump, soft-lockup resolution helpers. |

## Hypothesis discipline

- **One investigation per symptom cluster.** When N similar failures appear,
  trace ONE to root cause before touching the others; the root usually
  retires the cluster. Patch-each-symptom is how the fragile-workaround
  long tail grows.
- **Record discarded hypotheses in the commit body.** Commit `58cef239` is
  the model: "Root cause was NOT the compositor lock / pending redraw (the
  prior hypothesis): the 64-byte scancode ring never overflowed... a DEBUG
  kbd-ev trace showed the suppressor — not the ring — discarding the
  presses." A recorded negative saves the next session from re-walking the
  same dead end. The chronological home for these is
  `duetos-failure-archaeology`.
- **Name the discriminating experiment before running it.** For each
  candidate class, state what you expect to see if it is true AND if it is
  false (e.g. bounded heartbeats: silence = hang, beats = slowness; clean
  baseline grep: line present = not an oracle).
- **Keep the diagnostics you added, gated.** WARN for the failure summary,
  `KLOG_DEBUG_V` for detail, a probe on the failure leg — a clean boot stays
  quiet, a regression boot leaves a trail. Mechanics and gating rules:
  `duetos-kernel-conventions`.

## Provenance and maintenance

Authored 2026-08-13 against worktree branch `claude/fable-driver-wave-20260801`
(HEAD `8a55872c`, 2026-08-01) plus commit bodies read from `origin/main`.
Volatile facts and how to re-verify each:

```bash
# Analyzer scripts still exist with the documented contracts (read headers):
head -45 tools/test/boot-log-analyze.sh tools/test/boot-progress-localizer.sh
head -50 tools/qemu/triage-truncated-boot.sh tools/test/boot-determinism-sweep.sh
head -35 tools/test/ctest-boot-smoke.sh tools/debug/symbolize.sh

# Cited commits still resolve (worktree may need `git fetch origin main` first):
git log --format="%h %ad %s" --date=short -1 a0d56701   # UBSAN red herring / 6-boot rule
for c in 8cfcc470 dd45d709 3d6e870a 285278ca 05121fe8 83717fd0 cb53f104 3e730aa1; do \
  git log --format="%h %s" -1 $c; done                   # SMP-era oracle traps (2026-08-03)
git log --format="%h %s" -1 071ac8d6                     # freeze-vs-slowness (BigIntMod)
git log --format="%h %s" -1 58cef239                     # recorded discarded hypothesis

# Scope note still true? (empty output = commits now ancestors; delete the note)
git merge-base --is-ancestor 8cfcc470 HEAD || echo "still-not-in-HEAD"
ls tools/test/check-spinlock-log-order.py 2>/dev/null || echo "scanner still origin/main-only"

# Probe shell command + lockdep + fix journal still present:
grep -n '"probe"' kernel/shell/shell_dispatch.cpp
ls kernel/sync/lockdep.h kernel/diag/fix_journal.h kernel/debug/probes.h

# Loader base-collision discriminator log line still emitted:
grep -n "DLL load BEGIN base_va" kernel/loader/dll_loader.cpp
```

Class-of-bug shapes and the 2026-05-11 precedents (vcruntime140 memmove,
F9 IrqInstall) are sourced from the project CLAUDE.md "Fix Anything You
Surface" section — if that section changes, reconcile this skill against it.
