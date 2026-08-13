---
name: duetos-testing-and-validation
description: >-
  DuetOS evidence and test-tier runbook: what counts as proof, which tier to run, how to run and
  extend each one. TRIGGER when the user says "run the tests", "add a test", "host test", "ctest",
  "fuzz it", "is this verified", "what counts as evidence", "prove the fix", or before claiming any
  change is done. DO NOT TRIGGER for QEMU/boot-smoke launch mechanics or smoke profiles — that is
  duetos-boot-smoke-and-qemu; toolchain/env setup is duetos-build-and-env; the in-kernel self-test
  authoring idiom is duetos-kernel-conventions.
---

# DuetOS testing and validation

How to produce evidence that a change works, at the cheapest tier that actually proves it.
Core doctrine (from `CLAUDE.md`): **"it compiles" is never evidence a program works**, SKIP is
not PASS, and the absence of a FAIL line is not proof of pass.

**Not this skill:** launching QEMU, smoke profiles, serial-log capture → `duetos-boot-smoke-and-qemu`.
Installing toolchains / WSL vs MSVC mechanics → `duetos-build-and-env`. Writing the body of an
in-kernel self-test (`DUETOS_BOOT_SELFTEST`, sentinel format, probes) → `duetos-kernel-conventions`.
CI wiring → `duetos-ci-and-release`. Root-causing a failure you found → `duetos-debugging-playbook`.

## The evidence-tier inventory

| # | Tier | Where | Runner | Speed | Host |
|---|------|-------|--------|-------|------|
| 1 | C++ host unit tests (68 `test_*.cpp`) | `tests/host/` | CTest (standalone CMake project) | ms | Windows MSVC **or** WSL clang-18 (pin it — see tier 1) |
| 2 | Python static harnesses + audits | `tools/test/*.py`, `tools/build/test_*.py` | 10 wired into tier-1 ctest; rest standalone | s | any Python 3 |
| 3 | Rust host tests (22 crates) | `tools/dev/cargo-host-test.sh` | `rustc --test` per crate (bypasses cargo) | s | WSL/Linux |
| 4 | libFuzzer harnesses (37) | `tests/fuzz/` | `tools/test/fuzz-all.sh` / `make -C tests/fuzz` | min | WSL/Linux clang |
| 5 | On-target boot self-tests | `DUETOS_BOOT_SELFTEST` in `kernel/core/boot_bringup.cpp` | boot under QEMU, grep sentinels | min | WSL + qemu |
| 6 | On-target ctest smokes | root build: `duetos-boot-smoke`, `duetos-uefi-smoke`, `duetos-diff-boot-smoke` | CTest in root build dir | min–30 min | WSL + qemu |

Jargon: a **host test** runs as an ordinary process on the dev machine against kernel TUs compiled
natively; an **on-target** test runs inside a booted DuetOS kernel. A **harness** is the driver
program that feeds a parser/decision function; a **sentinel** is a greppable serial line like
`[<name>-selftest] PASS` that a smoke script asserts on.

## Tier selection: change type → minimum evidence

| You changed... | Minimum evidence | Also run if cheap |
|---|---|---|
| A freestanding header / pure decision logic (parsers, math, policy) | Tier 1 host test covering the new branch | Tier 4 if it parses untrusted bytes |
| A syscall number, DLL export, `.def` file, handler table | Tier 2 (the wired static harnesses catch drift) | Tier 1 |
| A Rust crate under `kernel/**/*_rust/` | Tier 3 for that crate | Tier 4 if it has a fuzz harness |
| Any untrusted-input parser (PE, FS, net, firmware, image) | Tier 4 short run (`FUZZ_SECONDS=60`) + tier 1 vectors | — |
| Runtime behaviour: scheduler, MM, IRQ, boot order, new boot-log line | Tier 5/6 live boot (see `duetos-boot-smoke-and-qemu`) | `boot-determinism-sweep.sh` if timing-sensitive |
| Anything SMP / timing / ASLR-sensitive, or an intermittent fix | **≥6-boot sweep**, not one boot (`a0d56701`) | — |
| Pure refactor with no behavioural delta, docs, unwired code | Build clean + tiers 1–2 that touch the files | No live boot needed (`wiki/tooling/Dev-Host-Setup.md`) |

