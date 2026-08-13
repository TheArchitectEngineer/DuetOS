---
name: duetos-kernel-conventions
description: >-
  DuetOS kernel C++/Rust idiom pack: the exact error-handling, logging, locking,
  allocation, initcall, self-test, and marker conventions code must follow to pass
  review. TRIGGER when the task is to "write kernel code", "return an error / use
  Result or RESULT_TRY", "add a log line / which KLOG macro", "take a spinlock",
  "allocate in the kernel" (frames/kheap/MMIO), "add a self-test or KBP_PROBE",
  or "place a STUB/GAP marker". DO NOT TRIGGER for: the driver probe/module model (use
  duetos-driver-architecture), cap-gating and syscall-boundary rules (use
  duetos-subsystem-isolation), build/QEMU invocations (duetos-build-and-env,
  duetos-boot-smoke-and-qemu), or triaging an existing failure
  (duetos-debugging-playbook).
---

# DuetOS kernel conventions — the idiom pack

This is the reference for HOW to write DuetOS kernel code: the exact macros,
types, and patterns reviewers expect. Every idiom below was verified against
the tree on 2026-08-13 (branch `claude/fable-driver-wave-20260801`, HEAD
`8a55872c`). If a signature here disagrees with the header, the header wins —
re-verify with the commands in the Provenance section.

**Not this skill's job:** driver probe/registration shape
(`duetos-driver-architecture`), what a subsystem may touch
(`duetos-subsystem-isolation`), how to build or boot
(`duetos-build-and-env`, `duetos-boot-smoke-and-qemu`), or debugging an
existing crash (`duetos-debugging-playbook`).

## Quick reference: task → idiom

| You want to... | Use | Defined in |
|---|---|---|
| Return an error from a fallible function | `return Err{ErrorCode::Foo};` | `kernel/util/result.h` |
| Return success from `Result<void>` | `return {};` | `kernel/util/result.h` |
| Propagate a callee's error | `RESULT_TRY(expr);` | `kernel/util/result.h` |
| Propagate + bind the value | `RESULT_TRY_ASSIGN(u64 n, expr);` | `kernel/util/result.h` |
| Log a failure summary | `KLOG_WARN(subsys, msg)` / `KLOG_ERROR_V(...)` | `kernel/log/klog.h` |
| Log verbose debug detail | `KLOG_DEBUG_V(subsys, msg, val)` | `kernel/log/klog.h` |
| Log once per boot from a hot path | `KLOG_ONCE_WARN(subsys, msg)` / `KLOG_ONCE_WARN_V` | `kernel/log/klog.h` |
| Trace a function's enter/exit + elapsed | `KLOG_TRACE_SCOPE(subsys, "FnName");` (first line) | `kernel/log/klog.h` |
| Silenceable high-volume chatter | area-tagged `KLOG_DEBUG_AV(LogArea::Net, ...)` | `kernel/log/klog.h` |
| Emit a resource snapshot at a checkpoint | `KLOG_METRICS(subsys, label)` | `kernel/log/klog.h` |
| Guard shared state (IRQ-safe) | `sync::SpinLockGuard g(lock);` | `kernel/sync/spinlock.h` |
| Non-blocking lock attempt | `sync::SpinLockTryGuard` (check `held()`) | `kernel/sync/spinlock.h` |
| Nested acquire under same public API | `sync::SpinLockRecursiveGuard` | `kernel/sync/spinlock.h` |
| Assert caller holds a lock | `SpinLockAssertHeld(lock)` / `LOCKDEP_ASSERT_HELD(class_id)` | `spinlock.h` / `lockdep.h` |
| Allocate a physical 4 KiB frame | `RESULT_TRY_ASSIGN(mm::PhysAddr f, mm::AllocateFrame());` | `kernel/mm/frame_allocator.h` |
| Allocate heap memory | `void* p = mm::KMalloc(bytes);` (nullptr on exhaustion) | `kernel/mm/kheap.h` |
| Map device registers | `mm::TryMapMmio(phys, bytes)` → `Result<void*>` | `kernel/mm/paging.h` |
| Register an init hook | `KERNEL_INITCALL(Drivers, "nvme.module", RegisterNvmeModule)` | `kernel/core/init.h` |
| Mark a regression site for GDB | `KBP_PROBE_V(ProbeId::kFoo, value)` | `kernel/debug/probes.h` |
| Gate a pure boot self-test | `DUETOS_BOOT_SELFTEST(FooSelfTest());` | `kernel/util/build_config.h` |
| Gate a >seconds-long self-test | `DUETOS_BOOT_SELFTEST_CI(...)` (cmdline `selftests=full`) | `kernel/core/boot_bringup.cpp` |
| Mark deliberately-wrong v0 code | `// STUB: <what's fake>` | convention (CLAUDE.md) |
| Mark a known-missing edge case | `// GAP: <missing> — <when to revisit>` | convention (CLAUDE.md) |

