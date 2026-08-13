---
name: duetos-subsystem-isolation
description: >-
  DuetOS subsystem-isolation doctrine and cap-gating machinery. TRIGGER when the task is "add a syscall", "write
  or modify a Win32 thunk / NT syscall handler", "make a Linux ABI change", "add or review a cap gate", "touch
  privilege checks", "write token or ACL code", or whenever any subsystem code touches kernel state. DO NOT TRIGGER for generic kernel
  idioms, naming, Result<T,E>, or lock conventions — use duetos-kernel-conventions; for driver probe/bus wiring
  use duetos-driver-architecture; for running the deny-leg smoke tests use duetos-testing-and-validation.
---

# DuetOS Subsystem Isolation — the DO-NOT-VIOLATE rules

## What this is

DuetOS runs Windows PE and Linux ELF binaries through in-kernel subsystem
facades (`kernel/subsystems/win32/`, `kernel/subsystems/linux/`) and
freestanding userland DLLs (`userland/libs/*`). This skill is the doctrine
that keeps those facades from ever *driving* the OS, plus the concrete
capability-gating machinery that enforces it. **Violations are bugs even if
they compile. Fix the gate; never extend the violation.**

Jargon, defined once:

| Term | Meaning |
|------|---------|
| **Facade / thunk** | Code that translates a foreign ABI shape (Win32, Linux) into the same kernel syscall a native DuetOS process would make. It has no authority of its own. |
| **Cap / capability** | One bit in `CapSet` (`kernel/proc/process.h`, enum `Cap`): `kCapFsRead`, `kCapFsWrite`, `kCapSpawnThread`, `kCapNet`, `kCapDebug`, `kCapInput`, `kCapDiag`, … 11 live bits as of 2026-08. `kCapCount` is a sentinel, never a live cap. |
| **Cap gate** | A check that a process holds the required cap before a state-mutating operation proceeds. Two layers exist (see below). |
| **Broker lease** | A temporary, generation-tagged, deadline-bounded cap grant installed by the elevation broker (`kernel/security/broker.h`). The ONLY post-spawn way a process gains a cap. |
| **Grant ceiling** | `Process::cap_ceiling` — a monotonic upper bound on what can ever be granted. `SYS_DROPCAPS` and `SE_PRIVILEGE_REMOVED` lower it irreversibly for the process lifetime. |

## The doctrine (memorize this paragraph)

**Win32 and Linux subsystems are facades for executing PE/ELF binaries. They
never drive DuetOS.** The kernel — its capability set, scheduler,
address-space ledger, filesystem mediation, and IPC — is the authority on
every effect a guest binary can have. Full text:
`wiki/kernel/Subsystem-Isolation.md`.

The one-question review test for any diff touching `kernel/subsystems/*` or
`userland/libs/*`:

> Could a malicious PE / ELF use this path to do something a native DuetOS
> process couldn't?

If yes, **the gate is wrong, not the workload**.

## The six rules

| # | Rule | Concrete form |
|---|------|---------------|
| 1 | No subsystem code mutates DuetOS state without a kernel-mediated, cap-gated syscall | A Win32 PE writing a file goes through `SYS_FILE_WRITE` (kCapFsWrite); a Linux binary spawning a thread goes through `SYS_THREAD_CREATE` (kCapSpawnThread). The thunk never skips the gate. |
| 2 | Auth and privilege are kernel-owned | Effective authority = durable `Process::caps` + unexpired broker leases, masked by monotonic `cap_ceiling`, serialized by `Process::cap_lock`. Gates consume only `ProcessCapsSnapshot` / `ProcessHasCap`. Token code never mutates cap storage directly. |
| 3 | Userland DLLs (`userland/libs/*`) are freestanding | No kernel headers, no assumptions about kernel internals. They issue syscalls and trust the return. |
| 4 | In-kernel subsystem code routes through public kernel APIs only | `mm::*`, `sched::*`, `fs::routing::*`, `core::Cap*`. Never touch regions tables, runqueues, or capability bitsets directly. |
| 5 | No subsystem-to-subsystem coupling | Win32 never calls Linux; Linux never calls Win32. Both call the kernel. |
| 6 | One source of truth per resource | One TCP stack, one VFS, one registry, one window manager — multiple ABI front-ends, one kernel-owned implementation. |

