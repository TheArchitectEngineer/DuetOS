---
name: duetos-boot-smoke-and-qemu
description: >-
  Run and observe a live DuetOS boot under QEMU and read what it says.
  TRIGGER when you need to "boot the kernel", "run in QEMU", "run a boot smoke /
  smoke profile", "read/triage the boot serial log", "check the
  completion sentinel", "screenshot the desktop", "send keys to the guest", or
  "check if a boot failure is intermittent". DO NOT TRIGGER for building the
  kernel/ISO or installing QEMU packages (use duetos-build-and-env first — no ISO,
  no boot), for hosted unit tests / ctest beyond the boot-smoke wiring (use
  duetos-testing-and-validation), or for root-causing a specific crash symptom
  (use duetos-debugging-playbook).
---

# DuetOS boot smoke and QEMU

Everything here observes a **live boot**: launch QEMU, capture the COM1 serial
transcript, and judge it by the sentinel contract. The serial log is the ground
truth for every claim about runtime behaviour — "it compiled" proves nothing.

**Shell context:** QEMU, GRUB, and every script below run **inside WSL** (Ubuntu).
The build tree they consume is the **WSL-native scratch copy** from
`duetos-build-and-env` (e.g. `/root/scratch/duetos-fable`) — the `/mnt/c/...`
9p view of the Windows checkout cannot host a kernel build, and this
checkout's tracked `build/x86_64-debug` is a stale `/mnt/c`-configured cache
containing **no ELF and no ISO**. Before trusting any existing build dir, run
`grep CMAKE_HOME_DIRECTORY <bin-dir>/CMakeCache.txt` — a `/mnt/c/` value means
rebuild fresh in scratch. Git Bash is fine only for scripts that themselves
shell into `wsl.exe`.

## The three exit-code traps (read before running anything)

1. **`wsl.exe -- bash -lc '...'` FABRICATES exit codes** — use
   `wsl.exe -e bash -lc '...'` or a sentinel branch
   (`cmd && echo BOOT_OK || echo BOOT_FAIL`). Full write-up:
   `duetos-build-and-env` "Trap zero".
2. **SKIP (exit 2) is NOT PASS.** `ctest-boot-smoke.sh` exits 2 for a missing
   prerequisite — including *wrong usage*: called with no `<cmake-binary-dir>`
   argument it prints a usage line and exits 2, which scrolls past looking like
   a pass. Always echo and check the numeric rc.
3. **Absence of a FAIL line is NOT proof of pass.** A self-test that never ran
   emits nothing. Judge a boot by the *presence* of completion sentinels and
   expected PASS lines, never by the absence of failures alone.

Two more WSL environment traps:

- **WSL wipes `/tmp` when the instance goes idle.** Logs written to `/tmp`
  between invocations can vanish. `run-exe.sh` honours `DUETOS_LOGDIR` for a
  persistent path; for hand-run boots redirect the log somewhere under the repo
  or `~/`.
- **Never serialize on `pgrep -f qemu`** (it matches waiters and deadlocks),
  and `pgrep -x qemu-system-x86_64` **never matches** — `comm` truncates
  process names at 15 chars. Match on `pgrep -f 'qemu-system-x86_64 -machine'`
  or track the PID you spawned.

## Prerequisites

The dev host ships **without** QEMU/OVMF/GRUB/xorriso/mtools — live-test tooling
is install-on-demand. Until installed, *build-clean is the only signal* and you
must report runtime claims as unverified. The full package list, the
"legitimately requires a live boot" test, and the canonical invocation live in
`wiki/tooling/Dev-Host-Setup.md`. Building the ISO itself is
**duetos-build-and-env**'s job; this skill assumes `build/<preset>/duetos.iso`
exists.

## Anatomy of one smoke run (walkthrough)

All steps in WSL, from the **scratch-tree** root (`/root/scratch/duetos-fable`
or wherever `sync-to-wsl-scratch.sh` put your copy — `duetos-build-and-env`
owns that setup).

```bash
# 0. Staleness check — never smoke a /mnt/c-configured cache:
grep CMAKE_HOME_DIRECTORY build/x86_64-debug/CMakeCache.txt  # must NOT say /mnt/c/...

# 1. Have an ISO (see duetos-build-and-env for configure/build details).
ls build/x86_64-debug/duetos.iso   # must exist; ISO target is disabled
                                   # if grub-mkrescue/xorriso were missing
                                   # at cmake configure time

# 2. Boot one smoke profile, capturing serial to build/<preset>/smoke-<profile>.log
tools/test/profile-boot-smoke.sh bringup build/x86_64-debug
echo "rc=$?"

# 3. Triage the captured log with the canonical analyzer
tools/test/boot-log-analyze.sh build/x86_64-debug/smoke-bringup.log
echo "rc=$?"
```