## Language, style, naming

- **C++23, freestanding.** No RTTI, no exceptions (`-fno-exceptions`); fallible
  paths return `duetos::core::Result<T, E>`. Zero warnings: `-Wall -Wextra
  -Wpedantic -Werror` (GCC/Clang), `/W4 /WX` on MSVC-hosted tests.
- **Naming:** `PascalCase` classes/methods/free functions, `camelCase` locals,
  `m_` prefix members, `UPPER_SNAKE` macros, `k`-prefixed `constexpr` constants
  (`kPageSize`, `kMaxInitcalls`), `g_` file-scope globals (`g_serial_lock`,
  `g_expensive_selftests`), `constinit` on kernel globals to forbid runtime
  static constructors.
- **Style:** Allman braces, 4-space indent, 120-column limit — `.clang-format`
  at repo root is `BasedOnStyle: Microsoft`, `Standard: Latest`. LF endings.
  `#pragma once` in every header; forward-declare over include where possible.
- **NEVER run `clang-format -i` on a `.S` file.** It parses assembly as C++ and
  mangles it. Assembly is hand-formatted (NASM/GAS, Intel syntax for NASM).
- **`std::` is userland-only.** Kernel code uses `duetos::` primitives and the
  fixed-width types from `kernel/util/types.h` (`u8..u64`, `i32`, `uptr`).
  CLAUDE.md's stated ownership standard is `std::unique_ptr` / `UniquePtr`
  owning, raw pointers non-owning — with the kernel using "the project's own
  smart pointer primitives". Current tree-state (2026-08-13, re-verify): no
  kernel smart-pointer class exists yet, so ownership is manual for now — the
  allocating scope frees on every exit path, raw pointers are non-owning, and
  error paths unwind explicitly (see the PE loader's alloc-ladder unwind for
  the pattern). If a kernel `UniquePtr` primitive has landed since, prefer it.
  `const` on all non-mutating methods/params; `constexpr` wherever it works.
- **No naked `new`/`delete`, no global mutable state** outside explicit
  per-CPU areas. If it looks like a singleton, it's probably per-CPU or
  per-process.

## Error handling: `Result<T, E>` (kernel/util/result.h)

The kernel-wide error enum is `duetos::core::ErrorCode : u8`:
`OutOfMemory, InvalidArgument, NotFound, AlreadyExists, PermissionDenied,
Timeout, Unsupported, BadState, IoError, Truncated, BufferTooSmall, Overflow,
Corrupt, NotReady, Busy, Deadlock, NoDevice, Unknown` (plus the `Ok = 0`
sentinel that never appears on an error path). `ErrorCodeName(code)` gives a
stable string for logs.

Canonical shapes (verbatim from the header's own doc block, ~line 32):

```cpp
Result<u64> ReadCount() {
    if (...)
        return ::duetos::core::Err{ErrorCode::IoError};
    return 42;
}

Result<void> Foo() {
    RESULT_TRY_ASSIGN(u64 n, ReadCount());  // early-return on error
    (void)n;
    return {};                              // success for Result<void>
}

Result<Thing> Bar() {
    RESULT_TRY(Foo());
    return Thing{};
}
```

Rules that trip people:

- **`RESULT_TRY_ASSIGN`'s first argument is a declarator** (`u64 n`,
  `auto x`) — never parenthesize it, it is not an expression.
