---
name: duetos-intel-gpu-campaign
description: >-
  Executable campaign runbook for DuetOS Intel iGPU (Gen9-Gen12) command submission — the Roadmap's five ordered
  slices from forcewake to modeset. TRIGGER when the task mentions "Intel GPU", "iGPU bring-up", "GGTT", "forcewake",
  "batch buffer", "BLT command", "XY_COLOR_BLT", "modeset", "GMBUS EDID", or "GPU command submission" on real Intel
  silicon. DO NOT TRIGGER for the general driver model or PCI/BAR plumbing (use duetos-driver-architecture), for
  virtio-gpu / Bochs VBE / QEMU display work (that is the working QEMU path, not this campaign), or for AMD CP /
  NVIDIA GSP bring-up (separate scaffolds, explicitly out of this campaign's scope).
---

# DuetOS Intel iGPU campaign — forcewake to modeset

This is the runbook for the Roadmap item **"Intel iGPU command submission (GGTT batch + 2D BLT)"**
(`wiki/reference/Roadmap.md`, section around line 611). It is the project's second "hardest live problem"
campaign. The defining constraint: **QEMU has no Intel iGPU model**, so every MMIO path here is
real-hardware-only, while every pure encoder is pinned by host tests and QEMU-run boot self-tests.

**Do not use this skill for:** virtio-gpu or Bochs VBE (the *working* QEMU display path — regressing it is
this campaign's cardinal sin), AMD (`amd_gpu.cpp`, CP/PM4) or NVIDIA (`nvidia_gpu.cpp`, GSP) bring-up, or
generic driver questions (→ `duetos-driver-architecture`). Build/toolchain issues → `duetos-build-and-env`;
boot smoke mechanics → `duetos-boot-smoke-and-qemu`.

## Vocabulary (each term defined once)

| Term | Meaning |
|------|---------|
| RCS | Render Command Streamer — the render engine's command ring, registers at BAR0+0x2000 |
| BCS | Blitter Command Streamer — 2D blit engine ring at 0x22000 (not yet used; BLT currently goes via RCS) |
| Forcewake | MMIO handshake that keeps a power-gated GT register domain awake; without it real-HW reads return garbage and writes are dropped |
| GGTT | Global Graphics Translation Table — the GPU's flat page table, aliased into the **upper half of BAR0**; maps host-phys pages to GPU virtual addresses |
| PTE | 64-bit GGTT page-table entry: `(phys & ~0xFFF) | present-bit` |
| Batch buffer | A GGTT-mapped page of commands executed indirectly via `MI_BATCH_BUFFER_START`, ending in `MI_BATCH_BUFFER_END` |
| Breadcrumb | A `PIPE_CONTROL` post-sync seqno write to a GGTT address that the CPU polls for completion |
| BLT | The 2D blit commands `XY_COLOR_BLT` (solid fill, ROP 0xF0) and `XY_SRC_COPY_BLT` (copy, ROP 0xCC) |
| GMBUS | Intel's I2C-over-DDC controller (PCH, regs at 0xC5100) used to read a monitor's EDID |
| Masked-bit register | Intel register where the upper 16 bits are a write-enable mask: writing `(bit<<16)|bit` sets, `(bit<<16)` clears |

## Current state (verified at HEAD 8a55872c, 2026-08-13)

All five Roadmap slices have **code in tree**, but the Roadmap section still stands because the MMIO
paths are **unverified on silicon** — that validation, plus the residuals below, is the live campaign.

| Unit | File (kernel/drivers/gpu/) | Lines | Status |
|------|---------------------------|-------|--------|
| Discovery + vendor dispatch | `gpu.cpp` | 651 | Real. `GpuInit` → `RunVendorProbe`; calls `intel::Bringup` only when `g.mmio_live` |
| Driver core | `intel_gpu.{h,cpp}` | 211+883 | Ring bring-up, store-imm probe, BLT fill submission — coded, silicon-unverified |
| Slice 1: forcewake | `intel_forcewake.{h,cpp}` | 97+128 | Coded. RENDER 0xA278/0x0D84 + GT 0xA188/0x130044, Gen9-11 fallback-ack erratum, `IntelRingUnstop` |
| Slice 2: GGTT | `intel_ggtt.{h,cpp}` | 69+125 | Coded. `EncodeGgttPte`, high-window scratch fill, `GgttMapPage` |
| Slice 3: batch+breadcrumb | `intel_gpu_cmds.{h,cpp}` | 177+78 | Coded. Encoders constexpr + static_assert; `IntelBatchExecProbe` dispatch |
| Slice 4: 2D BLT | `intel_gpu_cmds.h` + `intel_gpu.cpp` | — | Coded. `IntelBltColorFillProbe` (offscreen rung), `IntelBltColorFill` wired to compositor `FillRect`; `BitBlt`/BCS-ring open |
| Slice 5: display detect | `intel_display.{h,cpp}` | 62+116 | Coded. GMBUS EDID read + `IntelDisplayProbe`. SDEISR/HPD connector detect + plane reprogram NOT in tree — open |
| GSC firmware parser | `intel_gsc_fw.{h,cpp}` | 172+512 | Diagnostic-only. GAP at `intel_gsc_fw.cpp:266`: per-partition CPD/SHA-256 hash-chain verification missing (firmware trust gap) |
| Telemetry | `gpu_telemetry.{h,cpp}` | 67+104 | Read-only. See "untrustworthy signals" below |

Boot order fact (`kernel/core/boot_bringup.cpp`): `PciEnumerate` (line ~2211) → `VirtioInit` →
`GpuInit` (~2236) → Intel self-tests (`IntelForcewakeSelfTest`, `IntelGgttSelfTest`,
`IntelGpuCmdsSelfTest`, `IntelDisplaySelfTest`, `IntelRcsRingSelfTest` at lines 2246-2250) →
`NetInit` (~2335). Anything you break in `GpuInit` breaks the boot before networking exists.

## The verification ladder (what proves what)

Run bottom-up; each rung is cheap and each catches a different failure class.

1. **Host tests (encoders + geometry gates)** — milliseconds, run on every change:
   ```bash
   cmake -S tests/host -B build/host-tests
   cmake --build build/host-tests
   ctest --test-dir build/host-tests --output-on-failure -R "intel_blt|cvt|render_stats"
   ```
   `tests/host/test_intel_blt.cpp` pins `IsBltSurfaceGeometryValid` / `IsBltRectValid` rejection
   boundaries and the exact `EncodeColorBlt` DWORDs (dw0=0x54300005, dw1=0x03F00A00, ...).
   `test_cvt.cpp` covers CVT mode math; EDID/CEA-861 have on-target selftests.
2. **Constexpr pinning** — the MI/BLT/GMBUS/PTE encoders are `constexpr` with `static_assert`s in
   the headers: a wrong opcode fails the *compile*. Extend this pattern for any new encoder.
3. **QEMU boot smoke** — all five `[gpu/intel/*] selftest PASS` sentinels must appear (they are
   device-independent), the RCS selftest must print `[gpu/intel/rcs] no Intel device — skipped`,
   and the **virtio-gpu path must still produce pixels**. Triage with
   `tools/test/boot-log-analyze.sh <log>`.
4. **Full-link caveat (2026-08-13):** this worktree's full kernel link fails on a missing
   `loader/load_plan.h` for reasons unrelated to GPU work — a stale-branch artifact.
   **Rebase on `origin/main` first** (see `duetos-change-control`); per-TU syntax checks of
   the `intel_*` TUs work regardless.
5. **Real hardware** — the only rung that proves MMIO. Target: a Gen9 Skylake/Kaby-Lake NUC
   (no Optimus/hybrid mux) + serial UART capture. The non-destructive proof ladder
   (GPU-Implementation-Notes §6): liveness reads → store-imm cookie → batch-dispatched store →
   offscreen BLT with pixel read-back → only then the live framebuffer.

**Never report rungs 1-3 as "the driver works".** They prove encodings and gating, not execution.
The honest phrasing used throughout the tree is "coded, unverified on silicon" — keep it.

## Safety gate — before any slice that writes real GPU registers

Design-Decision 2026-06-06 ("Hardware telemetry readers ... read-only") set the then-current
boundary: GPU access was read-only telemetry. This campaign **deliberately crosses that boundary**
into volatile register writes. Procedurally that means, per `wiki/security/Hardware-Safety.md`:

- **Current-posture row "GPU / display / NIC / audio: Volatile-only"** binds you: no clock, voltage,
  fan, VBIOS/EEPROM, or power-limit writes, ever. Ring/forcewake/GGTT/BLT writes are volatile GT
  state and are inside the allowed envelope.
- **Pre-landing precondition rows** that bind specific slices *before they land*:
  - *Display modeset / PLL programming* (PHYS-DMG): drive only EDID-advertised modes; if EDID is
    absent/invalid, fall back to 640x480@60 / 1024x768@60, never a guessed high-rate mode.
  - *GPU clock / voltage tables* (PHYS-DMG): ship stock clocks only. RC6-off for v0 is fine
    (it is a power-management *disable*, not a clock/voltage write).
  - *GPU VBIOS / EEPROM flashing* (BRICK): no code path writes the card's ROM. The GSC parser
    stays diagnostic-only until an MEI driver slice with its own gate.
  - *DMA without IOMMU* (DATA-LOSS, any new bus-master driver): GGTT-mapped batch/scratch
    pages ARE GPU bus-master DMA targets. Binding precondition: enable + enforce the IOMMU
    before bus-master DMA, map only driver-owned pages, and validate descriptor/PTE targets
    (the low-slot warnings below are the campaign-local form of this rule — a bad GGTT PTE
    scribbles firmware or other-owner memory).
- The gate ships **in the same slice** as the writer, not as a follow-up. "Just one register"
  does not skip the table — that rationalization is a named wrong path in this campaign.
- Route the decision through `duetos-change-control` (Roadmap edit + Design-Decisions append in
  the same commit that lands a slice).

## Phase 0 — orient and claim (every session)

1. Git-sync (rebase on `origin/main`) — mandatory, and it also resolves the load_plan caveat.
2. If parallel sessions are possible: `tools/parallel/status.sh` then
   `tools/parallel/claim.sh gpu-intel "kernel/drivers/gpu/intel_*" "<desc>"`.
3. Read the Roadmap section (`wiki/reference/Roadmap.md` ~L611) — if it has changed or been
   deleted since 2026-08-13, the tree has moved; re-derive state before trusting this file's
   "current state" table.
4. Read `wiki/reference/GPU-Implementation-Notes.md` §Intel — the register-level prior art
   (i915-corroborated offsets, errata, proof ladder). It is the campaign's spec.
5. `docs/handoff/gpu-driver-next-session.md` has useful vendor-comparison context but is
   **stale where it matters**: it claims `Bringup` "frees the buffer and returns Unsupported"
   (it no longer does — real ring programming landed) and references a dead branch. Mine it
   for background only; trust the code and the wiki over it.

## Phase 1 — forcewake + GT-init (Roadmap slice 1)

Roadmap text: *"Forcewake + GT-init — hold RENDER+GT domains (Gen9 set/ack `0xA278`/`0x0D84` +
`0xA188`/`0x130044`) with the Gen9–11 fallback-ack erratum, RC6 off, un-stop the ring via
`RING_MI_MODE`."*

- **Entry criteria:** none — this is the root slice. Code exists; the live work is silicon
  validation and any fix it forces.
- **Files/symbols:** `intel_forcewake.{h,cpp}` — `ForcewakeGet`, `ForcewakeGetForRing`,
  `IntelRingUnstop`, `MaskedBitEnable/Disable`, domains `kFwRender`/`kFwGt`/`kFwMedia`.
  Called from `intel::Bringup` (`intel_gpu.cpp:371-372`) before any ring register write.
  Note: `Bringup` deliberately does **not** early-return on a forcewake-ack miss — the HEAD
  poll reports the failure uniformly.
- **Expected on real HW (serial):** `[gpu/intel] gen_info=... fuse_strap=... gfx_mode=...
  pwr_well_ctl2=...` from `Probe`, then `[gpu/intel] rcs_ring_phys=...`, then
  `[gpu/intel/rcs] ring online head=tail=00000100 ctl=... phys=...`.
- **Expected on QEMU:** `[gpu/intel/fw] selftest PASS (...)` (pure encoder test) and
  `[gpu/intel/rcs] no Intel device — skipped`. Nothing else — the vendor case never enters.
- **Failure branches:**
  - `KLOG_WARN "RCS ring head never caught tail (head)"` + probe `kGpuRingBringupFail` →
    forcewake never acked, ring still stopped, or RCS_START rejected. Check the DEBUG lines
    (final tail/ctl/ring phys, `KLOG_DEBUG_V`-gated — boot with debug loglevel). First
    suspects in order: fallback-ack erratum path not taken, RC6 put the GT back to sleep
    (RC6-off write is the Roadmap's ask — verify it actually landed before blaming the ring),
    HEAD-reset-until-it-sticks (HSW+ erratum, GPU-Implementation-Notes §1) not applied.
  - `Probe` prints `BAR0[0]=0xFFFFFFFF — MMIO decode failed` → the device isn't decoding;
    this is a PCI/BAR problem, not forcewake. Switch to `duetos-driver-architecture`.
  - Ack reads all-ones → you're past `g.mmio_size`; `IntelReg32` returns the dead-decode
    sentinel for out-of-bounds offsets. Check the BAR0 map size before the register offset.

## Phase 2 — GGTT manager (Roadmap slice 2)

Roadmap text: *"GGTT manager — encode 64-bit PTEs (`phys | present`, LM=0), write through the
BAR0 GTTMMADR upper-half alias, scratch-fill all slots, allocate GPU-VA above the GMADR
aperture."* (The code refines "scratch-fill all slots" to **high-window only** — the low slots
hold the firmware framebuffer; filling them would kill the live screen. The header comment in
`intel_ggtt.h` documents this deliberate deviation. Do not "fix" it back to the Roadmap wording.)

- **Entry criteria:** Phase 1 acks on the target part (a sleeping GT ignores PTE writes too).
- **Files/symbols:** `intel_ggtt.{h,cpp}` — `EncodeGgttPte` (constexpr, static_assert-pinned),
  `GgttInit` (PTEs at BAR0 + `mmio_size/2`), `GgttMapPage`, `GgttReady`. `GgttInit` is invoked
  from `Bringup` (`intel_gpu.cpp:376`); every gated dispatch checks `GgttReady()`.
- **Expected:** QEMU — `[gpu/intel/ggtt] selftest PASS`. Real HW — `GgttInit` returns a nonzero
  slot count and, critically, **the firmware-lit panel keeps displaying** after init.
- **Failure branches:**
  - Screen goes black/garbage right after GGTT init → you scribbled the low (firmware
    framebuffer) slots. This is the lost-slot collision class from CLAUDE.md. Verify the
    window base is above the GMADR (BAR2) aperture top.
  - PTE writes read back stale → missing posting-read after the write; the GGTT alias should
    be write-combining and needs a read-back to flush (GPU-Implementation-Notes §2).
  - `GgttMapPage` returns 0 → not initialised / window exhausted / unaligned phys — the three
    documented reasons; check in that order.

## Phase 3 — batch submission + breadcrumb (Roadmap slice 3)

Roadmap text: *"Batch submission + breadcrumb — `MI_BATCH_BUFFER_START` (full 48-bit lo/hi
addr) from a GGTT batch, `wmb` before the `RING_TAIL` doorbell, PIPE_CONTROL post-sync seqno +
poll."*

