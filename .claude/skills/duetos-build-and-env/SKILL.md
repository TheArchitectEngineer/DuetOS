---
name: duetos-build-and-env
description: >-
  Recreate the DuetOS build environment and build every artifact class: kernel ELF/ISO (clang cross-compile
  inside WSL), native MSVC host tests, Rust no_std crates, and libFuzzer harnesses. TRIGGER when asked to
  "build the kernel", "cmake preset won't configure", "set up the dev environment", "WSL build", "host tests
  won't configure", "clang toolchain missing", "cargo build fails", or "build the fuzzers". DO NOT TRIGGER for
  running/booting the built image or smoke-test triage (use duetos-boot-smoke-and-qemu), for the test
  inventory and what to run when (use duetos-testing-and-validation), or for CI pipeline questions (use
  duetos-ci-and-release).
---

# DuetOS: build and environment runbook

DuetOS is a freestanding x86_64 kernel. On this Windows 11 dev host the build splits into
four artifact classes, each with its own toolchain and its own place to run:

| Artifact class | Builds where | Toolchain | Entry point |
|---|---|---|---|
| Kernel ELF + bootable ISO | **Inside WSL**, in a WSL-native scratch copy (never `/mnt/c`) | clang 18 + lld, target `x86_64-unknown-none-elf` | `cmake --preset x86_64-debug` |
| Host unit tests (`tests/host/`) | **Natively on Windows** (MSVC) or Linux/CI (clang) | MSVC 14.38 + Ninja, or clang++-18 | `cmake -S tests/host -B build/host-tests` |
| Rust kernel crates (27-member workspace) | Inside WSL (rustup lives there) | pinned `nightly-2026-01-15`, `-Zbuild-std` | `cargo build` from repo root |
| Fuzz harnesses (`tests/fuzz/`, 37 targets) | Inside WSL | clang++ only (libFuzzer + ASan + UBSan) | `cd tests/fuzz && make` |

Definitions used throughout:

- **"Windows side"** = Git Bash or PowerShell on the Windows host, cwd = the repo checkout
  (this file's repo root; example paths below use `C:\Users\natew\DuetOS-fable-drivers-20260801`
  = `/mnt/c/Users/natew/DuetOS-fable-drivers-20260801` — substitute your checkout).
- **"Inside WSL"** = a shell inside the WSL Ubuntu distro (default user is root; home is `/root`).
- **9p mount** = WSL's `/mnt/c/...` view of the Windows filesystem. It **cannot host a CMake
  kernel build** (EINVAL from Ninja + CRLF corruption — documented in
  `tools/build/sync-to-wsl-scratch.sh`). Build in a WSL-native directory, always.

## Trap zero: WSL exit codes are fabricated with `wsl.exe --`

`wsl.exe -- bash -lc 'cmd; echo RC=$?'` prints the **outer shell's** status — always `RC=0`
— because the outer argument path re-expands `$?` even inside single quotes. Every exit code
read this way is wrong, and wrong in the reassuring direction. Verified 2026-07-29 in this
project: a boot smoke correctly exiting 1 was reported as rc=0 for half a session.

Rules for every `wsl.exe` call whose result you will act on:

- Prefer `wsl.exe -e bash -lc '...'` — the `-e` form execs the binary directly and
  propagates status correctly.
- Or branch inside the command: `wsl.exe -- bash -lc 'cmd && echo BUILD_OK || echo BUILD_FAIL'`
  and grep for the sentinel, never the code.
- `$?`, `$$`, `$!`, and any `$VAR` set on the Windows side are all re-expanded the same way
  in the `--` form. `-e` avoids all of it.

## Kernel build: presets are the only entry point

The root `CMakeLists.txt` hard-FATALs if `CMAKE_TOOLCHAIN_FILE` is unset — a plain
`cmake -B build` is rejected by design. Every configure goes through `CMakePresets.json`
(schema v6, generator **Ninja**, binaryDir `build/<preset>`, toolchain
`cmake/toolchains/x86_64-kernel.cmake`).

Toolchain facts (all from `cmake/toolchains/x86_64-kernel.cmake`):

- Compilers hard-pinned to `clang` / `clang++`; link via `lld` (`-fuse-ld=lld -nostdlib -static`).
- Target triple `x86_64-unknown-none-elf`, `-ffreestanding`, `-mno-sse -mgeneral-regs-only
  -mcmodel=kernel -mno-red-zone`, retpolines on, `-fstack-protector-strong`.
