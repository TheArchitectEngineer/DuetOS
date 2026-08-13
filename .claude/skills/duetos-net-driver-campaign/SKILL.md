---
name: duetos-net-driver-campaign
description: >-
  Executable campaign runbook for DuetOS network drivers: safely extending NIC
  PCI-ID classification (nic_ids.h) and driving the wireless driver shells
  (iwlwifi first) toward real TX/RX. TRIGGER when the task says "bring up wifi",
  "iwlwifi", "add a NIC id" / "extend nic_ids", "wireless firmware", "NIC
  classification", "e1000 family", "real hardware wifi test", or the symptom is
  "wifi doesn't work" / "no wireless device detected". DO NOT TRIGGER for the
  general driver model (probe dispatch, watcher/generation pattern, driver
  domains as concepts) or wired-NIC interrupt debugging ("e1000 doesn't get
  interrupts") — that is duetos-driver-architecture; nor for generic
  QEMU/boot-smoke mechanics (duetos-boot-smoke-and-qemu) or commit/PR process
  (duetos-change-control).
---

# DuetOS net-driver campaign: NIC classification + wireless bring-up

This is the runbook for the net-driver lane's live problem: the wired NICs are
real, the wireless drivers are **shells** (probe + chip-ID read + firmware
request + watch task — no proven data path), and the classification header
`kernel/drivers/net/nic_ids.h` is the single most regression-prone file in the
lane. Follow the phases in order; every gate lists what you should see and
where to branch if you see something else.

**Vocabulary** (used throughout):

- **Shell** — a driver that binds, reads a chip-ID register over MMIO, requests
  firmware, and spawns a watch task, but moves no packets.
- **Family tag** — the string a probe assigns to a NIC (`"e1000e"`,
  `"iwlwifi-9000"`, `"mt7921"`); printed in the `[net-probe] ... family=` boot
  line and grepped by the boot-smoke test.
- **Watch task** — a per-NIC kernel poll loop (1 s tick) that re-reads the
  chip-ID register and exits when `NetModuleGeneration()` changes.
- **CSR / TFD / RBD** — iwlwifi terms: Control-Status Registers (BAR0 register
  file), Transmit Frame Descriptor (TX ring entry), Receive Buffer Descriptor.

## Current state (verified 2026-08-13 at HEAD 8a55872c)

| Driver | File(s) under `kernel/drivers/net/` | Status |
|---|---|---|
| e1000 / e1000e | `net.cpp` (bring-up inline, ~1324 lines) | REAL — RX/TX, MAC read, link, DHCP via upper stack |
| AMD PCnet | `pcnet.cpp` | REAL — polled RX/TX over I/O ports (VirtualBox default NIC) |
| virtio-net | classified in `net.cpp` (`VirtioNetTag`) | classified; see file for bring-up state |
| USB CDC-ECM / RNDIS | `kernel/drivers/usb/cdc_ecm.h`, `rndis.h` | REAL (USB lane, not this campaign) |
| iwlwifi | `iwlwifi{,_fw,_rings,_upload,_ucode_builder}.{h,cpp}` | SHELL+ — ring/upload state machine WRITTEN but **untested on silicon**; QEMU cannot emulate it |
| rtl88xx | `rtl88xx{,_fw,_upload}.{h,cpp}` + `rtl88xx_fw_rust/` | SHELL |
| bcm43xx | `bcm43xx{,_fw,_upload}.{h,cpp}` + `bcm43xx_fw_rust/` | SHELL |
| mt76 (MT7615..MT7925) | `mt76{,_fw}.{h,cpp}` | SHELL — per-family firmware basename + parse |
| ath9k_htc (USB, open firmware) | `ath9k_htc{,_fw,_upload}.{h,cpp}` (301 lines main TU) | SHELL — needs a physical AR9271/AR7010 dongle |

`iwlwifi_upload.h` says it plainly: *"this code ships untested on the dev host
(no QEMU emulation of iwlwifi)"*. Do not report any wireless TX/RX as working.
The wireless data path is **unproven**; only real hardware can prove it.

## When NOT to use this skill

- Understanding the probe/watcher/driver-domain architecture in general →
  `duetos-driver-architecture` (the patterns live there; this skill only states
  the campaign-relevant rules).
- Running or debugging the QEMU boot smoke itself → `duetos-boot-smoke-and-qemu`.
- Host-test / ctest / fuzz mechanics beyond what Phase 0 needs →
  `duetos-testing-and-validation`.
- Commit, claim, wiki, and PR obligations → `duetos-change-control`.
- Intel iGPU work → `duetos-intel-gpu-campaign` (same "QEMU can't emulate the
  silicon" shape, different lane).

## The verification ladder — what each tier can and cannot prove

| Tier | Command (Linux/WSL) | Proves | Cannot prove |
|---|---|---|---|
| 1. Host tests | `cmake -S tests/host -B build/host-tests && cmake --build build/host-tests && ctest --test-dir build/host-tests --output-on-failure -R nic_ids` | Pure classification logic (`nic_ids.h` is freestanding) | Anything touching MMIO or kernel state |
| 2. Rust fw-parser tests | `bash tools/dev/cargo-host-test.sh` | TLV/blob parsing (`iwlwifi_fw_rust`, `rtl88xx_fw_rust`, `bcm43xx_fw_rust` are in its crate list) | Upload sequencing, register writes |
| 3. Full kernel build | `cmake --preset x86_64-debug && cmake --build build/x86_64-debug` | Cross-TU integration compiles | Runtime behaviour |
| 4. QEMU boot smoke | `bash tools/test/ctest-boot-smoke.sh build/x86_64-debug` (or via ctest) | e1000e probe/classification/MAC/link on the emulated 82574L; watch-task lifecycle | **Any wireless silicon behaviour** — QEMU has no iwlwifi/rtl88xx/bcm43xx/mt76 model |
| 5. Real hardware | serial UART capture + `tools/test/boot-log-analyze.sh <log>` | Wireless upload/TX/RX — the ONLY proof tier for the data path | — |

Never present tier N as evidence for a tier-N+1 claim. `-fsyntax-only` on one
TU is below tier 3 — it does not catch cross-TU breakage; don't trust it for
changes that touch `net.h`/`nic_ids.h` consumers.

On this Windows dev box, run the bash commands inside WSL (prefer
`wsl.exe -e bash -lc '<cmd>'` — the `--` form fabricates exit codes). Tier 3-4
kernel builds must run in a **WSL-native scratch copy** of the tree, never on
the `/mnt/c` 9p mount — sync + scratch-tree mechanics in `duetos-build-and-env`.

## Phase 0 — Preflight

Run these from the repo root before touching the lane.

**0.1 Sync check.** Rebase first — as of 2026-08-13 this worktree is far
behind `origin/main` and the full kernel build (tier 3) fails on a missing
`kernel/loader/load_plan.h` until rebased (the inversion story and its
verification commands live in `duetos-change-control`).

```bash
git fetch origin main
git log --oneline HEAD..origin/main | wc -l          # >0 → behind
git rebase origin/main                                # per CLAUDE.md sync rules
```

If the rebase conflicts inside `kernel/drivers/net/`, someone else is active
in the lane — run `tools/parallel/status.sh` and follow
`duetos-change-control` before proceeding.

**0.2 Baseline host tests** (tier 1 + 2):

```bash
cmake -S tests/host -B build/host-tests
cmake --build build/host-tests --parallel
ctest --test-dir build/host-tests --output-on-failure -R nic_ids
bash tools/dev/cargo-host-test.sh
```

EXPECTED: `nic_ids` test passes (its `finish_main("nic_ids")` prints a pass
summary); the cargo script runs the three net fw-parser crates among its list.
If `nic_ids` fails at baseline → the lane is already broken; stop and fix
before your change (see `duetos-failure-archaeology` for the git-mining moves).

**0.3 Baseline boot smoke** (tier 4). Requires QEMU + GRUB + xorriso + OVMF —
install list in `wiki/tooling/Dev-Host-Setup.md`; the script exits 2 (SKIP,
not FAIL) when QEMU or `duetos.iso` is absent. Build and run these in the WSL
scratch tree (per `duetos-build-and-env`), not on `/mnt/c`.

```bash
cmake --preset x86_64-debug && cmake --build build/x86_64-debug --parallel
bash tools/test/ctest-boot-smoke.sh build/x86_64-debug
```

EXPECTED in the log (exact expected= entry in the script):

```
[net-probe] vid=0x0000000000008086 did=0x00000000000010d3 family=e1000e
```

If instead you see `family=` with a different tag → classification regressed;
go to Phase 1 gates. If the line is missing entirely → the PCI walk or probe
dispatch broke; that's `duetos-driver-architecture` + `duetos-debugging-playbook`
territory, not a classification fix.

## Phase 1 — Extending NIC classification (`nic_ids.h`)

`kernel/drivers/net/nic_ids.h` is the **single source of truth**: constexpr,
freestanding (`util/types.h` only), consumed by both `net.cpp`'s family tagging
and every wireless driver's `*Matches` predicate. It exists because the
previous design — parallel whitelists in `net.cpp` and each driver, plus coarse
Intel ID *ranges* — mis-dispatched ixgbe/igb/igc silicon as "e1000e" and ran
e1000 register writes against the wrong register file (fixed in 7d2b4271).

### The checklist for adding or changing any ID

Work through ALL of these; each one has bitten before.

1. **Evidence rule.** Every new ID must cite its silicon in a comment and come
   from a Linux `pci_device_id` table or a vendor datasheet — the header's own
   EVIDENCE block names the exact Linux paths per family. Never guess an ID
   from the marketing name. Cautionary tales already encoded in the tree:
   `0xB852` is the **8852BE** (rtw89), not what the name pattern suggests; and
   Broadcom firmware names use `BcmChipNameFormat`'s decimal-vs-hex rule
   (0xAA52 → `"43602"` → `brcmfmac43602-pcie.bin`), which cannot be derived by
   printing hex.
2. **Predicate ordering is load-bearing.** In `IntelWiredFamilyFromDeviceId`,
   igb/igc/ixgbe/i40e are tested BEFORE the classic-e1000 range and the e1000e
   table because their IDs interleave in the same 0x10xx/0x15xx space. Never
   reorder; never replace an explicit table with a range.
3. **The bring-up gate must stay narrow.** `IntelE1000BringUpEligible` returns
   true only for `E1000Classic` and `E1000e`. igb/igc/ixgbe/i40e stay
   **probe-only**: they use queue-based ring registers at different offsets,
   and bringing them up as e1000 means wrong-register writes on real silicon.
   The safe failure mode is "no driver", never "wrong-register writes".
4. **Wireless prefix coverage.** `NicFamilyLooksWireless` matches tag PREFIXES
   (`iwlwifi`, `rtl87`, `rtl88`, `rtl89`, `bcm43`, `mt76`, `mt7615`...`mt7925`).
   Any new tag emitted by `IntelWirelessTag` / `RealtekWirelessTag` /
   `BroadcomWirelessTag` / `MediatekNicTag` (the last lives in `net.cpp`) must
   start with a covered prefix — `rtl8723be-wifi` was once missed because no
   `rtl87` prefix existed. Add the prefix in the same edit as the tag.
5. **Update `tests/host/test_nic_ids.cpp` exhaustively.** The file already
   contains full `0x0000..0xFFFF` device-id sweeps (three of them) asserting
   family-dispatch and gate invariants across the whole space — that sweep is
   what pinned the original mis-dispatch. Extend the explicit expectations AND
   make sure your new IDs behave under the sweeps.
6. **Check the boot-smoke expectations BEFORE renaming any tag.** The
   `expected=(...)` array in `tools/test/ctest-boot-smoke.sh` (~line 166; the
   net line is ~line 219) greps for exact `family=` strings. Commit 541411a0
   exists because a tag rename broke boot smoke after the code was "done".
   `grep -n "family=" tools/test/ctest-boot-smoke.sh` is the 5-second check.
7. **MediaTek is split.** PCI IDs → `Mt76FamilyFromDeviceId` in `mt76.cpp`
   (vendor `0x14C3` defined in `mt76.h` as `kVendorMediaTek`), tag strings →
   `MediatekNicTag` in `net.cpp`. If you add an mt76 ID, update both, plus the
   firmware basename map (`Mt76FirmwareBasenameForFamily` in `mt76_fw`) — a
   known family with no basename is deliberately left `firmware_pending` rather
   than issuing a lying load request.

### Phase 1 gate

```bash
cmake --build build/host-tests --parallel && \
  ctest --test-dir build/host-tests --output-on-failure -R nic_ids
bash tools/test/ctest-boot-smoke.sh build/x86_64-debug
```

EXPECTED: nic_ids passes including sweeps; boot smoke still finds
`family=e1000e` for did 0x10D3. If the sweep fails on an ID you did not touch →
you changed dispatch ordering; re-check item 2. If boot smoke fails on the
`[net-probe]` line → re-check item 6 before touching kernel code.

### Fenced-off wrong paths (do not re-derive these)

- **Coarse device-id range gates** for Intel wired parts — the original sin.
  Explicit per-ID tables only.
- **A second ID table anywhere.** If you find yourself writing a device-id
  list in a driver TU, it belongs in `nic_ids.h` (or `mt76.cpp`'s existing
  family map for MediaTek).
- **Renaming a family tag without grepping the boot-smoke script** (541411a0).
- **Trusting `-fsyntax-only`** for a change consumers see through `net.h` —
  run the full build (tier 3).

## Phase 2 — Wireless shells → data path

The Roadmap (`wiki/reference/Roadmap.md`, Drivers section) defines the slice
menu. Pick ONE lane per session; each is independently landable.

### Menu (from the Roadmap's own ordering)

| Lane | Roadmap item | Hardware needed | Why pick it |
|---|---|---|---|
| A. iwlwifi TX/RX | "iwlwifi — live-silicon TX / RX" | Intel Wi-Fi laptop/NUC + serial UART | Most code already written; highest-value commodity chip |
| B. ath9k_htc HTC | "ath9k_htc — HTC service negotiation" | AR9271/AR7010 USB dongle | The ONLY open-firmware path (bundleable, `FirmwarePolicyCanBundle` true) |
| C. Firmware kit / installer | "Wireless — real-hardware verification" residuals | none | Unblocks A and B: staged offline firmware before the network picker opens |

### Lane A — iwlwifi (read this before writing a register)

What already exists (all in `kernel/drivers/net/`):

- `iwlwifi.cpp` — `IwlwifiBringUp`: HW_REV read, per-Type firmware basename
  choice, `FwLoad` → `IwlFirmwareParse` → `IwlUploadDrive`, fw-state tracking,
  `[iwlwifi] online ... status=fw-missing|fw-ready|upload-failed|...` serial line.
- `iwlwifi_upload.{h,cpp}` — CSR reset → power-up → section copy → ALIVE-wait
  state machine (`IwlUploadStage`), 2 s timeout, heavy `wifi-diag` logging.
  **Written, never run on silicon.**
- `iwlwifi_rings.{h,cpp}` (598 lines) — TFD build + doorbell already exist:
  `kHbusTargWrptr = 0x460`, doorbell encodes `(queue_id << 8) | wptr`;
  TX-completion reclaim via SCD read-pointer polling; singleton
  `IwlRingsActivate` (one NIC in v0) called after a successful upload;
  the watch task drains via `IwlRingsServicePending` every tick.
  `IwlRingsServiceRx` is inert so far (records `rx-service-empty`, returns 0).
  `// GAP:` at line ~142: legacy <7000-series RBD format unimplemented.

What the Roadmap says is residual (these are the slices, in order):

1. **MSI-X negotiation** — IVAR LUT writes at `CSR_MSIX_IVAR_AD_REG = 0x2890`;
   route every cause to vector 0 for single-vector start. ALIVE handler lives
   in the MSI-X "other" vector. Until this lands, the watch task's periodic
   poll is the only completion source (that fallback is already wired).
2. **Per-TFD build hardening** — legacy format: 20 TBs, `__le16 hi_n_len`
   packed, `HBUS_TARG_WRPTR = 0x460` doorbell (compare against what
   `TfdEntrySet`/`SubmitTx` already do before adding anything).
3. **RX queue init** — `FH_RSCSR_*` at `0xBC0/0xBC4/0xBC8`; the write pointer
   must be a multiple of 8. Then `iwl_rx_packet` cmd dispatch on
   `REPLY_RX_MPDU_CMD` → `wdev::OnDataRx`.

Reference: Linux `drivers/net/wireless/intel/iwlwifi/pcie/{tx,rx,trans}.c`.
Start with legacy gen1 (7000/8000/9000); gen2's BC table + dynamic scheduler is
explicitly a separate slice. Register offsets are Intel hardware ABI — copying
numeric constants is fine; copying Linux code is not.

Campaign-critical structural rules (full rationale in
`duetos-driver-architecture`):

- **Any new poller must snapshot `NetModuleGeneration()` at spawn and exit
  when it changes** — exactly like `IwlwifiWatchEntry` / `Mt76WatchEntry` do.
  `NetShutdown` bumps the generation BEFORE tearing down NIC records, so a
  compliant poller never observes a half-cleared table. (Known `// GAP:` at
  `net.cpp` ~line 67: a watcher that hasn't had its first run when shutdown
  fires can survive one extra module cycle — bounded, documented, don't
  "fix" it casually.)
- **Watchers are spawned from `NetInit` after the probe returns** (see the
  `IwlwifiStartWatch`/`Mt76StartWatch` dispatch at `net.cpp` ~line 1035) —
  never from inside a probe/BringUp function.
- **Dead-chip handling**: `0xFFFFFFFF` from any CSR read means BAR unmapped or
  chip asleep — mark offline, `IwlRingsDeactivate`, don't tear down MMIO.

### Firmware plumbing (all lanes)

- Policy matrix: `kernel/drivers/net/firmware_policy.cpp`. Dispositions:
  `preferred` (open, bundleable: ath9k-htc-open, b43-openfwwf) /
  `runtime-package` (redistributable blob, NEVER in-tree, needs license notice
  + exact hash: iwlwifi, rtl88xx, mt76) / `research-only` (nexmon — must never
  become loadable). `FirmwarePolicySelfTest` pins these; if your change makes
  a blob bundleable, the self-test failing is the system working.
- Request path: `duetos::core::FwLoadRequest{vendor, basename, min/max_bytes}`
  → `FwLoad` (`kernel/loader/firmware_loader.h`) → parse → `FwRelease`.
  Result lands in `NicInfo::wireless_fw_state`
  (Missing / Incompatible / LoadError / UploadFailed / Ready).
- Synthetic test firmware: `tools/firmware/mkiwlucode.py` builds a structurally
  valid iwlwifi `.ucode` TLV container (88-byte header + LE TLVs) from
  caller-supplied sections — use it to exercise parse/upload paths without
  Intel-signed retail firmware. `tools/firmware/mkduetfw.py` wraps blobs in the
  DUETFWPK envelope.
- Offline kit for real hardware: `tools/firmware/prepare-wifi-firmware.py
  --source /lib/firmware --out <kit>` (SHA-256 + provenance + licenses; the
  kernel unwraps at load time). See `wiki/drivers/WiFi-Onboarding.md`.
- Background: `wiki/drivers/Wireless-Firmware.md`,
  `wiki/drivers/Open-Firmware-Landscape-2026.md`,
  `wiki/drivers/Wireless-Regulatory.md`.

### Phase 2 gate (per slice)

```bash
bash tools/dev/cargo-host-test.sh                      # fw parsers still green
cmake --build build/x86_64-debug --parallel            # tier 3
bash tools/test/ctest-boot-smoke.sh build/x86_64-debug # tier 4 regression only
```

EXPECTED: boot smoke unchanged — on QEMU there is no wireless device, so your
slice must be **invisible** there (no new serial spam, no new expected lines).
If QEMU output changed, you leaked wireless-path work into the wired path.
On real Intel Wi-Fi hardware, EXPECTED serial:
`[iwlwifi] online pci=... did=... hw_rev=... silicon=... status=fw-ready` and,
for an upload slice, a `wifi-diag` timeline ending in ALIVE. `status=fw-missing`
→ stage the firmware kit (Lane C). `status=upload-failed` → the
`IwlUploadResult` fields (`failed_at`, `last_csr_int`, `last_gp_cntrl`) are in
the diag record precisely so a remote debug cycle is tractable — read them
before changing code.

## Phase 3 — Validation, safety, and promotion

**3.1 Hardware safety is a pre-landing precondition, not a review comment.**
`wiki/security/Hardware-Safety.md` carries the binding table ("Pre-landing
preconditions — bind these BEFORE the owning driver lands"). For this lane the
rows that matter:

- **DMA without IOMMU (any new bus-master driver) is DATA-LOSS** — and Lane A
  slices 2-3 program exactly that: TFD rings, `FH_RSCSR_*` registers, and
  doorbells are bus-master DMA descriptor machinery. Binding precondition:
  enable + enforce the IOMMU before bus-master DMA, map only driver-owned
  buffers, and validate descriptor targets (the NVMe/AHCI staging-buffer
  pattern). A bad descriptor scribbles firmware or other-OS memory.
- **Wi-Fi TX power is PHYS-DMG (+ legal)** — any slice that programs radio TX
  power must clamp to the lesser of the regulatory limit and the
  EEPROM-calibrated max, defaulting to the most-restrictive world domain
  until a country is set. Today the tree is read-only on TX power
  (`net/wireless/reg_telemetry.cpp` reports caps, never programs).

Ship the gate in the SAME slice that first writes the register, or don't
write it.

**3.2 Honest status reporting (no oversell).** After a wireless slice:

- QEMU-green means "did not regress the wired path". Say exactly that.
- The upload/ring machinery remains labeled **untested on silicon** until a
  real-hardware serial log shows the ALIVE/TX/RX evidence. Keep the
  `iwlwifi_upload.h` header comment truthful — update it when silicon proof
  exists, not before.
- Keep `// STUB:` / `// GAP:` markers accurate (`git grep -nE "// (STUB|GAP):"`
  is the live inventory; the Roadmap's "Source-tree GAP markers" section lists
  the net-lane ones).

**3.3 Promotion through change control** (details in `duetos-change-control`):

- Claim the lane first: `tools/parallel/claim.sh net-drivers "<files>" "<desc>"`.
- Definition-of-Done items that this lane trips most often: delete the landed
  Roadmap section in the same commit; update the owning wiki pages
  (`wiki/drivers/Networking-Drivers.md`, `Wireless-80211.md`,
  `Wireless-Firmware.md` as applicable); append to
  `wiki/reference/Design-Decisions.md` if you ruled out an alternative
  (e.g. chose gen1 rings over gen2).
- Full local gate before commit: `bash tools/dev/check-local.sh` (doctor +
  wiki nav + clang-format dry-run + configure by default; build/ctest/smoke
  are opt-in flags — read its usage header).
- Commit the measurement rig with the fix (serial-capture helpers, analyzers)
  under `tools/test/` or `tools/qemu/` per the reusable-tooling rule.

**3.4 Real-hardware session shape** (when a machine is available):

1. Build the ISO, stage the firmware kit from `prepare-wifi-firmware.py`.
2. Boot with serial UART captured to a file.
3. `bash tools/test/boot-log-analyze.sh <log>` — launcher-agnostic triage;
   non-zero exit on any non-deliberate failure.
4. Compare the `wifi-diag` upload timeline against `IwlUploadStage` order
   (PrepareCard → SwReset → NicInit → SectionLoad → AliveWait → Complete).
   The first stage that deviates is the slice boundary for the fix.

## Provenance and maintenance

Authored 2026-08-13 against worktree branch `claude/fable-driver-wave-20260801`
at HEAD 8a55872c (2026-08-01). Every path, register constant, log line, and
status claim above was read from the tree at that commit. Volatile facts:
the 207-commit lag and the missing `kernel/loader/load_plan.h` describe THIS
worktree on 2026-08-13 and dissolve after a rebase; driver statuses change as
slices land.

Re-verify before relying:

- Driver inventory: `ls kernel/drivers/net/`
- Classification truth: `sed -n '1,60p' kernel/drivers/net/nic_ids.h` (header
  comment carries the evidence rule + history)
- Bring-up gate still narrow: `grep -n "IntelE1000BringUpEligible" -A4 kernel/drivers/net/nic_ids.h`
- Boot-smoke family expectations: `grep -n "family=" tools/test/ctest-boot-smoke.sh`
- Wireless residuals: `sed -n '538,668p' wiki/reference/Roadmap.md`
- Untested-on-silicon caveat still true: `grep -n "untested" kernel/drivers/net/iwlwifi_upload.h`
- Generation pattern intact: `grep -n "spawn_gen\|g_module_generation" kernel/drivers/net/*.cpp`
- Firmware policy matrix: `grep -n "FirmwareDisposition::" kernel/drivers/net/firmware_policy.cpp`
- Host-test wiring: `grep -n "add_host_test(nic_ids)" tests/host/CMakeLists.txt`
- Rebase debt: `git fetch origin main && git log --oneline HEAD..origin/main | wc -l`