- **T and E must be trivially copyable** (static_asserted). For a non-trivial
  payload, split the lifecycle manually.
- `Result` is `[[nodiscard]]` — you must check or propagate it.
- **Source-location capture is on by default** (`DUETOS_RESULT_LOC=1`): every
  `Err{code}` stamps file/func/line and `RESULT_TRY` propagates the ORIGINAL
  throw site, so log it at the sink, don't re-wrap errors just to re-tag them.
- Mixed error enums don't auto-coerce: a `Result<T, fs::Error>` body cannot
  `RESULT_TRY` a `Result<U, ErrorCode>` — translate explicitly at the boundary.
- `value_or(fallback)` is for genuinely sentinel-shaped boundaries only, not a
  substitute for handling the error.
- **Never return `-1` / `false` / `nullptr` sentinels from new code.** Two
  grandfathered exceptions: `mm::KMalloc` returns nullptr and `mm::MapMmio`
  returns 0 on failure — new callers should prefer `mm::TryMapMmio` (the
  Result-shaped sibling in `kernel/mm/paging.h`).

## Logging: klog (kernel/log/klog.h)

Every macro takes `(subsys_string, msg, ...)` where `subsys` is the
kernel-tree path sans `kernel/` (e.g. `"mm/paging"`, `"drivers/net/e1000"`).
Levels: Trace, Debug, Info, Warn, Error, Critical. Warn+ macros auto-capture
`__FILE__:__LINE__` and the renderer appends `at <path>:<line>`; Trace/Debug/
Info deliberately do not. Suffix grammar:

| Suffix | Shape | Example |
|---|---|---|
| (none) | message only | `KLOG_INFO("fs/vfs", "mounted root")` |
| `_V` | + one u64 as hex(dec) | `KLOG_WARN_V("mm/kheap", "alloc failed", bytes)` |
| `_S` | + labelled string | `KLOG_INFO_S("pci", "device", "name", dev_name)` |
| `_2V` | + two labelled u64s | `KLOG_INFO_2V("mm", "region", "base", b, "size", s)` |
| `_A` / `_AV` / `_AS` / `_A2V` | explicit `LogArea` first arg | `KLOG_DEBUG_AV(LogArea::Net, "net/tcp", "rtt", us)` |
| `KLOG_ONCE_INFO/WARN[_V]` | fires at most once per boot per call site | stub warnings, hot loops |
| `KLOG_TRACE_SCOPE(subsys, name)` | RAII enter/exit + elapsed µs + hang tracking | first line of init functions |
| `KLOG_METRICS(subsys, label)` | one-line heap/frames/ctx-switch/task snapshot | phase boundaries |

Gating discipline (the "Diagnostic Logging" contract in CLAUDE.md):

1. **Keep diagnostics you added while fixing a bug** — gate them, don't delete
   them. A clean boot stays quiet at default levels; a regression boot leaves a
   WARN sentinel + DEBUG-gated detail.
2. **WARN for the failure summary line, DEBUG_V/DEBUG_S for the verbose
   detail.** Trace-level call sites compile out entirely when
   `DUETOS_KLOG_COMPILE_FLOOR > 0` (release default is 1 = Debug floor);
   runtime threshold defaults to Warn in release (`kKlogDefaultLevel = 3`).
3. **Use area-tagged `_A` variants for high-volume chatter** (driver bring-up,
   syscall dispatch) so `logarea off <name>` can silence it. `LogArea` has one
   bit per subsystem class (Memory, Sched, Net, Storage, USB, GPU, Win32, ...).