The "legitimately requires a live boot" test (from `wiki/tooling/Dev-Host-Setup.md`): the commit
introduces/changes observable runtime behaviour, or claims end-to-end correctness a compile cannot
prove, or depends on a never-verified runtime claim. Refactors, docs-only, and not-yet-wired code
do **not** require one.

## Tier 1 — C++ host unit tests (`tests/host/`)

Standalone CMake project — **not** part of the root build. From the repo root:

```bash
# WSL/Linux: pin clang-18 — an unpinned configure resolves cc/c++ to GCC,
# whose green run is NON-authoritative (see below). On Windows/MSVC the
# vcvars invocation in duetos-build-and-env applies instead.
cmake -S tests/host -B build/host-tests -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
# One test:
ctest --test-dir build/host-tests -R nic_ids --output-on-failure
```

- Works on Windows with MSVC (fast loop, no WSL — vcvars mechanics in `duetos-build-and-env`)
  and on WSL with **clang-18 pinned**. Warnings are errors both ways (`/W4 /WX`,
  `-Wall -Wextra -Wpedantic -Werror`). A GCC-built run is non-authoritative: GCC ignores unused
  `static inline` functions that clang's `-Wunused-function` + `-Werror` (the CI toolchain)
  makes fatal — pin rationale in `duetos-build-and-env`.
- ASan+UBSan are ON by default and **auto-skipped on MSVC**; opt out with
  `-DDUETOS_HOST_TESTS_SANITIZERS=OFF`.
- No gtest. The framework is one header, `tests/host/host_test_helper.h`:
  `EXPECT_TRUE/FALSE/EQ/NE/STREQ` (record failure, keep running) and `ASSERT_*` (abort on first
  failure). `main` returns the failure count, so CTest fails on any miss.
- Include path is `../../kernel`, so a test writes `#include "util/cmdline.h"` exactly like kernel
  code. Only arch-neutral, dependency-light headers can be included; anything needing kernel-only
  globals fails to link unless you add the TU.

**Add a host test:**

1. Drop `tests/host/test_<thing>.cpp` with `int main()` + `EXPECT_*` + `return finish_main("<thing>");`,
   citing the kernel TU/header under test in the top-of-file comment.
2. Append `add_host_test(<thing>)` to `tests/host/CMakeLists.txt`. If the code under test is a
   `.cpp` (not header-only), add `target_sources(test_<thing> PRIVATE ".../kernel/<path>.cpp")` —
   see `test_ec` / `test_x509_verify` for multi-TU examples.
3. If the header legitimately trips the harness's extra `-Wconversion`/`-Wsign-conversion`, relax
   per-target (`target_compile_options(test_<thing> PRIVATE -Wno-conversion ...)`) — precedent:
   `test_kernel32_nls`, `test_pe_resources`. Do not weaken the global flags.
4. Rebuild + run the whole suite, not just the new test.

Pattern to imitate: heavy crypto moved from a ~200 s under-TCG boot self-test to hosted `test_ec` /
`test_x509_verify`; the in-kernel copies stay gated behind the `selftests=full` cmdline token.
Prefer this shape — host-pin the logic, keep a thin on-target sanity copy.

## Tier 2 — Python static harnesses and audits

Ten are wired into the tier-1 ctest run (so `ctest --test-dir build/host-tests` already runs them):
`cxxeh_context_contract`, `verify_pe_exports`, `gen_fix_patches_retirement`,
`capability_access_static`, `include_tracked` (catches an untracked header left behind by
targeted-path staging), `iphlpapi_socket_abi`, `win32_syscall_numbers`, `dll_def_exports`,
`vmm_gdb_packet_vectors`, `kernel32_32_exports_complete`.

Standalone audits live in `tools/test/*.py`. Run them **from the repo root**; flag conventions vary:

```bash
# argparse --root style:
python3 tools/test/native-syscall-dispatch-bijection.py --root .
python3 tools/test/check-syscall-numbers.py --root .
python3 tools/test/check-dll-def-exports.py --root .
# positional-paths style (default scan roots: kernel drivers subsystems userland):
python3 tools/test/alloc-null-check-audit.py
python3 tools/test/untimed-waitqueue-audit.py
python3 tools/test/waitqueue-block-lock-audit.py
```

Check the script's header comment before assuming a flag — not all take `--root`.
These are tuned for zero false positives on a clean tree, so **any non-zero exit is a real
regression**, not noise.

