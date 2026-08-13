---
name: duetos-failure-archaeology
description: >-
  Chronological record of every major DuetOS investigation, dead end, revert, and rejected fix, so no session
  re-derives or re-attempts them. TRIGGER when asking "has this failed before", "why was this reverted",
  "history of this bug", "did we try this already", "was there a past incident here", "post-mortem for X",
  "what went wrong with X", or before re-attempting an optimisation/diagnostic that may have been tried.
  DO NOT TRIGGER for live-symptom triage of a failure happening right now — use duetos-debugging-playbook
  (the symptom-indexed sibling); for process/revert mechanics use duetos-change-control.
---

# DuetOS failure archaeology

This is the **chronological incident ledger**: what was attempted, what failed, why, and what was learned.
Use it BEFORE re-attempting anything that smells like a prior attempt (a compositor perf gate, a lockdep
change, a keyboard-input heuristic, a global test counter). The symptom-indexed "my boot is broken NOW"
runbook is the sibling `duetos-debugging-playbook`; this file is history, not triage.

Every SHA below was re-verified on 2026-08-13 with `git log -1 --format="%h %s" <sha>` and its body read via
`git show -s <sha>`. Entries marked **(origin/main)** are NOT reachable from this worktree's HEAD
(`8a55872c`, branch `claude/fable-driver-wave-20260801`, 2026-08-01) — they exist only on `origin/main`,
which extends ~207 commits further (to 2026-08-05 as of this writing). Read them with
`git show -s <sha>` (fetch first if missing).

## Era map (verified against `git log` on 2026-08-13)

| Era | Dates | Shape |
|-----|-------|-------|
| Bring-up | 2026-04-19 → 2026-06-17 | ~3,300 commits; kernel, drivers, Win32 subsystem from zero |
| Dormancy | 2026-06-17 → 2026-07-26 | Zero commits for ~5.5 weeks |
| Host-app compat | 2026-07-26 → 2026-07-30 | Off-the-shelf Windows .exe compatibility push (see `HANDOFF.md`) |
| Parallel fleet | 2026-07-31 → 2026-08-01 | 654 commits in 2 days; many concurrent sessions via claim/release |
| (origin/main only) | 2026-08-02 → 2026-08-05 | Landing branch merges + the 2026-08-03 SMP-fallout fix wave |