4. **Avoid raw `arch::SerialWrite(...)` for new diagnostics.** It bypasses
   log levels and prints on every boot forever. Its only two legitimate uses:
   the pre-klog bring-up path, and structural sentinels CI greps for
   (self-test `PASS`/`FAIL` lines, `[smoke] profile=... complete`).
5. **Multi-call SerialWrite sequences must be wrapped in
   `arch::SerialLineGuard guard;`** (RAII, `kernel/arch/x86_64/serial.h`
   ~line 175) — without it SMP interleaves other CPUs' output at every call
   boundary and signature-grepping CI tests can't match the line. Keep the
   guarded scope short; IRQs are off inside it.
6. **Log-level abuse is a named bug class:** a WARN/ERROR that fires on a
   legitimate API failure mode (a timeout the caller handles, a
   release-from-non-owner the API defines) floods the log on normal contended
   workloads. Demote it to DEBUG — the return value is the real notification
   channel.
7. Do not promote every self-test PASS to `KLOG_INFO`; silent-pass is the
   default contract (see the self-test section).

## THE TRAP: never hold a lock across a KLOG or SerialWrite call

klog and raw `SerialWrite` both take the serial lock internally. Any subsystem
lock held across a log call therefore establishes the lock ordering
`subsystem-lock -> serial-lock`; because log writers already hold the serial
lock and can re-enter subsystem recorders, that ordering inverts into an ABBA
deadlock once SMP is up. This is not theoretical: the fix-journal held its
`g_lock` across a `KLOG_INFO_V` and hung the entire smoke matrix
(fixed in commit `05121fe8`, 2026-08). The pattern:

```cpp
// WRONG — deadlocks under SMP:
{
    sync::SpinLockGuard g(g_state_lock);
    m_count++;
    KLOG_INFO_V("diag/journal", "recorded", m_count);   // takes serial lock while holding g_state_lock
}

// RIGHT — snapshot under the lock, log after release:
u64 count_snapshot = 0;
{
    sync::SpinLockGuard g(g_state_lock);
    m_count++;
    count_snapshot = m_count;
}
KLOG_INFO_V("diag/journal", "recorded", count_snapshot);
```

A tree-wide static scanner for this shape exists on `main` since 2026-08-03:
`tools/test/check-spinlock-log-order.py` (commit `3e730aa1`; not present on
branches cut before that date). It tracks brace depth from each guard, so a
clean result is necessary, not sufficient — it cannot see logging inside a
callee. Review call chains by hand.

## Locking (kernel/sync/)

`sync::SpinLock` is a FIFO ticket lock with IRQ save/restore.
Zero-initialized = unlocked, so `static constinit SpinLock g_lock{};` works
anywhere. Manual and RAII forms:

```cpp
// Manual pair — flags MUST round-trip exactly:
const sync::IrqFlags flags = sync::SpinLockAcquire(lock);
// ... critical section (stay on-CPU, no blocking calls, no KLOG) ...
sync::SpinLockRelease(lock, flags);

// Preferred RAII:
{
    sync::SpinLockGuard g(lock);
    // ...
}

// Non-blocking: returns ErrorCode::Deadlock (self-held) or Busy:
sync::SpinLockTryGuard tg(lock);            // or (lock, max_spins) — Timeout on budget
if (tg.held()) { /* ... */ }

// Public entry points that legitimately nest (A calls B, both take the lock):
sync::SpinLockRecursiveGuard rg(lock);
```

Rules:

- **Not recursive** (plain acquire): re-acquiring a lock this CPU holds
  self-deadlocks. Use `SpinLockRecursiveGuard` for the nested-public-entry
  shape, or split into `Foo()` + `FooLocked()`.
- **Holding a spinlock across any blocking call is a contract violation**
  (SchedYield, WaitQueueBlock, anything that can sleep). Sleeping in an IRQ
  handler is a bug; no driver holds a sleeping mutex across DMA.
- **Document which locks the subsystem owns at the top of its header**, and
  put `SpinLockAssertHeld(lock)` at the top of `*Locked()` helpers that assume
  the caller acquired it.