## The two enforcement layers

### Layer 1 — central static gate (`core::SyscallGate`)

`kernel/syscall/cap_gate.h` / `.cpp`. `SyscallDispatch`
(`kernel/syscall/syscall.cpp`, ~line 525) calls
`SyscallGate(num, proc)` once, before the per-syscall switch. It consults
`kSyscallCapTable`, whose rows are `#include`d from
`kernel/syscall/cap_table.def`.

Behavior you must know:

- A syscall number **not in the table** gets `required_mask == 0` and the
  gate passes — that is the deliberate "handler enforces its own auth"
  signal, NOT an oversight. Context-dependent policy (foreign-PID vs self in
  `SYS_PROCESS_OPEN`, fd-dependent `SYS_WRITE`) stays in the handler; the
  table holds only *unconditionally* required caps.
- A `nullptr` process (kernel-thread origin) passes only if
  `required_mask == 0`.
- On denial the gate itself calls `RecordSandboxDenial(missing_cap)` and
  logs `KLOG_WARN_V("syscall-gate", ...)`; dispatch sets `rax = -1`.
- Every gated call (allow or deny) also fires the audit hook
  `security::CapAuditTrace` (`kernel/security/cap_audit.h`); denials persist
  as `diag::FixCategory::CapDenial` rows (`kernel/diag/fix_journal.h`).
- `SyscallGateSelfTest()` walks every table row at boot with a synthetic
  empty-caps and a trusted-caps process and **panics** on mismatch.

### Layer 2 — in-handler checks (belt and braces)

Handlers with static rows keep a redundant in-handler check, and handlers
with *conditional* policy own their check entirely. The canonical shape:

```cpp
core::Process* proc = core::CurrentProcess();
if (proc == nullptr || !core::ProcessHasCap(proc, core::kCapFsRead))
{
    core::RecordSandboxDenial(core::kCapFsRead);
    frame->rax = /* ABI-shaped denial, e.g. 0xC0000022 STATUS_ACCESS_DENIED */;
    return;
}
```

Rules for this layer:

- Always pair the failed check with `core::RecordSandboxDenial(cap)` — the
  denial counter feeds the kill threshold (`kSandboxDenialKillThreshold`,
  100 denials = hostile retry loop, process killed) and the fix journal.
- Return the **ABI-shaped** error the guest expects: NTSTATUS
  `0xC0000022` for Win32, `-EPERM` / `-EACCES` for Linux. Never invent a new
  shape.
- Layer 1 and Layer 2 MUST agree. If you change one, change the other.

## Adding a new syscall — gating checklist

Syscall numbers are published ABI: **once assigned, forever** (CLAUDE.md
anti-bloat rule 7). Walk this list in order:

1. **Edit the IDL, not the generated files.** The source of truth is
   `abi/native_syscalls.json`. `kernel/syscall/cap_table.def` is GENERATED by
   `tools/build/gen-native-syscall-abi.py` — its header says "Do not edit
   this file by hand", and hand edits will be flagged as drift.
2. Decide the cap policy: unconditional cap → add `caps` in the JSON row so
   the generator emits a `kSyscallCapTable` row; conditional cap → leave the
   table empty for it and gate in the handler (document why in a comment).
3. Regenerate and verify:

   ```bash
   python3 tools/build/gen-native-syscall-abi.py            # writes generated files
   python3 tools/build/gen-native-syscall-abi.py --check    # CI-shape verify, no writes
   ```

4. Implement the handler with the Layer-2 shape above (ABI-shaped error +
   `RecordSandboxDenial`).
5. Wire it into the dispatch switch — an undispatched handler is dead code
   (see duetos-kernel-conventions / "Wiring Things In").