Meta-tests (`tools/test/test-*.py`, e.g. `test-native-syscall-dispatch-bijection.py`,
`test-rust-ffi-signatures.py`, `test-kmutex-cancellation-contract.py`) are unittest suites that
verify the harnesses themselves. **If you edit a harness, run its meta-test**:
`python3 tools/test/test-<harness>.py`. If you add a harness, add a meta-test beside it.

## Tier 3 — Rust host tests

The workspace root `.cargo/config.toml` pins `target = x86_64-unknown-none` + build-std, and that
config cannot be overridden from inside the tree — so plain `cargo test` does not work. The wrapper
compiles each crate's `src/lib.rs` with `rustc --edition 2021 --test` directly (WSL/Linux):

```bash
bash tools/dev/cargo-host-test.sh              # all 22 hosted crates
bash tools/dev/cargo-host-test.sh ntfs_rust    # one crate (basename or full path)
```

Exit codes: `0` all pass, `1` any compile or test failure, `2` the filter matched no crate.
Covers 22 of the workspace's Rust crates (parsers, FS drivers, firmware parsers, IOMMU tables —
list is `HOST_TEST_CRATES` in the script). Crates **not** covered (no hosted `#[test]` path):
`kernel/rust`, `duetfs`, `aml_rust`, `class_rust`, `hid_rust` — changes there need tier 4/5
evidence instead. Adding coverage = add `#[test]` fns in the crate's `lib.rs` + append the crate
path to `HOST_TEST_CRATES`.

## Tier 4 — Fuzzing (`tests/fuzz/`)

37 libFuzzer harnesses (`fuzz_*.cpp`) over every untrusted-input parser: PE/ELF loaders, GPT,
FAT32/exFAT/NTFS/ext4, net, TLS, X.509/ASN.1, image decoders, EDID, USB class/HID, ACPI/AML,
GPU/NIC firmware blobs. Kernel-only headers are stubbed by `tests/fuzz/host_shim/`. Clang-only
(libFuzzer); needs the sanitizer runtime — `sudo apt-get install -y libclang-rt-18-dev` in WSL,
or **all 37 harnesses fail at link** with a missing `libclang_rt.asan-x86_64.a`
(`wiki/tooling/Dev-Host-Setup.md`).

```bash
tools/test/fuzz-all.sh                    # build all + run all; non-zero iff any artifact
FUZZ_SECONDS=300 FUZZ_JOBS=4 tools/test/fuzz-all.sh   # longer budget, bounded parallelism
make -C tests/fuzz all                    # just build (the cheap link-drift gate — see below)
make -C tests/fuzz run-eapol              # fuzz one harness for 60 s (corpus persists)
# Triage + replay:
ls tests/fuzz/build/art/*/crash-* tests/fuzz/corpus/*/crash-* 2>/dev/null
tests/fuzz/build/fuzz_<name> tests/fuzz/build/art/<name>/crash-<hash>
```

Artifacts (crash/timeout/oom/leak) are scoped per-harness under `tests/fuzz/build/art/<name>/`.
Harnesses with a `seeds/gen_<name>_seeds.py` get their corpus pre-seeded past the format gate.

**Recurring failure class — host_shim link drift.** When a kernel TU shared with a fuzz harness
gains a new dependency (a new `Result` helper, a flipped allocator signature, a new lock type),
the harness breaks **at link**, not at compile — `-fsyntax-only` does NOT catch it. This has
needed repeated fixes (`476d4e3c`, `f0c0db5d`, `d945fff6`, `6289a692`, `cb53f104`, ...). Rule:
after touching any TU that a `fuzz_*.cpp` or `host_shim/` file references, run
`make -C tests/fuzz all` — an actual LINK is the only sufficient check.

**Add a fuzz harness:** new `tests/fuzz/fuzz_<name>.cpp` (`LLVMFuzzerTestOneInput`), append
`$(BIN)/fuzz_<name>` to `HARNESSES` in `tests/fuzz/Makefile` + a build rule matching a sibling,
optionally `seeds/gen_<name>_seeds.py`, and a `maxlen_for` case in `fuzz-all.sh` if the format
needs more than the 4096-byte default. `fuzz-all.sh` auto-discovers built binaries, so the
Makefile row is the wiring.

## Tiers 5–6 — On-target: boot self-tests and ctest smokes

