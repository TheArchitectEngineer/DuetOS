# `duetos_wifi80211` — cargo-fuzz harness

Fuzzes the memory-safe Rust 802.11 management-frame walker
(`kernel/net/wifi80211_rust/`, crate `duetos_wifi80211`) directly,
through its 5 public FFI entry points — the same functions
`kernel/net/wireless/beacon.cpp::BeaconParse` calls into. See
[`tests/fuzz/README.md`](../../../../tests/fuzz/README.md) for why
these are the DuetOS parsers worth fuzzing (on-air, attacker-controlled
bytes) and what this style of harness will/won't catch; that file also
indexes every other fuzzer in the tree. This crate is the one exception
to that directory's C++/libFuzzer harnesses: `beacon.cpp` has no raw-byte
parsing left in it to fuzz (it's a thin FFI caller), so this crate is
fuzzed with idiomatic `cargo fuzz` instead of a `tests/fuzz/fuzz_*.cpp`
driver.

## What's covered

| Target | FFI entry point | Parses |
|--------|------------------|--------|
| `fuzz_frame_header` | `duetos_wifi80211_parse_frame_header` | 24-byte MAC header (frame control, addresses, sequence control); rejects extension frames |
| `fuzz_beacon_body` | `duetos_wifi80211_parse_beacon_body` | Beacon / Probe Response fixed 12-byte prefix (timestamp, interval, capability info) |
| `fuzz_ie` | `duetos_wifi80211_parse_ie` | One Information-Element tag/length/payload step of the IE-list walker (first native-pointer-width input bytes select the start offset; see the target's source comment) |
| `fuzz_country_ie` | `duetos_wifi80211_parse_country_ie` | 802.11d Country IE payload: alpha2 + environment + up to 16 sub-band triplets |
| `fuzz_eapol_key` | `duetos_wifi80211_parse_eapol_key` | IEEE 802.1X-2010 EAPOL-Key descriptor (4-way handshake message) |

Each target links the real `duetos_wifi80211` crate, so a Rust panic
(index/overflow) surfaces as a libFuzzer crash, same as every other
Rust-backed harness in `tests/fuzz/`.

## Build and run

```bash
# One-time host tool install (not part of the repo or workspace):
cargo install cargo-fuzz --locked

cd kernel/net/wifi80211_rust/fuzz
python3 seeds/gen_seeds.py                 # regenerates checked-in synthetic corpus/<target>/ seeds

cargo fuzz run fuzz_frame_header -- -max_total_time=60
cargo fuzz run fuzz_beacon_body  -- -max_total_time=60
cargo fuzz run fuzz_ie           -- -max_total_time=60
cargo fuzz run fuzz_country_ie   -- -max_total_time=60
cargo fuzz run fuzz_eapol_key    -- -max_total_time=60

# Or build every target without running:
cargo fuzz build
```

The pinned `nightly-2026-01-15` toolchain (repo-root
`rust-toolchain.toml`) satisfies `cargo fuzz`'s nightly requirement
(ASan/libFuzzer instrumentation needs unstable `-Z sanitizer=address`)
without any extra `+nightly` selector. `.cargo/config.toml` in this
directory clears the `unstable.build-std` list the repo-root config
pins for the kernel's `x86_64-unknown-none` target — see the comment
in that file for why an inherited pin would break the `std`-linked
libFuzzer binaries here. `cargo fuzz` always passes its own explicit
`--target <host-triple>`, so the repo-root `build.target` pin is
harmless (a CLI flag beats a config-file value).

This has not been build- or run-verified in this change — the
authoring environment is RAM-constrained and explicitly disallows
`cargo build`/`cargo fuzz` for this task. `Cargo.toml` /
`.cargo/config.toml` were checked with `python3 -c "import tomllib"`;
the 5 `fuzz_targets/*.rs` files were checked with
`rustfmt --check --edition 2021` (syntax-tree parse, no dependency
resolution) — both passed clean. `seeds/gen_seeds.py` was run and its
output byte-shapes cross-checked against the parsers' documented
accept/reject contracts in Python (no Rust toolchain involved). The
first real `cargo fuzz build`/`run` on a suitable host is still
outstanding.

## Reproducing a crash

```bash
cargo fuzz run fuzz_eapol_key artifacts/fuzz_eapol_key/crash-<sha1>
```