- **Tag contended locks with a lockdep class** (`lock.class_id = kLockClass...`
  from `kernel/sync/lockdep.h`) to opt into lock-order validation; untagged
  locks (class 0) pay nothing. `LOCKDEP_ASSERT_HELD(class_id)` asserts the
  precondition and fires the `kLockdepAssertHeldFailed` probe on violation.
- Sleepable / heavier primitives live alongside: `adaptive_mutex.h`,
  `rwlock.h`, `seqlock.h`, `rcu.h`. Pick a spinlock only for short,
  on-CPU critical sections reachable from IRQ context.

## Memory allocation (kernel/mm/)

There is no `malloc`, no global `operator new`. Three allocators, three jobs:

- **Physical frames** — `kernel/mm/frame_allocator.h`. All Result-shaped:
  `AllocateFrame()`, `AllocateFrameNode(node)`, `AllocateFrameInRange(max_phys)`
  (DMA windows), `AllocateContiguousFrames(count)`,
  `AllocateContiguousFramesInRange(count, max_phys)` — each returns
  `core::Result<PhysAddr>` with `Err{ErrorCode::OutOfMemory}` on exhaustion.
  Free with `FreeFrame(frame)` / `FreeContiguousFrames(base, count)` — the
  count must match the original allocation. `kPageSize = 4096`.
  OOM-path testing: `FrameAllocatorSetFailAfter(n)` injects failure after n
  more allocations — use it in self-tests that claim to handle OOM.
- **Kernel heap** — `kernel/mm/kheap.h`. `void* mm::KMalloc(u64 bytes)`
  (nullptr on exhaustion or bytes==0; check it — the `kHeapAllocFail` probe
  fires on the failure path), `mm::KFree(void* ptr)` (nullptr no-op;
  double-free panics). IRQ-safe and SMP-safe. Small allocations (1..512 B)
  route to slab caches automatically. Pool is a fixed 64 MiB
  (`kKernelHeapBytes`) with NO growth — the header explicitly says the durable
  fix for OOM is shrinking per-object cost, not raising the pool.
- **MMIO** — `kernel/mm/paging.h`. Prefer
  `RESULT_TRY_ASSIGN(void* regs, mm::TryMapMmio(phys, bytes));` over raw
  `MapMmio` (which returns 0 on arena exhaustion). `UnmapMmio(virt, bytes)`
  tears mappings down but the virtual arena range is a bump allocator and is
  NOT recycled — don't map/unmap in a loop.