- **Entry criteria:** Phase 2's `GgttReady()` true on the target part; `IsBroughtUp()` true.
- **Files/symbols:** `intel_gpu_cmds.{h,cpp}` — `EncodeBatchBufferStart` (bit 8 **clear** =
  GGTT address space), `EncodePipeControlQwWrite` (`QW_WRITE|GLOBAL_GTT_IVB|CS_STALL`),
  `kMiBatchBufferEnd`; dispatch `IntelBatchExecProbe` lives in `intel_gpu.cpp` (~line 568,
  where the ring state is).
- **Expected:** QEMU — `[gpu/intel/cmds] selftest PASS`. Real HW —
  `[gpu/intel/cmds] batch-exec PASS (GGTT MI_BATCH_BUFFER_START + store, ...)`.
- **Failure branches:**
  - `[gpu/intel/cmds] batch-exec readback=FFFFFFFF (GGTT batch dispatch unverified on this
    part)` → the probe ran and failed; that line is the *designed* honest output, not noise.
  - `KLOG_WARN "RCS batch-exec: HEAD did not catch TAIL"` → engine hung fetching the batch:
    batch GPU-VA not actually mapped (re-check Phase 2), odd DWORD count in the ring (ring
    submissions must be qword-aligned — pad with `MI_NOOP`), or missing store fence before the
    TAIL doorbell.
  - HEAD catches TAIL but the cookie never lands → the engine consumed the ring but the batch
    address decoded wrong (48-bit lo/hi split — the old "32-bit lo + 16-bit hi" reading is a
    documented prior mistake, corrected in GPU-Implementation-Notes §3) or PIPE_CONTROL's
    GGTT bit is missing so the seqno went to a PPGTT address.
  - Hang triage: implement/extend the i915-style hangcheck — dump RING_HEAD/TAIL/START/CTL,
    ACTHD, IPEIR, IPEHR, INSTDONE over serial (notes §6) and hand the dump to
    `duetos-debugging-playbook`.