Interpreting step 2 (`profile-boot-smoke.sh` exit contract):

| rc | Meaning |
|----|---------|
| 0  | Full pass — `[boot-report] result=pass` (or full legacy signature list), sentinel + scenario lines present, nothing forbidden |
| 1  | Real regression — missing expected signature, or a forbidden line (PANIC / DUETOS CRASH / triple fault / `] FAIL`). **Crashes are never retried** — a crash on a clean boot path is a real bug even if the next boot is green |
| 2  | Environment skip — QEMU not installed, `run.sh` missing, or unknown profile |

On failure it dumps the last 200 serial lines, crash-dump header fields, and a
PRESENT/MISSING map of every expected signature — read that before re-running.

The script also decodes QEMU's exit status against the kernel's hierarchical
`isa-debug-exit` scheme (`kernel/diag/boot_observe.h`): QEMU exits `(b<<1)|1`
for a guest OUT byte `b` to port 0xf4; `arch::TestExit` writes 0x10 → host
exit **33 (0x21) = "smoke sentinel reached"**. Classes: 0x10 pass, 0x20 hung,
0x40 phase-init-fail, 0x70 panic; low nibble = boot phase ordinal. A 143
(SIGTERM on timeout) is *unstructured* — the serial log, not the rc, is
authoritative (the kernel always prints `[boot] phase=... STUCK/FAIL` or a
panic before it TestExits).

## QEMU invocation anatomy — tools/qemu/run.sh

`tools/qemu/run.sh` is the single launcher. Canonical headless smoke
(from `wiki/tooling/Dev-Host-Setup.md`):

```bash
DUETOS_TIMEOUT=20 tools/qemu/run.sh          # WSL; full default boot, 20 s cap
```

Load-bearing behaviour (all verified in the script):

- **Boot path is ISO + GRUB + Multiboot2.** QEMU's `-kernel` flag speaks
  Multiboot 1; the kernel header is Multiboot 2, so the `-kernel` fallback
  (taken only when the ISO is missing) **will not boot** — the script warns.
  If you see that warning, go build the ISO, don't debug the "hang".
- **Firmware defaults to UEFI/OVMF** (`/usr/share/OVMF/OVMF_CODE_4M.fd` +
  `..._VARS_4M.fd`, overridable via `DUETOS_OVMF_CODE`/`DUETOS_OVMF_VARS`;
  per-run writable NVRAM copy at `build/<preset>/ovmf-vars.fd`).
  `DUETOS_LEGACY=1` boots SeaBIOS instead (`DUETOS_UEFI=0` is the honoured
  back-compat spelling).
- **Headless by default** (`-display none`); `DUETOS_DISPLAY=gtk` for a window.
- **`DUETOS_TIMEOUT=<secs>`** wraps QEMU in `timeout --foreground
  --preserve-status`; a timeout kill surfaces as rc 124/143.
- A positional `*.iso` argument replaces the boot image; **all remaining argv
  is forwarded raw to QEMU** — e.g. `tools/qemu/run.sh -s -S` starts QEMU
  waiting for GDB on :1234.
- `DUETOS_PRESET` (default `x86_64-debug`) selects
  `build/<preset>/duetos.iso` + `build/<preset>/kernel/duetos-kernel.elf`.
- **`DUETOS_SMOKE_PROFILE=<p>`** rebuilds a sidecar ISO
  (`build/<preset>/duetos-smoke-<p>.iso`) via `grub-mkrescue` with
  `smoke=<p>` baked into the GRUB cmdline. **`DUETOS_EXTRA_CMDLINE="..."`**
  likewise builds `duetos-extra-<tag>.iso` appending arbitrary cmdline text
  (combined with a profile when both are set). Both need `grub-mkrescue`.
- Accel auto-picks `kvm:tcg` when `/dev/kvm` is read-writable, else `tcg`;
  `DUETOS_ACCEL` overrides. The chosen value is logged as
  `[run.sh] qemu accel=...` — check it when a run is mysteriously slow.