Every acquire needs its release on EVERY exit path — refcount/alloc asymmetry
on error legs is a named recurring bug class (see CLAUDE.md "Fix Anything You
Surface"). Walk each exit from the acquiring scope: it either
succeeded-and-handed-off or failed-and-rolled-back.

## Initcalls (kernel/core/init.h)

Two coexisting registration forms feed one registry (`Phase`-bucketed, fixed
`kMaxInitcalls = 64`, run by `RunPhase(phase)` in registration order, stopping
at the first `Err`):

```cpp
// 1. File-scope macro (preferred for modules) — real examples:
//    kernel/drivers/storage/nvme.cpp:1862, kernel/fs/ramfs.cpp:1461
static ::duetos::core::Result<void> RegisterNvmeModule() { /* ... */ return {}; }
KERNEL_INITCALL(Drivers, "nvme.module", RegisterNvmeModule)

// 2. Runtime call from a bring-up TU:
core::InitcallRegisterOrPanic(core::Phase::Drivers, "my-subsys", MyInit);
```

Phases run in numeric order: `Earlycon, PhysMem, Paging, Heap, Idt, Apic,
Time, PerCpuBsp, Sched, Smp, Drivers, Vfs, Userland`. Registration is
single-threaded boot-only (not thread-safe by design; the table is read-only
after AP bring-up). `InitcallRegisterOrPanic` is for fixed boot registrations
where failure is a programmer bug; anything that can legitimately fail at
runtime uses `InitcallRegister` and handles the `Err`. Registration alone does
NOT imply execution — `RunPhase` must be called by the boot driver, and every
`init()` you write must be reachable from it ("Wiring Things In" rule: a
built-but-never-called subsystem gets wired in or deleted).

## Probes: KBP_PROBE (kernel/debug/probes.h)

A probe is a compile-time call site with a runtime arm state — the cheap way
to make a regression self-announce and give GDB a break anchor. Fire path is
"load a u8, compare, maybe log": safe from IRQ/trap context, no locks.

```cpp
// In a failure leg (value = which sub-check / offending datum):
KBP_PROBE_V(duetos::debug::ProbeId::kBootSelftestFail, check_index);
// No context value:
KBP_PROBE(duetos::debug::ProbeId::kProbeFail);
```

Adding a NEW probe is exactly three edits: extend `enum class ProbeId` in
`probes.h`, add one row to `kProbeTable` in `probes.cpp` (name like
`"panic.enter"` + default arm state), and place the macro at the site. Pick
`ProbeArm::ArmedLog` for rare high-signal events — a clean run logs nothing, a
regression run emits `[probe] <tag> rip=<caller>` immediately. Operators flip
arm states with the `probe` shell command. Pair with the live GDB stub
(`DUETOS_GDB_SERVER=ON`, attach via `tools/debug/duetos-gdb-attach.sh`, then
`b duetos::debug::ProbeFire`) to halt at the exact frame a regression first
surfaces.

## Boot self-tests

Registration is the flat call list in `kernel/core/boot_bringup.cpp`. Two
gates:

- `DUETOS_BOOT_SELFTEST(FooSelfTest());` (`kernel/util/build_config.h`) —
  compiled out in release (`kBootSelfTests` off). ONLY for pure tests with NO
  init side effect. A self-test that doubles as init validation (seed + check,
  like `RegistrySelfTest` after `RegistryInit`) is called unwrapped.
- `DUETOS_BOOT_SELFTEST_CI(HeavySelfTest());` (defined in
  `boot_bringup.cpp` ~line 476) — additionally requires the kernel cmdline
  token `selftests=full`. For wall-clock-expensive tests (the crypto cluster
  runs ~minutes under QEMU TCG). Deliberately NOT triggered by `smoke=` — the
  CI smoke gate must stay fast.

Sentinel contract (one `arch::SerialWrite` line, greppable by CI):

```
[<name>-selftest] PASS (<detail>)
[<name>-selftest] FAIL check=<n>
```

**Silent-pass is the default:** only emit the explicit PASS line if CI or a
human needs grep-able proof; never promote every PASS to `KLOG_INFO`. And the
inverse: absence of a FAIL line is NOT proof of pass — it may mean the test
was never called. Verify the hook exists in `boot_bringup.cpp`.

### Writing a new self-test — checklist

1. Name it `<Subsys>SelfTest()`, declare it in the subsystem's header next to
   its siblings (`ResultSelfTest`, `SpinLockSelfTest`, `PagingSelfTest`, ...).
2. Failure leg: emit `[<name>-selftest] FAIL check=<n>` via
   `arch::SerialWrite` (wrap multi-call output in `SerialLineGuard`), fire
   `KBP_PROBE_V(ProbeId::kBootSelftestFail, n)`, then panic or return —
   match your siblings' severity choice.
3. Hook it into `boot_bringup.cpp`: `DUETOS_BOOT_SELFTEST(...)` if pure,
   `DUETOS_BOOT_SELFTEST_CI(...)` if it costs more than ~a second under TCG,
   bare call if it doubles as init.
4. If it claims OOM handling, prove it with `FrameAllocatorSetFailAfter`.
5. Run the boot smoke and confirm (a) the FAIL leg actually fires when you
   sabotage a check, (b) a clean boot stays quiet at default log levels. See
   `duetos-testing-and-validation` / `duetos-boot-smoke-and-qemu` for harness
   invocations.

## STUB / GAP marker discipline

Two greppable markers bound the "what doesn't work yet" inventory
(`git grep -nE "// (STUB|GAP):"` — 182 matching lines under `kernel/` as of
2026-08-13: 14 STUB, 169 GAP, one doc line in `diag/fix_journal.h` naming
both):

- `// STUB:` — the code returns a constant / does nothing / returns the wrong
  target. Real callers WILL misbehave. Stays until a real implementation
  lands. Example (`kernel/subsystems/linux/syscall_misc.cpp` ~line 728):
  `// STUB: always claims the capability is present.`
- `// GAP: <what's missing> — <when to revisit>` — correct for the v0 happy
  path, one documented edge unimplemented. Example
  (`kernel/acpi/aml_eval.cpp` ~line 246):
  `// GAP: no >1 GiB SystemMemory — revisit if a real DSDT needs it.`

Place the marker on or immediately above the line that bakes in the omission.
**Do NOT pepper markers on code that does its job** — if removing the marker
wouldn't change a maintainer's belief about what works, don't write it.
Hot STUB sites can additionally report through the fix-journal macros in
`kernel/diag/fix_journal.h` so runtime hits are counted.

## Rust conventions

Rust is permitted for greenfield subsystems where memory-safety wins
(filesystems, USB, network) — but the subsystem must stand alone: no
Rust-in-the-middle of a C++ call chain. Toolchain is pinned in
`rust-toolchain.toml`: `nightly-2026-01-15`, `rust-src`, target
`x86_64-unknown-none`; bumping the pin is its own PR, never bundled.
Workspace lints (root `Cargo.toml`) deny `unsafe_op_in_unsafe_fn`,
`unused_must_use`, `non_ascii_idents`, plus the clippy lints that ban
leftover `unimplemented!()` / `dbg!()` / unfinished-marker macros (exact
names in the `[workspace.lints.clippy]` block). New crates put a `// SAFETY:` comment on every unsafe block
(the pre-discipline duetfs/USB crates are grandfathered). FFI ingress
contracts are checked by `tools/test/check-rust-ffi-signatures.py` /
`check-rust-ffi.py`.

## Provenance and maintenance

Written 2026-08-13 against branch `claude/fable-driver-wave-20260801`, HEAD
`8a55872c`. All symbols verified by reading the headers listed below; line
numbers are approximate. Re-verify before trusting a volatile fact:

```bash
# Result API + ErrorCode list
sed -n '90,120p;320,342p' kernel/util/result.h
# KLOG macro families + compile floor
grep -n "#define KLOG_" kernel/log/klog.h | head -40
# SpinLock API + guards
grep -n "SpinLock\|IrqFlags" kernel/sync/spinlock.h | head -20
# Allocators
grep -n "Result<PhysAddr>\|KMalloc\|TryMapMmio" kernel/mm/frame_allocator.h kernel/mm/kheap.h kernel/mm/paging.h
# Initcall phases + macro
grep -n "enum class Phase\|KERNEL_INITCALL" kernel/core/init.h
# Probe macro + table shape
grep -n "KBP_PROBE\|kProbeTable" kernel/debug/probes.h kernel/debug/probes.cpp | head
# Self-test gates
grep -n "DUETOS_BOOT_SELFTEST" kernel/util/build_config.h kernel/core/boot_bringup.cpp | head
# Live STUB/GAP inventory (count drifts constantly)
git grep -cE "// (STUB|GAP):" -- kernel | awk -F: '{s+=$2} END {print s}'
# Lock-across-log scanner (on main since 2026-08-03; absent on older branches)
ls tools/test/check-spinlock-log-order.py
# Rust pin + lints
head -25 rust-toolchain.toml; grep -A6 "workspace.lints" Cargo.toml
```

Volatile facts most likely to drift: the STUB/GAP counts, `kKernelHeapBytes`
(64 MiB, explicitly under pressure), `kMaxInitcalls` (64), the ProbeId roster,
and the Rust toolchain pin.