## Phase 4 — 2D BLT → GDI accel (Roadmap slice 4, "the T4-03 win")

Roadmap text: *"2D BLT → GDI accel (the T4-03 win) — `XY_COLOR_BLT` (ROP `0xF0` fill) +
`XY_SRC_COPY_BLT` (ROP `0xCC` copy) on the BCS ring; wire GDI `FillRect`/`BitBlt` to it."*
Current code deviates knowingly: BLT commands are submitted on the **RCS** ring (the parser
routes by 2D client bits); moving to BCS (0x22000) to keep RCS free is open work.

- **Entry criteria:** Phase 3 batch-exec PASS on the target part. The fill wiring additionally
  gates on `IntelBltColorFillVerified()` — the sticky flag set only after the offscreen probe
  read back the exact requested pixel this boot.
- **Files/symbols:** `EncodeColorBlt` / `EncodeSrcCopyBlt` / `IsBltSurfaceGeometryValid` /
  `IsBltRectValid` / `kMiFlushDw` (`intel_gpu_cmds.h`); `IntelBltColorFillProbe` (offscreen
  rung 4); `IntelBltColorFill` + `BltSurfaceDescriptor` + the nonblocking
  `BltSubmissionLease` serialization (`intel_gpu.cpp` ~lines 59-100, 665-780). Consumer:
  compositor `FramebufferFillRect` with the CPU loop as unconditional fallback.