- Emulated hardware every boot: q35, `-cpu max` (override `DUETOS_CPU`),
  `-smp 4,sockets=1,cores=2,threads=2` (override `DUETOS_SMP`), 512M
  (`DUETOS_RAM`), NVMe + AHCI/SATA scratch GPT images (regenerated per run),
  xHCI + usb-kbd/usb-mouse, e1000e NIC (mac 52:54:00:12:34:56) on SLIRP
  user netdev, virtio-rng/balloon/blk (`disable-legacy=on` — the transport
  is modern-only), intel-hda + hda-output, isa-debug-exit at port 0xf4,
  minidump sink on `-debugcon` at `build/<preset>/duetos.dmp`.
- Other env knobs (full list = read the script header):
  `DUETOS_SERIAL_FILE` (COM1 → unbuffered file; survives a host-side QEMU
  abort that would eat the buffered stdio tail), `DUETOS_ALLOW_REBOOT=1`
  (drops `-no-reboot`; required for S3 resume, which goes through a platform
  reset), `DUETOS_BOOT_STALL=<phase>` (wedge a phase to prove the hang
  watchdog), `DUETOS_IOMMU_DEVICE=1` (VT-d), `DUETOS_TPM=1` (swtpm),
  `DUETOS_GDB_TRANSPORT`/`DUETOS_GDB_PORT` (COM2 GDB stub — default TCP :1234,
  auto-falls-back to an ephemeral port when 1234 is busy and unpinned),
  `DUETOS_QMP`/`DUETOS_QMP_SOCK`, `DUETOS_STAGE_FILES`, `DUETOS_SMOKE_ISO`,
  `DUETOS_DRSH_HOST_PORT`, `DUETOS_NO_SXS_FIXTURE`.
- QMP socket: `build/<preset>/qmp.sock` by default, but when the build dir is
  on a Windows-mounted filesystem (drvfs/9p — the common WSL case) unix
  sockets can't bind there, so run.sh falls back to
  `$XDG_RUNTIME_DIR`-or-`/tmp/duetos-qmp-<tag>.sock` and prints the
  `DUETOS_QMP_SOCK=...` line to use for clients. Watch for that stderr line.

## Smoke profiles

Profile names mirror `kernel/test/smoke_profile.h` (kernel is source of
truth); CI runs them as a parallel matrix (`.github/workflows/build.yml`,
`qemu smoke (<profile>)` jobs):

| Profile | Exercises |
|---------|-----------|
| `bringup` | Boot through bringup-complete + driver init + self-tests; boot health IS the test |
| `ring3` | Native ring-3 smoke trio ("Hello from ring 3!" + SYS_WRITE cap-deny) |
| `pe-hello` | Freestanding PE + SEH/C++EH/TLS/delay-load PASS batteries |
| `pe-winapi` | Comprehensive Win32 PE (heap/strings/TLS/interlocked/thunk-alias/via-dll battery) |
| `pe-threads` | thread_stress + thread2/thread3 + syscall_stress, exit-code contracts |
| `pe-winkill` | Real-world MSVC PE ("Windows Kill " via std::cout) |
| `linux` | The Linux ABI smokes |
| `browser` | WinInet (`browser_pe.exe`) + WinSock (`mini_browser.exe`) PEs — hand-run only via `DUETOS_SMOKE_PROFILE=browser`; NOT in the CI matrix and NOT in `profile-boot-smoke.sh`'s case list (it exits 2 unknown-profile) |

Each profile boots, emits `[smoke] profile=<name> complete`, and TestExits.
Run one by hand with `DUETOS_SMOKE_PROFILE=<p> tools/qemu/run.sh`, or gated
via `tools/test/profile-boot-smoke.sh <p> <cmake-binary-dir>`.

## The harness family and ctest wiring

| Script (all WSL, repo root) | Job | Exit contract |
|---|---|---|
| `tools/test/profile-boot-smoke.sh <profile> <bin-dir>` | One profile, gated | 0 pass / 1 regression / 2 skip |
| `tools/test/ctest-boot-smoke.sh <bin-dir>` | Full-boot signature battery (CI-mirrored list) | 0 / 1 / 2 skip — **arg REQUIRED, usage error also exits 2** |
| `tools/test/diff-boot-smoke.sh <profile> <bin-dir>` | 4-row (engine,accel,cpu,firmware) matrix incl. Bochs; diffs sentinel streams | 0 pass / 1 row failed / **2 rows passed but diverged = real bug** / 3 skip |
| `tools/test/uefi-smoke.sh` | Native BOOTX64.EFI Phase-A path | 0 pass / 1 fail / 2 skip |