6. Prove **both legs**: an allow-leg with the cap and a deny-leg with
   `core::CapSetEmpty()` (see "Testing the deny leg" below).
7. Update `wiki/security/Capabilities.md` if you added a cap (its
   `<!-- AUTO:cap_list -->` block regenerates via `docs/sync-wiki.sh`).

## Worked examples — the pattern done right

Read these before writing a new gate; each is a live, verified instance:

| File | What it gates | Cap | Why it is right |
|------|---------------|-----|-----------------|
| `kernel/subsystems/win32/file_syscall.cpp` | Win32 file open/read path (~line 39) and NT set-info write path (~line 523) | kCapFsRead / kCapFsWrite | Checks cap FIRST, records denial, returns NTSTATUS `0xC0000022` — never a raw kernel error. |
| `kernel/subsystems/win32/registry.cpp` (~line 591) | Registry value set/delete | kCapFsWrite | Registry mutation is kernel-state change; gated on the SAME cap that protects the file it persists to — no parallel "registry privilege" invented. |
| `kernel/subsystems/linux/syscall.cpp` (~line 1300) | BSD socket family dispatch | kCapNet | Cap check before ANY socket work; denial returns `-EACCES`, the same shape Linux gives for a seccomp/LSM deny, distinguishable from "no socket layer". |
| `kernel/subsystems/linux/pidfd_splice.cpp` (~line 197) | `pidfd_getfd` cross-process fd theft | kCapDebug | Cross-process fd inspection is the same threat class as `PROCESS_VM_READ`, so it reuses the existing debug cap instead of adding a new one. |
| `kernel/subsystems/win32/apc_syscall.cpp`, `iocp_syscall.cpp`, `job_syscall.cpp` | Every thread-creating Win32 path | kCapSpawnThread | Same cap regardless of which API shape (APC, IOCP worker, job) requests the thread — one authority, many front-ends. |
| `kernel/subsystems/linux/syscall_clone.cpp` `DoFork` (~line 142) | fork/clone | kCapSpawnThread via `ProcessCaptureSpawnAuthority` | See "Spawn authority" below. |

## Spawn authority — leases never become durable child caps

Every spawn/fork/clone path calls
`core::ProcessCaptureSpawnAuthority(parent, required_mask, &child_caps,
&child_ceiling, &authority)` (`kernel/proc/process.h` ~line 1620,
implemented in `kernel/proc/process.cpp`). It atomically, under
`cap_lock`:

- verifies the parent's *effective* authority (durable + live leases) covers
  `required_mask` — a lease MAY authorize the spawn itself;
- emits `child_caps` from the parent's *durable* caps only — **a lease never
  becomes a durable child cap**;
- emits the inherited ceiling — the child can never be granted above it.

If you write a new spawn-shaped path (a Win32 `CreateProcess` variant, a
Linux `clone3` flag, a job-object respawn), route it through this helper.
Hand-rolling "copy `parent->caps` into the child" is the classic violation:
it silently promotes temporary lease authority into permanent child
authority. `kernel/subsystems/win32/spawn_syscall.cpp` (~lines 143, 341)
shows the correct call shape including the two-cap
(kCapFsRead | kCapSpawnThread) denial attribution.

## Token / privilege code — what Win32-shaped calls may do

Win32 token APIs are facades over kernel cap helpers
(`kernel/proc/process.h` ~lines 1594-1621). The legal moves:

| Intent | Helper | Reversible? |
|--------|--------|-------------|
| Read effective authority | `ProcessCapsSnapshot` / `ProcessHasCap` | n/a (read-only) |
| Disable a live bit (token disable) | `ProcessCapsDisableMask` | Yes — ceiling untouched |
| `SE_PRIVILEGE_REMOVED` / `SYS_DROPCAPS` | `ProcessCapsDropMask` | **No** — lowers the monotonic ceiling first, then clears bits |
| Enable a cap the process lacks | Elevation broker ONLY (`kernel/security/broker.h`) — installs a generation-tagged, positive-duration lease via `ProcessCapsGrantLease`, subject to role/password policy and the ceiling | Lease expires |