Fleet-era caution: HEAD's history holds **290** commits titled `feat(<subsystem>): complete subsystem
[session <name>]` against **356** `chore: claim subsystem` commits — arithmetic says ~66 claims never got a
matching completion. A claim commit is NOT evidence the work landed; check for the completion commit.

---

## Incidents, chronological

### Knowledge-tier retirement (2026-05-04, `0d5d9ccc`)

- **Symptom**: two documentation tiers (wiki + `.claude/knowledge/`) duplicating content; references rotting.
- **Root cause**: same facts maintained in two homes; slice-numbered postmortems became dead ends once folded
  into the wiki.
- **Evidence**: `0d5d9ccc` "Consolidate docs into the wiki — retire .claude/ knowledge tier" deleted ~120
  `.claude/knowledge/*.md` files and created `wiki/reference/Roadmap.md` from the old plan files.
- **Status/Lesson**: settled precedent — **`wiki/` is the single docs home**. `.claude/skills/` files (like
  this one) must cross-reference wiki pages, never fork their content.

### Self-reverted wrong fix: boot_pd corruption (2026-05-06, `bca65d48` → `cd17659e`)

- **Symptom**: triple fault under `smoke=pe-winapi`; `#PF` at `Fat32Guard` ctor, CR2 in FAT32 `.bss`, and the
  `#PF` handler's own `.text` page ALSO not present → infinite instruction-fetch fault loop → triple fault.
- **Root cause (of the incident, not the bug)**: `bca65d48` removed the boot `DiskPersistSelfTest` on the
  hypothesis it was the regressor. 18 minutes later `cd17659e` reverted it: *"My fix was wrong: 32eb714's
  DiskPersistSelfTest is NOT the regressor."* The fault reproduced identically with the fix applied. The real
  shape: `boot_pd` (PD[0] for `.text`, PD[5] for `.bss`, same PML4/PDPT) zeroed by a mass write through the
  direct-map alias — two distant VAs dying together fingered option (a), the whole PD page zeroed.
- **Evidence**: `cd17659e` body carries the full paging forensic (RIP/CR2/PD-entry walk). The diagnostic
  PML4 trip-wire `kernel/mm/kernel_half_watch.{cpp,h}` had been reverted the same morning (`729c18d4`),
  so the hunt lost its instrument.
- **Status/Lesson**: the disk-poisoning issue (`KERNEL.LOG` overlapping the NVMe reserved region) and the
  kernel-half PT corruption were **two distinct bugs**; conflating them cost a fix-revert cycle. Reproduce
  the exact fault WITH your fix applied before claiming the fix. Model postmortem-in-commit-body — read
  `git show -s cd17659e` in full before any future paging-corruption hunt.

### Checked-in smoke binaries purge (2026-05-09, `8fd9f515`)

- **Symptom**: ~129 compiled smoke-test `.exe` fixtures tracked in git.
- **Root cause**: fixtures committed instead of generated.
- **Evidence**: `8fd9f515` "Generate smoke PE fixtures in CMake" — deletion of 129 `.exe` files.
- **Status/Lesson**: settled — **generate test binaries at build time, never commit them**. If you find a
  checked-in binary fixture, that is a regression against this decision.

### Compositor dirty-gate revert (2026-05-17, PR #286 → `fd366763` / PR #288 `f2444a02`)

- **Symptom**: after the perf gate landed, the screen sat **frozen until a click** — the compositor skipped
  frames it wrongly judged clean.
- **Root cause**: a partial "skip compose when nothing changed" optimisation without full damage tracking is
  **unsound by construction** — sources of change that don't feed the dirty signal (cursor, async writers)
  are invisible to it.
- **Evidence**: `fd366763` reverts the PR #286 merge; `f2444a02` is the PR #288 revert merge.
- **Status/Lesson**: open — do not re-attempt a dirty gate piecemeal. The precondition is a full
  damage-driven compositor where every writer reports damage. See also the cursor entry below (same organ).

### Lockdep per-task held-set saga (2026-05-17, `7dc0b2d4` → `6990d020` → `a0d56701` → `1a2e5ed2`)

- **Symptom**: lockdep's single global held-class array reported a false lock-order inversion ~40x/boot for
  two tasks correctly holding two different sleeping mutexes (compositor vs fat32) across a context switch.
- **Attempt 1** (`12bec365`, reverted by `6990d020`): per-task held-set snapshot/restore. Single-boot clean,
  but a **6-boot determinism sweep** caught an intermittent **3/6** AP-bringup crash: `LockdepHeldRestore`
  via `Current()` at the TOP of `SchedFinishTaskSwitch` runs on the fresh-AP path before PerCpu is armed.
  The clang-format follow-up was reverted too (`07ce76cc`) — reverts must take their satellites.
- **Signature correction** (`a0d56701`): the sweep on the reverted tree showed the
  `[ubsan] tm-detail null-deref ty='PerCpu'` lines occur ~4x/boot in KNOWN-CLEAN boots (benign AP-reads-
  PerCpu-early noise). The real oracle is strictly the `WaitQueueBlock on non-Running task` KASSERT
  (0 in baseline, 0 reverted, present only with the commit). **The first regression signature was wrong.**
- **Attempt 2** (`1a2e5ed2`, landed): restore moved AFTER the existing `lock_ptr == nullptr` fresh-AP
  early-out, reads `pcpu->current_task` directly, gated on `state == Running`. Verified with an 8/8-boot
  sweep on the UBSAN build that exposed attempt 1.
- **Status/Lesson**: fixed. Two durable rules: (1) **">=6-boot sweep, never a single boot"** for anything
  touching context switch / AP bring-up — attempt 1 was single-boot-correct and 3/6 broken; (2) validate
  the regression signature itself against known-clean boots before chasing it.

### Cursor-in-compose v2-v4 revert (2026-05-18 → 2026-05-24, `d77478e3`, `d887c41f`)

- **Symptom**: dozens of residual cursor sprites ("trails") scattered across the screen after rapid mouse
  movement; three successive attempts each made it worse.
- **Root cause**: `MouseReader`'s `CursorMove` writes the **live framebuffer directly**, bypassing the
  compositor's snapshot tracking. The frame-elision diff compares offscreen vs snapshot only, so stale
  cursor pixels on the live FB are invisible to it and never erased. v2-v4 amplified this by building
  `g_backing` from `SaveAt(live FB)` — capturing stray cursor pixels into backing.
- **Evidence**: `d887c41f` "revert v2-v4 cursor-in-compose attempts (caused trails)" (full mechanism in
  body). Note `d77478e3` had landed the elision with **`[UNVALIDATED]` in its own subject** — a self-labeled
  unverified change that then interacted badly.
- **Status/Lesson**: reverted to v1 (CursorHide/Show with motion-region intersection; cursor flickers in
  motion regions, solid elsewhere). The real fix — compositor tracking of MouseReader's live-FB writes
  (snapshot invalidation hook) — is **open**. Do not re-attempt cursor-in-compose without it.

### Host-shim link-drift class (recurring, 2026-05-29 → 2026-08-03, 8+ commits)

- **Symptom**: a kernel TU gains a dependency; the kernel builds; a **non-kernel consumer** (fuzz target or
  hosted test that links that TU against `tests/fuzz/host_shim/` stubs) breaks at link time.
- **Root cause**: the host-shim stub files are part of any change to a shared kernel TU, but sessions treat
  them as follow-up cleanup. `-fsyntax-only` cannot catch it — both TUs compile fine; only the link notices.
- **Evidence** (the fix-cluster, all verified): `f0c0db5d`, `d945fff6` (2026-05-29), `3e9c071b`,
  `a94f5939` (05-29/30), `cc0ba48d`, `ba2fd245` (06-02), `e0415f04` (06-10), and `a04416f9` (2026-08-03,
  **origin/main**). Plus `cb53f104` (**origin/main**) — see the watchdog entry below — whose body states it
  was "the THIRD time this session a shared kernel TU gained a dependency and a non-kernel consumer broke"
  (`elf_loader.cpp` → fuzz_elf/fuzz_pe, `pe_loader.cpp` → MutexLock, `fat32.cpp` → MutexLockTimed).
- **Status/Lesson**: class is alive. When touching a kernel TU that fuzz/host tests link, **run the fuzz/host
  link** (`make all` in `tests/fuzz` linked all 71 targets in `cb53f104`'s verification), and treat the
  host-shim stub edit as part of the same commit. Syntax-check is not a link.

### Keyboard auto-repeat 4-commit chain (2026-05-30 → 2026-06-07, `acedc021` → `d2cfdf23` → `679857b4` → `58cef239`)

- **Symptom** (initial): one key tap produced repeated characters under VirtualBox.
- **The chain**: `acedc021` added a software typematic guard → `d2cfdf23` switched to release-gap detection
  (VirtualBox ACKs the `0xF3` set-typematic command **and then ignores it**) → `679857b4` switched the gap
  clock to `MonotonicNs` (TickCount lags under TCG) → `58cef239` (F-002/F-003) gated the whole suppressor to
  VirtualBox only, because on every other host it **dropped real keys**: fast typing 'peek' came out 'PEK',
  and a 6-press start-menu nav landed 3 rows short and opened CLOCK instead of IMAGE VIEWER.
- **Root cause** (final): the ~100 ms same-key re-press discriminator cannot distinguish host auto-repeat
  from a legitimate fast burst; only VirtualBox needs it (QEMU/KVM/VMware/real HW honour `0xF3`).
- **Evidence**: `58cef239` body also records the **discarded hypothesis**: it was NOT the compositor lock /
  pending redraw — the 64-byte scancode ring never overflowed; a DEBUG kbd-ev trace showed the suppressor
  discarding the presses. F-003 was a symptom of F-002, not a separate nav-table bug. Repro rig committed
  as `tools/test/drivers/f002-fastnav-repro.py`.
- **Status/Lesson**: fixed. Host-behaviour workarounds must be **gated on the detected hypervisor**
  (`HypervisorInfoGet().kind`), never applied globally. Record discarded hypotheses in the commit body.

### Win32 ABI-shape bugs (2026-05-30, `bedb2380`, `911824b2`)

- **`bedb2380`** — `__iob_func` classified as a DATA import: the IAT slot pointed at an RW/NX page, the CRT's
  `call [__iob_func]` fetched instructions from NX → `#PF` 0xc0000005 → no-op `__C_specific_handler`
  returned ContinueExecution → **SEH livelock, 47-63 deliveries** until boot timeout (timeout.exe, clip.exe).
  Fix: it is the CRT accessor *function*; route to the executable miss-thunk.
- **`911824b2`** — `WIN32_FIND_DATAW` used `long long` for the three FILETIME fields; 8-byte alignment
  inserted 4 pad bytes, shifting `cFileName` from offset 44 to 48. Real `cmd.exe` read at the
  `<windows.h>` offsets and faulted on a malformed name. Fix: FILETIME is two DWORDs (4-byte aligned);
  `_Static_assert`s now pin sizeof and offsets.
- **Related standing rules** (documented in `HANDOFF.md`, 2026-07-28, "Three ABI rules, now documented not
  folklore"): (1) **i386 lock structs are HALF the x86_64 size** — SRWLOCK 4 vs 8, CRITICAL_SECTION 24 vs
  40; HANDOFF calls it the **third instance of this class** after the 28-byte MSG and WNDCLASSEX's field
  shift. (2) A syscall returning 64 bits in `rax` is unusable from PE32 — only `eax` survives the
  compat-mode return. (3) An i386 `u64` local pins EBP, breaking 6-arg syscalls in that TU — stage u64s as
  aligned u32 pairs.
- **Status/Lesson**: fixed individually; the **class** (guest-visible struct layout diverging from the
  Microsoft ABI) recurs. Before writing any struct that crosses the PE boundary, pin sizeof + hot-field
  offsets with static_asserts against documented Windows values, per bitness.

### The "freeze" that was slowness (2026-06-02, `071ac8d6`)

- **Symptom**: x86_64-release-audit boot deterministically "froze" right after `[ec-selftest] PASS`; guest
  clock appeared stuck at ~9.3 s.
- **Root cause**: not a hang — pathological slowness. `crypto::BigIntMod`'s bit-by-bit long division shifted
  and subtracted across the FULL 128-limb (4096-bit) width for a ~12-limb P-384 modulus, ~768 iterations per
  reduction, ~4,600 reductions per ECDSA P-384 verify. Under TCG: minutes per verify, starving the timer.
- **Evidence**: `071ac8d6` — bounded serial heartbeats in the EC loops proved forward progress. Fix bounded
  the per-bit work to the modulus' active limb window (`m.used + 1`), plus x509 DN-equality gating.
- **Status/Lesson**: fixed. Before declaring a hang, prove the loop is not merely slow (heartbeat probes).
  An O(full-width) inner loop on a fixed-width bignum is the standing suspect for "crypto froze the boot".

### AP boot sentinel ordering (2026-07-28, `0c668495`)

- **Symptom**: intermittent AP-bringup fault the Roadmap carried as "root cause still open": UBSAN null-deref
  `ty='Task'` + `WaitQueueBlockTimeout on non-Running task cpu=0x1` — the same signature family as the
  2026-05-17 lockdep attempt-1 crash.
- **Root cause**: `SchedEnterOnAp` ran `SchedStartIdle` FIRST and minted/installed the boot sentinel after.
  The existing comment reasoned correctly about the LAPIC timer consumer ("install as current_task BEFORE
  arming the timer") but **missed a NEARER consumer**: `SchedStartIdle` itself allocates (`SchedCreate` →
  KMalloc) and takes locks, all with `current_task == nullptr` on a memset-zeroed fresh AP PerCpu.
- **Evidence**: `0c668495` body carries the captured boot sequence pinning the window exactly.
- **Status/Lesson**: fixed. A comment proving ordering against ONE consumer is not proof against all
  consumers in the window. Enumerate every `Current()`-reaching call between the two points.

### Design-Decisions NUL corruption — a guard that silently no-ops (2026-07-28, `HANDOFF.md`)

- **Symptom**: the wiki stale-reference check stopped finding anything, silently.
- **Root cause**: `wiki/reference/Design-Decisions.md` held **3,136 NUL bytes** from a concurrent append in
  an earlier session. `grep` classified the file as binary; the check had been emitting
  `references missing path 'Binary file ... matches'` instead of working.
- **Evidence**: `HANDOFF.md` ("Plus:" paragraph after the numbered bug list). No content was lost; once
  fixed, the check immediately found a real stale path.
- **Status/Lesson**: fixed. Two morals: concurrent appends to a shared wiki file can corrupt it (fleet-era
  hazard), and **a guard whose failure mode is silence is not a guard** — check that checkers still emit
  positive findings on a known-bad input. Detect recurrence: `grep -c $'\x00' wiki/reference/*.md`.

### NIC PCI-ID misclassification (2026-08-01, `7d2b4271` + `ed3105d3`, `541411a0`)

- **Symptom**: coarse e1000 ID ranges (0x10A4..0x10FF, 0x1500..0x15FF) dispatched **ixgbe/igb/igc/i40e
  silicon into the full e1000 register bring-up** — on real hardware that wrote reset/ring/IVAR values into
  the wrong registers. Also: rtw89 0xB852 mislabelled RTL8852AE (it is 8852BE); bcm43xx requested the
  non-existent firmware `brcmfmacaa52-pcie.bin` (violated brcmfmac's decimal-vs-hex chip-name rule — should
  be `brcmfmac43602-pcie.bin`); `rtl8723be-wifi` tags never matched the wireless heuristic, so that Wi-Fi
  part would have been treated as wired.
- **Root cause**: the same device-ID → family knowledge lived in **parallel whitelists that had drifted**
  (net.cpp tagger vs per-driver `*Matches` predicates), plus range-based gating instead of explicit IDs.
- **Evidence**: `7d2b4271` (HEAD~4 on this branch) — single-source constexpr header
  `kernel/drivers/net/nic_ids.h`, host-tested with an exhaustive 0x0000..0xFFFF sweep; unknown Intel IDs
  now classify probe-only (safe direction).
- **Follow-up incidents**: `ed3105d3` — the wireless liveness watch tasks leaked **one poller per NIC per
  NetShutdown→NetInit restart cycle**, old watchers mutating records they no longer owned; fixed by a module
  generation counter (one bounded race remains, marked as a GAP in net.cpp). `541411a0` — the boot-smoke
  expectation still pinned the old range-derived `e1000e-82574` tag; test expectations must move with the
  classification they check.
- **Status/Lesson**: fixed on this branch. Device-ID knowledge is single-sourced in `nic_ids.h`; do not add
  a new ID range or family tag anywhere else. See `duetos-net-driver-campaign` for the campaign itself.

### load_plan build-state inversion (2026-08-01 → 2026-08-13, `7d2b4271` vs `f0ebd7e2`/`e8eb86e8`)

- **Symptom**: on 2026-08-01, `7d2b4271`'s body truthfully recorded "Full kernel link is currently blocked
  upstream: origin/main's service_manifest.h includes loader/load_plan.h which is not yet in tree". By
  2026-08-02/03, `f0ebd7e2` and `e8eb86e8` (**origin/main**) landed the load-plan validator on main. As of
  **2026-08-13 the broken side is inverted**: on THIS branch `kernel/core/service_manifest.h:32` includes
  `loader/load_plan.h`, and that file does not exist on this branch — HEAD is the unbuildable side now.
- **Root cause**: a build-state claim about "the other branch" was treated as stable fact while both
  branches kept moving.
- **Evidence**: verified 2026-08-13 — `grep -n load_plan kernel/core/service_manifest.h` (line 32),
  `ls kernel/loader/load_plan.h` (absent on HEAD), `git ls-tree origin/main kernel/loader/` (present).
- **Status/Lesson**: **open on this branch** — rebase onto origin/main before diagnosing any "missing
  header" kernel-link failure here. Durable rule: **date-stamp every build-state claim** about another
  branch; re-verify it before acting on it.

### The 2026-08-03 SMP-fallout wave (all **origin/main** — not reachable from this worktree's HEAD)

Once AP bring-up actually started working (`8220ac0c` "push a return address before entering the AP's C++
entry", origin/main), a whole stratum of uniprocessor-era assumptions failed at once. Each is its own
incident; together they are the strongest recurring class in the repo.

- **AP GSBASE fake regression (`285278ca`)** — Symptom: `CurrentCpu()`'s "must stay at zero" non-BSP
  fallback counter fired **69x on every clean boot**, so every boot reported a regression. Root cause:
  `mov %ax, %gs` in `LoadGdtForCurrent` zeroes IA32_GS_BASE as a hardware side effect; the CPUHP split put
  a per-state klog call between GDT load and GSBASE restore, and klog tags lines via `CurrentCpu()`. Fix:
  the GDT step restores GSBASE itself, immediately — **"makes the 'must stay at zero' invariant true again
  rather than weakening the claim to match the bug."** Lesson: when an invariant check fires constantly,
  restoring the invariant beats relaxing the check; the new contract test also pins the three preconditions
  its rationale depends on.
- **Watchdog blocked by the filesystem (`83717fd0` + `cb53f104`)** — Symptom: a task hung **459 s** in a
  FAT32 write produced ZERO hung-task warnings; CI saw only `forbidden signature: qemu_timeout` on rotating
  smoke profiles. Root cause: `HungTaskTick()` ran inside the heartbeat beat, and the heartbeat's own
  `PersistBootSlotState` blocked on the same FAT32 volume lock — "a watchdog the watched subsystem can block
  is not a watchdog." Fix: bounded best-effort lock acquire for diagnostic writers only. The follow-up
  `cb53f104` then had to repair the build `83717fd0` broke — "Two defects, both mine, both from trusting
  -fsyntax-only where a link was required" (see host-shim link-drift class above). The underlying FAT32
  wedge remained **open** at that commit (`docs/handoff-boot-regression-2026-08-03.md` on origin/main).
- **Fix-journal ABBA deadlock (`05121fe8`, scanner `3e730aa1`)** — Symptom: the long-running "flaky
  qemu_timeout" failing a rotating set of 3-4 smoke profiles per run. Root cause: `FixJournalInit` held
  `g_lock` across a `KLOG_INFO_V` — **violating the lock-ordering rule stated in a comment at the top of
  that same file** — establishing g_lock → serial-lock, the inverse of klog writers reaching
  `FixJournalRecord`. Latent for months; became a live ABBA deadlock the moment SMP worked. The rotation was
  just which CPU lost the race. `3e730aa1` generalised it: a tree-wide scanner found **67 lock scopes
  logging while holding a lock** (report, not gate — the triage list for the class). Lesson: "a
  lock-ordering rule that lives only in a comment is a rule that gets broken" — the fix shipped a contract
  test verified to FAIL against the pre-fix code.
- **Global-counter test oracles invalidated by SMP (`8cfcc470`, `dd45d709`, `3d6e870a`)** —
  `8cfcc470`: `ElfLoaderUnwindSelfTest` sampled global `FreeFramesCount()` before/after its window — sound
  only on a quiescent uniprocessor; it panicked healthy 4-vCPU boots with "frame leak detected". Fix:
  per-address-space page-count assert (peer-CPU-proof) + gate the global check on `SmpCpusOnline() > 1`;
  deliberately NOT a tolerance widening, which would blind both modes.
  `dd45d709`: the OOM-injection counter (`FrameAllocatorSetFailAfter`) was global and consumed on the shared
  allocation path — a peer CPU's `AllocateKernelStack` absorbed the injected failure and panicked the box:
  **"a test harness that can inject faults into unrelated subsystems is worse than no harness."** Fix: scope
  injection to the arming task. Its body calls itself the **third instance that session** of a
  uniprocessor-era test mechanism unsafe under live SMP.
  `3d6e870a`: `pe-threads` smoke reported a PASS line as MISSING — the app emitted label and verdict as two
  writes; the kernel's write syscall is line-atomic **per call**, not across calls, and a peer CPU's klog
  landed between them. Fix: build each verdict line and emit it in ONE write.
- **ACPI false-complete fix comment (`6d36c183`)** — a prior helper's comment claimed "every UBSAN
  type-mismatch report from acpi.cpp resolves once the five XSDT loops all go through this helper" —
  **"which was not correct"**: it fixed the u64 XSDT loads only; 34 remaining reports were misaligned
  `const u32` RSDT reads (firmware may place tables off 4-byte alignment; QEMU routinely does). Works by
  luck on x86, would fault on the planned ARM64 tier. Lesson: a completeness claim in a comment needs the
  signal re-run to zero, not "the loops I saw are converted".
- **Stress oracle scoped wrong (`0ef1ffed`)** — `StressDriverArm()` ran ~270 lines of boot before
  `SmpStartAps()`; 64 stress workers starved the BSP so APs never started. Consequence: **"every stress=
  run recorded to date measured single-CPU behaviour regardless of -smp."** All prior stress conclusions
  are single-CPU data. Fix: arm after `SmpStartAps()` + `TopologyAssignClusters()`.

---

## Recurring failure classes (distilled from the entries above)

| Class | Instances here | Rule |
|-------|----------------|------|
| Single-boot verification of intermittent code paths | lockdep attempt 1 (3/6 crash, single boot clean); AP sentinel | Anything touching context switch / AP bring-up gets a >=6-boot sweep, never one boot |
| Global counters as test oracles under SMP | frame-count oracle, OOM-injection counter, stress driver, interleaved verdict writes | An oracle is sound only if no peer CPU can perturb it; scope to task/AS, or gate on `SmpCpusOnline()` |
| Syntax-check where a link was required | `cb53f104` (twice in one commit), 8-commit fuzz stub cluster | Kernel TUs linked into tests/fuzz and tests/host need a LINK; the host-shim stub edit is part of the change |
| Partial optimisation without full accounting | compositor dirty gate, cursor-in-compose v2-v4 | A skip-work optimisation is unsound until every writer feeds the signal; revert, don't patch |
| Comment/claim drift | ACPI "all resolved" comment, AP-sentinel ordering comment, load_plan build-state claim, e1000e smoke expectation | Claims are verified against the signal at the moment of use; date-stamp cross-branch claims |
| Weakening the check instead of restoring the invariant | GSBASE counter (right answer: make it zero again), frame-oracle tolerance (temptation explicitly rejected) | When an invariant check fires, first ask what broke the invariant |
| Rules living only in comments | fix-journal lock-order comment, lock-across-log class (67 scopes) | Turn the rule into a contract test/scanner verified to FAIL on the pre-fix code |
| Checked-in binaries / duplicated knowledge | 129 smoke .exes, parallel NIC whitelists, two doc tiers | One generated/single source; a second copy WILL drift |
| Watchdogs blockable by their subject | `HungTaskTick` inside the FS-touching heartbeat; grep-binary silencing the wiki check | A detector must not share a blocking dependency with what it detects; verify checkers on known-bad input |
| Wrong-fix-for-real-symptom conflation | `bca65d48` (disk poisoning vs PT corruption), F-003-as-symptom-of-F-002 | Reproduce the exact fault with the fix applied; trace one symptom of a cluster to root before touching siblings |

## Provenance and maintenance

All facts verified 2026-08-13 against worktree `claude/fable-driver-wave-20260801` @ `8a55872c` and
`origin/main` @ `24a4db24` (2026-08-05). Volatile facts: the load_plan inversion status, the origin/main
tip, and the claim/completion counts will drift — re-verify before quoting.

Re-verification one-liners (run from the repo root):

```bash
# Any cited SHA: subject, then full body
git log -1 --format="%h %s" <sha>
git show -s <sha>

# Is a SHA on this branch or only origin/main?
git merge-base --is-ancestor <sha> HEAD && echo ON-HEAD || echo origin/main-only

# Era boundaries and totals
git rev-list --count HEAD
git log --format="%cd" --date=short | sort -u

# Claim vs completion arithmetic (fleet era)
git log --format="%s" | grep -cE "^feat\(.*\): complete subsystem \[session"
git log --format="%s" | grep -cE "^chore: claim subsystem"

# load_plan inversion status (open as of 2026-08-13)
grep -n "load_plan" kernel/core/service_manifest.h; ls kernel/loader/load_plan.h

# NUL-corruption recurrence check
grep -c $'\x00' wiki/reference/Design-Decisions.md

# Standing inventories referenced above
git grep -nE "// (STUB|GAP):" | wc -l
```

When a new incident closes (a revert, a dead end, a rejected approach, a corrected regression signature),
append a chronological entry here in the same Symptom → Root cause → Evidence → Status/Lesson shape, and
update the symptom index in `duetos-debugging-playbook` if it is a live-triage pattern. Never merge the two:
this file is the chronology; the playbook is the symptom index.