- **Expected:** host — `ctest -R intel_blt` green. Real HW —
  `[gpu/intel/cmds] blt-fill PASS (XY_COLOR_BLT offscreen, pixel=0xFF00FF00)`.
- **Failure branches:**
  - `blt-fill readback=... (2D BLT unverified on this part)` → probe honest-failed; the
    compositor stays on CPU, which is correct. Do not force the flag.
  - Pixel reads back stale/zero but HEAD caught TAIL → missing `MI_FLUSH_DW` (the blitter is
    cached) or CPU read ordering — check `DmaSyncForCpu`-equivalent on the destination.
  - Wrong colour → write-mask bits (`kBltWriteRgba`) — the header itself says "confirm the
    exact write-mask on first-HW bring-up" for src-copy.
  - `KLOG_WARN "RCS blt-fill: serialized compose submission timed out; CPU fallback armed"` →
    the lease/timeout path worked as designed; investigate the hang like Phase 3, don't
    widen the timeout as a "fix".
  - Residual work: `XY_SRC_COPY_BLT` has an encoder + host-testable geometry but **no live
    consumer** (`FramebufferBlit` stays CPU until GDI sources have physical-surface
    descriptors); BCS-ring submission; both are `open`.

## Phase 5 — display detect / modeset (Roadmap slice 5, independent of 1-4)