ctest registration (root `CMakeLists.txt`): `duetos-boot-smoke`
(TIMEOUT 720, SKIP_RETURN_CODE 2), `duetos-uefi-smoke` (TIMEOUT 60,
SKIP_RETURN_CODE 2), `duetos-diff-boot-smoke` (runs `bringup`, TIMEOUT 2400,
SKIP_RETURN_CODE **3** — diff's exit 2 divergence deliberately falls through
to FAIL).

Timeout reality: `ctest-boot-smoke.sh`'s inner budget is a flat
`DUETOS_TIMEOUT` default of **600 s** — it no longer branches on `/dev/kvm`,
because under WSL2 nested virt `/dev/kvm` exists but runs at ~TCG speed and
the old 150 s "fast" budget produced false timeouts on pristine main. A
TCG timeout with a healthy banner and no forbidden line is reported as
SKIP (environmental), not FAIL. `ctest-boot-smoke.sh` also SKIPs on
release/LTO presets (expected signatures are Info-level; release runs at
Warn) and pins `DUETOS_PRESET` from the bin-dir basename.

## Reading a boot log

Canonical triage — run this first, always:

```bash
tools/test/boot-log-analyze.sh [logfile]   # WSL; no arg = newest of
                                           # /tmp/duetos-*.log, build/*/stress-*.log, build/*/*.log
```

Exit 0 iff a completion sentinel was reached AND no non-deliberate failure;
1 otherwise (so it doubles as a scripted gate); 2 = no log found. It is
launcher-agnostic — works on QEMU stdout, a VMware/VirtualBox serial-to-file,
or a real-hardware UART capture. Sections it reports: accel/SMP banner,
COMPLETION, HEALTH (hard-fault + `[E]`-line scan minus known-deliberate
self-test scaffolding), PHASE TIMINGS, SELF-TESTS (OK/SKIP/FAIL counts),
PE-COMPAT summary, tactility/Pass-B/C/D umbrellas, LOCKDEP pairs, KPATH
coverage, STRESS summary, and a final `verdict: OK|ATTENTION` line.

Sentinel vocabulary:

- **Completion sentinels** (any one proves the boot finished):
  `boot : metrics bringup-complete`, `[smoke] profile=<p> complete`,
  `[stress] done` (line-start), `phase=smp complete`.
- **`[smp] online=N/M`** is the *authoritative* CPU count — the per-AP
  `smp.ap_online` prints interleave under concurrent serial and are not
  reliable.
- **Self-test contract**: `[<name>-selftest] PASS` / `FAIL check=<n>` on
  COM1. Self-tests pass **silently by default** unless they emit an explicit
  PASS line — so absence of FAIL may mean "never hooked". Verify the
  BOOT_SELFTEST hook exists before trusting silence.
- **Forbidden lines** (any occurrence = regression; from
  `profile-boot-smoke.sh`): `PANIC`, `DUETOS CRASH`, `triple fault`,
  `[health] ESCALATE:`, the RETIRED/UNRESOLVED import lines, and the
  catch-all `] FAIL` (any subsystem self-test failure).
- **Exact-line gates exist** — e.g. `ctest-boot-smoke.sh` expects verbatim
  `[net-probe] vid=0x0000000000008086 did=0x00000000000010d3 family=e1000e`.
  Changing a sentinel's wording in kernel code breaks the gate; keep them in
  lockstep.
- Raw fallback grep when you can't run the analyzer:
  `grep -nE "\[E\] |PANIC|TRIPLE|FAIL|out of range|task-kill|kernel oops" <log>`

"Where did it stop?" is a different question from "is it healthy?":
`tools/test/boot-progress-localizer.sh [logfile]` reports "boot reached X,
next expected Y" against the ordered sentinel list (exit 0 iff the final
sentinel was reached).

## Intermittents — the 6-boot rule

One run is not enough for a crash that didn't reproduce. Sweep:

```bash
tools/test/boot-determinism-sweep.sh [runs] [per_boot_timeout_s]   # WSL; defaults 8 / 120
# Per-run logs: /tmp/sweep-<i>.log   Summary TSV: /tmp/sweep-summary.tsv
```