Execution mechanics (ISO build, QEMU launch, smoke profiles, log capture) live in
`duetos-boot-smoke-and-qemu`; self-test **authoring** idiom lives in `duetos-kernel-conventions`.
What belongs here is what they prove and how they gate:

- Self-tests register via `DUETOS_BOOT_SELFTEST(...)` in `kernel/core/boot_bringup.cpp` and emit
  `[<name>-selftest] PASS` / `FAIL` sentinels on serial. Expensive ones (the ~200 s under-TCG
  crypto class) are wrapped in `DUETOS_BOOT_SELFTEST_CI(...)`, which only runs when the kernel
  cmdline contains `selftests=full` — that token, **not** any `smoke=` profile flag, is the heavy
  gate.
- Root-build ctest rows (root `CMakeLists.txt`; run `ctest -R duetos- --output-on-failure` in the
  root build dir):

| Row | Timeout | SKIP code | Notes |
|---|---|---|---|
| `duetos-boot-smoke` | 720 s | exit 2 = SKIP (missing qemu/iso) | drives `tools/test/ctest-boot-smoke.sh` |
| `duetos-uefi-smoke` | 60 s | exit 2 = SKIP | Phase A banner via OVMF |
| `duetos-diff-boot-smoke` | 2400 s | exit **3** = SKIP; exit **2 = divergence = FAIL** | do not misread 2 as skip here |

- Triage any captured boot log with `tools/test/boot-log-analyze.sh <log>` — non-zero on any
  non-deliberate failure, so it doubles as a gate.
- Intermittent / timing-sensitive changes: `tools/test/boot-determinism-sweep.sh [runs] [timeout]`
  (defaults 8 runs, 120 s each) boots N times and diffs self-test counts, AP-online count, panic
  count, and distinct lockdep pairs across runs.

## The evidence bar — acceptance rules the incidents taught

Each rule cites the commit that paid for it. SHAs marked *(origin/main)* are reachable in the
repo's other branches but not in this worktree's branch history — `git show <sha>` still works.

1. **SKIP is not PASS.** `duetos-boot-smoke` exits 2 for "qemu/iso missing" and ctest prints SKIP —
   a run that skipped proved nothing. Always read the ctest tally line, not just the exit code.
2. **Absence of a FAIL line is not proof of pass.** The self-test may never have been called.
   Verify the `DUETOS_BOOT_SELFTEST` hook exists in `boot_bringup.cpp`, or grep for the explicit
   PASS sentinel if the test emits one.
3. **One boot is not verification for anything intermittent** (`a0d56701`). That incident's revert
   was only confirmed by a 6/6-clean sweep, and the sweep also exposed that the suspected UBSAN
   lines occurred ~4x/boot in known-clean boots — a single boot would have chased a red herring.
   Use `boot-determinism-sweep.sh`; ≥6 boots before claiming an intermittent is fixed.
4. **A count can be a floor.** Two error/pass counts are comparable only when both runs reached the
   same stage; a build that died early "improves" every downstream count to zero. An exactly-0-or-1
   result is the classic infrastructure-failure tell — check what produced it before believing it.
5. **Global-counter test oracles are unsound under SMP** (`8cfcc470`, origin/main): the ELF unwind
   self-test false-positived once other CPUs were live and mutating the counter it asserted on.
   Assert on per-task / per-invocation state, never on a global a peer CPU can touch.
6. **Fault injection must be scoped to the task that armed it** (`dd45d709`, origin/main): a
   test-only OOM injector that fired for unrelated subsystems manufactured failures elsewhere.
   A harness that can inject faults outside its own scope produces evidence about nothing.
7. **Serial verdicts must be single-write lines** (`3d6e870a`, origin/main): multi-write verdict
   lines interleave under SMP and the grep misses them. One `SerialWrite` per sentinel.
8. **Compile-clean ≠ link-clean ≠ runtime-clean.** `-fsyntax-only` misses the host_shim link-drift
   class entirely (tier 4 above); a successful link still says nothing about behaviour. Climb to
   the tier that observes the claim you are making.
9. **`wsl.exe -- bash -lc '...'` fabricates exit codes** — use `wsl.exe -e bash -lc ...` or a
   sentinel branch when reading a WSL exit status from the Windows side. Full write-up:
   `duetos-build-and-env` "Trap zero".