Roadmap text: *"Display detect/modeset (independent) — GMBUS EDID read + `SDEISR`/
`GEN11_DE_HPD_ISR` connector detect + primary-plane reprogram (keep firmware timings; defer
PLL math)."*

- **Entry criteria:** none from phases 1-4 (display block is not behind render forcewake), but
  the Hardware-Safety modeset row binds hard: EDID-advertised modes only, safe-mode fallback.
- **In tree:** GMBUS EDID read — `EncodeGmbus1Read` (constexpr), `GmbusReadEdid`,
  `IntelDisplayProbe` (walks common DDC pins, logs which returned a valid EDID header),
  called at the end of `intel::Probe`. Feeds the already-real, host-tested EDID/CEA-861/CVT
  parsers (`edid.cpp`, `cea861.cpp`, `cvt.cpp`).
- **Not in tree (open):** SDEISR / `GEN11_DE_HPD_ISR` hot-plug connector detect; primary-plane
  reprogram (`PLANE_SURF`/`PLANE_STRIDE`/`PLANE_CTL`, SKL+ base ~0x70180 per notes §5);
  DP-over-AUX EDID. PLL/transcoder math is deliberately deferred — keep firmware GOP timings.
- **Expected:** QEMU — `[gpu/intel/disp] selftest PASS`. Real HW — a probe log naming the DDC
  pin that produced a valid EDID.