A deterministic kernel must produce byte-stable counts across runs:
self-test OK/FAIL/SKIP, AP-online count, panic count, and *distinct* lockdep
(held,id) pairs. Raw lockdep inversion COUNT is legitimately timing-variable
(reported, not failed); a NEW distinct pair is a real finding.

Cautionary precedent (commit `a0d56701`): a lockdep change crashed **3 of 6**
boots that a single-boot "verification" had blessed. Sweep before and after,
and confirm any line you treat as the regression oracle is absent from
known-clean boots. Full incident narrative: `duetos-failure-archaeology`.

## QMP interaction and screenshots

QEMU exposes a QMP unix socket every run (disable with `DUETOS_QMP=0`) —
orthogonal to COM1/COM2, so it never disturbs the serial log or GDB. Helpers
(WSL; all honour `DUETOS_PRESET` / `DUETOS_QMP_SOCK`):

```bash
tools/test/qmp-cmd.sh '{"execute":"query-status"}'   # generic client; owns the
                                                      # greeting→qmp_capabilities handshake
tools/test/qmp-screendump.sh [OUT.ppm]   # framebuffer → P6 PPM (default /tmp/duetos-live.ppm)
tools/test/qmp-sendkey.sh ret            # one key / combo (ctrl-alt-t, shift-f10)
tools/test/qmp-sendstring.sh "duet"      # type a string char-by-char
tools/test/qmp-click.sh 512 384 left     # relative-mode mouse click at (x,y)
```

No QMP (VirtualBox / bare metal)? The kernel shell's `fbdump` command emits a
base64 PPM over COM1; decode with
`tools/test/serial-fbdump.sh SERIAL_LOG OUT.PPM`.

## Running an arbitrary Windows .exe

`tools/test/run-exe.sh <host-exe> [SFN]` stages the .exe onto the FAT32 GPT
image (`DUETOS_STAGE_FILES`), bakes `peexec=<SFN>` into the cmdline
(`DUETOS_EXTRA_CMDLINE`), boots headless, and greps a load/run report. Set
`DUETOS_LOGDIR` to a persistent path (WSL `/tmp` is volatile).
`tools/test/pe-corpus-run.sh` loops it over a corpus and grades each run
(TIMEOUT / NOTFOUND / LOADFAIL / NOSPAWN / FASTFAIL / FAULT / EXIT-rc /
CLEAN). Note TIMEOUT is graded separately: under TCG it usually means "ran
out of wall clock", not a staging bug.

## Attaching GDB (handoff)

`CMakePresets.json` sets `DUETOS_GDB_SERVER=ON`; COM2 carries the kernel's GDB
stub (TCP :1234 by default). Attach via `tools/debug/duetos-gdb-attach.sh`;
`KBP_PROBE` fires halt at `duetos::debug::ProbeFire`. Probe/breakpoint
conventions live in **duetos-kernel-conventions**; symptom-driven triage
(which localization doc, which probe to arm) lives in
**duetos-debugging-playbook**. For QEMU's own hypervisor-side debugger
instead, forward `-s -S` through run.sh's raw argv.

## Provenance and maintenance

Authored 2026-08-13 against branch `claude/fable-driver-wave-20260801`
(HEAD 8a55872c). Volatile facts date-stamped above: UEFI-default flip 2026-04;
flat 600 s inner timeout (replaced the /dev/kvm branch); `a0d56701` 3/6-boot
lesson 2026-05-17 (narrative in `duetos-failure-archaeology`).

Re-verify one-liners (WSL, repo root):

```bash
# Launcher env surface + defaults (UEFI, display, timeout, profiles):
grep -nE 'DUETOS_[A-Z_]+' tools/qemu/run.sh | head -60
# Analyzer completion sentinels + exit contract:
sed -n '1,80p' tools/test/boot-log-analyze.sh
# Profile list (kernel is source of truth) + CI matrix:
grep -n 'profile' tools/test/profile-boot-smoke.sh | head; sed -n '360,372p' .github/workflows/build.yml
# ctest wiring (names, TIMEOUT, SKIP_RETURN_CODE):
grep -n -A4 'add_test' CMakeLists.txt
# Exact-line gates (e.g. net-probe):
grep -n 'net-probe' tools/test/ctest-boot-smoke.sh
# Canonical smoke invocation:
grep -n 'DUETOS_TIMEOUT=20' wiki/tooling/Dev-Host-Setup.md
```