Everything else — integrity levels, ACL-shaped probes — **remains a
facade**: it may answer queries in the shape Win32 expects, but it gates
nothing and mutates nothing. Token code that writes `Process::caps`,
`cap_ceiling`, or `cap_leases` directly (instead of through the helpers) is
a rule-2 violation even if the observable behavior looks right.

## Repository audit checklist

Run this over any diff touching `kernel/subsystems/*` or `userland/libs/*`
(adapted from `wiki/kernel/Subsystem-Isolation.md`):

- [ ] Does any new state-mutating call skip the cap gate (neither a
      `cap_table.def` row nor an in-handler `ProcessHasCap`)?
- [ ] Does a userland DLL `#include` anything from `kernel/`?
- [ ] Does in-kernel subsystem code read or write anything not exported by
      `mm::*`, `sched::*`, `fs::routing::*`, `core::Cap*`?
- [ ] Does an ACL/integrity surface pretend to gate something, or does token
      code mutate capability storage directly instead of using the helpers +
      broker?
- [ ] Does a spawn path copy lease authority into the child (i.e. bypass
      `ProcessCaptureSpawnAuthority`)?
- [ ] Is a new parallel stack (TCP, VFS, registry, compositor) being
      introduced beside the existing kernel-owned one?

Any "yes" is a bug. Fix the underlying gate; do not extend the violation,
and do not "temporarily" whitelist the path.

## Testing the deny leg

An allow-leg test alone proves nothing about the gate.
`kernel/subsystems/linux/ring3_smoke.cpp` is the model: it spawns ring-3
processes with `core::CapSetEmpty()` (plus explicitly-added single caps) and
asserts both that permitted operations succeed AND that ungated operations
are refused. Denials it provokes land in the fix journal as `CapDenial`
rows (`kernel/diag/fix_journal.h`, category 7: ctx_a = syscall number,
ctx_b = pid, source_pin = `cap.<MissingCap>`).

When you add a gate, add or extend a smoke that exercises the deny leg with
an empty cap set. Boot-log verification and the QEMU harness are covered by
duetos-boot-smoke-and-qemu and duetos-testing-and-validation — this skill
only defines what MUST be tested, not how to run the rig.

## When NOT to use this skill

- Generic C++/kernel style, `Result<T,E>`, naming, lock discipline →
  **duetos-kernel-conventions**.
- Driver probe/enumeration wiring (PCI, NVMe, NIC) →
  **duetos-driver-architecture**; NIC-specific campaign state →
  **duetos-net-driver-campaign**.
- Running builds, boot smokes, or CTest → **duetos-build-and-env**,
  **duetos-boot-smoke-and-qemu**, **duetos-testing-and-validation**.
- Wiki page obligations after a slice lands → **duetos-docs-and-wiki**.

## Provenance and maintenance

Facts verified against worktree HEAD `8a55872c` on branch
`claude/fable-driver-wave-20260801`, 2026-08-13. Volatile numbers (line
numbers, "11 capability bits", ~30 cap-table rows) are as of that date.
Re-verify with:

- Doctrine text: `cat wiki/kernel/Subsystem-Isolation.md`
- Cap enum + helpers: `grep -n "kCap\|ProcessHasCap\|ProcessCaptureSpawnAuthority\|RecordSandboxDenial" kernel/proc/process.h`
- Gate + table: `sed -n '1,60p' kernel/syscall/cap_gate.h && cat kernel/syscall/cap_table.def`
- Generator drift check: `python3 tools/build/gen-native-syscall-abi.py --check`
- Live gate call sites: `git grep -n "ProcessHasCap" kernel/subsystems/`
- Cap-bit count: `grep -n "Capability bits" wiki/Home.md`
- Broker contract: `sed -n '1,40p' kernel/security/broker.h`