- **Failure branches:**
  - All pins return 0 bytes → GMBUS NAK (`SATOER`) on every pin: wrong pin map for the part,
    or the panel is eDP (EDID lives on DP-AUX, a different transport — documented out of v0).
  - Plane-reprogram slice (when you write it): a wrong write here can blank a panel you can't
    see errors on — do it with serial capture attached, readback-verify each register (the
    Hardware-Safety modeset discipline), and keep the firmware framebuffer address noted so
    you can restore it.

## Untrustworthy signals — do not cite these as evidence

| Signal | Reality | Why it's a trap |
|--------|---------|-----------------|
| `gpu_telemetry` `temp_c` | Always 0, `temp_valid` always false (v0 GAP, all vendors) | A temperature that reads healthy because it is never measured. Never use it to argue a bring-up is thermally safe |
| `gpu_telemetry` `freq_mhz_est` | Best-effort Gen9+ CAGF decode of raw `GEN6_RPSTAT1`; exact only Gen9-11 | Trust `rpstat_raw`; treat the MHz as approximate until per-gen decode lands |
| GSC firmware "parse OK" | `intel_gsc_fw.cpp:266` GAP: per-partition CPD/SHA-256 hash chain NOT verified | Structural validity is not integrity. Diagnostic-only until an MEI slice adds verification |
| Absence of a FAIL line | Could mean the self-test never ran | Verify the `DUETOS_BOOT_SELFTEST` hooks exist (`boot_bringup.cpp:2246-2250`) before trusting a quiet boot |
| HEAD catching TAIL on NOOPs | A wedged engine can advance past `MI_NOOP`s without honouring real work | That's exactly why `IntelRcsStoreImmProbe` exists — demand the cookie read-back |
| "It compiles / QEMU boots clean" | Rungs 1-3 only | Encoders proven; MMIO execution is not. Say "unverified on silicon" |

