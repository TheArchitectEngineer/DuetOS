---
name: duetos-driver-architecture
description: >-
  DuetOS driver-model contract: discovery, probe, wiring, teardown, restart, and the real-vs-shell driver inventory.
  TRIGGER when you need to "add a driver", touch "PCI probe" or PciEnumerate, debug a "driver won't bind" or
  "e1000 doesn't get interrupts" symptom, set up an "MMIO mapping", implement "driver teardown/restart" (driver
  domains, watch tasks), or wire "MSI-X" interrupts. DO NOT TRIGGER for extending nic_ids classification ("add a
  NIC id") or the wireless firmware bring-up campaign (use duetos-net-driver-campaign), for generic
  Result/klog/lock idioms (use duetos-kernel-conventions), or for QEMU boot-smoke mechanics (use
  duetos-boot-smoke-and-qemu).
---

# DuetOS driver architecture — the load-bearing contract

This skill is the architecture contract for the driver lane: how a PCI device
becomes a live driver, what a new driver MUST do to be probed / wired / torn
down / restarted, and which drivers are real vs diagnostic shells. It embeds
the invariants that were paid for with real bugs (wrong-register bring-up on
10G silicon, leaked MMIO mappings, zombie watch tasks).

**When NOT to use this skill:**

- Wireless firmware upload / rings / the Wi-Fi bring-up campaign → `duetos-net-driver-campaign`.
- Intel GPU display/GGTT/forcewake work → `duetos-intel-gpu-campaign`.
- `Result<T,E>`, KLOG levels, KBP probes, spinlock idioms → `duetos-kernel-conventions`.
- Building, QEMU flags, boot-log triage → `duetos-build-and-env`, `duetos-boot-smoke-and-qemu`.
- "Is this change allowed at all?" (hardware-write safety, TPM exclusions) → summary below, full doctrine in `duetos-subsystem-isolation` and `wiki/security/Hardware-Safety.md`.

Jargon used below, defined once:

- **TU** — translation unit, one `.cpp` file compiled on its own.
- **BAR** — PCI Base Address Register; tells you where a device's MMIO window lives.
- **MMIO** — memory-mapped I/O; device registers read/written as memory.
- **ECAM/MCFG** — PCIe memory-mapped config space, located via the ACPI MCFG table.
- **MSI-X** — per-vector message-signalled interrupts (vs legacy shared INTx pins).
- **Driver domain** — a named init/teardown pair registered with the kernel so the driver can be restarted without a reboot.
- **Watch task** — a 1 Hz kernel thread a driver spawns to poll device liveness.
- **Shell driver** — probe + telemetry + watch task exist, but no packet/data path yet.

## 1. Where drivers actually live (layout drift — read this first)

CLAUDE.md's "planned directory layout" shows a top-level `drivers/<class>/<driver>/`
tree. **That is aspirational and does not exist.** Every driver lives under
`kernel/drivers/<class>/` as flat files per class directory. Do not create the
top-level `drivers/` tree unilaterally — that promotion is a project-level
decision (a Design-Decisions entry), not a driver slice. Add new drivers next
to their class siblings.

Actual subdirectories and approximate size (`.cpp+.h+.rs` LOC, as of 2026-08-13):

| Dir | LOC | Contents |
|---|---|---|
| `kernel/drivers/video/` | ~29.6k | **GUI toolkit** (taskbar, widgets, TTF, console) — NOT hardware; only `framebuffer.cpp` touches the display |
| `kernel/drivers/gpu/` | ~13.4k | intel/amd/nvidia scaffolds, bochs_vbe, virtio_gpu, EDID/CEA-861/CVT parsers, modeset, telemetry |
| `kernel/drivers/usb/` | ~9.8k | xHCI (14 `xhci_*.cpp` TUs), MSC/SCSI, CDC-ECM, RNDIS, HID descriptor, btusb |
| `kernel/drivers/net/` | ~9.4k | net glue + e1000 (inside `net.cpp`), pcnet, wireless shells, `nic_ids.h`, firmware policy |
| `kernel/drivers/storage/` | ~4.8k | NVMe, AHCI, block layer |
| `kernel/drivers/virtio/` | ~3.6k | virtio-pci transport + blk/net/input/console/rng/balloon |
| `kernel/drivers/iommu/` | ~2.9k | VT-d (enforcing by default; `iommu=off` escape hatch — Design-Decision 2026-06-06), AMD IVRS |
| `kernel/drivers/audio/` | ~2.6k | Intel HDA, jack detection, PC speaker |
| `kernel/drivers/input/` | ~2.6k | PS/2 kbd/mouse, USB HID keyboard |
| `kernel/drivers/tpm/` | ~2.1k | TPM 2.0 TIS transport + PCR measurement (identity ops permanently excluded — `tpm.h`) |
| `kernel/drivers/pci/` | ~2.0k | the bus layer everything above sits on |
| `kernel/drivers/{mei,npu,psp,power}/` | small | telemetry-only probes |

## 2. Discovery: `PciEnumerate` and its cache

`core boot → duetos::drivers::pci::PciEnumerate()` (impl `kernel/drivers/pci/pci.cpp`,
decl `kernel/drivers/pci/pci.h`) walks bus 0 (device 0..31) with **recursive
descent through PCI-to-PCI bridges** (`EnumerateBus`/`EnumerateFunction`
follow each bridge's secondary bus), plus a sweep of every MCFG-declared bus
as a safety net when ECAM is live; functions 1..7 are probed only when the
multi-function header bit is set. A port-IO-only host stays bus-0-only
(logged: "no MCFG — using legacy port-IO (bus 0 only)"). Every present device
is cached in a fixed table of `kMaxDevices = 64`. Facts a driver
author must know:

- **Idempotent init-once**: a second `PciEnumerate()` without an intervening
  `PciTeardown()` returns immediately. `PciTeardown()` drops the cache so the walk
  can rerun (registered as the `"pci"` driver domain in `kernel/core/boot_bringup.cpp`).
- **Config access is real**: legacy `0xCF8/0xCFC` port pair PLUS ECAM via ACPI MCFG
  when present (the header's "no MCFG yet" scope-limits comment block is stale;
  `pci.cpp` prefers ECAM — see `EcamOffset` / the MCFG state at the top of the TU).
- Accessors: `PciDeviceCount()`, `PciDevice(i)` (panics out-of-range),
  `PciConfigRead{8,16,32}` / `PciConfigWrite32`, `PciFindCapability`,
  `PciFindExtCapability` (extended caps need ECAM; returns 0 on the legacy path).
- **BAR sizing is done for you**: `PciReadBar(addr, index)` size-probes
  non-destructively and returns decoded `{address, size, is_io, is_64bit,
  is_prefetchable}`. NEVER call it on a BAR a live driver is using — the probe
  briefly writes all-1s.

## 3. There is NO bus/driver match table

DuetOS has no `struct pci_driver` + id-table registration like Linux. **Each class
driver walks the PCI cache itself** at its init: filter by `class_code`, then
classify vendor/device IDs. `NetInit()` in `kernel/drivers/net/net.cpp` is the
canonical pattern to copy:

```text
NetInit():
  for each PciDevice(i) with class_code == kPciClassNetwork:
    fill NicInfo from the cached Device
    bar0 = PciReadBar(addr, 0)
    if bar0 valid MMIO: map_bytes = min(bar0.size, 2 MiB); mmio_virt = mm::MapMmio(...)
    if (!RunVendorProbe(nic, g_nic_count)):
        mm::UnmapMmio(mmio_virt, map_bytes)          // MUST unwind
        KLOG_WARN_V(...); KBP_PROBE_V(kProbeFail, did)
        continue                                     // no registry add
    g_nics[g_nic_count++] = nic                      // record lands in stable table
    if driver_online && wireless: start the watch task // ONLY after the copy
```

**The probe contract** (`RunVendorProbe(NicInfo&, iface_index)` in `net.cpp`):

- Returns **false** = no vendor matched, no driver code touched the device. The
  caller then unmaps the MMIO and skips the registry add — keeping the entry
  would leak a 2 MiB MMIO mapping per unrecognised controller for the life of
  the boot (the arena never reclaims; see §4).
- Returns **true** even for matched-but-not-brought-up devices — they stay in
  the registry so device manager lists them as `(probe only)`.
- Serial sentinel per device: `[net-probe] vid=... did=... family=...` suffixed
  `(driver online)` / `(driver shell online — firmware pending)` / `(probe only — no packet I/O)`.
  Boot-smoke greps match these as single substrings (hence `SerialLineGuard`).

## 4. MMIO mapping rules (the bump-allocator trap)

The MMIO virtual arena is a **monotonic bump allocator** — `mm::UnmapMmio` tears
down PTEs but **never recycles the virtual range** (Design-Decision 007,
`wiki/reference/Design-Decisions.md`; restated in `pci.h` and `net.h` comments).
Consequences that are load-bearing for driver code:

1. **Bound your mappings.** NetInit caps NIC BAR0 maps at 2 MiB (register files
   are <256 KiB; giant BARs on HPC NICs are RDMA doorbells no v0 driver touches).
   Copy that cap pattern for any device class with large BARs.
2. **Probe/teardown loops must be bounded.** Every map in a retry or restart
   cycle burns arena VA forever. `PciTeardown` explicitly documents that its ECAM
   aperture leaks on re-enumerate; a driver-domain restart that re-maps per cycle
   is acceptable only because restarts are rare and operator-driven.
3. Still **unmap on the no-match path** — it frees the physical PTE wiring even
   though the VA is not recycled, and it keeps the contract greppable.

## 5. Interrupts: MSI-X first, polling fallback is mandatory

`pci.h` exposes the full MSI-X toolkit; the one to reach for is the one-shot
helper:

```cpp
// Allocates a vector, installs `handler`, maps the table, programs entry 0
// routed to the BSP LAPIC. Returns the vector, or Err.
auto r = pci::PciMsixBindSimple(addr, /*entry_index=*/0, handler, /*out_route=*/nullptr);
```

Error modes: `Unsupported` (no MSI-X capability), `IoError` (table BAR
missing/IO-space), `OutOfMemory` (vector pool or MapMmio). **The contract:
`Unsupported` is a normal outcome and every driver must degrade to polling,
not fail bring-up.** e1000 is the reference (`E1000BringUp` in `net.cpp`,
around `PciMsixBindSimple` call): on success it programs IVAR + IMS and logs
`[e1000] MSI-X bound vector=...`; on failure it logs
`[e1000] MSI-X unavailable — RX task will tick-poll` and the RX task polls.
The same code path succeeds on e1000e (has MSI-X) and falls back on classic
e1000 (doesn't) — runtime capability detection, not an ID whitelist.

Handler shape gotcha: `arch::IrqHandler` is `void(*)()` with no context
argument — e1000 uses a per-slot thunk table (`kE1000SlotHandlers[slot_idx]`)
so each controller instance wakes its own wait queue. Multi-instance drivers
must do the same.

Lower-level pieces if you need them: `PciMsixFind`, `PciMsixRouteSimple`
(returns a `MsixRoute` with mapped table base for later mask/unmask),
`PciMsixSetEntry` / `PciMsixMaskEntry` (bounds-checked — wrong `table_size`
is a panic, never a silent OOB MMIO write), `PciMsixFunctionMask` for
quiescing at reset.

## 6. Boot wiring: imperative order + initcall registry

Boot bring-up is an imperative sequence in `kernel/core/boot_bringup.cpp`
(deliberately — "getting boot order wrong is a triple-fault, not a unit-test
failure", per `kernel/core/init.h`). Driver-relevant order, verified in the TU:

```text
Ps2KeyboardInit → Ps2MouseInit → pci::PciEnumerate → gpu::GpuInit →
net::NetInit → usb::UsbInit → xhci::XhciInit → audio::AudioInit →
storage::NvmeInit → storage::AhciInit
```

Adjacent to that sits the initcall registry (`kernel/core/init.h`):
`KERNEL_INITCALL(Phase, "label", fn)` at file scope stamps a constructor thunk
that `RunInitArray()` (called after `KernelHeapInit`) forwards into
`InitcallAutoRegister`. `RunPhase(Phase::Drivers)` invokes registered callbacks
in registration order and **stops at the first `Err`** (caller decides whether
to panic). Registry capacity `kMaxInitcalls = 64`; registration is
boot-single-threaded only. Note: `KERNEL_INITCALL` registers the callback —
someone still has to `RunPhase` the phase; today drivers use the macro mainly
to register their driver domain (next section), while their init function is
called imperatively.

## 7. Teardown and restart: driver domains + generation-scoped watchers

**Every restartable driver registers a fault domain**
(`kernel/security/driver_domain.h`):

```cpp
security::RegisterDriverDomain("drivers/net",
    []() -> Result<void> { NetInit(); return {}; },
    []() -> Result<void> { return NetShutdown(); });
```

Wired via `KERNEL_INITCALL(Drivers, "drivers/net.module", RegisterNetModule)`
at the bottom of `net.cpp`. Operators kick it from the kernel shell with
`DOMAIN RESTART <name>` (admin-gated; `CmdDomainRestart` in
`kernel/shell/shell_debug.cpp` → `RestartDriverDomain`). Current registrants
that matter for drivers: `"drivers/net"` (net.cpp), `"drivers/gpu"` (gpu.cpp),
`"drivers/audio"` (audio.cpp), `"nvme"` (nvme.cpp), `"drivers/usb/xhci"` and
`"pci"` and `"ahci"` (boot_bringup.cpp), plus fs domains (`fs/fat32`, `ramfs`).

**The generation-scoped watcher pattern** — copy this for ANY restartable
driver that spawns a polling task, or you will accumulate zombie pollers
across restarts:

1. Module-scope `constinit u32 g_module_generation = 0;` (`net.cpp`) with a
   read accessor `NetModuleGeneration()` (`net.h`).
2. `NetShutdown()` **bumps the generation FIRST**, before quiesce/record
   teardown — quoting the in-tree comment: the bump "is their exit signal, and
   it must precede the record teardown so no watcher observes a half-cleared
   table as 'still mine'".
3. Each watch task snapshots the generation at spawn and exits when it changes
   (`IwlwifiWatchEntry` in `iwlwifi.cpp` is the reference; rtl88xx / bcm43xx /
   mt76 mirror it). Poll cadence: ~1 Hz via `sched::SchedSleepTicks(100)` on
   the 100 Hz tick.
4. Watchers are started **only after** the NIC record is copied into the
   stable global table (`IwlwifiStartWatch(g_nics[nic_index])` etc. in `NetInit`) —
   never inside probe, because probe works on a stack-local record whose
   address dies.

Known bounded race (documented `// GAP:` near the top of `net.cpp`): a watch
task that hasn't had its first run when `NetShutdown` fires captures the NEW
generation and survives one extra cycle — bounded to one extra poller per NIC
per restart. Don't "fix" it casually; revisit only if domain restart becomes
hot-path.

**Quiesce discipline** (reference: `E1000QuiesceOne` in `net.cpp`): mask IRQs
(IMC all-ones) + drain ICR + clear IVAR → disable RX/TX (RCTL/TCTL = 0) →
software reset → free DMA rings/buffers → wake sleepers BEFORE zeroing the
context → clear `online` so racing TX/RX paths bail before touching freed
pointers. The MSI-X handler stays installed; device-side masking stops events.

## 8. Device-ID classification: one header, ordering is load-bearing

`kernel/drivers/net/nic_ids.h` (freestanding, all `constexpr`, host-tested by
`tests/host/test_nic_ids.cpp`) is the **single source of truth** for NIC
PCI-ID → family classification. It exists because two parallel whitelists
(family tags in `net.cpp` + per-driver `*Matches` predicates) drifted, and the
old coarse Intel ID ranges **mis-dispatched real hardware** — 10/40G silicon
classified as "e1000e" and received a full e1000 register bring-up against the
wrong register file (fixed in commit `7d2b4271`). The two structural rules to
preserve when touching it:

- **Predicate ordering in `IntelWiredFamilyFromDeviceId` is load-bearing** —
  specific families (igb → igc → ixgbe → i40e) are tested before the coarse
  classic-e1000/e1000e ranges because their IDs interleave in the same
  0x10xx/0x15xx space. Never reorder; never replace an explicit table with a
  range.
- **`IntelE1000BringUpEligible(did)` stays narrow** (true only for
  `E1000Classic`/`E1000e`) — the safe failure mode is "no driver", never
  "wrong-register writes".

The full add-an-ID checklist (evidence/sourcing rule, wireless prefix
coverage, exhaustive sweep tests, boot-smoke `family=` expectations, the
MediaTek split) is owned by **`duetos-net-driver-campaign` Phase 1** — use it
for ANY `nic_ids.h` change; do not re-derive it here.

## 9. Driver inventory — REAL vs SHELL (honest state, 2026-08-13)

"REAL" = data path works (packets/blocks/frames/input events flow).
"SHELL" = probe + telemetry + watch task, no data path.

| Driver | Files | State |
|---|---|---|
| e1000 / e1000e | inside `net/net.cpp` (no own file) | REAL — reset, rings, TX/RX, MSI-X-or-poll, DHCP-capable |
| AMD PCnet | `net/pcnet.cpp` | REAL — polled RX/TX + DHCP (VirtualBox default NIC) |
| virtio net/blk/input/console/rng | `virtio/` | REAL (over virtio-pci transport) |
| NVMe | `storage/nvme.cpp` | REAL; registered driver domain `"nvme"` |
| AHCI | `storage/ahci.cpp` | REAL; domain `"ahci"` |
| xHCI + USB classes | `usb/xhci*.cpp` (14 TUs), `msc_scsi`, `cdc_ecm`, `rndis`, HID | REAL |
| Intel HDA | `audio/hda.cpp` | REAL playback (capture arm-able — see GAP markers) |
| PS/2 kbd/mouse, USB HID keyboard | `input/` | REAL |
| Bochs VBE, virtio-gpu | `gpu/bochs_vbe.cpp`, `gpu/virtio_gpu.cpp` | REAL framebuffer |
| EDID / CEA-861 / CVT parsers | `gpu/edid.cpp` etc. | REAL (with self-tests) |
| iwlwifi | `net/iwlwifi*.{cpp,h}` | SHELL — rings/upload state machine written but "ships untested on the dev host (no QEMU emulation of iwlwifi)" (`iwlwifi_upload.h`); heavy diag logging is the stated core feature |
| rtl88xx, bcm43xx, ath9k_htc | `net/` | SHELL — probe + fw parsing/policy + watch |
| mt76 (MT7921/22/25) | `net/mt76.{cpp,h}` | SHELL — bring-up = read `MT_HW_BOUND` (BAR0+0x0008), reject all-ones/0; DMA rings, MCU firmware, 802.11 mgmt, WED explicitly out-of-scope per `mt76.h` |
| intel_gpu / amd_gpu / nvidia_gpu | `gpu/` | scaffold/v0 — telemetry, forcewake, GGTT, fw parsing; no acceleration. nvidia is diagnostics-only (GSP boot absent) |
| mei, npu, psp, power, tpm | own dirs | probe/telemetry tiers; TPM is TIS-transport + PCR measure only |

Do NOT trust a stale handoff that mentions `mt7921_contract.{h,cpp}` — those
files were never committed and do not exist on this branch.

## 10. Firmware and hardware-safety gates

- **Firmware policy** (`kernel/drivers/net/firmware_policy.{h,cpp}`): a
  deterministic classification matrix — `FirmwareSourceKind` {OpenSource,
  RedistributableBinary, ExtractedVendorBinary, PatchFramework} ×
  `FirmwareDisposition` {Preferred, RuntimePackage, ResearchOnly, Reject}.
  Hard rules: never commit closed blobs to the tree; runtime packages must be
  hash-pinned; closed-redistributable must carry the upstream license notice.
  Request path: `kernel/loader/firmware_loader.h` (+ `firmware_package.*`).
  Rust firmware parsers live beside their drivers: `iwlwifi_fw_rust/`,
  `rtl88xx_fw_rust/`, `bcm43xx_fw_rust/`, plus GPU equivalents.
- **Hardware-safety contract** (`wiki/security/Hardware-Safety.md`): a living
  page with (a) a "current posture" audit table (thermal/RAPL/UEFI-NVRAM etc.
  are read-only) and (b) a **pre-landing preconditions table** — every risky
  controller (SPI flash, EC writes, UEFI capsule, ...) must land its safety
  gate **in the same slice** as the driver, severity-ranked up to BRICK.
  Before writing any register that configures/persists (not just device
  bring-up state), check that table.
- **TPM**: identity/attestation operations are **permanently excluded** by the
  allow-list contract in `kernel/drivers/tpm/tpm.h` — do not add them.
- **IOMMU**: VT-d is enforcing by default; `iommu=off` is the cmdline escape
  hatch. DMA-capable drivers get their mappings through the kernel; see
  `kernel/mm/dma.h` for the sync contract (currently x86-only — the ARM64
  cache-maintenance half is a documented GAP in `dma.cpp`).

## 11. Checklist: adding a new PCI driver

Work through every row; each cites the reference implementation to copy.

| # | Step | Reference |
|---|---|---|
| 1 | **Enumerate**: filter `PciDevice(i)` by `class_code` (and subclass/prog_if) in your class's init function | `NetInit` in `net.cpp` |
| 2 | **Classify**: explicit, evidence-cited device-ID sets in a freestanding constexpr header; host-test it | `nic_ids.h` + `tests/host/test_nic_ids.cpp` |
| 3 | **Map MMIO bounded**: `PciReadBar` → cap map size → `mm::MapMmio`; remember the arena never reclaims VA | §4; `NetInit` 2 MiB cap |
| 4 | **Probe contract**: no-match ⇒ return false, caller unmaps + `KLOG_WARN_V` + `KBP_PROBE_V(kProbeFail, did)` + no registry add; matched-not-brought-up stays listed as probe-only | `RunVendorProbe` |
| 5 | **Interrupts**: try `PciMsixBindSimple`; treat `Unsupported` as normal and fall back to tick-polling; per-slot handler thunks for multi-instance | e1000 in `net.cpp` |
| 6 | **Wire it in**: call your init from the imperative sequence in `boot_bringup.cpp` at the right point in the order (§6). An unprobed driver is dead code — wire it or delete it | CLAUDE.md "Wiring Things In" |
| 7 | **Driver domain**: `RegisterDriverDomain("drivers/<name>", init, teardown)` via `KERNEL_INITCALL(Drivers, "<name>.module", ...)`; teardown must fully quiesce (mask IRQs → stop DMA → free rings → wake sleepers → clear online) | `RegisterNetModule` / `E1000QuiesceOne` |
| 8 | **Watcher pattern** (if you spawn a poller): snapshot module generation at spawn, exit on change, start only after the record is in the stable table, ~1 Hz `SchedSleepTicks(100)` | §7; `IwlwifiWatchEntry` |
| 9 | **Selftest sentinel**: emit a greppable single-line serial sentinel per device (`SerialLineGuard` if multi-part) so boot-smoke can gate on it; silent-pass rules per CLAUDE.md | `[net-probe] ...` lines |
| 10 | **Safety gate**: if the driver writes anything that persists or can damage hardware, land the gate from `wiki/security/Hardware-Safety.md`'s precondition table in the same slice | §10 |
| 11 | **STUB/GAP markers** on deliberate omissions; **wiki page** under `wiki/drivers/` (new driver class) or amend the existing page; roadmap row deleted if you landed one | CLAUDE.md Definition of Done |

## 12. Known driver-lane GAPs (open — verify before relying)

~25 `// STUB:`/`// GAP:` markers live under `kernel/drivers/` (re-derive:
`git grep -nE "// (STUB|GAP):" kernel/drivers`). The load-bearing ones:

- `net.cpp` (near `g_module_generation`): watcher/shutdown race, one extra poller per restart, bounded.
- `net.cpp` (`kMaxE1000 = 4`): a fifth e1000 adapter gets no context — probe-only.
- `net.cpp` (~line 790): no multi-NIC routing policy (no bonding/source-routing).
- `kernel/mm/dma.cpp`: ARM64 cache maintenance missing — all DMA drivers are x86-only.
- `gpu/nvidia_gpu.cpp`: GSP boot (WPR/FRTS/RISC-V release) absent — diagnostics only.
- `gpu/intel_gsc_fw.cpp`: CPD/SHA-256 hash-chain verification of GSC firmware partitions missing.
- `tpm/tpm.cpp`: TIS start method only; CRB/ACPI-Start unhandled.
- `virtio/virtio.cpp`: per-class probes for some device types are `// STUB:`.
- `net/iwlwifi_rings.cpp`: legacy (<7000-series) 32-bit RBD format unhandled.

## Provenance and maintenance

Authored 2026-08-13 against branch `claude/fable-driver-wave-20260801`, HEAD
`8a55872c`. Every path/symbol above was Read/Grep-verified on that tree. Line
numbers cited are approximate as of 2026-08-13; prefer symbol search.

Re-verification one-liners (run from the repo root):

```bash
git grep -n "RunVendorProbe" kernel/drivers/net/net.cpp            # probe contract still there
git grep -n "IntelE1000BringUpEligible" kernel/drivers/net/nic_ids.h kernel/drivers/net/net.cpp
git grep -n "PciMsixBindSimple" kernel/drivers/pci/pci.h kernel/drivers/net/net.cpp
git grep -n "RegisterDriverDomain(" kernel | grep -v "\.h:"        # current domain registrants
git grep -n "g_module_generation\|NetModuleGeneration" kernel/drivers/net/
git grep -n "007 — MMIO arena" wiki/reference/Design-Decisions.md  # bump-arena decision still stands
git grep -nE "// (STUB|GAP):" kernel/drivers | wc -l               # GAP inventory drift check
grep -n "PciEnumerate\|NetInit\|XhciInit\|NvmeInit\|AhciInit" kernel/core/boot_bringup.cpp | head
ls kernel/drivers                                                   # layout drift check (still no top-level drivers/)
```

If any of these come back empty or moved, update the matching section rather
than trusting this document.