10. **Renaming anything a smoke expectation matches breaks the smoke** (`541411a0`): renaming a
    NIC classifier tag desynced the `expected=(...)` family list in
    `tools/test/ctest-boot-smoke.sh`. Before renaming a sentinel, tag, or log prefix, run
    `grep -n "expected=" -A 60 tools/test/ctest-boot-smoke.sh` and update the list in the same
    commit.

## Adding a test — per-tier wiring checklist

A test that exists but is not wired into a runner is dead code ("Wiring Things In", `CLAUDE.md`).

- **Host test:** `tests/host/test_<x>.cpp` + `add_host_test(<x>)` row (+ `target_sources` for
  non-header-only TUs). Run the full suite once.
- **Python harness:** script in `tools/test/` + an `add_test(...)` row in
  `tests/host/CMakeLists.txt` (so it gates every ctest run) + a `tools/test/test-<x>.py` meta-test.
- **Rust:** `#[test]` in the crate + crate path in `HOST_TEST_CRATES` in
  `tools/dev/cargo-host-test.sh`.
- **Fuzz harness:** `fuzz_<x>.cpp` + `HARNESSES` row and build rule in `tests/fuzz/Makefile`
  (+ seeds generator + `maxlen_for` case if needed). Prove it links: `make -C tests/fuzz all`.
- **Boot self-test:** body per `duetos-kernel-conventions`; register in `boot_bringup.cpp`
  (`DUETOS_BOOT_SELFTEST`, or `DUETOS_BOOT_SELFTEST_CI` if expensive); fire a `KBP_PROBE` on the
  failure leg; if the smoke should assert the sentinel, add it to `expected=(...)` in
  `ctest-boot-smoke.sh` in the same commit (rule 10).

## Verification before completion (DuetOS edition)

Before saying "done" / committing, walk this list against what you actually changed:

- [ ] Named the tier that observes your claim (table above) and **ran it** — not a cheaper proxy.
- [ ] Tier-1 suite green: `ctest --test-dir build/host-tests --output-on-failure` (also covers
      the 10 wired static harnesses).
- [ ] Touched a shared kernel TU? `make -C tests/fuzz all` links (rule 8).
- [ ] Touched a hosted Rust crate? `bash tools/dev/cargo-host-test.sh <crate>`.
- [ ] Runtime behaviour changed? Live boot ran and `tools/test/boot-log-analyze.sh <log>` exited 0
      — and you confirmed it ran the path, not skipped it (rules 1–2).
- [ ] Intermittent or SMP/timing-sensitive? Sweep, ≥6 boots (rule 3).
- [ ] Renamed any sentinel/tag/prefix? `ctest-boot-smoke.sh` `expected=` list updated (rule 10).
- [ ] Every count you are quoting as progress: both runs reached the same stage (rule 4).
- [ ] Then the project-wide Definition of Done in `CLAUDE.md` (wiki, roadmap, clang-format, CI).

## Provenance and maintenance

Authored 2026-08-13 against worktree branch `claude/fable-driver-wave-20260801`, HEAD `8a55872c`.
Volatile counts (68 host tests, 22 Rust crates, 37 fuzz harnesses, 10 wired python rows) are
as-of that date. Re-verify with:

```bash
ls tests/host/test_*.cpp | wc -l                                  # host test count
grep -c add_host_test tests/host/CMakeLists.txt                   # wired host rows (+1 for the function def)
grep -c "^add_test" tests/host/CMakeLists.txt                     # python harness rows (excludes the one inside add_host_test)
grep -c '"kernel/' tools/dev/cargo-host-test.sh                   # hosted Rust crates
ls tests/fuzz/fuzz_*.cpp | wc -l                                  # fuzz harnesses
grep -n "SKIP_RETURN_CODE\|TIMEOUT " CMakeLists.txt               # on-target ctest rows
grep -n "selftests=full" kernel/core/boot_bringup.cpp             # heavy self-test gate
grep -n "expected=(" tools/test/ctest-boot-smoke.sh               # smoke expectation list
git show a0d56701 541411a0 --stat                                 # incident commits (in-branch)
```

Incident SHAs `8cfcc470`, `dd45d709`, `3d6e870a` verified via `git show <sha>` on 2026-08-13
(present in repo, not ancestors of this branch's HEAD). If any command above stops matching,
update this skill in the same commit that changed the underlying file.