## Known wrong paths (each bit someone already, or is a named class-of-bug)

- Writing any GT register (0x2000-0x2FFF and engine blocks) without holding RENDER **and** GT
  forcewake — silently no-ops on metal, works "fine" in your head.
- Scratch-filling or allocating low GGTT slots — kills the firmware framebuffer scanout.
- Skipping the Hardware-Safety precondition table because "it's just one register".
- Regressing virtio-gpu while touching `gpu.cpp`'s dispatch — the vendor `switch` in
  `RunVendorProbe` also owns the virtio framebuffer rebind path; every QEMU smoke must still
  show working scanout plus `[gpu/intel/rcs] no Intel device — skipped`.
- Trusting the handoff doc's stale claims (gated-stub Bringup, old branch) over the code.
- Reporting a QEMU-green boot as driver validation (see untrustworthy signals).
- Promoting probe PASS lines to `KLOG_INFO` — structural sentinels stay `arch::SerialWrite`,
  detail stays `KLOG_DEBUG_V` (per CLAUDE.md's diagnostic-logging contract).

## Definition of done for any slice in this campaign

1. Host tests + on-target self-tests extended in the same commit (constexpr/static_assert for
   new encoders; a `[gpu/intel/<unit>] selftest PASS` sentinel wired via `DUETOS_BOOT_SELFTEST`).
2. QEMU boot clean: `tools/test/boot-log-analyze.sh` exits 0; virtio-gpu scanout unregressed.
3. Failure legs fire `KBP_PROBE` (extend `ProbeId` if a new failure category) + one WARN.
4. `wiki/drivers/Graphics-Drivers.md` + `wiki/reference/GPU-Implementation-Notes.md` updated in
   the same commit; when silicon validation retires the Roadmap item, delete its section from
   `wiki/reference/Roadmap.md` in that commit (→ `duetos-change-control`, `duetos-docs-and-wiki`).
5. Honest labels: anything not proven on real hardware carries "unverified on silicon" in the
   code comment, the wiki, and the commit message.

## Provenance and maintenance

Authored 2026-08-13 against worktree HEAD 8a55872c (branch `claude/fable-driver-wave-20260801`).
All file/line references, register offsets, sentinels, and log strings were read from the tree at
that commit. Re-verify before trusting:

- Roadmap slice text still present: `grep -n "Intel iGPU command submission" wiki/reference/Roadmap.md`
- Current Intel unit inventory + sizes: `wc -l kernel/drivers/gpu/intel_*.{h,cpp}`
- Self-test hooks still wired: `grep -n "Intel.*SelfTest" kernel/core/boot_bringup.cpp`
- Bringup call chain unchanged: `grep -n "intel::Bringup\|ForcewakeGetForRing\|GgttInit" kernel/drivers/gpu/gpu.cpp kernel/drivers/gpu/intel_gpu.cpp`
- Serial sentinels unchanged: `grep -rn "gpu/intel" kernel/drivers/gpu/intel_gpu.cpp | grep SerialWrite`
- Host tests still pass: `ctest --test-dir build/host-tests --output-on-failure -R intel_blt`
- load_plan link caveat still applies (empty output = still missing, rebase first): `ls kernel/loader/ | grep load_plan`
- Telemetry GAPs still open: `grep -n "GAP" kernel/drivers/gpu/gpu_telemetry.h kernel/drivers/gpu/intel_gsc_fw.cpp`
- Safety posture rows: `grep -n "modeset\|volatile" wiki/security/Hardware-Safety.md`

Volatile facts most likely to rot: the "current state" table (any landed slice), the load_plan
caveat (fixed by any rebase), the Roadmap section itself (deleted when silicon validation lands),
and the BLT-on-RCS-not-BCS deviation.