- `CMAKE_C_COMPILER_WORKS`/`CMAKE_CXX_COMPILER_WORKS` are forced to 1 (the hosted link probe
  cannot work freestanding) — so a *broken* clang install fails at first real TU, not at configure.
- `-fcf-protection=none` deliberately: KVM's instruction emulator on the CI host cannot decode
  `endbr64` (documented GAP in the toolchain file). Do not "fix" this back to `branch` without
  reading that comment block.
- `-Werror` with a large extended warning floor. Zero-warning policy is real; any `warning:` is a
  fix target.

### Configure presets (16, all in `CMakePresets.json` — incl. the hidden `x86_64-base`; `x86_64-kasan` and `x86_64-debug-kasan` are two separate entries)

| Preset | What it adds |
|---|---|
| `x86_64-base` | hidden parent (Ninja, toolchain file, `DUETOS_ARCH=x86_64`) |
| `x86_64-debug` | Debug + `DUETOS_GDB_SERVER` + UBSAN + KASAN + `DUETOS_CAP_AUDIT=Full` + `DUETOS_SHELL_SELFTEST` |
| `x86_64-release` | Release, no instrumentation |
| `x86_64-debug-ubsan` / `-asan` / `-san` | narrower/broader sanitizer mixes on top of debug |
| `x86_64-debug-conv` | `-Wconversion`/`-Wsign-conversion` audit (non-fatal) |
| `x86_64-debug-ubsan-trap` | UBSAN trap mode (first UB = #UD, no log runtime) |
| `x86_64-debug-redteam` | AttackSim suite — not a normal-boot build |
| `x86_64-release-asserts` | Release + KASSERT + UBSAN runtime (paranoid release) |
| `x86_64-release-audit` | Release + full cap-gate + lock-order audit (forensic) |
| `x86_64-debug-fast` | cheaper instrumentation, faster boot, still debuggable |
| `x86_64-kasan` (alias `x86_64-debug-kasan`) | debug + KASAN + UBSAN + lock-order + full cap audit |
| `x86_64-release-lto` | ThinLTO |
| `x86_64-release-karl` | link-order randomization; seed from env `DUETOS_KARL_SEED`, emits `kernel.symbols` sidecar |

Build outputs (verified in `kernel/CMakeLists.txt`):

- Kernel ELF: `build/<preset>/kernel/duetos-kernel.elf` (CMake target `duetos-kernel`).
- Bootable ISO: `build/<preset>/duetos.iso` (CMake target `duetos-iso`, part of ALL — but only
  when `grub-mkrescue` **and** `xorriso` were found at configure time; otherwise configure
  prints "duetos-iso target disabled" and a plain build silently produces no ISO. Re-run
  configure after installing them.)

## From zero to built kernel (Windows host, numbered runbook)

Step 0 — provision WSL once (see "Provisioning a fresh WSL host" below), then:

1. **[Windows side]** Sync this checkout into a WSL-native scratch tree. The script self-locates
   (SRC = its own repo root, overridable via env `DUETOS_SRC`), excludes `build/` and `.git`,
   and is idempotent (`rsync -a --delete`):

   ```bash
   wsl.exe -e bash -lc '/mnt/c/Users/natew/DuetOS-fable-drivers-20260801/tools/build/sync-to-wsl-scratch.sh /root/scratch/duetos-fable'
   ```

   The dest argument is optional (default `/root/scratch/duetos-tactility` — shared, so pick
   your own dest when other sessions may be active). Re-run after every Windows-side edit;
   the scratch copy is a build mirror, not a second checkout (no `.git` inside).

2. **[Windows side]** Configure + build in one guarded call (`-e` form, sentinel output):

   ```bash
   wsl.exe -e bash -lc 'export PATH=/usr/lib/llvm-18/bin:$PATH; cd /root/scratch/duetos-fable && cmake --preset x86_64-debug && cmake --build build/x86_64-debug -j8 && echo BUILD_OK || echo BUILD_FAIL'
   ```

   The `PATH` export matches `tools/build/wsl-kernel-build.sh`: it puts `llvm-objcopy` on PATH
   for `tools/build/build-linux-vdso.sh` (which prefers it, falling back to GNU `objcopy`).

3. **[Windows side]** Verify the artifacts exist — never claim "built" from a green-looking log:

   ```bash
   wsl.exe -e bash -lc 'ls -la /root/scratch/duetos-fable/build/x86_64-debug/kernel/duetos-kernel.elf /root/scratch/duetos-fable/build/x86_64-debug/duetos.iso'
   ```

4. Booting/smoking the ISO is the **duetos-boot-smoke-and-qemu** skill's job — hand off there.

### Kernel-build traps (each one has bitten a session)

- **The tracked `wsl-*` helper scripts hardcode `cd ~/source/DuetOS` — a DIFFERENT checkout.**
  `tools/build/wsl-kernel-build.sh [target]`, `wsl-build-isos.sh`, and
  `wsl-clang-format-check.sh` all `cd ~/source/DuetOS` (= `/root/source/DuetOS` inside WSL),
  which as of 2026-08-13 is a separate git clone last at commit `099fed71` (2026-08-05) — not
  this worktree, and days behind it. Running them "builds successfully" against the wrong tree.
  Use them only after making `/root/source/DuetOS` current; otherwise run `cmake` directly in
  your scratch copy as in the runbook above. (`sync-to-wsl-scratch.sh` and `wsl-tree-paths.sh`
  are the fixed-generation scripts: they self-locate / fail closed instead of hardcoding.)
- **A `/mnt/c`-configured build cache is poison.** This checkout ships a stale
  `build/x86_64-debug/` whose `CMakeCache.txt` says `CMAKE_HOME_DIRECTORY=/mnt/c/...` and which
  contains **no ELF and no ISO**. Any cache configured on the 9p mount is unsupported (EINVAL +
  CRLF) and unproven — do not `cmake --build` it and report the result as a kernel build.
  Detect with:
  `grep CMAKE_HOME_DIRECTORY build/x86_64-debug/CMakeCache.txt` (Windows side, Git Bash) —
  a `/mnt/c/` value means: ignore this tree, build fresh in scratch.
- **ISO target silently absent.** If configure ran before `grub-mkrescue`/`xorriso` were
  installed, the `duetos-iso` target does not exist and ALL builds only the ELF. The configure
  log line is `duetos-iso target disabled (grub-mkrescue or xorriso not found).` — reconfigure
  after installing.
- **`wsl-tree-paths.sh` is SOURCED, never executed.** It resolves `WIN_TREE` (from env
  `DUETOS_WIN_TREE`, else its own repo root if under `/mnt/`) and `WSL_TREE` (from env
  `DUETOS_WSL_TREE`, else `/root/scratch/duetos-main` if present) and exports both, failing
  closed on ambiguity. Helpers that shuttle files between the trees source it.
- **Scratch copies have no `.git`.** `git` commands in `/root/scratch/duetos-*` fail or, worse,
  find an enclosing repo. All git operations happen on the Windows checkout
  (see **duetos-change-control** for branch/claim rules).
- **Full link fails on a missing `loader/load_plan.h`?** That is a stale branch, not a code
  bug — rebase on `origin/main` first (the inversion story lives in **duetos-change-control**).

## Fast host-test loop (no WSL, seconds not minutes)

`tests/host/CMakeLists.txt` is a **standalone CMake project** — it is NOT included from the
root and does NOT use the kernel toolchain, so the presets-only rule does not apply to it.
It builds hosted unit tests for arch-agnostic kernel TUs plus Python-driven contract tests.
C++23, `/W4 /WX` on MSVC, `-Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion
-Wsign-conversion` elsewhere. ASan+UBSan default ON (`DUETOS_HOST_TESTS_SANITIZERS`) but
**automatically skipped under MSVC** — the Windows loop runs unsanitized; sanitized coverage
comes from the Linux/CI leg.

**[Windows side, Git Bash, from the repo root]** — MSVC's `cl.exe` is not on PATH, and this
environment exports `CC=claude.exe` which CMake will wrongly probe as the C compiler. Always
go through vcvars with the compilers pinned:

```bash
cmd //c "call \"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat\" && set CC=cl && set CXX=cl && cmake -S tests/host -B build/host-tests -G Ninja && cmake --build build/host-tests && ctest --test-dir build/host-tests --output-on-failure"
```

(From PowerShell use `cmd /c "..."` — single slash.) The existing
`build/host-tests/CMakeCache.txt` in this checkout proves this exact shape works: MSVC
14.38 `cl.exe`, generator Ninja. CTest needs Python 3 findable (`find_package(Python3
REQUIRED)`); the contract tests invoke scripts under `tools/test/` and `tools/build/`.

**[Inside WSL / Linux]** — pin clang explicitly. CMake's default probe resolves `cc`/`c++`
to GCC on ubuntu-24.04, and GCC ignores unused `static inline` functions that clang's
`-Wunused-function` + `-Werror` makes fatal — so a GCC-built run green-lights code the real
toolchain rejects (documented in `.github/workflows/build.yml`, which pins the same way):

```bash
cmake -S tests/host -B build/host-tests -G Ninja -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

What the suite covers vs. the QEMU matrix is **duetos-testing-and-validation** territory.

## Rust crates

- Workspace: 27 members, all under `kernel/` (see root `Cargo.toml`). Toolchain pinned by
  `rust-toolchain.toml`: `nightly-2026-01-15`, components `rust-src`, target
  `x86_64-unknown-none`, profile minimal. Bumping the pin is its own PR, never bundled.
- `.cargo/config.toml` pins `[build] target = "x86_64-unknown-none"` +
  `build-std = ["core", "alloc"]` for **every** cargo invocation under the tree — you cannot
  override it with env vars or `--config` from inside the tree.
- **[Inside WSL, repo/scratch root]** `cargo build` cross-builds all crates for the kernel
  target (first run downloads the pinned nightly via rustup — rustup lives in WSL at
  `/root/.cargo/bin`). The kernel CMake build links the resulting staticlibs.
- **Hosted Rust tests bypass cargo entirely** (because of the config pin):

  ```bash
  bash tools/dev/cargo-host-test.sh              # all covered crates
  bash tools/dev/cargo-host-test.sh ntfs_rust    # one crate, by basename or path
  ```

  It compiles each crate's `src/lib.rs` with `rustc --edition 2021 --test` and runs the
  binary. Coverage is 22 of the 27 workspace members; **not covered** (no hosted-test entry):
  `kernel/rust`, `kernel/fs/duetfs`, `kernel/acpi/aml_rust`, `kernel/drivers/usb/class_rust`,
  `kernel/drivers/usb/hid_rust`.
- Formatting: `rustfmt.toml` (edition 2021, max_width 120, idiomatic K&R — deliberately NOT
  the C++ Allman style). Lint gate: `cargo clippy --workspace -D warnings` (mirrors CI; run
  via `tools/dev/analyze.sh` clippy phase).

## Fuzz harnesses

**[Inside WSL]** `tests/fuzz/` holds 37 libFuzzer C++ harnesses driven by a Makefile
(host clang only — `CXX := clang++`, `-fsanitize=fuzzer,address,undefined`, kernel headers
shimmed by `tests/fuzz/host_shim/`):

```bash
cd tests/fuzz
make            # build every harness into tests/fuzz/build/
make run-eapol  # fuzz one target for 60 s (corpus/<name>/ persists)
```

Known trap: without the `libclang-rt-18-dev` package **all 37 targets fail at link** with
`cannot find .../libclang_rt.asan-x86_64.a` — a missing-package error that reads like a code
break. `rustc` is additionally needed for `fuzz_pe` (delegates to the `duetos_exec_meta`
crate). The 802.11 beacon walker is fuzzed via cargo-fuzz in `kernel/net/wifi80211_rust/`,
not here.

## Provisioning a fresh WSL host

Preflight first — `tools/dev/doctor.sh` is read-only (reports + apt hints, never installs):

```bash
# [Inside WSL, repo/scratch root]
tools/dev/doctor.sh --build   # required build/format tools only (default)
tools/dev/doctor.sh --live    # build tools + QEMU/ISO live-boot tools
tools/dev/doctor.sh --ci      # same as --live; for local CI-smoke reproduction
```

The full install list lives in `wiki/tooling/Dev-Host-Setup.md` — install groups, verbatim
rationale, and the "legitimately requires" test for when the live toolbox is warranted
(runtime behaviour changed, or a correctness claim a compile can't prove; NOT for pure
refactors or docs-only changes). The load-bearing groups:

| apt packages | Without them |
|---|---|
| `clang-18 lld-18 llvm-18 cmake ninja-build` (CI's list) | no kernel build at all |
| `qemu-system-x86 grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools ovmf` | no ISO target, no live boot |
| `gcc-mingw-w64-x86-64 gcc-mingw-w64-i686` | smoke PE blobs emit as `_len=0` stubs; ring3 PE tests can't load |
| `libclang-rt-18-dev` | all 37 fuzz targets fail at link |
| `gdb strace lsof tcpdump binutils` and friends | GDB stub unusable, debug friction |

CI symlinks `clang-18` to `/usr/local/bin/clang` (`.github/workflows/build.yml`); the
current WSL distro already has that symlink plus cmake/ninja/grub-mkrescue/xorriso/QEMU/rustup
installed (verified 2026-08-13). A bare `clang` NOT resolving is fixed either by that symlink
or by `export PATH=/usr/lib/llvm-18/bin:$PATH`.

## Formatting and local preflight

- `.clang-format`: Microsoft base, IndentWidth 4, 120 col. CI pins **clang-format-18** —
  match the major version locally or you will get churn diffs.
- **Never pass a `.S` file to `clang-format`** — it parses assembly as C++ and mangles it.
  The tracked format sweeps only glob `*.h *.hpp *.c *.cpp` under `kernel/ userland/`;
  keep that discipline in ad-hoc commands too.
- `tools/dev/check-local.sh` — one-command local CI preflight. Defaults: doctor + wiki
  nav/quality + clang-format dry-run + CMake configure for `x86_64-debug`. Opt-in flags:
  `--preset <name>`, `--build`, `--ctest` (implies `--build`), `--smoke` (implies `--build`).
- `tools/dev/analyze.sh` — static/dynamic analysis phases: `own` (invariant checks, gating),
  `cppcheck` (gating, error severity only), `tidy` (advisory), `clippy` (gating,
  `-D warnings`), `dynamic` (opt-in `--dynamic`: ubsan + kasan QEMU boots). Missing optional
  tools downgrade to a skip with an apt hint — they never fake a pass.

Both scripts assume a Linux host — run them inside WSL against the scratch tree.

## When NOT to use this skill

- Running the built ISO, boot-log triage, smoke profiles → **duetos-boot-smoke-and-qemu**.
- What tests exist and which to run for a given change → **duetos-testing-and-validation**.
- Branching, claims, PARALLEL_WORK.md, commit/PR discipline → **duetos-change-control**.
- CI workflow internals, release gating → **duetos-ci-and-release**.
- Coding style / kernel idioms (this file only covers the compiler flags that enforce them)
  → **duetos-kernel-conventions**.

## Provenance and maintenance

Authored 2026-08-13 against worktree branch `claude/fable-driver-wave-20260801` (HEAD
`8a55872c`, 2026-08-01; origin/main was ~207 commits ahead — re-verify volatile facts after
rebasing). WSL-distro state (installed tools, `/root/source/DuetOS` at `099fed71`, scratch
trees) is a snapshot of that date. Everything else was read from tracked files.

One-line re-verification (Windows Git Bash at repo root unless noted):

- Preset list: `grep '"name"' CMakePresets.json`
- Toolchain pins/flags: `sed -n '1,60p' cmake/toolchains/x86_64-kernel.cmake`
- Presets-only FATAL: `sed -n '1,12p' CMakeLists.txt`
- ELF/ISO targets + gating: `grep -n 'duetos-iso\|OUTPUT_NAME\|GRUB_MKRESCUE' kernel/CMakeLists.txt`
- 9p-mount rationale + script args: `sed -n '1,30p' tools/build/sync-to-wsl-scratch.sh`
- Hardcoded-checkout trap: `grep -n 'cd ~/source' tools/build/wsl-*.sh`
- Stale cache check: `grep CMAKE_HOME_DIRECTORY build/x86_64-debug/CMakeCache.txt`
- Host-test invocation + MSVC/sanitizer split: `sed -n '19,52p' tests/host/CMakeLists.txt`
- CI clang-18 pin rationale: `grep -n -B8 'CMAKE_CXX_COMPILER=clang' .github/workflows/build.yml`
- Rust pins: `cat rust-toolchain.toml .cargo/config.toml`; members: `sed -n '9,37p' Cargo.toml`
- Hosted-Rust coverage: `grep -c 'kernel/' tools/dev/cargo-host-test.sh` vs `grep -c '"kernel/' Cargo.toml`
- Fuzz target count: `ls tests/fuzz/*.cpp | wc -l` (37 on 2026-08-13)
- WSL tool state: `wsl.exe -e bash -lc 'command -v clang cmake ninja grub-mkrescue xorriso qemu-system-x86_64'`
- Exit-code trap proof: `wsl.exe -- bash -lc 'bash -c "exit 1"; echo RC=$?'` prints RC=0;
  `wsl.exe -e bash -lc 'bash -c "exit 1"; echo RC=$?'` prints RC=1.
